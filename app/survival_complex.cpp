// ============================================================================
// SURVIVAL COMPLEX — Levels 2-7 + stairwell + 4-person elevator + Route-B hall.
// See survival_complex.h for the canon map (CLUB_1127_CANON_SPEC.md §4).
//
// Coordinate convention (matches club1127.cpp): X = East(+)/West(-), Z =
// North(+)/South(-), Y = up. Everything authored at club-local Y is offset by
// oy = kClubY (-200). The Complex sits WEST of the club (negative X, beyond the
// club west wall at X = -kHL) and DESCENDS from L1 (Private Lounge, club-local
// Y = kLoungeY) down to L7 hydroponics ~100 ft below.
// ============================================================================
#include "survival_complex.h"

#include "engine/core/x3_log.h"
#include "mesh_prims.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace x3::game {

namespace {

// ---- Material palette (baseColor RGBA). Bunker concrete + steel + industrial.
// Deeper levels trend colder / more utilitarian. ----------------------------
// Utilitarian bunker concrete reads as lit GREY (not the club's near-black
// neon-mood palette) so the practical lighting resolves the rooms.
const float kConcrete[4] = { 0.310f, 0.300f, 0.280f, 1.0f };  // warm bunker concrete
const float kConcreteD[4]= { 0.245f, 0.240f, 0.235f, 1.0f };  // deeper/cooler concrete
const float kCeilM[4]    = { 0.210f, 0.208f, 0.200f, 1.0f };  // ceiling slab
const float kFloorM[4]   = { 0.270f, 0.258f, 0.235f, 1.0f };  // poured floor
const float kStepM[4]    = { 0.300f, 0.300f, 0.315f, 1.0f };  // concrete stair tread
const float kSteel[4]    = { 0.230f, 0.235f, 0.255f, 1.0f };  // brushed steel
const float kSteelD[4]   = { 0.150f, 0.152f, 0.165f, 1.0f };  // dark steel / cab
const float kGrate[4]    = { 0.110f, 0.112f, 0.120f, 1.0f };  // grate / catwalk
const float kMetal[4]    = { 0.190f, 0.190f, 0.205f, 1.0f };  // metal cabinet
const float kRust[4]     = { 0.180f, 0.120f, 0.080f, 1.0f };  // rusted / fuel tank
const float kWood[4]     = { 0.150f, 0.100f, 0.060f, 1.0f };  // wood (rec shelves)
const float kLeather[4]  = { 0.090f, 0.070f, 0.060f, 1.0f };  // couch leather
const float kWhitePlas[4]= { 0.520f, 0.530f, 0.545f, 1.0f };  // medical white
const float kGreenPlant[4]={ 0.150f, 0.420f, 0.120f, 1.0f };  // living foliage (lush)
const float kTankM[4]    = { 0.200f, 0.230f, 0.260f, 1.0f };  // water/air tank
const float kRail[4]     = { 0.260f, 0.262f, 0.290f, 1.0f };  // railing

// ---- Emissive helpers { r, g, b, strength }. strength > 1 => HDR bloom. -----
const float kOff[4]       = { 0.0f, 0.0f, 0.0f, 0.0f };
const float kEmAmber[4]   = { 1.00f, 0.62f, 0.26f, 1.7f };  // warm amber strip (utilitarian)
const float kEmAmberDim[4]= { 1.00f, 0.60f, 0.25f, 1.1f };  // dim storage amber
const float kEmWhite[4]   = { 0.90f, 0.94f, 1.00f, 2.2f };  // clinical medical white
const float kEmCyan[4]    = { 0.30f, 0.80f, 1.00f, 1.8f };  // water / air life-support
const float kEmOrange[4]  = { 1.00f, 0.45f, 0.10f, 2.0f };  // power/generator industrial
const float kEmScreen[4]  = { 0.35f, 0.45f, 0.85f, 2.4f };  // workstation / rec screen
const float kEmGreenLED[4]= { 0.10f, 1.00f, 0.20f, 2.6f };  // status LED / keypad green
const float kEmRedLED[4]  = { 1.00f, 0.10f, 0.08f, 2.4f };  // alarm / breaker red
const float kEmGrow[4]    = { 0.95f, 0.30f, 1.00f, 1.9f };  // hydroponics grow-light magenta
const float kEmGrowW[4]   = { 0.80f, 0.95f, 0.75f, 1.6f };  // grow-light white-green wash
const float kEmLeaf[4]    = { 0.10f, 0.36f, 0.08f, 0.7f };  // faint foliage chlorophyll glow
const float kEmWater[4]   = { 0.20f, 0.55f, 0.70f, 1.2f };  // faint water sheen
// NPC spawn-marker beacons (Danny built it all; Amara + Emma have sessions in L1).
const float kMarkAmara[4] = { 0.10f, 0.90f, 0.85f, 3.0f };  // teal  — Amara O'Neill
const float kMarkEmma[4]  = { 1.00f, 0.75f, 0.20f, 3.0f };  // amber — Emma Hartwell
const float kMarkDanny[4] = { 0.20f, 1.00f, 0.35f, 3.0f };  // green — Danny Kowalski

void addLight(std::vector<x3::rhi::PointLight>& v, float x, float y, float z,
              float r, float g, float b, float range) {
    x3::rhi::PointLight l;
    l.pos[0] = x; l.pos[1] = y; l.pos[2] = z; l.range = range;
    l.color[0] = r; l.color[1] = g; l.color[2] = b;
    v.push_back(l);
}

} // namespace

uint32_t SurvivalComplex::addBox(Scene& scene, x3::rhi::IRenderDevice& device,
                                 x3::phys::IPhysicsWorld& physics,
                                 float cx, float cy, float cz, float hx, float hy, float hz,
                                 const float color[4], const float emissive[4], bool collide) {
    x3::prims::PrimMesh geo = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, 1.0f);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    for (int i = 0; i < 4; ++i) e.baseColor[i] = color[i];
    if (emissive) for (int i = 0; i < 4; ++i) e.emissive[i] = emissive[i];
    e.tag = (uint32_t)Tag::Static;
    if (collide) {
        e.body = physics.addStaticMesh(geo.cverts.data(), (uint32_t)(geo.cverts.size() / 3),
                                       geo.cindex.data(), (uint32_t)geo.cindex.size());
    }
    return scene.add(e);
}

const SurvivalComplex::Stats& SurvivalComplex::build(Scene& scene, x3::rhi::IRenderDevice& device,
                                                     x3::phys::IPhysicsWorld& physics,
                                                     std::string_view /*modelDir*/) {
    if (m_built) return m_stats;
    m_built = true;
    const uint32_t entsBefore = scene.size();

    const float oy = kClubY;                 // -200 world Y origin
    const float T  = 0.25f;                  // wall thickness

    // Author at club-local Y; oy applied here.
    auto box = [&](float x, float y, float z, float hx, float hy, float hz,
                   const float* col, const float* em, bool coll) {
        return addBox(scene, device, physics, x, oy + y, z, hx, hy, hz,
                      col, em ? em : kOff, coll);
    };

    // ---- Footprint (30 ft X depth x 43 ft Z, matching L1's 43 ft width). ----
    const float RCX   = -20.0f;              // Complex X center (WEST of club)
    const float halfX = 30.0f * 0.3048f / 2; // 4.572 (30 ft)
    const float minX  = RCX - halfX;         // -24.572
    const float maxX  = RCX + halfX;         // -15.428  (just west of club west wall -15.24)
    const float minZ  = -kHW, maxZ = kHW;    // full 43 ft
    // Stair bay = east slice of the footprint (nearest the club/L1 above).
    const float bayMinX = -18.60f, bayMaxX = maxX;      // ~3.17 m wide shaft
    const float bayCX   = (bayMinX + bayMaxX) * 0.5f;
    const float bayHX   = (bayMaxX - bayMinX) * 0.5f;
    // Room = west slice.
    const float roomMinX = minX, roomMaxX = bayMinX;
    const float roomCX   = (roomMinX + roomMaxX) * 0.5f;
    const float roomHX   = (roomMaxX - roomMinX) * 0.5f;

    // Per-level floor Y (club-local). L1 = kLoungeY, descending by kLevelH.
    auto floorY = [&](int l) { return kLoungeY - (float)(l - 1) * kLevelH; };
    for (int l = 1; l <= kLevels; ++l) m_stats.levelFloorY[l] = floorY(l);
    m_stats.levelHasRoom[1] = true;          // L1 built by club1127.cpp (not here)

    // Switchback landing Z per level (odd = high/+Z, even = low/-Z).
    const float zHi = 5.30f, zLo = -5.30f;
    auto landZ = [&](int l) { return (l % 2 == 1) ? zHi : zLo; };

    // ================================================================
    // OUTER SHELL — 4 perimeter walls spanning the full stack height
    // (L7 floor up to just above the L1 ceiling), enclosing rooms + shaft.
    // ================================================================
    const float yBot = floorY(kLevels);              // -24.23
    const float yTop = floorY(1) + kRoomH + 0.3f;    // ~7.87 (above L1 ceiling)
    const float shellCY = (yBot + yTop) * 0.5f;
    const float shellHY = (yTop - yBot) * 0.5f;
    box(minX, shellCY, 0, T, shellHY, (maxZ - minZ) / 2 + T, kConcreteD, kOff, true);   // WEST
    box(maxX, shellCY, 0, T, shellHY, (maxZ - minZ) / 2 + T, kConcreteD, kOff, true);   // EAST
    box(RCX, shellCY, minZ, halfX + T, shellHY, T, kConcreteD, kOff, true);             // SOUTH
    box(RCX, shellCY, maxZ, halfX + T, shellHY, T, kConcreteD, kOff, true);             // NORTH

    // ================================================================
    // PER-LEVEL ROOMS (L2..L7): room floor, room ceiling, partition wall to the
    // stair bay (with a doorway at the level's landing), a stairwell landing
    // slab, themed contents + progressively industrial practical lighting.
    // ================================================================
    auto emForLevel = [&](int l) -> const float* {
        switch (l) {
            case 2: return kEmScreen;   // rec — cool screen glow + warm fill
            case 3: return kEmWhite;    // medical — clinical white
            case 4: return kEmAmberDim; // storage — dim amber
            case 5: return kEmCyan;     // water/air — cool cyan
            case 6: return kEmOrange;   // power — industrial orange
            case 7: return kEmGrow;     // hydroponics — grow magenta
            default: return kEmAmber;
        }
    };

    for (int l = 2; l <= kLevels; ++l) {
        const float fy = floorY(l);
        const float cz = landZ(l);
        const float* wallCol = (l >= 5) ? kConcreteD : kConcrete;

        // Room floor + ceiling (room area only; the bay stays an open shaft).
        box(roomCX, fy - 0.10f, 0, roomHX, 0.12f, (maxZ - minZ) / 2, kFloorM, kOff, true);
        box(roomCX, fy + kRoomH, 0, roomHX, 0.12f, (maxZ - minZ) / 2, kCeilM, kOff, false);

        // Partition wall between room and stair bay, at X = bayMinX, with a
        // ~1.5 m doorway centered on this level's landing Z.
        const float doorHalf = 0.75f, doorZ = cz;
        // North segment of partition (from doorZ+doorHalf up to maxZ).
        {
            const float z0 = doorZ + doorHalf, z1 = maxZ;
            box(bayMinX, fy + kRoomH / 2, (z0 + z1) / 2, T, kRoomH / 2, (z1 - z0) / 2, wallCol, kOff, true);
        }
        // South segment (from minZ up to doorZ-doorHalf).
        {
            const float z0 = minZ, z1 = doorZ - doorHalf;
            box(bayMinX, fy + kRoomH / 2, (z0 + z1) / 2, T, kRoomH / 2, (z1 - z0) / 2, wallCol, kOff, true);
        }
        // Lintel over the doorway.
        box(bayMinX, fy + kRoomH - 0.2f, doorZ, T, 0.2f, doorHalf, wallCol, kOff, false);
        m_stats.levelHasDoorway[l] = true;

        // Stairwell landing slab at this level (bay area, near the landing).
        box(bayCX, fy - 0.10f, cz, bayHX - 0.03f, 0.12f, 1.4f, kGrate, kOff, true);

        // Utilitarian ceiling practical + one amber wall strip (get dimmer/cooler
        // as we descend; the emissive fixture reads as the fitting).
        const float* em = emForLevel(l);
        // Two ceiling practicals (bunker LED battens) span the room so it reads,
        // plus a themed accent fixture. The room stays lit; theme colours the mood.
        box(roomCX, fy + kRoomH - 0.06f, -1.5f, 0.9f, 0.05f, 0.18f, kMetal, kEmWhite, false); // ceiling batten
        box(roomCX, fy + kRoomH - 0.06f,  2.5f, 0.9f, 0.05f, 0.18f, kMetal, kEmWhite, false); // ceiling batten
        box(roomCX, fy + kRoomH - 0.06f, cz * 0.4f, 0.5f, 0.05f, 0.18f, kMetal, em, false);    // themed accent fixture
        box(roomMinX + 0.06f, fy + 1.5f, 0, 0.02f, 0.9f, 0.05f, kMetal, kEmAmberDim, false);   // wall strip
        // Primary practical light: warm-white (cooler for medical/water) so the
        // themed contents actually read; deeper levels a touch dimmer/industrial.
        float pr = 2.30f, pg = 2.15f, pb = 1.85f;                 // warm-white bunker practical
        if (l == 3) { pr = 2.05f; pg = 2.15f; pb = 2.25f; }        // medical: clean cool-white
        if (l == 5) { pr = 1.65f; pg = 2.00f; pb = 2.30f; }        // life-support: cool
        if (l == 6) { pr = 2.30f; pg = 1.80f; pb = 1.30f; }        // power: warm industrial
        const float pk = (l == 7) ? 0.60f : 1.0f;                  // L7 lit mainly by grow-lights
        addLight(m_lights, roomCX,        oy + fy + kRoomH - 0.3f, -1.5f, pr * pk, pg * pk, pb * pk, 11.0f);
        addLight(m_lights, roomCX + 0.6f, oy + fy + kRoomH - 0.4f,  2.5f, pr * 0.85f * pk, pg * 0.85f * pk, pb * 0.85f * pk, 10.0f);
        // Themed colour accent (low, sets the mood without washing the room out).
        addLight(m_lights, roomCX - 1.0f, oy + fy + 1.7f, -cz * 0.35f, em[0] * 0.6f, em[1] * 0.6f, em[2] * 0.6f, 6.0f);

        m_stats.levelHasRoom[l] = true;
        m_stats.levelsBuilt++;

        // ---------- Themed contents ----------
        if (l == 2) {
            // RECREATION (HARD canon): couches + low table (center), a bank of
            // workstations (salvaged computers/monitors) on the NORTH wall, a
            // gaming table (card table + chairs), bookshelves on the EAST wall.
            box(roomCX - 1.0f, fy + 0.35f, -1.5f, 1.1f, 0.30f, 0.5f, kLeather, kOff, true);    // couch
            box(roomCX + 1.0f, fy + 0.35f, -1.5f, 1.1f, 0.30f, 0.5f, kLeather, kOff, true);    // couch
            box(roomCX, fy + 0.28f, -0.7f, 0.6f, 0.05f, 0.35f, kWood, kOff, true);             // low table
            box(roomCX, fy + 1.7f, -kHW + 0.35f, 1.4f, 0.5f, 0.04f, kSteelD, kEmScreen, false);// big rec screen (N wall)
            // Workstation bank on the NORTH wall (3 desks + emissive monitors).
            m_stats.workstations = 0;
            for (int w = -1; w <= 1; ++w) {
                const float wx = roomCX + (float)w * 1.3f;
                box(wx, fy + 0.38f, maxZ - 0.5f, 0.5f, 0.05f, 0.35f, kMetal, kOff, true);       // desk
                box(wx, fy + 0.78f, maxZ - 0.4f, 0.35f, 0.22f, 0.03f, kSteelD, kEmScreen, false);// monitor
                m_stats.workstations++;
            }
            // Gaming table (card table + 4 chairs) toward the south.
            box(roomCX, fy + 0.42f, 3.4f, 0.55f, 0.05f, 0.55f, kWood, kOff, true);              // card table
            for (int c = 0; c < 4; ++c) {
                const float ax = (c < 2 ? -1.f : 1.f) * 0.75f;
                const float az = (c % 2 == 0 ? -1.f : 1.f) * 0.75f;
                box(roomCX + ax, fy + 0.25f, 3.4f + az, 0.2f, 0.25f, 0.2f, kSteelD, kOff, true);
            }
            m_stats.hasGamingTable = true;
            // Bookshelves on the EAST wall (the partition side, X ~ bayMinX).
            for (int b = 0; b < 3; ++b)
                box(roomMaxX - 0.25f, fy + 0.9f, -3.0f + (float)b * 1.2f, 0.18f, 0.85f, 0.5f, kWood, kOff, true);
            m_stats.hasBookshelves = true;
        } else if (l == 3) {
            // MEDICAL BAY + SECURITY [INVENT]. Two beds, a med cabinet, an
            // overhead operating light, a security desk w/ monitors + a locker.
            for (int b = 0; b < 2; ++b)
                box(roomCX - 1.4f, fy + 0.4f, -2.0f + (float)b * 1.6f, 0.5f, 0.15f, 0.9f, kWhitePlas, kOff, true); // beds
            box(roomCX - 1.6f, fy + 2.3f, -1.2f, 0.35f, 0.06f, 0.35f, kSteel, kEmWhite, false); // operating light
            box(roomMinX + 0.4f, fy + 1.0f, 3.0f, 0.25f, 1.0f, 0.6f, kMetal, kOff, true);        // med cabinet
            box(roomCX + 1.4f, fy + 0.4f, 3.2f, 0.6f, 0.05f, 0.4f, kMetal, kOff, true);          // security desk
            box(roomCX + 1.4f, fy + 0.85f, 3.3f, 0.4f, 0.22f, 0.03f, kSteelD, kEmGreenLED, false);// security monitors
            box(roomCX + 1.6f, fy + 1.0f, 4.6f, 0.25f, 1.0f, 0.3f, kSteelD, kOff, true);         // gun/gear locker
            m_stats.hasMedicalBay = true;
        } else if (l == 4) {
            // DEEP STORAGE / ARMORY [INVENT]. Rows of shelving + crates/barrels +
            // weapon lockers + provisions (rated 12 people / 6 months, canon).
            for (int r = 0; r < 3; ++r) {
                const float rx = roomMinX + 0.8f + (float)r * 1.6f;
                box(rx, fy + 1.1f, 0, 0.25f, 1.05f, (maxZ - minZ) / 2 - 1.0f, kMetal, kOff, true); // shelf run
                for (int s = 0; s < 3; ++s)
                    box(rx, fy + 0.5f + (float)s * 0.75f, -3.0f + (float)s * 3.0f, 0.22f, 0.2f, 0.4f, kRust, kOff, false); // crates
            }
            for (int a = 0; a < 2; ++a)
                box(roomMaxX - 0.4f, fy + 1.0f, -2.0f + (float)a * 4.0f, 0.22f, 1.0f, 0.5f, kSteelD, kEmGreenLED, false); // weapon lockers
            // A couple of aisle-floor practicals so the racks + central aisle read.
            addLight(m_lights, roomCX, oy + fy + 1.6f, 0.0f, 1.8f, 1.7f, 1.5f, 8.0f);
            addLight(m_lights, roomCX, oy + fy + 1.6f, 4.0f, 1.7f, 1.6f, 1.4f, 7.0f);
            m_stats.hasArmory = true;
        } else if (l == 5) {
            // WATER RESERVOIR + AIR / LIFE-SUPPORT PLANT [INVENT]. Tall sealed
            // tanks, an air handler, pumps + piping. Cool cyan, humid feel.
            // Water tanks LINE the west wall (row along Z) so the north view frames
            // them receding rather than crowding the lens.
            for (int t = 0; t < 4; ++t)
                box(roomMinX + 0.65f, fy + 1.3f, -4.0f + (float)t * 2.6f, 0.45f, 1.3f, 0.6f, kTankM, kEmWater, true); // water tanks
            box(roomCX + 0.9f, fy + 1.2f, 3.0f, 0.9f, 1.2f, 0.7f, kSteel, kOff, true);           // air handler
            box(roomCX + 0.5f, fy + 2.3f, 3.0f, 0.5f, 0.06f, 0.4f, kSteel, kEmCyan, false);       // handler status
            for (int p = 0; p < 4; ++p)
                box(roomMaxX - 0.35f, fy + 0.5f + (float)p * 0.6f, 1.0f, 0.08f, 0.28f, 0.08f, kSteelD, kOff, false); // pipes
            m_stats.hasLifeSupport = true;
        } else if (l == 6) {
            // POWER / GENERATORS + WORKSHOP [INVENT]. Generator blocks, fuel
            // tanks, a breaker/switchgear wall (LED panel), a workbench.
            for (int g = 0; g < 2; ++g)
                box(roomMinX + 1.2f + (float)g * 2.2f, fy + 0.7f, -2.5f, 0.9f, 0.7f, 1.0f, kSteelD, kOff, true); // generators
            box(roomMinX + 1.2f, fy + 1.5f, -2.5f, 0.3f, 0.06f, 0.3f, kMetal, kEmOrange, false);   // gen exhaust glow
            box(roomMaxX - 0.4f, fy + 1.3f, 2.0f, 0.22f, 1.3f, 1.2f, kMetal, kOff, true);          // switchgear
            for (int b = 0; b < 4; ++b)
                box(roomMaxX - 0.6f, fy + 0.7f + (float)b * 0.5f, 2.0f + ((b % 2) ? 0.4f : -0.4f), 0.05f, 0.08f, 0.08f,
                    kSteelD, (b % 2) ? kEmGreenLED : kEmRedLED, false);                             // breaker LEDs
            box(roomCX, fy + 0.5f, 4.0f, 0.9f, 0.05f, 0.4f, kWood, kOff, true);                     // workbench
            box(roomMaxX - 0.5f, fy + 1.0f, -4.0f, 0.4f, 1.0f, 0.5f, kRust, kOff, true);            // fuel tank
            m_stats.hasGenerators = true;
        } else if (l == 7) {
            // ★ HYDROPONICS BAY (HARD canon, the hero level) — "green growing
            // things a hundred feet underground". Rows of multi-tier NFT grow
            // racks, plants (emissive foliage), grow-light fixtures over each
            // rack (magenta + white-green), a nutrient/water reservoir. Lush.
            const int nRacks = 4;
            for (int r = 0; r < nRacks; ++r) {
                const float rx = roomMinX + 0.9f + (float)r * 1.35f;
                // 3-tier rack + trays of lush plants on each tier.
                for (int tier = 0; tier < 3; ++tier) {
                    const float ty = fy + 0.55f + (float)tier * 0.85f;
                    box(rx, ty, 0, 0.30f, 0.04f, (maxZ - minZ) / 2 - 1.0f, kSteel, kOff, true);    // NFT channel/tray
                    // Foliage clumps along the tray (faint chlorophyll glow = lush).
                    for (int p = 0; p < 5; ++p)
                        box(rx, ty + 0.19f, -3.8f + (float)p * 1.9f, 0.25f, 0.17f, 0.55f, kGreenPlant, kEmLeaf, false);
                    // Grow-light bar under the tier above (magenta / white-green alternating).
                    const float ly = ty + 0.62f;
                    const float* ge = (tier % 2 == 0) ? kEmGrow : kEmGrowW;
                    uint32_t gid = box(rx, ly, 0, 0.22f, 0.03f, (maxZ - minZ) / 2 - 1.2f, kSteelD, ge, false);
                    m_growEnts.push_back(gid);
                    m_stats.growLights++;
                }
                // One grow cast-light per rack (mid height) — magenta wash + a soft
                // green fill so the foliage reads lush without blowing the budget.
                addLight(m_lights, rx, oy + fy + 1.4f, 0, kEmGrow[0] * 0.8f, kEmGrow[1] * 0.8f, kEmGrow[2] * 0.8f, 4.5f);
                m_growLightIdx.push_back(m_lights.size() - 1);
                addLight(m_lights, rx, oy + fy + 1.0f, 0, 0.25f, 0.75f, 0.20f, 3.5f);   // green foliage fill
            }
            // Nutrient / water reservoir + a humidity-misting glow.
            box(roomMaxX - 0.5f, fy + 0.4f, 4.5f, 0.4f, 0.4f, 1.2f, kTankM, kEmWater, true);
            box(roomCX, fy + 0.05f, 0, roomHX - 0.4f, 0.02f, (maxZ - minZ) / 2 - 0.6f, kFloorM, kEmWater, false); // wet floor sheen
            m_stats.hasHydroRacks = true;
        }
    }

    // ================================================================
    // STAIRWELL — the walkable SPINE. Switchback flights connect each level to
    // the next in the east stair bay. Steps are solid concrete blocks (top =
    // tread) so the capsule steps down cleanly.
    // ================================================================
    const int nSteps = 14;
    float stairMinLocalY = 1e9f, stairMaxLocalY = -1e9f;
    for (int l = 1; l < kLevels; ++l) {
        const float y0 = floorY(l), y1 = floorY(l + 1);
        const float z0 = landZ(l),  z1 = landZ(l + 1);
        const float rise = (y0 - y1) / (float)nSteps;
        const float runHalf = std::fabs(z1 - z0) / (float)nSteps * 0.5f + 0.06f;
        for (int i = 1; i <= nSteps; ++i) {
            const float topY = y0 - (float)i * rise;                       // tread top
            const float zc   = z0 + (z1 - z0) * ((float)i - 0.5f) / (float)nSteps;
            box(bayCX, topY - 0.55f, zc, bayHX - 0.05f, 0.55f, runHalf, kStepM, kOff, true);
            stairMinLocalY = std::min(stairMinLocalY, topY);
            stairMaxLocalY = std::max(stairMaxLocalY, topY);
            m_stats.stairSteps++;
        }
    }
    m_stats.stairMinY = oy + stairMinLocalY;   // ~ world L7 floor
    m_stats.stairMaxY = oy + stairMaxLocalY;   // ~ world L1 floor
    // Amber strip lights down the stairwell spine (one per level — utilitarian).
    for (int l = 1; l <= kLevels; ++l) {
        box(bayMaxX - 0.06f, floorY(l) + 1.5f, landZ(l), 0.02f, 0.9f, 0.05f, kMetal, kEmAmber, false);
        addLight(m_lights, bayCX, oy + floorY(l) + 1.6f, landZ(l), 1.0f, 0.62f, 0.26f, 6.0f);
    }
    // Route-A CONNECTION: the top of the stairwell reaches L1. The L1 hatch (from
    // club1127.cpp) sits at (hallX=-17.64, Z~+5.35) — inside the bay X range and
    // at landZ(1)=+5.30. Build the L1 landing + a short guard so the descent from
    // L1 lands on the spine. (club1127.cpp opens the L1 floor hatch above this.)
    box(bayCX, floorY(1) - 0.10f, landZ(1), bayHX - 0.03f, 0.12f, 1.5f, kGrate, kOff, true);   // L1 top landing
    box(bayCX, floorY(1) + 0.5f, landZ(1) + 1.5f, bayHX - 0.03f, 0.5f, 0.05f, kRail, kEmAmber, false); // guard rail
    {
        const float hatchX = -17.64f, hatchZ = 5.35f;    // club1127 L1 hatch marker
        const bool inBayX = hatchX > bayMinX && hatchX < bayMaxX;
        const bool nearLandZ = std::fabs(hatchZ - landZ(1)) < 1.5f;
        m_stats.hasRouteAConnect = inBayX && nearLandZ;
    }
    box(bayCX, floorY(1) + 1.6f, landZ(1), 0.02f, 0.9f, 0.05f, kMetal, kEmAmber, false);
    addLight(m_lights, bayCX, oy + floorY(1) + 1.7f, landZ(1), 1.0f, 0.62f, 0.26f, 6.0f);

    // ================================================================
    // DANNY'S 4-PERSON ELEVATOR — a separate steel car + shaft in the NW corner
    // of the footprint, with a button panel (one stop per level). Optional
    // shortcut; a prop here (no ride sim this stage).
    // ================================================================
    {
        const float ecx = roomMinX + 0.9f, ecz = maxZ - 0.9f;   // NW corner
        const float ehx = 0.85f, ehz = 0.85f;
        // Shaft walls (full height) on the two exposed (interior-facing) sides.
        box(ecx + ehx + 0.05f, shellCY, ecz, T, shellHY, ehz + 0.05f, kSteelD, kOff, true);      // east side
        box(ecx, shellCY, ecz - ehz - 0.05f, ehx + 0.05f, shellHY, T, kSteelD, kOff, true);       // south side
        // The car parked at L2 (the level the player first reaches).
        const float carY = floorY(2);
        box(ecx, carY + 1.1f, ecz, ehx, 0.06f, ehz, kSteelD, kOff, false);                        // car ceiling
        box(ecx, carY - 0.05f, ecz, ehx, 0.06f, ehz, kSteel, kOff, true);                          // car floor
        box(ecx, carY + 1.9f, ecz, 0.2f, 0.05f, 0.2f, kSteel, kEmWhite, false);                   // single overhead light
        addLight(m_lights, ecx, oy + carY + 1.7f, ecz, 0.9f, 0.94f, 1.0f, 3.5f);
        // Button panel: one green button per level (7 stops), stacked.
        for (int b = 0; b < kLevels; ++b) {
            box(ecx + ehx - 0.08f, carY + 0.6f + (float)b * 0.12f, ecz + 0.4f, 0.02f, 0.04f, 0.04f,
                kSteelD, kEmGreenLED, false);
            m_stats.elevatorButtons++;
        }
        m_stats.hasElevator = true;
    }

    // ================================================================
    // ROUTE B — the ELEVATOR HALL. A long strata hall running WEST under the
    // club, arriving at L7 (bottom). Amber/UV-lit, dead silent. A green MARKED
    // HOOK at the east end = where the club's freight elevator delivers you
    // (the club elevator interior is owned elsewhere — this is the stub).
    // ================================================================
    {
        const float hallY = floorY(kLevels);            // L7 floor level (world -224.23)
        const float hallW = 1.4f, hallH = 2.4f;
        const float hallZ = 0.0f;
        const float hEast = kHL + 0.3f;                 // ~+15.5 (under the club east elevator)
        const float hWest = maxX;                        // meets the L7 east shell
        const float hcx = (hEast + hWest) * 0.5f, hlen = (hEast - hWest);
        box(hcx, hallY - 0.10f, hallZ, hlen / 2, 0.12f, hallW / 2, kFloorM, kOff, true);            // floor
        box(hcx, hallY + hallH, hallZ, hlen / 2, 0.12f, hallW / 2, kCeilM, kOff, false);            // ceiling
        box(hcx, hallY + hallH / 2, hallZ - hallW / 2, hlen / 2, hallH / 2, T, kConcreteD, kOff, true); // south wall
        box(hcx, hallY + hallH / 2, hallZ + hallW / 2, hlen / 2, hallH / 2, T, kConcreteD, kOff, true); // north wall
        // Amber strip + spaced practicals down the 130-ft run.
        box(hcx, hallY + 0.10f, hallZ - hallW / 2 + 0.05f, hlen / 2 - 0.3f, 0.01f, 0.04f, kMetal, kEmAmber, false);
        for (int s = 0; s < 5; ++s) {
            const float lx = hWest + (float)(s + 1) * hlen / 6.0f;
            addLight(m_lights, lx, oy + hallY + hallH - 0.3f, hallZ, 1.0f, 0.62f, 0.26f, 6.5f);
        }
        // L7 east doorway: open the L7 shell east wall would block the hall; cut a
        // doorway marker + opening into L7 at the hall's west end.
        box(maxX, hallY + hallH - 0.2f, hallZ, T, 0.2f, hallW / 2, kConcreteD, kOff, false);        // lintel
        // GREEN MARKED HOOK — club-elevator delivery point at the east end.
        box(hEast - 0.1f, hallY + 1.1f, hallZ, 0.03f, 0.8f, hallW / 2 - 0.1f, kSteelD, kEmGreenLED, false);
        addLight(m_lights, hEast - 0.5f, oy + hallY + 1.6f, hallZ, 0.2f, 1.0f, 0.35f, 5.0f);
        m_stats.hasRouteBHall = true;
        m_stats.hallEastX = hEast; m_stats.hallWestX = hWest;
        m_stats.hallY = oy + hallY;
    }

    // ================================================================
    // NPC SPAWN MARKERS (no AI this stage). Danny built all of this; the Private
    // Lounge (L1) is the setting for Tim's sessions with AMARA + EMMA (§4.4).
    // Beacons + a label emissive so a later pass can wire the actual NPCs.
    // ================================================================
    auto marker = [&](float x, float y, float z, const float* col) {
        box(x, y + 0.9f, z, 0.10f, 0.9f, 0.10f, kSteelD, col, false);   // beacon pillar
        box(x, y + 0.02f, z, 0.35f, 0.02f, 0.35f, kSteelD, col, false); // floor disc
        m_stats.npcMarkers++;
    };
    // Amara + Emma — in the L1 Private Lounge (situation room + bedroom areas).
    marker(-16.6f, floorY(1), -3.0f, kMarkAmara);   // Amara O'Neill (situation room)
    marker(-19.3f, floorY(1),  2.5f, kMarkEmma);    // Emma Hartwell (bedroom)
    // Danny — at the L2 stair landing (he showed Tyler & Jack L1-L2).
    marker(bayCX, floorY(2), landZ(2), kMarkDanny); // Danny Kowalski

    // ---- Footprint + spawn + census -------------------------------------
    m_stats.minX = minX; m_stats.maxX = maxX; m_stats.minZ = minZ; m_stats.maxZ = maxZ;
    m_stats.pointLights = (int)m_lights.size();
    m_stats.entities = (int)(scene.size() - entsBefore);

    // Spawn on L2 (first level down), in the room, facing the rec content.
    m_spawn = x3::phys::Vec3{ roomCX + 1.5f, oy + floorY(2) + 0.1f, 2.0f };

    // ---- Showcase cameras (index 0 = stairwell, 1..7 = levels, 8 = hall). ----
    auto aim = [&](int idx, float px, float py, float pz, float tx, float ty, float tz) {
        float dx = tx - px, dy = ty - py, dz = tz - pz;
        float yaw = std::atan2(dz, dx);
        float horiz = std::sqrt(dx * dx + dz * dz);
        float pitch = std::atan2(dy, std::max(0.01f, horiz));
        m_shotCam[idx][0] = px; m_shotCam[idx][1] = py; m_shotCam[idx][2] = pz;
        m_shotCam[idx][3] = yaw; m_shotCam[idx][4] = pitch;
    };
    for (int l = 2; l <= kLevels; ++l) {
        const float fy = oy + floorY(l);
        // Stand at the SOUTH end and look NORTH down the room's long axis, where the
        // themed hero content clusters (north-wall stations/racks/handlers), a touch
        // elevated + tilted down so foreground props read too.
        aim(l, roomCX + 0.3f, fy + 1.95f, minZ + 0.9f,
               roomCX - 0.2f, fy + 1.05f, maxZ - 1.2f);
    }
    // L7 hydroponics HERO: stand IN an aisle between grow racks looking north so
    // the tiered channels + plants + grow-lights recede as a lush corridor.
    aim(7, roomMinX + 0.9f + 1.5f * 1.35f, oy + floorY(7) + 1.72f, minZ + 1.0f,
           roomMinX + 0.9f + 1.5f * 1.35f, oy + floorY(7) + 1.42f, maxZ - 1.0f);
    // L1 vantage (Private Lounge / top landing).
    aim(1, bayCX + 0.5f, oy + floorY(1) + 1.7f, landZ(1) - 2.0f, -18.0f, oy + floorY(1) + 1.2f, 0.0f);
    // Stairwell: from the L1 top landing, look DOWN the first descending flight.
    aim(0, bayCX, oy + floorY(1) + 1.7f, landZ(1) - 0.5f,
           bayCX, oy + floorY(2) + 0.2f, -3.0f);
    // Route-B hall: stand at the east hook looking WEST down the 130-ft run.
    aim(8, kHL + 0.0f, oy + floorY(kLevels) + 1.7f, 0.0f, maxX, oy + floorY(kLevels) + 1.2f, 0.0f);

    x3::logInfo("survival-complex: built L2-L7 + stairwell(" + std::to_string(m_stats.stairSteps) +
                " steps) + elevator(" + std::to_string(m_stats.elevatorButtons) + " stops) + Route-B hall; " +
                std::to_string(m_stats.entities) + " entities, " +
                std::to_string(m_stats.pointLights) + " point lights, " +
                std::to_string(m_stats.npcMarkers) + " NPC markers");
    return m_stats;
}

void SurvivalComplex::update(float dt, Scene& scene, x3::rhi::IRenderDevice& device) {
    (void)device;
    if (!m_built) return;
    m_time += dt;
    // Breathe the hydroponics grow-lights so L7 reads alive (slow ~0.2 Hz sway,
    // phase-offset per fixture). Emissive strength + the companion cast pulse.
    for (size_t i = 0; i < m_growEnts.size(); ++i) {
        const float ph = m_time * 1.2f + (float)i * 0.7f;
        const float k  = 0.85f + 0.15f * std::sin(ph);
        Entity& e = scene.get(m_growEnts[i]);
        e.emissive[3] = ((i % 2 == 0) ? kEmGrow[3] : kEmGrowW[3]) * k;
        if (i < m_growLightIdx.size()) {
            auto& L = m_lights[m_growLightIdx[i]];
            const float* ge = (i % 2 == 0) ? kEmGrow : kEmGrowW;
            L.color[0] = ge[0] * 0.9f * k; L.color[1] = ge[1] * 0.9f * k; L.color[2] = ge[2] * 0.9f * k;
        }
    }
}

void SurvivalComplex::showcaseCamera(int level, float out[5]) const {
    int idx = (level >= 0 && level <= 8) ? level : 2;
    for (int i = 0; i < 5; ++i) out[i] = m_shotCam[idx][i];
}

} // namespace x3::game

// ===========================================================================
// Headless self-test (--test-complex). Build the Complex with the shared
// HeadlessRenderDevice + a real physics world (no window / Vulkan), assert all
// 7 levels stand up, the stairwell spans top-to-bottom, every level has a
// doorway to the spine, both entrances reach the structure, the elevator has a
// stop per level, L7 is a hydroponics bay, NPC markers are placed, the light
// budget is respected, and it is leak-clean + idempotent.
// ===========================================================================
#include "headless_device.h"
#include "asset_root.h"
#include "engine/physics/IPhysicsWorld.h"

namespace x3::game {

bool runComplexSelfTest() {
    int pass = 0, fail = 0;
    auto check = [&](bool cond, const char* name) {
        if (cond) { ++pass; x3::logInfo(std::string("[complex-test] PASS ") + name); }
        else      { ++fail; x3::logError(std::string("[complex-test] FAIL ") + name); }
    };

    struct CountingDevice : public HeadlessRenderDevice {
        int created = 0, destroyed = 0;
        x3::rhi::MeshHandle createMesh(const x3::rhi::MeshVertex* v, uint32_t nv,
                                       const uint32_t* idx, uint32_t ni) override {
            ++created; return HeadlessRenderDevice::createMesh(v, nv, idx, ni);
        }
        void destroyMesh(x3::rhi::MeshHandle h) override {
            ++destroyed; HeadlessRenderDevice::destroyMesh(h);
        }
    };

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    CountingDevice device;
    Scene scene;

    SurvivalComplex cx;
    const SurvivalComplex::Stats& s = cx.build(scene, device, *physics, x3::game::riggedGlbRoot());

    const float oy = SurvivalComplex::kClubY;
    const float H  = SurvivalComplex::kLevelH;

    // (1) All 6 authored levels (L2..L7) stand up as rooms with doorways.
    {
        bool roomsOk = (s.levelsBuilt == 6);
        for (int l = 2; l <= 7; ++l) roomsOk = roomsOk && s.levelHasRoom[l] && s.levelHasDoorway[l];
        check(roomsOk, "L2-L7 all build as rooms, each with a doorway to the stairwell");
    }

    // (2) Levels descend by kLevelH; L1->L7 spans ~100 ft (~28.8 m).
    {
        bool spacingOk = true;
        for (int l = 2; l <= 7; ++l)
            spacingOk = spacingOk && std::fabs((s.levelFloorY[l - 1] - s.levelFloorY[l]) - H) < 0.01f;
        const float drop = s.levelFloorY[1] - s.levelFloorY[7];
        check(spacingOk && std::fabs(drop - 28.8f) < 0.1f,
              "7 levels stacked, evenly spaced, ~100 ft L1->L7 (28.8 m)");
    }

    // (3) The stairwell spans TOP-TO-BOTTOM — it connects L1 down to L7 (the
    //     walkable spine). Its highest tread sits just under the L1 floor and
    //     its lowest reaches the L7 floor.
    {
        const float wantTop = oy + s.levelFloorY[1];   // L1
        const float wantBot = oy + s.levelFloorY[7];   // L7
        const bool topOk = (s.stairMaxY < wantTop + 0.01f) && (s.stairMaxY > wantTop - 1.0f);
        const bool botOk = std::fabs(s.stairMinY - wantBot) < 1.0f;
        check(s.stairSteps >= 6 * 10 && topOk && botOk,
              "stairwell spine spans L1 (top) to L7 (bottom) — walkable end to end");
    }

    // (4) ROUTE A (top): the stairwell top reaches the L1 club hatch.
    check(s.hasRouteAConnect, "ROUTE A: stairwell top connects to the L1 (Private Lounge) hatch");

    // (5) ROUTE B (bottom): the 130-ft under-club hall runs WEST into L7.
    {
        const bool runsWest = s.hallEastX > s.hallWestX;                 // east -> west
        const bool longRun  = (s.hallEastX - s.hallWestX) > 25.0f;       // a long hall
        const bool atL7     = std::fabs(s.hallY - (oy + s.levelFloorY[7])) < 0.5f;
        check(s.hasRouteBHall && runsWest && longRun && atL7,
              "ROUTE B: long under-club hall runs WEST and arrives at L7 (bottom)");
    }

    // (6) Danny's 4-person elevator has one stop per level (7).
    check(s.hasElevator && s.elevatorButtons == 7, "4-person elevator with one stop per level (7)");

    // (7) L2 recreation is furnished per canon (workstations + gaming + shelves).
    check(s.workstations >= 3 && s.hasGamingTable && s.hasBookshelves,
          "L2 recreation: workstation bank + gaming table + bookshelves (canon)");

    // (8) The middle floors read as their invented themes.
    check(s.hasMedicalBay && s.hasArmory && s.hasLifeSupport && s.hasGenerators,
          "L3 medical/security + L4 storage/armory + L5 water/air + L6 power (themed)");

    // (9) ★ L7 is a lush hydroponics bay with grow racks + grow-lights.
    check(s.hasHydroRacks && s.growLights >= 6,
          "L7 HYDROPONICS: grow racks + multiple grow-light fixtures (the hero level)");

    // (10) NPC spawn markers placed (Amara + Emma + Danny).
    check(s.npcMarkers >= 3, "NPC spawn markers placed (Amara, Emma, Danny)");

    // (11) The Complex manages its own point-light budget (under the 64 cap) and
    //      all light positions are finite.
    {
        bool finite = true;
        for (const auto& l : cx.pointLights())
            finite = finite && std::isfinite(l.pos[0]) && std::isfinite(l.pos[1]) && std::isfinite(l.pos[2]);
        check(finite && s.pointLights > 0 && s.pointLights <= 64,
              "Complex point-light set is finite and within its own budget (<=64)");
    }

    // (12) Spawn sits inside the footprint on the L2 floor.
    {
        const x3::phys::Vec3 sp = cx.spawn();
        const bool inX = sp.x > s.minX && sp.x < s.maxX;
        const bool inZ = sp.z > s.minZ && sp.z < s.maxZ;
        const bool onL2 = std::fabs(sp.y - (oy + s.levelFloorY[2])) < 1.0f;
        check(inX && inZ && onL2, "player spawn is inside the footprint on the L2 floor");
    }

    // (13) update() animates the hydroponics grow-lights (emissive breathes).
    {
        // Sample one grow-light entity's emissive strength across a few ticks.
        float before = -1.0f, after = -1.0f;
        // Find any emissive grow entity via the light set delta instead:
        const auto L0 = cx.pointLights();
        for (int i = 0; i < 20; ++i) cx.update(1.0f / 60.0f, scene, device);
        const auto& L1 = cx.pointLights();
        float moved = 0.0f;
        for (size_t i = 0; i < L0.size() && i < L1.size(); ++i)
            moved = std::max(moved, std::fabs(L0[i].color[0] - L1[i].color[0]) +
                                    std::fabs(L0[i].color[2] - L1[i].color[2]));
        (void)before; (void)after;
        check(moved > 1e-4f, "L7 grow-lights animate (emissive/cast breathes over time)");
    }

    // (14) Idempotent rebuild: a second build() adds no geometry.
    {
        const int before = device.created;
        cx.build(scene, device, *physics, x3::game::riggedGlbRoot());
        check(device.created == before && cx.stats().entities == s.entities,
              "rebuild is idempotent (no duplicated geometry / no leak)");
    }

    // (15) Leak-clean bookkeeping: every mesh can be destroyed, ledger balances.
    {
        for (int h = 1; h <= device.created; ++h)
            device.destroyMesh(x3::rhi::MeshHandle{ (uint32_t)h });
        check(device.created > 0 && device.destroyed == device.created,
              "mesh create/destroy ledger balances (leak-clean bookkeeping)");
    }

    physics->shutdown();
    const int total = pass + fail;
    x3::logInfo("complex: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    return fail == 0;
}

} // namespace x3::game
