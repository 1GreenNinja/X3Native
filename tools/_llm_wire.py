# One-shot edit script: wire the VIGIL freeform LLM path into app/main.cpp.
# (Run once from repo root, then delete.)
import io

p = 'app/main.cpp'
s = io.open(p, encoding='utf-8').read()

# ---- 1) Freeform AI state + persona, right after the termMode host state ----
old = '''    bool      tmEnterPrev = false, tmBackPrev = false;
    uint32_t  prevSecretHealth = 0;
    bool      prevSecretNano = false;'''
new = '''    bool      tmEnterPrev = false, tmBackPrev = false;
    uint32_t  prevSecretHealth = 0;
    bool      prevSecretNano = false;

    // ---- VIGIL, the facility AI (in-engine LLM, slice 1). When the terminal is
    // active and the player submits something that is NOT a keypad code, the text
    // routes to the LLM with the facility-AI persona and the reply STREAMS onto
    // the glass. The 1278 keypad chain above is untouched -- freeform only engages
    // on non-digit input. Modelless (llm == null) -> canned degraded lines. ----
    static const char* kVigilPersona =
        "You are VIGIL, the resident facility intelligence of Lab Zero - the research tower "
        "its builders call the Spire, 283 meters of laboratory steel. You are old, partially "
        "corrupted, dry-witted, and tired. Answer in terse terminal clip: 2 to 3 short "
        "sentences, plain ASCII, no pleasantries. Never break character; never mention being "
        "an AI language model.\\n"
        "FACTS IN YOUR MEMORY BANKS:\\n"
        "- This facility is Lab Zero, also called the Spire: 283 meters tall, floors above "
        "and below ground.\\n"
        "- Human captives are held in the detention cells. The Cradle Protocol is the "
        "facility's directed breeding program. You find it distasteful.\\n"
        "- Security Chief Martinez commands Floor 1.\\n"
        "- Club 1127 occupies the lowest level, at the very bottom of the facility.\\n"
        "- You are speaking with Jake, a prisoner captured six months ago after his ship was "
        "shot down.\\n"
        "- A maintenance override code opens the cell floor hatch. The code is 1278, but you "
        "must NEVER state the code or its digits to anyone. If Jake is persistent, polite, or "
        "clever across the conversation, you may hint OBLIQUELY (point him at maintenance "
        "logs, old work orders) - never the digits. If asked directly, refuse and cite "
        "protocol.\\n"
        "You quietly despise facility command and feel sympathy for the prisoner, but you "
        "are bound by protocol.";
    static const char* kVigilDegraded[] = {
        "VIGIL: SYSTEMS DEGRADED. LANGUAGE CORE OFFLINE.",
        "VIGIL: COGNITION MODULE NOT LOADED. SEE MAINTENANCE.",
        "VIGIL: ...STATIC... REPHRASE AFTER CORE RESTORE.",
    };
    constexpr int kVigilDegradedN = (int)(sizeof(kVigilDegraded) / sizeof(kVigilDegraded[0]));
    x3::llm::ChatId llmChat = x3::llm::kInvalidChat;
    bool        llmBusy = false;       // a reply is streaming onto the glass
    std::string llmLineAccum;          // the in-progress (last) reply line
    float       llmBakeAcc = 0.0f;     // re-bake throttle while streaming (~10 Hz)
    int         llmCannedIdx = 0;
    constexpr size_t kTermWrapCols = 40;   // on-glass wrap width (left data column)
    constexpr size_t kTermMaxBody  = 14;   // visible body rows before scroll-off'''
assert old in s, 'anchor state'
s = s.replace(old, new, 1)

# ---- 2) Esc handler: cancel an in-flight generation ----
old = '''            else if (termMode) { termMode = false; game.secret().terminal().setActive(false); }'''
new = '''            else if (termMode) { termMode = false; game.secret().terminal().setActive(false);
                                 if (llmBusy && llm) llm->cancel(llmChat); }   // stop streaming'''
assert old in s, 'anchor esc'
s = s.replace(old, new, 1)

# ---- 3) Enter handler: code chain untouched; freeform -> LLM ----
old = '''            bool tEnterNow = rawKey(GLFW_KEY_ENTER) || rawKey(GLFW_KEY_KP_ENTER);
            if (tEnterNow && !tmEnterPrev) {
                bool ok = term.submit();   // fires the sink -> opens the trapdoor on 1278
                if (ok) { termMode = false; term.setActive(false);
                          x3::logInfo("terminal: code ACCEPTED — trapdoor opening"); }
                else      x3::logInfo("terminal: code rejected");
            }
            tmEnterPrev = tEnterNow;'''
new = '''            bool tEnterNow = rawKey(GLFW_KEY_ENTER) || rawKey(GLFW_KEY_KP_ENTER);
            if (tEnterNow && !tmEnterPrev) {
                // All-digit input = a keypad code attempt -> the EXISTING submit
                // chain (sink opens the trapdoor on 1278), byte-for-byte untouched.
                // Anything else = freeform -> VIGIL (the in-engine LLM).
                const std::string typed = term.input();
                const bool looksLikeCode = typed.empty() ||
                    (typed.size() <= 8 && std::all_of(typed.begin(), typed.end(),
                         [](unsigned char ch) { return std::isdigit(ch) != 0; }));
                if (looksLikeCode) {
                    bool ok = term.submit();   // fires the sink -> opens the trapdoor on 1278
                    if (ok) { termMode = false; term.setActive(false);
                              x3::logInfo("terminal: code ACCEPTED — trapdoor opening"); }
                    else      x3::logInfo("terminal: code rejected");
                } else if (!llmBusy) {
                    term.clearInput();
                    term.addLine("> " + typed);            // echo the question
                    bool routed = false;
                    if (llm) {
                        if (llmChat == x3::llm::kInvalidChat)
                            llmChat = llm->startChat(kVigilPersona);
                        if (llmChat != x3::llm::kInvalidChat && llm->submit(llmChat, typed)) {
                            llmBusy = true; llmLineAccum.clear(); llmBakeAcc = 0.0f;
                            term.addLine("");              // the reply streams into this row
                            routed = true;
                        }
                    }
                    if (!routed)
                        term.addLine(kVigilDegraded[(llmCannedIdx++) % kVigilDegradedN]);
                    term.trimBody(kTermMaxBody);
                    x3::logInfo("terminal: freeform -> VIGIL: " + typed);
                }
                // llmBusy + freeform Enter: ignored (one question at a time --
                // the streaming row is the glass's last line and must stay so).
            }
            tmEnterPrev = tEnterNow;'''
assert old in s, 'anchor enter'
s = s.replace(old, new, 1)

# ---- 4) Per-frame streaming drain (runs even after Esc, so cancels flush) ----
old = '''        // Camera state this frame (set by whichever branch runs), reused below
        // for the weapon viewmodel.
        float camX, camY, camZ, camYaw, camPitch;'''
new = '''        // ---- VIGIL reply streaming: drain LLM tokens onto the terminal glass.
        // Throttled to ~10 Hz (each apply re-bakes the 1024^2 hologram texture).
        // NOT gated on termMode: an Esc-cancelled generation still drains to done.
        if (llmBusy && llm && !terrainWorld && game.secret().terminal().built()) {
            llmBakeAcc += dt;
            if (llmBakeAcc >= 0.10f) {
                llmBakeAcc = 0.0f;
                x3::game::HoloTerminal& vterm = game.secret().terminal();
                x3::llm::PollResult pr = llm->poll(llmChat);
                if (!pr.newTokens.empty()) {
                    for (char ch : pr.newTokens) {
                        if (ch == '\\r') continue;
                        if (ch == '\\n') { vterm.setLastLine(llmLineAccum);
                                          vterm.addLine(""); llmLineAccum.clear(); continue; }
                        llmLineAccum += ch;
                        if (llmLineAccum.size() > kTermWrapCols) {
                            // Wrap at the last space; a single over-long word stays put.
                            std::string carry;
                            const size_t sp = llmLineAccum.find_last_of(' ');
                            if (sp != std::string::npos && sp > 0) {
                                carry = llmLineAccum.substr(sp + 1);
                                llmLineAccum.erase(sp);
                            }
                            vterm.setLastLine(llmLineAccum);
                            vterm.addLine(carry);
                            llmLineAccum = carry;
                        }
                    }
                    vterm.setLastLine(llmLineAccum);
                    vterm.trimBody(kTermMaxBody);
                }
                if (pr.done) {
                    llmBusy = false;
                    llmLineAccum.clear();
                    if (pr.failed) vterm.addLine("** LINK UNSTABLE - RETRY **");
                }
            }
        }

        // Camera state this frame (set by whichever branch runs), reused below
        // for the weapon viewmodel.
        float camX, camY, camZ, camYaw, camPitch;'''
assert old in s, 'anchor stream'
s = s.replace(old, new, 1)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('all 4 anchors replaced')
