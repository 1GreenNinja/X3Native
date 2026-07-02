// --test-llm — in-engine LLM plumbing self-test.
//
// PART A (always runs, no model file needed): the MOCK backend exercises the
// full async contract — load, startChat, submit/poll streaming, cancel,
// endChat, teardown-while-busy (join-before-destroy), double-unload.
// PART B (gated on the real .gguf existing): load Qwen2.5-3B-Instruct Q4_K_M,
// run ONE real prompt through the facility-AI persona, assert a non-empty
// reply that mentions the facility, log tokens/sec, unload clean.
// PART C (also gated on the .gguf): ai_gpu 0/1 parity — the SAME prompt through
// the CPU path and the GPU path both produce a non-empty in-persona reply
// (GPU auto-falls back to CPU on a non-CUDA build, so both still pass).
// The suite is green with AND without the model present.
#include "engine/llm/ILlmSystem.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace x3::llm {

namespace {

int  g_pass = 0;
int  g_fail = 0;

void check(bool ok, const char* what) {
    if (ok) { ++g_pass; x3::logInfo(std::string("  [llm-test] PASS ") + what); }
    else    { ++g_fail; x3::logError(std::string("  [llm-test] FAIL ") + what); }
}

// Drain a chat until done (or timeout). Returns the accumulated text and the
// number of NON-EMPTY polls (so streaming-in-pieces is observable).
struct DrainResult {
    std::string text;
    int    nonEmptyPolls = 0;
    int    tokens = 0;          // exact model-token count (PollResult metering)
    double firstTokenS = -1.0;  // time to first token, seconds
    bool   done = false;
};
DrainResult drain(ILlmSystem& sys, ChatId chat, int timeoutMs) {
    DrainResult r;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        PollResult p = sys.poll(chat);
        if (!p.newTokens.empty()) {
            if (r.firstTokenS < 0.0)
                r.firstTokenS = std::chrono::duration<double>(
                                    std::chrono::steady_clock::now() - t0).count();
            r.text += p.newTokens; ++r.nonEmptyPolls; r.tokens += p.newTokenCount;
        }
        if (p.done) { r.done = true; break; }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        if (ms > timeoutMs) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return r;
}

void testMockBackend() {
    x3::logInfo("[llm-test] PART A — mock backend plumbing");

    // L0: factory + load + identity.
    auto sys = createMockLlmSystem();
    check(sys != nullptr, "L0 createMockLlmSystem() returns a system");
    check(!sys->modelLoaded(), "L0 not loaded before loadModel");
    ModelOpts opts;
    check(sys->loadModel("(mock)", opts), "L0 loadModel succeeds");
    check(sys->modelLoaded(), "L0 modelLoaded() true after load");
    check(std::string(sys->backendName()) == "mock", "L0 backendName is 'mock'");

    // L1: chats are real handles; submit before startChat fails.
    check(!sys->submit(kInvalidChat, "x"), "L1 submit on kInvalidChat rejected");
    ChatId c1 = sys->startChat("PERSONA-A");
    check(c1 != kInvalidChat, "L1 startChat returns a valid id");

    // L2: deterministic echo + STREAMING (tokens arrive across multiple polls).
    check(sys->submit(c1, "what is this facility"), "L2 submit accepted (async)");
    DrainResult d = drain(*sys, c1, 5000);
    check(d.done, "L2 generation reports done");
    check(d.text == "MOCK[1]: WHAT IS THIS FACILITY", ("L2 deterministic echo text (got '" + d.text + "')").c_str());
    check(d.nonEmptyPolls >= 2, "L2 tokens streamed over >=2 polls (not one blob)");

    // L3: second turn on the same chat; busy-rejection while generating.
    check(sys->submit(c1, "hello"), "L3 second submit accepted");
    check(!sys->submit(c1, "overlap"), "L3 overlapping submit rejected while busy");
    d = drain(*sys, c1, 5000);
    check(d.done && d.text == "MOCK[1]: HELLO", "L3 second-turn echo correct");

    // L4: cancel stops a long generation early.
    std::string longText;
    for (int i = 0; i < 400; ++i) longText += "word ";
    check(sys->submit(c1, longText), "L4 long submit accepted");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sys->cancel(c1);
    d = drain(*sys, c1, 5000);
    check(d.done, "L4 cancelled generation reports done");
    check(d.text.size() < longText.size(), "L4 cancel stopped output early");

    // L5: multiple chats are independent.
    ChatId c2 = sys->startChat("PERSONA-B");
    check(c2 != kInvalidChat && c2 != c1, "L5 second chat distinct");
    check(sys->submit(c2, "ping"), "L5 submit on chat 2");
    d = drain(*sys, c2, 5000);
    check(d.done && d.text == "MOCK[2]: PING", "L5 chat-2 echo isolated (ordinal 2)");
    sys->endChat(c2);
    PollResult dead = sys->poll(c2);
    check(dead.done && dead.failed, "L5 poll on ended chat reports done+failed");

    // L6: teardown WHILE a generation is in flight (join-before-destroy).
    std::string longer;
    for (int i = 0; i < 1000; ++i) longer += "tok ";
    sys->submit(c1, longer);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sys->unload();                       // must join the worker, not crash/leak
    check(!sys->modelLoaded(), "L6 unload() while busy: clean stop, not loaded");
    sys->unload();                       // double-unload is safe
    check(true, "L6 double unload safe");
    check(sys->memoryUsedBytes() == 0, "L6 memoryUsedBytes 0 after unload");
    sys.reset();                         // dtor after unload: no-op
    check(true, "L6 destructor after unload safe");

    // L7: destructor JOINS a live generation (no explicit unload).
    {
        auto sys2 = createMockLlmSystem();
        sys2->loadModel("(mock)", opts);
        ChatId c = sys2->startChat("PERSONA-C");
        sys2->submit(c, longer);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        // sys2 destroyed here mid-generation — must join cleanly.
    }
    check(true, "L7 destroy mid-generation joins cleanly");
}

void testRealModel(const std::string& path) {
    x3::logInfo("[llm-test] PART B — real model: " + path);

    auto sys = createLlmSystem();
    check(sys != nullptr, "R0 createLlmSystem() returns a system");

    ModelOpts opts;
    opts.contextTokens   = 2048;
    opts.maxOutputTokens = 192;
    opts.temperature     = 0.7f;
    opts.seed            = 42;          // deterministic-ish for the suite

    const auto tLoad0 = std::chrono::steady_clock::now();
    const bool loaded = sys->loadModel(path, opts);
    const double loadS = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - tLoad0).count();
    check(loaded, "R1 real model loads");
    if (!loaded) return;
    x3::logInfo("  [llm-test] model loaded in " + std::to_string(loadS) + " s, ~" +
                std::to_string(sys->memoryUsedBytes() / (1024 * 1024)) + " MB resident");

    ChatId chat = sys->startChat(
        "You are the facility intelligence of Lab Zero, a 283-meter research "
        "spire. Answer the prisoner's questions tersely, in character. Always "
        "name the facility (Lab Zero, the Spire) when asked about it.");
    check(chat != kInvalidChat, "R2 startChat with persona");

    const auto tGen0 = std::chrono::steady_clock::now();
    check(sys->submit(chat, "What is this facility?"), "R3 real submit accepted");
    DrainResult d = drain(*sys, chat, 120000);
    const double genS = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - tGen0).count();
    check(d.done, "R4 real generation completes");
    check(!d.text.empty(), "R5 reply non-empty");
    std::string low = d.text;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char ch) { return (char)std::tolower(ch); });
    const bool mentions = low.find("lab zero") != std::string::npos ||
                          low.find("spire")    != std::string::npos ||
                          low.find("facility") != std::string::npos;
    check(mentions, "R6 reply mentions the facility");
    // tok/s metered with EXACT token counts: prompt eval = time to first token;
    // decode rate = remaining tokens over remaining time.
    const double decodeS = genS - (d.firstTokenS > 0.0 ? d.firstTokenS : 0.0);
    const double tokPerS = (d.tokens > 1 && decodeS > 0.0) ? (d.tokens - 1) / decodeS : 0.0;
    x3::logInfo("  [llm-test] reply: " + std::to_string(d.tokens) + " tokens in " +
                std::to_string(genS) + " s (first token at " +
                std::to_string(d.firstTokenS) + " s incl. prompt eval) => " +
                std::to_string(tokPerS) + " tok/s decode");
    x3::logInfo("  [llm-test] reply text: " + d.text);

    // R7: SECOND TURN on the same chat — validates the multi-turn KV path and
    // gives a longer generation for an honest sustained-decode tok/s number.
    const auto tGen1 = std::chrono::steady_clock::now();
    check(sys->submit(chat, "Describe this facility floor by floor, in detail."),
          "R7 second-turn submit accepted");
    DrainResult d2 = drain(*sys, chat, 180000);
    const double gen2S = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - tGen1).count();
    check(d2.done && !d2.text.empty(), "R8 second-turn reply completes non-empty");
    const double decode2S = gen2S - (d2.firstTokenS > 0.0 ? d2.firstTokenS : 0.0);
    const double tokPerS2 = (d2.tokens > 1 && decode2S > 0.0) ? (d2.tokens - 1) / decode2S : 0.0;
    x3::logInfo("  [llm-test] turn 2: " + std::to_string(d2.tokens) + " tokens, first at " +
                std::to_string(d2.firstTokenS) + " s, sustained " +
                std::to_string(tokPerS2) + " tok/s decode");
    x3::logInfo("  [llm-test] turn-2 text: " + d2.text);

    sys->unload();
    check(!sys->modelLoaded(), "R9 unload clean");
}

// PART C — ai_gpu 0/1 parity: the SAME prompt through the CPU path (gpuLayers=0)
// and the GPU path (gpuLayers=99) must BOTH produce a non-empty, in-persona
// reply. On a box without a CUDA build the GPU request auto-falls back to CPU,
// so both cases still pass (and log which backend actually ran); on a CUDA build
// this exercises the real GPU offload path end to end.
void testGpuCpuParity(const std::string& path) {
    x3::logInfo("[llm-test] PART C — ai_gpu 0/1 parity (same prompt, both paths in-persona)");
    const char* persona =
        "You are the facility intelligence of Lab Zero, a 283-meter research "
        "spire. Answer the prisoner's questions tersely, in character. Always "
        "name the facility (Lab Zero, the Spire) when asked about it.";
    const char* prompt = "What is this facility?";
    for (int gpu = 0; gpu <= 1; ++gpu) {
        const char* tag = gpu ? "ai_gpu=1" : "ai_gpu=0";
        auto sys = createLlmSystem();
        ModelOpts opts;
        opts.contextTokens   = 2048;
        opts.maxOutputTokens = 96;
        opts.temperature     = 0.0f;      // greedy: deterministic per path
        opts.seed            = 42;
        opts.gpuLayers       = gpu ? 99 : 0;
        const bool loaded = sys->loadModel(path, opts);
        check(loaded, gpu ? "C1 model loads (ai_gpu=1)" : "C1 model loads (ai_gpu=0)");
        if (!loaded) continue;
        x3::logInfo(std::string("  [llm-test] ") + tag + " backend: " + sys->backendName());
        ChatId chat = sys->startChat(persona);
        check(chat != kInvalidChat, gpu ? "C2 startChat (ai_gpu=1)" : "C2 startChat (ai_gpu=0)");
        check(sys->submit(chat, prompt), gpu ? "C3 submit (ai_gpu=1)" : "C3 submit (ai_gpu=0)");
        DrainResult d = drain(*sys, chat, 120000);
        check(d.done && !d.text.empty(),
              gpu ? "C4 non-empty reply (ai_gpu=1)" : "C4 non-empty reply (ai_gpu=0)");
        std::string low = d.text;
        std::transform(low.begin(), low.end(), low.begin(),
                       [](unsigned char ch) { return (char)std::tolower(ch); });
        const bool inPersona = low.find("lab zero") != std::string::npos ||
                               low.find("spire")    != std::string::npos ||
                               low.find("facility") != std::string::npos;
        check(inPersona, gpu ? "C5 reply in-persona (ai_gpu=1)" : "C5 reply in-persona (ai_gpu=0)");
        x3::logInfo(std::string("  [llm-test] ") + tag + " reply: " + d.text);
        sys->unload();
    }
}

} // namespace

bool runLlmSelfTest(const char* modelPath) {
    g_pass = 0; g_fail = 0;

    testMockBackend();

    std::error_code ec;
    if (modelPath && std::filesystem::exists(modelPath, ec)) {
        testRealModel(modelPath);
        testGpuCpuParity(modelPath);
    } else {
        x3::logInfo(std::string("[llm-test] PART B skipped — no model at ") +
                    (modelPath ? modelPath : "(null)") +
                    " (suite stays green modelless by design)");
    }

    x3::logInfo("[llm-test] " + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::llm
