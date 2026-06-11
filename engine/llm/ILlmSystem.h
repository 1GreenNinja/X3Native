#pragma once
// In-engine LLM interface — living NPC minds over llama.cpp.
//
// QUARANTINE DISCIPLINE (same as IRenderDevice / IConsole / INavigation): NO
// llama.cpp types appear in this header. The llama backend lives entirely inside
// engine/llm/LlamaLlmSystem.cpp (linked PRIVATE), so engine consumers compile
// against pure std:: types and the game keeps working when the model file (or
// the whole backend) is absent.
//
// THREADING CONTRACT: submit() is ASYNC — it enqueues a generation job for the
// system's dedicated inference thread and returns immediately. Generated tokens
// accumulate in a mutex-guarded per-chat buffer; the frame thread drains them
// with poll() (non-blocking). cancel() requests a stop between tokens. The
// destructor / unload() JOIN the worker before freeing backend state (the
// job-bridge lesson: never destroy state a live thread can still touch).
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace x3::llm {

// Opaque conversation handle. 0 is never a valid chat.
using ChatId = std::uint32_t;
inline constexpr ChatId kInvalidChat = 0;

// Model/load + generation options (defaults mirror the ai_* cvars).
struct ModelOpts {
    int   contextTokens   = 2048;   // ai_ctx — KV context length per chat
    int   maxOutputTokens = 256;    // ai_maxtokens — per-reply generation cap
    float temperature     = 0.7f;   // ai_temp
    int   threads         = 0;      // inference threads; 0 = hardware_concurrency-2
    std::uint32_t seed    = 0;      // sampler seed; 0 = nondeterministic
};

// poll() result: everything generated since the last poll, plus terminal state.
struct PollResult {
    std::string newTokens;        // freshly generated text (may be empty)
    int         newTokenCount = 0;// model tokens in newTokens (for tok/s metering)
    bool        done   = false;   // generation finished (EOG / token cap / cancel)
    bool        failed = false;   // generation aborted on an error (done is also true)
};

class ILlmSystem {
public:
    virtual ~ILlmSystem() = default;

    // Load a model (blocking; call during a load phase, not mid-frame). Returns
    // false if the file is missing/unreadable — the caller falls back to canned
    // responses (the game must work modelless).
    virtual bool loadModel(const std::string& ggufPath, const ModelOpts& opts) = 0;
    virtual bool modelLoaded() const = 0;

    // Open a conversation seeded with a persona/system prompt. Returns
    // kInvalidChat if no model is loaded.
    virtual ChatId startChat(const std::string& personaSystemPrompt) = 0;

    // Queue a user message for generation (ASYNC — returns immediately).
    // Returns false if the chat is unknown or a reply is already generating.
    virtual bool submit(ChatId chat, const std::string& userText) = 0;

    // Drain tokens generated since the last poll. Non-blocking; frame-safe.
    virtual PollResult poll(ChatId chat) = 0;

    // Request the in-flight generation stop at the next token boundary. The
    // reply finishes early: poll() reports done once the worker acknowledges.
    virtual void cancel(ChatId chat) = 0;

    // Close a conversation and free its context memory. Cancels first.
    virtual void endChat(ChatId chat) = 0;

    // Drop the model + all chats (joins the worker thread). Safe to call twice.
    virtual void unload() = 0;

    // Approximate resident bytes (model weights + per-chat KV/compute buffers).
    virtual std::size_t memoryUsedBytes() const = 0;

    // "llama.cpp" / "mock" — for boot logs and the self-test.
    virtual const char* backendName() const = 0;
};

// The real backend (llama.cpp, CPU inference). Always constructible — if the
// engine was built without the llama backend this still returns a stub whose
// loadModel() always fails (callers use the same modelless fallback path).
std::unique_ptr<ILlmSystem> createLlmSystem();

// Deterministic echo backend for plumbing tests (--test-llm) and harnesses:
// loadModel() accepts any path, submit("x") streams "MOCK(<persona-hash>): X"
// word-by-word from a real worker thread (so the async contract is exercised).
std::unique_ptr<ILlmSystem> createMockLlmSystem();

// --test-llm: mock-backend plumbing tests (submit/poll/stream/cancel/teardown)
// always run; if `modelPath` names an existing .gguf, a real load + one real
// prompt round-trip runs too (and tokens/sec is logged). Returns true if all
// executed checks pass — the suite is green with AND without the model file.
bool runLlmSelfTest(const char* modelPath);

} // namespace x3::llm
