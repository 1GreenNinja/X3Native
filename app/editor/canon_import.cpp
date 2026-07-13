// Open the real game level in the editor — canon project.json -> LevelDoc.
// See canon_import.h for the scope contract. Pure logic.
#include "canon_import.h"
#include "../json_mini.h"
#include "../asset_root.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace jm = x3::game::jmini;

namespace x3::editor {

std::string canonLevelPath() {
    // Resolve through assetRoot(), exactly like the game (level_loader.cpp). NEVER a
    // hardcoded absolute path in a fallback chain — that is the L2 landmine, a baked-in
    // C:\...\OneDrive path that existed on one machine and silently overrode the repo.
    return x3::game::assetRoot() + "/levels/EscapeLab48_AllFloors_v2.project.json";
}

namespace {
// Numeric-aware sort of floor keys "1".."10" so 10 does not sort before 2.
bool floorKeyLess(const CanonFloorInfo& a, const CanonFloorInfo& b) {
    const long ia = std::strtol(a.key.c_str(), nullptr, 10);
    const long ib = std::strtol(b.key.c_str(), nullptr, 10);
    if (ia != ib) return ia < ib;
    return a.key < b.key;
}
} // namespace

CanonProject openCanonProjectFromString(const std::string& json) {
    CanonProject p;
    p.rawJson = json;

    jm::JReader rd(json);
    jm::JVal doc = rd.parse();
    if (!rd.ok || doc.t != jm::JVal::Obj) {
        p.error = "not a valid project JSON";
        return p;
    }
    p.name = doc.sval("name", "Canon Level");

    const jm::JVal* floors = doc.get("floors");
    if (!floors || floors->t != jm::JVal::Obj) {
        p.error = "project has no floors{} object";
        return p;
    }
    for (const auto& kv : floors->obj) {
        const jm::JVal& f = kv.second;
        if (f.t != jm::JVal::Obj) continue;
        CanonFloorInfo fi;
        fi.key  = kv.first;
        fi.name = f.sval("name", "Floor " + kv.first);
        const jm::JVal* rooms = f.get("rooms");
        const jm::JVal* doors = f.get("doors");
        fi.rooms = (rooms && rooms->t == jm::JVal::Arr) ? (int)rooms->arr.size() : 0;
        fi.doors = (doors && doors->t == jm::JVal::Arr) ? (int)doors->arr.size() : 0;
        p.floors.push_back(std::move(fi));
    }
    std::sort(p.floors.begin(), p.floors.end(), floorKeyLess);
    p.ok = !p.floors.empty();
    if (!p.ok && p.error.empty()) p.error = "project has no floors";
    return p;
}

CanonProject openCanonProject(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) {
        CanonProject p;
        p.error = "no canon level at " + path;
        return p;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    CanonProject p = openCanonProjectFromString(ss.str());
    if (p.ok)
        x3::logInfo("[canon] " + p.name + " — " + std::to_string(p.floors.size()) +
                    " floors (" + path + ")");
    else
        x3::logWarn("[canon] " + p.error);
    return p;
}

void roomTypeTint(const std::string& type, float outRgb[3]) {
    // A small, stable palette by room TYPE so an imported floor reads structurally
    // instead of as 53 identical grey boxes. Values are muted (this is a blockout, not
    // final art) but distinct enough to tell a cell from a lab from a hall at a glance.
    struct TT { const char* key; float r, g, b; };
    static const TT kMap[] = {
        { "Jake Cell",      0.80f, 0.55f, 0.30f },  // the start — warm, stands out
        { "Cell",           0.55f, 0.42f, 0.30f },
        { "Holding Cell",   0.55f, 0.42f, 0.30f },
        { "Hallway",        0.62f, 0.64f, 0.68f },  // circulation — neutral bright
        { "Connector Hall", 0.58f, 0.60f, 0.64f },
        { "Elevator Lobby", 0.50f, 0.58f, 0.70f },
        { "Stairway",       0.50f, 0.58f, 0.70f },
        { "Lab",            0.40f, 0.66f, 0.66f },  // teal-clinical
        { "Medical Bay",    0.45f, 0.70f, 0.62f },
        { "Security",       0.70f, 0.40f, 0.40f },  // red — hostile spaces
        { "Armory",         0.72f, 0.45f, 0.38f },
        { "Boss Arena",     0.75f, 0.32f, 0.34f },
        { "Server Room",    0.40f, 0.52f, 0.72f },
        { "Office",         0.66f, 0.62f, 0.50f },
        { "Storage",        0.55f, 0.55f, 0.50f },
        { "Archive",        0.60f, 0.56f, 0.46f },
        { "Observation",    0.52f, 0.62f, 0.70f },
        { "Cave",           0.42f, 0.40f, 0.34f },  // underground — dim earthy
        { "Cave Chamber",   0.44f, 0.42f, 0.36f },
        { "Underground",    0.42f, 0.40f, 0.34f },
    };
    for (const TT& t : kMap) {
        if (type == t.key) { outRgb[0] = t.r; outRgb[1] = t.g; outRgb[2] = t.b; return; }
    }
    outRgb[0] = 0.60f; outRgb[1] = 0.60f; outRgb[2] = 0.63f;   // default machined grey
}

bool importCanonFloor(const CanonProject& proj, const std::string& floorKey, LevelDoc& out) {
    // Re-parse the raw JSON to reach the chosen floor's rooms. (openCanonProject kept
    // only the floor SUMMARY; the geometry is pulled on demand, one floor at a time.)
    jm::JReader rd(proj.rawJson);
    jm::JVal doc = rd.parse();
    if (!rd.ok || doc.t != jm::JVal::Obj) return false;
    const jm::JVal* floors = doc.get("floors");
    if (!floors || floors->t != jm::JVal::Obj) return false;
    const jm::JVal* floor = floors->get(floorKey.c_str());
    if (!floor || floor->t != jm::JVal::Obj) return false;   // unknown floor: leave out untouched

    LevelDoc doc2;                       // build aside; only commit on success
    std::string fname = floor->sval("name", "Floor " + floorKey);
    doc2.name  = proj.name + " — " + fname;
    doc2.biome = "facility";

    const jm::JVal* rooms = floor->get("rooms");
    if (rooms && rooms->t == jm::JVal::Arr) {
        for (const jm::JVal& r : rooms->arr) {
            if (r.t != jm::JVal::Obj) continue;
            const std::string rn = r.sval("n", "room");
            const std::string rt = r.sval("t", "");
            BlockoutBrush b;
            b.type = 0;                                   // a room is a Box
            b.name = rn.empty() ? rt : rn;
            b.pos[0]  = r.fnum("x", 0.0f);
            b.pos[1]  = r.fnum("y", 0.0f);
            b.pos[2]  = r.fnum("z", 0.0f);
            // w/h/d are FULL sizes in the canon schema (BlockoutBrush.size is also full
            // extents), so they copy straight across — no half/full conversion trap.
            b.size[0] = std::max(0.25f, r.fnum("w", 2.0f));
            b.size[1] = std::max(0.25f, r.fnum("h", 2.0f));
            b.size[2] = std::max(0.25f, r.fnum("d", 2.0f));
            roomTypeTint(rt, b.tint);
            b.collide = true;
            doc2.brushes.push_back(std::move(b));

            // Seed the player start at Jake's cell if this floor has it.
            if (rt == "Jake Cell" || rn == "Jake Cell") {
                doc2.playerStart[0] = b.pos[0];
                doc2.playerStart[1] = b.pos[1];
                doc2.playerStart[2] = b.pos[2];
            }
        }
    }

    out = std::move(doc2);
    return true;
}

// ---------------------------------------------------------------------------
bool runCanonImportSelfTest() {
    int pass = 0, fail = 0;
    auto ck = [&](bool c, const char* what) {
        if (c) ++pass;
        else { ++fail; x3::logInfo(std::string("[canon-test]   FAIL: ") + what); }
    };

    // A miniature two-floor project in the real 10.7 shape.
    const std::string js =
        "{\"version\":\"10.7\",\"type\":\"project\",\"name\":\"Test Facility\",\"floors\":{"
        "\"1\":{\"name\":\"Detention\",\"rooms\":["
        "  {\"n\":\"Jake Cell\",\"t\":\"Jake Cell\",\"x\":5,\"y\":0,\"z\":40,\"w\":4,\"h\":3,\"d\":4},"
        "  {\"n\":\"Main Hall\",\"t\":\"Hallway\",\"x\":22,\"y\":0,\"z\":44.5,\"w\":44,\"h\":5,\"d\":5}"
        "],\"doors\":[[0,1]]},"
        "\"2\":{\"name\":\"Labs\",\"rooms\":["
        "  {\"n\":\"Lab A\",\"t\":\"Lab\",\"x\":10,\"y\":0,\"z\":10,\"w\":8,\"h\":4,\"d\":8}"
        "],\"doors\":[]}"
        "}}";

    const CanonProject p = openCanonProjectFromString(js);
    ck(p.ok, "a well-formed project parses");
    ck(p.name == "Test Facility", "the project name is read");
    ck(p.floors.size() == 2, "both floors are listed");
    ck(!p.floors.empty() && p.floors[0].key == "1" && p.floors[0].rooms == 2,
       "floor 1 reports its room count");
    ck(p.floors.size() == 2 && p.floors[0].doors == 1, "floor 1 reports its door count");

    // Numeric floor ordering (the trap: string sort puts "10" before "2").
    {
        const std::string js10 =
            "{\"name\":\"N\",\"floors\":{\"2\":{\"rooms\":[]},\"10\":{\"rooms\":[]},\"1\":{\"rooms\":[]}}}";
        const CanonProject q = openCanonProjectFromString(js10);
        ck(q.ok && q.floors.size() == 3 &&
           q.floors[0].key == "1" && q.floors[1].key == "2" && q.floors[2].key == "10",
           "floors sort NUMERICALLY (1,2,10) not as strings (1,10,2)");
    }

    // Import floor 1.
    LevelDoc d;
    ck(importCanonFloor(p, "1", d), "floor 1 imports");
    ck(d.brushes.size() == 2, "both rooms become brushes");
    ck(!d.brushes.empty() && d.brushes[1].name == "Main Hall", "the room name is carried");
    ck(!d.brushes.empty() && std::fabs(d.brushes[1].pos[0] - 22.0f) < 1e-4f &&
       std::fabs(d.brushes[1].size[0] - 44.0f) < 1e-4f,
       "the room's center AND full-size copy straight across (no half/full mixup)");
    ck(std::fabs(d.playerStart[2] - 40.0f) < 1e-4f,
       "playerStart is seeded at the Jake Cell");
    ck(d.name.find("Detention") != std::string::npos, "the doc name carries the floor name");
    ck(!d.brushes.empty() && !d.brushes[0].collide == false, "imported rooms get collision");

    // Import floor 2 into the same doc must REPLACE, not append.
    ck(importCanonFloor(p, "2", d) && d.brushes.size() == 1,
       "importing another floor REPLACES the doc (does not accumulate)");

    // A bad floor key must leave the doc untouched (not clear it).
    LevelDoc keep;
    importCanonFloor(p, "1", keep);
    const size_t before = keep.brushes.size();
    ck(!importCanonFloor(p, "99", keep) && keep.brushes.size() == before,
       "an unknown floor key fails and leaves the doc INTACT");

    // Tints are distinct by type (a cell is not a hall).
    float cellT[3], hallT[3];
    roomTypeTint("Cell", cellT);
    roomTypeTint("Hallway", hallT);
    ck(cellT[0] != hallT[0] || cellT[1] != hallT[1] || cellT[2] != hallT[2],
       "a Cell and a Hallway get different tints");

    // A garbage project fails softly.
    ck(!openCanonProjectFromString("}{ not json").ok, "garbage fails softly, with a reason");

    x3::logInfo("[canon-test] " + std::to_string(pass) + " passed, " +
                std::to_string(fail) + " failed");
    return fail == 0;
}

} // namespace x3::editor
