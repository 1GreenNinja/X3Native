// WAVE-3 ROOM-DRESSING RECIPES — see room_dressing.h. Dressing math follows the
// cell_dressing laws: 0.14 m inset vs graybox planes (0.2 m slabs centered), yaw-only
// prop placement, instance emissive[3] SCALES material emissive, contact shadows
// ground props, one key light per room, one accent hue per zone (ART_BIBLE §2/§3).
#include "room_dressing.h"
#include "asset_root.h"      // riggedGlbRoot() — the F2 rescue-captive character GLBs
#include "holo_terminal.h"   // bakeMedicalMonitor() — the F2 rescue-room dark-glass screen

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kInset  = 0.14f;   // wall/ceiling panel inset vs the graybox plane
constexpr float kFloorLift = 0.012f;

// ---- Zones (recipe ids). 0 = no recipe (room left graybox). -------------------------
enum Zone : uint8_t { ZNone = 0, ZHall, ZCorridor, ZWard, ZSecurity, ZLab, ZBoss,
                      ZLobby, ZStorage,
                      // W3-2 — the tower floors (ART_BIBLE §3 zone palettes):
                      ZMedical,    // F2 Medical Bay — clinical sickly-green
                      ZGenetics,   // F3 Genetics Lab — green pushed harder
                      ZCyber,      // F4 Cybernetics — dark steel + cyan instruments
                      ZDroneBay,   // F5 Drone Station — industrial hangar + amber caution
                      ZSalvari,    // F6 Salvari — darkest, alien, biolume green
                      ZExec,       // F7 Executive — clean dark luxury + brass (new bible entry)
                      ZCave,       // W5-1 F4.5 Nexus Chamber — fog/atmosphere ONLY (canon_45
                                   // hand-dresses the cavern; no panel recipe)
                      ZOrganic,    // W8-1 — the NON-platform "Cave Chamber" story rooms
                                   // (F3 Spawning Chamber, F6 Portal/Energy Nexus/First
                                   // Contact): black-brown organic base, minimal warm
                                   // practicals, biolume-green accent (ART_BIBLE §3
                                   // monster-space palette). Previously swallowed by the
                                   // fog-only ZCave rule and left bare graybox.
                      ZCount };

// ---- Converted-kit props (paths + probed AABBs, from the cell_dressing tables). -----
const char* kRelConsole = "ModularSciFi_Interior/SM_Console.glb";
const char* kRelPipes   = "ModularSciFi_Interior/SM_Pipes_A.glb";
const char* kRelCrateS  = "SciFi_Warehouse_Kit/Crate Short.glb";
const char* kRelCrateL  = "SciFi_Warehouse_Kit/Crate Long.glb";
const char* kRelBarrel  = "SciFi_Warehouse_Kit/Barrel.glb";
const char* kRelPallet  = "SciFi_Warehouse_Kit/Pallet.glb";
const char* kRelBin     = "SciFi_Warehouse_Kit/Garbage Bin.glb";
const char* kRelCot     = "Detention/SM_Hospital_Bed.glb";
// W8-1 — desc-gold kit expansion (paths + probed AABBs shared with cell_dressing;
// kDroneAabb probed via tools/glb_node_bounds.py).
const char* kRelFusebox = "SciFi_Warehouse_Kit/Fusebox 01.glb";
const char* kRelCam     = "SciFi_Warehouse_Kit/Security Cam.glb";
const char* kRelExting  = "SciFi_Warehouse_Kit/Fire Extinguisher.glb";
const char* kRelHang    = "SciFi_Warehouse_Kit/Hanging Light.glb";
const char* kRelDuct    = "SciFi_Warehouse_Kit/Duct Straight.glb";
const char* kRelVent    = "SciFi_Warehouse_Kit/Duct Vent.glb";
const char* kRelDrone   = "Characters/Drone.glb";

struct Aabb { float minx, miny, minz, maxx, maxy, maxz; };
constexpr Aabb kConsAabb   { -0.47f,  0.00f, -0.31f,  0.31f, 1.55f, 0.29f };
constexpr Aabb kPipesAabb  { -0.123f,-0.011f,  0.00f,  0.539f,0.164f,3.000f };
constexpr Aabb kCrateSAabb { -0.668f, 0.000f,-0.000f,  0.000f,0.600f,0.671f };
constexpr Aabb kCrateLAabb { -0.640f, 0.000f, 0.000f,  0.000f,0.600f,1.274f };
constexpr Aabb kBarrelAabb { -0.441f, 0.000f,-0.456f,  0.440f,1.225f,0.425f };
constexpr Aabb kPalletAabb { -1.559f, 0.000f,-0.005f,  0.003f,0.198f,1.519f };
constexpr Aabb kBinAabb    { -0.277f, 0.001f,-0.277f,  0.277f,1.061f,0.277f };
constexpr Aabb kCotAabb    { -1.200f, 0.100f,-0.600f,  1.100f,1.200f,0.600f };
constexpr Aabb kFuseAabb   { -0.329f,-0.936f,-0.245f, -0.144f,1.308f,0.396f };
constexpr Aabb kCamAabb    { -0.540f,-0.051f,-0.158f, -0.150f,0.209f,0.158f };
constexpr Aabb kExtAabb    { -0.394f, 0.000f,-0.184f, -0.151f,1.137f,0.184f };
constexpr Aabb kHangAabb   { -0.273f,-0.952f,-0.299f,  0.273f,0.000f,0.299f };  // hangs DOWN from maxy=0
constexpr Aabb kDuctAabb   { -0.335f,-0.337f,-0.000f,  0.335f,1.200f,6.000f };  // long duct, runs in Z
constexpr Aabb kVentAabb   { -0.327f,-0.327f, 0.000f,  0.327f,0.327f,0.999f };  // grate cube, depth in +Z
constexpr Aabb kDroneAabb  { -0.900f,-0.300f,-0.900f,  0.900f,0.300f,0.900f };  // 1.8 m quad drone, center origin
inline float acx(const Aabb& a) { return (a.minx + a.maxx) * 0.5f; }
inline float acy(const Aabb& a) { return (a.miny + a.maxy) * 0.5f; }
inline float acz(const Aabb& a) { return (a.minz + a.maxz) * 0.5f; }

// ---- Per-zone recipe: surfaces, palette, fog (ART_BIBLE §3 zone table). --------------
struct Recipe {
    const char* wall;   float wallTile;
    const char* floor;  float floorTile;
    const char* ceil;   float ceilTile;
    // key light (the ONE statement), fill (<= half), accent (the ONE hue)
    float keyR, keyG, keyB, keyRange;
    float accR, accG, accB, accRange;
    x3::rhi::IRenderDevice::FogParams fog;
};

x3::rhi::IRenderDevice::FogParams fogOf(float r, float g, float b, float d,
                                        float start, float cap) {
    x3::rhi::IRenderDevice::FogParams f;
    f.enabled = true; f.color[0] = r; f.color[1] = g; f.color[2] = b;
    f.density = d; f.start = start; f.maxOpacity = cap;
    return f;
}

// ---- HONEST INTERIOR AMBIENT (the rifthub lesson, applied game-wide-ish) -------------
// THE CRUTCH: the render device's DEFAULT ambient is {0.42, 0.44, 0.50} (a "small
// constant lift" that is anything but small) and NOTHING in the canon world ever called
// setAmbient — so every interior in the facility was lit by a huge omnidirectional wash.
// Ambient is not light: it is a flat term added to every surface regardless of direction,
// so raising it lights a room BY DESTROYING ITS CONTRAST. The detention cell was the
// proof — a white cardboard box with 17 point lights inside it and no shadow anywhere.
// (Same lesson the rifthub hall learned at 0.100 -> 0.032; see rifthub.cpp applyAtmosphere.)
//
// So the zone atmosphere now owns AMBIENT + IBL as well as fog:
//   * ZWard (detention — Jake's cell) goes to the rifthub's honest floor. The cell is
//     lit by ONE practical now; ambient only keeps the deepest corner off pure black.
//   * Every other interior zone gets a MODERATE cut (0.42 -> ~0.16). Their recipe key
//     lights (1.3-2.3 @ range 5-6) carry them; this buys back highlight-to-shadow
//     gradient without a 52-room re-light this pass. (Backlog: hand-tune per zone.)
//   * ZCave / ZOrganic stay near-black (they were authored as fog-and-silhouette spaces).
//   * OUTSIDE any room (kNoRoom = the SEAM-2 exterior / streamed planet) the ENGINE
//     DEFAULT is restored, so daylight on the tower facade is byte-identical to before.
// IBL comes down with it for the same reason (the split-sum bias term is albedo-
// INDEPENDENT: a bright probe paints a neutral grey wash over any surface — the
// "ghost-gate" failure the rifthub root-caused).
struct ZoneAir { float amb[3]; float ibl; };
const ZoneAir& airFor(uint8_t z) {
    // GAMMA-RECAL (fix/gamma-recal, 2026-07-25): these airs were tuned against the BENT
    // output curve (the swapchain never sRGB-encoded — 5951890b), so they all read
    // brighter now. TWO calibration sources, two postures:
    //   * HALLS/CORRIDORS (kHall, NEW) — anchored to the owner's locked acceptance
    //     target ("the hall outside the cell as it used to look WITH the flashlight —
    //     without it", reference frames in docs/screenshots/gamma_recal/). That look
    //     needs a genuinely dark air (~0.024) so the warm pendant pools + near-black
    //     fog own the read. Precision-matched, not guessed.
    //   * EVERY OTHER INTERIOR (kMid/kDark/kOrganic) — the owner PLAYED the post-gamma
    //     build live (2026-07-25) and called the facility "OK, a bit too bright —
    //     needs to be turned down a HAIR." So these take a GENTLE trim (~20-25%) from
    //     their pre-fix values, no restructure. Do not overcorrect rooms into gloom.
    // Both are VALUE retunes from the corrected baseline — no exposure/encode hacks.
    static const ZoneAir kHall   { { 0.024f, 0.025f, 0.029f }, 0.20f };  // halls/corridors (anchor-matched)
    static const ZoneAir kDark   { { 0.024f, 0.026f, 0.030f }, 0.17f };  // detention (hair trim)
    static const ZoneAir kMid    { { 0.115f, 0.121f, 0.134f }, 0.30f };  // built interiors (hair trim)
    static const ZoneAir kOrganic{ { 0.016f, 0.021f, 0.018f }, 0.15f };  // cave / monster space (hair trim)
    switch (z) {
        case ZHall:
        case ZCorridor: return kHall;
        case ZWard:    return kDark;
        case ZCave:
        case ZOrganic: return kOrganic;
        default:       return kMid;
    }
}
// The engine defaults (VulkanRenderDevice m_ambient / m_iblIntensity), re-asserted the
// moment the eye leaves the room graph so the exterior/sky path is untouched.
constexpr float kExteriorAmbient[3] = { 0.42f, 0.44f, 0.50f };
constexpr float kExteriorIbl        = 0.50f;   // app_run sets this for the SEAM-2 facade

// Indexed by Zone. Texture sets are AD-3's curated survivors (matlib-verified).
const Recipe& recipeFor(uint8_t z) {
    static const Recipe kRecipes[ZCount] = {
        /*ZNone*/     {},
        // ===== GAMMA-RECAL (fix/gamma-recal, 2026-07-25): the whole table was tuned
        // against the BENT curve (5951890b). The owner PLAYED the corrected build and
        // called the facility 'OK, a bit too bright — turn it down a HAIR', so every
        // non-corridor zone takes a GENTLE hue-preserving trim: keys+accents x0.85,
        // fog colours x0.8 (density/reach untouched). ZWard alone takes x0.72+fog x0.7
        // — measured blown (WR-1 mean 74-76, p50 ~60, p95 175-192, cream-washed walls;
        // frames in docs/screenshots/gamma_recal/zones_*). ZHall/ZCorridor are NOT in
        // this scheme: they are precision-anchored to the owner's hall reference
        // (see their own comment). =====
        // ===== KEY LEVELS — THE WHOLE TABLE WAS HALF-LIT (2026-07-12 facility audit) =====
        // Every zone key in this table was authored at a mean of 0.7-2.4 while EVERY practical
        // that actually WORKS in this building runs 3.2-3.3: the cell's fluorescent tube is
        // 3.30 (art/cell, 3f7e6d0) and env_art's ceiling fixtures are 3.2. The recipe rooms
        // were not "moody" — they were keyed at HALF the building's own fixture standard, and
        // the 0.42 ambient wash hid it until the wash died.
        //   MEASURED, flashlight OFF: E Cell Hall mean 7.9 (67% of pixels <= 6/255),
        //   W Service 7.3 (70% void), Security 8.3 (68% void), Main Hall 13.2 (68% void).
        // Every key below is now scaled so its MEAN lands on 3.2, with the HUE RATIO PRESERVED
        // EXACTLY — a corridor key is still the same cool blue-white, a ward key still the same
        // warm amber; only the LEVEL moves. The boost is capped at 2.2x, which deliberately
        // leaves ZStorage (2.46), ZSalvari (1.94) and ZOrganic (1.59) BELOW the standard: those
        // rooms are meant to be darker than a corridor, and this preserves that intent.
        // This is not a crutch — it is matching each room to the fixture standard the rest of
        // the facility already proved. It pairs with the ceiling VALUE clamp in
        // surface_library.h (albedo band [0.08, 0.40]); the two MUST ship together — the clamp
        // alone measurably darkened every corridor (see the note there).
        // GAMMA-RECAL (fix/gamma-recal): the hall/corridor keys were COOL blue-white and
        // their fog a pale cyan — under the bent curve that photographed as black falloff
        // with the WARM torch pooling on the near walls, and THAT read is the owner's
        // locked acceptance target ("the hall as it used to look WITH the flashlight —
        // without it"). So the corridor rhythm keys now ARE the old torch: the exact
        // warm-white the flashlight carried (3.30/3.10/2.75), one pool every ~8 m, and
        // the fog drops to a near-black warm-neutral so the run between pools falls into
        // darkness instead of a milky cyan wash (the honest curve lifted the old 0.03-0.046
        // fog colours ~5x on screen). Matched against the 5951890b^ reference frames in
        // docs/screenshots/gamma_recal/ (hall_cam{A,B,C}).
        /*ZHall*/     { "mw_metal_trim_b", 2.8f, "sr_rubberfloor", 2.2f, "mw_metal_panels_a", 3.0f,
                        1.90f, 1.79f, 1.58f, 5.5f,   0.14f, 0.75f, 0.85f, 2.6f,
                        fogOf(0.002f, 0.002f, 0.0018f, 0.100f, 2.5f, 0.97f) },
        /*ZCorridor*/ { "mw_concrete_panels_a", 2.6f, "sr_rubberfloor", 2.2f, "mw_metal_panels_a", 3.0f,
                        1.90f, 1.79f, 1.58f, 5.5f,   0.14f, 0.75f, 0.85f, 2.4f,
                        fogOf(0.002f, 0.002f, 0.0018f, 0.100f, 2.5f, 0.97f) },
        // W2-A DETENTION = hazard AMBER (bible/audit). The accent was already amber;
        // warm the fog tint from near-neutral to a clear amber wash and widen the
        // accent so every ward cell reads amber, not cold (audit fix: "detention
        // reads cyan"). Halls stay cyan (ZHall/ZCorridor untouched); Jake's frozen
        // cell is ZNone and is pinned to the old warm-neutral fog in build() so this
        // amber never bleeds into the canon hand-calibrated reference.
        /*ZWard*/     { "hh_wall_01a", 3.0f, "hh_floor_01a", 2.4f, "hh_ceiling_01a", 2.8f,
        // LAND-LIGHTING: the KEY takes the audit's honest value (2.20 -> 4.27); the amber
        // ACCENT + amber FOG above are the deliberate detention identity and are kept. The
        // old key was tuned while applyCabAtmosphere washed 0.42 ambient over the whole game
        // (B1) — with the wash gone it under-lights by ~2x. Hue is unchanged (both keys are
        // 1 : 0.77 : 0.47 warm); only the level moves.
                        3.07f, 2.38f, 1.47f, 3.6f,   1.24f, 0.69f, 0.16f, 2.9f,
                        fogOf(0.041f, 0.029f, 0.016f, 0.0042f, 1.2f, 0.62f) },
        /*ZSecurity*/ { "mw_concrete_panels_a", 2.4f, "mw_metal_grate", 2.0f, "mw_metal_panels_a", 3.0f,
                        2.67f, 2.67f, 2.81f, 3.2f,   1.19f, 0.06f, 0.04f, 2.2f,
                        fogOf(0.016f, 0.018f, 0.021f, 0.0030f, 1.2f, 0.55f) },
        /*ZLab*/      { "mw_plaster_painted", 2.6f, "sr_rubberfloor", 2.2f, "hh_ceiling_01a", 2.8f,
                        2.64f, 2.87f, 2.64f, 6.5f,   0.21f, 0.94f, 0.30f, 2.6f,
                        fogOf(0.030f, 0.037f, 0.032f, 0.0030f, 1.5f, 0.55f) },
        /*ZBoss*/     { "sr_concrete_01", 2.8f, "sr_concrete_a", 2.6f, "mw_metal_panels_a", 3.2f,
                        3.61f, 2.82f, 1.73f, 6.0f,   1.28f, 0.81f, 0.21f, 2.6f,
                        fogOf(0.036f, 0.032f, 0.027f, 0.0040f, 1.4f, 0.62f) },
        /*ZLobby*/    { "mw_metal_trim_a", 2.8f, "sr_rubberfloor", 2.2f, "mw_metal_panels_a", 3.0f,
                        2.50f, 2.69f, 2.97f, 4.6f,   0.12f, 0.64f, 0.72f, 2.4f,
                        fogOf(0.024f, 0.032f, 0.037f, 0.0040f, 1.5f, 0.60f) },
        /*ZStorage*/  { "mw_concrete_panels_b", 2.6f, "sr_concrete_a", 2.4f, "mw_metal_panels_a", 3.0f,
                        2.52f, 2.15f, 1.59f, 4.2f,   1.28f, 0.81f, 0.21f, 2.2f,
                        fogOf(0.036f, 0.032f, 0.027f, 0.0035f, 1.2f, 0.60f) },
        // ---- W3-2 tower floors. Sets include AD-3's four previously-unused curated
        // survivors (cc_porous_cement, mw_thermal_padding, sr_metal_b, mw_metal_grate). ----
        /*ZMedical*/  { "hh_wall_01a", 3.0f, "hh_floor_01a", 2.4f, "hh_ceiling_01a", 2.8f,
                        2.64f, 2.89f, 2.64f, 5.5f,   0.26f, 0.89f, 0.30f, 2.6f,
                        fogOf(0.032f, 0.038f, 0.032f, 0.0032f, 1.4f, 0.55f) },
        /*ZGenetics*/ { "mw_plaster_painted", 2.6f, "hh_floor_01a", 2.4f, "hh_ceiling_01a", 2.8f,
                        2.52f, 3.05f, 2.58f, 5.5f,   0.17f, 1.02f, 0.26f, 2.8f,
                        fogOf(0.027f, 0.042f, 0.029f, 0.0042f, 1.4f, 0.60f) },
        /*ZCyber*/    { "sr_metal_b", 2.6f, "mw_metal_grate", 2.0f, "mw_metal_panels_a", 3.0f,
                        2.41f, 2.66f, 3.08f, 5.0f,   0.14f, 0.72f, 0.89f, 2.6f,
                        fogOf(0.019f, 0.026f, 0.032f, 0.0038f, 1.4f, 0.60f) },
        // W8-1 floor identity: the drone station stands on HAZARD-STRIPED deck plate
        // (sr_floorstripes — a curated set no zone used yet), not the same grate as F4.
        // W2-A F5 floor scale fix (report §1.2): sr_floorstripes at 2.4 m/repeat read as
        // fine CORDUROY across the big drone deck. 6.0 m/repeat enlarges each hazard band
        // so the deck reads as HANGAR LANES (a code dial, not a reforge).
        // LAND-LIGHTING: 6.0 m floor scale KEPT; key takes the audit's honest value (1.75 -> 3.46).
        /*ZDroneBay*/ { "mw_thermal_padding", 2.8f, "sr_floorstripes", 6.0f, "mw_metal_panels_a", 3.2f,
                        2.94f, 2.78f, 2.44f, 6.5f,   1.32f, 0.81f, 0.21f, 2.8f,
                        fogOf(0.028f, 0.028f, 0.026f, 0.0035f, 1.5f, 0.60f) },
        /*ZSalvari*/  { "sr_concrete_01", 2.8f, "sr_concrete_a", 2.6f, "sr_concrete_01", 3.2f,
                        2.06f, 1.72f, 1.16f, 4.5f,   0.21f, 0.94f, 0.38f, 2.8f,
                        fogOf(0.014f, 0.021f, 0.017f, 0.0060f, 1.2f, 0.72f) },
        // LAND-LIGHTING: cc_exec_floor (the curated exec carpet) KEPT from main; the key takes
        // the audit's honest value (2.00 -> 3.62).
        /*ZExec*/     { "cc_porous_cement", 3.2f, "cc_exec_floor", 2.6f, "mw_metal_panels_a", 3.2f,
                        3.08f, 2.77f, 2.31f, 5.5f,   1.36f, 0.98f, 0.38f, 2.6f,
                        fogOf(0.032f, 0.029f, 0.024f, 0.0025f, 1.6f, 0.50f) },
        // W5-1: the Nexus Chamber — no surfaces/lights (canon_45 owns the look);
        // the fog IS the recipe: near-black, heavy, silhouettes-over-detail.
        /*ZCave*/     { nullptr, 0, nullptr, 0, nullptr, 0,
                        0, 0, 0, 0,   0, 0, 0, 0,
                        fogOf(0.008f, 0.011f, 0.008f, 0.0140f, 0.8f, 0.88f) },
        // W8-1: organic story rooms — dark concrete base under a lattice lid, one dim
        // warm practical, BIOLUME GREEN accent, heavy green-black fog (§3 monster spaces;
        // the blood-red half of the two-accent exception is painted per-room, not here).
        /*ZOrganic*/  { "sr_concrete_01", 2.8f, "sr_concrete_a", 2.6f, "sr_metal_lattice", 3.0f,
                        1.78f, 1.34f, 0.94f, 4.2f,   0.24f, 0.98f, 0.38f, 3.0f,
                        fogOf(0.010f, 0.018f, 0.012f, 0.0085f, 1.0f, 0.78f) },
    };
    return kRecipes[z < ZCount ? z : ZNone];
}

} // namespace

std::vector<std::string> recipeSurfaceSets() {
    std::vector<std::string> out;
    auto add = [&out](const char* n) {
        if (n && *n && std::find(out.begin(), out.end(), n) == out.end())
            out.emplace_back(n);
    };
    for (uint8_t z = 1; z < ZCount; ++z) {
        const Recipe& r = recipeFor(z);
        add(r.wall); add(r.floor); add(r.ceil);
    }
    return out;
}

namespace {

// Classify a canon room into a recipe zone by name/type (case-sensitive canonical data).
uint8_t classify(const CanonRoom& r, const CanonBeats& bt, uint32_t roomId) {
    if (roomId == bt.jakeCell) return ZNone;          // frozen hand-calibrated reference
    if (r.cy < -50.0f)         return ZNone;          // Cave / Hidden Sub-Level (organic zone, later)
    auto has = [&](const char* s) { return r.name.find(s) != std::string::npos; };

    // ---- W3-2: TOWER FLOORS route by ELEVATION BAND (the data ships absolute
    // elevations: F2 y~10, F3 ~20, F4 ~30, the F4.5 tiers 33..64, F5 ~65, F6 ~78,
    // F7 ~91). Structural kinds keep the shared recipes; everything else takes the
    // floor's zone. The F4.5 Cave-Chamber tiers stay ZNone (the organic monster
    // zone is its own future pass, like the deep caves).
    if (r.cy > 5.0f) {
        // W5-1: the Nexus Chamber (4.5 tiers + the open-ceiling Access room) takes the
        // CAVE zone — no panels (canon_45 hand-dresses), but the heavy near-black fog
        // rides the zone-atmosphere path like every other zone.
        if (r.platform || r.openCeiling) return ZCave;
        // W8-1: the remaining "Cave Chamber" rooms are ORDINARY story rooms the old
        // rule left bare (F3 Spawning Chamber, F6 Portal Chamber / Energy Nexus /
        // First Contact Chamber) — they take the organic monster-space recipe.
        if (r.type == "Cave Chamber") return ZOrganic;
        if (has("Elevator"))                         return ZLobby;
        // Long tower corridors (18-24 m) need the HALL treatment (light RHYTHM +
        // trim walls) — a single mid key leaves them black tunnels (R2 eye round).
        if (has("Hall") || has("Corridor"))
            return (std::max(r.w, r.d) >= 14.0f) ? ZHall : ZCorridor;
        if (has("Boss") || r.type == "Boss Arena")   return ZBoss;
        if (r.type == "Holding Cell")                return ZWard;   // Quarantine / Sarah's cell
        if (has("Security") || has("Guard") || has("Armory") || has("Weapons Locker"))
                                                     return ZSecurity;
        if (r.type == "Storage" || has("Storage") || has("Coolant") || has("Power Junction")
            || has("Maintenance") || has("Recharge") || has("Cold Room")) return ZStorage;
        const float y = r.cy;
        if (y < 18.0f)  return ZMedical;             // F2 wards / theaters / pharmacy
        if (y < 28.0f)  return ZGenetics;            // F3
        // F4's wing rooms ride the full 9 m floor ceiling, so their center-y is
        // 30 + 4.5 = 34.5 — past the old 33 band edge (which assumed the canon
        // rooms' lower ceilings) and into ZDroneBay. 35 keeps every F4 room cyan;
        // the F4.5 tiers (33..64) never reach this line (platform/openCeiling ->
        // ZCave, type "Cave Chamber" -> ZOrganic above).
        if (y < 35.0f)  return ZCyber;               // F4
        if (y < 76.0f)  return ZDroneBay;            // F5
        if (y < 88.0f)  return ZSalvari;             // F6
        return ZExec;                                // F7 + roof rooms
    }

    // ---- Floor 1 (unchanged from W3-1). ----
    if (has("Main Hall"))                            return ZHall;
    if (has("Hall") || has("Corridor"))              return ZCorridor;
    if (roomId == bt.security || has("Security") || has("Armory")) return ZSecurity;
    if (roomId == bt.research || has("Research") || has("Lab") || has("Genetics")
        || roomId == bt.medical || has("Medical"))   return ZLab;
    if (roomId == bt.bossArena || has("Boss"))       return ZBoss;
    if (has("Elevator"))                             return ZLobby;
    if (has("Storage") || has("Supply") || has("Mess") || has("Cafeteria")) return ZStorage;
    if (r.type == "Cell" || has("WL-") || has("WR-") || has("EL-") || has("ER-")
        || has("Ward"))                              return ZWard;
    return ZNone;
}

// ---- W8-1 floor identity (CAMPAIGN_LEDGER: "corridors read similar F2-F7"). One cheap
// distinguishing element per floor, recipe-driven: a colored wall STRIPE band along every
// corridor/lobby wall run + the corridor accent light and guide strips re-hued to match,
// so each floor's connective tissue carries its own signature while staying inside the
// one-accent-per-room law (the floor hue REPLACES corridor teal, never joins it).
// Fields: stripe hue (r,g,b) · emissive scale · band height above floor · band thickness.
struct FloorId { float r, g, b; float em; float y; float th; };
const FloorId* floorIdentity(int fno) {
    static const FloorId kIds[] = {
        { 0.32f, 0.95f, 0.55f, 0.55f, 1.35f, 0.16f },  // F2 medical green band
        { 0.20f, 1.00f, 0.35f, 0.45f, 1.55f, 0.10f },  // F3 genetics code line
        { 0.72f, 0.28f, 1.00f, 0.60f, 1.65f, 0.12f },  // F4 purple neon (data desc: "Purple neon")
        { 1.00f, 0.62f, 0.10f, 0.50f, 0.45f, 0.22f },  // F5 low amber hazard band
        { 0.28f, 1.00f, 0.45f, 0.40f, 1.75f, 0.07f },  // F6 biolume glyph line
        { 0.95f, 0.70f, 0.30f, 0.30f, 1.05f, 0.05f },  // F7 brass trim
    };
    return (fno >= 2 && fno <= 7) ? &kIds[fno - 2] : nullptr;   // F1 keeps its teal
}

// ---- Face/segment math for opening-aware wall tiling. --------------------------------
// Faces: 0 = x0 plane (panel faces +X), 1 = x1 (-X), 2 = z0 (+Z), 3 = z1 (-Z).
struct Cut { float lo, hi; };

void collectCuts(const CanonFloor& floor, uint32_t room, const CanonRoom& r,
                 std::vector<Cut> cuts[4]) {
    const float m = 0.35f;   // clearance beyond the cut for jambs/frames
    for (const CanonDoorway& d : floor.doorways) {
        if (d.a != room && d.b != room) continue;
        if (d.kind == DoorwayKind::CrossLevel) continue;   // vertical tube, not a wall cut
        if (d.axis == 0) {   // wall plane X=const -> cut on face 0 or 1, span along Z
            const int f = (std::fabs(d.cx - r.x0()) <= std::fabs(d.cx - r.x1())) ? 0 : 1;
            cuts[f].push_back({ d.cz - d.cutHalf - m, d.cz + d.cutHalf + m });
        } else {             // wall plane Z=const -> cut on face 2 or 3, span along X
            const int f = (std::fabs(d.cz - r.z0()) <= std::fabs(d.cz - r.z1())) ? 2 : 3;
            cuts[f].push_back({ d.cx - d.cutHalf - m, d.cx + d.cutHalf + m });
        }
    }
}

// Subtract cut intervals from [lo..hi]; append surviving segments >= minLen.
void segments(float lo, float hi, std::vector<Cut>& cuts, float minLen,
              std::vector<Cut>& out) {
    std::sort(cuts.begin(), cuts.end(), [](const Cut& a, const Cut& b) { return a.lo < b.lo; });
    float cur = lo;
    for (const Cut& c : cuts) {
        if (c.lo > cur && (std::min(c.lo, hi) - cur) >= minLen)
            out.push_back({ cur, std::min(c.lo, hi) });
        cur = std::max(cur, c.hi);
        if (cur >= hi) return;
    }
    if (hi - cur >= minLen) out.push_back({ cur, hi });
}

// Column-major yaw-about-Y / pitch-about-X rotations composed into a TR transform.
void makeTR(float t[16], float yawY, float pitchX, float px, float py, float pz) {
    const float cy = std::cos(yawY),  sy = std::sin(yawY);
    const float cx = std::cos(pitchX), sx = std::sin(pitchX);
    // R = Ry * Rx (columns = images of basis vectors)
    t[0] = cy;        t[1] = 0.0f;  t[2]  = -sy;       t[3]  = 0;
    t[4] = sy * sx;   t[5] = cx;    t[6]  = cy * sx;   t[7]  = 0;
    t[8] = sy * cx;   t[9] = -sx;   t[10] = cy * cx;   t[11] = 0;
    t[12] = px;       t[13] = py;   t[14] = pz;        t[15] = 1;
}

} // namespace

// ---- Mesh helpers --------------------------------------------------------------------

x3::rhi::MeshHandle RoomDressing::quadMesh(x3::rhi::IRenderDevice& device,
                                           float w, float h, float tileMeters) {
    // Dedupe by quantized dims (0.25 m grid) + tile size.
    const uint64_t key = (uint64_t)std::lround(w * 4.0f) << 40 |
                         (uint64_t)std::lround(h * 4.0f) << 16 |
                         (uint64_t)std::lround(tileMeters * 8.0f);
    for (auto& kv : m_quadCache) if (kv.first == key) return kv.second;
    // Centered XY quad, normal +Z, UVs repeat every tileMeters. Double-sided winding
    // so the orient math can never backface-cull a wall out of existence.
    const float hw = w * 0.5f, hh = h * 0.5f;
    const float tu = w / tileMeters, tv = h / tileMeters;
    x3::rhi::MeshVertex v[4] = {};
    const float px[4] = { -hw,  hw,  hw, -hw };
    const float py[4] = { -hh, -hh,  hh,  hh };
    const float uu[4] = { 0, tu, tu, 0 };
    const float vv[4] = { tv, tv, 0, 0 };
    for (int i = 0; i < 4; ++i) {
        v[i].pos[0] = px[i]; v[i].pos[1] = py[i]; v[i].pos[2] = 0.0f;
        v[i].normal[2] = 1.0f; v[i].uv[0] = uu[i]; v[i].uv[1] = vv[i];
    }
    const uint32_t idx[12] = { 0,1,2, 0,2,3,  0,2,1, 0,3,2 };
    x3::rhi::MeshHandle mh = device.createMesh(v, 4, idx, 12);
    m_quadCache.push_back({ key, mh });
    return mh;
}

uint32_t RoomDressing::loadAsset(const std::string& rel) {
    for (uint32_t i = 0; i < m_assetPaths.size(); ++i)
        if (m_assetPaths[i] == rel) return i;
    Asset a;
    a.model = m_loader->load(rel);
    if (a.model.ok) {
        a.drawables = x3::asset::makeDrawables(a.model);
        a.ok = !a.drawables.empty();
    }
    if (!a.ok) x3::logWarn("[room-dress] failed to load " + rel + " (prop skipped)");
    m_assetTable.push_back(std::move(a));
    m_assetPaths.push_back(rel);
    return (uint32_t)m_assetTable.size() - 1;
}

void RoomDressing::placeProp(uint32_t room, uint32_t asset, float yaw, float s,
                             float ax, float ay, float az,
                             float wx, float wy, float wz,
                             const float emissive[4], const float tint[4]) {
    if (asset >= m_assetTable.size() || !m_assetTable[asset].ok) return;
    const float c = std::cos(yaw), sn = std::sin(yaw);
    PropInst e; e.room = room; e.asset = asset;
    e.transform[0] = c * s;  e.transform[2] = -sn * s;
    e.transform[5] = s;
    e.transform[8] = sn * s; e.transform[10] = c * s;
    e.transform[15] = 1.0f;
    const float rpx = (c * ax + sn * az) * s;
    const float rpy = ay * s;
    const float rpz = (-sn * ax + c * az) * s;
    e.transform[12] = wx - rpx; e.transform[13] = wy - rpy; e.transform[14] = wz - rpz;
    if (emissive) for (int i = 0; i < 4; ++i) e.emissive[i] = emissive[i];
    if (tint)     for (int i = 0; i < 4; ++i) e.tint[i]     = tint[i];
    m_props.push_back(e);
}

// ---- Build ----------------------------------------------------------------------------

bool RoomDressing::build(x3::rhi::IRenderDevice& device,
                         std::string_view surfaceLibDir, std::string_view convertedGlbDir,
                         const CanonFloor& floor, const CanonBeats& beats) {
    if (!floor.valid()) return false;
    m_surf.mount(std::string(surfaceLibDir));
    m_assets.reset(x3::asset::createAssetSource());
    if (m_assets->mountDir(convertedGlbDir, 0)) {
        // F2 rescue captives (Aria/Keisha/Emily) live in the rigged-GLB tree, not the
        // converted kit — mount it too (higher priority; no path collision expected) so
        // the "Rescue Room" recipe can load the Anna cast onto the beds.
        m_assets->mountDir(riggedGlbRoot(), 1);
        m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));
    }

    m_roomZone.assign(floor.rooms.size(), ZNone);
    m_zoneFog.assign(ZCount, recipeFor(ZWard).fog);   // default = detention tint
    for (uint8_t z = 1; z < ZCount; ++z) m_zoneFog[z] = recipeFor(z).fog;
    // W2-A: Jake's cell classifies ZNone (frozen hand-calibrated reference). Pin its
    // atmosphere to the ORIGINAL warm-neutral detention fog so the ward-zone amber
    // recolor above never shifts the canon cell the vigil beat is framed in.
    m_zoneFog[ZNone] = fogOf(0.045f, 0.040f, 0.034f, 0.0035f, 1.2f, 0.60f);

    // Surface sets loaded once up front (name -> stable cache pointer).
    auto setIdx = [&](const char* name) -> uint32_t {
        const SurfaceSet& s = m_surf.get(device, name);
        m_sets.push_back(&s);
        return (uint32_t)m_sets.size() - 1;
    };

    const uint32_t aConsole = m_loader ? loadAsset(kRelConsole) : 0;
    const uint32_t aPipes   = m_loader ? loadAsset(kRelPipes)   : 0;
    const uint32_t aCrateS  = m_loader ? loadAsset(kRelCrateS)  : 0;
    const uint32_t aCrateL  = m_loader ? loadAsset(kRelCrateL)  : 0;
    const uint32_t aBarrel  = m_loader ? loadAsset(kRelBarrel)  : 0;
    const uint32_t aPallet  = m_loader ? loadAsset(kRelPallet)  : 0;
    const uint32_t aBin     = m_loader ? loadAsset(kRelBin)     : 0;
    const uint32_t aCot     = m_loader ? loadAsset(kRelCot)     : 0;
    // W8-1 desc-gold kit.
    const uint32_t aFuse    = m_loader ? loadAsset(kRelFusebox) : 0;
    const uint32_t aCam     = m_loader ? loadAsset(kRelCam)     : 0;
    const uint32_t aExt     = m_loader ? loadAsset(kRelExting)  : 0;
    const uint32_t aHang    = m_loader ? loadAsset(kRelHang)    : 0;
    const uint32_t aDuct    = m_loader ? loadAsset(kRelDuct)    : 0;
    const uint32_t aVent    = m_loader ? loadAsset(kRelVent)    : 0;
    const uint32_t aDrone   = m_loader ? loadAsset(kRelDrone)   : 0;
    // F2 rescue captives — the Anna cast (rescue.cpp maps the SAME live models):
    // Aria=AnnaCasual, Keisha=AnnaBodySuit, Emily=AnnaTactical. These are PRE-POSED
    // supine bakes (headless Blender pose-and-bake: spine raised to the bed incline,
    // knees up, legs apart, arms at sides). The bake is already lying face-up (head +Z,
    // back at model-Y 0), so the recipe just positions + yaws them onto the beds.
    const uint32_t aAria    = m_loader ? loadAsset("Detention/Captive_Aria_Posed.glb")   : 0;
    const uint32_t aKeisha  = m_loader ? loadAsset("Detention/Captive_Keisha_Posed.glb") : 0;
    const uint32_t aEmily   = m_loader ? loadAsset("Detention/Captive_Emily_Posed.glb")  : 0;

    // Kit tints (cell_dressing palette family).
    const float tCrate[4]  = { 0.66f, 0.60f, 0.52f, 1.0f };
    const float tBarrel[4] = { 0.46f, 0.34f, 0.25f, 1.0f };
    const float tPallet[4] = { 0.48f, 0.38f, 0.26f, 1.0f };
    const float tSteel[4]  = { 0.42f, 0.45f, 0.50f, 1.0f };
    const float tDarkCon[4] = { 0.18f, 0.21f, 0.28f, 1.0f };
    // W8-1 tints: gunmetal wall boxes / dark cam housing / safety red / pale clinical /
    // sickly specimen green / cryo white-blue / drone airframe grey / dark organic pod.
    const float tPanelBox[4] = { 0.30f, 0.32f, 0.36f, 1.0f };
    const float tDark[4]     = { 0.09f, 0.10f, 0.12f, 1.0f };
    const float tRed[4]      = { 0.62f, 0.10f, 0.08f, 1.0f };
    const float tClinic[4]   = { 0.78f, 0.82f, 0.80f, 1.0f };
    const float tSpecimen[4] = { 0.38f, 0.58f, 0.44f, 1.0f };
    const float tCryo[4]     = { 0.62f, 0.72f, 0.80f, 1.0f };
    const float tAirframe[4] = { 0.50f, 0.53f, 0.57f, 1.0f };
    const float tPod[4]      = { 0.28f, 0.30f, 0.22f, 1.0f };

    // Shadow-disc mesh (radial fade, drawn through the glass pass) — cell_dressing's.
    x3::rhi::MeshHandle disc{};
    {
        std::vector<x3::rhi::MeshVertex> verts;
        std::vector<uint32_t> idx;
        auto push = [&](float x, float z) {
            x3::rhi::MeshVertex mv{}; mv.pos[0] = x; mv.pos[2] = z;
            mv.normal[1] = 1.0f; mv.uv[0] = 0.5f; verts.push_back(mv);
        };
        push(0, 0);
        // 64 segments (was 20): the coarse ring read as a POLYGONAL "jagged rounded"
        // silhouette when scaled up to a bed-sized contact shadow (owner note #3).
        const int seg = 64;
        for (int i = 0; i <= seg; ++i) {
            const float t = (float)i / seg * 2.0f * kPi;
            push(std::cos(t), std::sin(t));
        }
        // SINGLE winding (owner note #3 root cause): the old mesh emitted BOTH windings
        // per triangle at identical depth, so the fan self-z-fought — at grazing angles
        // the 64 radial fan edges showed as hard pixelated "spokes" radiating from the
        // shadow (the real jagged aberration, on top of the coarse silhouette). The glass
        // pipeline already rasterizes CULL_MODE_NONE (double-sided), so one winding draws
        // from every angle with no self-z-fight.
        for (int i = 1; i <= seg; ++i) {
            idx.push_back(0); idx.push_back(i); idx.push_back(i + 1);
        }
        disc = device.createMesh(verts.data(), (uint32_t)verts.size(),
                                 idx.data(), (uint32_t)idx.size());
    }
    // W8-1: generalized tinted floor blob (glass-pass disc). Blood trails / chemical
    // stains / scorch reuse the shadow-disc mesh with a motivated hue (ART_BIBLE §4:
    // grime is directional and motivated — these ground the desc-authored violence).
    auto floorBlob = [&](uint32_t room, float x, float y, float z, float rx, float rz,
                         float cr, float cg, float cb, float a) {
        ProcDraw p; p.room = room; p.mesh = disc; p.glass = true;
        p.color[0] = cr; p.color[1] = cg; p.color[2] = cb; p.color[3] = a;
        p.transform[0] = rx; p.transform[5] = 1; p.transform[10] = rz; p.transform[15] = 1;
        p.transform[12] = x; p.transform[13] = y + kFloorLift; p.transform[14] = z;
        m_proc.push_back(p);
    };
    auto shadowBlob = [&](uint32_t room, float x, float y, float z, float rx, float rz,
                          float dark) {
        // FEATHERED contact-shadow POOL (owner note #3). The old single hard disc read
        // as a jagged rounded aberration on the floor. Stack three concentric discs — a
        // dark core with progressively softer, wider rings — so the edge FEATHERS into a
        // soft grounding pool instead of a hard cut. Each ring is lifted a hair to avoid
        // coplanar z-fighting in the glass pass (innermost highest). Combined with the
        // 64-seg mesh above, the polygonal jaggedness is gone.
        const float cr = 0.02f, cg = 0.02f, cb = 0.03f;
        floorBlob(room, x, y + 0.004f, z, rx * 1.32f, rz * 1.32f, cr, cg, cb, dark * 0.25f);
        floorBlob(room, x, y + 0.006f, z, rx * 1.00f, rz * 1.00f, cr, cg, cb, dark * 0.50f);
        floorBlob(room, x, y + 0.008f, z, rx * 0.68f, rz * 0.68f, cr, cg, cb, dark * 0.78f);
    };
    // Dried-blood smear: dark desaturated red, stretched along its run.
    auto bloodBlob = [&](uint32_t room, float x, float y, float z, float rx, float rz,
                         float a) {
        floorBlob(room, x, y, z, rx, rz, 0.21f, 0.03f, 0.02f, a);
    };

    uint32_t nPanels = 0, nLightsBefore = 0;
    for (uint32_t ri = 0; ri < floor.rooms.size(); ++ri) {
        const CanonRoom& r = floor.rooms[ri];
        const uint8_t z = classify(r, beats, ri);
        if (z == ZNone) continue;
        m_roomZone[ri] = z;
        const Recipe& rec = recipeFor(z);
        // W5-1: fog-only zones (ZCave — the Nexus Chamber) carry NO surface recipe;
        // canon_45 hand-dresses them. The zone tag above still drives the fog.
        if (!rec.wall) continue;
        const uint32_t wallSet  = setIdx(rec.wall);
        const uint32_t floorSet = setIdx(rec.floor);
        const uint32_t ceilSet  = setIdx(rec.ceil);

        const float fY = r.y0(), cY = r.y1();
        const float wallH = (cY - fY) - 0.05f;
        const float wallCy = (fY + cY) * 0.5f;

        // W8-1 floor identity: corridors/lobbies on the tower floors carry a per-floor
        // signature stripe + re-hued accent/guide-strips (F1 keeps its teal).
        const int fno = (ri < floor.roomFloorNum.size()) ? floor.roomFloorNum[ri]
                                                         : floor.floorNum;
        const bool corridorish = (z == ZHall || z == ZCorridor || z == ZLobby);
        const FloorId* fid = corridorish ? floorIdentity(fno) : nullptr;

        // ---- Walls: opening-aware segments per face (Law 1: never cover a door). ----
        std::vector<Cut> cuts[4];
        collectCuts(floor, ri, r, cuts);
        std::vector<Cut> segs;
        auto addWallPanel = [&](int face, float lo, float hi) {
            const float len = hi - lo;
            Panel p; p.room = ri; p.set = wallSet;
            p.mesh = quadMesh(device, len, wallH, rec.wallTile);
            const float mid = (lo + hi) * 0.5f;
            switch (face) {
                case 0: makeTR(p.transform,  kPi * 0.5f, 0, r.x0() + kInset, wallCy, mid); break;
                case 1: makeTR(p.transform, -kPi * 0.5f, 0, r.x1() - kInset, wallCy, mid); break;
                case 2: makeTR(p.transform,  0.0f,       0, mid, wallCy, r.z0() + kInset); break;
                case 3: makeTR(p.transform,  kPi,        0, mid, wallCy, r.z1() - kInset); break;
            }
            m_panels.push_back(p); ++nPanels;
            // W8-1: the floor-identity stripe band rides every wall run, 2 cm proud of
            // the panel so it never z-fights (painted-glow discipline: emissive well
            // under lamp level — a signature band, not a light source).
            if (fid && len >= 1.4f) {
                ProcDraw s; s.room = ri;
                s.mesh = quadMesh(device, len - 0.30f, fid->th, 1.0f);
                const float sy = fY + fid->y;
                const float in = kInset + 0.02f;
                switch (face) {
                    case 0: makeTR(s.transform,  kPi * 0.5f, 0, r.x0() + in, sy, mid); break;
                    case 1: makeTR(s.transform, -kPi * 0.5f, 0, r.x1() - in, sy, mid); break;
                    case 2: makeTR(s.transform,  0.0f,       0, mid, sy, r.z0() + in); break;
                    case 3: makeTR(s.transform,  kPi,        0, mid, sy, r.z1() - in); break;
                }
                s.color[0] = fid->r * 0.35f; s.color[1] = fid->g * 0.35f;
                s.color[2] = fid->b * 0.35f; s.color[3] = 1.0f;
                s.emissive[0] = fid->r; s.emissive[1] = fid->g; s.emissive[2] = fid->b;
                s.emissive[3] = fid->em;
                m_proc.push_back(s);
            }
        };
        for (int f = 0; f < 4; ++f) {
            segs.clear();
            const float lo = (f < 2) ? r.z0() : r.x0();
            const float hi = (f < 2) ? r.z1() : r.x1();
            segments(lo + 0.05f, hi - 0.05f, cuts[f], 0.55f, segs);
            for (const Cut& s : segs) addWallPanel(f, s.lo, s.hi);
        }

        // ---- Floor + ceiling (single panels; floor skipped if a descent tube pierces
        // this room — the graybox segments there stay the truth). ----
        // QA MAINLEVEL SWEEP D17 (2026-07-22): the old rule skipped the WHOLE floor
        // panel for ANY room a CrossLevel doorway touches. Written for the deep
        // descent tubes, but the merged tower's elevator SPINE and the isolated-room
        // linker also emit CrossLevel edges — so EVERY tower floor's main corridor +
        // elevator lobby dressed no floor and showed a huge Lambert-bright graybox
        // expanse (sweep2 F2_F2_Main_Corridor_a / F3_F3_Specimen_Hall_a /
        // F4_F4_Augmentation_Corridor_a — the worst surface read on every floor).
        // A tube only pierces the floor of the UPPER room of its pair. Lay the lower
        // room's floor whole, and the upper room's floor as up to FOUR panels AROUND
        // the tube's 3x3 m mouth so a real descent hole is never sealed by dressing.
        {
            const float x0 = r.x0() + 0.05f, x1 = r.x1() - 0.05f;
            const float z0 = r.z0() + 0.05f, z1 = r.z1() - 0.05f;
            // Collect this room's tube mouths (upper-room piercings only).
            struct Hole { float x0, x1, z0, z1; };
            std::vector<Hole> holes;
            for (const CanonDoorway& d : floor.doorways) {
                if (d.kind != DoorwayKind::CrossLevel) continue;
                if (d.a != ri && d.b != ri) continue;
                const CanonRoom& other = floor.rooms[(d.a == ri) ? d.b : d.a];
                if (r.y0() <= other.y0()) continue;        // we are the LOWER room: floor intact
                const float m = 1.5f + 0.2f;               // tube half (1.5) + wall + margin
                holes.push_back({ d.cx - m, d.cx + m, d.cz - m, d.cz + m });
            }
            auto layFloor = [&](float fx0, float fx1, float fz0, float fz1) {
                if (fx1 - fx0 < 0.30f || fz1 - fz0 < 0.30f) return;   // sliver: skip
                Panel p; p.room = ri; p.set = floorSet;
                p.mesh = quadMesh(device, fx1 - fx0, fz1 - fz0, rec.floorTile);
                makeTR(p.transform, 0, -kPi * 0.5f,
                       (fx0 + fx1) * 0.5f, fY + kFloorLift, (fz0 + fz1) * 0.5f);
                m_panels.push_back(p); ++nPanels;
            };
            if (holes.empty()) {
                layFloor(x0, x1, z0, z1);
            } else {
                // One hole is the practical case (spine lobby / linked corridor):
                // frame it with 4 panels (W/E full-depth strips + N/S center strips).
                const Hole& h = holes.front();
                const float hx0 = std::max(h.x0, x0), hx1 = std::min(h.x1, x1);
                const float hz0 = std::max(h.z0, z0), hz1 = std::min(h.z1, z1);
                layFloor(x0, hx0, z0, z1);                 // west strip
                layFloor(hx1, x1, z0, z1);                 // east strip
                layFloor(hx0, hx1, z0, hz0);               // south center
                layFloor(hx0, hx1, hz1, z1);               // north center
            }
        }
        {
            Panel p; p.room = ri; p.set = ceilSet;
            p.mesh = quadMesh(device, r.w - 0.1f, r.d - 0.1f, rec.ceilTile);
            makeTR(p.transform, 0, kPi * 0.5f, r.cx, cY - kInset, r.cz);
            m_panels.push_back(p); ++nPanels;
        }

        // ---- Lights: ONE key statement + accent at the first doorway (§2/§3). -------
        nLightsBefore = (uint32_t)m_lights.size();
        auto addLight = [&](float x, float y, float zp, float range,
                            float cr, float cg, float cb) {
            CanonLight cl; cl.room = ri;
            cl.light.pos[0] = x; cl.light.pos[1] = y; cl.light.pos[2] = zp;
            cl.light.range = range;
            cl.light.color[0] = cr; cl.light.color[1] = cg; cl.light.color[2] = cb;
            m_lights.push_back(cl);
        };
        const bool longX = r.w >= r.d;
        // ---- 2026-07-12, FACILITY LIGHTING AUDIT — THE KEY LIGHTS COULD NOT REACH THEIR
        // OWN FLOOR. The Recipe's keyRange is a FIXED CONSTANT that knows nothing about how
        // tall the room is — but the key is hung on the CEILING. Point attenuation here goes
        // as (1 - d/range)^2, so a key whose range is barely more than the ceiling height
        // has already fallen to ~zero by the time it gets down to the floor:
        //     ZCorridor  keyRange 5.0, ceiling 4.5 -> key at y=4.15, floor 4.15 m below
        //                => atten (1 - 4.15/5.0)^2 = 0.029.   THE FLOOR GETS ~3% OF THE KEY.
        //     ZHall      keyRange 6.0, ceiling 5.0 -> key at y=4.65
        //                => atten (1 - 4.65/6.0)^2 = 0.051.   ~5%.
        //     ZSecurity  keyRange 3.2, ceiling 4.5 -> the key DIES 0.9 m ABOVE THE FLOOR.
        // The rooms were not under-lit; they were lit by lamps that stopped in mid-air.
        // MEASURED, flashlight OFF (docs/screenshots/lighting_audit/facility):
        //     East Cell Hall     mean 7.9  p05 0.9 / p95 20.7 (spread 20 — FLAT), 67% void
        //     W Service Corridor mean 7.3                     (spread 20 — FLAT), 70% void
        //     Security Station   mean 8.3                     (spread 26 — FLAT), 68% void
        //     Main Hall          mean 13.2                                        68% void
        // env_art already had this right (`range = max(9, ceil + 4.5)`) — it sizes the reach
        // to the room. RoomDressing never did. So: keep the recipe's authored range as a
        // FLOOR, and raise it when the room is tall enough that the key would not otherwise
        // land — reach = drop-to-floor + 4 m of spread. This is REACH, not LUMENS: the key
        // colours are untouched. We are not making the halls brighter; we are letting the
        // light that was already there actually arrive.
        //
        // TALL-ROOM KEY CLAMP (F2-F7 wings) — the OTHER half of the same bug, and it lands
        // TOGETHER with the reach fix. The zone keys were authored for the canon floors'
        // 3.5-5 m ceilings. The wing floors are 8-12 m tall, so a ceiling-hung key never
        // touches the props and they read as black blobs. Hang the key no higher than ~4.6 m
        // above the floor (a dropped cable pendant); rooms up to 5.1 m tall are byte-identical
        // (min picks the ceiling), so the calibrated canon-F1 look is untouched.
        // The reach is then sized off the key's ACTUAL hung height, not the ceiling — clamp
        // first, THEN measure the drop, or a 12 m wing room would buy 12 m of range for a
        // lamp that is only 4.6 m up.
        const float keyY     = std::min(cY - 0.5f,  fY + 4.6f);   // pendant (rooms/labs)
        // GAMMA-RECAL: the rows used to hug the ceiling (cY - 0.35). Under the honest
        // curve that painted a HOT patch on the ceiling right over every lamp (p95 ~110
        // vs the reference's 72 in the hall match) while the eye-level walls stayed
        // dark. Hang them as dropped pendants — 1.5 m of cable — so the pool lands on
        // the walls and floor the player actually looks at, and the ceiling above falls
        // toward the reference's darkness. Rooms keep their own keyY (unchanged).
        const float rowY     = std::min(cY - 1.50f, fY + 3.50f);  // corridor/hall light rows
        const float keyReach = std::max(rec.keyRange, (keyY - fY) + 4.0f);
        const float rowReach = std::max(rec.keyRange, (rowY - fY) + 4.0f);
        if (z == ZHall || z == ZCorridor) {
            // GAMMA-RECAL rhythm: DENSE dim warm pendants (4 m pitch, short reach)
            // instead of the old sparse 8 m rhythm. The reference read (old torch on
            // the bent curve) is bright IMMEDIATE walls falling fast into darkness —
            // and the spatial diff of the sparse rig showed exactly the inverse
            // (dark frame-edge walls beside the camera, bright 5-15 m band where the
            // pools landed). A 4 m pitch keeps every wall section within ~2 m of a
            // lamp, so the near field reads warm-lit from ANY standpoint, and the
            // heavy near-black zone fog owns everything beyond — the same
            // camera-centric falloff the torch gave, built from real fixtures.
            const float len = longX ? r.w : r.d;
            const int nKeys = std::max(1, (int)(len / 4.0f));
            for (int i = 0; i < nKeys; ++i) {
                const float t = (i + 0.5f) / nKeys - 0.5f;
                addLight(r.cx + (longX ? t * len : 0), rowY,
                         r.cz + (longX ? 0 : t * len),
                         rowReach, rec.keyR, rec.keyG, rec.keyB);
            }
        } else {
            // Clamped pendant height (main) + reach sized to its real drop (audit).
            addLight(r.cx, keyY, r.cz, keyReach, rec.keyR, rec.keyG, rec.keyB);
            if (r.w * r.d > 40.0f)   // wide room: a dim fill at <= half the key
                addLight(r.cx, fY + 0.6f, r.cz, rec.keyRange * 0.8f,
                         rec.keyR * 0.4f, rec.keyG * 0.4f, rec.keyB * 0.4f);
        }
        // Accent at the first doorway threshold (the zone's ONE hue — on tower
        // corridors/lobbies the FLOOR-IDENTITY hue replaces corridor teal, W8-1).
        {
            const float accR = fid ? fid->r * 1.1f : rec.accR;
            const float accG = fid ? fid->g * 1.1f : rec.accG;
            const float accB = fid ? fid->b * 1.1f : rec.accB;
            for (const CanonDoorway& d : floor.doorways) {
                if (d.a != ri && d.b != ri) continue;
                if (d.kind == DoorwayKind::CrossLevel) continue;
                addLight(d.cx, fY + 2.0f, d.cz, rec.accRange, accR, accG, accB);
                break;
            }
        }

        // ---- W5-2: the WARD DOOR TELL — a thin amber light-under-the-door strip at
        // each ward threshold (RESCUE_SETPIECE_DESIGN.md §1.2: the read before the
        // burst-in is sound + light bleeding under the door, never a visual of the
        // act). Painted-glow discipline like the R2 guide strips: warm, low, floor-
        // level, spanning the door mouth along the wall the cut runs in. Amber = the
        // detention accent (§3), so the tell stays inside the zone's one-hue law.
        // F2's named wards classify ZMedical (the elevation rule wins over the name
        // rule), so cover BOTH: any ZWard room, or a Medical room actually named as
        // a ward (Ward A: Keisha / Ward B: Emily / Ward C: Aria / Sarah's cell).
        const bool wardTell = (z == ZWard) ||
            (z == ZMedical && r.name.find("Ward") != std::string::npos);
        if (wardTell) {
            for (const CanonDoorway& d : floor.doorways) {
                if (d.a != ri && d.b != ri) continue;
                if (d.kind == DoorwayKind::CrossLevel || d.junction) continue;
                const float span = d.cutHalf * 2.0f * 0.92f;   // just inside the jambs
                ProcDraw s; s.room = ri;
                // axis 0: wall plane X=const -> the mouth runs along Z (thin in X).
                s.mesh = quadMesh(device, d.axis == 0 ? 0.14f : span,
                                          d.axis == 0 ? span  : 0.14f, 1.0f);
                makeTR(s.transform, 0, -kPi * 0.5f, d.cx, fY + kFloorLift + 0.003f, d.cz);
                s.color[0] = 0.42f; s.color[1] = 0.26f; s.color[2] = 0.06f; s.color[3] = 1.0f;
                s.emissive[0] = 1.0f; s.emissive[1] = 0.58f; s.emissive[2] = 0.12f;
                s.emissive[3] = 0.85f;
                m_proc.push_back(s);
            }
        }

        // ---- Guide strips (hall/corridor leading line, §3.2 wayfinding). ------------
        if (z == ZHall || z == ZCorridor) {
            const float len = (longX ? r.w : r.d) - 1.2f;
            const int nSeg = std::max(1, (int)(len / 3.5f));
            for (int i = 0; i < nSeg; ++i) {
                const float t = (i + 0.5f) / nSeg - 0.5f;
                ProcDraw s; s.room = ri;
                s.mesh = quadMesh(device, longX ? 2.4f : 0.09f, longX ? 0.09f : 2.4f, 1.0f);
                makeTR(s.transform, 0, -kPi * 0.5f,
                       r.cx + (longX ? t * len : 0), fY + kFloorLift + 0.002f,
                       r.cz + (longX ? 0 : t * len));
                // R2: a PAINTED guide line, not a Tron beam — darker teal, glow well
                // under lamp level (§4 instrument law; round-1 read as a light source).
                // W8-1: on tower floors the guide line takes the floor-identity hue so
                // the corridor keeps ONE accent (never teal + floor hue together).
                if (fid) {
                    s.color[0] = fid->r * 0.30f; s.color[1] = fid->g * 0.30f;
                    s.color[2] = fid->b * 0.30f; s.color[3] = 1.0f;
                    s.emissive[0] = fid->r * 0.50f; s.emissive[1] = fid->g * 0.50f;
                    s.emissive[2] = fid->b * 0.50f; s.emissive[3] = 0.55f;
                } else {
                    s.color[0] = 0.04f; s.color[1] = 0.30f; s.color[2] = 0.34f; s.color[3] = 1.0f;
                    s.emissive[0] = 0.06f; s.emissive[1] = 0.45f; s.emissive[2] = 0.50f;
                    s.emissive[3] = 0.55f;
                }
                m_proc.push_back(s);
            }
        }

        // ---- Hero props on a cut-free face (never block a doorway). -----------------
        if (m_loader) {
            int freeFace = -1;
            for (int f = 0; f < 4; ++f) if (cuts[f].empty()) { freeFace = f; break; }
            // face -> inward normal (nx,nz) + a wall-hug position at the face center
            const float margin = 0.55f;
            float px = r.cx, pz = r.cz, yaw = 0.0f;
            if (freeFace >= 0) {
                switch (freeFace) {
                    case 0: px = r.x0() + margin; pz = r.cz; yaw =  kPi * 0.5f; break;
                    case 1: px = r.x1() - margin; pz = r.cz; yaw = -kPi * 0.5f; break;
                    case 2: px = r.cx; pz = r.z0() + margin; yaw =  kPi;        break;
                    case 3: px = r.cx; pz = r.z1() - margin; yaw =  0.0f;       break;
                }
            }
            const uint32_t seed = ri * 2654435761u;
            const float jitter = ((seed >> 8 & 0xFF) / 255.0f - 0.5f) * 0.8f;
            switch (z) {
                case ZMedical:   // W3-2: the F2 wards (Keisha/Emily/Aria) get real cots
                case ZWard:
                    // The F2 SIGNATURE rescue rooms own their bed/captive/equipment via
                    // the "Rescue Room" desc-gold branch below — skip the generic wall cot
                    // + bin so the bed isn't doubled.
                    if (r.name.find("Rescue Room") == std::string::npos) {
                        if (freeFace >= 0) {
                            // Cot long axis ALONG the wall: local X (2.3 m) -> face tangent.
                            const float cotYaw = (freeFace < 2) ? kPi * 0.5f : 0.0f;
                            placeProp(ri, aCot, cotYaw, 1.0f, acx(kCotAabb), kCotAabb.miny,
                                      acz(kCotAabb), px + (freeFace >= 2 ? jitter : 0),
                                      fY, pz + (freeFace < 2 ? jitter : 0), nullptr, nullptr);
                            shadowBlob(ri, px, fY, pz, 0.8f, 1.4f, 0.5f);
                        }
                        placeProp(ri, aBin, 0.0f, 0.9f, acx(kBinAabb), kBinAabb.miny,
                                  acz(kBinAabb), r.x1() - 0.5f, fY + 0.02f, r.z1() - 0.5f,
                                  nullptr, tSteel);
                    }
                    break;
                case ZSecurity: case ZLobby: case ZLab:
                case ZGenetics: case ZCyber: case ZExec:   // W3-2: console = the focal instrument
                    // R2: the task pool is the room's focal statement — keep it even when
                    // every face carries a doorway (small spine rooms); only the console
                    // prop needs the free wall.
                    if (freeFace < 0)
                        addLight(r.cx, fY + 2.1f, r.cz, rec.keyRange * 0.7f,
                                 rec.keyR * 0.9f, rec.keyG * 0.9f, rec.keyB * 0.9f);
                    if (freeFace >= 0) {
                        placeProp(ri, aConsole, yaw + kPi, 1.0f, acx(kConsAabb),
                                  kConsAabb.miny, kConsAabb.maxz, px, fY, pz,
                                  nullptr, tDarkCon);
                        shadowBlob(ri, px, fY, pz, 0.55f, 0.45f, 0.45f);
                        // Task key OVER the console (the room's focal point).
                        addLight(px, fY + 2.1f, pz, rec.keyRange * 0.7f,
                                 rec.keyR * 0.9f, rec.keyG * 0.9f, rec.keyB * 0.9f);
                    }
                    if (z == ZSecurity)
                        placeProp(ri, aCrateS, 0.3f + jitter, 1.0f, acx(kCrateSAabb),
                                  kCrateSAabb.miny, acz(kCrateSAabb),
                                  r.cx + 1.2f, fY + 0.02f, r.cz - 1.0f, nullptr, tCrate);
                    if (z == ZLab)
                        placeProp(ri, aBin, 0.0f, 0.9f, acx(kBinAabb), kBinAabb.miny,
                                  acz(kBinAabb), r.x0() + 0.5f, fY + 0.02f, r.z0() + 0.5f,
                                  nullptr, tSteel);
                    break;
                case ZBoss:
                    // WAVE (barrels-universal): the boss-room fuel drums are explodable
                    // when the host wires the sink (canon loop -> BarrelSystem); else static.
                    if (m_barrelSink) {
                        m_barrelSink(r.cx - r.w * 0.28f, fY, r.cz - r.d * 0.22f);
                        m_barrelSink(r.cx + r.w * 0.30f, fY, r.cz + r.d * 0.18f);
                    } else {
                        placeProp(ri, aBarrel, 0.0f, 1.0f, acx(kBarrelAabb), kBarrelAabb.miny,
                                  acz(kBarrelAabb), r.cx - r.w * 0.28f, fY + 0.02f,
                                  r.cz - r.d * 0.22f, nullptr, tBarrel);
                        placeProp(ri, aBarrel, 0.9f, 1.0f, acx(kBarrelAabb), kBarrelAabb.miny,
                                  acz(kBarrelAabb), r.cx + r.w * 0.30f, fY + 0.02f,
                                  r.cz + r.d * 0.18f, nullptr, tBarrel);
                    }
                    placeProp(ri, aCrateL, 1.1f + jitter, 1.0f, acx(kCrateLAabb),
                              kCrateLAabb.miny, acz(kCrateLAabb),
                              r.cx + r.w * 0.12f, fY + 0.02f, r.cz - r.d * 0.30f,
                              nullptr, tCrate);
                    shadowBlob(ri, r.cx - r.w * 0.28f, fY, r.cz - r.d * 0.22f, 0.55f, 0.55f, 0.45f);
                    shadowBlob(ri, r.cx + r.w * 0.30f, fY, r.cz + r.d * 0.18f, 0.55f, 0.55f, 0.45f);
                    break;
                case ZDroneBay:   // W3-2: hangar clutter = the storage kit under amber caution
                case ZStorage: {
                    placeProp(ri, aCrateL, jitter * 0.5f, 1.0f, acx(kCrateLAabb),
                              kCrateLAabb.miny, acz(kCrateLAabb), px, fY + 0.02f, pz,
                              nullptr, tCrate);
                    placeProp(ri, aCrateS, jitter * 0.5f, 1.0f, acx(kCrateSAabb),
                              kCrateSAabb.miny, acz(kCrateSAabb), px, fY + 0.62f, pz,
                              nullptr, tCrate);
                    placeProp(ri, aPallet, 0.0f, 1.0f, acx(kPalletAabb), kPalletAabb.miny,
                              acz(kPalletAabb), r.cx, fY + 0.02f, r.cz, nullptr, tPallet);
                    // WAVE (barrels-universal): the storage/hangar fuel drum is explodable
                    // when the host wires the sink (canon loop -> BarrelSystem); else static.
                    if (m_barrelSink) {
                        m_barrelSink(r.cx - 1.4f, fY, r.cz + 1.1f);
                    } else {
                        placeProp(ri, aBarrel, 0.4f, 1.0f, acx(kBarrelAabb), kBarrelAabb.miny,
                                  acz(kBarrelAabb), r.cx - 1.4f, fY + 0.02f, r.cz + 1.1f,
                                  nullptr, tBarrel);
                    }
                    shadowBlob(ri, px, fY, pz, 0.8f, 0.8f, 0.5f);
                    break;
                }
                case ZHall: {
                    // Overhead pipe runs along the hall's long axis (industrial read).
                    const float py = cY - 0.20f;
                    const float pipeYaw = longX ? kPi * 0.5f : 0.0f;
                    placeProp(ri, aPipes, pipeYaw, 1.0f, acx(kPipesAabb), kPipesAabb.maxy,
                              1.5f, r.cx, py, r.cz + (longX ? r.d * 0.28f : 0), nullptr, tSteel);
                    placeProp(ri, aPipes, pipeYaw, 1.0f, acx(kPipesAabb), kPipesAabb.maxy,
                              1.5f, r.cx + (longX ? 0 : r.w * 0.28f), py,
                              r.cz + (longX ? -r.d * 0.28f : 0), nullptr, tBarrel);
                    break;
                }
                default: break;
            }
        }

        // ================================================================================
        // W8-1 DESC GOLD — per-room hero dressing authored from the level data's `desc`
        // fields (the "whole content pass authored in data", CAMPAIGN_LEDGER backlog).
        // Every named story room on F2-F7 gets the scene its one-line desc describes:
        // gurneys + blood trails, growth-tank rows, docked drone bays, the Salvari
        // portal, server rows, executive desks. Purely visual (props/lights/painted
        // stains, no collision). The gameplay VERBS the descs promise are cataloged in
        // docs/DESC_MECHANICS_TODO.md — this pass makes the rooms READ; the mechanics
        // pass makes them PLAY.
        // ================================================================================
        if (m_loader) {
            auto nameHas = [&](const char* s) { return r.name.find(s) != std::string::npos; };
            int mfree = -1;
            for (int f = 0; f < 4; ++f) if (cuts[f].empty()) { mfree = f; break; }
            // A point on face `face` at param t (-0.5..0.5 along the face), inset from
            // the graybox plane.
            auto wallPt = [&](int face, float t, float inset, float& sx, float& sz) {
                sx = r.cx + t * (r.w - 1.4f); sz = r.cz + t * (r.d - 1.4f);
                switch (face) {
                    case 0: sx = r.x0() + inset; break;
                    case 1: sx = r.x1() - inset; break;
                    case 2: sz = r.z0() + inset; break;
                    case 3: sz = r.z1() - inset; break;
                }
            };
            // True if [pos-half..pos+half] on this face is clear of every doorway cut.
            auto faceClear = [&](int face, float pos, float half) {
                for (const Cut& c : cuts[face])
                    if (pos + half > c.lo && pos - half < c.hi) return false;
                return true;
            };
            // Wall-mount yaw families (verified against the cell's R4/R5 mounts).
            auto yawFuse = [](int f) { return f == 0 ?  kPi * 0.5f : f == 1 ? -kPi * 0.5f
                                            : f == 2 ?  0.0f       : kPi; };
            auto yawExt  = [](int f) { return f == 0 ?  kPi        : f == 1 ? 0.0f
                                            : f == 2 ?  kPi * 0.5f : -kPi * 0.5f; };
            auto yawVent = [](int f) { return f == 0 ? -kPi * 0.5f : f == 1 ?  kPi * 0.5f
                                            : f == 2 ?  0.0f       : kPi; };
            auto fusebox = [&](float t) {
                if (mfree < 0) return; float sx, sz; wallPt(mfree, t, 0.10f, sx, sz);
                placeProp(ri, aFuse, yawFuse(mfree), 0.55f, acx(kFuseAabb), 0.0f,
                          kFuseAabb.minz, sx, fY + 1.45f, sz, nullptr, tPanelBox);
            };
            auto extinguisher = [&](float t) {
                if (mfree < 0) return; float sx, sz; wallPt(mfree, t, 0.10f, sx, sz);
                placeProp(ri, aExt, yawExt(mfree), 1.0f, kExtAabb.maxx, kExtAabb.miny,
                          acz(kExtAabb), sx, fY + 0.55f, sz, nullptr, tRed);
            };
            auto ventHi = [&](float t) {
                if (mfree < 0) return; float sx, sz; wallPt(mfree, t, 0.16f, sx, sz);
                placeProp(ri, aVent, yawVent(mfree), 1.0f, acx(kVentAabb), acy(kVentAabb),
                          kVentAabb.minz, sx, fY + 2.6f, sz, nullptr, tSteel);
            };
            auto cornerCam = [&]() {   // +X/+Z upper corner, dark housing + red LED
                placeProp(ri, aCam, kPi * 0.75f, 1.0f, acx(kCamAabb), acy(kCamAabb),
                          acz(kCamAabb), r.x1() - 0.30f, cY - 0.40f, r.z1() - 0.30f,
                          nullptr, tDark);
                addLight(r.x1() - 0.42f, cY - 0.50f, r.z1() - 0.42f, 1.4f, 0.9f, 0.05f, 0.04f);
            };
            auto hangLamp = [&](float x, float zp, float lr, float lg, float lb,
                                float em, float range) {
                const float e[4] = { lr, lg, lb, em };
                // Tall-room clamp (same law as keyY): in the 8-12 m wing halls the
                // pendant drops on its cable to ~4.3 m so its pool reaches the props;
                // canon rooms (<= 4.4 m) are unchanged.
                const float lampY = std::min(cY, fY + 4.3f);
                placeProp(ri, aHang, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, x, lampY - 0.10f, zp,
                          e, tSteel);
                addLight(x, lampY - 1.15f, zp, range, lr, lg, lb);
            };
            auto cot = [&](float x, float zp, float yaw, const float* tint) {
                placeProp(ri, aCot, yaw, 1.0f, acx(kCotAabb), kCotAabb.miny, acz(kCotAabb),
                          x, fY, zp, nullptr, tint);
                const bool alongZ = std::fabs(std::sin(yaw)) > 0.7f;
                shadowBlob(ri, x, fY, zp, alongZ ? 0.75f : 1.30f, alongZ ? 1.30f : 0.75f, 0.45f);
            };
            auto console = [&](float x, float zp, float faceYaw) {   // faceYaw: 0=+Z, pi=-Z
                placeProp(ri, aConsole, faceYaw, 1.0f, acx(kConsAabb), kConsAabb.miny,
                          kConsAabb.maxz, x, fY, zp, nullptr, tDarkCon);
                shadowBlob(ri, x, fY, zp, 0.55f, 0.45f, 0.40f);
            };
            auto barrel = [&](float x, float zp, float yaw, const float* tint) {
                placeProp(ri, aBarrel, yaw, 1.0f, acx(kBarrelAabb), kBarrelAabb.miny,
                          acz(kBarrelAabb), x, fY + 0.02f, zp, nullptr, tint);
            };
            auto crateSm = [&](float x, float zp, float yaw, const float* tint, float lift) {
                placeProp(ri, aCrateS, yaw, 1.0f, acx(kCrateSAabb), kCrateSAabb.miny,
                          acz(kCrateSAabb), x, fY + lift, zp, nullptr, tint);
            };
            auto crateLg = [&](float x, float zp, float yaw, const float* tint, float lift) {
                placeProp(ri, aCrateL, yaw, 1.0f, acx(kCrateLAabb), kCrateLAabb.miny,
                          acz(kCrateLAabb), x, fY + lift, zp, nullptr, tint);
            };
            auto palletAt = [&](float x, float zp) {
                placeProp(ri, aPallet, 0.0f, 1.0f, acx(kPalletAabb), kPalletAabb.miny,
                          acz(kPalletAabb), x, fY + 0.02f, zp, nullptr, tPallet);
            };
            auto droneAt = [&](float x, float y, float zp, float yaw, const float* tint) {
                placeProp(ri, aDrone, yaw, 1.0f, 0.0f, kDroneAabb.miny, 0.0f, x, y, zp,
                          nullptr, tint);
            };
            // Painted emissive quad (glow strip / pad / specimen shimmer). pitch -pi/2
            // lays it flat on the floor (or a pedestal top at y).
            auto glowQuad = [&](float w, float h, float x, float y, float zp, float yaw,
                                float pitch, float cr, float cg, float cb, float em) {
                ProcDraw s; s.room = ri;
                s.mesh = quadMesh(device, w, h, 1.0f);
                makeTR(s.transform, yaw, pitch, x, y, zp);
                s.color[0] = cr * 0.30f; s.color[1] = cg * 0.30f; s.color[2] = cb * 0.30f;
                s.color[3] = 1.0f;
                s.emissive[0] = cr; s.emissive[1] = cg; s.emissive[2] = cb; s.emissive[3] = em;
                m_proc.push_back(s);
            };
            // Translucent shimmer plane (the portal / holo art / observation glass).
            auto glassQuad = [&](float w, float h, float x, float y, float zp, float yaw,
                                 float cr, float cg, float cb, float alpha, float em) {
                ProcDraw p; p.room = ri; p.glass = true;
                p.mesh = quadMesh(device, w, h, 1.0f);
                makeTR(p.transform, yaw, 0, x, y, zp);
                p.color[0] = cr; p.color[1] = cg; p.color[2] = cb; p.color[3] = alpha;
                p.emissive[0] = cr; p.emissive[1] = cg; p.emissive[2] = cb; p.emissive[3] = em;
                m_proc.push_back(p);
            };
            const float cx0 = r.cx, cz0 = r.cz;

            // ---------------- F2 SIGNATURE RESCUE ROOMS ----------------
            // The owner's spec: a WHITE clinical rescue room — a 2.3 m hospital bed
            // CENTERED with >=1.2 m walk-around, the captive RESTRAINED SUPINE on it
            // (straps over the blanket, non-gratuitous clinical dread), ACTIVE monitoring
            // equipment, a bedside vitals console + IV drum, and ONE surgical key light
            // overhead under a dropped clinical ceiling. Room B (Keisha) is the
            // magnetically SEALED room — a RED locked door tell frames its door (the lock
            // MECHANIC is a host hook). Aria=AnnaCasual, Keisha=AnnaBodySuit,
            // Emily=AnnaTactical (the same live cast rescue.cpp uses).
            if (nameHas("Rescue Room")) {
                const bool sealed = nameHas("Keisha");
                const uint32_t cap = nameHas("Aria")   ? aAria
                                   : nameHas("Keisha") ? aKeisha : aEmily;

                // (1) Dropped clinical ceiling at ~3.3 m so the tall wing plate reads as an
                // intimate clinical room (the surgical pendant hangs just under it).
                const float dropY = fY + 3.3f;
                {
                    Panel p; p.room = ri; p.set = ceilSet;
                    p.mesh = quadMesh(device, r.w - 0.1f, r.d - 0.1f, rec.ceilTile);
                    makeTR(p.transform, 0, kPi * 0.5f, r.cx, dropY, r.cz);
                    m_panels.push_back(p); ++nPanels;
                }

                // (2) The bed, CENTERED, long axis ALONG Z (head to the back/north wall).
                cot(cx0, cz0, kPi * 0.5f, tClinic);
                ++m_rescueBeds;

                // (3) The captive laid SUPINE on the bed. The GLB is a PRE-POSED supine
                // bake (face UP, head toward the +Z pillow/incline end, feet toward the
                // door, back at model-Y 0), so this is just position + a small per-girl
                // yaw — no 90° roll of a standing model. Her spine is baked to the bed's
                // head incline (head rests ON the pillow, not buried), knees up, legs
                // apart, arms at her sides. keepTex keeps her authored PBR skin/clothes
                // (bypasses the wing black-prop material lift).
                if (m_loader && cap < m_assetTable.size() && m_assetTable[cap].ok) {
                    // Orientation, settled by direct orthographic diagnosis (session
                    // lead, Blender top+side renders of the exported GLBs): the models
                    // are ANATOMICALLY PERFECT — supine, face up, knees up, legs apart —
                    // but their NATIVE in-engine head points to -Z (feet +Z) while the
                    // bed's pillow/incline is at +Z. So all three need a 180° yaw to land
                    // the head on the pillow. The prior "no flip" placement put every head
                    // at the FOOT of the bed (owner caught it). kPi + per-girl delta.
                    const float capYaw = nameHas("Aria")   ? (kPi + 0.04f)
                                       : nameHas("Keisha") ? (kPi - 0.05f)
                                                           : (kPi + 0.02f);
                    const float mattressY = fY + 0.55f;   // mattress sleeping surface
                    PropInst pc; pc.room = ri; pc.asset = cap; pc.keepTex = true;
                    // With the head now correctly at the +Z pillow (post-flip), shift her
                    // TOWARD the pillow (+Z) so the whole body lies ON the mattress and the
                    // lower legs don't hang off the foot rail (the prior -Z shift was tuned
                    // for the wrong orientation and slid her off the foot end).
                    makeTR(pc.transform, capYaw, 0.0f, cx0, mattressY, cz0 + 0.28f);
                    m_props.push_back(pc);
                    ++m_rescueCaptives;
                    // (3b) Restraint straps re-seated to the supine knees-up pose: a chest
                    // strap over the torso and a leg strap across the raised SHINS below the
                    // knees. Dark bands laid flat over the body, low emissive so they read
                    // without glowing.
                    auto strap = [&](float zp, float yy, float wid) {
                        ProcDraw s; s.room = ri;
                        s.mesh = quadMesh(device, wid, 0.14f, 1.0f);
                        makeTR(s.transform, 0.0f, -kPi * 0.5f, cx0, yy, zp);
                        s.color[0] = 0.05f; s.color[1] = 0.05f; s.color[2] = 0.06f; s.color[3] = 1.0f;
                        s.emissive[0] = 0.10f; s.emissive[1] = 0.10f; s.emissive[2] = 0.12f;
                        s.emissive[3] = 0.30f;
                        m_proc.push_back(s);
                    };
                    strap(cz0 + 0.02f, mattressY + 0.24f, 1.05f);   // chest (over torso)
                    strap(cz0 - 0.62f, mattressY + 0.30f, 1.25f);   // legs (across raised shins)
                }

                // (4) Advanced medical equipment: a bedside vitals console, an IV/cryo
                // drum, and the DARK-GLASS ROUNDED VITALS MONITOR on the head wall.
                // Owner note #1: the flat emissive TEAL quad is replaced with the same
                // black-glass rounded-screen LANGUAGE as Jake's-cell terminal — a near-
                // black rounded pane baked with a green ECG heart-rate trace + glowing
                // vitals rows under the captive's NAME, driven as an emissiveTex over a
                // near-black albedo (the ACES texture-gated glow law) so it reads as a
                // LIVE dark-glass monitor, never a bright slab.
                console(cx0 - 2.4f, cz0 - 1.2f, kPi * 0.5f);        // bedside vitals cart
                barrel(cx0 + 2.3f, cz0 - 1.0f, 0.3f, tCryo);        // IV drip drum
                {
                    const std::string capName = nameHas("Aria")   ? "ARIA"
                                              : nameHas("Keisha") ? "KEISHA" : "EMILY";
                    std::vector<uint8_t> mon = bakeMedicalMonitor(1024, capName);
                    const x3::rhi::TextureHandle monTex =
                        device.createTexture(mon.data(), 1024, 1024, /*srgb*/true);
                    // A dedicated screen quad with MIRRORED U — mounted on the +Z (head)
                    // wall via a kPi yaw (the wall-panel convention), which flips X, so the
                    // readout text reads upright to the player (same reason HoloTerminal
                    // pre-flips its back-fan U). 1.5 x 1.0 m dark-glass monitor.
                    const float hw = 0.75f, hh = 0.50f;
                    x3::rhi::MeshVertex mv[4] = {};
                    const float mpx[4] = { -hw,  hw,  hw, -hw };
                    const float mpy[4] = { -hh, -hh,  hh,  hh };
                    const float muu[4] = { 1.0f, 0.0f, 0.0f, 1.0f };   // mirrored U
                    const float mvv[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
                    for (int i = 0; i < 4; ++i) {
                        mv[i].pos[0] = mpx[i]; mv[i].pos[1] = mpy[i]; mv[i].pos[2] = 0.0f;
                        mv[i].normal[2] = 1.0f; mv[i].uv[0] = muu[i]; mv[i].uv[1] = mvv[i];
                    }
                    const uint32_t midx[12] = { 0,1,2, 0,2,3,  0,2,1, 0,3,2 };
                    ProcDraw sc; sc.room = ri; sc.tex = monTex;
                    sc.mesh = device.createMesh(mv, 4, midx, 12);
                    makeTR(sc.transform, kPi, 0.0f, cx0, fY + 2.05f, r.z1() - kInset - 0.04f);
                    // Near-black glass albedo + neutral ~1.15 emissive multiplier so the
                    // baked green/cyan status colors survive (texture-gated glow).
                    sc.color[0] = 0.030f; sc.color[1] = 0.033f; sc.color[2] = 0.042f;
                    sc.color[3] = 1.0f;
                    sc.emissive[0] = 1.0f; sc.emissive[1] = 1.0f; sc.emissive[2] = 1.0f;
                    sc.emissive[3] = 1.15f;
                    m_proc.push_back(sc);
                }

                // (5) SOFTENED surgical EXAM light (owner note #2a): the single hot pendant
                // was blowing out. Drop the fixture emissive so it isn't a white slab, and
                // make the key WARM-NEUTRAL, LOWER, and WIDER so it pools on the bed like a
                // real clinical exam light instead of a blowout hot spot.
                {
                    // NOTE: the Hanging Light GLB carries its OWN (bright) emissive
                    // material, so the draw path takes the material's emissive RGB and
                    // uses only ePend[3] as the SCALE — the RGB here is inert. The old
                    // scale 1.0 clipped the fixture to a blown white slab under ACES; drop
                    // the scale so the shade reads as a soft warm lamp, not a hot slab.
                    const float ePend[4] = { 0.85f, 0.80f, 0.68f, 0.30f };   // dimmed warm fixture
                    placeProp(ri, aHang, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
                              cx0, dropY - 0.12f, cz0, ePend, tSteel);
                    addLight(cx0, fY + 2.55f, cz0, 4.6f, 1.02f, 0.94f, 0.80f);  // soft warm exam pool
                }

                // (5b) WALL COVE FILLS (owner note #2b): a few COOLER, low-energy fills up
                // the two side walls so the room has shape — a clinical cove-light feel —
                // instead of one hot pendant + black corners. Each is well under half the
                // warm key energy so the pendant stays the ONE motivated statement.
                {
                    const float coveY = dropY - 0.45f;
                    const float cR = 0.34f, cG = 0.44f, cB = 0.58f;   // cool clinical fill
                    addLight(r.x0() + 0.55f, coveY, cz0 - 1.9f, 3.0f, cR, cG, cB);
                    addLight(r.x0() + 0.55f, coveY, cz0 + 1.9f, 3.0f, cR, cG, cB);
                    addLight(r.x1() - 0.55f, coveY, cz0 - 1.9f, 3.0f, cR, cG, cB);
                    addLight(r.x1() - 0.55f, coveY, cz0 + 1.9f, 3.0f, cR, cG, cB);
                }

                // (6) Room B magnetic-seal tell: a RED locked door frame + bleed-under-the-
                // door glow (GEOMETRY/light only — the lock MECHANIC is a host hook).
                if (sealed) {
                    const float rz = r.z0() + kInset + 0.05f;
                    glowQuad(0.09f, 2.10f, cx0 - 0.72f, fY + 1.10f, rz, 0.0f, 0.0f,
                             1.00f, 0.05f, 0.04f, 0.95f);
                    glowQuad(0.09f, 2.10f, cx0 + 0.72f, fY + 1.10f, rz, 0.0f, 0.0f,
                             1.00f, 0.05f, 0.04f, 0.95f);
                    glowQuad(1.62f, 0.12f, cx0, fY + 2.20f, rz, 0.0f, 0.0f,
                             1.00f, 0.05f, 0.04f, 0.95f);
                    glowQuad(1.10f, 0.12f, cx0, fY + kFloorLift + 0.006f, r.z0() + 0.20f, 0.0f,
                             -kPi * 0.5f, 1.00f, 0.06f, 0.05f, 0.85f);
                    addLight(cx0, fY + 1.4f, r.z0() + 0.5f, 2.0f, 1.10f, 0.06f, 0.05f);
                }

            // ---------------- F2 MEDICAL BAY ----------------
            } else if (nameHas("F2: Main Corridor")) {
                // "Gurneys line walls. Blood trails." (faceClear: never park a gurney
                // in front of a ward/theater door)
                if (faceClear(0, cz0 - 3.4f, 1.5f)) cot(r.x0() + 0.95f, cz0 - 3.4f, kPi * 0.5f, tClinic);
                if (faceClear(1, cz0 + 2.6f, 1.5f)) cot(r.x1() - 0.95f, cz0 + 2.6f, kPi * 0.5f, tClinic);
                bloodBlob(ri, cx0 - 0.6f, fY, cz0 - 1.0f, 0.45f, 1.70f, 0.50f);
                bloodBlob(ri, cx0 + 0.8f, fY, cz0 + 4.0f, 0.40f, 1.30f, 0.40f);
            } else if (nameHas("Operating Theater A")) {
                // "Active surgery tables. Alien instruments."
                cot(cx0 - 2.3f, cz0, 0.0f, tClinic);
                cot(cx0 + 2.3f, cz0, 0.0f, tClinic);
                hangLamp(cx0 - 2.3f, cz0, 1.60f, 1.70f, 1.65f, 1.2f, 3.0f);
                hangLamp(cx0 + 2.3f, cz0, 1.60f, 1.70f, 1.65f, 1.2f, 3.0f);
                crateSm(cx0 + 4.4f, cz0 - 2.2f, 0.4f, tClinic, 0.02f);   // instrument cart
                bloodBlob(ri, cx0 - 2.0f, fY, cz0 + 0.9f, 0.55f, 0.75f, 0.55f);
            } else if (nameHas("Operating Theater B")) {
                // "Failed experiments. Containment pods cracked."
                barrel(cx0 - 3.6f, cz0 - 2.2f, 0.3f, tSpecimen);
                barrel(cx0 - 2.4f, cz0 - 2.6f, 1.4f, tSpecimen);
                barrel(cx0 - 3.0f, cz0 + 2.4f, 0.8f, tSpecimen);
                cot(cx0 + 2.5f, cz0, 0.0f, tClinic);
                bloodBlob(ri, cx0 + 1.4f, fY, cz0 + 0.8f, 0.65f, 0.55f, 0.50f);
                addLight(cx0 - 3.0f, fY + 2.2f, cz0 - 2.2f, 2.6f, 0.30f, 1.05f, 0.35f);
            } else if (z == ZMedical && nameHas("Ward ")) {
                // Ward density: instrument cart + a recessed vent (the cot/bin come
                // from the zone recipe).
                crateSm(cx0 + r.w * 0.28f, cz0 - r.d * 0.22f, 0.25f, tClinic, 0.02f);
                ventHi(0.30f);
            } else if (nameHas("Pharmacy")) {
                // "Medical supplies. Antidote components."
                crateSm(cx0 - 2.2f, cz0 - 1.6f, 0.0f, tClinic, 0.02f);
                crateSm(cx0 - 2.2f, cz0 - 1.6f, 0.3f, tClinic, 0.62f);
                crateLg(cx0 + 2.0f, cz0 - 1.7f, 0.0f, tClinic, 0.02f);
                fusebox(0.25f);
            } else if (nameHas("Quarantine Zone")) {
                // "Sealed chamber. Infection research."
                barrel(cx0 - 1.8f, cz0 + 1.2f, 0.5f, tSpecimen);
                barrel(cx0 + 1.6f, cz0 - 1.0f, 1.1f, tSpecimen);
                ventHi(-0.30f);
                bloodBlob(ri, cx0 + 0.2f, fY, cz0 + 0.3f, 0.50f, 0.50f, 0.40f);
            } else if (nameHas("Chen's Office")) {
                // "Personal logs. Enhancement data." (display name is not "Chen" —
                // OPEN ledger item #1; this matches the DATA room name only).
                crateLg(cx0 - 0.6f, cz0 + 0.8f, 0.0f, tDarkCon, 0.02f);   // the desk
                console(cx0 - 0.6f, cz0 + 1.9f, kPi);
                hangLamp(cx0 - 0.6f, cz0 + 0.3f, 2.20f, 1.70f, 1.05f, 1.0f, 3.2f);
            } else if (nameHas("Recovery Ward")) {
                // The big medical hall: two rows of recovery cots down the long (Z) axis
                // hugging the side (X) walls, an IV drip drum beside each, warm surgical
                // pendants overhead, a nurse-station console at the head wall, a blood
                // trail. Dressed to FILL the signature space (door-safe via faceClear).
                for (int i = -2; i <= 2; ++i) {
                    const float zp = cz0 + i * 5.2f;
                    if (faceClear(0, zp, 1.7f)) {
                        cot(r.x0() + 1.1f, zp, kPi * 0.5f, tClinic);
                        barrel(r.x0() + 2.7f, zp + 1.5f, 0.3f * (i + 2), tCryo);
                    }
                    if (faceClear(1, zp, 1.7f)) {
                        cot(r.x1() - 1.1f, zp, kPi * 0.5f, tClinic);
                        barrel(r.x1() - 2.7f, zp + 1.5f, 0.9f * (i + 2), tCryo);
                    }
                    hangLamp(cx0, zp, 1.60f, 1.72f, 1.66f, 1.0f, 3.6f);
                }
                console(cx0, r.z0() + 0.9f, 0.0f);
                addLight(cx0, fY + 2.4f, r.z0() + 1.5f, 3.4f, 0.30f, 1.05f, 0.42f);
                bloodBlob(ri, cx0 - 1.4f, fY, cz0 + 2.0f, 0.5f, 1.6f, 0.45f);

            // ---------------- F3 GENETICS LAB ----------------
            } else if (nameHas("Specimen Hall")) {
                // "Growth tanks. Bubbling fluid. Alien DNA."
                const float bx[4] = { r.x0() + 0.85f, r.x0() + 0.85f, r.x1() - 0.85f, r.x1() - 0.85f };
                const float bz[4] = { cz0 - 6.0f, cz0 + 2.0f, cz0 - 2.0f, cz0 + 6.0f };
                const int   bf[4] = { 0, 0, 1, 1 };
                for (int i = 0; i < 4; ++i) {
                    if (!faceClear(bf[i], bz[i], 1.1f)) continue;   // never block a lab door
                    barrel(bx[i], bz[i], 0.4f * i, tSpecimen);
                    glowQuad(0.95f, 0.95f, bx[i], fY + kFloorLift + 0.004f, bz[i], 0,
                             -kPi * 0.5f, 0.20f, 1.00f, 0.40f, 0.35f);
                }
            } else if (nameHas("DNA Sequencing Lab")) {
                // "Holographic DNA strands. 4 terminals." (zone console = the 4th)
                console(cx0 - 2.4f, r.z0() + 0.75f, 0.0f);
                console(cx0,        r.z0() + 0.75f, 0.0f);
                console(cx0 + 2.4f, r.z0() + 0.75f, 0.0f);
                addLight(cx0, fY + 2.2f, r.z0() + 1.3f, 2.6f, 0.25f, 1.10f, 0.35f);
            } else if (nameHas("Hybridization Chamber")) {
                // "Cross-species tanks. Failed hybrids."
                barrel(cx0 - 3.2f, cz0 - 1.8f, 0.2f, tSpecimen);
                barrel(cx0 - 2.2f, cz0 - 2.3f, 1.0f, tSpecimen);
                barrel(cx0 + 3.0f, cz0 + 1.6f, 1.8f, tSpecimen);
                crateSm(cx0 + 3.8f, cz0 - 2.2f, 0.5f, tCrate, 0.02f);
                bloodBlob(ri, cx0 + 0.5f, fY, cz0 - 0.5f, 0.55f, 0.55f, 0.45f);
            } else if (nameHas("Growth Tank Array")) {
                // "20 tanks. 3 active creatures growing." (6 read as the array; the
                // 3 ACTIVE ones carry the underglow)
                int active = 0;
                for (int ix = -1; ix <= 1; ++ix) for (int iz = 0; iz < 2; ++iz) {
                    const float bx = cx0 + ix * 2.4f, bz = cz0 + (iz ? 1.5f : -1.5f);
                    barrel(bx, bz, 0.35f * (ix + iz * 2), tSpecimen);
                    if (((ix + 2) + iz * 3) % 2 == 0 && active < 3) {
                        glowQuad(0.95f, 0.95f, bx, fY + kFloorLift + 0.004f, bz, 0,
                                 -kPi * 0.5f, 0.22f, 0.95f, 0.35f, 0.50f);
                        ++active;
                    }
                }
                addLight(cx0, fY + 2.0f, cz0, 3.0f, 0.22f, 0.95f, 0.35f);
            } else if (nameHas("Spawning Chamber")) {
                // "Alien egg pods. Pulsing organic walls." (ZOrganic — the two-accent
                // exception: biolume green + the blood-brown floor)
                const float ex[5] = { -2.8f, -1.2f, 2.2f, 3.0f, -2.0f };
                const float ez[5] = { -1.8f, -2.4f, -1.2f, 1.8f, 2.2f };
                for (int i = 0; i < 5; ++i)
                    barrel(cx0 + ex[i], cz0 + ez[i], 0.35f + 0.6f * i, tPod);
                floorBlob(ri, cx0 - 1.8f, fY, cz0 - 1.6f, 1.6f, 1.2f, 0.10f, 0.09f, 0.03f, 0.55f);
                floorBlob(ri, cx0 + 2.4f, fY, cz0 + 0.6f, 1.2f, 1.5f, 0.10f, 0.09f, 0.03f, 0.50f);
                glowQuad(0.8f, 0.8f, cx0 - 1.2f, fY + kFloorLift + 0.004f, cz0 - 2.4f, 0,
                         -kPi * 0.5f, 0.25f, 1.00f, 0.40f, 0.40f);
                addLight(cx0 + 2.2f, fY + 1.4f, cz0 - 1.2f, 2.6f, 0.25f, 1.00f, 0.40f);
            } else if (nameHas("Clone Storage")) {
                // "Deactivated clones. Jake clone data."
                cot(cx0 - 2.6f, cz0, 0.0f, tCryo);
                cot(cx0 + 2.6f, cz0, 0.0f, tCryo);
                console(cx0, r.z0() + 0.70f, 0.0f);
                addLight(cx0, fY + 2.1f, cz0, 2.8f, 1.00f, 1.15f, 1.40f);
            } else if (nameHas("Cold Room")) {
                // "-40C. Timer: 30s before damage."
                barrel(cx0 - 1.8f, cz0 - 1.2f, 0.3f, tCryo);
                barrel(cx0 + 1.6f, cz0 + 1.0f, 1.2f, tCryo);
                fusebox(-0.25f);
                addLight(cx0, fY + 2.2f, cz0, 3.0f, 0.90f, 1.15f, 1.50f);
            } else if (nameHas("Decontamination")) {
                // "UV flood chamber. Kills infection."
                ventHi(0.25f); ventHi(-0.25f);
                addLight(cx0, cY - 0.6f, cz0, 3.0f, 0.75f, 0.35f, 1.20f);
            } else if (nameHas("Gene Vat Gallery")) {
                // The signature genetics hall: two long rows of bubbling growth vats down
                // the side (X) walls, each underlit green, a central pair of sequencing
                // consoles, a sickly-green key. Fills the big hall with the rows-of-tanks
                // read (door-safe on the +X wall).
                for (int i = -3; i <= 3; ++i) {
                    const float zp = cz0 + i * 3.7f;
                    barrel(r.x0() + 1.0f, zp, 0.3f * (i + 3), tSpecimen);
                    glowQuad(0.9f, 0.9f, r.x0() + 1.0f, fY + kFloorLift + 0.004f, zp, 0,
                             -kPi * 0.5f, 0.20f, 1.00f, 0.40f, 0.40f);
                    if (faceClear(1, zp, 1.1f)) {
                        barrel(r.x1() - 1.0f, zp, 0.5f * (i + 3), tSpecimen);
                        glowQuad(0.9f, 0.9f, r.x1() - 1.0f, fY + kFloorLift + 0.004f, zp, 0,
                                 -kPi * 0.5f, 0.20f, 1.00f, 0.40f, 0.40f);
                    }
                }
                console(cx0 - 1.6f, cz0 - 2.0f, 0.0f);
                console(cx0 + 1.6f, cz0 + 2.0f, kPi);
                addLight(cx0, fY + 2.6f, cz0, 3.6f, 0.22f, 1.10f, 0.38f);

            // ---------------- F4 CYBERNETICS WING ----------------
            } else if (nameHas("Augmentation Corridor")) {
                // "Augmentation chairs line both sides." (door-safe)
                if (faceClear(0, cz0 - 5.0f, 1.5f)) cot(r.x0() + 0.95f, cz0 - 5.0f, kPi * 0.5f, tSteel);
                if (faceClear(1, cz0,        1.5f)) cot(r.x1() - 0.95f, cz0,        kPi * 0.5f, tSteel);
                if (faceClear(0, cz0 + 5.0f, 1.5f)) cot(r.x0() + 0.95f, cz0 + 5.0f, kPi * 0.5f, tSteel);
            } else if (nameHas("Augmentation Bay")) {
                // "8 aug chairs. Strength/speed/armor." (4 read the array)
                cot(cx0 - 2.2f, cz0 - 1.6f, 0.0f, tSteel);
                cot(cx0 + 2.2f, cz0 - 1.6f, 0.0f, tSteel);
                cot(cx0 - 2.2f, cz0 + 1.6f, 0.0f, tSteel);
                cot(cx0 + 2.2f, cz0 + 1.6f, 0.0f, tSteel);
                addLight(cx0, fY + 2.3f, cz0, 3.4f, 0.16f, 0.85f, 1.05f);
            } else if (nameHas("Neural Interface Lab")) {
                // "Brain-computer links. Memory extraction."
                console(cx0 - 2.0f, r.z0() + 0.70f, 0.0f);
                console(cx0 + 2.0f, r.z0() + 0.70f, 0.0f);
                cornerCam();
            } else if (nameHas("Prototype Testing")) {
                // "Testing arena. Obstacle course." — a crate slalom
                crateLg(cx0 - 2.6f, cz0 - 2.0f,  0.35f, tCrate, 0.02f);
                crateSm(cx0 - 0.6f, cz0 - 0.4f,  1.20f, tCrate, 0.02f);
                crateLg(cx0 + 1.6f, cz0 + 1.2f, -0.40f, tCrate, 0.02f);
                crateSm(cx0 + 3.0f, cz0 + 2.4f,  0.80f, tCrate, 0.02f);
                palletAt(cx0 - 3.2f, cz0 + 2.2f);
            } else if (nameHas("Workshop")) {
                // "Cybernetic limbs. Tools. Schematics."
                palletAt(cx0 - 1.5f, cz0);
                crateLg(cx0 - 1.5f, cz0, 0.15f, tCrate, 0.22f);   // bench on the pallet
                extinguisher(0.30f);
                fusebox(-0.30f);
            } else if (nameHas("Coolant System")) {
                // "Liquid nitrogen. Sabotage = boss weakness." (mechanic: TODO doc)
                barrel(cx0 - 1.8f, cz0 - 1.2f, 0.2f, tCryo);
                barrel(cx0 - 0.6f, cz0 - 1.5f, 0.9f, tCryo);
                barrel(cx0 + 1.4f, cz0 - 1.0f, 1.6f, tCryo);
                console(cx0 + 1.6f, cz0 + 1.2f, kPi);   // the coolant control anchor
                placeProp(ri, aPipes, kPi * 0.5f, 1.0f, acx(kPipesAabb), kPipesAabb.maxy,
                          1.5f, cx0, cY - 0.20f, cz0 - 1.0f, nullptr, tSteel);
                addLight(cx0, cY - 0.4f, cz0, 3.2f, 0.80f, 1.00f, 1.20f);
            } else if (nameHas("Power Junction")) {
                // "EMP device craftable here." (mechanic: TODO doc)
                fusebox(0.0f); fusebox(0.32f); fusebox(-0.32f);
                crateLg(cx0 - 1.5f, cz0 + 0.8f, 0.0f, tCrate, 0.02f);   // the EMP bench
                console(cx0 + 1.2f, cz0 + 0.9f, -kPi * 0.5f);

            // ---------------- F5 DRONE STATION ----------------
            } else if (nameHas("F5: Main Corridor")) {
                // "Drone launch rails on ceiling."
                placeProp(ri, aDuct, 0.0f, 1.0f, acx(kDuctAabb), kDuctAabb.maxy,
                          acz(kDuctAabb), cx0 - 1.2f, cY - 0.16f, cz0 - 4.0f, nullptr, tSteel);
                placeProp(ri, aDuct, 0.0f, 1.0f, acx(kDuctAabb), kDuctAabb.maxy,
                          acz(kDuctAabb), cx0 + 1.2f, cY - 0.16f, cz0 + 4.0f, nullptr, tSteel);
                droneAt(cx0, cY - 1.1f, cz0 - 3.0f, 0.4f, tAirframe);   // one on the rail
            } else if (nameHas("Drone Bay Alpha")) {
                // "40 surveillance drones docked." (4 + racks read the bay)
                palletAt(cx0 - 2.8f, cz0 - 2.0f);
                palletAt(cx0 + 2.4f, cz0 - 2.0f);
                droneAt(cx0 - 2.8f, fY + 0.52f, cz0 - 2.0f,  0.2f, tAirframe);
                droneAt(cx0 + 2.4f, fY + 0.52f, cz0 - 2.0f, -0.3f, tAirframe);
                crateLg(cx0 - 0.4f, cz0 + 2.0f, 0.10f, tCrate, 0.02f);
                droneAt(cx0 - 0.4f, fY + 0.94f, cz0 + 2.0f, 0.9f, tAirframe);
                droneAt(cx0 + 3.4f, fY + 0.34f, cz0 + 1.4f, 1.8f, tAirframe);
                shadowBlob(ri, cx0 - 2.8f, fY, cz0 - 2.0f, 1.1f, 1.1f, 0.45f);
                shadowBlob(ri, cx0 + 2.4f, fY, cz0 - 2.0f, 1.1f, 1.1f, 0.45f);
                cornerCam();
            } else if (nameHas("Drone Bay Beta")) {
                // "30 combat drones. Heavy armor." — darker airframes
                palletAt(cx0 - 2.6f, cz0 - 1.8f);
                droneAt(cx0 - 2.6f, fY + 0.52f, cz0 - 1.8f, 2.6f, tPanelBox);
                droneAt(cx0 + 2.2f, fY + 0.34f, cz0 - 2.2f, 2.4f, tPanelBox);
                droneAt(cx0 + 0.2f, fY + 0.34f, cz0 + 2.4f, 4.0f, tPanelBox);
                barrel(cx0 - 3.8f, cz0 + 2.2f, 0.4f, tBarrel);
                crateLg(cx0 - 1.2f, cz0 - 3.2f, 0.0f, tCrate, 0.02f);
            } else if (nameHas("Central Control Hub")) {
                // "Master hack terminal. Sarah's objective." — a console arc, hot task pool
                console(cx0 - 2.6f, cz0 - 2.2f, 0.0f);
                console(cx0,        cz0 - 2.6f, 0.0f);
                console(cx0 + 2.6f, cz0 - 2.2f, 0.0f);
                addLight(cx0, fY + 2.5f, cz0 - 2.2f, 4.0f, 1.75f, 1.65f, 1.45f);
                cornerCam();
            } else if (nameHas("Maintenance Bay")) {
                // "Drone repair. Spare parts." — one airframe opened on a pallet
                palletAt(cx0, cz0 - 1.0f);
                droneAt(cx0, fY + 0.52f, cz0 - 1.0f, 2.6f, tAirframe);
                crateSm(cx0 - 2.8f, cz0 + 1.6f, 0.3f, tCrate, 0.02f);
                crateSm(cx0 - 2.1f, cz0 + 2.1f, 0.9f, tCrate, 0.02f);
                extinguisher(0.30f);
                fusebox(-0.30f);
                shadowBlob(ri, cx0, fY, cz0 - 1.0f, 1.1f, 1.1f, 0.45f);
            } else if (nameHas("Weapons Locker")) {
                // "Plasma cores. EMP warheads." — cyan core drums stay instrument-dim
                crateLg(cx0 - 2.4f, cz0 - 1.8f, 0.0f, tCrate, 0.02f);
                crateLg(cx0 - 2.4f, cz0 + 1.4f, 0.0f, tCrate, 0.02f);
                crateSm(cx0 + 2.0f, cz0 - 2.0f, 0.0f, tCrate, 0.02f);
                crateSm(cx0 + 2.0f, cz0 - 2.0f, 0.4f, tCrate, 0.62f);
                barrel(cx0 + 2.8f, cz0 + 1.8f, 0.3f, tCryo);
                barrel(cx0 + 3.7f, cz0 + 1.3f, 1.0f, tCryo);
                glowQuad(1.6f, 1.2f, cx0 + 3.2f, fY + kFloorLift + 0.004f, cz0 + 1.55f, 0,
                         -kPi * 0.5f, 0.30f, 0.80f, 1.00f, 0.30f);
            } else if (nameHas("Recharge Station")) {
                // "Energy cores. Battery packs." — charge pads + junction boxes
                fusebox(0.0f); fusebox(0.30f); fusebox(-0.30f);
                barrel(cx0 - 2.6f, cz0 + 0.8f, 0.2f, tBarrel);
                barrel(cx0 + 2.6f, cz0 - 0.8f, 0.9f, tBarrel);
                glowQuad(1.8f, 1.2f, cx0 - 2.6f, fY + kFloorLift + 0.004f, cz0 + 0.8f, 0,
                         -kPi * 0.5f, 1.00f, 0.62f, 0.10f, 0.45f);
                glowQuad(1.8f, 1.2f, cx0 + 2.6f, fY + kFloorLift + 0.004f, cz0 - 0.8f, 0,
                         -kPi * 0.5f, 1.00f, 0.62f, 0.10f, 0.45f);
            } else if (nameHas("Assembly Bay")) {
                // The huge drone-assembly hangar — the F5 signature. A grid of docked-drone
                // stations on pallets fills the bay, each over an amber hazard charge pad;
                // conveyor crate lines run the aisles; overhead gantry pipes span the hall;
                // a corner watch-cam. Dressed to fill the VOLUME, not a lone prop.
                for (int gx = -2; gx <= 2; ++gx) {
                    for (int gz = -1; gz <= 1; ++gz) {
                        const float x = cx0 + gx * 9.0f, zp = cz0 + gz * 12.0f;
                        palletAt(x, zp);
                        droneAt(x, fY + 0.52f, zp, 0.4f * (float)(gx + gz * 3), tAirframe);
                        glowQuad(1.8f, 1.4f, x, fY + kFloorLift + 0.004f, zp, 0,
                                 -kPi * 0.5f, 1.00f, 0.62f, 0.10f, 0.40f);
                        shadowBlob(ri, x, fY, zp, 1.1f, 1.1f, 0.40f);
                    }
                }
                for (int i = -2; i <= 2; ++i) {
                    crateLg(cx0 + i * 5.0f, cz0 - 6.0f, (i & 1) ? 0.10f : -0.10f, tCrate, 0.02f);
                    crateLg(cx0 + i * 5.0f, cz0 + 6.0f, (i & 1) ? -0.10f : 0.10f, tCrate, 0.02f);
                }
                placeProp(ri, aPipes, kPi * 0.5f, 1.0f, acx(kPipesAabb), kPipesAabb.maxy,
                          1.5f, cx0, cY - 0.25f, cz0 - 9.0f, nullptr, tSteel);
                placeProp(ri, aPipes, kPi * 0.5f, 1.0f, acx(kPipesAabb), kPipesAabb.maxy,
                          1.5f, cx0, cY - 0.25f, cz0 + 9.0f, nullptr, tSteel);
                cornerCam();

            // ---------------- F6 SALVARI LEVEL ----------------
            } else if (nameHas("Artifact Corridor")) {
                // "Alien tech displays. Hovering specimens." — pedestal line, biolume tops
                const float px[3] = { cx0 - 1.8f, cx0 + 1.8f, cx0 - 1.8f };
                const float pz[3] = { cz0 - 6.0f, cz0, cz0 + 6.0f };
                for (int i = 0; i < 3; ++i) {
                    crateSm(px[i], pz[i], 0.0f, tDark, 0.02f);
                    glowQuad(0.62f, 0.62f, px[i] - 0.33f, fY + 0.645f, pz[i] + 0.33f, 0,
                             -kPi * 0.5f, 0.28f, 1.00f, 0.45f, 0.50f);
                }
            } else if (nameHas("Artifact Storage")) {
                // "Salvari relics. Ancient weapons."
                crateSm(cx0 - 2.4f, cz0 - 1.5f, 0.0f, tDark, 0.02f);
                glowQuad(0.62f, 0.62f, cx0 - 2.73f, fY + 0.645f, cz0 - 1.17f, 0,
                         -kPi * 0.5f, 0.28f, 1.00f, 0.45f, 0.50f);
                crateSm(cx0 + 2.4f, cz0 + 1.0f, 0.2f, tDark, 0.02f);
                crateLg(r.x0() + 1.1f, r.z1() - 1.3f,  0.20f, tCrate, 0.02f);
                crateLg(r.x0() + 1.1f, r.z0() + 1.3f, -0.20f, tCrate, 0.02f);
            } else if (nameHas("Analysis Lab")) {
                // "Alien tech reverse-engineering."
                console(cx0 - 2.0f, r.z0() + 0.70f, 0.0f);
                console(cx0 + 2.0f, r.z0() + 0.70f, 0.0f);
                addLight(cx0, fY + 2.3f, r.z0() + 1.3f, 3.0f, 0.25f, 1.10f, 0.45f);
            } else if (nameHas("Portal Chamber")) {
                // "Active portal to alien homeworld." — THE F6 focal statement.
                // (Eye round 2: em 1.1/alpha 0.42 BLEW OUT to a flat mint slab —
                // capped to a deep-green shimmer per the no-blown-emissive law.)
                glassQuad(3.4f, 3.6f, cx0, fY + 2.0f, cz0 - 2.2f, 0.0f,
                          0.08f, 0.42f, 0.22f, 0.30f, 0.35f);
                // Inner event-horizon core (round 3): a brighter heart 8 cm proud of
                // the pane fakes a radial falloff so the portal has DEPTH, not one tone.
                glassQuad(1.7f, 2.0f, cx0, fY + 1.9f, cz0 - 2.12f, 0.0f,
                          0.20f, 0.85f, 0.42f, 0.32f, 0.75f);
                glowQuad(3.6f, 1.2f, cx0, fY + kFloorLift + 0.004f, cz0 - 2.2f, 0,
                         -kPi * 0.5f, 0.25f, 1.00f, 0.42f, 0.50f);
                addLight(cx0 - 2.0f, fY + 1.8f, cz0 - 2.2f, 3.4f, 0.28f, 1.15f, 0.45f);
                addLight(cx0 + 2.0f, fY + 1.8f, cz0 - 2.2f, 3.4f, 0.28f, 1.15f, 0.45f);
                barrel(cx0 - 3.6f, cz0 + 2.6f, 0.4f, tPod);
                barrel(cx0 + 3.4f, cz0 + 2.8f, 1.2f, tPod);
                floorBlob(ri, cx0 - 2.6f, fY, cz0 + 1.8f, 1.3f, 1.0f, 0.10f, 0.09f, 0.03f, 0.45f);
            } else if (nameHas("Salvari Containment")) {
                // "3 prisoners. Can be freed as allies."
                cot(cx0 - 3.0f, cz0 + 2.6f, 0.0f, tSteel);
                cot(cx0,        cz0 + 2.6f, 0.0f, tSteel);
                cot(cx0 + 3.0f, cz0 + 2.6f, 0.0f, tSteel);
            } else if (nameHas("Transformation Pods")) {
                // "Human-alien hybrid conversion."
                barrel(cx0 - 1.8f, cz0,        0.2f, tSpecimen);
                barrel(cx0,        cz0 - 0.4f, 0.8f, tSpecimen);
                barrel(cx0 + 1.8f, cz0,        1.5f, tSpecimen);
                glowQuad(0.95f, 0.95f, cx0, fY + kFloorLift + 0.004f, cz0 - 0.4f, 0,
                         -kPi * 0.5f, 0.25f, 1.00f, 0.40f, 0.40f);
                bloodBlob(ri, cx0 + 0.9f, fY, cz0 + 1.2f, 0.50f, 0.60f, 0.45f);
            } else if (nameHas("Energy Nexus")) {
                // "Portal power. Overload = seal forever." — the hot green core (the
                // one deliberate emissive statement in the organic zone)
                const float coreH = (cY - fY) - 1.4f;
                glowQuad(0.9f, coreH, cx0, fY + coreH * 0.5f + 0.4f, cz0, 0.0f, 0.0f,
                         0.30f, 1.25f, 0.50f, 1.3f);
                glowQuad(0.9f, coreH, cx0, fY + coreH * 0.5f + 0.4f, cz0, kPi * 0.5f, 0.0f,
                         0.30f, 1.25f, 0.50f, 1.3f);
                fusebox(0.30f); fusebox(-0.30f);
                addLight(cx0, fY + 1.8f, cz0, 4.2f, 0.30f, 1.25f, 0.50f);
            } else if (nameHas("First Contact Chamber")) {
                // "Salvari explain the invasion's origin." — a meeting circle
                crateSm(cx0 - 1.6f, cz0 + 1.2f, 0.0f, tDark, 0.02f);
                crateSm(cx0 + 1.6f, cz0 + 1.2f, 0.3f, tDark, 0.02f);
                crateSm(cx0, cz0 - 1.8f, 0.0f, tDark, 0.02f);
                glowQuad(0.62f, 0.62f, cx0 - 0.33f, fY + 0.645f, cz0 - 1.47f, 0,
                         -kPi * 0.5f, 0.28f, 1.00f, 0.45f, 0.45f);
                addLight(cx0, fY + 2.4f, cz0, 3.6f, 1.15f, 0.90f, 0.62f);

            // ---------------- F7 EXECUTIVE SUITE ----------------
            } else if (nameHas("Executive Corridor")) {
                // "Luxurious. Holographic art." — brass shimmer panes on cut-free spans
                const float az[3] = { cz0 - 6.0f, cz0, cz0 + 6.0f };
                for (int i = 0; i < 3; ++i) {
                    const int f = (i == 1) ? 1 : 0;
                    if (!faceClear(f, az[i], 1.1f)) continue;
                    const float ax = (f == 0) ? r.x0() + kInset + 0.04f : r.x1() - kInset - 0.04f;
                    glassQuad(1.7f, 1.0f, ax, fY + 1.7f, az[i],
                              (f == 0) ? kPi * 0.5f : -kPi * 0.5f,
                              1.00f, 0.75f, 0.38f, 0.50f, 0.60f);
                }
            } else if (nameHas("Security Checkpoint")) {
                // "Armed guards. Biometric scanners."
                cornerCam();
                crateSm(cx0 - 1.0f, cz0 - 2.2f, 0.0f, tDark, 0.02f);
                crateSm(cx0 + 0.4f, cz0 - 2.2f, 0.0f, tDark, 0.02f);
            } else if (nameHas("Server Room")) {
                // "OverLord's local node. Hackable." — two racked rows over an aisle
                console(cx0 - 1.6f, cz0 - 1.6f, 0.0f);
                console(cx0 + 1.6f, cz0 - 1.6f, 0.0f);
                console(cx0 - 1.6f, cz0 + 1.6f, kPi);
                console(cx0 + 1.6f, cz0 + 1.6f, kPi);
                addLight(cx0, fY + 1.8f, cz0, 2.2f, 0.16f, 0.85f, 1.05f);
                fusebox(0.30f);
                ventHi(-0.30f);
            } else if (nameHas("Clone Lab")) {
                // "Jake's clone. Mirror confrontation."
                cot(cx0 - 1.5f, cz0, 0.0f, tCryo);
                glassQuad(2.0f, 2.4f, cx0 + 1.2f, fY + 1.4f, cz0, kPi * 0.5f,
                          0.80f, 0.95f, 1.05f, 0.35f, 0.25f);
                addLight(cx0 - 1.5f, cY - 0.5f, cz0, 3.0f, 1.00f, 1.15f, 1.35f);
            } else if (nameHas("Executive Offices")) {
                // "Invasion timeline. Earth conquest plans."
                crateLg(cx0 - 2.2f, cz0 - 1.2f, 0.0f, tDarkCon, 0.02f);
                crateLg(cx0 + 2.2f, cz0 + 1.0f, 0.0f, tDarkCon, 0.02f);
                console(cx0 + 2.2f, cz0 + 2.1f, kPi);
                hangLamp(cx0, cz0, 1.60f, 1.15f, 0.45f, 0.9f, 3.4f);
            } else if (nameHas("Comms Center")) {
                // "Surface communication. Distress beacon."
                console(cx0, cz0 - 1.0f, 0.0f);
                fusebox(0.28f);
                placeProp(ri, aPipes, kPi * 0.5f, 1.0f, acx(kPipesAabb), kPipesAabb.maxy,
                          1.5f, cx0, cY - 0.20f, cz0 + 1.0f, nullptr, tSteel);
                addLight(cx0, cY - 0.4f, cz0, 3.0f, 1.60f, 1.15f, 0.45f);
            } else if (nameHas("Observation Deck")) {
                // "360° glass dome. Alien sky, two moons." — stay pristine; two benches
                crateSm(cx0 - 3.0f, cz0 + 2.5f, 0.0f, tDark, 0.02f);
                crateSm(cx0 + 3.0f, cz0 + 2.5f, 0.0f, tDark, 0.02f);
            } else if (nameHas("Guard Post")) {
                // "Rooftop sentry." (A: sniper / B: searchlight)
                cornerCam();
            } else if (nameHas("Boardroom")) {
                // The executive conference hall — F7 signature. A long central table (crate
                // spine) flanked by two rows of exec chairs, brass pendants over the table,
                // holographic strategy art on both long walls, a head-of-table presentation
                // console. Clean dark luxury + brass.
                // Dark WALNUT veneer, not coal: tDark (0.09 albedo) stays black under
                // any light (diffuse ~0.03 post-tint) — the zone brief is "brass +
                // dark wood veneer", so the table + chairs take a veneer brown that
                // actually responds to the pendant pools.
                const float tVeneer[4] = { 0.33f, 0.24f, 0.15f, 1.0f };
                for (int i = -3; i <= 3; ++i) {
                    crateLg(cx0, cz0 + i * 1.9f, (i & 1) ? 0.02f : 0.0f, tVeneer, 0.02f);
                    crateSm(cx0 - 2.2f, cz0 + i * 1.9f, 0.0f, tVeneer, 0.02f);
                    crateSm(cx0 + 2.2f, cz0 + i * 1.9f, kPi,  tVeneer, 0.02f);
                }
                hangLamp(cx0, cz0 - 4.2f, 1.60f, 1.15f, 0.45f, 1.0f, 3.6f);
                hangLamp(cx0, cz0 + 4.2f, 1.60f, 1.15f, 0.45f, 1.0f, 3.6f);
                console(cx0, r.z0() + 0.9f, 0.0f);
                glassQuad(2.4f, 1.4f, r.x0() + kInset + 0.04f, fY + 1.9f, cz0 - 4.0f, kPi * 0.5f,
                          1.00f, 0.78f, 0.40f, 0.45f, 0.55f);
                glassQuad(2.4f, 1.4f, r.x1() - kInset - 0.04f, fY + 1.9f, cz0 + 4.0f, -kPi * 0.5f,
                          1.00f, 0.78f, 0.40f, 0.45f, 0.55f);
                addLight(cx0, std::min(cY - 0.5f, fY + 4.6f), cz0, 4.0f, 1.55f, 1.15f, 0.55f);
            }
        }
        ++m_roomsDressed;
    }
    (void)nLightsBefore;

    x3::logInfo("[room-dress] " + std::to_string(m_roomsDressed) + " rooms dressed: " +
                std::to_string(nPanels) + " surface panels, " +
                std::to_string(m_props.size()) + " props, " +
                std::to_string(m_lights.size()) + " recipe lights, " +
                std::to_string(m_proc.size()) + " strips/shadows");
    return m_roomsDressed > 0;
}

// ---- Draw ------------------------------------------------------------------------------

void RoomDressing::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                        const std::vector<uint32_t>& visibleRooms) const {
    if (m_panels.empty() && m_props.empty() && m_proc.empty()) return;
    // Per-frame visibility bitmap (same PVS set the scene cull uses).
    static thread_local std::vector<uint8_t> vis;
    vis.assign(m_roomZone.size(), 0);
    for (uint32_t v : visibleRooms) if (v < vis.size()) vis[v] = 1;

    for (const Panel& p : m_panels) {
        if (p.room >= vis.size() || !vis[p.room]) continue;
        m_surf.drawPanel(device, frame, *m_sets[p.set], p.mesh, p.transform);
    }
    for (const PropInst& e : m_props) {
        if (e.room >= vis.size() || !vis[e.room]) continue;
        const Asset& a = m_assetTable[e.asset];
        for (const auto& d : a.drawables) {
            float fin[16];
            x3::asset::mulMat4(e.transform, d.nodeTransform, fin);
            const bool matEmis = d.emissiveTexId != 0 || d.emissiveFactor[0] > 0.001f ||
                                 d.emissiveFactor[1] > 0.001f || d.emissiveFactor[2] > 0.001f;
            float emis[4];
            if (matEmis) { emis[0] = d.emissiveFactor[0]; emis[1] = d.emissiveFactor[1];
                           emis[2] = d.emissiveFactor[2]; emis[3] = e.emissive[3]; }
            else         { emis[0] = e.emissive[0]; emis[1] = e.emissive[1];
                           emis[2] = e.emissive[2]; emis[3] = e.emissive[3]; }
            const float bc[4] = { d.baseColorFactor[0] * e.tint[0],
                                  d.baseColorFactor[1] * e.tint[1],
                                  d.baseColorFactor[2] * e.tint[2],
                                  d.baseColorFactor[3] * e.tint[3] };
            // BLACK-PROP FIX (wing floors, m_propMatLift) — RE-JUDGED after the engine fix
            // (5c35d65, the 1/pi shading-path split). The original fix had TWO halves and
            // they did NOT age the same way:
            //
            //  * THE METALNESS CLAMP — STILL NEEDED, KEPT. The converted kit furniture
            //    (crates/beds/vats/chairs) carries NO mr map, so the loader synthesizes a
            //    1x1 MR from the glTF default metallicFactor = 1.0. A full metal has NO
            //    diffuse lobe at all (diff = albedo*(1-metallic) -> 0) — and that is TRUE
            //    ON EITHER SIDE OF THE 1/PI FIX. It is a separate bug (a bad material
            //    default on furniture that is not metal), so the clamp survives the engine
            //    fix untouched: a wooden crate gets its diffuse lobe back.
            //
            //  * DROPPING THE ALBEDO TEXTURE — CRUTCH, REMOVED. That half only existed
            //    because the surviving diffuse was still 1/pi too dark, so the mid-tone kit
            //    texture (~0.12 linear) had to be thrown away and replaced by the brighter
            //    flat recipe tint just to be visible. mesh.frag now shades these honestly
            //    (~3.14x brighter at metallic 0), so the tint-over-texture substitution both
            //    DOUBLE-COUNTS (blowing the props toward flat white) and needlessly destroys
            //    all the surface detail the kit texture carries. The texture is restored and
            //    the recipe tint goes back to being what it says it is: a TINT modulating a
            //    real material, in `bc`, exactly as the canon/detention dressing has always
            //    done. Props keep form + tint AND grain.
            constexpr float kPropMetallicClamp = 0.35f;   // 1.0 = full metal (no diffuse); 0.35 restores diffuse
            // keepTex props (the rescue captives) NEVER lift — a hero character keeps her
            // authored PBR skin/clothes even on the wing floors.
            const bool lift = m_propMatLift && !e.keepTex;
            const x3::rhi::TextureHandle baseTex{ d.baseColorTexId };   // always textured now
            const float metalScale = lift ? kPropMetallicClamp : 1.0f;
            device.drawMeshPBR(frame, x3::rhi::MeshHandle{ d.meshId },
                               baseTex,
                               x3::rhi::TextureHandle{ d.normalTexId },
                               x3::rhi::TextureHandle{ d.mrTexId },
                               bc, emis, fin, d.alphaMask, d.alphaBlend,
                               x3::rhi::TextureHandle{ d.emissiveTexId },
                               x3::rhi::TextureHandle{ d.detailTexId },
                               d.detailUvScale, d.clearcoat, d.clearcoatRough,
                               /*selfLight=*/0.0f, metalScale);
        }
    }
    const x3::rhi::TextureHandle white{ 0 };
    for (const ProcDraw& p : m_proc) {
        if (p.room >= vis.size() || !vis[p.room]) continue;
        if (p.glass) {
            x3::rhi::IRenderDevice::GlassMaterial gm;
            gm.opacity = p.color[3]; gm.refraction = 0.0f;
            gm.roughness = 1.0f; gm.specular = 0.0f;
            gm.tint[0] = p.color[0]; gm.tint[1] = p.color[1]; gm.tint[2] = p.color[2];
            device.drawMeshGlass(frame, p.mesh, white, p.color, p.emissive, gm, p.transform);
        } else if (p.tex.valid()) {
            // Dark-glass screen: near-black albedo (p.color) + the baked monitor as an
            // EMISSIVE map so only the glowing readout texels bloom (ACES glow law).
            device.drawMeshPBR(frame, p.mesh, white,
                               x3::rhi::TextureHandle{ 0 }, x3::rhi::TextureHandle{ 0 },
                               p.color, p.emissive, p.transform, /*alphaMask*/false,
                               /*alphaBlend*/false, /*emissiveTex*/p.tex);
        } else {
            device.drawMeshEmissive(frame, p.mesh, white, p.color, p.emissive, p.transform);
        }
    }
}

void RoomDressing::applyZoneAtmosphere(x3::rhi::IRenderDevice& device, uint32_t eyeRoom) {
    int zone = ZWard;   // detention default (matches the cell_dressing base opt-in)
    if (eyeRoom < m_roomZone.size() && m_roomZone[eyeRoom] != ZNone)
        zone = m_roomZone[eyeRoom];
    // Is the eye INSIDE the room graph at all? kNoRoom (or a room id past the table)
    // means the exterior — the facade, the apron, the streamed planet — which is lit by
    // the sky/IBL rig and must keep the engine defaults.
    const bool interior = (eyeRoom < m_roomZone.size());
    const int key = interior ? zone : -2;    // -2 = "exterior" (distinct from -1 = unset)
    if (key == m_lastZone) return;
    m_lastZone = key;
    device.setFog(m_zoneFog[(size_t)zone]);
    if (interior) {
        const ZoneAir& air = airFor((uint8_t)zone);
        device.setAmbient(air.amb[0], air.amb[1], air.amb[2]);
        device.setIblIntensity(air.ibl);
        x3::logInfo("[zone-air] room " + std::to_string(eyeRoom) + " zone " +
                    std::to_string(zone) + " -> ambient " + std::to_string(air.amb[0]) +
                    " ibl " + std::to_string(air.ibl));
    } else {
        device.setAmbient(kExteriorAmbient[0], kExteriorAmbient[1], kExteriorAmbient[2]);
        device.setIblIntensity(kExteriorIbl);
    }
}

void RoomDressing::forEachPropInstance(
    const std::function<void(uint32_t, const std::string&,
                             const std::vector<x3::asset::ModelDrawable>&,
                             const float*)>& fn) const {
    for (const PropInst& p : m_props) {
        if (p.asset >= m_assetTable.size()) continue;
        const Asset& a = m_assetTable[p.asset];
        if (!a.ok) continue;
        fn(p.room, m_assetPaths[p.asset], a.drawables, p.transform);
    }
}

} // namespace x3::game
