// AI-powered dialog + TTS voice on skinned NPCs — implementation. See dialog.h.
//
// Offline-safe by construction: the only outward calls are the HOST-injected
// std::function hooks (AiProviderFn / TtsProviderFn) and the borrowed IAudioSystem
// / ISpeakingNpc. With no hooks set, the system is a deterministic authored-tree
// walker. NO socket, NO key, NO HTTP — the host owns every cloud touch.

#include "dialog.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_set>

namespace x3::dialog {

// ---------------------------------------------------------------------------
// Names + small helpers.
// ---------------------------------------------------------------------------
const char* voiceName(VoiceId v) {
    switch (v) {
        case VoiceId::Narrator: return "Narrator";
        case VoiceId::Sarah:    return "Sarah";
        case VoiceId::Kthara:   return "K'thara";
        case VoiceId::Jake:     return "Jake";
        default:                return "?";
    }
}

const char* modeName(Mode m) {
    switch (m) {
        case Mode::Tree:   return "Tree";
        case Mode::Ai:     return "Ai";
        case Mode::Hybrid: return "Hybrid";
        default:           return "?";
    }
}

// Estimate a spoken duration from a line's length when the TTS vendor didn't
// supply one (and for the silent / subtitle-only path). ~13 chars/sec reading
// pace, clamped to a sane floor/ceiling so a one-word line still shows a beat and
// a long line doesn't hang forever. Pure; deterministic.
static float estimateDuration(std::string_view line) {
    constexpr float kCharsPerSec = 13.0f;
    constexpr float kMin = 1.2f;
    constexpr float kMax = 12.0f;
    float d = (float)line.size() / kCharsPerSec;
    if (d < kMin) d = kMin;
    if (d > kMax) d = kMax;
    return d;
}

// ---------------------------------------------------------------------------
// Tree.
// ---------------------------------------------------------------------------
const Node* Tree::nodeById(int32_t id) const {
    if (id == kEndNode || id == kNoNode) return nullptr;
    for (const Node& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

bool Tree::validate() const {
    if (nodes.empty()) {
        x3::logError("dialog: tree has no nodes");
        return false;
    }
    // Unique ids.
    std::unordered_set<int32_t> ids;
    for (const Node& n : nodes) {
        if (n.id == kEndNode || n.id == kNoNode) {
            x3::logError("dialog: node uses a reserved sentinel id");
            return false;
        }
        if (!ids.insert(n.id).second) {
            x3::logError("dialog: duplicate node id " + std::to_string(n.id));
            return false;
        }
    }
    // Start node exists.
    if (nodeById(startNode) == nullptr) {
        x3::logError("dialog: start node " + std::to_string(startNode) + " missing");
        return false;
    }
    // Every choice target is a real node or kEndNode.
    for (const Node& n : nodes) {
        for (const Choice& c : n.choices) {
            if (c.nextNode != kEndNode && nodeById(c.nextNode) == nullptr) {
                x3::logError("dialog: node " + std::to_string(n.id) +
                             " choice -> missing node " + std::to_string(c.nextNode));
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Sample authored trees (offline content). These are real, playable branches the
// blueprint calls for; they are the guaranteed path with NO network.
// ---------------------------------------------------------------------------
Tree sampleSarahTree() {
    // Sarah, the hacker ally, on the comm as Jake wakes in the detention cell (F1).
    Tree t;
    t.startNode = 100;
    t.nodes = {
        { 100, "Sarah", VoiceId::Sarah,
          "Jake! You're awake — thank God. I'm Sarah, I've been hacking your cell "
          "feed. Listen, the whole facility thinks you're still sedated. We have a "
          "narrow window.",
          {
            { "Where am I? What happened to me?", 110 },
            { "Who are you and why are you helping me?", 120 },
            { "Just get me out of here.", 130 },
          } },
        { 110, "Sarah", VoiceId::Sarah,
          "You're Subject 7-Alpha, three floors underground in Lab Zero. They "
          "rewrote you, Jake — you're stronger than anything down here now. Use it.",
          {
            { "Stronger how?", 111 },
            { "Then let's move.", 130 },
          } },
        { 111, "Sarah", VoiceId::Sarah,
          "Try the cell door. Don't knock — TEAR it. I'll have the corridor blind "
          "for ninety seconds.",
          {
            { "On it.", 130 },
          } },
        { 120, "Sarah", VoiceId::Sarah,
          "I'm on the inside, and I want out as badly as you do. Trust me or don't, "
          "but I'm the only voice down here that isn't trying to dissect you.",
          {
            { "Fair enough. What's the plan?", 130 },
            { "Why should I believe you?", 121 },
          } },
        { 121, "Sarah", VoiceId::Sarah,
          "Because I just unlocked your cell instead of venting it. Now MOVE — "
          "Martinez is already on his way down.",
          {
            { "Understood.", 130 },
          } },
        { 130, "Sarah", VoiceId::Sarah,
          "Head for the elevator at the end of the block. Chief Martinez will try to "
          "stop you. Drop him and the floor is yours. I'll be in your ear the whole way.",
          {} },   // terminal: conversation ends after this line
    };
    return t;
}

Tree sampleKtharaTree() {
    // K'thara, the Salvari mentor, on first contact on the alien surface (Act 2).
    Tree t;
    t.startNode = 200;
    t.nodes = {
        { 200, "K'thara", VoiceId::Kthara,
          "Stand still, surface-walker. Your kind wears the Dominion's stink, yet "
          "you bleed like the hunted. I am K'thara of the Salvari. Speak — quickly.",
          {
            { "I escaped the facility. I'm not with them.", 210 },
            { "I need allies. Will the Salvari help me?", 220 },
            { "Lower your weapon and we'll talk.", 230 },
          } },
        { 210, "K'thara", VoiceId::Kthara,
          "Escaped. They do not let their weapons escape — they LOSE them, and then "
          "they hunt them. That makes you useful, and hunted, like us.",
          {
            { "Then we want the same thing.", 220 },
            { "Useful enough to trust?", 230 },
          } },
        { 220, "K'thara", VoiceId::Kthara,
          "The Salvari help those who fight the hive, not those who merely flee it. "
          "Prove your aim is at the Overlord and not at your own escape, and you will "
          "have a hundred blades at your back.",
          {
            { "Point me at the Overlord.", 240 },
            { "I'll prove it.", 240 },
          } },
        { 230, "K'thara", VoiceId::Kthara,
          "Hah. Bold, for prey. Very well — my blade is lowered. But if your hands "
          "twitch wrong, surface-walker, mine will be the last face you misjudge.",
          {
            { "Understood. Let's talk.", 220 },
          } },
        { 240, "K'thara", VoiceId::Kthara,
          "Then walk with me to the camp. There is much you do not yet know about "
          "this world — and about what was done to you. The crystals remember.",
          {} },   // terminal
    };
    return t;
}

// ===========================================================================
// DialogSystem — the concrete IDialogSystem.
// ===========================================================================
namespace {

class DialogSystem final : public IDialogSystem {
public:
    DialogSystem() {
        m_history.reserve(kHistoryMax + 1);
        m_aiCtxHistory.reserve(kHistoryMax + 1);
    }

    // ---- wiring ----
    void setAiProvider(AiProviderFn fn) override  { m_ai  = std::move(fn); }
    void setTtsProvider(TtsProviderFn fn) override { m_tts = std::move(fn); }
    void setAudioSystem(x3::audio::IAudioSystem* a) override { m_audio = a; }
    void setSpeakingNpc(ISpeakingNpc* n) override { m_npc = n; }
    void setMode(Mode m) override { m_mode = m; }
    Mode mode() const override { return m_mode; }

    // ---- run ----
    bool start(const Tree& tree) override {
        if (!tree.validate()) return false;
        m_tree = &tree;
        m_history.clear();
        m_aiCtxHistory.clear();
        m_linesDelivered = 0;
        m_lastFromProvider = false;
        m_lastHadVoice = false;
        m_active = true;
        deliverNode(tree.startNode, /*choiceText=*/"");
        return true;
    }

    bool active() const override { return m_active; }
    std::string_view currentLine() const override { return m_curLine; }
    std::string_view currentSpeaker() const override { return m_curSpeaker; }
    bool speaking() const override { return m_speaking; }

    const std::vector<Choice>& choices() const override {
        // While speaking (or inactive) no choices are offered yet.
        static const std::vector<Choice> kNone;
        if (!m_active || m_speaking) return kNone;
        return m_curChoices;
    }

    bool choose(uint32_t index) override {
        if (!m_active || m_speaking) return false;
        if (index >= m_curChoices.size()) return false;
        const Choice& c = m_curChoices[index];
        // Record the player's pick in history (short-term memory for the provider).
        pushHistory("You", c.text);
        const std::string choiceText = c.text;   // copy before deliver mutates state
        if (c.nextNode == kEndNode) {
            endConversation();
            return true;
        }
        deliverNode(c.nextNode, choiceText);
        return true;
    }

    void skip() override {
        if (!m_active || !m_speaking) return;
        stopVoice();
        exitSpeaking();
    }

    void update(float dt) override {
        if (!m_active || !m_speaking) return;
        m_lineTimer += dt;
        float phase = (m_lineDuration > 0.0f) ? (m_lineTimer / m_lineDuration) : 1.0f;
        if (phase > 1.0f) phase = 1.0f;
        if (m_npc) m_npc->tickSpeaking(dt, phase);
        if (m_lineTimer >= m_lineDuration) {
            // Line finished playing -> exit speaking, reveal choices.
            stopVoice();
            exitSpeaking();
        }
    }

    uint32_t linesDelivered() const override { return m_linesDelivered; }
    bool lastLineFromProvider() const override { return m_lastFromProvider; }
    bool lastLineHadVoice() const override { return m_lastHadVoice; }

private:
    // Deliver the NPC line at `nodeId`: resolve the line (tree / provider), fire
    // TTS, enter the speaking state, and stage the node's choices. `choiceText` is
    // the player choice that led here ("" at the start). Fires hooks AT MOST ONCE.
    void deliverNode(int32_t nodeId, std::string_view choiceText) {
        const Node* node = m_tree ? m_tree->nodeById(nodeId) : nullptr;
        if (!node) { endConversation(); return; }

        m_curNode    = nodeId;
        m_curSpeaker = node->speaker;
        m_curVoice   = node->voice;
        m_curChoices = node->choices;   // copy (small, authored once)

        // --- 1. Resolve the spoken line: authored tree, or AI provider. ---
        m_lastFromProvider = false;
        std::string line = node->line;             // authored fallback (the baseline)
        if ((m_mode == Mode::Ai || m_mode == Mode::Hybrid) && m_ai) {
            DialogContext ctx;
            ctx.speaker      = node->speaker;
            ctx.voice        = node->voice;
            ctx.authoredLine = node->line;
            ctx.playerChoice = choiceText;
            ctx.history      = &m_aiCtxHistory;
            ctx.nodeId       = nodeId;
            std::string gen = m_ai(ctx);           // <-- the ONLY AI touch; host owns it
            if (!gen.empty()) {                    // empty => graceful fallback to tree
                line = std::move(gen);
                m_lastFromProvider = true;
            }
        }
        m_curLine = std::move(line);

        // History + counters.
        pushHistory(node->speaker, m_curLine);
        ++m_linesDelivered;

        // --- 2. TTS: request a clip (once) + play it; set the line duration. ---
        m_lastHadVoice = false;
        float clipDur = 0.0f;
        if (m_tts) {
            AudioClip clip = m_tts(m_curLine, m_curVoice);   // <-- the ONLY TTS touch
            if (!clip.empty()) {
                m_lastHadVoice = true;
                clipDur = clip.durationSec;
                playClip(clip);
            }
        }
        m_lineDuration = (clipDur > 0.0f) ? clipDur : estimateDuration(m_curLine);
        m_lineTimer    = 0.0f;

        // --- 3. Enter the speaking state on the skinned NPC (talk pose / bob). ---
        enterSpeaking();
    }

    void enterSpeaking() {
        m_speaking = true;
        if (m_npc) m_npc->beginSpeaking(m_curLine, m_curVoice, m_lineDuration);
    }

    void exitSpeaking() {
        if (!m_speaking) return;
        m_speaking = false;
        if (m_npc) m_npc->endSpeaking();
        // A terminal node (no choices) ends the conversation once its line finishes.
        if (m_active && m_curChoices.empty()) endConversation();
    }

    void endConversation() {
        if (m_speaking) {
            stopVoice();
            m_speaking = false;
            if (m_npc) m_npc->endSpeaking();
        }
        m_active = false;
        m_curChoices.clear();
        m_curNode = kNoNode;
    }

    // Play the TTS clip through the borrowed audio system (path-based clips). PCM
    // sample clips are accepted (m_lastHadVoice already set) but the engine's audio
    // path here plays files; a host that returns PCM wires its own playback. Either
    // way the duration paces the subtitle + speaking state. Null audio => silent
    // (still counts as "had voice" so the speaking state + pacing run).
    void playClip(const AudioClip& clip) {
        if (!m_audio) return;
        if (!clip.path.empty()) {
            x3::audio::SoundHandle h = m_audio->load(clip.path);
            if (h.valid()) {
                m_voice = h;
                m_audio->playSound2D(h, 1.0f, 1.0f);
            }
        }
        // (PCM-sample clips: a host helper would stream clip.samples; not wired in
        // the engine to avoid pulling raw-PCM playback into this module.)
    }

    void stopVoice() {
        // The miniaudio one-shot frees itself; nothing to explicitly stop for a 2D
        // one-shot. Reset the handle so a later skip()/end() is idempotent.
        m_voice = {};
    }

    // Append "Name: line" to the bounded history (drops the oldest past kHistoryMax).
    void pushHistory(std::string_view name, std::string_view line) {
        std::string entry;
        entry.reserve(name.size() + 2 + line.size());
        entry.append(name).append(": ").append(line);
        m_history.push_back(entry);
        m_aiCtxHistory.push_back(std::move(entry));
        while (m_history.size() > kHistoryMax) m_history.erase(m_history.begin());
        while (m_aiCtxHistory.size() > kHistoryMax) m_aiCtxHistory.erase(m_aiCtxHistory.begin());
    }

    // ---- wiring (borrowed; not owned) ----
    AiProviderFn               m_ai;
    TtsProviderFn              m_tts;
    x3::audio::IAudioSystem*   m_audio = nullptr;
    ISpeakingNpc*              m_npc   = nullptr;
    Mode                       m_mode  = Mode::Tree;

    // ---- run state ----
    const Tree*       m_tree = nullptr;
    bool              m_active   = false;
    bool              m_speaking = false;
    int32_t           m_curNode  = kNoNode;
    std::string       m_curLine;
    std::string       m_curSpeaker;
    VoiceId           m_curVoice = VoiceId::Narrator;
    std::vector<Choice> m_curChoices;

    float             m_lineTimer    = 0.0f;
    float             m_lineDuration = 0.0f;

    // Two copies of the history: m_history for general bookkeeping, m_aiCtxHistory
    // is the exact vector handed to the provider via DialogContext (same content;
    // kept separate so the borrowed pointer is always valid + bounded).
    std::vector<std::string> m_history;
    std::vector<std::string> m_aiCtxHistory;

    x3::audio::SoundHandle m_voice{};   // last-played TTS one-shot handle

    // ---- diagnostics ----
    uint32_t m_linesDelivered   = 0;
    bool     m_lastFromProvider = false;
    bool     m_lastHadVoice     = false;
};

} // namespace

IDialogSystem* createDialogSystem() { return new DialogSystem(); }

// ===========================================================================
// Headless self-test (--test-dialog). Fully offline; stub hooks only.
// ===========================================================================
namespace {

// A stub speaking-NPC that records the speaking lifecycle WITHOUT any renderer /
// Skinner — it just counts begin/tick/end so the test can assert the NPC enters
// and exits the speaking state across a line. (The real host implementation drives
// a MonsterSystem's anim Skinner read-only; see SpeakingMonster in main.cpp.)
struct StubNpc final : ISpeakingNpc {
    int   begins = 0, ends = 0, ticks = 0;
    bool  speakingNow = false;
    float lastEstDur = 0.0f;
    float lastPhase = 0.0f;
    VoiceId lastVoice = VoiceId::Narrator;
    std::string lastLine;

    void beginSpeaking(std::string_view line, VoiceId voice, float estDur) override {
        ++begins; speakingNow = true; lastEstDur = estDur; lastVoice = voice;
        lastLine.assign(line);
    }
    void tickSpeaking(float dt, float phase01) override { (void)dt; ++ticks; lastPhase = phase01; }
    void endSpeaking() override { ++ends; speakingNow = false; }
};

// A tiny deterministic stub AI provider: returns an in-character line built PURELY
// from the local context (NO network) so the test can assert the system used the
// provider's reply over the authored tree line. Marker prefix makes it identifiable.
static std::string stubAiProvider(const DialogContext& ctx) {
    std::string out = "[AI:";
    out += voiceName(ctx.voice);
    out += "] ";
    // Echo a transformed version of the authored line so it is clearly provider-made
    // yet in-context (a host's real Claude/Grok call would do the creative rewrite).
    out += "(rewritten) ";
    out.append(ctx.authoredLine.substr(0, std::min<size_t>(ctx.authoredLine.size(), 24)));
    return out;
}

// An AI provider that always returns empty -> must fall back to the authored tree.
static std::string emptyAiProvider(const DialogContext&) { return std::string(); }

// A stub TTS vendor: returns a clip with a known duration + a fake path, NO file
// I/O, NO network. Proves the TTS request fires + the speaking state is entered.
static AudioClip stubTts(const std::string& line, VoiceId voice) {
    AudioClip c;
    c.path = "stub://voice/";
    c.path += voiceName(voice);
    c.durationSec = 0.5f + 0.01f * (float)line.size();   // deterministic from text
    return c;
}

} // namespace

bool runDialogSelfTest() {
    int passed = 0, total = 0;
    auto check = [&](bool cond, const char* what) {
        ++total;
        if (cond) { ++passed; }
        else { x3::logError(std::string("dialog: FAIL — ") + what); }
    };

    // -- T1: tree validates + advances through nodes + a player-choice branch. --
    {
        Tree sarah = sampleSarahTree();
        check(sarah.validate(), "T1 sample Sarah tree validates");

        IDialogSystem* d = createDialogSystem();
        check(d->start(sarah), "T1 start() succeeds on a valid tree");
        check(d->active(), "T1 conversation is active after start");
        check(d->speaking(), "T1 NPC is speaking immediately after start");
        check(d->currentSpeaker() == "Sarah", "T1 speaker is Sarah");
        check(!d->currentLine().empty(), "T1 first line is non-empty");
        // No choices revealed while still speaking.
        check(d->choices().empty(), "T1 choices hidden while speaking");
        // Tick past the line to reveal the choices (no TTS -> estimated duration).
        for (int i = 0; i < 2000 && d->speaking(); ++i) d->update(0.05f);
        check(!d->speaking(), "T1 line finishes after enough updates");
        check(d->choices().size() == 3, "T1 root node offers 3 choices");
        // Branch: choice 1 ("Who are you...") -> node 120, deterministic.
        uint32_t before = d->linesDelivered();
        check(d->choose(1), "T1 choose(1) advances");
        check(d->linesDelivered() == before + 1, "T1 a new line was delivered on branch");
        check(d->speaking(), "T1 speaking again after the branch line");
        check(d->currentSpeaker() == "Sarah", "T1 branch speaker still Sarah");
        // Out-of-range / mid-speaking guards.
        check(!d->choose(99), "T1 mid-speaking choose() is ignored");
        delete d;
    }

    // -- T2: NO provider, Tree mode -> lines come from the authored tree. --
    {
        IDialogSystem* d = createDialogSystem();
        Tree t = sampleSarahTree();
        d->setMode(Mode::Tree);
        d->start(t);
        check(!d->lastLineFromProvider(), "T2 Tree mode line is authored (not provider)");
        const Node* root = t.nodeById(t.startNode);
        check(root && d->currentLine() == root->line, "T2 line equals the authored node line");
        delete d;
    }

    // -- T3: WITH a stub provider (Ai mode) -> lines come FROM the provider. --
    {
        IDialogSystem* d = createDialogSystem();
        d->setMode(Mode::Ai);
        d->setAiProvider(&stubAiProvider);
        Tree t = sampleSarahTree();
        d->start(t);
        check(d->lastLineFromProvider(), "T3 Ai mode used the provider's reply");
        check(d->currentLine().rfind("[AI:", 0) == 0, "T3 line carries the provider marker");
        delete d;
    }

    // -- T4: provider returns EMPTY -> graceful fallback to the authored tree. --
    {
        IDialogSystem* d = createDialogSystem();
        d->setMode(Mode::Ai);
        d->setAiProvider(&emptyAiProvider);
        Tree t = sampleKtharaTree();
        d->start(t);
        check(!d->lastLineFromProvider(), "T4 empty provider falls back to the tree");
        const Node* root = t.nodeById(t.startNode);
        check(root && d->currentLine() == root->line, "T4 fallback line equals the authored line");
        delete d;
    }

    // -- T5: Ai provider set but mode==Tree -> provider is NOT consulted. --
    {
        IDialogSystem* d = createDialogSystem();
        d->setMode(Mode::Tree);
        d->setAiProvider(&stubAiProvider);
        Tree t = sampleSarahTree();
        d->start(t);
        check(!d->lastLineFromProvider(), "T5 Tree mode ignores an injected provider");
        delete d;
    }

    // -- T6: stub TTS -> a clip is requested + the NPC enters/exits speaking. --
    {
        StubNpc npc;
        IDialogSystem* d = createDialogSystem();
        d->setTtsProvider(&stubTts);
        d->setSpeakingNpc(&npc);
        Tree t = sampleKtharaTree();
        d->start(t);
        check(d->lastLineHadVoice(), "T6 TTS clip was requested + accepted");
        check(npc.begins == 1, "T6 NPC entered the speaking state once");
        check(npc.speakingNow, "T6 NPC is in the speaking state during the line");
        check(npc.ends == 0, "T6 NPC has not exited speaking yet");
        // The clip duration (deterministic from text) should drive the timer.
        check(npc.lastEstDur > 0.0f, "T6 NPC got the clip's estimated duration");
        // Tick across the line -> the NPC ticks then exits.
        for (int i = 0; i < 2000 && d->speaking(); ++i) d->update(0.05f);
        check(npc.ticks > 0, "T6 NPC was ticked while speaking");
        check(npc.ends == 1, "T6 NPC exited the speaking state once");
        check(!npc.speakingNow, "T6 NPC is no longer speaking after the line");
        delete d;
    }

    // -- T7: NO TTS hook -> the line is silent text (no voice), but still spoken
    //        on the NPC + still paced by the estimated duration. --
    {
        StubNpc npc;
        IDialogSystem* d = createDialogSystem();
        d->setSpeakingNpc(&npc);   // NPC but no TTS
        Tree t = sampleSarahTree();
        d->start(t);
        check(!d->lastLineHadVoice(), "T7 no TTS hook -> the line is silent (no voice)");
        check(npc.begins == 1, "T7 NPC still enters the speaking state (subtitle pose)");
        check(!d->currentLine().empty(), "T7 subtitle text still present without voice");
        for (int i = 0; i < 2000 && d->speaking(); ++i) d->update(0.05f);
        check(npc.ends == 1, "T7 NPC exits speaking when the silent line elapses");
        delete d;
    }

    // -- T8: skip() reveals choices immediately + exits the speaking state. --
    {
        StubNpc npc;
        IDialogSystem* d = createDialogSystem();
        d->setSpeakingNpc(&npc);
        Tree t = sampleSarahTree();
        d->start(t);
        check(d->speaking(), "T8 speaking before skip");
        check(d->choices().empty(), "T8 choices hidden before skip");
        d->skip();
        check(!d->speaking(), "T8 not speaking after skip");
        check(npc.ends == 1, "T8 skip exited the speaking state");
        check(d->choices().size() == 3, "T8 choices revealed after skip");
        delete d;
    }

    // -- T9: a full branch walk to a TERMINAL node ends the conversation. --
    {
        IDialogSystem* d = createDialogSystem();
        Tree t = sampleSarahTree();
        d->start(t);
        d->skip();                 // reveal root choices
        // Root choice 2 ("Just get me out of here.") -> node 130 (terminal).
        check(d->choose(2), "T9 choose to the terminal node");
        check(d->active(), "T9 still active while the terminal line plays");
        d->skip();                 // finish the terminal line
        check(!d->active(), "T9 conversation ends after the terminal line finishes");
        check(d->choices().empty(), "T9 no choices once ended");
        check(!d->choose(0), "T9 choose() after end is a no-op");
        delete d;
    }

    // -- T10: deterministic replay — the same choices yield the same lines, and
    //         the run requires no network (only the local stubs were ever called). --
    {
        auto walk = [](IDialogSystem* d) {
            Tree t = sampleKtharaTree();
            d->start(t);
            d->skip();
            d->choose(0);          // -> 210
            d->skip();
            d->choose(0);          // -> 220
            d->skip();
            return std::string(d->currentLine());
        };
        IDialogSystem* a = createDialogSystem();
        IDialogSystem* b = createDialogSystem();
        std::string la = walk(a), lb = walk(b);
        check(!la.empty() && la == lb, "T10 identical choice path -> identical line (deterministic)");
        delete a; delete b;
    }

    // -- T11: Hybrid mode + provider -> provider rewrites, fallback intact. --
    {
        IDialogSystem* d = createDialogSystem();
        d->setMode(Mode::Hybrid);
        d->setAiProvider(&stubAiProvider);
        Tree t = sampleKtharaTree();
        d->start(t);
        check(d->mode() == Mode::Hybrid, "T11 mode is Hybrid");
        check(d->lastLineFromProvider(), "T11 Hybrid mode used the provider");
        delete d;
    }

    std::printf("dialog: %d/%d passed\n", passed, total);
    x3::logInfo("dialog: " + std::to_string(passed) + "/" + std::to_string(total) + " passed");
    return passed == total;
}

} // namespace x3::dialog
