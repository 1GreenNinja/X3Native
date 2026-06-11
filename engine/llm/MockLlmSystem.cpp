// MockLlmSystem — deterministic echo backend for --test-llm and harnesses.
//
// Exercises the FULL async contract of ILlmSystem with zero model weight: a
// real dedicated worker thread, a job queue, mutex-guarded per-chat token
// buffers, between-token cancellation, and join-before-destroy teardown. The
// reply for submit(chat, "what is this") is deterministic:
//   "MOCK[<n>]: WHAT IS THIS" streamed one word per "token".
// where <n> = startChat order (1-based), so tests can assert exact text.
#include "engine/llm/ILlmSystem.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace x3::llm {

namespace {

class MockLlmSystem final : public ILlmSystem {
public:
    MockLlmSystem() = default;
    ~MockLlmSystem() override { unload(); }

    bool loadModel(const std::string& ggufPath, const ModelOpts& opts) override {
        (void)ggufPath;   // the mock "loads" anything, including a missing path
        std::lock_guard<std::mutex> lk(m_mx);
        m_opts = opts;
        if (!m_loaded) {
            m_quit = false;
            m_worker = std::thread([this] { workerMain(); });
            m_loaded = true;
        }
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
        Chat& c = m_chats[id];
        c.ordinal = (std::uint32_t)m_chats.size();   // 1-based startChat order
        c.persona = personaSystemPrompt;
        return id;
    }

    bool submit(ChatId chat, const std::string& userText) override {
        std::lock_guard<std::mutex> lk(m_mx);
        auto it = m_chats.find(chat);
        if (it == m_chats.end() || it->second.busy) return false;
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
        r.done = it->second.done;
        return r;
    }

    void cancel(ChatId chat) override {
        std::lock_guard<std::mutex> lk(m_mx);
        auto it = m_chats.find(chat);
        if (it != m_chats.end()) it->second.cancelReq = true;
    }

    void endChat(ChatId chat) override {
        std::lock_guard<std::mutex> lk(m_mx);
        auto it = m_chats.find(chat);
        if (it == m_chats.end()) return;
        it->second.cancelReq = true;
        // Drop queued (not yet running) jobs for this chat; an in-flight job
        // sees cancelReq and finishes early — its writes land in a dead map
        // entry guard (the worker re-looks the chat up under the lock).
        for (auto& j : m_jobs) if (j.chat == chat) j.chat = kInvalidChat;
        m_chats.erase(it);
    }

    void unload() override {
        {
            std::lock_guard<std::mutex> lk(m_mx);
            if (!m_loaded) return;
            m_quit = true;
            m_cv.notify_one();
        }
        if (m_worker.joinable()) m_worker.join();   // JOIN BEFORE FREE
        std::lock_guard<std::mutex> lk(m_mx);
        m_chats.clear();
        m_jobs.clear();
        m_loaded = false;
    }

    std::size_t memoryUsedBytes() const override {
        std::lock_guard<std::mutex> lk(m_mx);
        return m_loaded ? m_chats.size() * 1024 + 4096 : 0;
    }

    const char* backendName() const override { return "mock"; }

private:
    struct Chat {
        std::uint32_t ordinal = 0;
        std::string   persona;
        std::string   pending;        // tokens awaiting poll()
        int           pendingCount = 0;
        bool          busy = false;   // a job is queued/running
        bool          done = false;   // last job finished
        bool          cancelReq = false;
    };
    struct Job { ChatId chat; std::string text; };

    void workerMain() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(m_mx);
                m_cv.wait(lk, [this] { return m_quit || !m_jobs.empty(); });
                if (m_quit) return;
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }
            runJob(job);
        }
    }

    void runJob(const Job& job) {
        // Deterministic reply: "MOCK[<ordinal>]: <UPPERCASED INPUT>".
        std::uint32_t ordinal = 0;
        {
            std::lock_guard<std::mutex> lk(m_mx);
            auto it = m_chats.find(job.chat);
            if (it == m_chats.end()) return;     // chat ended while queued
            ordinal = it->second.ordinal;
        }
        // Tokenize the input into words; stream them one per "token".
        std::vector<std::string> words;
        std::string cur;
        for (char ch : job.text) {
            if (std::isspace((unsigned char)ch)) { if (!cur.empty()) { words.push_back(cur); cur.clear(); } }
            else cur += (char)std::toupper((unsigned char)ch);
        }
        if (!cur.empty()) words.push_back(cur);

        emit(job.chat, "MOCK[" + std::to_string(ordinal) + "]:");
        bool cancelled = false;
        for (const std::string& w : words) {
            // A tiny sleep per token so cancel/poll interleaving is REAL.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            {
                std::lock_guard<std::mutex> lk(m_mx);
                auto it = m_chats.find(job.chat);
                if (it == m_chats.end() || it->second.cancelReq || m_quit) { cancelled = true; }
            }
            if (cancelled) break;
            emit(job.chat, " " + w);
        }
        std::lock_guard<std::mutex> lk(m_mx);
        auto it = m_chats.find(job.chat);
        if (it != m_chats.end()) { it->second.done = true; it->second.busy = false; }
    }

    void emit(ChatId chat, const std::string& text) {
        std::lock_guard<std::mutex> lk(m_mx);
        auto it = m_chats.find(chat);
        if (it != m_chats.end()) { it->second.pending += text; ++it->second.pendingCount; }
    }

    mutable std::mutex      m_mx;
    std::condition_variable m_cv;
    std::thread             m_worker;
    bool                    m_loaded = false;
    bool                    m_quit   = false;
    ModelOpts               m_opts{};
    ChatId                  m_nextId = 1;
    std::map<ChatId, Chat>  m_chats;
    std::deque<Job>         m_jobs;
};

} // namespace

std::unique_ptr<ILlmSystem> createMockLlmSystem() {
    return std::make_unique<MockLlmSystem>();
}

} // namespace x3::llm
