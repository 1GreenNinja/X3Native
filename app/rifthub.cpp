// EFLZ Portal Hub — see rifthub.h for the design overview.
//
// Clean-room: built ONLY from X3Native's own Scene / trigger / mesh_prims
// systems + the engine interfaces. No RBDOOM / id Tech / Doom / Quake — or any
// other game-engine — source was consulted. CONTENT/LEVEL-SCRIPT ONLY.
#include "rifthub.h"
#include "asset_root.h"
#include "mesh_prims.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace x3::game {

namespace {

// Hub layout: 8 portals arranged on a circle of radius kRingRadius around the
// spawn point at world origin. Each portal is a Stargate-INSPIRED gateway
// (original procedural design): a thick grey-stone RING built from a single
// circle of N deep tangent box segments (the ring's plane is perpendicular to
// the outward radial direction so the portal's doorway face points back at the
// hub center), amber CHEVRON locking clamps ringing its outer face, a small
// octagonal floor-plate at the base carrying the per-destination accent, and
// the event-horizon membrane pool filling the opening.
constexpr float kHubHalf       = 20.0f;   // 40 m square floor (footprint half)
constexpr float kRingRadius    = 14.0f;   // portal placement radius (around spawn)

// ---- The HALL (phase C: the hub becomes a PLACE, not a checkerboard void) ------
// One industrial hall shell around the gate circle. SEAM LAW: the wall INNER
// faces sit exactly on the +/-kHubHalf floor edge (flush, zero gap); N/S walls
// span the full width including the wall thickness, E/W walls BUTT between
// them (corner boxes never overlap coplanar); the ceiling slab overlaps the
// wall tops. Walls collide (containment — the player can't leave the shell).
constexpr float kHallWallH     = 10.0f;   // interior wall height
constexpr float kHallWallT     = 0.30f;   // wall thickness
constexpr float kHallCeilT     = 0.20f;   // ceiling slab thickness
constexpr float kConcreteTint[3] = { 0.42f, 0.44f, 0.46f };  // dark venue concrete
constexpr float kFloorTint[3]    = { 0.34f, 0.36f, 0.38f };  // wet dark floor
constexpr uint32_t kHallColumns  = 8;     // perimeter steel columns
constexpr float kColumnHalf      = 0.32f;
constexpr uint32_t kHallBeams    = 5;     // ceiling beam count per direction
constexpr float kBeamHalfW       = 0.22f;
constexpr float kBeamHalfH       = 0.28f;
constexpr float kBeamY           = 9.45f; // beam centerline height
constexpr uint32_t kStripCount   = 8;     // ceiling strip lights
constexpr float kStripTint[3]    = { 0.72f, 0.82f, 0.95f };  // cool white-blue
constexpr float kStripEm         = 1.90f; // capped — fixtures, not suns
constexpr uint32_t kCableCount   = 10;    // hanging catenary cables
constexpr uint32_t kCableSegs    = 9;
constexpr float kCableSag        = 1.15f;
// Hall fill lights (appended AFTER the 8 animated gate lights in m_lights).
constexpr float kHallLightColor[3] = { 0.55f, 0.62f, 0.72f };  // cool industrial
constexpr float kHallLightI        = 3.20f;
constexpr float kHallLightRange    = 20.0f;

// ---- Stone gateway ring geometry ----------------------------------------------
// A SUBSTANTIAL, thick ring you walk through — a single circle of N deep tangent
// box segments with a beefy squarish cross-section (real radial thickness + real
// depth through the gate). Grey stone, NON-glowing (a faint emissive self-lift
// only so it reads in shadow — it is NOT an energy source).
constexpr uint32_t kRingSegments   = 40;     // deep box segments (N at 9° each — smooth torus)
constexpr float    kRingY          = 2.2f;   // ring center height above the floor
constexpr float    kRingR          = 2.05f;  // ring CENTERLINE radius
constexpr float    kRingHalfRad    = 0.40f;  // radial half-thickness (0.80 m thick band)
constexpr float    kRingHalfDepth  = 0.45f;  // half-depth through the gate (0.90 m deep)
constexpr float    kRingStone[3]   = { 0.55f, 0.66f, 0.63f };  // teal-patina multiplier (over the metal set)
constexpr float    kRingEmissive   = 0.06f;  // near-zero self-lift — the blue core + hall LIGHT the metal
// TRUE TORUS ring params (step-2 AAA smooth ring; replaces the box segments):
constexpr float    kRingTubeR      = 0.40f;  // tube radius => 0.80 m band / 0.80 m depth
constexpr uint32_t kRingMajorSeg   = 64;     // segments around the ring centerline (smooth)
constexpr uint32_t kRingMinorSeg   = 16;     // segments around the tube cross-section
// Ring inner edge = kRingR - kRingHalfRad = 1.65 m; the membrane pool's outer
// band tops out near 1.585 m, so the opening stays clear (no clip).
// Segment "long-axis" half-extent = half the chord between adjacent segment
// centers, with a small overlap so neighboring boxes butt cleanly without gaps.
inline float segHalfTangent(float ringR) {
    const float pi = 3.14159265358979f;
    return ringR * std::sin(pi / (float)kRingSegments) * 1.06f;  // 6% overlap (chunky butt)
}

// ---- Chevron clamp HOUSINGS + amber slit cores (ROUND 2, "get industrial") -----
// Round-1 kept a whole-shape emissive amber TRIANGLE as the core — still "flat
// yellow party triangles" on Tim's live eyeball. Round 2: the triangle DIES.
// Each chevron is now a machined clamp built from boxes only, every body panel
// textured from the PBR library, and the ONLY emitter is a thin amber-lit SLIT
// strip inset in the face plate (dark glass when unlit — near-black baseColor —
// so nothing ever reads as saturated yellow plastic). Flicker capped at 2.0.
constexpr uint32_t kChevronCount   = 9;      // 9 locking clamps (one prominent at top)
constexpr float    kChevAmber[3]   = { 1.00f, 0.46f, 0.08f };  // amber-orange lock glow
constexpr float    kChevSeatR      = 2.02f;  // seat radius (clamp center, over the tube)
constexpr float    kChevMinEm      = 0.70f;  // amber slit flicker trough
constexpr float    kChevMaxEm      = 1.55f;  // amber slit flicker peak (powered)
constexpr float    kChevEmCap      = 2.00f;  // HARD cap incl. surge lift (the brief's ~2.0)
constexpr float    kChevFlickerHz  = 0.85f;  // slow flicker rate (Hz)
constexpr float    kChevPhaseStep  = 0.7f;   // per-chevron phase offset (rad)
// Slit core: a thin horizontal lit strip (tangent-long), barely proud of the cap.
constexpr float    kChevSlitHalfTan = 0.150f;
constexpr float    kChevSlitHalfRad = 0.028f;
constexpr float    kChevSlitHalfDep = 0.016f;
constexpr float    kChevSlitDark[3] = { 0.05f, 0.035f, 0.02f };  // unlit = dark glass
// Housing (dark gunmetal clamp body seated INTO the ring — spans the tube band):
constexpr float    kHouseHalfTan   = 0.32f;  // body half-width (tangent)
constexpr float    kHouseHalfRad   = 0.42f;  // body half-height (radial: grips the tube)
constexpr float    kHouseHalfDep   = 0.15f;  // body proud half-thickness (outward)
constexpr float    kFlangeHalfTan  = 0.080f; // side jaw flange bars
constexpr float    kFlangeHalfRad  = 0.34f;
constexpr float    kFlangeHalfDep  = 0.19f;  // jaws bite deeper than the body
// Stepped face-cap plate (the machined bevel read: body -> cap -> slit).
constexpr float    kCapHalfTan     = 0.22f;
constexpr float    kCapHalfRad     = 0.28f;
constexpr float    kCapHalfDep     = 0.050f;

// ---- Ring v2 over-plates + rivets (industrialize the smooth torus) -------------
// Varied-depth riveted armor plates wrapped over the torus rim break the
// uniform donut: per-portal kPlateArcCount arc plates at jittered angular
// slots/sizes seated on the OUTER rim, plus small rivet studs on the front
// face. Textured from the curated surface library, tinted toward the locked
// TEAL-OXIDE patina.
constexpr uint32_t kPlateArcCount  = 12;
constexpr float    kPlateSeatR     = 2.16f;   // plate center radius (over the tube crest)
constexpr float    kPatinaTint[3]  = { 0.62f, 0.78f, 0.74f };  // teal-oxide multiplier
constexpr float    kSteelTint[3]   = { 0.52f, 0.55f, 0.58f };  // neutral steel multiplier
constexpr float    kDarkTint[3]    = { 0.30f, 0.33f, 0.35f };  // dark housing metal
constexpr uint32_t kRivetCount     = 12;      // front-face rivet studs per portal
constexpr float    kRivetHalf      = 0.032f;

// ---- Segmented amber RATCHET TRACK (inner-facing edge; PortalAnimated.mp4) -----
// Small amber segments ringing the gate's inner front edge. Dormant: dim.
// SURGE: a bright chase sweeps the circumference (activation feedback).
// OPEN: steady powered glow. All writes capped at kTrackEmCap.
constexpr uint32_t kTrackSegs      = 36;
constexpr float    kTrackR         = 1.80f;   // segment center radius
constexpr float    kTrackHalfTan   = 0.10f;
constexpr float    kTrackHalfRad   = 0.055f;
constexpr float    kTrackHalfDep   = 0.018f;
constexpr float    kTrackEmIdle    = 0.22f;
constexpr float    kTrackEmOpen    = 0.90f;
constexpr float    kTrackChase     = 2.00f;   // added peak at the chase crest
constexpr float    kTrackChaseRadS = 9.0f;    // chase sweep speed (rad/s)
constexpr float    kTrackEmCap     = 2.30f;

// ---- ORANGE conduits + coils + TEAL holo screens (phase D dressing) ------------
// The locked palette's accents: ORANGE emissive conduit pipes running
// gate -> floor -> skirt (tick() phases the emissive along the run so power
// visibly flows), coil rings on the riser, and 1-2 TEAL holo data screens
// per gate on posts (glass panes, procedural readout textures).
constexpr float    kConduitOrange[3] = { 1.00f, 0.29f, 0.035f };
constexpr float    kConduitEmBase    = 0.65f;
constexpr float    kConduitFlowAmp   = 0.42f;
constexpr float    kConduitEmCap     = 1.30f;   // orange must stay ORANGE (never yellow-clips)
constexpr float    kConduitFlowHz    = 0.55f;   // flow pulse rate
constexpr float    kConduitFlowK     = 1.60f;   // phase step per segment (the travel)
constexpr float    kConduitHalf      = 0.045f;  // pipe half-thickness
constexpr float    kCoilOrangeEm     = 0.85f;
constexpr float    kHoloTeal[3]      = { 0.12f, 0.85f, 0.75f };
constexpr float    kHoloEm           = 0.35f;   // soft glow floor — the TEXTURE carries the read
                                                // (flat glass emissive floods the pane otherwise)

// ---- A-frame support cradle (the gate is INSTALLED, not floating) --------------
constexpr float    kSkirtHalfTan   = 1.70f;   // base plinth under the ring bottom
constexpr float    kSkirtHalfY     = 0.25f;
constexpr float    kSkirtHalfDep   = 0.85f;
constexpr float    kStrutBaseOut   = 2.85f;   // strut foot lateral offset (along right)
constexpr float    kStrutTopOut    = 1.95f;   // strut head lateral offset
constexpr float    kStrutTopY      = 2.55f;   // strut head height (grabs the ring side)
constexpr float    kStrutHalfW     = 0.17f;
constexpr float    kStrutHalfT     = 0.12f;
constexpr float    kAnchorHalfTan  = 0.55f;   // floor anchor plates at the strut feet
constexpr float    kAnchorHalfY    = 0.05f;
constexpr float    kAnchorHalfDep  = 0.45f;

// ---- Floor plate (octagonal) --------------------------------------------------
// 8 small box wedges in a ring on the floor — a low-poly "disk" the player
// steps onto. Slightly elevated so it reads against the dark ground checker.
constexpr uint32_t kPlateSegments  = 8;
constexpr float    kPlateRingR     = 1.30f;  // plate ring radius (center of each wedge)
constexpr float    kPlateHalfY     = 0.05f;  // plate slab half-Y (flat)
constexpr float    kPlateBoxThick  = 0.50f;  // plate wedge half-extent (radial)
// Floor wedges butt at their outer edge — half-tangent = R * sin(pi/8) * 1.04 overlap.
inline float plateHalfTangent() {
    const float pi = 3.14159265358979f;
    return kPlateRingR * std::sin(pi / (float)kPlateSegments) * 1.04f;
}

// ---- Pool center (energy-core hot spot) -----------------------------------------
// Two small thin vertical slabs at the exact ring center — the brightest
// blue-white point of the event-horizon pool (the membrane bands fill the rest
// of the opening around them). Small so the round membrane silhouette wins.
constexpr float    kCoreHalfW      = 0.30f;  // 0.6m wide hot-center slab
constexpr float    kCoreHalfH      = 0.30f;  // 0.6m tall
constexpr float    kCoreHalfT      = 0.025f; // 0.05m thick (thin disk)

// ---- Trigger volume -----------------------------------------------------------
constexpr float kTrigHalfXZ    = 2.5f;    // trigger volume half-extent (wider than ring)
constexpr float kTrigHalfY     = 2.5f;    // trigger volume vertical half-extent

// Player spawn Y (capsule feet): standing on the ground plane (which sits at
// y = -0.10 below the world origin; feet at +0 is fine).
constexpr float kSpawnFeetY    = 0.05f;

// ---- Animation phase + electric-blue energy core ------------------------------
// The per-destination accent lives on the FLOOR PLATE (the ring is neutral).
// The energy core is a unifying deep electric blue — every portal reads as a
// blue plasma storm in an industrial gate. MEMBRANE v2 (fable-rock art pass):
// the v1 core ran emissive 4..9 and near-white color — ACES tonemap +
// bloom turned every membrane into a BLOWN-OUT WHITE FLASHLIGHT (Tim's
// verdict). v2 law: SATURATED blue colors, strengths hard-capped ~2.x so the
// blue channel dominance survives the tonemapper.
constexpr float    kShimmerPhaseStep  = 0.5f;            // per-portal phase offset (rad)
constexpr float    kCoreBlue[3]       = { 0.20f, 0.50f, 1.00f };  // electric blue
constexpr float    kCoreInnerBlue[3]  = { 0.55f, 0.78f, 1.00f };  // hot center (pale BLUE, not white)
constexpr float    kCoreInnerHalfW    = 0.17f;          // inner hot-spot half-width
constexpr float    kCoreInnerHalfH    = 0.17f;          // inner hot-spot half-height
constexpr float    kCoreInnerHalfT    = 0.020f;         // inner disk half-thickness
constexpr float    kCoreBlueMinEm     = 1.10f;          // core blue pulse min
constexpr float    kCoreBlueMaxEm     = 1.90f;          // core blue pulse max
constexpr float    kCoreEmCap         = 2.60f;          // HARD cap incl. kawoosh surge
constexpr float    kCoreFreqHz        = 3.2f;           // core pulses faster than the ripple

// ---- Blue CORE point light (casts the event horizon onto the grey stone) ------
// The step-4 resolution of the blue-vs-grey conflict: keep the grey STONE ring,
// but drive a cool-blue point light from each ring center that lights the stone
// (+ floor + chevrons) blue, pulsing slowly with the hum so the gate breathes.
constexpr float    kCoreLightBlue[3]  = { 0.30f, 0.60f, 1.00f };  // cool blue (plan spec)
constexpr float    kCoreLightBase     = 2.2f;           // base intensity multiplier
constexpr float    kCoreLightMin      = 1.4f;           // pulse-with-hum floor
constexpr float    kCoreLightMax      = 3.2f;           // pulse-with-hum peak
constexpr float    kCoreLightFreqHz   = 1.1f;           // slow hum-synced breathe
constexpr float    kCoreLightRange    = 7.0f;           // reaches the gate + plate

// ---- Event-horizon membrane v2 (the DEEP-BLUE PLASMA STORM) --------------------
// The fable-rock art pass (docs/RIFTHUB_ART_TARGET.md, palette LOCKED: BLUE
// membranes). v1 was 70 flat emissive boxes at strengths 2.4..5.5 + fresnel
// +3.2 + kawoosh +11 — every one of them tonemap-clipped to WHITE. v2 is
// three layers per portal, every emissive write CLAMPED to a per-layer cap:
//   [0] VISTA disk: a dim procedural "other world" backdrop sitting a step
//       OUTWARD of the plasma sheet — glimpsed through the storm, and the
//       depth offset gives real parallax as the player strafes the gate.
//   [1] PLASMA disk: a two-sided fan carrying a procedural FILAMENT emissive
//       texture (ridged-noise lightning webs baked blue-white on deep blue)
//       on the PBR route (mrTex forces it so emissiveTex is honoured). The
//       per-entity emissive is DEEP BLUE at a capped strength; tick() slowly
//       ROTATES the disk so the filament web visibly churns.
//   [2] FRESNEL RIM: a thin bright-blue torus hugging the ring's inner edge
//       (the grazing-angle read), shimmering, capped.
// Live tendrils (forked lightning arcs) + spark motes are drawn per-frame by
// drawFx() — see the FX section below.
constexpr float    kMembraneR         = 1.58f;           // disk radius (ring inner edge 1.65)
constexpr uint32_t kMembraneDiskSegs  = 48;              // fan segments (smooth silhouette)
constexpr float    kVistaDepth        = 0.10f;           // vista sits this far OUTWARD of the plasma
// Plasma layer: deep blue, saturated. Texture carries the white-blue filament
// detail; the per-entity emissive tint keeps the whole sheet blue-dominant.
constexpr float    kPlasmaBlue[3]     = { 0.24f, 0.52f, 1.00f };
constexpr float    kPlasmaEmBase      = 1.45f;           // IDLE steady-state strength
constexpr float    kPlasmaEmBaseOpen  = 1.90f;           // OPEN throat runs hotter (still capped)
constexpr float    kPlasmaEmWobble    = 0.30f;           // organic breathe amplitude
constexpr float    kPlasmaEmCap      = 2.40f;            // HARD CAP (blue must survive tonemap)
constexpr float    kPlasmaSpinRadS    = 0.22f;           // slow storm rotation (rad/s)
constexpr float    kPlasmaSpinOpenX   = 2.6f;            // OPEN spins faster (streaming throat)
// Vista layer: dim — it reads only where the plasma texture is dark. On OPEN
// the vista DISSOLVES into the energy (fades toward kVistaEmOpen).
constexpr float    kVistaTint[3]      = { 0.55f, 0.80f, 0.95f };
constexpr float    kVistaEmBase       = 0.42f;
constexpr float    kVistaEmOpen       = 0.08f;           // dissolved (throat state)
constexpr float    kVistaFadePerSec   = 0.35f;           // dissolve rate on OPEN
constexpr float    kVistaEmCap        = 0.70f;
constexpr float    kVistaSpinRadS     = -0.06f;          // counter-rotates (parallax cue)
// Fresnel rim ring: bright blue, the one deliberately hot edge — still capped.
constexpr float    kRimR              = 1.60f;           // rim centerline radius
constexpr float    kRimTubeR          = 0.050f;          // thin tube
constexpr float    kRimBlue[3]        = { 0.30f, 0.62f, 1.00f };
constexpr float    kRimEmBase         = 1.35f;
constexpr float    kRimShimmerAmp     = 0.35f;
constexpr float    kRimShimmerHz      = 0.90f;
constexpr float    kRimEmCap          = 2.00f;   // v1 lesson: >2.9 reads WHITE after ACES
// ---- Kawoosh one-shot (the ACTIVATION SURGE state) ------------------------------
// On activation the storm SURGES: strength rides toward the caps, the plasma
// tint slides toward pale blue-white (bright BLUE-white at the peak, never
// flat white — every write still clamped), the tendrils re-target into a
// VORTEX RING whipping the rim (PortalAnimated.mp4 state 2), and the surge
// settles into the OPEN throat state instead of back to idle.
constexpr float    kKawooshDur        = 1.60f;           // total surge duration (s)
constexpr float    kKawooshPeakEm     = 1.60f;           // peak ADDED emissive at the flash
constexpr float    kKawooshDecay      = 3.4f;            // exponential decay rate (1/s)
constexpr float    kKawooshTint[3]    = { 0.62f, 0.80f, 1.00f };  // surge color (pale blue)
// ---- Membrane lightning arcs (the lightning-gun forked-bolt idea, re-done
// as membrane tendrils: short-lived jagged chords crawling the disk) ------------
constexpr float    kArcThickness      = 0.020f;          // beam half-thickness (m)
constexpr uint32_t kArcSegs           = 7;               // polyline segments per arc
constexpr float    kArcLifeMin        = 0.22f;           // seconds
constexpr float    kArcLifeMax        = 0.55f;
constexpr float    kArcCooldownMin    = 0.10f;           // spawner gap between arcs
constexpr float    kArcCooldownMax    = 0.55f;
constexpr float    kArcColor[4]       = { 0.60f, 0.80f, 1.00f, 1.0f };
constexpr float    kArcEmColor[3]     = { 0.52f, 0.75f, 1.00f };
constexpr float    kArcEmPeak         = 2.40f;           // capped bright BLUE-white
constexpr float    kArcLift           = 0.075f;          // arcs float this far HUB-side of the plasma
// ---- Spark motes (embers drifting off the membrane) ----------------------------
constexpr float    kMoteRatePerSec    = 7.0f;            // steady spawn rate per portal
constexpr int      kMoteBurst         = 36;              // kawoosh burst count
constexpr float    kMoteLifeMin       = 0.9f;
constexpr float    kMoteLifeMax       = 2.2f;

// Portal authoring table — ORDER is the clockwise arrangement around the hub
// starting at +X (angle 0 -> -X around -Y rotation; we iterate i=0..7 at
// angle = i * (2pi/8) so portal 0 is at +X, 1 at +X+Z, 2 at +Z, etc.).
struct PortalSpec {
    const char*    worldName;
    uint32_t       triggerId;
    float          tint[3];   // emissive color (HDR-ish; strength applied below)
};
// Tint palette per the task spec:
//   act2caves: violet | act2: orange-amber | valley: cyan | cliffs: white-gold
//   club: magenta | destruct: red | ragdoll: green | terrain: sky-blue
constexpr PortalSpec kPortalTable[] = {
    { "act2caves", (uint32_t)RifthubTrigger::Act2Caves, { 0.75f, 0.30f, 1.00f } }, // violet
    { "act2",      (uint32_t)RifthubTrigger::Act2,      { 1.00f, 0.55f, 0.15f } }, // orange-amber
    { "valley",    (uint32_t)RifthubTrigger::Valley,    { 0.20f, 0.85f, 1.00f } }, // cyan
    { "cliffs",    (uint32_t)RifthubTrigger::Cliffs,    { 1.00f, 0.92f, 0.65f } }, // white-gold
    { "club",      (uint32_t)RifthubTrigger::Club,      { 1.00f, 0.20f, 0.85f } }, // magenta
    { "destruct",  (uint32_t)RifthubTrigger::Destruct,  { 1.00f, 0.20f, 0.15f } }, // red
    { "ragdoll",   (uint32_t)RifthubTrigger::Ragdoll,   { 0.30f, 1.00f, 0.40f } }, // green
    { "terrain",   (uint32_t)RifthubTrigger::Terrain,   { 0.45f, 0.70f, 1.00f } }, // sky-blue
};
constexpr uint32_t kPortalCount = (uint32_t)(sizeof(kPortalTable) / sizeof(kPortalTable[0]));
static_assert(kPortalCount == kRifthubTrigCount, "kRifthubTrigCount must match the portal table");

// Build a column-major 4x4 model transform from three orthonormal basis vectors
// (the rendered box's local +X / +Y / +Z, each in WORLD space) plus a world-space
// translation. The mesh is authored centered at the local origin (makeBox with
// cx=cy=cz=0), so the transform fully relocates + reorients it. Mirrors the
// column layout used by valley.cpp's placeTilted().
inline void makeXform(float m[16],
                      const float xAxis[3], const float yAxis[3], const float zAxis[3],
                      float wx, float wy, float wz) {
    m[ 0] = xAxis[0]; m[ 1] = xAxis[1]; m[ 2] = xAxis[2]; m[ 3] = 0.0f;
    m[ 4] = yAxis[0]; m[ 5] = yAxis[1]; m[ 6] = yAxis[2]; m[ 7] = 0.0f;
    m[ 8] = zAxis[0]; m[ 9] = zAxis[1]; m[10] = zAxis[2]; m[11] = 0.0f;
    m[12] = wx;       m[13] = wy;       m[14] = wz;       m[15] = 1.0f;
}

// Add an origin-centered box with a per-entity orientation + translation written
// directly into the Entity transform. Returns BOTH the mesh handle (so shutdown
// can free it) and the entity id (so tick() can pulse its emissive[3]).
struct AddedEntity {
    x3::rhi::MeshHandle mesh;
    uint32_t            entityId;
};
// `baseCol` (optional) decouples the unlit surface color from the emissive
// tint — the ROUND-2 realism law: an emitter's DARK body must not read as
// saturated plastic when its glow is dim (pass a near-black baseCol).
AddedEntity addOrientedEmissiveBox(Scene& scene, x3::rhi::IRenderDevice& device,
                                   float hx, float hy, float hz,
                                   const float xAxis[3], const float yAxis[3], const float zAxis[3],
                                   float wx, float wy, float wz,
                                   const float tint[3], float emStrength,
                                   const float* baseCol = nullptr) {
    // Origin-centered mesh; reorient + translate via Entity::transform.
    x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, 0.0f, 0.0f, 0.0f);
    Entity e;
    e.mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                               m.index.data(), (uint32_t)m.index.size());
    const float* bc = baseCol ? baseCol : tint;
    e.baseColor[0] = bc[0]; e.baseColor[1] = bc[1]; e.baseColor[2] = bc[2];
    e.baseColor[3] = 1.0f;
    e.emissive[0] = tint[0]; e.emissive[1] = tint[1]; e.emissive[2] = tint[2];
    e.emissive[3] = emStrength;
    e.tag = (uint32_t)Tag::Prop;
    makeXform(e.transform, xAxis, yAxis, zAxis, wx, wy, wz);
    uint32_t id = scene.add(e);
    return AddedEntity{ e.mesh, id };
}

// Add an origin-centered box dressed with a curated PBR surface set (albedo +
// normal + MR on the PBR route). Falls back to the flat-tinted emissive box
// when the set didn't load (headless / assets not fetched). `tint` multiplies
// the albedo (the teal-patina wash); `uvScale` sets texture tiling (makeBox
// emits 1 tile per meter at 1.0).
AddedEntity addOrientedSurfBox(Scene& scene, x3::rhi::IRenderDevice& device,
                               float hx, float hy, float hz,
                               const float xAxis[3], const float yAxis[3], const float zAxis[3],
                               float wx, float wy, float wz,
                               const SurfaceSet* sf, const float tint[3],
                               float emStrength, float uvScale = 1.0f) {
    x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, 0.0f, 0.0f, 0.0f, uvScale);
    Entity e;
    e.mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                               m.index.data(), (uint32_t)m.index.size());
    if (sf && sf->ok) {
        e.tex = sf->albedo; e.normalTex = sf->normal; e.mrTex = sf->mr;
    }
    e.baseColor[0] = tint[0]; e.baseColor[1] = tint[1]; e.baseColor[2] = tint[2];
    e.baseColor[3] = 1.0f;
    e.emissive[0] = tint[0]; e.emissive[1] = tint[1]; e.emissive[2] = tint[2];
    e.emissive[3] = emStrength;
    e.tag = (uint32_t)Tag::Prop;
    makeXform(e.transform, xAxis, yAxis, zAxis, wx, wy, wz);
    uint32_t id = scene.add(e);
    return AddedEntity{ e.mesh, id };
}

// (ROUND 2: the tri-prism chevron helper is GONE — the yellow triangle died
// with it. Clamps are boxes + a thin lit slit; see the chevron block below.)

// Add a PRE-BUILT (origin-centered) PrimMesh as an emissive Entity, oriented via
// the three world-space basis axes + translated to (wx,wy,wz). Used for the
// procedural TORUS ring (the smooth stone gate) which is authored once per portal.
AddedEntity addOrientedEmissiveMesh(Scene& scene, x3::rhi::IRenderDevice& device,
                                    const x3::prims::PrimMesh& m,
                                    const float xAxis[3], const float yAxis[3], const float zAxis[3],
                                    float wx, float wy, float wz,
                                    const float tint[3], float emStrength,
                                    const SurfaceSet* sf = nullptr) {
    Entity e;
    e.mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                               m.index.data(), (uint32_t)m.index.size());
    if (sf && sf->ok) {
        e.tex = sf->albedo; e.normalTex = sf->normal; e.mrTex = sf->mr;
    }
    e.baseColor[0] = tint[0]; e.baseColor[1] = tint[1]; e.baseColor[2] = tint[2];
    e.baseColor[3] = 1.0f;
    e.emissive[0] = tint[0]; e.emissive[1] = tint[1]; e.emissive[2] = tint[2];
    e.emissive[3] = emStrength;
    e.tag = (uint32_t)Tag::Prop;
    makeXform(e.transform, xAxis, yAxis, zAxis, wx, wy, wz);
    uint32_t id = scene.add(e);
    return AddedEntity{ e.mesh, id };
}

// ---- Membrane disk mesh (two-sided fan) -----------------------------------------
// A flat disk of `segs` fan triangles in the LOCAL XY plane (normal +Z — the
// same object-space convention as makeTorus, so the SAME portal basis orients
// both). Two-sided: the index list winds both ways so the membrane reads from
// inside AND outside the hub circle. UVs map the disk inscribed in [0,1]^2 so
// the plasma/vista textures land centered.
x3::prims::PrimMesh makeMembraneDisk(float radius, uint32_t segs) {
    x3::prims::PrimMesh m;
    const float twoPi = 6.2831853f;
    m.verts.push_back({ { 0, 0, 0 }, { 0, 0, 1 }, { 0.5f, 0.5f } });
    for (uint32_t s = 0; s <= segs; ++s) {
        const float th = (float)s * (twoPi / (float)segs);
        const float c = std::cos(th), sn = std::sin(th);
        m.verts.push_back({ { radius * c, radius * sn, 0 }, { 0, 0, 1 },
                            { 0.5f + 0.5f * c, 0.5f - 0.5f * sn } });
    }
    for (uint32_t s = 0; s < segs; ++s) {
        const uint32_t a = 1 + s, b = 2 + s;
        m.index.insert(m.index.end(), { 0u, a, b });   // front (+Z)
        m.index.insert(m.index.end(), { 0u, b, a });   // back  (-Z)
    }
    return m;
}

// ---- Procedural membrane textures ------------------------------------------------
// Tiny value-noise fBm toolkit (deterministic, build-time only). hash01 comes
// from mesh_prims.h (x3::prims). Smooth bilinear value noise over an n-lattice.
float valueNoise(float x, float y, uint32_t lattice, uint32_t salt) {
    const int xi = (int)std::floor(x), yi = (int)std::floor(y);
    const float fx = x - (float)xi, fy = y - (float)yi;
    auto lat = [&](int ix, int iy) {
        const uint32_t ux = (uint32_t)((ix % (int)lattice + (int)lattice) % (int)lattice);
        const uint32_t uy = (uint32_t)((iy % (int)lattice + (int)lattice) % (int)lattice);
        return x3::prims::detail::hash01(ux, uy, lattice, salt);
    };
    const float sx = fx * fx * (3.0f - 2.0f * fx);
    const float sy = fy * fy * (3.0f - 2.0f * fy);
    const float a = lat(xi, yi),     b = lat(xi + 1, yi);
    const float c = lat(xi, yi + 1), d = lat(xi + 1, yi + 1);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}
float fbm(float x, float y, int octaves, uint32_t salt) {
    float sum = 0.0f, amp = 0.5f, freq = 1.0f;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * valueNoise(x * freq, y * freq, 1024u, salt + (uint32_t)o * 131u);
        amp *= 0.5f; freq *= 2.02f;
    }
    return sum;
}
// RIDGED fbm: 1-|2n-1| per octave — sharp bright creases on a dark field, the
// classic "lightning filament web" structure.
float ridged(float x, float y, int octaves, uint32_t salt) {
    float sum = 0.0f, amp = 0.55f, freq = 1.0f;
    for (int o = 0; o < octaves; ++o) {
        float n = valueNoise(x * freq, y * freq, 1024u, salt + (uint32_t)o * 977u);
        n = 1.0f - std::fabs(2.0f * n - 1.0f);
        sum += amp * n * n;
        amp *= 0.55f; freq *= 2.15f;
    }
    return sum;
}

// PLASMA-STORM emissive map: deep blue field, white-blue ridged filament webs,
// brighter toward the center, dark falloff at the rim (the rim ring carries the
// edge glow instead — keeps the silhouette round and the edge crisp). A slight
// radius-proportional angular twist is baked in so the web looks sheared by the
// storm's rotation even on the first frame.
std::vector<uint8_t> makePlasmaRGBA(uint32_t n) {
    std::vector<uint8_t> px(n * n * 4);
    auto clamp8 = [](float v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); };
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            const float u = ((float)x + 0.5f) / (float)n * 2.0f - 1.0f;   // [-1,1]
            const float v = ((float)y + 0.5f) / (float)n * 2.0f - 1.0f;
            const float r = std::sqrt(u * u + v * v);                     // 0..~1.41
            // Storm-shear domain twist: rotate the sample by r-proportional angle.
            const float tw = r * 2.4f;
            const float cu = u * std::cos(tw) - v * std::sin(tw);
            const float cv = u * std::sin(tw) + v * std::cos(tw);
            const float fil  = ridged(cu * 3.2f + 7.3f, cv * 3.2f + 2.1f, 5, 0xB01Du);
            const float body = fbm(cu * 2.2f + 3.7f, cv * 2.2f + 9.4f, 4, 0x5EEDu);
            // Radial envelope: bright-ish core, live mid, dark rim (r>1 unused corners).
            float env = 1.0f - r;               // 1 center -> 0 rim
            if (env < 0.0f) env = 0.0f;
            env = 0.22f + 0.78f * env * env;    // keep the mid-field alive
            // Filament intensity: sharpen the ridged web into distinct tendrils.
            float t = fil - 0.55f;
            if (t < 0.0f) t = 0.0f;
            t = t * 2.6f;
            if (t > 1.0f) t = 1.0f;
            t = t * t;
            // Deep blue base modulated by body noise -> filament white-blue on top.
            const float baseR =  6.0f + 26.0f * body;
            const float baseG = 22.0f + 60.0f * body;
            const float baseB = 90.0f + 120.0f * body;
            const float filR = 210.0f, filG = 232.0f, filB = 255.0f;
            float R = (baseR + (filR - baseR) * t) * env;
            float G = (baseG + (filG - baseG) * t) * env;
            float B = (baseB + (filB - baseB) * t) * env;
            uint8_t* p = &px[(y * n + x) * 4];
            p[0] = clamp8(R); p[1] = clamp8(G); p[2] = clamp8(B); p[3] = 255;
        }
    }
    return px;
}

// OPEN-THROAT emissive map (PortalAnimated.mp4 state 3): radial plasma
// STREAMING from a hot center — ridged noise sampled in POLAR space with a
// high angular frequency and a low radial frequency, so the creases become
// spokes rushing outward; a slight spiral twist keeps it organic. The disk's
// faster OPEN rotation makes the spokes visibly stream.
std::vector<uint8_t> makeThroatRGBA(uint32_t n) {
    std::vector<uint8_t> px(n * n * 4);
    auto clamp8 = [](float v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); };
    const float twoPi = 6.2831853f;
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            const float u = ((float)x + 0.5f) / (float)n * 2.0f - 1.0f;
            const float v = ((float)y + 0.5f) / (float)n * 2.0f - 1.0f;
            const float r = std::sqrt(u * u + v * v);
            float ang = std::atan2(v, u);                  // [-pi, pi]
            ang += r * 1.8f;                               // spiral twist
            // Polar-space ridged noise: angular coord wraps via cos/sin embedding
            // (sampling on the unit circle keeps the -pi/+pi seam continuous).
            const float sx = std::cos(ang) * 2.9f, sy = std::sin(ang) * 2.9f;
            const float spokes = ridged(sx + r * 1.3f + 5.1f, sy + r * 1.3f + 8.7f, 4, 0x09E4u);
            const float body   = fbm(u * 2.0f + 1.3f, v * 2.0f + 6.6f, 3, 0x7EAAu);
            // Radial envelope: HOT center streaming out, alive to near the rim.
            float env = 1.0f - r;
            if (env < 0.0f) env = 0.0f;
            const float core = std::exp(-r * r * 9.0f);    // hot center burst
            float t = spokes - 0.42f;
            if (t < 0.0f) t = 0.0f;
            t *= 2.2f; if (t > 1.0f) t = 1.0f;
            const float e2 = 0.30f + 0.70f * env;
            const float baseR = 10.0f + 30.0f * body, baseG = 30.0f + 66.0f * body,
                        baseB = 110.0f + 110.0f * body;
            const float filR = 205.0f, filG = 230.0f, filB = 255.0f;
            float R = (baseR + (filR - baseR) * t) * e2 + core * 150.0f;
            float G = (baseG + (filG - baseG) * t) * e2 + core * 190.0f;
            float B = (baseB + (filB - baseB) * t) * e2 + core * 255.0f;
            uint8_t* p = &px[(y * n + x) * 4];
            p[0] = clamp8(R); p[1] = clamp8(G); p[2] = clamp8(B); p[3] = 255;
        }
    }
    return px;
}

// VISTA backdrop: the "other world" glimpsed through the storm — a dark
// night-sky gradient with teal-blue nebula banding and a scatter of stars,
// dark enough that it only reads where the plasma web is thin.
std::vector<uint8_t> makeVistaRGBA(uint32_t n) {
    std::vector<uint8_t> px(n * n * 4);
    auto clamp8 = [](float v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); };
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            const float u = ((float)x + 0.5f) / (float)n;
            const float v = ((float)y + 0.5f) / (float)n;
            const float sky = 1.0f - v;                       // brighter "horizon" low
            const float neb = fbm(u * 5.0f + 11.0f, v * 5.0f + 4.0f, 4, 0xA57Au);
            float R = 4.0f  + 14.0f * neb + 8.0f  * sky;
            float G = 10.0f + 34.0f * neb + 18.0f * sky;
            float B = 18.0f + 52.0f * neb + 30.0f * sky;
            // Star scatter: rare hash spikes.
            const float star = x3::prims::detail::hash01(x, y, n, 0x57A2u);
            if (star > 0.9985f) { R = 190.0f; G = 215.0f; B = 255.0f; }
            uint8_t* p = &px[(y * n + x) * 4];
            p[0] = clamp8(R); p[1] = clamp8(G); p[2] = clamp8(B); p[3] = 255;
        }
    }
    return px;
}

// TEAL HOLO DATA SCREEN texture: dark navy glass field, teal border + header
// bar, rows of "text" block bars (hash-varied widths), a trace graph line,
// and horizontal scanlines. Two variants (salt) so neighbouring screens
// don't read as clones. Deliberately abstract — set dressing, not UI.
std::vector<uint8_t> makeHoloDataRGBA(uint32_t n, uint32_t salt) {
    std::vector<uint8_t> px(n * n * 4);
    auto put = [&](uint32_t x, uint32_t y, float r, float g, float b) {
        uint8_t* p = &px[(y * n + x) * 4];
        p[0] = (uint8_t)(r * 255.0f); p[1] = (uint8_t)(g * 255.0f);
        p[2] = (uint8_t)(b * 255.0f); p[3] = 255;
    };
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            // Base: dark blue-black glass + subtle scanlines every 4 px.
            float r = 0.010f, g = 0.028f, b = 0.030f;
            if ((y & 3u) == 0u) { g += 0.012f; b += 0.012f; }
            // Border + header band.
            const bool border = x < 6 || x >= n - 6 || y < 6 || y >= n - 6;
            const bool header = y >= 10 && y < 34;
            if (border) { r = 0.05f; g = 0.55f; b = 0.48f; }
            else if (header && x > 14 && x < n - 14) { r = 0.04f; g = 0.30f; b = 0.27f; }
            put(x, y, r, g, b);
        }
    }
    // "Text" rows: teal block bars with hash-varied widths.
    for (uint32_t row = 0; row < 9; ++row) {
        const uint32_t y0 = 48 + row * 20;
        if (y0 + 8 >= n - 80) break;
        uint32_t x0 = 16;
        while (x0 + 10 < n - 20) {
            const float w01 = x3::prims::detail::hash01(x0, row * 7u + 1u, n, salt);
            const uint32_t w = 8 + (uint32_t)(w01 * 26.0f);
            const bool lit = x3::prims::detail::hash01(x0, row * 13u + 3u, n, salt ^ 0x99u) > 0.25f;
            if (lit)
                for (uint32_t y = y0; y < y0 + 8; ++y)
                    for (uint32_t x = x0; x < std::min(x0 + w, n - 20); ++x)
                        put(x, y, 0.06f, 0.62f, 0.55f);
            x0 += w + 8;
        }
    }
    // Trace graph strip along the bottom: a wandering bright teal line.
    {
        const uint32_t gy0 = n - 72, gy1 = n - 16;
        float v = 0.5f;
        for (uint32_t x = 12; x < n - 12; ++x) {
            v += (x3::prims::detail::hash01(x, 5u, n, salt ^ 0x42u) - 0.5f) * 0.16f;
            if (v < 0.05f) v = 0.05f; if (v > 0.95f) v = 0.95f;
            const uint32_t y = gy0 + (uint32_t)((float)(gy1 - gy0) * v);
            for (int dy = -1; dy <= 1; ++dy)
                put(x, (uint32_t)((int)y + dy), 0.10f, 0.90f, 0.80f);
        }
    }
    return px;
}

// Compose a beam transform: unit box (half-extent 0.5) stretched a->b with a
// square cross-section of `thickness` half-extent — the CombatFx drawBeam
// technique, re-implemented here for the membrane tendrils (clean-room, our
// own fx.cpp precedent).
void beamXform(float m[16], const x3::phys::Vec3& a, const x3::phys::Vec3& b,
               float thickness) {
    x3::phys::Vec3 seg{ b.x - a.x, b.y - a.y, b.z - a.z };
    const float len = std::sqrt(seg.x * seg.x + seg.y * seg.y + seg.z * seg.z);
    const float inv = (len > 1e-6f) ? 1.0f / len : 0.0f;
    const x3::phys::Vec3 d{ seg.x * inv, seg.y * inv, seg.z * inv };
    x3::phys::Vec3 ref = (std::fabs(d.y) < 0.99f) ? x3::phys::Vec3{ 0, 1, 0 }
                                                  : x3::phys::Vec3{ 1, 0, 0 };
    // u = normalize(ref x d), v = d x u.
    x3::phys::Vec3 u{ ref.y * d.z - ref.z * d.y,
                      ref.z * d.x - ref.x * d.z,
                      ref.x * d.y - ref.y * d.x };
    const float ul = std::sqrt(u.x * u.x + u.y * u.y + u.z * u.z);
    const float uinv = (ul > 1e-6f) ? 1.0f / ul : 0.0f;
    u.x *= uinv; u.y *= uinv; u.z *= uinv;
    const x3::phys::Vec3 v{ d.y * u.z - d.z * u.y,
                            d.z * u.x - d.x * u.z,
                            d.x * u.y - d.y * u.x };
    const float t2 = thickness * 2.0f;
    m[0] = u.x * t2;  m[1] = u.y * t2;  m[2] = u.z * t2;  m[3] = 0;
    m[4] = v.x * t2;  m[5] = v.y * t2;  m[6] = v.z * t2;  m[7] = 0;
    m[8] = d.x * len; m[9] = d.y * len; m[10] = d.z * len; m[11] = 0;
    m[12] = (a.x + b.x) * 0.5f; m[13] = (a.y + b.y) * 0.5f; m[14] = (a.z + b.z) * 0.5f; m[15] = 1;
}

} // namespace

void Rifthub::build(Scene& scene, x3::rhi::IRenderDevice& device,
                    x3::phys::IPhysicsWorld& physics, TriggerSystem& triggers) {
    if (m_built) return;

    // ===== Spawn point (center of the ring) =====
    m_spawn = x3::phys::Vec3{ 0.0f, kSpawnFeetY, 0.0f };

    // ===== Shared membrane v2 + FX resources (one set for all 8 portals) =====
    // Disk fan + rim torus meshes are SHARED handles referenced by every
    // portal's membrane entities; the plasma/throat/vista emissive maps + the
    // 1x1 MR texel (forces the PBR route so emissiveTex is honoured) likewise.
    // The FX beam box is the unit box the lightning arcs (and the hall's
    // catenary cables) stretch per-instance. Created FIRST — the hall reuses it.
    {
        x3::prims::PrimMesh disk = makeMembraneDisk(kMembraneR, kMembraneDiskSegs);
        m_diskMesh = device.createMesh(disk.verts.data(), (uint32_t)disk.verts.size(),
                                       disk.index.data(), (uint32_t)disk.index.size());
        x3::prims::PrimMesh rim = x3::prims::makeTorus(kRimR, kRimTubeR, 48, 10);
        m_rimMesh = device.createMesh(rim.verts.data(), (uint32_t)rim.verts.size(),
                                      rim.index.data(), (uint32_t)rim.index.size());
        x3::prims::PrimMesh beam = x3::prims::makeBox(0.5f, 0.5f, 0.5f, 0, 0, 0);
        m_fxBeamMesh = device.createMesh(beam.verts.data(), (uint32_t)beam.verts.size(),
                                         beam.index.data(), (uint32_t)beam.index.size());
        auto plasmaPx = makePlasmaRGBA(512);
        m_plasmaTex = device.createTexture(plasmaPx.data(), 512, 512, true);
        auto throatPx = makeThroatRGBA(512);
        m_throatTex = device.createTexture(throatPx.data(), 512, 512, true);
        auto vistaPx = makeVistaRGBA(256);
        m_vistaTex = device.createTexture(vistaPx.data(), 256, 256, true);
        // MR texel: glTF packing G=roughness B=metallic -> fully rough dielectric.
        const uint8_t mrPx[4] = { 0, 255, 0, 255 };
        m_mrFlat = device.createTexture(mrPx, 1, 1, false);
        // Teal holo data-screen textures (two variants; phase D dressing).
        auto holoA = makeHoloDataRGBA(256, 0x1AB5u);
        m_holoTexA = device.createTexture(holoA.data(), 256, 256, true);
        auto holoB = makeHoloDataRGBA(256, 0x7C3Du);
        m_holoTexB = device.createTexture(holoB.data(), 256, 256, true);
    }

    // ===== Curated PBR surface sets — loaded FIRST (the floor + hall use them).
    // The library owns the textures (freed in shutdown via destroyAll).
    // LFS-budget note (2026-07-11): the intro-cockpit rusted/patina sets could
    // NOT be harvested (GitHub LFS budget exhausted), so everything dresses
    // from the 24 sets already materialized on this branch, tinted toward the
    // locked teal-oxide patina.
    m_surf.mount(assetRoot() + "/surface_library");
    const SurfaceSet& sPlate = m_surf.get(device, "mw_metal_panels_a");  // riveted industrial panels
    const SurfaceSet& sDark  = m_surf.get(device, "sr_metal_b");         // dark steel (housings/cradle)
    const SurfaceSet& sTrim  = m_surf.get(device, "mw_metal_trim_b");    // trim (plates variant)
    const SurfaceSet& sFloor = m_surf.get(device, "sr_concrete_01");     // hall floor concrete
    const SurfaceSet& sWall  = m_surf.get(device, "mw_concrete_panels_b"); // hall walls
    // Wet-floor MR texel (glTF packing G=rough B=metal): low roughness => the
    // dark concrete takes tight specular + IBL sheen (the wet reflective read).
    {
        const uint8_t wetPx[4] = { 0, 52, 24, 255 };
        m_mrWet = device.createTexture(wetPx, 1, 1, false);
    }

    // ===== Ground (static collision + the WET CONCRETE floor) =====
    // 40x40 m flat slab at y=-0.10 so the slab TOP sits at y=0 (the world Y=0
    // plane every other graybox uses). Phase C: the dev checker is gone — dark
    // concrete albedo+normal from the library with the glossy wet MR override.
    {
        x3::prims::PrimMesh g = x3::prims::makeBox(kHubHalf, 0.10f, kHubHalf,
                                                    0.0f, -0.10f, 0.0f, 0.22f);
        m_groundMesh = device.createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                         g.index.data(), (uint32_t)g.index.size());
        Entity ge;
        ge.mesh = m_groundMesh;
        if (sFloor.ok) {
            ge.tex = sFloor.albedo; ge.normalTex = sFloor.normal; ge.mrTex = m_mrWet;
        }
        ge.baseColor[0] = kFloorTint[0]; ge.baseColor[1] = kFloorTint[1];
        ge.baseColor[2] = kFloorTint[2]; ge.baseColor[3] = 1.0f;
        ge.tag = (uint32_t)Tag::Static;
        scene.add(ge);
        physics.addStaticMesh(g.cverts.data(), (uint32_t)(g.cverts.size() / 3),
                              g.cindex.data(), (uint32_t)g.cindex.size());
    }

    // ===== HALL SHELL (walls / ceiling / columns / beams / strips / cables) ====
    {
        const float ident[3][3] = { {1,0,0}, {0,1,0}, {0,0,1} };
        const float wallMidY = kHallWallH * 0.5f;
        // A collidable, textured, world-baked box (walls/columns): verts baked
        // in world space (identity transform), collision from the same mesh.
        auto hallBox = [&](float cx0, float cy0, float cz0, float hx, float hy, float hz,
                           const SurfaceSet* sf, const float tint[3], float uv,
                           bool collide, float em = 0.0f) {
            x3::prims::PrimMesh b = x3::prims::makeBox(hx, hy, hz, cx0, cy0, cz0, uv);
            Entity e;
            e.mesh = device.createMesh(b.verts.data(), (uint32_t)b.verts.size(),
                                       b.index.data(), (uint32_t)b.index.size());
            m_portalMeshes.push_back(e.mesh);
            if (sf && sf->ok) { e.tex = sf->albedo; e.normalTex = sf->normal; e.mrTex = sf->mr; }
            e.baseColor[0] = tint[0]; e.baseColor[1] = tint[1]; e.baseColor[2] = tint[2];
            e.baseColor[3] = 1.0f;
            e.emissive[0] = tint[0]; e.emissive[1] = tint[1]; e.emissive[2] = tint[2];
            e.emissive[3] = em;
            e.tag = (uint32_t)Tag::Static;
            scene.add(e);
            if (collide)
                physics.addStaticMesh(b.cverts.data(), (uint32_t)(b.cverts.size() / 3),
                                      b.cindex.data(), (uint32_t)b.cindex.size());
        };
        // Walls (SEAM LAW): inner faces exactly at +/-kHubHalf. N/S walls span
        // the full width INCLUDING the E/W wall thickness; E/W walls butt
        // between them — no coplanar overlap at the corners, no gaps.
        const float wallC = kHubHalf + kHallWallT * 0.5f;   // wall center plane
        const float nsHalfX = kHubHalf + kHallWallT;         // full span incl. corners
        const float ewHalfZ = kHubHalf;                      // butts between N/S
        hallBox(0.0f, wallMidY,  wallC, nsHalfX, wallMidY + 0.10f, kHallWallT * 0.5f,
                &sWall, kConcreteTint, 0.25f, /*collide=*/true);
        hallBox(0.0f, wallMidY, -wallC, nsHalfX, wallMidY + 0.10f, kHallWallT * 0.5f,
                &sWall, kConcreteTint, 0.25f, /*collide=*/true);
        hallBox( wallC, wallMidY, 0.0f, kHallWallT * 0.5f, wallMidY + 0.10f, ewHalfZ,
                &sWall, kConcreteTint, 0.25f, /*collide=*/true);
        hallBox(-wallC, wallMidY, 0.0f, kHallWallT * 0.5f, wallMidY + 0.10f, ewHalfZ,
                &sWall, kConcreteTint, 0.25f, /*collide=*/true);
        // Ceiling slab: overlaps the wall tops (flush lid, no sky leak).
        hallBox(0.0f, kHallWallH + kHallCeilT * 0.5f, 0.0f,
                nsHalfX, kHallCeilT * 0.5f, nsHalfX,
                &sWall, kDarkTint, 0.12f, /*collide=*/false);
        // Perimeter steel columns: corners + wall midpoints, floor -> ceiling.
        {
            const float cpos = kHubHalf - kColumnHalf;   // flush against the walls
            const float colXZ[kHallColumns][2] = {
                {  cpos,  cpos }, { -cpos,  cpos }, {  cpos, -cpos }, { -cpos, -cpos },
                {  cpos,  0.0f }, { -cpos,  0.0f }, {  0.0f,  cpos }, {  0.0f, -cpos },
            };
            for (uint32_t c = 0; c < kHallColumns; ++c)
                hallBox(colXZ[c][0], wallMidY, colXZ[c][1],
                        kColumnHalf, wallMidY, kColumnHalf,
                        &sPlate, kDarkTint, 0.5f, /*collide=*/true);
        }
        // Ceiling I-beam grid: kHallBeams beams along X and along Z at kBeamY.
        for (uint32_t b = 0; b < kHallBeams; ++b) {
            const float o = -16.0f + (float)b * 8.0f;
            hallBox(0.0f, kBeamY, o, kHubHalf, kBeamHalfH, kBeamHalfW,
                    &sPlate, kDarkTint, 0.5f, /*collide=*/false);
            hallBox(o, kBeamY - 0.02f, 0.0f, kBeamHalfW, kBeamHalfH - 0.02f, kHubHalf,
                    &sPlate, kDarkTint, 0.5f, /*collide=*/false);
        }
        // Ceiling strip lights: emissive bars under the beams over the gate
        // circle (capped — the STRIPS read as fixtures; the point lights below
        // carry the actual illumination).
        for (uint32_t s3 = 0; s3 < kStripCount; ++s3) {
            const float ang = ((float)s3 + 0.5f) * (6.2831853f / (float)kStripCount);
            const float sx = std::cos(ang) * 10.0f;
            const float sz = std::sin(ang) * 10.0f;
            x3::prims::PrimMesh b = x3::prims::makeBox(1.6f, 0.045f, 0.16f, sx, kBeamY - 0.38f, sz);
            Entity e;
            e.mesh = device.createMesh(b.verts.data(), (uint32_t)b.verts.size(),
                                       b.index.data(), (uint32_t)b.index.size());
            m_portalMeshes.push_back(e.mesh);
            e.baseColor[0] = kStripTint[0]; e.baseColor[1] = kStripTint[1];
            e.baseColor[2] = kStripTint[2]; e.baseColor[3] = 1.0f;
            e.emissive[0] = kStripTint[0]; e.emissive[1] = kStripTint[1];
            e.emissive[2] = kStripTint[2]; e.emissive[3] = kStripEm;
            e.tag = (uint32_t)Tag::Static;
            scene.add(e);
        }
        // Hanging catenary cables: sagging thin runs between beam crossings —
        // every segment reuses the SHARED unit fx box via a beam transform
        // (zero extra meshes). Deterministic drape per cable index.
        for (uint32_t cb = 0; cb < kCableCount; ++cb) {
            auto h01 = [&](uint32_t salt) {
                return x3::prims::detail::hash01(cb * 7u + 3u, cb * 13u + 1u, 4096u, salt);
            };
            const float x0 = -16.0f + 32.0f * h01(0x11u);
            const float z0 = -16.0f + 32.0f * h01(0x22u);
            const float x1 = x0 + (-10.0f + 20.0f * h01(0x33u));
            const float z1 = z0 + (-10.0f + 20.0f * h01(0x44u));
            const float sag = kCableSag * (0.7f + 0.6f * h01(0x55u));
            x3::phys::Vec3 prev{ x0, kBeamY - kBeamHalfH, z0 };
            for (uint32_t s3 = 1; s3 <= kCableSegs; ++s3) {
                const float t = (float)s3 / (float)kCableSegs;
                // Quadratic drape: max sag at the middle.
                const float y = kBeamY - kBeamHalfH - sag * 4.0f * t * (1.0f - t);
                x3::phys::Vec3 pt{ x0 + (x1 - x0) * t, y, z0 + (z1 - z0) * t };
                Entity e;
                e.mesh = m_fxBeamMesh;   // SHARED unit box
                e.baseColor[0] = 0.05f; e.baseColor[1] = 0.05f; e.baseColor[2] = 0.055f;
                e.baseColor[3] = 1.0f;
                e.tag = (uint32_t)Tag::Static;
                beamXform(e.transform, prev, pt, 0.016f);
                scene.add(e);
                prev = pt;
            }
        }
        // Distant machinery silhouettes: dark blocky clusters hugging the
        // walls between columns — they read as shapes in the fog, not detail.
        for (uint32_t mc = 0; mc < 8; ++mc) {
            auto h01 = [&](uint32_t salt) {
                return x3::prims::detail::hash01(mc * 5u + 2u, mc * 11u + 7u, 4096u, salt);
            };
            const float ang = (float)mc * (6.2831853f / 8.0f) + 0.39f;
            const float mx = std::cos(ang) * (kHubHalf - 2.2f);
            const float mz = std::sin(ang) * (kHubHalf - 2.2f);
            const float bw = 0.9f + 1.4f * h01(0x66u);
            const float bh = 1.6f + 2.6f * h01(0x77u);
            const float bd = 0.7f + 1.1f * h01(0x88u);
            hallBox(mx, bh * 0.5f, mz, bw, bh * 0.5f, bd,
                    &sDark, kDarkTint, 0.5f, /*collide=*/true, /*em=*/0.02f);
            // A smaller unit stacked on top + a vent pipe to break the box read.
            hallBox(mx + bw * 0.3f, bh + 0.45f, mz, bw * 0.45f, 0.45f, bd * 0.6f,
                    &sDark, kDarkTint, 0.5f, /*collide=*/false, /*em=*/0.02f);
            hallBox(mx - bw * 0.5f, bh + 0.9f, mz, 0.09f, 0.9f, 0.09f,
                    &sDark, kDarkTint, 0.5f, /*collide=*/false, /*em=*/0.02f);
        }
    }

    // ===== Portals (clockwise ring around the spawn) =====
    m_portals.clear();
    m_portals.reserve(kPortalCount);
    // Each portal authors: 1 ring torus + kPlateSegments floor wedges +
    // kChevronCount amber chevron prisms + 2 core disks (per-entity meshes),
    // plus the 3 shared-mesh membrane entities (vista/plasma/rim).
    m_portalMeshes.reserve(kPortalCount *
        (1 + kPlateSegments + kChevronCount + 2));

    const float twoPi = 6.2831853f;
    for (uint32_t i = 0; i < kPortalCount; ++i) {
        const PortalSpec& sp = kPortalTable[i];
        // Hub angle for portal i — clockwise starting at +X.
        const float hubAng = (float)i * (twoPi / (float)kPortalCount);
        const float cx = std::cos(hubAng) * kRingRadius;
        const float cz = std::sin(hubAng) * kRingRadius;

        RiftPortal p;
        p.worldName  = sp.worldName;
        p.triggerId  = sp.triggerId;
        p.worldPos   = x3::phys::Vec3{ cx, 0.0f, cz };
        p.tint[0] = sp.tint[0]; p.tint[1] = sp.tint[1]; p.tint[2] = sp.tint[2];
        p.activated  = false;

        // ---- Portal-local basis ---------------------------------------------
        // The "outward" axis (radial from hub center to portal center) is the
        // ring's NORMAL — the ring's plane is perpendicular to it, so the
        // portal's doorway face points back toward the hub center. Up is world
        // +Y. "Right" (along the ring's plane, horizontal) is up x outward.
        const float invR = 1.0f / kRingRadius;
        const float outwardX = cx * invR;
        const float outwardZ = cz * invR;
        // Right = (0,1,0) x (outwardX, 0, outwardZ) = (-outwardZ, 0, outwardX). Unit.
        const float rightX = -outwardZ;
        const float rightZ =  outwardX;

        // ---- Stone gateway ring (single thick circle of deep segments) -------
        // For each segment at ring angle θ (0..2π going CCW in the ring's local
        // plane starting from local +right = world (rightX,0,rightZ)):
        //   - center in ring plane = R*cos(θ)*right + R*sin(θ)*up
        //   - segment tangent      = -sin(θ)*right + cos(θ)*up
        //   - segment radial-out   =  cos(θ)*right + sin(θ)*up
        //   - outward stays the outward axis (depth through the ring)
        // TRUE PROCEDURAL TORUS (step-2 AAA ring): one smooth-shaded stone donut
        // per portal replaces the old 40 tangent box segments (chunky-butt
        // overlap). Authored origin-centered in its local XY plane (hole along
        // local +Z) then oriented so local +X = ring "right", +Y = world up,
        // +Z = outward — the hole faces back at the hub center and the player
        // walks through along outward. Centerline R = kRingR (2.05 m), tube
        // r = kRingTubeR (0.40 m) => 0.80 m band / 0.80 m walk-through depth.
        // Grey stone, faint self-lift only (kRingEmissive ~0.30, NOT a glow) —
        // the blue core light does the actual lighting of the stone.
        const uint32_t ringEntFirst = scene.size();
        {
            x3::prims::PrimMesh torus =
                x3::prims::makeTorus(kRingR, kRingTubeR, kRingMajorSeg, kRingMinorSeg);
            // Tile the metal set around the ring: u wraps the 12.9 m major
            // circumference ONCE by default (a smeared stretch) — rescale so a
            // tile lands roughly every 1.3 m major / 1.25 m minor.
            for (auto& v : torus.verts) { v.uv[0] *= 10.0f; v.uv[1] *= 2.0f; }
            const float locX[3] = { rightX, 0.0f, rightZ };   // ring "right"
            const float locY[3] = { 0.0f,   1.0f, 0.0f    };   // world up
            const float locZ[3] = { outwardX, 0.0f, outwardZ };// outward (hole axis)
            AddedEntity ae = addOrientedEmissiveMesh(
                scene, device, torus,
                locX, locY, locZ,
                cx, kRingY, cz,
                kRingStone, /*emStrength=*/kRingEmissive, &sPlate);
            m_portalMeshes.push_back(ae.mesh);
        }
        p.ringEntFirst = ringEntFirst;
        p.ringEntCount = 1;   // one torus entity (was kRingSegments box segments)

        // ---- Ring v2 OVER-PLATES + rivets (industrial armor over the torus) --
        // kPlateArcCount varied plates seated over the tube crest at jittered
        // angular slots + sizes (deterministic per portal+slot hash), skinned
        // from the curated sets, alternating patina/steel tints. Rivet studs
        // dot the hub-facing front face between chevrons.
        {
            auto h01 = [&](uint32_t s3, uint32_t salt) {
                return x3::prims::detail::hash01(i * 131u + s3, s3 * 17u + 5u, 4096u, salt);
            };
            for (uint32_t s3 = 0; s3 < kPlateArcCount; ++s3) {
                const float slot = (float)s3 * (twoPi / (float)kPlateArcCount);
                const float th   = slot + (h01(s3, 0xA1u) - 0.5f) * 0.18f;
                const float ct = std::cos(th), st = std::sin(th);
                const float radX = ct * rightX, radY = st, radZ = ct * rightZ;
                const float tanX = -st * rightX, tanY = ct, tanZ = -st * rightZ;
                const float locX[3] = { tanX, tanY, tanZ };       // tangent (arc width)
                const float locY[3] = { radX, radY, radZ };       // radial (plate thickness)
                const float locZ[3] = { outwardX, 0.0f, outwardZ };// through-gate depth
                const float halfTan = 0.34f + 0.28f * h01(s3, 0xB2u);
                const float halfRad = 0.075f + 0.070f * h01(s3, 0xC3u);   // varied depth
                const float halfDep = 0.30f + 0.16f * h01(s3, 0xD4u);
                const float seatR   = kPlateSeatR + halfRad * 0.5f;
                const bool patina   = h01(s3, 0xE5u) > 0.45f;
                AddedEntity ae = addOrientedSurfBox(
                    scene, device, halfTan, halfRad, halfDep,
                    locX, locY, locZ,
                    cx + seatR * radX, kRingY + seatR * radY, cz + seatR * radZ,
                    patina ? &sPlate : &sTrim,
                    patina ? kPatinaTint : kSteelTint,
                    /*emStrength=*/0.05f, /*uvScale=*/0.9f);
                m_portalMeshes.push_back(ae.mesh);
            }
            // Rivet studs on the front face (small dark boxes, offset between
            // the chevron seats).
            for (uint32_t rv = 0; rv < kRivetCount; ++rv) {
                const float th = ((float)rv + 0.5f) * (twoPi / (float)kRivetCount) + 1.5707963f;
                const float ct = std::cos(th), st = std::sin(th);
                const float radX = ct * rightX, radY = st, radZ = ct * rightZ;
                const float locX[3] = { -st * rightX, ct, -st * rightZ };
                const float locY[3] = { radX, radY, radZ };
                const float locZ[3] = { outwardX, 0.0f, outwardZ };
                const float proud = kRingHalfDepth + kRivetHalf * 0.4f;
                AddedEntity ae = addOrientedSurfBox(
                    scene, device, kRivetHalf, kRivetHalf, kRivetHalf,
                    locX, locY, locZ,
                    cx + kRingR * radX - outwardX * proud,
                    kRingY + kRingR * radY,
                    cz + kRingR * radZ - outwardZ * proud,
                    &sDark, kDarkTint, /*emStrength=*/0.03f);
                m_portalMeshes.push_back(ae.mesh);
            }
        }

        // ---- A-frame support CRADLE (the gate is INSTALLED, not floating) ----
        // Base skirt plinth under the ring bottom + two canted A-frame struts
        // grabbing the ring's sides + floor anchor plates at the strut feet.
        {
            const float locX[3] = { rightX, 0.0f, rightZ };
            const float locY[3] = { 0.0f,   1.0f, 0.0f    };
            const float locZ[3] = { outwardX, 0.0f, outwardZ };
            // Skirt plinth (top at 0.50 m — below the 0.55 m ring opening).
            AddedEntity skirt = addOrientedSurfBox(
                scene, device, kSkirtHalfTan, kSkirtHalfY, kSkirtHalfDep,
                locX, locY, locZ,
                cx, kSkirtHalfY, cz,
                &sDark, kDarkTint, /*emStrength=*/0.04f, /*uvScale=*/0.6f);
            m_portalMeshes.push_back(skirt.mesh);
            for (int side = -1; side <= 1; side += 2) {
                const float bx = cx + rightX * kStrutBaseOut * (float)side;
                const float bz = cz + rightZ * kStrutBaseOut * (float)side;
                const float tx = cx + rightX * kStrutTopOut * (float)side;
                const float tz = cz + rightZ * kStrutTopOut * (float)side;
                // Canted strut blade: thickness along the gate's depth axis.
                x3::prims::PrimMesh strut = x3::prims::makeCantedStrut(
                    bx, 0.0f, bz, tx, kStrutTopY, tz,
                    kStrutHalfW, kStrutHalfT, outwardX, outwardZ, /*uvScale=*/0.5f);
                const float ident[3][3] = { {1,0,0}, {0,1,0}, {0,0,1} };
                AddedEntity ae = addOrientedEmissiveMesh(
                    scene, device, strut,
                    ident[0], ident[1], ident[2], 0.0f, 0.0f, 0.0f,
                    kSteelTint, /*emStrength=*/0.04f, &sDark);
                m_portalMeshes.push_back(ae.mesh);
                // Floor anchor plate at the strut foot.
                AddedEntity anchor = addOrientedSurfBox(
                    scene, device, kAnchorHalfTan, kAnchorHalfY, kAnchorHalfDep,
                    locX, locY, locZ,
                    bx, kAnchorHalfY, bz,
                    &sTrim, kSteelTint, /*emStrength=*/0.04f);
                m_portalMeshes.push_back(anchor.mesh);
            }
        }

        // ---- ORANGE conduits + coil rings + TEAL holo screens (phase D) ------
        // Portal-local frame for the dressing (the cradle's basis is scoped).
        const float dX[3] = { rightX, 0.0f, rightZ };
        const float dY[3] = { 0.0f, 1.0f, 0.0f };
        const float dZ[3] = { outwardX, 0.0f, outwardZ };
        // World point in the gate plane, nudged hub-side of the ring face.
        auto gatePt = [&](float alongRight, float y, float alongOut) {
            return x3::phys::Vec3{
                cx + rightX * alongRight + outwardX * alongOut,
                y,
                cz + rightZ * alongRight + outwardZ * alongOut };
        };
        // Conduit runs: gate -> riser -> floor -> skirt, one per side (the
        // right side rides higher, the left lower — no clone read). Every
        // segment is the SHARED unit fx box stretched a->b; contiguous span
        // so tick() can phase the emissive ALONG the run (power flow).
        p.conduitEntFirst = scene.size();
        {
            const float off = -0.58f;   // hub-side of the gate face
            const x3::phys::Vec3 runA[5] = {
                gatePt( 1.62f, 3.25f, off), gatePt( 2.46f, 2.55f, off),
                gatePt( 2.46f, 0.55f, off), gatePt( 1.95f, 0.16f, off),
                gatePt( 0.85f, 0.16f, off),
            };
            const x3::phys::Vec3 runB[5] = {
                gatePt(-1.85f, 2.85f, off), gatePt(-2.46f, 2.15f, off),
                gatePt(-2.46f, 0.50f, off), gatePt(-1.90f, 0.16f, off),
                gatePt(-0.85f, 0.16f, off),
            };
            auto addRun = [&](const x3::phys::Vec3* run) {
                for (int s3 = 0; s3 < 4; ++s3) {
                    Entity e;
                    e.mesh = m_fxBeamMesh;   // SHARED unit box
                    e.baseColor[0] = 0.35f; e.baseColor[1] = 0.18f; e.baseColor[2] = 0.06f;
                    e.baseColor[3] = 1.0f;
                    e.emissive[0] = kConduitOrange[0]; e.emissive[1] = kConduitOrange[1];
                    e.emissive[2] = kConduitOrange[2]; e.emissive[3] = kConduitEmBase;
                    e.tag = (uint32_t)Tag::Prop;
                    beamXform(e.transform, run[s3], run[s3 + 1], kConduitHalf);
                    scene.add(e);
                }
            };
            addRun(runA);
            addRun(runB);
        }
        p.conduitEntCount = 8;
        // Coil rings on the right riser (orange, static warm glow).
        for (int coil = 0; coil < 2; ++coil) {
            x3::prims::PrimMesh torus = x3::prims::makeTorus(0.14f, 0.032f, 20, 8);
            // Ring horizontal around the vertical riser: local X = gate right,
            // local Y = outward, local Z (hole axis) = world up.
            const float locX3[3] = { rightX, 0.0f, rightZ };
            const float locY3[3] = { outwardX, 0.0f, outwardZ };
            const float locZ3[3] = { 0.0f, 1.0f, 0.0f };
            const x3::phys::Vec3 at = gatePt(2.46f, 1.95f - 0.42f * (float)coil, -0.58f);
            AddedEntity ae = addOrientedEmissiveMesh(
                scene, device, torus, locX3, locY3, locZ3,
                at.x, at.y, at.z, kConduitOrange, kCoilOrangeEm);
            m_portalMeshes.push_back(ae.mesh);
        }
        // Two TEAL holo data screens flanking the approach (glass panes on
        // dark posts, facing back toward the hub center).
        for (int scr = -1; scr <= 1; scr += 2) {
            const float sideR = 2.45f * (float)scr;
            const x3::phys::Vec3 at = gatePt(sideR, 1.52f, -1.35f);
            // Post.
            AddedEntity post = addOrientedSurfBox(
                scene, device, 0.030f, 0.62f, 0.030f,
                dX, dY, dZ, at.x, 0.62f, at.z,
                &sDark, kDarkTint, 0.03f);
            m_portalMeshes.push_back(post.mesh);
            // Glass pane with the holo readout (the pane IS the screen — the
            // club OLED-glass lesson: a pane OVER a screen depth-occludes it).
            x3::prims::PrimMesh pane = x3::prims::makeBox(0.36f, 0.25f, 0.012f, 0, 0, 0);
            Entity e;
            e.mesh = device.createMesh(pane.verts.data(), (uint32_t)pane.verts.size(),
                                       pane.index.data(), (uint32_t)pane.index.size());
            m_portalMeshes.push_back(e.mesh);
            e.tex = (scr > 0) ? m_holoTexA : m_holoTexB;
            e.baseColor[0] = 2.0f; e.baseColor[1] = 2.3f; e.baseColor[2] = 2.3f;
            e.baseColor[3] = 1.0f;
            e.emissive[0] = kHoloTeal[0]; e.emissive[1] = kHoloTeal[1];
            e.emissive[2] = kHoloTeal[2]; e.emissive[3] = kHoloEm;
            e.transparent = true;
            e.glass.opacity = 0.88f;
            e.glass.refraction = 0.0f;
            e.glass.roughness = 0.06f;
            e.glass.specular = 1.0f;
            e.glass.tint[0] = 0.75f; e.glass.tint[1] = 1.0f; e.glass.tint[2] = 0.95f;
            e.tag = (uint32_t)Tag::Prop;
            // Face back toward the hub: pane thin axis (+Z local) = -outward.
            const float pX[3] = { -rightX, 0.0f, -rightZ };
            const float pY[3] = { 0.0f, 1.0f, 0.0f };
            const float pZ[3] = { -outwardX, 0.0f, -outwardZ };
            makeXform(e.transform, pX, pY, pZ, at.x, at.y, at.z);
            scene.add(e);
        }

        // ---- Octagonal floor plate (8 wedge boxes) ---------------------------
        // Each wedge is a thin box tangent to a circle of radius kPlateRingR,
        // axis-aligned in Y (flat on the ground). Reuses the same local basis
        // (right axis vs world Z) — for the floor plate the ring lives in the
        // WORLD XZ plane around the portal center, so the wedge tangent is the
        // tangent to the floor-ring circle.
        for (uint32_t s = 0; s < kPlateSegments; ++s) {
            const float th = (float)s * (twoPi / (float)kPlateSegments);
            const float ct = std::cos(th);
            const float st = std::sin(th);
            // Floor-ring local frame: local +right = (rightX,0,rightZ),
            //                         local +outward = (outwardX,0,outwardZ).
            // Wedge center: portal center + R*(ct*right + st*outward).
            const float wcx = cx + kPlateRingR * (ct * rightX + st * outwardX);
            const float wcy = kPlateHalfY;
            const float wcz = cz + kPlateRingR * (ct * rightZ + st * outwardZ);
            // Tangent (long axis, horizontal) = -st*right + ct*outward.
            const float tgX = -st * rightX + ct * outwardX;
            const float tgZ = -st * rightZ + ct * outwardZ;
            // Radial-out (in-plane on the floor) = ct*right + st*outward.
            const float rdX =  ct * rightX + st * outwardX;
            const float rdZ =  ct * rightZ + st * outwardZ;
            // Local +X = tangent (long arc), +Y = world up, +Z = radial-out.
            const float locX[3] = { tgX, 0.0f, tgZ };
            const float locY[3] = { 0.0f, 1.0f, 0.0f };
            const float locZ[3] = { rdX, 0.0f, rdZ };
            AddedEntity ae = addOrientedEmissiveBox(
                scene, device,
                plateHalfTangent(), kPlateHalfY, kPlateBoxThick,
                locX, locY, locZ,
                wcx, wcy, wcz,
                sp.tint, /*emStrength=*/0.45f);  // identity accent, not a beacon (cap law)
            m_portalMeshes.push_back(ae.mesh);
        }

        // ---- Chevron clamp HOUSINGS + amber SLIT cores (round 2) --------------
        // Chevron 0 sits at 12 o'clock; the rest step clockwise. Each clamp is
        // machined from boxes only: dark gunmetal BODY seated into the ring's
        // tube band + two deeper side JAW flanges + a stepped steel face CAP —
        // all textured from the PBR library, none emissive. Two passes so the
        // ANIMATED span stays contiguous: housings first, then the 9 thin
        // amber-lit SLIT strips tick() flickers (chevronEntFirst = the slits;
        // near-black baseColor so a dim slit reads as dark glass, not yellow).
        auto chevBasis = [&](uint32_t c, float out[3], float tan[3], float rad[3]) {
            const float th = 1.5707963f - (float)c * (twoPi / (float)kChevronCount);
            const float ct = std::cos(th);
            const float st = std::sin(th);
            rad[0] = ct * rightX; rad[1] = st; rad[2] = ct * rightZ;
            tan[0] = -st * rightX; tan[1] = ct; tan[2] = -st * rightZ;
            out[0] = outwardX; out[1] = 0.0f; out[2] = outwardZ;
        };
        const float houseProud = kRingHalfDepth + kHouseHalfDep * 0.35f;
        const float capProud   = houseProud + kHouseHalfDep + kCapHalfDep;
        for (uint32_t c = 0; c < kChevronCount; ++c) {
            float outv3[3], tanv3[3], radv3[3];
            chevBasis(c, outv3, tanv3, radv3);
            const float hcx = cx     + kChevSeatR * radv3[0] - outwardX * houseProud;
            const float hcy = kRingY + kChevSeatR * radv3[1];
            const float hcz = cz     + kChevSeatR * radv3[2] - outwardZ * houseProud;
            // Clamp body (dark gunmetal, spans the tube band — seated INTO the ring).
            AddedEntity body = addOrientedSurfBox(
                scene, device, kHouseHalfTan, kHouseHalfRad, kHouseHalfDep,
                tanv3, radv3, outv3, hcx, hcy, hcz,
                &sDark, kDarkTint, /*emStrength=*/0.02f);
            m_portalMeshes.push_back(body.mesh);
            // Two side jaw flanges — deeper than the body, the mechanical bite.
            for (int fside = -1; fside <= 1; fside += 2) {
                AddedEntity fl = addOrientedSurfBox(
                    scene, device, kFlangeHalfTan, kFlangeHalfRad, kFlangeHalfDep,
                    tanv3, radv3, outv3,
                    hcx + tanv3[0] * (kHouseHalfTan + kFlangeHalfTan) * (float)fside,
                    hcy + tanv3[1] * (kHouseHalfTan + kFlangeHalfTan) * (float)fside,
                    hcz + tanv3[2] * (kHouseHalfTan + kFlangeHalfTan) * (float)fside,
                    &sDark, kDarkTint, /*emStrength=*/0.02f);
                m_portalMeshes.push_back(fl.mesh);
            }
            // Stepped face-cap plate (body -> cap bevel step, brushed trim set).
            AddedEntity cap = addOrientedSurfBox(
                scene, device, kCapHalfTan, kCapHalfRad, kCapHalfDep,
                tanv3, radv3, outv3,
                cx     + kChevSeatR * radv3[0] - outwardX * capProud,
                kRingY + kChevSeatR * radv3[1],
                cz     + kChevSeatR * radv3[2] - outwardZ * capProud,
                &sTrim, kDarkTint, /*emStrength=*/0.02f);
            m_portalMeshes.push_back(cap.mesh);
        }
        const uint32_t chevronEntFirst = scene.size();
        for (uint32_t c = 0; c < kChevronCount; ++c) {
            float outv3[3], tanv3[3], radv3[3];
            chevBasis(c, outv3, tanv3, radv3);
            // The ONLY emitter: a thin amber slit strip barely proud of the cap.
            const float slitProud = capProud + kCapHalfDep + kChevSlitHalfDep * 0.6f;
            const float ccx = cx     + kChevSeatR * radv3[0] - outwardX * slitProud;
            const float ccy = kRingY + kChevSeatR * radv3[1];
            const float ccz = cz     + kChevSeatR * radv3[2] - outwardZ * slitProud;
            AddedEntity ae = addOrientedEmissiveBox(
                scene, device,
                kChevSlitHalfTan, kChevSlitHalfRad, kChevSlitHalfDep,
                tanv3, radv3, outv3,
                ccx, ccy, ccz,
                kChevAmber, /*emStrength=*/kChevMinEm, kChevSlitDark);
            m_portalMeshes.push_back(ae.mesh);
        }
        p.chevronEntFirst = chevronEntFirst;
        p.chevronEntCount = kChevronCount;

        // ---- Segmented amber RATCHET TRACK (inner front edge; the video's
        // activation-feedback detail). Contiguous span for tick()'s chase.
        p.trackEntFirst = scene.size();
        for (uint32_t t3 = 0; t3 < kTrackSegs; ++t3) {
            const float th = (float)t3 * (twoPi / (float)kTrackSegs);
            const float ct = std::cos(th), st = std::sin(th);
            const float radX = ct * rightX, radY = st, radZ = ct * rightZ;
            const float locX[3] = { -st * rightX, ct, -st * rightZ };
            const float locY[3] = { radX, radY, radZ };
            const float locZ[3] = { outwardX, 0.0f, outwardZ };
            const float proud = kRingHalfDepth + kTrackHalfDep * 0.6f;
            AddedEntity ae = addOrientedEmissiveBox(
                scene, device, kTrackHalfTan, kTrackHalfRad, kTrackHalfDep,
                locX, locY, locZ,
                cx + kTrackR * radX - outwardX * proud,
                kRingY + kTrackR * radY,
                cz + kTrackR * radZ - outwardZ * proud,
                kChevAmber, /*emStrength=*/kTrackEmIdle);
            m_portalMeshes.push_back(ae.mesh);
        }
        p.trackEntCount = kTrackSegs;

        // ---- Pool center: soft ROUND electric-blue hot spot ------------------
        // Two small round glow disks (the SHARED membrane disk mesh, uniformly
        // basis-scaled down) floating just hub-side of the plasma sheet. Round
        // + capped: the v1 white SQUARE hot-spot read as a literal square on
        // screen (Tim's screenshot) — a disk melts into the storm instead.
        auto addCoreDisk = [&](float scale, float depth, const float tint[3],
                               float em) -> uint32_t {
            Entity e;
            e.mesh = m_diskMesh;
            e.baseColor[0] = tint[0]; e.baseColor[1] = tint[1]; e.baseColor[2] = tint[2];
            e.baseColor[3] = 1.0f;
            e.emissive[0] = tint[0]; e.emissive[1] = tint[1]; e.emissive[2] = tint[2];
            e.emissive[3] = em;
            e.tag = (uint32_t)Tag::Prop;
            const float sx[3] = { rightX * scale, 0.0f, rightZ * scale };
            const float sy[3] = { 0.0f, scale, 0.0f };
            const float sz[3] = { outwardX * scale, 0.0f, outwardZ * scale };
            makeXform(e.transform, sx, sy, sz,
                      cx - outwardX * depth, kRingY, cz - outwardZ * depth);
            return scene.add(e);
        };
        (void)kCoreHalfW; (void)kCoreHalfH; (void)kCoreHalfT;
        (void)kCoreInnerHalfW; (void)kCoreInnerHalfH; (void)kCoreInnerHalfT;
        p.coreEnt      = addCoreDisk(0.14f, 0.045f, kCoreBlue,      kCoreBlueMinEm);
        p.coreInnerEnt = addCoreDisk(0.07f, 0.060f, kCoreInnerBlue, kCoreBlueMaxEm);

        // ---- Event-horizon membrane v2 (the DEEP-BLUE PLASMA STORM) -----------
        // Three entities on SHARED meshes/textures, authored contiguously
        // (vista, plasma, rim — the span tick() animates). The vista disk sits
        // a step OUTWARD (glimpsed through the storm, parallax as the player
        // strafes); the plasma disk carries the filament emissive texture on
        // the PBR route; the rim torus hugs the ring's inner edge.
        p.rightX = rightX; p.rightZ = rightZ;
        p.outX   = outwardX; p.outZ = outwardZ;
        const uint32_t membraneEntFirst = scene.size();
        const float locX[3] = { rightX, 0.0f, rightZ };
        const float locY[3] = { 0.0f,   1.0f, 0.0f    };
        const float locZ[3] = { outwardX, 0.0f, outwardZ };
        // [0] VISTA disk (dim backdrop). PBR route (mrTex) so the emissive is
        //     TEXTURE-GATED — dark starscape texels stay dark instead of a flat
        //     pale wash (the emissive-path draw has no emissive map). Low
        //     albedo so the core point light doesn't wash it out either.
        {
            Entity e;
            e.mesh = m_diskMesh;
            e.tex  = m_vistaTex;
            e.mrTex = m_mrFlat;
            e.emissiveTex = m_vistaTex;
            e.baseColor[0] = 0.20f; e.baseColor[1] = 0.24f; e.baseColor[2] = 0.30f;
            e.baseColor[3] = 1.0f;
            e.emissive[0] = kVistaTint[0]; e.emissive[1] = kVistaTint[1];
            e.emissive[2] = kVistaTint[2]; e.emissive[3] = kVistaEmBase;
            e.tag = (uint32_t)Tag::Prop;
            makeXform(e.transform, locX, locY, locZ,
                      cx + outwardX * kVistaDepth, kRingY, cz + outwardZ * kVistaDepth);
            scene.add(e);
        }
        // [1] PLASMA disk (filament emissive map; mrTex forces the PBR route so
        //     the emissive texture is honoured; deep-blue tint, capped strength).
        {
            Entity e;
            e.mesh = m_diskMesh;
            e.tex  = m_plasmaTex;                 // albedo: the same web, dim under no light
            e.mrTex = m_mrFlat;
            e.emissiveTex = m_plasmaTex;
            e.baseColor[0] = 0.10f; e.baseColor[1] = 0.14f; e.baseColor[2] = 0.30f;
            e.baseColor[3] = 1.0f;
            e.emissive[0] = kPlasmaBlue[0]; e.emissive[1] = kPlasmaBlue[1];
            e.emissive[2] = kPlasmaBlue[2]; e.emissive[3] = kPlasmaEmBase;
            e.tag = (uint32_t)Tag::Prop;
            makeXform(e.transform, locX, locY, locZ, cx, kRingY, cz);
            scene.add(e);
        }
        // [2] FRESNEL RIM ring (bright blue inner-edge glow, shimmer in tick()).
        {
            Entity e;
            e.mesh = m_rimMesh;
            e.baseColor[0] = 0.10f; e.baseColor[1] = 0.18f; e.baseColor[2] = 0.35f;
            e.baseColor[3] = 1.0f;
            e.emissive[0] = kRimBlue[0]; e.emissive[1] = kRimBlue[1];
            e.emissive[2] = kRimBlue[2]; e.emissive[3] = kRimEmBase;
            e.tag = (uint32_t)Tag::Prop;
            makeXform(e.transform, locX, locY, locZ, cx, kRingY, cz);
            scene.add(e);
        }
        p.membraneEntFirst = membraneEntFirst;
        p.membraneEntCount = 3;
        p.vistaEm  = kVistaEmBase;   // IDLE state: vista faintly visible
        p.throatOn = false;

        m_portals.push_back(p);

        // Trigger volume: wider than the ring so the player only needs to
        // step into the plate area (not thread the ring) to fire the rift.
        const x3::phys::Vec3 tmin{ cx - kTrigHalfXZ, -kTrigHalfY, cz - kTrigHalfXZ };
        const x3::phys::Vec3 tmax{ cx + kTrigHalfXZ,  kTrigHalfY, cz + kTrigHalfXZ };
        triggers.add(tmin, tmax, sp.triggerId, /*enabled=*/true);
    }

    // ===== Per-portal blue CORE lights (cast the event horizon onto the stone) =====
    m_lights.clear();
    m_lights.reserve(m_portals.size() + 5);
    for (const auto& p : m_portals) {
        x3::rhi::PointLight L;
        L.pos[0] = p.worldPos.x; L.pos[1] = kRingY; L.pos[2] = p.worldPos.z;
        L.range  = kCoreLightRange;
        L.color[0] = kCoreLightBlue[0] * kCoreLightBase;
        L.color[1] = kCoreLightBlue[1] * kCoreLightBase;
        L.color[2] = kCoreLightBlue[2] * kCoreLightBase;
        m_lights.push_back(L);
    }
    // HALL fill lights (STATIC — appended after the animated gate lights so
    // tick()'s per-portal indexing is untouched): four cool overheads under
    // the strip-light ring + one center. These raise the hall's average
    // luminance so auto-exposure settles instead of pinning its 2.2x ceiling
    // (the v1/v2 pale-wash root cause).
    {
        const float pos[5][2] = { {10,10}, {-10,10}, {10,-10}, {-10,-10}, {0,0} };
        for (int l = 0; l < 5; ++l) {
            x3::rhi::PointLight L;
            L.pos[0] = pos[l][0]; L.pos[1] = kBeamY - 0.6f; L.pos[2] = pos[l][1];
            L.range  = kHallLightRange;
            L.color[0] = kHallLightColor[0] * kHallLightI;
            L.color[1] = kHallLightColor[1] * kHallLightI;
            L.color[2] = kHallLightColor[2] * kHallLightI;
            m_lights.push_back(L);
        }
    }

    physics.optimizeBroadphase();
    m_built = true;
    x3::logInfo("[rifthub] hub built with " + std::to_string(m_portals.size()) +
                " Stargate-style portals (smooth procedural TORUS ring " +
                std::to_string(kRingMajorSeg) + "x" + std::to_string(kRingMinorSeg) +
                " + " + std::to_string(kChevronCount) +
                " amber chevrons + octagonal plate + PLASMA-STORM membrane v2: "
                "vista + filament disk + fresnel rim, capped emissive)");
}

void Rifthub::tick(float dt, Scene& scene) {
    if (!m_built) return;
    m_time += dt;

    const float twoPi = 6.2831853f;
    const float coreOmega  = twoPi * kCoreFreqHz;      // fast blue energy pulse
    const float chevOmega  = twoPi * kChevFlickerHz;   // slow amber chevron flicker
    auto& ents = scene.entities();
    const uint32_t sceneN = (uint32_t)ents.size();
    auto capped = [](float v, float cap) { return v > cap ? cap : v; };

    for (uint32_t i = 0; i < m_portals.size(); ++i) {
        RiftPortal& p = m_portals[i];
        const float phase = (float)i * kShimmerPhaseStep;

        // --- Kawoosh one-shot: decay the activation-surge timer + shape the
        //     surge envelope (fast attack, exponential decay). surge01 in [0,1]
        //     drives BOTH the added strength and the tint slide toward pale
        //     blue — bright BLUE-white at the peak, never flat white.
        float kawooshEm = 0.0f, surge01 = 0.0f;
        if (p.kawoosh > 0.0f) {
            p.kawoosh -= dt;
            if (p.kawoosh < 0.0f) p.kawoosh = 0.0f;
            const float tSince = kKawooshDur - p.kawoosh;          // seconds since the flash
            kawooshEm = kKawooshPeakEm * std::exp(-kKawooshDecay * tSince);
            surge01   = kawooshEm / kKawooshPeakEm;
        }
        // Membrane/gate state (PortalAnimated.mp4 arc), derived from the
        // existing gameplay latches: IDLE / SURGE (kawoosh) / OPEN (settled).
        const bool surging = p.kawoosh > 0.0f;
        const bool open    = p.activated && !surging;

        // NOTE: the ring itself is NOT animated (metal doesn't pulse).

        // --- Blue core light: slow hum-synced breathe onto the gate metal ---
        if (i < m_lights.size()) {
            const float lS  = std::sin(m_time * (twoPi * kCoreLightFreqHz) + phase);
            const float l01 = 0.5f * (lS + 1.0f);
            float lI  = kCoreLightMin + (kCoreLightMax - kCoreLightMin) * l01;
            lI += surge01 * 1.6f;   // the kawoosh also floods the bay with light
            m_lights[i].color[0] = kCoreLightBlue[0] * lI;
            m_lights[i].color[1] = kCoreLightBlue[1] * lI;
            m_lights[i].color[2] = kCoreLightBlue[2] * lI;
        }

        // --- Amber chevron SLIT cores: slow per-chevron flicker (powered gate);
        //     the surge lifts every slit toward the CAP (locks slamming shut).
        for (uint32_t c = 0; c < p.chevronEntCount; ++c) {
            const uint32_t e = p.chevronEntFirst + c;
            if (e >= sceneN) break;
            const float chevPhase = phase + (float)c * kChevPhaseStep;
            const float s01 = 0.5f * (std::sin(m_time * chevOmega + chevPhase) + 1.0f);
            ents[e].emissive[3] = capped(kChevMinEm + (kChevMaxEm - kChevMinEm) * s01
                                         + surge01 * 0.45f, kChevEmCap);
        }

        // --- Amber RATCHET TRACK: dim when dormant; a bright CHASE sweeps the
        //     circumference during the surge; steady powered glow once OPEN.
        for (uint32_t t3 = 0; t3 < p.trackEntCount; ++t3) {
            const uint32_t e = p.trackEntFirst + t3;
            if (e >= sceneN) break;
            const float segAng = (float)t3 * (twoPi / (float)kTrackSegs);
            float em = open ? kTrackEmOpen : kTrackEmIdle;
            if (open) em += 0.12f * std::sin(m_time * 2.0f + segAng * 3.0f + phase);
            if (surging) {
                // Chase crest orbiting the track; sharpened cos^8 lobe.
                float cph = std::cos(segAng - m_time * kTrackChaseRadS + phase);
                if (cph < 0.0f) cph = 0.0f;
                cph = cph * cph; cph = cph * cph; cph = cph * cph;   // ^8
                em = kTrackEmIdle + surge01 * (0.55f + kTrackChase * cph);
            }
            ents[e].emissive[3] = capped(em, kTrackEmCap);
        }

        // --- ORANGE conduit flow: the emissive pulse travels ALONG the run
        //     (segment index = phase step) so power visibly streams toward the
        //     gate; the surge doubles the throb. Warm accent, capped.
        for (uint32_t t3 = 0; t3 < p.conduitEntCount; ++t3) {
            const uint32_t ce = p.conduitEntFirst + t3;
            if (ce >= sceneN) break;
            const float flow = std::sin(m_time * twoPi * kConduitFlowHz
                                        - (float)(t3 % 4u) * kConduitFlowK + phase);
            float em = kConduitEmBase + kConduitFlowAmp * (0.5f * (flow + 1.0f));
            em += surge01 * 0.35f;
            ents[ce].emissive[3] = capped(em, kConduitEmCap);
        }

        // --- Energy core: faster electric-blue pulse (core + brighter inner),
        //     both CLAMPED to kCoreEmCap so the hot center stays blue.
        const float coreS   = std::sin(m_time * coreOmega + phase);
        const float coreT01 = 0.5f * (coreS + 1.0f);
        const float coreEm  = kCoreBlueMinEm + (kCoreBlueMaxEm - kCoreBlueMinEm) * coreT01
                            + kawooshEm * 0.45f;
        if (p.coreEnt < sceneN)
            ents[p.coreEnt].emissive[3] = capped(coreEm, kCoreEmCap);
        if (p.coreInnerEnt < sceneN)
            ents[p.coreInnerEnt].emissive[3] = capped(coreEm * 1.10f + 0.25f, kCoreEmCap);

        // --- MEMBRANE STATE MACHINE (PortalAnimated.mp4 animation arc) -------
        // Texture swap into the THROAT happens the moment the portal activates
        // — the kawoosh flash is at full brightness on that exact frame, so
        // the swap hides inside it.
        if (p.activated && !p.throatOn && p.membraneEntFirst + 1 < sceneN) {
            Entity& e = ents[p.membraneEntFirst + 1];
            e.tex = m_throatTex;
            e.emissiveTex = m_throatTex;
            p.throatOn = true;
        }
        // Vista dissolve: IDLE holds base, OPEN fades toward "pure energy".
        {
            const float target = p.activated ? kVistaEmOpen : kVistaEmBase;
            const float step = kVistaFadePerSec * dt;
            if (p.vistaEm > target) { p.vistaEm -= step; if (p.vistaEm < target) p.vistaEm = target; }
            else                    { p.vistaEm += step; if (p.vistaEm > target) p.vistaEm = target; }
        }

        // --- PLASMA-STORM membrane: breathe + rotate, every write capped -----
        const float rightv[3] = { p.rightX, 0.0f, p.rightZ };
        const float upv[3]    = { 0.0f, 1.0f, 0.0f };
        const float outv[3]   = { p.outX, 0.0f, p.outZ };
        const float cx = p.worldPos.x, cz = p.worldPos.z;
        // [0] vista: slow counter-rotation, faint breathe (parallax backdrop),
        //     dissolving on OPEN.
        if (p.membraneEntFirst + 0 < sceneN) {
            Entity& e = ents[p.membraneEntFirst + 0];
            const float a = m_time * kVistaSpinRadS + phase;
            const float ca = std::cos(a), sa = std::sin(a);
            const float rx[3] = {  ca * rightv[0] + sa * upv[0],
                                   ca * rightv[1] + sa * upv[1],
                                   ca * rightv[2] + sa * upv[2] };
            const float ry[3] = { -sa * rightv[0] + ca * upv[0],
                                  -sa * rightv[1] + ca * upv[1],
                                  -sa * rightv[2] + ca * upv[2] };
            makeXform(e.transform, rx, ry, outv,
                      cx + p.outX * kVistaDepth, kRingY, cz + p.outZ * kVistaDepth);
            const float breathe = 0.06f * std::sin(m_time * 0.7f + phase);
            e.emissive[3] = capped(std::max(0.0f, p.vistaEm + breathe), kVistaEmCap);
        }
        // [1] plasma: IDLE = slow filament churn; OPEN = the throat streaming
        //     (faster spin + hotter base, texture already swapped); SURGE rides
        //     the kawoosh envelope on top. Organic two-sine breathe; the surge
        //     tint slides deep blue -> pale blue (NOT white); always capped.
        if (p.membraneEntFirst + 1 < sceneN) {
            Entity& e = ents[p.membraneEntFirst + 1];
            const float dir = (i & 1u) ? -1.0f : 1.0f;   // alternate spin direction
            const float spin = kPlasmaSpinRadS * (open ? kPlasmaSpinOpenX : 1.0f);
            // Continuous angle across the state change: integrate instead of
            // evaluating a*t (a rate jump would snap the disk).
            p.spinAngle += spin * dir * dt;
            const float a = p.spinAngle + phase;
            const float ca = std::cos(a), sa = std::sin(a);
            const float rx[3] = {  ca * rightv[0] + sa * upv[0],
                                   ca * rightv[1] + sa * upv[1],
                                   ca * rightv[2] + sa * upv[2] };
            const float ry[3] = { -sa * rightv[0] + ca * upv[0],
                                  -sa * rightv[1] + ca * upv[1],
                                  -sa * rightv[2] + ca * upv[2] };
            makeXform(e.transform, rx, ry, outv, cx, kRingY, cz);
            const float wob = kPlasmaEmWobble *
                (0.62f * std::sin(m_time * 1.15f * twoPi * 0.31f + phase) +
                 0.38f * std::sin(m_time * 1.15f * twoPi * 0.53f + phase * 2.1f));
            const float base = open || surging ? kPlasmaEmBaseOpen : kPlasmaEmBase;
            e.emissive[3] = capped(base + wob + kawooshEm, kPlasmaEmCap);
            // Surge tint: deep blue -> pale blue (NOT white) with the envelope.
            for (int c3 = 0; c3 < 3; ++c3)
                e.emissive[c3] = kPlasmaBlue[c3] +
                                 (kKawooshTint[c3] - kPlasmaBlue[c3]) * surge01;
        }
        // [2] fresnel rim: slow shimmer + kawoosh lift + a touch hotter when
        //     OPEN (the throat's grazing edge), capped.
        if (p.membraneEntFirst + 2 < sceneN) {
            Entity& e = ents[p.membraneEntFirst + 2];
            const float shim = 0.5f * (std::sin(m_time * twoPi * kRimShimmerHz + phase) + 1.0f);
            const float lift = open ? 0.25f : 0.0f;
            e.emissive[3] = capped(kRimEmBase + lift + kRimShimmerAmp * shim + kawooshEm * 0.55f,
                                   kRimEmCap);
        }

        // --- Lightning-arc spawner. IDLE: sparse cross-disk chords. SURGE:
        //     pool forced FULL of rim-orbit whips (the VORTEX RING). OPEN:
        //     center->rim radial streamers at a moderate rate.
        p.arcCooldown -= dt;
        for (uint32_t a = 0; a < RiftPortal::kMaxArcs; ++a)
            if (p.arcs[a].life > 0.0f) p.arcs[a].life -= dt;
        uint32_t live = 0;
        for (uint32_t a = 0; a < RiftPortal::kMaxArcs; ++a)
            if (p.arcs[a].life > 0.0f) ++live;
        const int arcMode = surging ? 1 : (open ? 2 : 0);
        if (live < RiftPortal::kMaxArcs &&
            (surging || p.arcCooldown <= 0.0f)) {
            spawnArc(p, arcMode);
            const float cdScale = open ? 0.55f : 1.0f;   // throat streams more often
            p.arcCooldown = surging ? 0.02f
                          : (kArcCooldownMin + (kArcCooldownMax - kArcCooldownMin) * frand()) * cdScale;
        }

        // --- Spark motes: steady bleed off the membrane (kawoosh bursts are
        //     spawned in onTrigger). The OPEN throat sheds harder.
        p.moteAccum += dt * kMoteRatePerSec * (open ? 1.7f : 1.0f);
        while (p.moteAccum >= 1.0f) {
            p.moteAccum -= 1.0f;
            const float th = frand() * twoPi;
            const float rr = (0.25f + 0.70f * frand()) * kMembraneR;
            Mote mo;
            mo.px = cx + rr * std::cos(th) * p.rightX;
            mo.py = kRingY + rr * std::sin(th);
            mo.pz = cz + rr * std::cos(th) * p.rightZ;
            // Drift gently hub-side off the surface + a slow rise.
            mo.vx = -p.outX * (0.12f + 0.22f * frand()) + frandSym() * 0.08f;
            mo.vy = 0.06f + 0.16f * frand();
            mo.vz = -p.outZ * (0.12f + 0.22f * frand()) + frandSym() * 0.08f;
            mo.maxLife = mo.life = kMoteLifeMin + (kMoteLifeMax - kMoteLifeMin) * frand();
            mo.size = 0.012f + 0.024f * frand();
            mo.r = 0.45f + 0.25f * frand(); mo.g = 0.65f + 0.20f * frand(); mo.b = 1.0f;
            spawnMote(mo);
        }
    }

    // --- Integrate the mote pool (whole hub, one pass) ---
    for (int m = 0; m < kMaxMotes; ++m) {
        Mote& mo = m_motes[m];
        if (mo.life <= 0.0f) continue;
        mo.life -= dt;
        mo.px += mo.vx * dt; mo.py += mo.vy * dt; mo.pz += mo.vz * dt;
        mo.vx -= mo.vx * 0.6f * dt; mo.vz -= mo.vz * 0.6f * dt;   // gentle drag
    }
}

// ---- FX helpers -----------------------------------------------------------------
float Rifthub::frand() {
    m_rng ^= m_rng << 13; m_rng ^= m_rng >> 17; m_rng ^= m_rng << 5;
    return (float)(m_rng & 0xFFFFFFu) / 16777216.0f;
}
float Rifthub::frandSym() { return frand() * 2.0f - 1.0f; }

void Rifthub::spawnMote(const Mote& m) {
    // Free slot if any, else recycle round-robin (bounded pool).
    for (int k = 0; k < kMaxMotes; ++k) {
        const int idx = (m_nextMote + k) % kMaxMotes;
        if (m_motes[idx].life <= 0.0f) {
            m_motes[idx] = m;
            m_nextMote = (idx + 1) % kMaxMotes;
            return;
        }
    }
    m_motes[m_nextMote] = m;
    m_nextMote = (m_nextMote + 1) % kMaxMotes;
}

void Rifthub::spawnArc(RiftPortal& p, int mode) {
    for (uint32_t a = 0; a < RiftPortal::kMaxArcs; ++a) {
        if (p.arcs[a].life > 0.0f) continue;
        RiftPortal::MembraneArc& arc = p.arcs[a];
        const float twoPi = 6.2831853f;
        arc.mode = (uint8_t)mode;
        if (mode == 1) {
            // SURGE rim-orbit (the VORTEX RING): a long circumferential whip
            // hugging the rim — both endpoints near the edge, a big sweep.
            arc.a0 = frand() * twoPi;
            arc.a1 = arc.a0 + (2.6f + 2.4f * frand()) * (frand() > 0.5f ? 1.0f : -1.0f);
            arc.r0 = (0.82f + 0.13f * frand()) * kMembraneR;
            arc.r1 = (0.82f + 0.13f * frand()) * kMembraneR;
            arc.maxLife = arc.life =
                (kArcLifeMin + (kArcLifeMax - kArcLifeMin) * frand()) * 0.6f;  // frantic
        } else if (mode == 2) {
            // OPEN radial streamer: center -> rim (looking down the throat).
            arc.a0 = frand() * twoPi;
            arc.a1 = arc.a0 + frandSym() * 0.5f;   // nearly radial, slight shear
            arc.r0 = 0.08f * kMembraneR;
            arc.r1 = (0.80f + 0.15f * frand()) * kMembraneR;
            arc.maxLife = arc.life = kArcLifeMin + (kArcLifeMax - kArcLifeMin) * frand();
        } else {
            // IDLE chord across the disk (sparse wandering tendril).
            arc.a0 = frand() * twoPi;
            arc.r0 = (0.15f + 0.75f * frand()) * kMembraneR;
            arc.a1 = arc.a0 + 2.0f + 2.2f * frand();
            arc.r1 = (0.25f + 0.70f * frand()) * kMembraneR;
            arc.maxLife = arc.life = kArcLifeMin + (kArcLifeMax - kArcLifeMin) * frand();
        }
        arc.seed = m_rng ^ (a * 0x9E3779B9u) ^ 0xA5A5A5A5u;
        arc.fork = mode != 1 && frand() > 0.35f;   // orbits stay clean rings
        return;
    }
}

uint32_t Rifthub::liveArcCount(uint32_t portalIdx) const {
    if (portalIdx >= m_portals.size()) return 0;
    uint32_t n = 0;
    for (uint32_t a = 0; a < RiftPortal::kMaxArcs; ++a)
        if (m_portals[portalIdx].arcs[a].life > 0.0f) ++n;
    return n;
}

uint32_t Rifthub::liveMoteCount() const {
    uint32_t n = 0;
    for (int m = 0; m < kMaxMotes; ++m)
        if (m_motes[m].life > 0.0f) ++n;
    return n;
}

void Rifthub::drawFx(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) {
    if (!m_built || !m_fxBeamMesh.valid()) return;

    // ---- Lightning arcs: jagged forked white-blue tendrils on the membrane.
    // Endpoints live in disk-polar coords; interior vertices are jittered in
    // the disk plane EVERY FRAME (a per-frame remix of the arc seed) so the
    // tendril crackles like the lightning gun's bolt. Each segment is the
    // shared unit box stretched a->b (beamXform); drawn with drawMeshEmissive
    // at a CAPPED white-blue emissive so even the tendrils never clip white.
    for (uint32_t i = 0; i < m_portals.size(); ++i) {
        const RiftPortal& p = m_portals[i];
        const float cx = p.worldPos.x, cz = p.worldPos.z;
        // Point on the membrane plane (polar), lifted a hair HUB-side.
        auto diskPt = [&](float ang, float rad) {
            const float c = std::cos(ang), s = std::sin(ang);
            return x3::phys::Vec3{
                cx + rad * c * p.rightX - p.outX * kArcLift,
                kRingY + rad * s,
                cz + rad * c * p.rightZ - p.outZ * kArcLift };
        };
        for (uint32_t a = 0; a < RiftPortal::kMaxArcs; ++a) {
            const RiftPortal::MembraneArc& arc = p.arcs[a];
            if (arc.life <= 0.0f) continue;
            const float k = (arc.maxLife > 0.0f) ? arc.life / arc.maxLife : 0.0f;
            // Fast attack, linear fade: pow-shaped so the strike POPS then dies.
            const float env = (k > 0.85f) ? (1.0f - k) / 0.15f : k / 0.85f;
            // Per-frame jitter rng: remix the arc seed with a frame-quantized time.
            uint32_t rng = arc.seed ^ (uint32_t)(m_time * 60.0f) * 0x27D4EB2Du;
            auto jr = [&]() {
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                return (float)(rng & 0xFFFFFFu) / 16777216.0f * 2.0f - 1.0f;
            };
            const float em[4] = { kArcEmColor[0], kArcEmColor[1], kArcEmColor[2],
                                  kArcEmPeak * (0.35f + 0.65f * env) };
            const x3::phys::Vec3 A = diskPt(arc.a0, arc.r0);
            x3::phys::Vec3 prev = A;
            x3::phys::Vec3 forkFrom{};
            bool haveFork = false;
            // Rim-orbit whips (SURGE vortex ring) sweep a long angle: more
            // segments + tighter jitter so the orbit reads as a circling ring;
            // idle chords / open radials keep the coarse crackling look.
            const uint32_t segs = (arc.mode == 1) ? 12u : kArcSegs;
            const float jamp = (arc.mode == 1) ? 0.07f : 0.16f;
            const float thick = kArcThickness * (0.7f + 0.3f * env)
                              * ((arc.mode == 1) ? 1.35f : 1.0f);
            for (uint32_t s3 = 1; s3 <= segs; ++s3) {
                const float t = (float)s3 / (float)segs;
                // Interpolate in POLAR space so the tendril hugs the disk.
                const float ang = arc.a0 + (arc.a1 - arc.a0) * t;
                const float rad = arc.r0 + (arc.r1 - arc.r0) * t;
                x3::phys::Vec3 pt = diskPt(ang, rad);
                if (s3 < segs) {
                    // Jitter in the disk plane (right/up), clamped inside the rim.
                    const float j1 = jr() * jamp, j2 = jr() * jamp;
                    pt.x += p.rightX * j1;
                    pt.y += j2;
                    pt.z += p.rightZ * j1;
                    if (s3 == segs / 2 && arc.fork) { forkFrom = pt; haveFork = true; }
                }
                float model[16];
                beamXform(model, prev, pt, thick);
                device.drawMeshEmissive(frame, m_fxBeamMesh, x3::rhi::TextureHandle{},
                                        kArcColor, em, model);
                prev = pt;
            }
            // Short fork branch off the midpoint (the forked-bolt read).
            if (haveFork) {
                x3::phys::Vec3 fp = forkFrom;
                const float fa = arc.a0 + (arc.a1 - arc.a0) * 0.5f + (jr() > 0 ? 1.1f : -1.1f);
                const float fr = std::min(kMembraneR * 0.9f,
                                          (arc.r0 + arc.r1) * 0.5f + 0.35f);
                const x3::phys::Vec3 fEnd = diskPt(fa, fr);
                for (int fs = 1; fs <= 3; ++fs) {
                    const float t = (float)fs / 3.0f;
                    x3::phys::Vec3 pt{ forkFrom.x + (fEnd.x - forkFrom.x) * t,
                                       forkFrom.y + (fEnd.y - forkFrom.y) * t,
                                       forkFrom.z + (fEnd.z - forkFrom.z) * t };
                    if (fs < 3) {
                        const float j1 = jr() * 0.10f, j2 = jr() * 0.10f;
                        pt.x += p.rightX * j1; pt.y += j2; pt.z += p.rightZ * j1;
                    }
                    float model[16];
                    beamXform(model, fp, pt, kArcThickness * 0.55f);
                    device.drawMeshEmissive(frame, m_fxBeamMesh, x3::rhi::TextureHandle{},
                                            kArcColor, em, model);
                    fp = pt;
                }
            }
        }
    }

    // ---- Spark motes: one additive billboard batch for the whole hub.
    uint32_t n = 0;
    for (int m = 0; m < kMaxMotes; ++m) {
        const Mote& mo = m_motes[m];
        if (mo.life <= 0.0f) continue;
        const float k = (mo.maxLife > 0.0f) ? mo.life / mo.maxLife : 0.0f;
        x3::rhi::IRenderDevice::ParticleInstance& inst = m_moteScratch[n++];
        inst.pos[0] = mo.px; inst.pos[1] = mo.py; inst.pos[2] = mo.pz;
        inst.size = mo.size * (0.6f + 0.4f * k);
        inst.color[0] = mo.r * 1.6f; inst.color[1] = mo.g * 1.6f;
        inst.color[2] = mo.b * 1.6f; inst.color[3] = k;
    }
    if (n) device.submitParticles(m_moteScratch, n,
                                  x3::rhi::IRenderDevice::ParticleBlend::Additive);
}

void Rifthub::applyAtmosphere(x3::rhi::IRenderDevice& device) const {
    // Industrial hall haze: enough that the far wall's machinery reads as
    // silhouettes and the gate light shafts get body, never a milky wash.
    x3::rhi::IRenderDevice::FogParams fog;
    fog.enabled  = true;
    fog.color[0] = 0.020f; fog.color[1] = 0.030f; fog.color[2] = 0.050f;   // cold blue haze
    fog.density  = 0.024f;
    fog.start    = 2.5f;
    fog.maxOpacity = 0.80f;
    device.setFog(fog);
    // Teal-shadow / warm-highlight grade (the locked palette: blue key,
    // orange accents) + a light vignette.
    x3::rhi::IRenderDevice::GradeParams g;
    g.strength = 0.35f;
    g.shadowTint[0] = 0.90f; g.shadowTint[1] = 1.02f; g.shadowTint[2] = 1.05f;
    g.highlightTint[0] = 1.05f; g.highlightTint[1] = 1.00f; g.highlightTint[2] = 0.96f;
    g.saturation = 1.06f;
    g.vignette   = 0.08f;
    device.setGrade(g);
    // Cool low ambient (the hall is DARK; the gates + strips carry it), an
    // interior IBL probe so the wet floor / gate metal reflect the hall not
    // an open sky, and a NEGATIVE exposure bias so auto-exposure (which pins
    // its 2.2x ceiling in a dark scene) can't wash the metal pale.
    device.setAmbient(0.050f, 0.058f, 0.078f);
    device.setIblProbe(true);
    device.setExposure(0.95f);
}

void Rifthub::shutdown(x3::rhi::IRenderDevice& device) {
    if (!m_built) return;
    if (m_groundMesh.valid()) device.destroyMesh(m_groundMesh);
    if (m_groundTex.valid())  device.destroyTexture(m_groundTex);
    for (auto h : m_portalMeshes) if (h.valid()) device.destroyMesh(h);
    m_portalMeshes.clear();
    // Shared membrane v2 resources: referenced by many entities, freed ONCE.
    if (m_diskMesh.valid())   { device.destroyMesh(m_diskMesh);      m_diskMesh   = {}; }
    if (m_rimMesh.valid())    { device.destroyMesh(m_rimMesh);       m_rimMesh    = {}; }
    if (m_fxBeamMesh.valid()) { device.destroyMesh(m_fxBeamMesh);    m_fxBeamMesh = {}; }
    if (m_plasmaTex.valid())  { device.destroyTexture(m_plasmaTex);  m_plasmaTex  = {}; }
    if (m_throatTex.valid())  { device.destroyTexture(m_throatTex);  m_throatTex  = {}; }
    if (m_vistaTex.valid())   { device.destroyTexture(m_vistaTex);   m_vistaTex   = {}; }
    if (m_mrFlat.valid())     { device.destroyTexture(m_mrFlat);     m_mrFlat     = {}; }
    if (m_mrWet.valid())      { device.destroyTexture(m_mrWet);      m_mrWet      = {}; }
    if (m_holoTexA.valid())   { device.destroyTexture(m_holoTexA);   m_holoTexA   = {}; }
    if (m_holoTexB.valid())   { device.destroyTexture(m_holoTexB);   m_holoTexB   = {}; }
    m_surf.destroyAll(device);   // curated PBR sets (ring plates / housings / hall)
    for (int m = 0; m < kMaxMotes; ++m) m_motes[m].life = 0.0f;
    m_portals.clear();
    m_lights.clear();
    m_time = 0.0f;
    m_built = false;
}

void Rifthub::onTrigger(uint32_t triggerId) {
    for (auto& p : m_portals) {
        if (p.triggerId == triggerId) {
            if (!p.activated) {
                p.activated = true;
                p.kawoosh = kKawooshDur;   // fire the activation KAWOOSH surge
                // Spark BURST off the membrane (the splash-out) + force the
                // tendril pool full — the unstable-vortex moment.
                const float twoPi = 6.2831853f;
                for (int b = 0; b < kMoteBurst; ++b) {
                    const float th = frand() * twoPi;
                    const float rr = (0.15f + 0.80f * frand()) * kMembraneR;
                    Mote mo;
                    mo.px = p.worldPos.x + rr * std::cos(th) * p.rightX;
                    mo.py = kRingY + rr * std::sin(th);
                    mo.pz = p.worldPos.z + rr * std::cos(th) * p.rightZ;
                    mo.vx = -p.outX * (0.6f + 1.2f * frand()) + frandSym() * 0.5f;
                    mo.vy = frandSym() * 0.55f;
                    mo.vz = -p.outZ * (0.6f + 1.2f * frand()) + frandSym() * 0.5f;
                    mo.maxLife = mo.life = 0.5f + 1.1f * frand();
                    mo.size = 0.015f + 0.030f * frand();
                    mo.r = 0.55f + 0.25f * frand(); mo.g = 0.72f + 0.18f * frand(); mo.b = 1.0f;
                    spawnMote(mo);
                }
                for (uint32_t a = 0; a < RiftPortal::kMaxArcs; ++a)
                    if (p.arcs[a].life <= 0.0f) spawnArc(p, /*mode=*/1);   // vortex ring
                std::string name = p.worldName ? p.worldName : "?";
                x3::logInfo(std::string("[rifthub] entered ") + name +
                            " rift — relaunch with --world " + name +
                            " to traverse");
            }
            return;
        }
    }
}

bool Rifthub::hudPromptForEye(const x3::phys::Vec3& eye, std::string& outPrompt,
                              float hudRadiusM) const {
    if (!m_built) return false;
    // Find the CLOSEST portal within hudRadiusM (XZ distance — eye Y is the
    // player's head height, portal anchors sit on the ground).
    int   bestIdx  = -1;
    float bestD2   = hudRadiusM * hudRadiusM;
    for (uint32_t i = 0; i < m_portals.size(); ++i) {
        const auto& p = m_portals[i];
        const float dx = eye.x - p.worldPos.x;
        const float dz = eye.z - p.worldPos.z;
        const float d2 = dx*dx + dz*dz;
        if (d2 < bestD2) { bestD2 = d2; bestIdx = (int)i; }
    }
    if (bestIdx < 0) return false;
    const auto& p = m_portals[(uint32_t)bestIdx];
    if (p.activated) {
        outPrompt = std::string("Rift activated: ") + (p.worldName ? p.worldName : "?");
    } else {
        outPrompt = std::string("Rift: ") + (p.worldName ? p.worldName : "?") +
                    " — walk in to activate";
    }
    return true;
}

bool Rifthub::allActivated() const {
    for (const auto& p : m_portals) if (!p.activated) return false;
    return !m_portals.empty();
}

// ===========================================================================
// Headless self-test (--test-rifthub). Mirrors --test-act2caves' structure:
// build on a HeadlessDevice + Jolt world, drive triggers/tick, assert.
// ===========================================================================
namespace {

int rh_pass = 0, rh_fail = 0;
void rhCheck(bool cond, const char* name) {
    if (cond) { ++rh_pass; x3::logInfo(std::string("[rifthub-test] PASS ") + name); }
    else      { ++rh_fail; x3::logError(std::string("[rifthub-test] FAIL ") + name); }
}

// The --world targets the current host actually launches into (main.cpp's
// worldMode dispatch). Every portal worldName must be one of these so a portal
// always signposts a slice the player can really relaunch into.
bool isKnownWorldTarget(const char* w) {
    if (!w) return false;
    const std::string s(w);
    return s == "act2caves" || s == "act2" || s == "valley" || s == "cliffs" ||
           s == "club" || s == "destruct" || s == "ragdoll" || s == "terrain";
}

} // namespace

bool runRifthubSelfTest() {
    rh_pass = rh_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    phys->init();
    HeadlessRenderDevice device;
    Scene scene;
    TriggerSystem triggers;
    Rifthub hub;
    hub.build(scene, device, *phys, triggers);

    // T0 — built with exactly 8 portals (one per --world target).
    rhCheck(hub.built() && hub.portalCount() == kRifthubTrigCount &&
            hub.portalCount() == 8,
            "T0 hub built with 8 portals (one per --world target)");

    // T0b — trigger ids are exactly the fresh 200-207 range (no collision with
    //       Act-1 10/30/40/50, Act-2 host 80-82, caves 100-108), all 8 distinct.
    {
        bool ok = true;
        uint32_t seen = 0;   // bitmask of (id - kRifthubTrigBase)
        for (uint32_t i = 0; i < hub.portalCount(); ++i) {
            const uint32_t id = hub.portal(i).triggerId;
            if (id < kRifthubTrigBase || id >= kRifthubTrigBase + kRifthubTrigCount) ok = false;
            else {
                const uint32_t bit = 1u << (id - kRifthubTrigBase);
                if (seen & bit) ok = false;   // duplicate id
                seen |= bit;
            }
        }
        if (seen != ((1u << kRifthubTrigCount) - 1u)) ok = false;  // all 8 present
        rhCheck(ok, "T0b trigger ids are the distinct fresh 200-207 range");
    }

    // T1 — each portal owns a contiguous span of stone-ring + amber-chevron +
    //      event-horizon membrane entities + 2 core disks, and the spans index
    //      valid scene entities.
    {
        const uint32_t sceneN = scene.size();
        bool ok = sceneN > 0;
        for (uint32_t i = 0; i < hub.portalCount(); ++i) {
            const RiftPortal& p = hub.portal(i);
            if (p.ringEntCount == 0 || p.chevronEntCount == 0) ok = false;
            if (p.membraneEntCount == 0 || p.trackEntCount == 0) ok = false;
            if (p.ringEntFirst + p.ringEntCount > sceneN)      ok = false;
            if (p.coreEnt >= sceneN || p.coreInnerEnt >= sceneN) ok = false;
            if (p.chevronEntFirst + p.chevronEntCount > sceneN) ok = false;
            if (p.trackEntFirst + p.trackEntCount > sceneN)     ok = false;
            if (p.membraneEntFirst + p.membraneEntCount > sceneN) ok = false;
        }
        rhCheck(ok, "T1 every portal owns valid ring/chevron/core/membrane entity spans");
    }

    // T2 — all 8 portal names map to REAL --world targets the host launches.
    {
        bool ok = true;
        for (uint32_t i = 0; i < hub.portalCount(); ++i)
            if (!isKnownWorldTarget(hub.portal(i).worldName)) ok = false;
        rhCheck(ok, "T2 all 8 portal names are real --world targets");
    }

    // T3 — at load NOTHING is activated; allActivated() is false; the HUD prompt
    //      reads "walk in to activate" for a portal we stand next to.
    {
        bool noneActive = true;
        for (uint32_t i = 0; i < hub.portalCount(); ++i)
            if (hub.portal(i).activated) noneActive = false;
        std::string prompt;
        const RiftPortal& p0 = hub.portal(0);
        bool hud = hub.hudPromptForEye(p0.worldPos, prompt) &&
                   prompt.find("walk in") != std::string::npos;
        rhCheck(noneActive && !hub.allActivated() && hud,
                "T3 inert at load: no portal activated, HUD prompts 'walk in'");
    }

    // Capture the IDLE-state plasma texture id before any trigger fires (T8
    // asserts the OPEN state swapped it to the throat texture).
    const uint32_t idlePlasmaTexId =
        scene.entities()[hub.portal(0).membraneEntFirst + 1].emissiveTex.id;

    // T4 — entering each portal's trigger volume (a point inside it, via the
    //      shared TriggerSystem) latches THAT portal's `activated` flag + flips
    //      its HUD prompt to "Rift activated:"; only after ALL are entered does
    //      allActivated() become true.
    {
        bool perPortalOk = true;
        for (uint32_t i = 0; i < hub.portalCount(); ++i) {
            const RiftPortal& p = hub.portal(i);
            // Stand at the portal center (inside its 5 m square trigger volume).
            const x3::phys::Vec3 inside{ p.worldPos.x, 0.0f, p.worldPos.z };
            const auto fired = triggers.update(inside);
            bool sawId = false;
            for (uint32_t id : fired) { hub.onTrigger(id); if (id == p.triggerId) sawId = true; }
            std::string prompt;
            hub.hudPromptForEye(inside, prompt);
            if (!sawId || !hub.portal(i).activated ||
                prompt.find("Rift activated:") == std::string::npos)
                perPortalOk = false;
            // allActivated() must stay false until the very last portal.
            if (i + 1 < hub.portalCount() && hub.allActivated()) perPortalOk = false;
            // Move away so the next update() re-arms an enter edge for this id.
            triggers.update(x3::phys::Vec3{ 1000.0f, 1000.0f, 1000.0f });
        }
        rhCheck(perPortalOk && hub.allActivated(),
                "T4 each trigger latches its portal; allActivated() only after all 8");
    }

    // T5 — tick(dt) advances the animation: chevron + core + PLASMA membrane
    //      emissive values DIFFER between two ticks taken at different times
    //      (the flicker + pulse + storm-breathe are live), the RING stays
    //      STATIC across ticks (metal doesn't pulse), the chevron flicker
    //      stays in its declared band, AND the plasma disk's transform
    //      ROTATES between ticks (the storm churns — membrane v2).
    {
        const RiftPortal& p = hub.portal(0);
        // Settle the T4 kawoosh surges first (the surge pins the plasma at its
        // CAP — correct behaviour, but it would mask the steady-state breathe
        // this test samples). 3 s > kKawooshDur.
        for (int s = 0; s < 180; ++s) hub.tick(1.0f / 60.0f, scene);
        hub.tick(0.0f, scene);   // sample at a known phase
        const uint32_t plasmaEnt = p.membraneEntFirst + 1;
        const float ring0 = scene.entities()[p.ringEntFirst].emissive[3];
        const float chev0 = scene.entities()[p.chevronEntFirst].emissive[3];
        const float core0 = scene.entities()[p.coreEnt].emissive[3];
        const float mem0  = scene.entities()[plasmaEnt].emissive[3];
        const float rot0  = scene.entities()[plasmaEnt].transform[1];  // basis X.y (spins in-plane)
        // Advance ~a tenth of a second — enough for the pulse sines + the
        // kPlasmaSpinRadS rotation to move appreciably.
        hub.tick(0.1f, scene);
        const float ring1 = scene.entities()[p.ringEntFirst].emissive[3];
        const float chev1 = scene.entities()[p.chevronEntFirst].emissive[3];
        const float core1 = scene.entities()[p.coreEnt].emissive[3];
        const float mem1  = scene.entities()[plasmaEnt].emissive[3];
        const float rot1  = scene.entities()[plasmaEnt].transform[1];
        bool moved = std::fabs(chev1 - chev0) > 1e-3f &&
                     std::fabs(core1 - core0) > 1e-3f &&
                     std::fabs(mem1  - mem0)  > 1e-4f;
        // Ring is authored once + never animated: emissive must not change.
        bool ringStatic = std::fabs(ring1 - ring0) < 1e-6f;
        // Chevron flicker stays within its declared [min,max] band.
        bool bounded = chev1 >= kChevMinEm - 0.01f && chev1 <= kChevMaxEm + 0.01f;
        // The storm rotates: the plasma disk's basis actually turned.
        bool rotates = std::fabs(rot1 - rot0) > 1e-5f;
        rhCheck(moved && ringStatic && bounded && rotates,
                "T5 tick() advances chevron + core + plasma storm; ring static; disk rotates");
    }

    // T6 — EMISSIVE CAP LAW (the blown-white v1 fix, the heart of membrane
    //      v2): fire a fresh kawoosh on portal 1, then tick 2 s at 60 Hz and
    //      assert EVERY membrane-layer emissive stays at or under its cap the
    //      whole way through the surge — the surge peaks bright blue, never
    //      tonemap-clipping flat white. Also: the surge tint slides toward
    //      kKawooshTint but the BLUE channel stays dominant at every sample.
    {
        RiftPortal& p1 = const_cast<RiftPortal&>(hub.portal(1));
        p1.kawoosh = kKawooshDur;   // re-fire the surge (already activated in T4)
        bool underCap = true, blueDominant = true;
        for (int s = 0; s < 120; ++s) {
            hub.tick(1.0f / 60.0f, scene);
            const auto& ents = scene.entities();
            const float vista  = ents[p1.membraneEntFirst + 0].emissive[3];
            const float plasma = ents[p1.membraneEntFirst + 1].emissive[3];
            const float rim    = ents[p1.membraneEntFirst + 2].emissive[3];
            const float core   = ents[p1.coreEnt].emissive[3];
            const float inner  = ents[p1.coreInnerEnt].emissive[3];
            if (vista  > kVistaEmCap  + 1e-4f) underCap = false;
            if (plasma > kPlasmaEmCap + 1e-4f) underCap = false;
            if (rim    > kRimEmCap    + 1e-4f) underCap = false;
            if (core   > kCoreEmCap   + 1e-4f) underCap = false;
            if (inner  > kCoreEmCap   + 1e-4f) underCap = false;
            // Ring v2: chevron cores + ratchet-track chase obey their caps too.
            if (ents[p1.chevronEntFirst].emissive[3] > kChevEmCap + 1e-4f) underCap = false;
            for (uint32_t t3 = 0; t3 < p1.trackEntCount; ++t3)
                if (ents[p1.trackEntFirst + t3].emissive[3] > kTrackEmCap + 1e-4f)
                    underCap = false;
            // Phase D: the orange conduit flow obeys its cap under the surge.
            for (uint32_t t3 = 0; t3 < p1.conduitEntCount; ++t3)
                if (ents[p1.conduitEntFirst + t3].emissive[3] > kConduitEmCap + 1e-4f)
                    underCap = false;
            const Entity& pe = ents[p1.membraneEntFirst + 1];
            if (!(pe.emissive[2] > pe.emissive[0] && pe.emissive[2] > pe.emissive[1]))
                blueDominant = false;
        }
        rhCheck(underCap && blueDominant,
                "T6 emissive cap law: kawoosh surge stays <= caps, blue dominant");
    }

    // T7 — membrane FX are ALIVE: after the T6 ticking, the ticking portals
    //      have live lightning arcs (the spawner keeps tendrils crawling) and
    //      the hub has live spark motes (steady bleed + kawoosh bursts).
    {
        uint32_t arcs = 0;
        for (uint32_t i = 0; i < hub.portalCount(); ++i) arcs += hub.liveArcCount(i);
        rhCheck(arcs > 0 && hub.liveMoteCount() > 0,
                "T7 membrane FX alive: lightning arcs + spark motes exist");
    }

    // T8 — MEMBRANE STATE MACHINE (PortalAnimated.mp4 arc): after activation +
    //      surge decay the portal is OPEN — the plasma disk swapped from the
    //      idle nebula texture to the THROAT texture, and the vista has
    //      dissolved (faded well below its idle base) — while a hypothetical
    //      idle portal would still hold the nebula. (All portals were entered
    //      in T4, so all 8 must be in the OPEN state by now.)
    {
        bool swapped = true, dissolved = true;
        for (uint32_t i = 0; i < hub.portalCount(); ++i) {
            const RiftPortal& p = hub.portal(i);
            const Entity& plasma = scene.entities()[p.membraneEntFirst + 1];
            if (plasma.emissiveTex.id == idlePlasmaTexId) swapped = false;
            if (!p.throatOn) swapped = false;
            if (p.vistaEm > 0.15f) dissolved = false;   // faded toward kVistaEmOpen
        }
        rhCheck(swapped && dissolved,
                "T8 state machine: OPEN throat texture swapped in, vista dissolved");
    }

    hub.shutdown(device);
    phys->shutdown();

    x3::logInfo("rifthub: " + std::to_string(rh_pass) + "/" +
                std::to_string(rh_pass + rh_fail) + " passed");
    return rh_fail == 0;
}

} // namespace x3::game
