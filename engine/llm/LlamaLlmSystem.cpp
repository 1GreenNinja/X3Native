// LlamaLlmSystem — the real ILlmSystem backend over llama.cpp (CPU inference).
//
// QUARANTINE: every llama.cpp/ggml type lives in THIS translation unit only
// (linked PRIVATE into x3engine); engine/llm/ILlmSystem.h stays llama-free.
//
// VENDORING: llama.cpp is pulled by CMake FetchContent pinned to release tag
// b9590 (see engine/CMakeLists.txt), built static, CPU backend only
// (GGML_VULKAN OFF — the inference stack never touches the engine's Vulkan).
//
// THREADING: ALL llama calls except model load/free run on ONE dedicated
// inference thread (per-chat contexts are created lazily on that thread, so
// no llama_context is ever touched from two threads). submit() enqueues and
// returns; tokens land in a mutex-guarded per-chat buffer drained by poll().
// cancel() flips a flag checked between tokens. unload()/dtor JOIN the worker
// BEFORE freeing contexts/model (the fix/stability job-bridge lesson).
//
// PROMPTING: Qwen2.5-Instruct speaks ChatML; the template is hand-rolled here
// (LLAMA_BUILD_COMMON is OFF — core libllama only):
//   <|im_start|>system\n{persona}<|im_end|>\n
//   <|im_start|>user\n{text}<|im_end|>\n<|im_start|>assistant\n ... generate
// Multi-turn history persists in the chat's KV cache (positions are tracked
// by llama_decode); each reply is closed with <|im_end|>\n so the next turn
// appends cleanly.
#include "engine/llm/ILlmSystem.h"
#include "engine/core/x3_log.h"

#include <llama.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace x3::llm {

namespace {

// Route llama.cpp's internal logging into the engine log, warnings and errors
// only (the per-tensor load chatter is far too noisy for the boot log).
void llamaLogTrampoline(ggml_log_level level, const char* text, void* /*user*/) {
    if (!text || !*text) return;
    std::string msg(text);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    if (msg.empty()) return;
    if (level == GGML_LOG_LEVEL_ERROR)      x3::logError("[llama] " + msg);
    else if (level == GGML_LOG_LEVEL_WARN)  x3::logWarn ("[llama] " + msg);
    // info/debug: dropped
}

// Backend init/free is process-global in llama.cpp; refcount across systems.
std::mutex g_backendMx;
int        g_backendRefs = 0;
void backendAcquire() {
    std::lock_guard<std::mutex> lk(g_backendMx);
    if (g_backendRefs++ == 0) {
        llama_log_set(&llamaLogTrampoline, nullptr);
        llama_backend_init();
    }
}
void backendRelease() {
    std::lock_guard<std::mutex> lk(g_backendMx);
    if (--g_backendRefs == 0) llama_backend_free();
}

class LlamaLlmSystem final : public ILlmSystem {
public:
    LlamaLlmSystem() = default;
    ~LlamaLlmSystem() override { unload(); }

    bool loadModel(const std::string& ggufPath, const ModelOpts& opts) override {
        unload();   // idempotent re-load
        backendAcquire();

        llama_model_params mp = llama_model_default_params();
        mp.n_gpu_layers = 0;                 // v1: CPU only (build has no GPU backend anyway)
        llama_model* model = llama_model_load_from_file(ggufPath.c_str(), mp);
        if (!model) {
            x3::logWarn("[llm] model load FAILED: " + ggufPath);
            backendRelease();
            return false;
        }

        std::lock_guard<std::mutex> lk(m_mx);
        m_model     = model;
        m_modelSize = (std::size_t)llama_model_size(model);
        m_opts      = opts;
        if (m_opts.contextTokens   < 256) m_opts.contextTokens   = 256;
        if (m_opts.maxOutputTokens < 8)   m_opts.maxOutputTokens = 8;
        m_quit = false;
        m_worker = std::thread([this] { workerMain(); });
        m_loaded = true;
        x3::logInfo("[llm] loaded " + ggufPath + " (" +
                    std::to_string(m_modelSize / (1024 * 1024)) + " MB weights, ctx " +
                    std::to_string(m_opts.contextTokens) + ", backend llama.cpp/CPU)");
        return true;
    }

    bool modelLoaded() const override {
        std::lock_guard<std::mutex> lk(m_mx);
        return m_loaded;
    }

    ChatId startChat(const std::string& personaSystemPrompt) override {
        std::lock_guard<std::mutex> lk(m_mx);
        if (!m_loaded) return kInvalidChat;
        const ChatId id = m_nextId++;
        Chat& c   = m_chats[id];
        c.persona = personaSystemPrompt;
        return id;   // the llama_context is created lazily ON THE WORKER (first submit)
    }

    bool submit(ChatId chat, const std::string& userText) override {
        std::lock_guard<std::mutex> lk(m_mx);
        auto it = m_chats.find(chat);
        if (it == m_chats.end() || it->second.busy || !m_loaded) return false;
        it->second.busy = true;
        it->second.done = false;
        it->second.cancelReq = false;
        m_jobs.push_back(Job{ chat, userText });
        m_cv.notify_one();
        return true;
    }

    PollResult poll(ChatId chat) override {
        PollResult r;
        std::lock_guard<std::mutex> lk(m_mx);
        auto it = m_chats.find(chat);
        if (it == m_chats.end()) { r.done = true; r.failed = true; return r; }
        r.newTokens.swap(it->second.pending);
        r.newTokenCount = it->second.pendingCount;
        it->second.pendingCount = 0;
        r.done   = it->second.done;
        r.failed = it->second.failed;
        return r;
    }

    void cancel(ChatId chat) override {
        std::lock_guard<std::mutex> lk(m_mx);
        auto it = m_chats.find(chat);
        if (it != m_chats.end()) it->second.cancelReq = true;
    }

    void endChat(ChatId chat) override {
        // The context belongs to the worker; queue its destruction there so we
        // never free a context mid-decode. Mark the chat dead immediately.
        std::lock_guard<std::mutex> lk(m_mx);
        auto it = m_chats.find(chat);
        if (it == m_chats.end()) return;
        it->second.cancelReq = true;
        for (auto& j : m_jobs) if (j.chat == chat) j.chat = kInvalidChat;
        if (it->second.ctx) m_retiredCtxs.push_back(it->second.ctx);
        if (it->second.smpl) m_retiredSmpls.push_back(it->second.smpl);
        m_chats.erase(it);
        m_cv.notify_one();   // wake the worker to sweep retired contexts
    }

    void unload() override {
        {
            std::lock_guard<std::mutex> lk(m_mx);
            if (!m_loaded) return;
            m_quit = true;
            m_cv.notify_one();
        }
        if (m_worker.joinable()) m_worker.join();   // JOIN BEFORE FREE — always
        std::lock_guard<std::mutex> lk(m_mx);
        for (auto& kv : m_chats) {
            if (kv.second.smpl) llama_sampler_free(kv.second.smpl);
            if (kv.second.ctx)  llama_free(kv.second.ctx);
        }
        m_chats.clear();
        sweepRetiredLocked();
        m_jobs.clear();
        if (m_model) { llama_model_free(m_model); m_model = nullptr; }
        m_modelSize = 0;
        m_loaded = false;
        backendRelease();
        x3::logInfo("[llm] unloaded (model + contexts freed, worker joined)");
    }

    std::size_t memoryUsedBytes() const override {
        std::lock_guard<std::mutex> lk(m_mx);
        std::size_t total = m_modelSize;
        for (const auto& kv : m_chats) total += kv.second.stateBytes;
        return total;
    }

    const char* backendName() const override { return "llama.cpp"; }

private:
    struct Chat {
        std::string     persona;
        llama_context*  ctx  = nullptr;   // WORKER-OWNED (created lazily there)
        llama_sampler*  smpl = nullptr;   // WORKER-OWNED
        bool            primed = false;   // system prompt decoded into the KV
        std::size_t     stateBytes = 0;   // KV/compute buffer estimate
        std::string     pending;
        int             pendingCount = 0;   // model tokens in `pending`
        bool            busy = false;
        bool            done = false;
        bool            failed = false;
        bool            cancelReq = false;
    };
    struct Job { ChatId chat; std::string text; };

    // ---------------- worker thread ----------------

    void workerMain() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(m_mx);
                m_cv.wait(lk, [this] { return m_quit || !m_jobs.empty() ||
                                              !m_retiredCtxs.empty() || !m_retiredSmpls.empty(); });
                sweepRetiredLocked();
                if (m_quit) return;
                if (m_jobs.empty()) continue;
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }
            if (job.chat == kInvalidChat) continue;   // chat ended while queued
            runJob(job);
        }
    }

    void sweepRetiredLocked() {
        for (llama_sampler* s : m_retiredSmpls) llama_sampler_free(s);
        for (llama_context* c : m_retiredCtxs)  llama_free(c);
        m_retiredSmpls.clear();
        m_retiredCtxs.clear();
    }

    bool tokenize(const std::string& text, bool addSpecial, std::vector<llama_token>& out) {
        const llama_vocab* vocab = llama_model_get_vocab(m_model);
        int n = -llama_tokenize(vocab, text.c_str(), (int32_t)text.size(),
                                nullptr, 0, addSpecial, /*parse_special*/true);
        if (n <= 0) return false;
        out.resize((size_t)n);
        n = llama_tokenize(vocab, text.c_str(), (int32_t)text.size(),
                           out.data(), (int32_t)out.size(), addSpecial, true);
        if (n < 0) return false;
        out.resize((size_t)n);
        return true;
    }

    // Decode a token span into the chat's KV (positions auto-tracked).
    bool decodeTokens(llama_context* ctx, std::vector<llama_token>& toks) {
        if (toks.empty()) return true;
        llama_batch batch = llama_batch_get_one(toks.data(), (int32_t)toks.size());
        return llama_decode(ctx, batch) == 0;
    }

    void emit(ChatId chat, const std::string& text, int tokenCount = 0) {
        std::lock_guard<std::mutex> lk(m_mx);
        auto it = m_chats.find(chat);
        if (it != m_chats.end()) {
            it->second.pending      += text;
            it->second.pendingCount += tokenCount;
        }
    }

    bool isCancelled(ChatId chat) {
        std::lock_guard<std::mutex> lk(m_mx);
        if (m_quit) return true;
        auto it = m_chats.find(chat);
        return it == m_chats.end() || it->second.cancelReq;
    }

    void finish(ChatId chat, bool failed) {
        std::lock_guard<std::mutex> lk(m_mx);
        auto it = m_chats.find(chat);
        if (it == m_chats.end()) return;
        it->second.done   = true;
        it->second.failed = failed;
        it->second.busy   = false;
    }

    void runJob(const Job& job) {
        // Snapshot what we need; create the context lazily (worker-side).
        std::string persona;
        llama_context* ctx  = nullptr;
        llama_sampler* smpl = nullptr;
        bool primed = false;
        {
            std::lock_guard<std::mutex> lk(m_mx);
            auto it = m_chats.find(job.chat);
            if (it == m_chats.end()) return;
            persona = it->second.persona;
            ctx     = it->second.ctx;
            smpl    = it->second.smpl;
            primed  = it->second.primed;
        }

        if (!ctx) {
            llama_context_params cp = llama_context_default_params();
            cp.n_ctx           = (uint32_t)m_opts.contextTokens;
            cp.n_batch         = (uint32_t)m_opts.contextTokens;   // prompt fits one decode
            int threads = m_opts.threads;
            if (threads <= 0) {
                // Hybrid-CPU default: half the logical threads (≈ physical
                // cores), capped at 16 — past that, CPU GEMM stops scaling and
                // the engine's frame/job threads start losing cores.
                threads = (int)std::thread::hardware_concurrency() / 2;
                // 6 (was 16): measured in-game — 16 llama threads on the 14900K
                // starved the render thread (~33ms/frame lost with the model merely
                // LOADED). Short NPC lines don't need GEMM scaling; frames do.
                if (threads > 6) threads = 6;
                if (threads < 1)  threads = 1;
            }
            cp.n_threads       = threads;
            cp.n_threads_batch = threads;
            ctx = llama_init_from_model(m_model, cp);
            if (!ctx) { emit(job.chat, "[LLM ERROR: context init failed]"); finish(job.chat, true); return; }

            llama_sampler_chain_params sp = llama_sampler_chain_default_params();
            smpl = llama_sampler_chain_init(sp);
            if (m_opts.temperature <= 0.0f) {
                llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
            } else {
                llama_sampler_chain_add(smpl, llama_sampler_init_min_p(0.05f, 1));
                llama_sampler_chain_add(smpl, llama_sampler_init_temp(m_opts.temperature));
                llama_sampler_chain_add(smpl, llama_sampler_init_dist(
                    m_opts.seed ? m_opts.seed : LLAMA_DEFAULT_SEED));
            }

            std::lock_guard<std::mutex> lk(m_mx);
            auto it = m_chats.find(job.chat);
            if (it == m_chats.end()) {
                // Chat ended between snapshot and init: retire immediately.
                llama_sampler_free(smpl);
                llama_free(ctx);
                return;
            }
            it->second.ctx        = ctx;
            it->second.smpl       = smpl;
            it->second.stateBytes = llama_state_get_size(ctx);
        }

        // ---- Build + decode this turn's prompt (ChatML). ----
        std::string turn;
        if (!primed)
            turn += "<|im_start|>system\n" + persona + "<|im_end|>\n";
        turn += "<|im_start|>user\n" + job.text + "<|im_end|>\n<|im_start|>assistant\n";

        std::vector<llama_token> toks;
        if (!tokenize(turn, /*addSpecial*/!primed, toks) || !decodeTokens(ctx, toks)) {
            emit(job.chat, "[LLM ERROR: prompt decode failed (context full?)]");
            finish(job.chat, true);
            return;
        }
        if (!primed) {
            std::lock_guard<std::mutex> lk(m_mx);
            auto it = m_chats.find(job.chat);
            if (it != m_chats.end()) it->second.primed = true;
        }

        // ---- Generate. ----
        const llama_vocab* vocab = llama_model_get_vocab(m_model);
        bool failed = false;
        int  generated = 0;
        for (; generated < m_opts.maxOutputTokens; ++generated) {
            if (isCancelled(job.chat)) break;
            const llama_token tok = llama_sampler_sample(smpl, ctx, -1);
            if (llama_vocab_is_eog(vocab, tok)) break;

            char buf[256];
            const int n = llama_token_to_piece(vocab, tok, buf, (int)sizeof(buf), 0, /*special*/false);
            if (n > 0) emit(job.chat, std::string(buf, (size_t)n), 1);

            std::vector<llama_token> one{ tok };
            if (!decodeTokens(ctx, one)) { failed = true; break; }   // KV full
        }

        // Close the assistant turn in the KV so the NEXT turn appends cleanly.
        // (The sampled EOG token was never decoded; this writes the canonical
        // "<|im_end|>\n" regardless of whether we stopped on EOG or the cap.)
        if (!failed) {
            std::vector<llama_token> closer;
            if (tokenize("<|im_end|>\n", false, closer)) {
                if (!decodeTokens(ctx, closer)) failed = true;
            }
        }
        if (failed) emit(job.chat, " [TRANSMISSION TRUNCATED]");
        finish(job.chat, failed);
    }

    // ---------------- state ----------------

    mutable std::mutex          m_mx;
    std::condition_variable     m_cv;
    std::thread                 m_worker;
    bool                        m_loaded = false;
    bool                        m_quit   = false;
    ModelOpts                   m_opts{};
    llama_model*                m_model = nullptr;
    std::size_t                 m_modelSize = 0;
    ChatId                      m_nextId = 1;
    std::map<ChatId, Chat>      m_chats;
    std::deque<Job>             m_jobs;
    std::vector<llama_context*> m_retiredCtxs;    // endChat'd; freed on the worker
    std::vector<llama_sampler*> m_retiredSmpls;
};

} // namespace

std::unique_ptr<ILlmSystem> createLlmSystem() {
    return std::make_unique<LlamaLlmSystem>();
}

} // namespace x3::llm
