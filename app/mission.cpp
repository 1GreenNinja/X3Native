// MISSION RUNNER — x3.mission/1 implementation. See mission.h.
#include "mission.h"
#include "level1_game.h"
#include "canon_play.h"          // --test-canonmission + pollCanonMissionFlags (--world canonlevel)
#include "level_loader.h"        // loadCanonTower / buildCanonFloor for the canon self-test
#include "headless_device.h"
#include "scene.h"
#include "timeline.h"
#include "asset_root.h"
#include "engine/core/x3_log.h"
#include "engine/physics/IPhysicsWorld.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <unordered_set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace x3::game {

// ===========================================================================
// Model lookups
// ===========================================================================

const MissionStage* MissionDoc::stageById(std::string_view id) const {
    for (const MissionStage& s : stages) if (s.id == id) return &s;
    return nullptr;
}

// ===========================================================================
// Loader
// ===========================================================================

namespace {

bool parseStage(const JValue& jv, MissionStage& out, std::vector<std::string>& errors,
                const std::string& where) {
    if (!jv.isObj()) { errors.push_back(where + ": stage is not an object"); return false; }
    bool ok = true;
    out.id = jv.find("id") ? jv.find("id")->asStr() : "";
    if (out.id.empty()) { errors.push_back(where + ": stage missing `id`"); ok = false; }
    const std::string sw = where + "." + out.id;
    out.objectiveText = jv.find("objective") ? asciiFold(jv.find("objective")->asStr()) : "";
    out.next   = jv.find("next")    ? jv.find("next")->asStr()    : "";
    out.failTo = jv.find("fail_to") ? jv.find("fail_to")->asStr() : "";
    // Optional per-stage fast-travel lock (the world map's mission gate).
    if (const JValue* nft = jv.find("no_fasttravel"))
        out.noFastTravel = (nft->t == JValue::T::Bool) ? nft->b : false;
    ok &= parseStoryFxList(jv.find("on_enter"),    out.onEnter,    errors, sw + ".on_enter");
    ok &= parseStoryFxList(jv.find("on_complete"), out.onComplete, errors, sw + ".on_complete");
    ok &= parseStoryFxList(jv.find("on_fail"),     out.onFail,     errors, sw + ".on_fail");
    ok &= parseStoryCondList(jv.find("advance_when"), out.advanceWhen, errors, sw + ".advance_when");
    ok &= parseStoryCondList(jv.find("fail_when"),    out.failWhen,    errors, sw + ".fail_when");
    if (const JValue* br = jv.find("branch")) {
        if (!br->isObj()) { errors.push_back(sw + ": `branch` is not an object"); return false; }
        out.hasBranch = true;
        ok &= parseStoryCondList(br->find("if"), out.branch.conds, errors, sw + ".branch.if");
        out.branch.thenTo = br->find("then") ? br->find("then")->asStr() : "";
        out.branch.elseTo = br->find("else") ? br->find("else")->asStr() : "";
        if (out.branch.thenTo.empty() || out.branch.elseTo.empty()) {
            errors.push_back(sw + ": `branch` wants both `then` and `else`");
            ok = false;
        }
    }
    return ok;
}

} // namespace

bool loadMissionFromJson(std::string_view jsonText, std::string_view srcName,
                         MissionDoc& out, std::vector<std::string>& errors) {
    const std::string where0 = std::string(srcName);
    JParser parser(jsonText);
    JValue root = parser.parseValue();
    if (!parser.ok || !root.isObj()) {
        errors.push_back(where0 + ": JSON parse failed");
        return false;
    }
    const std::string fmt = root.find("format") ? root.find("format")->asStr() : "";
    if (fmt != "x3.mission/1") {
        errors.push_back(where0 + ": format is `" + fmt + "` (want x3.mission/1)");
        return false;
    }
    out.id    = root.find("id")    ? root.find("id")->asStr()               : "";
    out.title = root.find("title") ? asciiFold(root.find("title")->asStr()) : "";
    out.start = root.find("start") ? root.find("start")->asStr()            : "";
    if (out.id.empty()) { errors.push_back(where0 + ": missing mission `id`"); return false; }
    const JValue* stages = root.find("stages");
    if (!stages || !stages->isArr()) {
        errors.push_back(where0 + ": missing `stages` array");
        return false;
    }
    bool ok = true;
    uint32_t si = 0;
    for (const JValue& s : *stages->arr) {
        MissionStage st;
        const std::string sw = where0 + ":stage" + std::to_string(si++);
        if (parseStage(s, st, errors, sw)) out.stages.push_back(std::move(st));
        else ok = false;
    }
    if (out.stages.empty()) { errors.push_back(where0 + ": no stages"); ok = false; }
    if (ok && out.start.empty()) out.start = out.stages.front().id;
    return ok;
}

bool loadMissionFile(const std::string& path, MissionDoc& out,
                     std::vector<std::string>& errors) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { errors.push_back(path + ": cannot open"); return false; }
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const std::string name = std::filesystem::path(path).filename().string();
    return loadMissionFromJson(src, name, out, errors);
}

std::string findMissionFile(const std::string& name) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path exe;
#ifdef _WIN32
    {
        char buf[1024];
        DWORD n = GetModuleFileNameA(nullptr, buf, (DWORD)sizeof(buf));
        exe = (n && n < sizeof(buf)) ? fs::path(std::string(buf, n)).parent_path() : fs::path(".");
    }
#else
    exe = fs::current_path();
#endif
    const fs::path rel = fs::path("missions") / name;
    const fs::path cands[] = {
        exe / ".." / ".." / ".." / rel,    // build/bin/<Config> -> repo root
        exe / rel,
        fs::path(".") / rel,
        rel,
    };
    for (const fs::path& c : cands)
        if (fs::is_regular_file(c, ec)) return c.string();
    return "";
}

bool validateMission(const MissionDoc& doc, std::vector<std::string>& errors) {
    bool ok = true;
    const std::string where = doc.id;
    // Unique, non-empty stage ids ("end" is reserved).
    for (size_t i = 0; i < doc.stages.size(); ++i) {
        if (doc.stages[i].id == "end") {
            errors.push_back(where + ": stage id `end` is reserved");
            ok = false;
        }
        for (size_t j = i + 1; j < doc.stages.size(); ++j)
            if (doc.stages[i].id == doc.stages[j].id) {
                errors.push_back(where + ": duplicate stage id `" + doc.stages[i].id + "`");
                ok = false;
            }
    }
    // Start resolves.
    if (!doc.stageById(doc.start)) {
        errors.push_back(where + ": start `" + doc.start + "` is not a stage");
        ok = false;
    }
    // Every next/branch/fail_to target resolves to a stage or "end".
    auto checkRef = [&](const std::string& ref, const std::string& at) {
        if (ref.empty() || ref == "end") return;
        if (!doc.stageById(ref)) {
            errors.push_back(where + "." + at + ": dangling stage ref `" + ref + "`");
            ok = false;
        }
    };
    for (const MissionStage& s : doc.stages) {
        checkRef(s.next, s.id + ".next");
        checkRef(s.failTo, s.id + ".fail_to");
        if (s.hasBranch) {
            checkRef(s.branch.thenTo, s.id + ".branch.then");
            checkRef(s.branch.elseTo, s.id + ".branch.else");
        }
    }
    // Reachability from start over next/branch/fail_to.
    if (doc.stageById(doc.start)) {
        std::unordered_set<std::string> seen;
        std::vector<const MissionStage*> q;
        auto push = [&](const std::string& id) {
            if (id.empty() || id == "end" || seen.count(id)) return;
            if (const MissionStage* s = doc.stageById(id)) { seen.insert(id); q.push_back(s); }
        };
        push(doc.start);
        while (!q.empty()) {
            const MissionStage* s = q.back(); q.pop_back();
            push(s->next); push(s->failTo);
            if (s->hasBranch) { push(s->branch.thenTo); push(s->branch.elseTo); }
        }
        for (const MissionStage& s : doc.stages)
            if (!seen.count(s.id)) {
                errors.push_back(where + ": stage `" + s.id + "` unreachable from start");
                ok = false;
            }
    }
    return ok;
}

// ===========================================================================
// MissionEventBridge
// ===========================================================================

void MissionEventBridge::onEvent(std::string_view name, const x3::script::EventArgs& args) {
    if (!m_flags) return;
    const std::string n(name);
    m_flags->set("ev." + n);                       // latched: the event happened
    auto arg = [&](const char* key) -> std::string {
        for (const auto& kv : args) if (kv.first == key) return kv.second;
        return "";
    };
    if (n == "trigger_enter") {
        const std::string zone = arg("zone");
        if (!zone.empty()) m_flags->set("trigger." + zone);
    } else if (n == "terminal_code") {
        const std::string code = arg("code");
        if (!code.empty()) m_flags->set("code." + code + ".entered");
    }
}

void MissionEventBridge::onKill(std::string_view type) {
    setKills(type, kills(type) + 1);
}

void MissionEventBridge::setKills(std::string_view type, int total) {
    const std::string t(type);
    int& cur = m_kills[t];
    if (total <= cur) return;                      // only raises
    if (m_flags)
        for (int n = cur + 1; n <= total; ++n)     // every newly reached milestone
            m_flags->set("kill." + t + "." + std::to_string(n));
    cur = total;
}

int MissionEventBridge::kills(std::string_view type) const {
    auto it = m_kills.find(std::string(type));
    return it == m_kills.end() ? 0 : it->second;
}

void MissionEventBridge::onRescue(std::string_view who) {
    if (m_flags) m_flags->set(lowerAscii(who) + ".rescued");
}

// ===========================================================================
// Level-1 poll adapter — Level1Game public queries -> mission flags.
// ===========================================================================

void pollLevel1MissionFlags(const Level1Game& game, MissionEventBridge& bridge,
                            StoryFlags& flags) {
    // Beat edges (latched; StoryFlags::set is idempotent).
    if (game.doorState('A') == DoorState::Open) flags.set("l1.doorA.open");
    if (game.armed())                           flags.set("l1.armed");
    if (game.armed() && game.checkpointEnemies().count() > 0 &&
        game.checkpointEnemies().aliveCount() == 0)
        flags.set("l1.checkpoint.clear");
    if (game.martinezDead())                    flags.set("l1.martinez.dead");
    if (game.complete())                        flags.set("l1.complete");

    // Trigger-zone entries this tick (the same ids the host forwards to Lua).
    for (uint32_t tid : game.lastFiredTriggers())
        flags.set("trigger." + std::to_string(tid));

    // Kill counters: dead = spawned - alive per group (monotonic in Level 1 —
    // enemies never respawn). The bridge emits "kill.<type>.<n>" milestones.
    auto deadOf = [](const MonsterManager& mm) {
        return (int)mm.count() - (int)mm.aliveCount();
    };
    bridge.setKills("hostile", deadOf(game.corridorEnemies()) +
                               deadOf(game.checkpointEnemies()));
    if (game.martinezDead()) bridge.setKills("martinez", 1);
}

// ===========================================================================
// Canon poll adapter — CanonPlay public queries -> mission flags (P1-4).
// ===========================================================================
// The rescue/clone/Sarah beats advance on the StoryFlags the canon host already
// writes (girl.freed.*, girl.extracted.*, clone.defeated, sarah.freed,
// sarah.extracted) — missions and dialog share one world. This adapter only
// bridges the beats CanonPlay owns directly but never mirrors into StoryFlags:
// leaving the cell, arming, Martinez, and the player's floor (the climb).

void pollCanonMissionFlags(const CanonPlay& play, MissionEventBridge& bridge,
                           StoryFlags& flags, int playerFloor) {
    // Beat edges (latched; StoryFlags::set is idempotent).
    if (play.leftCell())                         flags.set("canon.leftCell");
    if (play.armed())                            flags.set("canon.armed");
    if (play.martinezSpawned() && !play.martinezAlive())
                                                 flags.set("canon.martinez.dead");

    // The floor climb: the host passes the player's current floor (derived from
    // the stairwell nav chain's floorForY). Latched, so reaching floor 7 once is
    // enough for {"flag":"canon.floor.7"} — descending later never clears it.
    if (playerFloor >= 1 && playerFloor <= 9)
        flags.set("canon.floor." + std::to_string(playerFloor));
}


// ===========================================================================
// MissionRunner
// ===========================================================================

const std::string& MissionRunner::currentObjective() const {
    static const std::string kEmpty;
    return (m_state == State::Running && m_stage) ? m_stage->objectiveText : kEmpty;
}

std::string MissionRunner::condKey(const std::string& stageId) const {
    return "mission." + (m_doc ? m_doc->id : std::string()) + "." + stageId;
}

void MissionRunner::emitObjective(const std::string& text) {
    if (m_objectiveSink) m_objectiveSink(text);
}

void MissionRunner::setAtFlag(const std::string& stageId) {
    if (!m_ctx.flags || !m_doc) return;
    const std::string pre = "mission." + m_doc->id + ".at.";
    for (const MissionStage& s : m_doc->stages) m_ctx.flags->clear(pre + s.id);
    if (!stageId.empty()) m_ctx.flags->set(pre + stageId);
}

bool MissionRunner::enterStage(const std::string& id, bool fireFx) {
    if (id == "end") { completeMission(); return true; }
    const MissionStage* s = m_doc ? m_doc->stageById(id) : nullptr;
    if (!s) {
        x3::logWarn("[mission] stage `" + id + "` not in doc `" +
                    (m_doc ? m_doc->id : std::string("?")) + "`");
        return false;
    }
    m_stage = s;
    m_stageId = s->id;
    setAtFlag(s->id);
    if (!s->objectiveText.empty()) emitObjective(s->objectiveText);
    if (fireFx) applyChatFx(s->onEnter, m_ctx, m_doc->id);
    return true;
}

void MissionRunner::completeMission() {
    if (m_ctx.flags && m_doc) {
        setAtFlag("");                                 // clear the position marker
        m_ctx.flags->set("mission." + m_doc->id + ".done");
    }
    m_stage = nullptr;
    m_stageId.clear();
    m_state = State::Complete;
    emitObjective("");
    if (m_doc) x3::logInfo("[mission] COMPLETE: " + m_doc->id);
}

void MissionRunner::failMission() {
    if (m_ctx.flags && m_doc) {
        setAtFlag("");
        m_ctx.flags->set("mission." + m_doc->id + ".failed");
    }
    m_stage = nullptr;
    m_stageId.clear();
    m_state = State::Failed;
    emitObjective("");
    if (m_doc) x3::logWarn("[mission] FAILED: " + m_doc->id);
}

bool MissionRunner::start(const MissionDoc& doc) {
    m_doc = &doc;
    m_state = State::Running;
    m_stage = nullptr;
    m_stageId.clear();
    if (!enterStage(doc.start, /*fireFx*/true)) { m_state = State::Idle; return false; }
    tick();                                            // cascade already-satisfied stages
    return true;
}

bool MissionRunner::resume(const MissionDoc& doc) {
    if (m_ctx.flags) {
        if (m_ctx.flags->has("mission." + doc.id + ".done")) {
            m_doc = &doc; m_state = State::Complete; m_stage = nullptr; m_stageId.clear();
            return true;
        }
        if (m_ctx.flags->has("mission." + doc.id + ".failed")) {
            m_doc = &doc; m_state = State::Failed; m_stage = nullptr; m_stageId.clear();
            return true;
        }
        const std::string pre = "mission." + doc.id + ".at.";
        for (const MissionStage& s : doc.stages) {
            if (m_ctx.flags->has(pre + s.id)) {
                m_doc = &doc;
                m_state = State::Running;
                // Re-enter WITHOUT re-firing onEnter (the save already carries
                // its results) — but re-emit the objective text for the HUD.
                m_stage = &s;
                m_stageId = s.id;
                if (!s.objectiveText.empty()) emitObjective(s.objectiveText);
                x3::logInfo("[mission] resumed " + doc.id + " at stage `" + s.id + "`");
                tick();
                return true;
            }
        }
    }
    return start(doc);                                  // no marker — fresh start
}

void MissionRunner::tick() {
    if (m_state != State::Running || !m_doc || !m_stage) return;
    // Cascade as far as conditions allow; bounded so a doc of immediately-true
    // stages cannot spin (validateMission guarantees no id cycles of length 0,
    // but authored same-tick chains are legal and finite per pass).
    for (int hop = 0; hop < 64 && m_state == State::Running && m_stage; ++hop) {
        const MissionStage& s = *m_stage;
        // Fail first (a stage that is simultaneously failed + advanced fails —
        // fail conditions are the stricter contract).
        if (!s.failWhen.empty() && evalChatConds(s.failWhen, m_ctx, condKey(s.id))) {
            applyChatFx(s.onFail, m_ctx, m_doc->id);
            if (!s.failTo.empty()) {
                if (!enterStage(s.failTo, /*fireFx*/true)) { failMission(); return; }
                continue;
            }
            failMission();
            return;
        }
        // advance_when (AND; an EMPTY list advances immediately — fx-only stage).
        if (!evalChatConds(s.advanceWhen, m_ctx, condKey(s.id))) return;
        applyChatFx(s.onComplete, m_ctx, m_doc->id);
        std::string to = s.next;
        if (s.hasBranch)
            to = evalChatConds(s.branch.conds, m_ctx, condKey(s.id)) ? s.branch.thenTo
                                                                     : s.branch.elseTo;
        if (to.empty()) to = "end";
        if (!enterStage(to, /*fireFx*/true)) { failMission(); return; }
    }
}

// ===========================================================================
// Headless self-test (--test-mission).
// ===========================================================================

namespace {

constexpr float kDt = 1.0f / 60.0f;

x3::phys::Vec3 vsub(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
    return x3::phys::Vec3{ a.x - b.x, a.y - b.y, a.z - b.z };
}

// A tiny inline doc exercising onEnter/onComplete fx, branch, fail+failTo+Failed,
// and the kill-counter / trigger / terminal-code bridge flags.
const char* kTinyMission = R"JSON({
  "format": "x3.mission/1",
  "id": "tiny",
  "title": "Tiny Test Mission",
  "stages": [
    { "id": "s1", "objective": "Reach the marker",
      "on_enter":  [ {"set": "tiny.started"}, {"fire": "mission_stage", "args": {"stage": "s1"}} ],
      "advance_when": [ {"flag": "trigger.7"} ],
      "on_complete": [ {"set": "tiny.s1.done"} ],
      "next": "s2" },
    { "id": "s2", "objective": "Kill 3 crawlers",
      "advance_when": [ {"flag": "kill.crawler.3"} ],
      "fail_when": [ {"flag": "tiny.alarm"} ],
      "on_fail": [ {"set": "tiny.alarm.handled"}, {"clear": "tiny.alarm"} ],
      "fail_to": "s1_retry",
      "next": "gate" },
    { "id": "s1_retry", "objective": "Regroup and try again",
      "advance_when": [ {"flag": "tiny.regrouped"} ],
      "next": "s2" },
    { "id": "gate", "objective": "Enter the code",
      "advance_when": [ {"flag": "code.1278.entered"} ],
      "branch": { "if": [ {"flag": "tiny.hero"} ], "then": "hero_end", "else": "coward_end" } },
    { "id": "hero_end", "objective": "Walk out the front door",
      "on_enter": [ {"set": "tiny.path.hero"} ],
      "advance_when": [ {"flag": "tiny.exit"} ],
      "on_complete": [ {"fire": "mission_complete", "args": {"mission": "tiny"}} ],
      "next": "end" },
    { "id": "coward_end", "objective": "Sneak out the back",
      "on_enter": [ {"set": "tiny.path.coward"} ],
      "advance_when": [ {"flag": "tiny.exit"} ],
      "fail_when": [ {"flag": "tiny.spotted"} ],
      "next": "end" }
  ]
})JSON";

} // namespace

bool runMissionSelfTest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* name) {
        ++total;
        if (ok) { ++pass; x3::logInfo(std::string("  PASS M") + std::to_string(total) + " " + name); }
        else    {         x3::logWarn(std::string("  FAIL M") + std::to_string(total) + " " + name); }
        return ok;
    };

    // ---- M: parse + validate the tiny doc. ---------------------------------
    MissionDoc tiny;
    {
        std::vector<std::string> errs;
        const bool ok = loadMissionFromJson(kTinyMission, "tiny.inline", tiny, errs) &&
                        validateMission(tiny, errs);
        for (const auto& e : errs) x3::logWarn("[mission-test] " + e);
        check(ok && tiny.stages.size() == 6 && tiny.start == "s1",
              "parse: tiny inline doc loads + validates (6 stages, start=s1)");
    }

    // ---- M: loader/validator REJECT broken docs. ---------------------------
    {
        std::vector<std::string> errs;
        MissionDoc bad;
        const char* kBadKind = R"({"format":"x3.mission/1","id":"b","stages":[
            {"id":"a","advance_when":[{"frobnicate":1}],"next":"end"}]})";
        const bool kindRejected = !loadMissionFromJson(kBadKind, "bad1", bad, errs);

        errs.clear(); bad = {};
        const char* kDangling = R"({"format":"x3.mission/1","id":"b","stages":[
            {"id":"a","advance_when":[{"flag":"f"}],"next":"nowhere"}]})";
        const bool dangleRejected = loadMissionFromJson(kDangling, "bad2", bad, errs) &&
                                    !validateMission(bad, errs);

        errs.clear(); bad = {};
        const char* kDup = R"({"format":"x3.mission/1","id":"b","stages":[
            {"id":"a","advance_when":[{"flag":"f"}],"next":"a2"},
            {"id":"a2","advance_when":[{"flag":"f"}],"next":"end"},
            {"id":"a2","advance_when":[{"flag":"f"}],"next":"end"}]})";
        const bool dupRejected = loadMissionFromJson(kDup, "bad3", bad, errs) &&
                                 !validateMission(bad, errs);

        errs.clear(); bad = {};
        const char* kUnreach = R"({"format":"x3.mission/1","id":"b","stages":[
            {"id":"a","advance_when":[{"flag":"f"}],"next":"end"},
            {"id":"orphan","advance_when":[{"flag":"f"}],"next":"end"}]})";
        const bool orphanRejected = loadMissionFromJson(kUnreach, "bad4", bad, errs) &&
                                    !validateMission(bad, errs);

        check(kindRejected && dangleRejected && dupRejected && orphanRejected,
              "validate: unknown op / dangling ref / duplicate id / unreachable stage all rejected");
    }

    // ---- Context: flags + a REAL Lua script observing fired fx. ------------
    StoryFlags flags;
    std::unique_ptr<x3::script::IScriptSystem> scripts(
        x3::script::createLuaScriptSystem(nullptr));
    std::vector<std::string> marks;            // events observed BY the script
    scripts->registerFunction("mark",
        [&](const std::vector<x3::script::ScriptValue>& a) -> x3::script::ScriptValue {
            if (!a.empty()) marks.push_back(a[0].asString());
            return {};
        });
    const char* kObserverLua =
        "function onEvent(name, args)\n"
        "  if name == 'mission_stage' then x3.mark('stage:' .. args.stage) end\n"
        "  if name == 'mission_complete' then x3.mark('complete:' .. args.mission) end\n"
        "end\n";
    const auto sid = scripts->load("mission_test.lua", kObserverLua);
    check(sid != x3::script::kInvalidScript && !scripts->status(sid).failed,
          "lua: observer script loads");

    MissionEventBridge bridge;
    bridge.bind(&flags);

    MissionRunner runner;
    runner.ctx().flags   = &flags;
    runner.ctx().scripts = scripts.get();
    std::vector<std::string> objSeq;           // every objective text emitted
    runner.setObjectiveSink([&](const std::string& t) { objSeq.push_back(t); });

    // ---- M: start -> s1; onEnter fx fired (flag + x3.fire seen by Lua). ----
    check(runner.start(tiny) && runner.currentStageId() == "s1" &&
          runner.state() == MissionRunner::State::Running,
          "run: start lands s1 (Running)");
    check(flags.has("tiny.started") &&
          marks.size() == 1 && marks[0] == "stage:s1",
          "fx: s1 on_enter set flag + fired mission_stage (observed by Lua)");
    check(!objSeq.empty() && objSeq.back() == "Reach the marker",
          "objective: s1 text emitted to the sink");

    // ---- M: no flags -> no advance; trigger bridge -> s2. -------------------
    runner.tick();
    check(runner.currentStageId() == "s1", "advance: holds s1 until the trigger fires");
    bridge.onEvent("trigger_enter", {{"zone", "7"}, {"who", "player"}});
    check(flags.has("trigger.7") && flags.has("ev.trigger_enter"),
          "bridge: trigger_enter -> trigger.7 + ev.trigger_enter flags");
    runner.tick();
    check(runner.currentStageId() == "s2" && flags.has("tiny.s1.done") &&
          objSeq.back() == "Kill 3 crawlers",
          "advance: trigger flag advances s1 -> s2 (on_complete fx fired)");

    // ---- M: fail_when -> on_fail + fail_to retry loop. ----------------------
    flags.set("tiny.alarm");
    runner.tick();
    check(runner.currentStageId() == "s1_retry" && flags.has("tiny.alarm.handled") &&
          !flags.has("tiny.alarm") && runner.state() == MissionRunner::State::Running,
          "fail: fail_when -> on_fail fx + fail_to retry stage (still Running)");
    flags.set("tiny.regrouped");
    runner.tick();
    check(runner.currentStageId() == "s2", "retry: regroup advances back to s2");

    // ---- M: kill-counter bridge milestones advance s2. ----------------------
    bridge.onKill("crawler");
    runner.tick();
    check(runner.currentStageId() == "s2" && flags.has("kill.crawler.1") &&
          !flags.has("kill.crawler.3"),
          "kills: 1 kill sets kill.crawler.1 only (no advance)");
    bridge.onKill("crawler");
    bridge.onKill("crawler");
    runner.tick();
    check(flags.has("kill.crawler.3") && runner.currentStageId() == "gate",
          "kills: 3rd kill sets the milestone -> s2 advances to gate");

    // ---- M: terminal-code bridge + branch else-path. -------------------------
    bridge.onEvent("terminal_code", {{"code", "1278"}});
    runner.tick();
    check(flags.has("code.1278.entered") && runner.currentStageId() == "coward_end" &&
          flags.has("tiny.path.coward"),
          "branch: code flag advances gate; no tiny.hero -> else path (coward_end)");

    // ---- M: SAVE mid-mission -> flags round-trip -> RESUME same stage. ------
    {
        const std::string blob = flags.serialize();
        check(flags.has("mission.tiny.at.coward_end"),
              "persist: position marker mission.tiny.at.coward_end is in the flags");

        StoryFlags flags2;
        check(flags2.deserialize(blob), "persist: flags blob round-trips");

        MissionRunner runner2;
        runner2.ctx().flags   = &flags2;
        runner2.ctx().scripts = scripts.get();
        std::vector<std::string> objSeq2;
        runner2.setObjectiveSink([&](const std::string& t) { objSeq2.push_back(t); });
        const size_t marksBefore = marks.size();
        check(runner2.resume(tiny) && runner2.currentStageId() == "coward_end" &&
              runner2.state() == MissionRunner::State::Running &&
              !objSeq2.empty() && objSeq2.back() == "Sneak out the back",
              "resume: lands the saved stage with the right objective text");
        check(marks.size() == marksBefore,
              "resume: on_enter fx NOT re-fired on resume");

        // Finish the resumed run.
        flags2.set("tiny.exit");
        runner2.tick();
        check(runner2.state() == MissionRunner::State::Complete &&
              flags2.has("mission.tiny.done") && objSeq2.back().empty(),
              "resume: resumed mission completes (done flag + objective cleared)");
        // A later resume of a done mission stays Complete (no restart).
        MissionRunner runner3;
        runner3.ctx().flags = &flags2;
        check(runner3.resume(tiny) && runner3.state() == MissionRunner::State::Complete,
              "resume: a done mission resumes straight to Complete");
    }

    // ---- M: the hero branch + hard fail (no fail_to) on a fresh run. --------
    {
        StoryFlags f3;
        MissionRunner r3;
        r3.ctx().flags = &f3;
        f3.set("tiny.hero");
        f3.set("trigger.7");
        f3.set("kill.crawler.3");                  // pre-satisfied: cascade test
        f3.set("code.1278.entered");
        check(r3.start(tiny) && r3.currentStageId() == "hero_end" &&
              f3.has("tiny.path.hero"),
              "cascade: pre-satisfied stages chain s1->s2->gate->hero_end in one start");
        // coward_end's hard fail (fail_when with NO fail_to) -> Failed.
        StoryFlags f4;
        MissionRunner r4;
        r4.ctx().flags = &f4;
        f4.set("trigger.7"); f4.set("kill.crawler.3"); f4.set("code.1278.entered");
        r4.start(tiny);                            // lands coward_end (no hero flag)
        f4.set("tiny.spotted");
        r4.tick();
        check(r4.state() == MissionRunner::State::Failed &&
              f4.has("mission.tiny.failed"),
              "fail: fail_when with no fail_to -> mission Failed (+failed flag)");
    }

    // ---- M: missions/level1.mission.json parses + validates from disk. ------
    MissionDoc l1doc;
    {
        const std::string path = findMissionFile("level1.mission.json");
        std::vector<std::string> errs;
        const bool ok = !path.empty() && loadMissionFile(path, l1doc, errs) &&
                        validateMission(l1doc, errs);
        for (const auto& e : errs) x3::logWarn("[mission-test] " + e);
        check(ok && l1doc.id == "level1" && l1doc.stages.size() == 5,
              "level1: missions/level1.mission.json loads + validates (5 stages)");
    }

    // =========================================================================
    // THE LEVEL-1 EQUIVALENCE WALK: the doc-driven runner vs the hardcoded
    // ObjectiveSystem beat list, on the REAL Level1Game, same scripted walk the
    // --test-level1 suite drives. The doc must produce the SAME objective text
    // sequence the hardcoded path walks.
    // =========================================================================
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
        physics->init();
        HeadlessRenderDevice device;
        Scene scene;
        Level1Game game;
        game.setDevice(device);
        game.build(scene, device, *physics, riggedGlbRoot());
        const Level1Layout& L = game.layout();

        StoryFlags l1flags;
        MissionEventBridge l1bridge;
        l1bridge.bind(&l1flags);
        MissionRunner l1run;
        l1run.ctx().flags = &l1flags;

        // The DOC-DRIVEN objective sequence (what the runner emits)...
        std::vector<std::string> docSeq;
        l1run.setObjectiveSink([&](const std::string& t) { docSeq.push_back(t); });
        // ...vs the HARDCODED sequence (the live ObjectiveSystem list lane),
        // sampled on every change.
        std::vector<std::string> hardSeq;
        auto sampleHard = [&]() {
            const std::string& cur = game.objectives().currentLabel();
            if (hardSeq.empty() || hardSeq.back() != cur) hardSeq.push_back(cur);
        };
        sampleHard();                                  // the spawn objective
        check(l1run.start(l1doc), "level1: mission doc starts");

        auto step = [&](const x3::phys::Vec3& pos, int frames) {
            for (int i = 0; i < frames; ++i) {
                game.tick(kDt, scene, *physics, pos, pos);
                physics->step(kDt);
                scene.update(*physics);
                pollLevel1MissionFlags(game, l1bridge, l1flags);
                l1run.tick();
                sampleHard();
            }
        };
        auto aim = [&](const x3::phys::Vec3& eye, const x3::phys::Vec3& tgt) {
            return vsub(tgt, eye);
        };

        // Beat: use Door A's button; let the door animate open (alarm spawns).
        {
            x3::phys::Vec3 btn{ L.doorA.x - 0.10f - 0.12f, 1.3f, L.doorA.z + 0.6f + 0.5f };
            x3::phys::Vec3 eye{ btn.x - 1.5f, btn.y, btn.z };
            game.onUse(eye, aim(eye, btn), scene, *physics);
            step(x3::phys::Vec3{ L.spawn.x, 0.05f, 0.0f }, 80);
        }
        // Beat: grab the cell sidearm -> armed.
        step(x3::phys::Vec3{ L.cellCenter.x, 0.05f, L.cellCenter.z }, 6);
        // Fight: melee down the corridor alarm enemies (mirrors --test-level1).
        for (int g = 0; g < 200 && game.corridorEnemies().aliveCount() > 0; ++g) {
            MonsterManager& co = game.corridorEnemies();
            for (uint32_t i = 0; i < co.count(); ++i) {
                if (co.at(i).alive()) {
                    const Entity& e = scene.get(co.at(i).entity());
                    x3::phys::Vec3 t{ e.transform[12], e.transform[13], e.transform[14] };
                    x3::phys::Vec3 ey{ t.x - 1.0f, 0.6f, t.z };
                    game.onMelee(ey, aim(ey, t), scene, *physics);
                    break;
                }
            }
            step(x3::phys::Vec3{ L.corridorCenter.x - 4.0f, 0.05f, L.corridorCenter.z }, 2);
        }
        // Walk to the armory (Door C opened by the armed beat).
        step(x3::phys::Vec3{ L.armoryCenter.x, 0.05f, L.armoryCenter.z }, 80);
        // Fight: shoot the checkpoint enemies dead.
        {
            x3::phys::Vec3 eye{ L.checkpointCenter.x - 0.9f, 0.6f, L.checkpointCenter.z };
            for (int g = 0; g < 100 && game.checkpointEnemies().aliveCount() > 0; ++g) {
                MonsterManager& cp = game.checkpointEnemies();
                for (uint32_t i = 0; i < cp.count(); ++i) {
                    if (cp.at(i).alive()) {
                        const Entity& e = scene.get(cp.at(i).entity());
                        x3::phys::Vec3 t{ e.transform[12], e.transform[13], e.transform[14] };
                        game.onFire(eye, aim(eye, t), scene, *physics);
                        break;
                    }
                }
                step(eye, 2);
            }
        }
        // Beat: cross the arena trigger -> Martinez spawns; shoot him dead.
        step(x3::phys::Vec3{ L.doorD.x + 2.0f, 0.05f, 0.0f }, 80);
        {
            x3::phys::Vec3 tgt{ L.arenaCenter.x, 0.6f, L.arenaCenter.z };
            x3::phys::Vec3 eye{ L.arenaCenter.x - 4.0f, 0.6f, L.arenaCenter.z };
            for (int i = 0; i < 30 && game.martinezAlive(); ++i) {
                game.onFire(eye, aim(eye, tgt), scene, *physics);
                step(eye, 2);
            }
            step(eye, 80);                             // Door E animates open
        }
        // Beat: ride out — the elevator trigger (armed by the boss death) -> WIN.
        step(x3::phys::Vec3{ L.elevatorCenter.x, 0.05f, 0.0f }, 6);

        check(game.complete(), "level1: the scripted walk completes the level");
        check(l1run.state() == MissionRunner::State::Complete,
              "level1: the mission doc completes on the same walk");
        check(l1flags.has("mission.level1.done") && l1flags.has("l1.win.recorded"),
              "level1: done + reward flags recorded");

        // THE GATE: identical objective sequences. The hardcoded lane ends on ""
        // (list finished); the doc lane ends on "" (mission complete) — compare
        // the full sequences element-by-element.
        bool same = docSeq.size() == hardSeq.size();
        if (same)
            for (size_t i = 0; i < docSeq.size(); ++i)
                if (docSeq[i] != hardSeq[i]) { same = false; break; }
        if (!same) {
            std::ostringstream os;
            os << "[mission-test] sequence mismatch:\n  doc :";
            for (const auto& s : docSeq)  os << " | " << (s.empty() ? "<end>" : s);
            os << "\n  hard:";
            for (const auto& s : hardSeq) os << " | " << (s.empty() ? "<end>" : s);
            x3::logWarn(os.str());
        }
        check(same && docSeq.size() == 6,
              "level1 EQUIVALENCE: doc-driven objective sequence == hardcoded beat list (5 beats + end)");

        game.shutdown();                               // ragdolls before the world dies
        physics->shutdown();
    }

    x3::logInfo("mission: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    return pass == total;
}

// ===========================================================================
// --test-canonmission (P1-4): the canon mission spine, headless.
// ===========================================================================

bool runCanonMissionSelfTest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* name) {
        ++total;
        if (ok) { ++pass; x3::logInfo(std::string("  PASS C") + std::to_string(total) + " " + name); }
        else    {         x3::logWarn(std::string("  FAIL C") + std::to_string(total) + " " + name); }
        return ok;
    };

    // ---- C1: missions/canon_act1.mission.json parses + validates from disk. ----
    MissionDoc doc;
    {
        const std::string path = findMissionFile("canon_act1.mission.json");
        std::vector<std::string> errs;
        const bool ok = !path.empty() && loadMissionFile(path, doc, errs) &&
                        validateMission(doc, errs);
        for (const auto& e : errs) x3::logWarn("[canonmission-test] " + e);
        check(ok && doc.id == "canon_act1" && doc.stages.size() == 9 &&
              doc.start == "wake",
              "canon_act1.mission.json loads + validates (9 stages, start=wake)");
    }

    // ---- The full beat walk: the objective string is present and correct at
    // every beat, cell -> helipad, driven purely by the flag bridge. ----
    StoryFlags flags;
    MissionRunner runner;
    runner.ctx().flags = &flags;
    std::vector<std::string> objSeq;
    runner.setObjectiveSink([&](const std::string& t) { objSeq.push_back(t); });

    check(runner.start(doc) && runner.currentStageId() == "wake" &&
          runner.state() == MissionRunner::State::Running &&
          !objSeq.empty() && objSeq.back() == "Escape the detention cell",
          "start: lands wake with the cell objective");
    runner.tick();
    check(runner.currentStageId() == "wake" && flags.has("canon.mission.begun"),
          "negative control: no flags -> holds wake (objective never blanks mid-game)");

    flags.set("canon.leftCell");
    runner.tick();
    check(runner.currentStageId() == "arm" && objSeq.back() == "Find a weapon",
          "beat wake -> arm on canon.leftCell");
    flags.set("canon.armed");
    runner.tick();
    check(runner.currentStageId() == "martinez" && objSeq.back() == "Defeat Chief Martinez",
          "beat arm -> martinez on canon.armed");
    flags.set("canon.martinez.dead");
    runner.tick();
    check(runner.currentStageId() == "triage" && objSeq.back() == "Rescue the girls in the Medical Bay",
          "beat martinez -> triage on canon.martinez.dead");
    flags.set("girl.freed.keisha");
    runner.tick();
    check(runner.currentStageId() == "climb" && objSeq.back() == "Climb the tower to Floor 7",
          "beat triage -> climb on girl.freed.keisha (any-of)");
    flags.set("canon.floor.7");
    runner.tick();
    check(runner.currentStageId() == "clone" && objSeq.back() == "Defeat Jake's Clone on Floor 7",
          "beat climb -> clone on canon.floor.7");
    flags.set("clone.defeated");
    runner.tick();
    check(runner.currentStageId() == "sarah" && objSeq.back() == "Free Sarah from the containment field",
          "beat clone -> sarah on clone.defeated");
    flags.set("sarah.freed");
    runner.tick();
    check(runner.currentStageId() == "helipad" && objSeq.back() == "Get Sarah to the Helipad",
          "beat sarah -> helipad on sarah.freed");
    flags.set("sarah.extracted");
    runner.tick();
    check(runner.currentStageId() == "outro" && objSeq.back() == "TO BE CONTINUED",
          "beat helipad -> outro (TO BE CONTINUED) on sarah.extracted");
    // Negative control: the outro holds until the Act-2 handoff (Phase 5) — the
    // objective stays present through the win card, exactly like today.
    runner.tick();
    check(runner.currentStageId() == "outro" && runner.state() == MissionRunner::State::Running,
          "outro holds: no act2.handoff -> still Running with 'TO BE CONTINUED'");
    flags.set("act2.handoff");
    runner.tick();
    check(runner.state() == MissionRunner::State::Complete &&
          flags.has("mission.canon_act1.done") && objSeq.back().empty(),
          "outro -> Complete on act2.handoff (done flag + objective cleared)");

    // Continuity proof: exactly the 9 stage texts then the terminal empty — no
    // mid-game gap anywhere in the sequence.
    check(objSeq.size() == 10,
          "objective sequence is the 9 beats + the terminal empty (continuous cell->helipad->outro)");

    // ---- The escaped-path cascade: intro.outcome=escaped + already armed skips
    // the cell/arm beats and lands straight on Martinez (no blank objective). ----
    {
        StoryFlags f2;
        f2.set("intro.outcome=escaped");
        f2.set("canon.armed");
        MissionRunner r2;
        r2.ctx().flags = &f2;
        std::vector<std::string> seq2;
        r2.setObjectiveSink([&](const std::string& t) { seq2.push_back(t); });
        check(r2.start(doc) && r2.currentStageId() == "martinez" &&
              !seq2.empty() && seq2.back() == "Defeat Chief Martinez",
              "escaped cascade: outcome=escaped + armed -> straight to Martinez (cell/arm skipped)");
    }

    // ---- pollCanonMissionFlags on a REAL CanonPlay (canonical JSON permitting). ----
    {
        CanonFloor floor = loadCanonTower(canonProjectJsonPath());
        if (!floor.valid()) {
            x3::logInfo("  SKIP canonical tower JSON absent — poll-adapter section PASS (skipped)");
        } else {
            HeadlessRenderDevice device;
            std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
            physics->init();
            Scene scene;
            buildCanonFloor(floor, scene, device, *physics);
            CanonPlay play;
            play.build(floor, scene, device, *physics, riggedGlbRoot(), canonGirlsDialogPath());

            StoryFlags pf;
            MissionEventBridge pb;
            pb.bind(&pf);

            // Fresh build: unarmed, still in the cell -> no canon.* flags.
            pollCanonMissionFlags(play, pb, pf, 1);
            check(!pf.has("canon.leftCell") && !pf.has("canon.armed"),
                  "poll: fresh CanonPlay (in-cell, unarmed) sets no canon.* flags");
            // Arm -> canon.armed.
            play.cheatArm(scene);
            pollCanonMissionFlags(play, pb, pf, 1);
            check(pf.has("canon.armed"), "poll: cheatArm -> canon.armed");
            // Martinez alive -> not dead.
            check(!pf.has("canon.martinez.dead"), "poll: Martinez alive -> canon.martinez.dead unset");
            // Floor bridge.
            pollCanonMissionFlags(play, pb, pf, 3);
            check(pf.has("canon.floor.3"), "poll: playerFloor 3 -> canon.floor.3");
            pollCanonMissionFlags(play, pb, pf, 7);
            check(pf.has("canon.floor.7"), "poll: playerFloor 7 -> canon.floor.7");

            play.shutdown();
            physics->shutdown();
        }
    }

    x3::logInfo("canonmission: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    return pass == total;
}

} // namespace x3::game
