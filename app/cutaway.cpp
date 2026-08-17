// LEVEL ARCHITECT — CUTAWAY VIEW. See app/cutaway.h for the what and the why.
#include "cutaway.h"

#include "mesh_prims.h"
#include "asset_root.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace x3::game {

using x3::prims::PrimMesh;
using x3::prims::makeBox;

namespace {

// ---------------------------------------------------------------------------
// FLOOR-1 DESCRIPTIONS.
//
// The canonical project ships a "desc" on every Floor 2-7 room ("Brain-computer
// links. Memory extraction.") and an EMPTY one on all 53 Floor-1 rooms — which
// is why the Babylon reference can card the Neural Interface Lab but the
// detention block would come up blank. These fill that hole, in the project's
// own voice (clipped fragments, one concrete detail, a number where the data
// gives one).
//
// PAIRED VALUE (NO_SLOP rule 4): keyed by the room's canonical `n`. The paired
// site is the JSON's floors."1".rooms[].desc — if a row there is ever authored
// for real, DELETE its line here and the JSON wins (mergeDescriptions only
// fills EMPTY descs, so a JSON desc silently overrides this table by design).
// ---------------------------------------------------------------------------
struct DescRow { const char* name; const char* desc; };
const DescRow kF1Desc[] = {
    { "Main Hall",             "44 m spine of the detention level. Every cell door faces it." },
    { "Entrance",              "The apron door. Badge readers, blast shutter, one camera." },
    { "Admin Office",          "Duty rosters and transfer paperwork. Someone left a mug." },
    { "IT Room",               "Patch panels and a dead terminal. The cameras route through here." },
    { "Network Hub",           "Fibre trunk for the whole tower. Cut it and the doors go manual." },
    { "West Cell Hall",        "40 m of cell doors. Numbered WL-1 through WL-8, west side." },
    { "Bottom Hall",           "The south run. Links both cell halls to the boss approach." },
    { "East Cell Hall",        "40 m of cell doors. Numbered EL/ER, east side." },
    { "W Service Corridor",    "Staff-only run behind the west cells. Laundry carts and pipes." },
    { "E Service Corridor",    "Staff-only run behind the east cells. Coolant lines overhead." },
    { "Security Station",      "Door control for the whole floor. Monitor wall, one guard." },
    { "Research Lab",          "Sample benches and a centrifuge. Subject notes on the glass." },
    { "Medical Bay",           "Two beds and a drug cabinet. Where the failures are patched." },
    { "Armory",               "Racked sidearms behind a coded door. The guards restock here." },
    { "Hidden Supply Cache",   "Not on any floor plan. Rations, cells, a spare keycard." },
    { "Boss Approach",         "The last corridor before Martinez. Nothing to hide behind." },
    { "Boss Arena (Martinez)", "20 m x 15 m. Chief Martinez holds the elevator behind him." },
    { "Elevator Lobby",        "The only way up. Call panel needs a security badge." },
    { "Elevator Shaft",        "The tower's vertical spine. It runs the full 203 m." },
    { "Hidden Sub-Level",      "174 m down, off the shaft. Nobody logged what is stored here." },
    { "Cave System",           "Natural rock at -178 m. The facility was built on top of it." },
    { "WL-1: Jake's Cell",     "7 x 6 m. Bed, terminal, and a floor hatch nobody knows about." },
    { "WR-1",                  "Standard holding cell. Occupant transferred, record sealed." },
    { "EL-1",                  "Standard holding cell. Scratches on the wall count to 40." },
    { "ER-1",                  "Standard holding cell. Empty, door left open." },
    { "WL-2",                  "Standard holding cell. Ration wrappers under the bunk." },
    { "WR-2",                  "Standard holding cell. The light has been out for weeks." },
    { "EL-2",                  "Standard holding cell. Occupant listed as 'in processing'." },
    { "ER-2",                  "Standard holding cell. Bunk stripped to the frame." },
    { "WL-3",                  "Standard holding cell. Someone drew the tower on the wall." },
    { "WR-3",                  "Standard holding cell. Intake date is three years old." },
    { "EL-3",                  "Standard holding cell. Drain in the floor, recently used." },
    { "ER-3",                  "Standard holding cell. The observation slot is welded shut." },
    { "WL-4",                  "Standard holding cell. Nothing left but the smell." },
    { "WR-4",                  "Standard holding cell. Restraint bolts set into the floor." },
    { "EL-4",                  "Standard holding cell. Occupant moved to Medical, F2." },
    { "ER-4",                  "Standard holding cell. Door reads FAULT on the panel." },
    { "WL-5",                  "Standard holding cell. A name scratched out, then rewritten." },
    { "WR-5",                  "Standard holding cell. Clean. Too clean." },
    { "EL-5",                  "Standard holding cell. Occupant listed only as SUBJECT 12." },
    { "ER-5",                  "Standard holding cell. The bunk is bolted to the ceiling." },
    { "WL-6",                  "Standard holding cell. Tally marks stop at 212." },
    { "WR-6: Isolation",       "Sound-deadened isolation cell. No window, no slot, no light." },
    { "EL-6",                  "Standard holding cell. Blood on the frame, wiped once." },
    { "ER-6",                  "Standard holding cell. Occupant returned from Genetics, F3." },
    { "WL-7",                  "Standard holding cell. Rations for two, one bunk." },
    { "WR-7",                  "Standard holding cell. The hatch grate has been prised." },
    { "EL-7",                  "Standard holding cell. Reserved for transfers to F6." },
    { "ER-7",                  "Standard holding cell. Empty since the last purge." },
    { "WL-8",                  "Standard holding cell. Nearest the boss approach. They hear it." },
    { "WR-8",                  "Standard holding cell. Door welded from the outside." },
    { "EL-8",                  "Standard holding cell. Storage now. Crates to the ceiling." },
    { "ER-8",                  "Standard holding cell. Last cell on the east run." },
};
constexpr uint32_t kF1DescCount = (uint32_t)(sizeof(kF1Desc) / sizeof(kF1Desc[0]));

// ---------------------------------------------------------------------------
// THE LEGEND. One row per canonical room `type`, matched EXACTLY (the type set
// is closed: 20 values across the 7 floors, asserted by --test-cutaway). Cool,
// low-key hues so they sit inside dark glass the way the reference's do — these
// are BASE COLOURS, not emissive strengths (the draw scales them down).
// ---------------------------------------------------------------------------
struct TypeColor { const char* type; float r, g, b; };
const TypeColor kTypeColor[] = {
    { "Jake Cell",      1.00f, 0.78f, 0.30f },   // the one cell that matters — warm amber
    { "Cell",           0.42f, 0.52f, 0.72f },
    { "Holding Cell",   0.46f, 0.44f, 0.66f },
    { "Hallway",        0.36f, 0.62f, 0.78f },
    { "Connector Hall", 0.32f, 0.55f, 0.70f },
    { "Stairway",       0.55f, 0.72f, 0.92f },
    { "Elevator Lobby", 0.40f, 0.80f, 0.95f },
    { "Security",       0.90f, 0.42f, 0.38f },
    { "Armory",         0.95f, 0.55f, 0.28f },
    { "Medical Bay",    0.45f, 0.90f, 0.72f },
    { "Lab",            0.55f, 0.85f, 0.55f },
    { "Server Room",    0.50f, 0.62f, 0.95f },
    { "Archive",        0.62f, 0.58f, 0.85f },
    { "Office",         0.72f, 0.70f, 0.60f },
    { "Observation",    0.80f, 0.80f, 0.90f },
    { "Storage",        0.60f, 0.56f, 0.48f },
    { "Boss Arena",     1.00f, 0.30f, 0.35f },
    { "Cave",           0.45f, 0.40f, 0.34f },
    { "Cave Chamber",   0.50f, 0.44f, 0.36f },
    { "Underground",    0.72f, 0.45f, 0.85f },
};
constexpr uint32_t kTypeColorCount = (uint32_t)(sizeof(kTypeColor) / sizeof(kTypeColor[0]));

// Per-BAND glass tint. The reference's floors read as one cool blue-grey family
// with a faint hue drift up the stack, so the eye can tell decks apart through
// six layers of glass without any of them looking coloured.
const float kBandTint[9][3] = {
    { 0.42f, 0.52f, 0.62f },   // F1 detention  — neutral steel
    { 0.40f, 0.56f, 0.60f },   // F2 medical    — faint green-teal
    { 0.40f, 0.58f, 0.54f },   // F3 genetics   — green
    { 0.46f, 0.50f, 0.68f },   // F4 cybernetics— violet-blue
    { 0.56f, 0.52f, 0.46f },   // F5 drones     — amber-grey
    { 0.40f, 0.56f, 0.50f },   // F6 salvari    — biolume green
    { 0.54f, 0.50f, 0.60f },   // F7 executive  — warm grey-violet
    { 0.40f, 0.36f, 0.44f },   // sub-level     — dead violet
    { 0.42f, 0.52f, 0.62f },   // fallback
};

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// A room is a "deep" room (the sub-level band) if its FLOOR sits far below the
// tower's ground deck. The canonical data files the cave + hidden sub-level
// under floor 1 at y = -174 / -178; nothing else on any floor is below -20.
bool isDeepRoom(const CanonRoom& r) { return r.cy < -20.0f; }

} // namespace

// ---------------------------------------------------------------------------

void cutawayTypeColor(std::string_view roomType, float outRgb[3]) {
    for (uint32_t i = 0; i < kTypeColorCount; ++i) {
        if (roomType == kTypeColor[i].type) {
            outRgb[0] = kTypeColor[i].r; outRgb[1] = kTypeColor[i].g; outRgb[2] = kTypeColor[i].b;
            return;
        }
    }
    // Unlisted type: a neutral grey-blue. --test-cutaway fails if this is ever
    // reached for real data, so an added room type gets a legend row, not a
    // silent grey box.
    outRgb[0] = 0.50f; outRgb[1] = 0.54f; outRgb[2] = 0.60f;
}

const char* cutawayGroupFor(const CutRoom& r) {
    // Mirrors the reference tool's STRUCTURES list, resolved from canon data.
    if (contains(r.name, "Jake"))                                 return "Jake's Cell";
    if (r.type == "Cell" || r.type == "Holding Cell")             return "Cells";
    if (r.name == "Main Hall")                                    return "Main Hall";
    if (contains(r.name, "Cell Hall") || r.name == "Bottom Hall") return "U-Hall";
    if (contains(r.name, "Service Corridor"))                     return "Service Corridors";
    if (r.type == "Boss Arena" || contains(r.name, "Boss"))       return "Boss Areas";
    if (contains(r.name, "Elevator") || r.type == "Elevator Lobby") return "Elevator Shaft";
    if (r.type == "Cave" || r.type == "Cave Chamber")             return "Cave System";
    if (r.type == "Underground")                                  return "Hidden / Sub-Level";
    if (r.type == "Hallway" || r.type == "Connector Hall" || r.type == "Stairway")
                                                                  return "Corridors";
    if (r.type == "Security" || r.type == "Armory")               return "Security & Armory";
    if (r.type == "Lab" || r.type == "Medical Bay" || r.type == "Server Room")
                                                                  return "Labs";
    return "Rooms";
}

void CutawayModel::towerFraming(float outCenter[3], float& outRadius) const {
    float lo[3] = {  1e9f,  1e9f,  1e9f };
    float hi[3] = { -1e9f, -1e9f, -1e9f };
    for (const CutRoom& r : rooms) {
        if (bands[r.band].subLevel) continue;      // frame the STACK, not the cave
        lo[0] = std::min(lo[0], r.x0()); hi[0] = std::max(hi[0], r.x1());
        lo[1] = std::min(lo[1], r.y0()); hi[1] = std::max(hi[1], r.y1());
        lo[2] = std::min(lo[2], r.z0()); hi[2] = std::max(hi[2], r.z1());
    }
    if (lo[0] > hi[0]) { outCenter[0] = outCenter[1] = outCenter[2] = 0; outRadius = 50; return; }
    for (int k = 0; k < 3; ++k) outCenter[k] = (lo[k] + hi[k]) * 0.5f;
    float dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2];
    outRadius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ---------------------------------------------------------------------------

CutawayModel buildCutawayModel(std::string_view jsonPath) {
    CutawayModel m;
    m.sourcePath = std::string(jsonPath);
    // ONE loader, the canonical one. Every room center/extent, the door graph
    // and the per-room floor number all come from loadCanonTower — the cutaway
    // adds no geometry of its own.
    m.canon = loadCanonTower(jsonPath);
    if (!m.canon.valid()) {
        x3::logError("cutaway: could not load the canonical project at " + m.sourcePath);
        return m;
    }

    const uint32_t n = (uint32_t)m.canon.rooms.size();
    m.rooms.reserve(n);

    // Bands: one per canonical floor, in floor order, plus the sub-level deck.
    // roomFloorNum is filled by loadCanonTower; guard the single-floor path.
    auto floorNumOf = [&](uint32_t i) -> int {
        if (i < m.canon.roomFloorNum.size()) return m.canon.roomFloorNum[i];
        return m.canon.floorNum;
    };
    // Floor display names, straight out of the project ("Detention Level", ...).
    // loadCanonTower keeps only the FIRST floor's name, so re-read the per-floor
    // names by loading each floor's header cheaply through the same parser.
    std::vector<std::string> floorNames(16);
    for (int f = 1; f <= 7; ++f) {
        CanonFloor one = loadCanonFloor(jsonPath, f);
        floorNames[(size_t)f] = one.valid() ? one.name : ("Floor " + std::to_string(f));
    }

    int maxFloor = 1;
    for (uint32_t i = 0; i < n; ++i) maxFloor = std::max(maxFloor, floorNumOf(i));
    for (int f = 1; f <= maxFloor; ++f) {
        CutBand b;
        b.floorNum = f;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "F%d %s", f, floorNames[(size_t)f].c_str());
        b.name = buf;
        b.y0 =  1e9f; b.y1 = -1e9f;
        m.bands.push_back(b);
    }
    CutBand sub;
    sub.name = "Sub-Level / Caves";
    sub.floorNum = 1;
    sub.subLevel = true;
    sub.y0 = 1e9f; sub.y1 = -1e9f;
    m.bands.push_back(sub);
    const uint8_t kSubBand = (uint8_t)(m.bands.size() - 1);

    uint32_t fromJson = 0, authored = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const CanonRoom& c = m.canon.rooms[i];
        CutRoom r;
        r.id       = i;
        r.floorNum = floorNumOf(i);
        r.name     = c.name;
        r.type     = c.type;
        r.desc     = c.desc;
        r.cx = c.cx; r.cy = c.cy; r.cz = c.cz;
        r.w  = c.w;  r.h  = c.h;  r.d  = c.d;
        r.band = isDeepRoom(c) ? kSubBand : (uint8_t)(r.floorNum - 1);
        cutawayTypeColor(r.type, r.color);

        // Fill an empty desc from the authored Floor-1 table (see kF1Desc).
        if (r.desc.empty()) {
            for (uint32_t k = 0; k < kF1DescCount; ++k) {
                if (r.name == kF1Desc[k].name) { r.desc = kF1Desc[k].desc; break; }
            }
            if (!r.desc.empty()) ++authored;
        } else {
            ++fromJson;
        }
        m.rooms.push_back(std::move(r));
    }

    // Groups, derived from the rooms (order of first appearance, so the list
    // reads top-down the way the building does).
    for (CutRoom& r : m.rooms) {
        const char* g = cutawayGroupFor(r);
        uint8_t idx = 0xFF;
        for (uint8_t k = 0; k < (uint8_t)m.groups.size(); ++k)
            if (m.groups[k].name == g) { idx = k; break; }
        if (idx == 0xFF) {
            CutGroup ng; ng.name = g;
            ng.color[0] = r.color[0]; ng.color[1] = r.color[1]; ng.color[2] = r.color[2];
            m.groups.push_back(ng);
            idx = (uint8_t)(m.groups.size() - 1);
        }
        r.group = idx;
        m.groups[idx].roomCount++;
    }

    // Band spans + counts.
    for (const CutRoom& r : m.rooms) {
        CutBand& b = m.bands[r.band];
        b.y0 = std::min(b.y0, r.y0());
        b.y1 = std::max(b.y1, r.y1());
        b.roomCount++;
    }
    // Drop bands with no rooms (defensive — the canon has none today).
    m.bands.erase(std::remove_if(m.bands.begin(), m.bands.end(),
                                 [](const CutBand& b) { return b.roomCount == 0; }),
                  m.bands.end());

    // Stats — MEASURED, never quoted.
    CutStats& s = m.stats;
    s.floors = (uint32_t)maxFloor;
    s.rooms  = n;
    s.doors  = m.canon.jsonDoorCount;
    s.groups = (uint32_t)m.groups.size();
    s.descsFromJson = fromJson;
    s.descsAuthored = authored;
    s.towerY0 =  1e9f; s.towerY1 = -1e9f; s.caveY0 = 1e9f;
    float lox = 1e9f, hix = -1e9f, loz = 1e9f, hiz = -1e9f;
    for (const CutRoom& r : m.rooms) {
        if (r.type == "Cell" || r.type == "Holding Cell" || r.type == "Jake Cell") s.cells++;
        s.caveY0 = std::min(s.caveY0, r.y0());
        if (m.bands[r.band].subLevel) continue;
        s.towerY0 = std::min(s.towerY0, r.y0());
        s.towerY1 = std::max(s.towerY1, r.y1());
        lox = std::min(lox, r.x0()); hix = std::max(hix, r.x1());
        loz = std::min(loz, r.z0()); hiz = std::max(hiz, r.z1());
    }
    s.footprintX = hix - lox;
    s.footprintZ = hiz - loz;

    x3::logInfo("cutaway: " + std::to_string(s.rooms) + " rooms / " +
                std::to_string(s.doors) + " doors / " + std::to_string(m.bands.size()) +
                " bands / " + std::to_string(s.groups) + " groups; desc " +
                std::to_string(fromJson) + " from JSON + " + std::to_string(authored) +
                " authored");
    return m;
}

// ===========================================================================
// The live view
// ===========================================================================

namespace {

// A hollow EDGE CAGE: the 12 edges of a box as thin beams of constant WORLD
// thickness, merged into one mesh. Constant thickness (not a scaled unit cube)
// is what keeps a 44 m hall's outline the same line weight as a 4 m cell's.
PrimMesh makeEdgeCage(float x0, float y0, float z0, float x1, float y1, float z1, float t) {
    PrimMesh out;
    auto add = [&](const PrimMesh& p) {
        const uint32_t base = (uint32_t)out.verts.size();
        out.verts.insert(out.verts.end(), p.verts.begin(), p.verts.end());
        for (uint32_t i : p.index) out.index.push_back(base + i);
    };
    const float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f, hz = (z1 - z0) * 0.5f;
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, cz = (z0 + z1) * 0.5f;
    // 4 beams along X (at the 4 YZ corners), 4 along Y, 4 along Z.
    for (int sy = -1; sy <= 1; sy += 2)
        for (int sz = -1; sz <= 1; sz += 2)
            add(makeBox(hx, t, t, cx, cy + sy * hy, cz + sz * hz));
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sz = -1; sz <= 1; sz += 2)
            add(makeBox(t, hy, t, cx + sx * hx, cy, cz + sz * hz));
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
            add(makeBox(t, t, hz, cx + sx * hx, cy + sy * hy, cz));
    return out;
}

const float kIdentity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

} // namespace

bool CutawayView::build(x3::rhi::IRenderDevice& device, std::string_view jsonPath) {
    m_model = buildCutawayModel(jsonPath);
    if (!m_model.valid()) return false;

    // A 1x1 white base-colour texture: every cutaway surface is a flat tint, so
    // one shared texel serves all of them (no untextured-metal trap — these are
    // pure emissive/glass draws, X3_WORLD_RULES rule 5).
    const uint8_t whitePx[4] = { 255, 255, 255, 255 };
    m_white = device.createTexture(whitePx, 1, 1, /*srgb*/ true);

    device.beginUploadBatch();
    m_gpu.resize(m_model.rooms.size());
    for (size_t i = 0; i < m_model.rooms.size(); ++i) {
        const CutRoom& r = m_model.rooms[i];
        RoomGpu& g = m_gpu[i];
        // Shell — the room volume, inset a hair so coincident room faces (the
        // canon has plenty of shared walls) do not z-fight in the glass pass.
        {
            const float k = 0.02f;
            PrimMesh p = makeBox(r.w * 0.5f - k, r.h * 0.5f - k, r.d * 0.5f - k, r.cx, r.cy, r.cz);
            g.shell = device.createMesh(p.verts.data(), (uint32_t)p.verts.size(),
                                        p.index.data(), (uint32_t)p.index.size());
        }
        // Floor plate — the slab that makes each deck readable as a deck.
        {
            const float t = 0.16f;
            PrimMesh p = makeBox(r.w * 0.5f - 0.05f, t, r.d * 0.5f - 0.05f,
                                 r.cx, r.y0() + t, r.cz);
            g.plate = device.createMesh(p.verts.data(), (uint32_t)p.verts.size(),
                                        p.index.data(), (uint32_t)p.index.size());
        }
        // Edge cage — the outline.
        {
            PrimMesh p = makeEdgeCage(r.x0(), r.y0(), r.z0(), r.x1(), r.y1(), r.z1(), 0.055f);
            g.cage = device.createMesh(p.verts.data(), (uint32_t)p.verts.size(),
                                       p.index.data(), (uint32_t)p.index.size());
        }
    }
    // Door markers: every resolved doorway as one small prism, ALL merged into a
    // single mesh (they toggle as one group, so one draw is right).
    {
        std::vector<PrimMesh> perBand(m_model.bands.size());
        auto add = [](PrimMesh& all, const PrimMesh& p) {
            const uint32_t base = (uint32_t)all.verts.size();
            all.verts.insert(all.verts.end(), p.verts.begin(), p.verts.end());
            for (uint32_t idx : p.index) all.index.push_back(base + idx);
        };
        for (const CanonDoorway& dw : m_model.canon.doorways) {
            // A doorway belongs to the band of the LOWER of the two rooms it
            // joins, so a cross-level tube's pip hides with the deck it leaves.
            if (dw.a >= m_model.rooms.size() || dw.b >= m_model.rooms.size()) continue;
            const CutRoom& ra = m_model.rooms[dw.a];
            const CutRoom& rb = m_model.rooms[dw.b];
            const uint8_t band = (ra.cy <= rb.cy) ? ra.band : rb.band;
            add(perBand[band], makeBox(0.28f, 1.05f, 0.28f, dw.cx, dw.cy + 1.05f, dw.cz));
        }
        m_doorMesh.assign(m_model.bands.size(), x3::rhi::MeshHandle{});
        for (size_t b = 0; b < perBand.size(); ++b) {
            if (perBand[b].verts.empty()) continue;
            m_doorMesh[b] = device.createMesh(perBand[b].verts.data(),
                                              (uint32_t)perBand[b].verts.size(),
                                              perBand[b].index.data(),
                                              (uint32_t)perBand[b].index.size());
        }
    }

    // ---- THE BUILDING ENVELOPE (the reference's Steel Frame / Exterior /
    // Floor Planes rows). Without it the canon reads as seven floating trays,
    // because the real decks are 10-35 m apart with nothing authored between
    // them. The envelope is the FACILITY, drawn from the rooms' own extents —
    // it invents no dimension the data does not already carry.
    {
        float lo[3] = {  1e9f,  1e9f,  1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
        for (const CutRoom& r : m_model.rooms) {
            if (m_model.bands[r.band].subLevel) continue;
            lo[0] = std::min(lo[0], r.x0()); hi[0] = std::max(hi[0], r.x1());
            lo[1] = std::min(lo[1], r.y0()); hi[1] = std::max(hi[1], r.y1());
            lo[2] = std::min(lo[2], r.z0()); hi[2] = std::max(hi[2], r.z1());
        }
        if (lo[0] < hi[0]) {
            const float m1 = 1.2f;                       // skin standoff (m)
            const float ex0 = lo[0] - m1, ex1 = hi[0] + m1;
            const float ez0 = lo[2] - m1, ez1 = hi[2] + m1;
            const float ey0 = lo[1] - 1.0f, ey1 = hi[1] + 2.5f;
            PrimMesh shell = makeBox((ex1 - ex0) * 0.5f, (ey1 - ey0) * 0.5f, (ez1 - ez0) * 0.5f,
                                     (ex0 + ex1) * 0.5f, (ey0 + ey1) * 0.5f, (ez0 + ez1) * 0.5f);
            m_envShell = device.createMesh(shell.verts.data(), (uint32_t)shell.verts.size(),
                                           shell.index.data(), (uint32_t)shell.index.size());

            // Steel frame: 4 corner columns the full height + a rim beam ring at
            // every deck level, so the eye reads floor-to-floor spacing.
            PrimMesh fr;
            auto addF = [&](const PrimMesh& p) {
                const uint32_t base = (uint32_t)fr.verts.size();
                fr.verts.insert(fr.verts.end(), p.verts.begin(), p.verts.end());
                for (uint32_t i : p.index) fr.index.push_back(base + i);
            };
            const float t = 0.30f;
            for (int sx = 0; sx < 2; ++sx)
                for (int sz = 0; sz < 2; ++sz)
                    addF(makeBox(t, (ey1 - ey0) * 0.5f, t,
                                 sx ? ex1 : ex0, (ey0 + ey1) * 0.5f, sz ? ez1 : ez0));
            for (const CutBand& b : m_model.bands) {
                if (b.subLevel) continue;
                const float y = b.y0;
                addF(makeBox((ex1 - ex0) * 0.5f, t * 0.6f, t, (ex0 + ex1) * 0.5f, y, ez0));
                addF(makeBox((ex1 - ex0) * 0.5f, t * 0.6f, t, (ex0 + ex1) * 0.5f, y, ez1));
                addF(makeBox(t, t * 0.6f, (ez1 - ez0) * 0.5f, ex0, y, (ez0 + ez1) * 0.5f));
                addF(makeBox(t, t * 0.6f, (ez1 - ez0) * 0.5f, ex1, y, (ez0 + ez1) * 0.5f));
            }
            m_envFrame = device.createMesh(fr.verts.data(), (uint32_t)fr.verts.size(),
                                           fr.index.data(), (uint32_t)fr.index.size());

            // Floor planes: one thin full-footprint slab per band, toggled with
            // that band, so hiding F5-F7 removes their decks too.
            m_planeMesh.assign(m_model.bands.size(), x3::rhi::MeshHandle{});
            for (size_t bi = 0; bi < m_model.bands.size(); ++bi) {
                const CutBand& b = m_model.bands[bi];
                if (b.subLevel) continue;
                PrimMesh p = makeBox((ex1 - ex0) * 0.5f, 0.09f, (ez1 - ez0) * 0.5f,
                                     (ex0 + ex1) * 0.5f, b.y0 - 0.12f, (ez0 + ez1) * 0.5f);
                m_planeMesh[bi] = device.createMesh(p.verts.data(), (uint32_t)p.verts.size(),
                                                    p.index.data(), (uint32_t)p.index.size());
            }
        }
    }
    device.endUploadBatch();

    frameTower();
    m_built = true;
    return true;
}

void CutawayView::shutdown(x3::rhi::IRenderDevice& device) {
    for (RoomGpu& g : m_gpu) {
        if (g.shell.valid()) device.destroyMesh(g.shell);
        if (g.plate.valid()) device.destroyMesh(g.plate);
        if (g.cage.valid())  device.destroyMesh(g.cage);
    }
    m_gpu.clear();
    for (auto& h : m_planeMesh) if (h.valid()) device.destroyMesh(h);
    m_planeMesh.clear();
    if (m_envShell.valid()) { device.destroyMesh(m_envShell); m_envShell = {}; }
    if (m_envFrame.valid()) { device.destroyMesh(m_envFrame); m_envFrame = {}; }
    for (auto& h : m_doorMesh) if (h.valid()) device.destroyMesh(h);
    m_doorMesh.clear();
    if (m_white.valid())    { device.destroyTexture(m_white); m_white = {}; }
    m_built = false;
}

// ---- camera ---------------------------------------------------------------

// Distance at which a bounding sphere of `radius` FITS the vertical field of
// view with a margin. `radius * 2` was the first cut and put the 111 m stack off
// the top and bottom of a 1280x720 frame — the framing has to come from the FOV,
// not from a multiplier that happens to look right at one aspect ratio.
float CutawayView::fitDistance(float radius) const {
    const float halfFov = m_fov * 0.5f * 3.14159265f / 180.0f;
    const float s = std::sin(halfFov);
    return (s > 1e-3f) ? (radius / s) * 0.97f : radius * 3.0f;
}

void CutawayView::recomputeEye() {
    const float cp = std::cos(m_pitch);
    m_eye[0] = m_pivot[0] - m_dist * cp * std::cos(m_yaw);
    m_eye[1] = m_pivot[1] - m_dist * std::sin(m_pitch);
    m_eye[2] = m_pivot[2] - m_dist * cp * std::sin(m_yaw);
}

void CutawayView::frameTower() {
    float c[3]; float rad = 50.0f;
    m_model.towerFraming(c, rad);
    m_pivot[0] = c[0]; m_pivot[1] = c[1]; m_pivot[2] = c[2];
    m_dist  = fitDistance(rad);
    m_yaw   = -2.20f;    // the reference's three-quarter vantage
    m_pitch = -0.42f;
    recomputeEye();
}

void CutawayView::frameAll() {
    float lo[3] = {  1e9f,  1e9f,  1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
    for (const CutRoom& r : m_model.rooms) {
        lo[0] = std::min(lo[0], r.x0()); hi[0] = std::max(hi[0], r.x1());
        lo[1] = std::min(lo[1], r.y0()); hi[1] = std::max(hi[1], r.y1());
        lo[2] = std::min(lo[2], r.z0()); hi[2] = std::max(hi[2], r.z1());
    }
    if (lo[0] > hi[0]) return;
    for (int k = 0; k < 3; ++k) m_pivot[k] = (lo[k] + hi[k]) * 0.5f;
    const float dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2];
    m_dist = fitDistance(0.5f * std::sqrt(dx * dx + dy * dy + dz * dz));
    recomputeEye();
}

void CutawayView::orbitDrag(float dxPx, float dyPx) {
    m_yaw   += dxPx * 0.0060f;
    m_pitch += dyPx * 0.0045f;
    if (m_pitch >  1.50f) m_pitch =  1.50f;
    if (m_pitch < -1.50f) m_pitch = -1.50f;
    recomputeEye();
}

void CutawayView::dolly(float notches) {
    m_dist *= std::pow(0.88f, notches);
    if (m_dist < 4.0f)   m_dist = 4.0f;
    if (m_dist > 1600.f) m_dist = 1600.f;
    recomputeEye();
}

void CutawayView::pan(float dxPx, float dyPx) {
    // Screen-relative pan of the pivot, scaled by distance so it feels the same
    // zoomed in or out.
    const float s = m_dist * 0.0016f;
    const float rx = -std::sin(m_yaw), rz = std::cos(m_yaw);   // camera right
    m_pivot[0] -= rx * dxPx * s;
    m_pivot[2] -= rz * dxPx * s;
    m_pivot[1] += dyPx * s;
    recomputeEye();
}

void CutawayView::applyCamera(x3::rhi::IRenderDevice& device) const {
    // The tower is ~285 m tall and we orbit outside it — the device's 200 m
    // default far plane would clip the far half of the building away.
    device.setCameraFar(std::max(600.0f, m_dist * 3.0f));
    device.setCamera(m_eye[0], m_eye[1], m_eye[2], m_yaw, m_pitch, m_fov);
}

// ---- visibility -----------------------------------------------------------

void CutawayView::setOpacity(float o) {
    m_opacity = std::clamp(o, 0.02f, 1.0f);
}
void CutawayView::toggleBand(uint32_t i) {
    if (i < m_model.bands.size()) m_model.bands[i].visible = !m_model.bands[i].visible;
}
void CutawayView::soloBand(uint32_t i) {
    for (uint32_t k = 0; k < m_model.bands.size(); ++k) m_model.bands[k].visible = (k == i);
}
void CutawayView::showAllBands() {
    for (CutBand& b : m_model.bands) b.visible = true;
    for (CutGroup& g : m_model.groups) g.visible = true;
}
void CutawayView::toggleGroup(uint32_t i) {
    if (i < m_model.groups.size()) m_model.groups[i].visible = !m_model.groups[i].visible;
}
bool CutawayView::bandVisible(uint32_t i) const {
    return i < m_model.bands.size() && m_model.bands[i].visible;
}
bool CutawayView::roomVisible(const CutRoom& r) const { return m_model.visible(r); }

// ---- render ---------------------------------------------------------------

void CutawayView::render(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) {
    if (!m_built || !frame.valid) return;

    // ---- WHY THE SHELLS USE *ADDITIVE* GLASS ------------------------------
    // The glass pass's normal (M2) path samples ONE scene-colour copy taken
    // before the pass and MIXES toward it, so overlapping panes REPLACE each
    // other rather than stacking — six decks of it read as six flat pastel
    // boxes, which is exactly what the first capture of this view looked like.
    // GlassMaterial::additive is the accumulating mode (glow + 0.965*dst per
    // layer, back faces self-extinguishing), so depth of building becomes
    // BRIGHTNESS: one pane is a whisper, six stacked panes are a solid mass.
    // That accumulation IS the X-ray look in the reference captures.
    //
    // EXPOSURE PAIRING (NO_SLOP rule 4). The host runs this scene at
    // r_exposure = kCutawayExposure (host_cutaway.cpp) because the engine's HDR
    // clear is a fixed dark slate that tonemaps to mid-grey at exposure 1 — the
    // reference's background is near-black. Every emissive strength below is
    // therefore quoted PRE-exposure and is ~1/kCutawayExposure larger than it
    // would be in a normally-exposed world. Change one, change both.
    const float dark[4] = { 0.0f, 0.0f, 0.0f, 1.0f };   // black albedo: pure emission
    const float op = solid ? 1.0f : m_opacity;

    auto glow = [&](x3::rhi::MeshHandle mesh, const float rgb[3], float k, float rim) {
        if (!mesh.valid()) return;
        x3::rhi::IRenderDevice::GlassMaterial gm{};
        gm.additive = rim;                 // accumulating mode + its rim-fade exponent
        gm.opacity  = 1.0f;                // unused by the additive path
        gm.tint[0] = rgb[0]; gm.tint[1] = rgb[1]; gm.tint[2] = rgb[2];
        const float bc[4] = { 1, 1, 1, 1 };
        const float em[4] = { k, k, k, 1.0f };   // glow = em.rgb * em.a * texel * tint * rim
        device.drawMeshGlass(frame, mesh, m_white, bc, em, gm, kIdentity, /*alphaBlend*/ true);
    };

    // ---- OPAQUE LINE-WORK ONLY --------------------------------------------
    // Only the thin structure writes depth: the edge cages, the steel columns
    // and the door pips. Everything with AREA (deck planes, room plates, room
    // shells, the exterior skin) goes through the additive path below, because
    // an opaque deck plate hides every room under it — which is the opposite of
    // a cutaway. (The second capture of this view had exactly that: seven grey
    // trays, each hiding the floor beneath.)
    if (envelope && m_envFrame.valid()) {
        const float e[4] = { 0.55f, 0.68f, 0.86f, 1.0f };   // cold steel
        device.drawMeshEmissive(frame, m_envFrame, m_white, dark, e, kIdentity);
    }
    if (cages) {
        for (size_t i = 0; i < m_model.rooms.size(); ++i) {
            const CutRoom& r = m_model.rooms[i];
            if (!roomVisible(r) || !m_gpu[i].cage.valid()) continue;
            const float e[4] = { r.color[0] * 3.2f, r.color[1] * 3.2f, r.color[2] * 3.2f, 1.0f };
            device.drawMeshEmissive(frame, m_gpu[i].cage, m_white, dark, e, kIdentity);
        }
    }
    if (doors) {
        const float e[4] = { 0.45f, 2.60f, 1.05f, 1.0f };   // the reference's green door pips
        for (size_t b = 0; b < m_doorMesh.size(); ++b) {
            if (!m_doorMesh[b].valid() || !m_model.bands[b].visible) continue;
            device.drawMeshEmissive(frame, m_doorMesh[b], m_white, dark, e, kIdentity);
        }
    }

    // ---- EVERYTHING WITH AREA, accumulating --------------------------------
    // rim 0.22: low, so grazing faces still register (a high exponent would melt
    // every side wall away and leave only the faces pointing at the camera).
    if (envelope && m_envShell.valid()) {
        const float envTint[3] = { 0.34f, 0.46f, 0.60f };
        glow(m_envShell, envTint, op * 1.5f, 0.22f);
    }
    if (planes) {
        const float deck[3] = { 0.30f, 0.42f, 0.58f };
        for (size_t bi = 0; bi < m_planeMesh.size(); ++bi) {
            if (!m_planeMesh[bi].valid() || !m_model.bands[bi].visible) continue;
            glow(m_planeMesh[bi], deck, 0.80f, 0.05f);  // near-flat: read from above AND edge-on
        }
    }
    if (plates) {
        for (size_t i = 0; i < m_model.rooms.size(); ++i) {
            const CutRoom& r = m_model.rooms[i];
            if (!roomVisible(r) || !m_gpu[i].plate.valid()) continue;
            glow(m_gpu[i].plate, r.color, 2.1f, 0.05f);
        }
    }
    for (size_t i = 0; i < m_model.rooms.size(); ++i) {
        const CutRoom& r = m_model.rooms[i];
        if (!roomVisible(r) || !m_gpu[i].shell.valid()) continue;
        const float* t = kBandTint[std::min<size_t>(r.band, 8)];
        glow(m_gpu[i].shell, t, op * 3.4f, 0.22f);
    }
}

// ---- hover pick -----------------------------------------------------------

uint32_t cutawayPick(const CutawayModel& m,
                     const float eye[3], float yaw, float pitch, float fovDeg,
                     float mouseXPx, float mouseYPx, uint32_t viewW, uint32_t viewH) {
    if (m.rooms.empty() || viewW == 0 || viewH == 0) return kNoRoom;
    // Build the ray from the camera pose (engine convention: forward =
    // (cos p cos y, sin p, cos p sin y), up = +Y), so it can never disagree with
    // what applyCamera() pushed.
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    const float fwd[3] = { cp * cy, sp, cp * sy };
    // right = normalize(cross(fwd, worldUp)) = (-fwd.z, 0, fwd.x); up =
    // cross(right, fwd). This is the SAME right vector every fly-cam in the tree
    // uses (`rx=-dz/rl, rz=dx/rl`). It was written negated here first, which
    // mirrored the hover ray in BOTH screen axes — the card named the room
    // diagonally opposite the cursor. Caught by C20's projection cross-check,
    // which is the entire reason that check exists.
    float rgt[3] = { -fwd[2], 0.0f, fwd[0] };
    float rl = std::sqrt(rgt[0] * rgt[0] + rgt[2] * rgt[2]);
    if (rl < 1e-5f) { rgt[0] = 1; rgt[2] = 0; rl = 1; }
    rgt[0] /= rl; rgt[2] /= rl;
    const float up[3] = { rgt[1] * fwd[2] - rgt[2] * fwd[1],
                          rgt[2] * fwd[0] - rgt[0] * fwd[2],
                          rgt[0] * fwd[1] - rgt[1] * fwd[0] };

    const float aspect = (float)viewW / (float)viewH;
    const float tanHalf = std::tan(fovDeg * 0.5f * 3.14159265f / 180.0f);
    // NDC: +x right, +y UP (screen y grows down, so flip).
    const float ndcX = (2.0f * mouseXPx / (float)viewW - 1.0f) * tanHalf * aspect;
    const float ndcY = (1.0f - 2.0f * mouseYPx / (float)viewH) * tanHalf;
    float dir[3];
    for (int k = 0; k < 3; ++k) dir[k] = fwd[k] + rgt[k] * ndcX + up[k] * ndcY;
    const float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (dl < 1e-6f) return kNoRoom;
    for (int k = 0; k < 3; ++k) dir[k] /= dl;

    uint32_t best = kNoRoom;
    float bestT = 1e9f;
    for (const CutRoom& r : m.rooms) {
        if (!m.visible(r)) continue;
        const float lo[3] = { r.x0(), r.y0(), r.z0() };
        const float hi[3] = { r.x1(), r.y1(), r.z1() };
        float t0 = 0.0f, t1 = 1e9f;
        bool hit = true;
        for (int k = 0; k < 3; ++k) {
            if (std::fabs(dir[k]) < 1e-7f) {
                if (eye[k] < lo[k] || eye[k] > hi[k]) { hit = false; break; }
                continue;
            }
            const float inv = 1.0f / dir[k];
            float a = (lo[k] - eye[k]) * inv, b = (hi[k] - eye[k]) * inv;
            if (a > b) std::swap(a, b);
            t0 = std::max(t0, a); t1 = std::min(t1, b);
            if (t0 > t1) { hit = false; break; }
        }
        if (hit && t0 < bestT) { bestT = t0; best = r.id; }
    }
    return best;
}

uint32_t CutawayView::pickRoom(float mouseXPx, float mouseYPx,
                               uint32_t viewW, uint32_t viewH) const {
    if (!m_built) return kNoRoom;
    return cutawayPick(m_model, m_eye, m_yaw, m_pitch, m_fov,
                       mouseXPx, mouseYPx, viewW, viewH);
}

// ---- HUD ------------------------------------------------------------------

namespace {

// The reference's palette, read off the captures.
const float kInk[4]      = { 0.030f, 0.045f, 0.070f, 0.86f };   // panel fill
const float kInkSolid[4] = { 0.035f, 0.055f, 0.090f, 0.95f };   // card fill
const float kEdge[4]     = { 0.180f, 0.500f, 0.850f, 1.00f };   // the blue border
const float kTitle[4]    = { 0.310f, 0.680f, 0.960f, 1.00f };   // caps title
const float kBody[4]     = { 0.700f, 0.760f, 0.820f, 1.00f };
const float kDim[4]      = { 0.400f, 0.470f, 0.550f, 1.00f };
const float kOn[4]       = { 0.450f, 0.820f, 0.980f, 1.00f };
const float kOff[4]      = { 0.300f, 0.330f, 0.380f, 1.00f };

void frameRect(x3::rhi::IRenderDevice& d, const x3::rhi::FrameContext& f,
               float x, float y, float w, float h, const float fill[4], const float edge[4]) {
    d.drawHudQuad(f, x, y, w, h, fill);
    d.drawHudQuad(f, x,         y,         w, 1.0f, edge);
    d.drawHudQuad(f, x,         y + h - 1, w, 1.0f, edge);
    d.drawHudQuad(f, x,         y,      1.0f, h,    edge);
    d.drawHudQuad(f, x + w - 1, y,      1.0f, h,    edge);
}

// Word-wrap `s` into <= `cols` character lines (the HUD mono font advances a
// fixed cell, so a column count IS a pixel width).
void wrapText(const std::string& s, size_t cols, std::vector<std::string>& out) {
    out.clear();
    std::string line;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(' ', i);
        if (j == std::string::npos) j = s.size();
        const std::string word = s.substr(i, j - i);
        if (!line.empty() && line.size() + 1 + word.size() > cols) { out.push_back(line); line.clear(); }
        if (!line.empty()) line += ' ';
        line += word;
        i = j + 1;
    }
    if (!line.empty()) out.push_back(line);
}

} // namespace

void CutawayView::drawCard(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                           uint32_t room) const {
    if (room >= m_model.rooms.size()) return;
    const CutRoom& r = m_model.rooms[room];

    uint32_t vw = 0, vh = 0;
    device.hudSize(vw, vh);
    if (!vw || !vh) return;

    // Anchor on the room's projected CENTER (the reference cards float beside
    // the room they name).
    float sx = (float)vw * 0.5f, sy = (float)vh * 0.5f;
    device.worldToScreen(r.cx, r.cy, r.cz, sx, sy);

    // Sized to sit in the same type family as the panels (9 px body): the card
    // is a label on the model, not a second UI.
    const float titlePx = 11.0f, bodyPx = 9.0f;
    std::string title = r.name;
    for (char& c : title) if (c >= 'a' && c <= 'z') c = (char)(c - 32);

    std::vector<std::string> body;
    wrapText(r.desc, 52, body);

    const float padX = 10.0f, padY = 8.0f;
    float textW = device.textAdvance(x3::rhi::FontRole::Console, title.c_str(), titlePx);
    for (const std::string& l : body)
        textW = std::max(textW, device.textAdvance(x3::rhi::FontRole::Console, l.c_str(), bodyPx));
    const float w = textW + padX * 2.0f;
    const float h = padY * 2.0f + titlePx * 1.5f + 4.0f + (float)body.size() * (bodyPx * 1.55f);

    float x = sx + 18.0f, y = sy + 12.0f;
    if (x + w > (float)vw - 8.0f) x = (float)vw - 8.0f - w;
    if (y + h > (float)vh - 8.0f) y = (float)vh - 8.0f - h;
    if (x < 8.0f) x = 8.0f;
    if (y < 8.0f) y = 8.0f;

    frameRect(device, frame, x, y, w, h, kInkSolid, kEdge);
    float ty = y + padY;
    device.drawHudText(frame, title.c_str(), x + padX, ty, titlePx, kTitle);
    ty += titlePx * 1.5f + 4.0f;
    for (const std::string& l : body) {
        device.drawHudText(frame, l.c_str(), x + padX, ty, bodyPx, kBody);
        ty += bodyPx * 1.55f;
    }
}

void CutawayView::drawUi(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                         uint32_t hovered) const {
    if (!frame.valid) return;
    uint32_t vw = 0, vh = 0;
    device.hudSize(vw, vh);
    if (!vw || !vh) return;

    // The HUD mono font advances a FIXED cell == the glyph size, so a panel's
    // width is a character count and every row is exactly `rowH` tall. Panels
    // are sized from those two facts rather than from a guessed pixel height —
    // the first pass guessed, and the floors list ran out through the bottom of
    // its own border.
    // rowH is 1.55x the glyph size, not 1.25x: the mono atlas's glyph BOX is
    // taller than the advance cell, so tighter rows collide (they did).
    const float px = 9.0f, rowH = 14.0f, pad = 9.0f;
    auto boxW = [&](int cols) { return (float)cols * px + pad * 2.0f; };

    // ---- LEFT: the tool panel (camera hint, floors, structures, view) -------
    if (panel) {
        const int   cols = 30;
        const float w = boxW(cols);
        // Rows: title + hint + [gap+hdr] + bands + [gap+hdr] + groups +
        //       [gap+hdr] + opacity label + slider + 5 checkboxes.
        const int nRows = 2 + 1 + (int)m_model.bands.size()
                            + 1 + (int)m_model.groups.size()
                            + 1 + 2 + 5;
        const float h = pad * 2.0f + (float)nRows * rowH + 3.0f * 4.0f;
        const float x = 12.0f, y = 12.0f;
        frameRect(device, frame, x, y, w, h, kInk, kEdge);
        float ty = y + pad;
        auto line = [&](const char* s, const float* c) {
            device.drawHudText(frame, s, x + pad, ty, px, c); ty += rowH;
        };
        auto header = [&](const char* s) { ty += 4.0f; line(s, kDim); };
        char buf[160];

        line("CUTAWAY - ESCAPELAB48", kTitle);
        line("drag orbit  wheel zoom  MMB pan", kDim);

        header("FLOORS  1-9 toggle / shift solo");
        for (uint32_t i = 0; i < m_model.bands.size(); ++i) {
            const CutBand& b = m_model.bands[i];
            std::snprintf(buf, sizeof(buf), "[%c] %-23.23s %2u",
                          b.visible ? 'x' : ' ', b.name.c_str(), b.roomCount);
            line(buf, b.visible ? kOn : kOff);
        }

        header("STRUCTURES        ctrl+1-9");
        for (uint32_t i = 0; i < m_model.groups.size(); ++i) {
            const CutGroup& g = m_model.groups[i];
            std::snprintf(buf, sizeof(buf), "[%c] %-23.23s %2u",
                          g.visible ? 'x' : ' ', g.name.c_str(), g.roomCount);
            line(buf, g.visible ? kOn : kOff);
        }

        header("VIEW");
        // The OPACITY SLIDER — the reference's one continuous control.
        {
            std::snprintf(buf, sizeof(buf), "Opacity  %.2f          [ / ]", solid ? 1.0f : m_opacity);
            device.drawHudText(frame, buf, x + pad, ty, px, kBody);
            ty += rowH;
            const float bx = x + pad, bw = w - pad * 2.0f, bh = 4.0f;
            const float track[4] = { 0.10f, 0.14f, 0.20f, 1.0f };
            device.drawHudQuad(frame, bx, ty + 3.0f, bw, bh, track);
            device.drawHudQuad(frame, bx, ty + 3.0f, bw * (solid ? 1.0f : m_opacity), bh, kTitle);
            ty += rowH;
        }
        auto check = [&](bool on, const char* label) {
            std::snprintf(buf, sizeof(buf), "[%c] %s", on ? 'x' : ' ', label);
            line(buf, on ? kOn : kOff);
        };
        check(solid,    "Solid View            (V)");
        check(planes,   "Floor Planes          (N)");
        check(plates,   "Room Plates           (P)");
        check(cages,    "Wireframe             (G)");
        check(envelope, "Steel Frame/Exterior  (X)");
    }

    // ---- RIGHT: the colour legend ------------------------------------------
    if (legend) {
        const float w = boxW(16) + 16.0f;    // + the swatch column
        const float h = pad * 2.0f + (float)(kTypeColorCount + 1) * rowH + 4.0f;
        const float x = (float)vw - w - 12.0f, y = 12.0f;
        frameRect(device, frame, x, y, w, h, kInk, kEdge);
        float ty = y + pad;
        device.drawHudText(frame, "LEGEND", x + pad, ty, px, kDim);
        ty += rowH + 4.0f;
        for (uint32_t i = 0; i < kTypeColorCount; ++i) {
            const float sw[4] = { kTypeColor[i].r, kTypeColor[i].g, kTypeColor[i].b, 1.0f };
            device.drawHudQuad(frame, x + pad, ty + 1.0f, 8.0f, 8.0f, sw);
            device.drawHudText(frame, kTypeColor[i].type, x + pad + 14.0f, ty, px, kBody);
            ty += rowH;
        }
    }

    // ---- BOTTOM-LEFT: the stats panel (all MEASURED from the data) ---------
    if (panel) {
        const CutStats& s = m_model.stats;
        char rows[9][80];
        std::snprintf(rows[0], 80, "Source          EscapeLab48_AllFloors_v2");
        std::snprintf(rows[1], 80, "Total floors    %u + sub-level", s.floors);
        std::snprintf(rows[2], 80, "Total rooms     %u", s.rooms);
        std::snprintf(rows[3], 80, "Doorways        %u", s.doors);
        std::snprintf(rows[4], 80, "Detention cells %u", s.cells);
        std::snprintf(rows[5], 80, "Stack height    %.0f m (%.0f..%.0f)",
                      s.towerY1 - s.towerY0, s.towerY0, s.towerY1);
        std::snprintf(rows[6], 80, "Cave depth      %.0f m", s.caveY0);
        std::snprintf(rows[7], 80, "Footprint       %.0f x %.0f m", s.footprintX, s.footprintZ);
        std::snprintf(rows[8], 80, "Descriptions    %u data + %u authored",
                      s.descsFromJson, s.descsAuthored);
        const float w = boxW(41);
        const float h = pad * 2.0f + 10.0f * rowH + 4.0f;
        const float x = 12.0f, y = (float)vh - h - 12.0f;
        frameRect(device, frame, x, y, w, h, kInk, kEdge);
        float ty = y + pad;
        device.drawHudText(frame, "FACILITY", x + pad, ty, px, kDim);
        ty += rowH + 4.0f;
        for (int i = 0; i < 9; ++i) {
            device.drawHudText(frame, rows[i], x + pad, ty, px, kBody); ty += rowH;
        }
    }

    // ---- BOTTOM-RIGHT: the live POS / DEPTH readout -------------------------
    {
        char pos[80];
        std::snprintf(pos, sizeof(pos), "X:%.1f Y:%.1f Z:%.1f", m_eye[0], m_eye[1], m_eye[2]);
        // DEPTH: which deck the camera's height puts it level with — the
        // reference's "Surface" readout, resolved against the real band spans.
        const char* deck = (m_eye[1] > m_model.stats.towerY1) ? "Above roof"
                         : (m_eye[1] < m_model.stats.towerY0) ? "Below grade" : "Surface";
        for (const CutBand& b : m_model.bands)
            if (m_eye[1] >= b.y0 && m_eye[1] <= b.y1) { deck = b.name.c_str(); break; }
        const float w = boxW(28);
        const float h = pad * 2.0f + 4.0f * rowH;
        const float x = (float)vw - w - 12.0f, y = (float)vh - h - 12.0f;
        frameRect(device, frame, x, y, w, h, kInk, kEdge);
        float ty = y + pad;
        device.drawHudText(frame, "POS",   x + pad, ty, px, kDim);  ty += rowH;
        device.drawHudText(frame, pos,     x + pad, ty, px, kOn);   ty += rowH;
        device.drawHudText(frame, "DEPTH", x + pad, ty, px, kDim);  ty += rowH;
        device.drawHudText(frame, deck,    x + pad, ty, px, kBody);
    }

    // ---- The hover card, last so it sits over everything -------------------
    if (hovered < m_model.rooms.size()) drawCard(device, frame, hovered);
}

// ===========================================================================
// --test-cutaway
// ===========================================================================

namespace {
int g_pass = 0, g_fail = 0;
void ck(bool ok, const char* what) {
    if (ok) { ++g_pass; x3::logInfo(std::string("[cutaway-test] PASS ") + what); }
    else    { ++g_fail; x3::logError(std::string("[cutaway-test] FAIL ") + what); }
}
} // namespace

bool runCutawaySelfTest() {
    g_pass = g_fail = 0;
    const std::string path = canonProjectJsonPath();
    x3::logInfo("[cutaway-test] source: " + path);

    CutawayModel m = buildCutawayModel(path);
    ck(m.valid(), "C1 the canonical project loads and builds a cutaway model");
    if (!m.valid()) { x3::logError("[cutaway-test] 0 passed, 1 failed"); return false; }

    // The three numbers the canonical file is known by (docs + level_loader.h).
    ck(m.stats.floors == 7,  "C2 7 floors");
    ck(m.stats.rooms == 124, "C3 124 rooms built");
    ck(m.stats.doors == 160, "C4 160 doors parsed from the project");

    // Bands: 7 floor decks + the sub-level, each non-empty, each with a real span.
    ck(m.bands.size() == 8, "C5 8 visibility bands (7 floors + sub-level)");
    bool spans = true, counted = true;
    uint32_t bandRooms = 0;
    for (const CutBand& b : m.bands) {
        if (!(b.y1 > b.y0)) spans = false;
        if (b.roomCount == 0) counted = false;
        bandRooms += b.roomCount;
    }
    ck(spans && counted, "C6 every band has rooms and a positive Y span");
    ck(bandRooms == m.stats.rooms, "C7 every room lands in exactly one band");

    // The sub-level band is the deep one and holds the two -174/-178 rooms.
    const CutBand& sub = m.bands.back();
    ck(sub.subLevel && sub.roomCount == 2 && sub.y1 < -100.0f,
       "C8 the sub-level band holds the 2 deep rooms (Cave System + Hidden Sub-Level)");

    // Groups (the STRUCTURES list) partition the rooms too.
    uint32_t groupRooms = 0;
    for (const CutGroup& g : m.groups) groupRooms += g.roomCount;
    ck(m.groups.size() >= 8 && groupRooms == m.stats.rooms,
       "C9 the structure groups partition every room");

    // EVERY room has a description — the whole point of the hover card.
    uint32_t missing = 0;
    std::string firstMissing;
    for (const CutRoom& r : m.rooms)
        if (r.desc.empty()) { if (!missing) firstMissing = r.name; ++missing; }
    if (missing) x3::logError("[cutaway-test] first room with no desc: " + firstMissing);
    ck(missing == 0, "C10 every room carries a hover description");
    ck(m.stats.descsFromJson + m.stats.descsAuthored == m.stats.rooms,
       "C11 descriptions account for every room (JSON + authored)");
    ck(m.stats.descsFromJson == 71 && m.stats.descsAuthored == 53,
       "C12 71 descriptions come from the project data, 53 are authored for F1");

    // The legend covers every room type in the data (no silent grey boxes).
    uint32_t uncoloured = 0;
    std::string firstUncoloured;
    for (const CutRoom& r : m.rooms) {
        bool found = false;
        for (uint32_t k = 0; k < kTypeColorCount; ++k)
            if (r.type == kTypeColor[k].type) { found = true; break; }
        if (!found) { if (!uncoloured) firstUncoloured = r.type; ++uncoloured; }
    }
    if (uncoloured) x3::logError("[cutaway-test] first unlegended type: " + firstUncoloured);
    ck(uncoloured == 0, "C13 the legend colours every room type in the data");

    // The named reference rooms resolve, with the reference's own words.
    const CanonFloor& cf = m.canon;
    const uint32_t nil = cf.roomByName("Neural Interface Lab");
    ck(nil != kNoRoom && m.rooms[nil].desc.find("Brain-computer links") != std::string::npos,
       "C14 'Neural Interface Lab' resolves and carries the project's own desc");
    const uint32_t jake = cf.roomByName("Jake's Cell");
    ck(jake != kNoRoom && !m.rooms[jake].desc.empty(),
       "C15 Jake's Cell resolves and now has a description (the F1 hole is closed)");

    // Framing + geometry sanity: the stack is the ~285 m spire the docs name,
    // and the cave sits ~178 m under it.
    float c[3], rad = 0;
    m.towerFraming(c, rad);
    const float stack = m.stats.towerY1 - m.stats.towerY0;
    ck(stack > 100.0f && stack < 400.0f && rad > 20.0f,
       "C16 the above-ground stack frames to a sane height + radius");
    ck(m.stats.caveY0 < -170.0f, "C17 the cave deck is >170 m below the tower floor");

    // ---- HOVER PICK, fired for real at the real facility (no GPU) ----------
    // Aim at a NAMED room rather than at "the middle of the building": the eye
    // stands 400 m off the +X side at the Boss Arena's own Y and Z and looks
    // back along -X, so the correct answer is a single known room and the check
    // cannot pass by accidentally clipping something. (An earlier version of
    // this test aimed at the stack's centroid and failed because nothing in the
    // canon happens to occupy that exact line — the guess, not the pick, was
    // wrong. NO_SLOP rule 9: assert against a measured target.)
    {
        const uint32_t W = 1600, H = 900;
        const uint32_t target = cf.roomByName("Boss Arena (Martinez)");
        ck(target != kNoRoom, "C18 the aiming target 'Boss Arena (Martinez)' resolves");
        if (target != kNoRoom) {
            const CutRoom& t = m.rooms[target];
            float eye[3] = { t.cx + 400.0f, t.cy, t.cz };
            const float yawBack = 3.14159265f;   // looking along -X
            const uint32_t hit = cutawayPick(m, eye, yawBack, 0.0f, 45.0f,
                                             W * 0.5f, H * 0.5f, W, H);
            ck(hit == target,
               "C19 the center ray hits exactly the room it was aimed at");

            // Off-center rays, CROSS-CHECKED AGAINST A PROJECTION. Sweep the
            // cursor over the stack and, for every hit, project that room's 8
            // corners with a second, independently-written perspective transform
            // and require the sampled pixel to fall inside the room's projected
            // rect. Ray-casting and projecting are different maths; if the pick
            // has a sign or axis flipped, they disagree.
            float sweepEye[3] = { c[0] + rad * 1.6f, c[1], c[2] };
            const float fovS = 45.0f;
            const float tanH = std::tan(fovS * 0.5f * 3.14159265f / 180.0f);
            const float aspect = (float)W / (float)H;
            const float fwd[3] = { -1.0f, 0.0f, 0.0f };     // yaw = pi, pitch = 0
            const float rgt[3] = {  0.0f, 0.0f, -1.0f };    // normalize(cross(fwd,+Y))
            const float upv[3] = {  0.0f, 1.0f,  0.0f };
            auto project = [&](float px, float py, float pz, float& sx, float& sy) -> bool {
                const float rel[3] = { px - sweepEye[0], py - sweepEye[1], pz - sweepEye[2] };
                const float vz = rel[0]*fwd[0] + rel[1]*fwd[1] + rel[2]*fwd[2];
                if (vz <= 0.01f) return false;
                const float vx = rel[0]*rgt[0] + rel[1]*rgt[1] + rel[2]*rgt[2];
                const float vy = rel[0]*upv[0] + rel[1]*upv[1] + rel[2]*upv[2];
                sx = (vx / (vz * tanH * aspect) * 0.5f + 0.5f) * (float)W;
                sy = (0.5f - vy / (vz * tanH) * 0.5f) * (float)H;
                return true;
            };
            uint32_t hits = 0, disagree = 0;
            // 16x9 samples over the viewport. Most MISS — the frustum at this
            // distance is wider than the building and the decks have gaps
            // between them — so the bar is "a healthy number of hits, and every
            // one of them agrees", not "everything hits".
            for (int gx = 1; gx < 16; ++gx) {
                for (int gy = 1; gy < 9; ++gy) {
                    const float mxp = (float)W * (float)gx / 16.0f;
                    const float myp = (float)H * (float)gy / 9.0f;
                    const uint32_t h = cutawayPick(m, sweepEye, yawBack, 0.0f, fovS, mxp, myp, W, H);
                    if (h == kNoRoom) continue;
                    ++hits;
                    const CutRoom& r = m.rooms[h];
                    float lox = 1e9f, hix = -1e9f, loy = 1e9f, hiy = -1e9f;
                    bool allFront = true;
                    for (int k = 0; k < 8; ++k) {
                        const float px = (k & 1) ? r.x1() : r.x0();
                        const float py = (k & 2) ? r.y1() : r.y0();
                        const float pz = (k & 4) ? r.z1() : r.z0();
                        float sx = 0, sy = 0;
                        if (!project(px, py, pz, sx, sy)) { allFront = false; break; }
                        lox = std::min(lox, sx); hix = std::max(hix, sx);
                        loy = std::min(loy, sy); hiy = std::max(hiy, sy);
                    }
                    if (!allFront) continue;
                    const float slack = 1.5f;
                    if (mxp < lox - slack || mxp > hix + slack ||
                        myp < loy - slack || myp > hiy + slack) {
                        ++disagree;
                        x3::logError("[cutaway-test] pick/projection disagree at " +
                                     std::to_string((int)mxp) + "," + std::to_string((int)myp) +
                                     " -> '" + r.name + "'");
                    }
                }
            }
            x3::logInfo("[cutaway-test] ray sweep: " + std::to_string(hits) + " hits, " +
                        std::to_string(disagree) + " disagreements");
            ck(hits >= 20 && disagree == 0,
               "C20 an off-center ray sweep hits the stack and every hit agrees with the projection");

            // A ray aimed at empty sky must miss.
            const uint32_t miss = cutawayPick(m, eye, yawBack, 1.40f, 45.0f,
                                              W * 0.5f, H * 0.5f, W, H);
            ck(miss == kNoRoom, "C21 a ray aimed at the sky hits nothing");

            // Hiding every band must make the same center ray miss — the proof
            // the visibility toggles really gate the pick (a hover card naming a
            // hidden floor's room is the exact bug this catches).
            CutawayModel hidden = m;
            for (CutBand& b : hidden.bands) b.visible = false;
            const uint32_t none = cutawayPick(hidden, eye, yawBack, 0.0f, 45.0f,
                                              W * 0.5f, H * 0.5f, W, H);
            ck(none == kNoRoom, "C22 hiding every band makes the pick miss (toggles gate hover)");

            // Hiding only the target's own STRUCTURE group must hand the pick to
            // whatever is behind it, never to the hidden room.
            CutawayModel noBoss = m;
            noBoss.groups[m.rooms[target].group].visible = false;
            const uint32_t behind = cutawayPick(noBoss, eye, yawBack, 0.0f, 45.0f,
                                                W * 0.5f, H * 0.5f, W, H);
            ck(behind != target, "C23 hiding a structure group removes it from the pick");
        }
    }

    x3::logInfo("[cutaway-test] " + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
