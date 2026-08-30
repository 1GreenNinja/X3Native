// EFLZ Portal Hub — see rifthub.h for the design overview.
//
// Clean-room: built ONLY from X3Native's own Scene / trigger / mesh_prims
// systems + the engine interfaces. No RBDOOM / id Tech / Doom / Quake — or any
// other game-engine — source was consulted. CONTENT/LEVEL-SCRIPT ONLY.
#include "rifthub.h"
#include "destinations.h"  // the ONE destination registry (every place the game has)
#include "rift_depths.h"   // W-RIFT: the approach corridor (T24/T25)
#include "elevator.h"      // W-RIFT: the RIFT stop + the 4790 lock (T26)
#include "asset_root.h"
#include "mesh_prims.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdio>
#include <cstring>
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
constexpr float kConcreteTint[3] = { 0.48f, 0.50f, 0.52f };  // dark venue concrete
constexpr float kFloorTint[3]    = { 0.46f, 0.48f, 0.51f };  // wet dark floor (round 2: must READ)
constexpr uint32_t kHallColumns  = 8;     // perimeter steel columns
constexpr float kColumnHalf      = 0.32f;
constexpr uint32_t kHallBeams    = 5;     // ceiling beam count per direction
constexpr float kBeamHalfW       = 0.22f;
constexpr float kBeamHalfH       = 0.28f;
constexpr float kBeamY           = 9.45f; // beam centerline height
constexpr uint32_t kStripCount   = 8;     // ceiling strip lights
constexpr float kStripTint[3]    = { 0.72f, 0.82f, 0.95f };  // cool white-blue
constexpr float kStripEm         = 2.30f; // capped — fixtures, not suns
constexpr uint32_t kCableCount   = 10;    // hanging catenary cables
constexpr uint32_t kCableSegs    = 9;
constexpr float kCableSag        = 1.15f;
// Hall fill lights (appended AFTER the 8 animated gate lights in m_lights).
// ROUND 2 ("it's pitch black behind the gates"): fills nearly doubled + wider
// so the shell/beams/columns/machinery silhouettes READ — moody, not void.
// ROUND 5 (owner: "we did NOT get any more light in the room" — the round-4
// +33% deck bump was invisible): the fills were fighting the INVERSE-SQUARE
// law and losing. A 7.5-intensity light 6.8 m above the deck delivers
// 7.5/6.8^2 ~= 0.16 to the floor, which a 0.47-albedo wet concrete turns into
// ~0.07 of diffuse — black. The rig is rebuilt, not nudged: 9 overheads at
// 2.4x the intensity, 8 deck fills at 2.9x, and a per-gate KEY light that
// finally carves the ring hardware (highlight top / shadow bottom, the
// reference's light). The membranes are STILL the key light of their bays —
// the grade, fog and palette are untouched.
//
// ===== ROUND 9 — THE HONEST-LIGHTING RE-TUNE (owner: "now it's TOO bright in the
// portal room"). READ THIS BEFORE TOUCHING A NUMBER BELOW. =====================
// Every intensity in this block was tuned UNDER A BROKEN RENDERER. mesh.frag carried
// two lighting paths, and the PBR route (any surface with an MR / normal map — which
// here means the GLB gates AND the wet-concrete deck, via m_mrWet) shaded at 1/PI of
// the plain-prim route beside it. Surfaces that should have matched came out ~3.1x too
// dark (worse still on metal), so rounds 2-8 kept pouring on lumens to compensate:
// overheads 7.5 -> 18, deck fills 3.2 -> 9.2, washes 2.2 -> 5.0, the gate key 26 -> 70,
// ambient roughly doubled, exposure bias pushed to 1.24.
//
// That shading bug is FIXED (shaders/mesh.frag; the PBR path now matches the dielectric
// convention). All of that compensation is now stacking on top of HONEST lighting, and
// the hall blew out: the concrete deck and walls render as pale washed-out grey — a
// well-lit warehouse, which is the exact opposite of this room.
//
// So the rig comes DOWN by the factor it was inflated by (~PI), and it comes down
// EVERYWHERE — the compensation was global, so the cure has to be. The target is the
// one in docs/RIFTHUB_ART_TARGET.md: a MOODY DARK INDUSTRIAL HALL. You can read the
// room — beams, columns, machinery silhouettes, the wet deck's reflections — but it is
// deliberately DARK, and THE MEMBRANES ARE THE KEY LIGHT OF THEIR BAYS. Blue membrane
// glow + orange conduit accents dominate; the concrete reads as DARK WET CONCRETE.
//
// AMBIENT gets cut hardest of all, on the lesson this file already learned the hard way
// (see applyAtmosphere): ambient is omnidirectional, so raising it lights a room BY
// FLATTENING IT. Contrast comes from the point rig and the membranes, never from here.
constexpr float kHallLightColor[3] = { 0.55f, 0.62f, 0.72f };  // cool industrial
constexpr float kHallLightI        = 5.80f;   // R9: was 18.0 (= 7.5-era value / the PI inflation)
constexpr float kHallLightRange    = 30.0f;
// Wall-wash accents at the perimeter machinery clusters (warm — they pick the
// silhouettes out of the dark without flattening the mood).
constexpr float kWashColor[3]      = { 0.70f, 0.58f, 0.42f };
constexpr float kWashI             = 1.60f;   // R9: was 5.00
constexpr float kWashRange         = 11.0f;
// Mid-floor DECK fills (between the gate bays — the "fantastic wet floor" the
// owner wants to actually SEE). These land straight on the PBR wet concrete, so they
// were the single biggest contributor to the white-floor blowout.
constexpr float kDeckColor[3]      = { 0.50f, 0.58f, 0.72f };
constexpr float kDeckI             = 2.80f;   // R9: was 9.20
constexpr float kDeckRange         = 14.0f;
constexpr uint32_t kDeckFills      = 8;       // was 4
// Per-gate KEY light (round 5, bug 4): a hard-ish cool-white key above and
// hub-side of each gate. This is what the reference stills have and we did not:
// a directional bite that puts a HIGHLIGHT on the ring's upper plates and drops
// its lower half into shadow, so the machined hardware reads as machined metal
// instead of flat cardboard.
constexpr float kGateKeyColor[3]   = { 0.86f, 0.90f, 1.00f };
// R9 (mirror fix): 70 -> 15 -> 5.0. BOTH earlier numbers were tuned against a tube
// that was MIRRORED, i.e. physically incapable of taking light — so they were fitting
// a light rig to a surface that could never respond, and the number was meaningless.
// With the shell un-mirrored the gate finally shades, and 15 blew it to a white
// marshmallow. 5.0 is what an honestly-lit machined tube actually wants.
constexpr float kGateKeyI          = 5.0f;    // R9: was 70. That 70 was, by its own R8
                                              // comment, "3x its honest value because GLB
                                              // meshes shaded at 1/PI of the prims beside
                                              // them". The shading bug is FIXED, so the
                                              // 3x compensation is now pure blowout: the
                                              // gate is lit honestly again at ~70/PI.
constexpr float kGateKeyRange      = 13.0f;
constexpr float kGateKeyUp         = 4.6f;    // rakes the tube's crown from above-front
// ROUND 8 — WHY THE KEY WENT BACK UP. The R8 gate is ONE TUBE with a 1.9 m membrane
// sitting in its throat, and that membrane is the single brightest thing in the hall.
// Auto-exposure meters it, stops the frame down, and the tube — a mid-value weathered
// metal — crushes to a BLACK SILHOUETTE (the first K_1 capture). Every rivet, plate
// join and rust run in the forged normal map goes down with it. The gate does not need
// less exposure; it needs more LIGHT ON THE METAL, so it can hold its own value
// standing next to its own portal.
//   This is NOT the round-6 blowout coming back. That was a self-EMISSIVE being
//   double-counted by the fixed shading path. There is no emissive on the tube body at
//   all now. This is a lamp: it obeys the inverse-square law, it falls off, and it
//   casts a shadow — which is exactly what carves the machined surface.
// A second, LOW key rakes the tube's underside so the whole body reads instead of a
// lit crown sitting on a black belly.
constexpr float kGateLowI          = 2.60f;   // R9: was 30.0 (same PI inflation)
constexpr float kGateLowUp         = 0.9f;
constexpr float kGateLowHubOff     = 4.4f;
constexpr float kGateLowRange      = 11.0f;
constexpr float kGateKeyHubOff     = 1.9f;    // hub-side standoff from the gate plane
// Warm UNDER-fill (the reference's conduit/indicator bounce on the lower plates).
constexpr float kGateFillColor[3]  = { 1.00f, 0.66f, 0.36f };
constexpr float kGateFillI         = 2.40f;   // R9: was 20.0 (same PI inflation)
constexpr float kGateFillRange     = 7.0f;
constexpr float kGateFillUp        = 0.85f;
constexpr float kGateFillHubOff    = 2.2f;

// ---- Stone gateway ring geometry ----------------------------------------------
// A SUBSTANTIAL, thick ring you walk through — a single circle of N deep tangent
// box segments with a beefy squarish cross-section (real radial thickness + real
// depth through the gate). Grey stone, NON-glowing (a faint emissive self-lift
// only so it reads in shadow — it is NOT an energy source).
// ROUND 8 — THE GATE IS *ONE LARGE METALLIC TUBE*. These mirror the constants in
// tools/build_rifthub_gate.py (keep them in sync; that file is the authority):
//   tube centerline R = 2.60, tube r = 0.66 -> throat opens at 1.94, outer rim 3.26.
// The membrane (1.895) and its fresnel rim (1.868) both clear the 1.94 throat.
constexpr uint32_t kRingSegments   = 40;     // (fallback-only) deep box segments
constexpr float    kRingY          = 2.2f;   // ring center height above the floor
constexpr float    kRingR          = 2.60f;  // TUBE CENTERLINE radius (was 2.05)
constexpr float    kRingHalfRad    = 0.66f;  // (fallback) radial half-thickness
constexpr float    kRingHalfDepth  = 0.66f;  // (fallback) half-depth through the gate
constexpr float    kRingStone[3]   = { 0.62f, 0.70f, 0.68f };  // teal-cast tint over the DARK trim_a set
constexpr float    kRingEmissive   = 0.06f;  // near-zero self-lift — the blue core + hall LIGHT the metal
// FALLBACK torus (only when gate_ring.glb is absent): a plain heavy tube, matching
// the authored one's mass so the world reads the same if the GLB never lands.
constexpr float    kRingTubeR      = 0.66f;  // tube radius (was 0.40)
constexpr uint32_t kRingMajorSeg   = 96;     // smooth silhouette
constexpr uint32_t kRingMinorSeg   = 28;     // smooth highlight sweep
// Ring inner edge = kRingR - kRingHalfRad = 1.65 m; the membrane pool's outer
// band tops out near 1.585 m, so the opening stays clear (no clip).
// Segment "long-axis" half-extent = half the chord between adjacent segment
// centers, with a small overlap so neighboring boxes butt cleanly without gaps.
inline float segHalfTangent(float ringR) {
    const float pi = 3.14159265358979f;
    return ringR * std::sin(pi / (float)kRingSegments) * 1.06f;  // 6% overlap (chunky butt)
}

// ---- CHEVRONS: DELETED (ROUND 7 addendum 2 — owner: "No chevrons needed") ------
// Nine amber-lit clamp housings used to ring the gate's face. They were a
// Stargate-franchise borrow, they were never OUR machine, and across rounds 1-6
// they were the single most persistent source of the toy read: round 1's flat
// yellow triangles became round 2's machined housings with amber slits, and the
// owner's verdict never actually moved. So they are gone — the housings, the jaw
// flanges, the face caps, the slit cores, the flicker, the constants, the entity
// span in RiftPortal, and the self-test assertions that policed them.
//
// What replaces them is NOT another ring of parts. It is the tube's own surface:
// features CUT INTO it (segment seams, recessed bands, vent grilles, the indicator
// groove, the operator bay) plus the SD3.5-forged normal/height maps that carry
// the rivets, plate joins, welds, rust and stencils. Detail as SURFACE, not as
// silhouette — R7's whole point.
//
// The one amber thing that survives is the segmented indicator TRACK below, and it
// survives on evidence: it is visible in the owner's own reference footage, and it
// now sits RECESSED INSIDE a groove machined into the tube (v = -152 deg), which is
// exactly what R7 addendum 2 permits ("indicator lighting stays subtle and
// integrated (recessed slits), never a ring of amber triangles").
constexpr float    kChevAmber[3]   = { 1.00f, 0.46f, 0.08f };  // the amber the TRACK still uses
constexpr float    kChevSlitDark[3] = { 0.05f, 0.035f, 0.02f }; // unlit emitter = dark glass

// ---- Ring v2 over-plates + rivets (industrialize the smooth torus) -------------
// Varied-depth riveted armor plates wrapped over the torus rim break the
// uniform donut: per-portal kPlateArcCount arc plates at jittered angular
// slots/sizes seated on the OUTER rim, plus small rivet studs on the front
// face. Textured from the curated surface library, tinted toward the locked
// TEAL-OXIDE patina.
constexpr uint32_t kPlateArcCount  = 12;
constexpr float    kPlateSeatR     = 2.16f;   // plate center radius (over the tube crest)
constexpr float    kPatinaTint[3]  = { 0.30f, 0.38f, 0.36f };  // teal-oxide multiplier (round 2: grime-dark)
constexpr float    kSteelTint[3]   = { 0.30f, 0.33f, 0.36f };  // neutral steel multiplier (round 2: grime-dark)
constexpr float    kDarkTint[3]    = { 0.30f, 0.33f, 0.35f };  // dark tint over BRIGHT sets
constexpr float    kGunTint[3]     = { 0.72f, 0.76f, 0.80f };  // over the DARK trim_a set (~0.21 effective)
constexpr uint32_t kRivetCount     = 12;      // front-face rivet studs per portal
constexpr float    kRivetHalf      = 0.032f;
// ROUND 3 gate-GLB material tints (over the same curated sets): darker than the
// round-2 procedural tints — the F_1 shots read chalky-pale under the blue core
// light because the GLB's big smart-projected plates catch far more of it.
// ---- ROUND 9: THE TUBE WAS BLACK. THIS IS WHY, AND THIS IS THE FIX. -----------
// Tim, four rounds running: "the gate is supposed to be ONE LARGE metallic Tube."
// It kept rendering as a black void ring. It was never a lighting problem.
//
// The tints below MULTIPLY the fallback surface-set albedo. They were authored to
// beat down the SD-FORGED gate sets (gate_tube_hull / gate_ring_plate / ...), which
// came out sandy and bleached. But THOSE SETS WERE NEVER HARVESTED — the LFS budget
// ran out (see the note at m_surf.mount below), so they do not exist on any machine.
// Every gate group therefore silently falls back to the CURATED sets, and a tint of
// 0.22 meant to tame a bleached texture instead crushed an already-correct one:
//
//   patina/hull : mw_metal_panels_a albedo 0.789 x 0.22 = 0.174, then metallic 0.80
//                 removes 80% of the diffuse lobe  ->  effective albedo 0.035  = BLACK
//   steel       : mw_metal_trim_b   albedo 0.591 x 0.24 = 0.142, metallic 0.85  = BLACK
//
// mesh.frag's PBR diffuse is `albedo * (1 - metallic)`. A 0.03-albedo surface CANNOT
// be lit; 5x the key light moves it by nothing. This is KNOWN_BUGS "VALUE, NOT LUMENS"
// and L5 (metallic clamp) in the same object. Renormalized to real machined-metal
// values (~0.55-0.63 sRGB effective) so the membrane's blue key actually CASTS on the
// tube and SHAPES it, which is the whole point of a big round machined thing.
// With the mirror fixed the tube FINALLY takes light — and at a 0.58 albedo it came
// back blown-out white: a marshmallow. That is the same lesson from the other side.
// These are now real DARK MACHINED STEEL values: the blue key glints off the crest
// and the body holds a mid-dark value instead of glowing like plastic.
constexpr float    kGatePlateTint[3] = { 0.31f, 0.33f, 0.36f };  // x0.789 => ~0.24/0.26/0.28 dark machined gunmetal (the TUBE)
constexpr float    kGateSteelTint[3] = { 0.44f, 0.47f, 0.51f };  // x0.591 => ~0.26/0.28/0.30 machined steel
constexpr float    kGateDarkTint[3]  = { 0.85f, 0.88f, 0.94f };  // x0.276 => ~0.23/0.24/0.26 darker hardware
// ROUND 4 lane-1 tints — over the SD-FORGED gate sets (which carry their own
// value + patina). The forged albedos run SATURATED teal, and the locked
// palette wants DARK STEEL DOMINANT with teal as accent: the dominant plate
// group gets a de-tealing grime tint (R held vs G/B suppressed), the accent
// group keeps its teal but grimed down, hardware near pass-through (the
// piston albedo is already near-black).
// ROUND 5 — the forged sets are now IMG2IMG'd FROM THE OWNER'S OWN REFERENCE
// FRAME (tools/forge_gate_textures.py --img2img), so each albedo already carries
// the reference's exact value, patina and rust bleed (means 39/58/71 etc. — real
// weathered-metal darkness). The round-4 tints (0.34-0.44) existed to beat down
// text-prompted albedos that came out sandy/bleached; multiplying THESE by 0.4
// would crush the gate to near-black. Near pass-through now, with only a slight
// de-teal on the dominant plate group (the locked palette wants dark steel
// dominant, teal as accent).
// Texture-gated ambient strength for the gate (see the authoring block).
constexpr float    kGateAmbient       = 0.16f;
constexpr float    kForgePlateTint[3] = { 1.00f, 0.96f, 0.92f };  // riveted armor — pass-through, de-tealed
constexpr float    kForgeTealTint[3]  = { 0.95f, 1.00f, 1.00f };  // peeling teal accent keeps its oxide
constexpr float    kForgeDarkTint[3]  = { 1.00f, 1.00f, 1.05f };  // machined hardware (already near-black)

// ---- Segmented amber RATCHET TRACK (inner-facing edge; PortalAnimated.mp4) -----
// Small amber segments ringing the gate's inner front edge. Dormant: dim.
// SURGE: a bright chase sweeps the circumference (activation feedback).
// OPEN: steady powered glow. All writes capped at kTrackEmCap.
// ROUND 8: the track now seats INSIDE the groove machined into the tube at the
// inner-front shoulder. The tube's surface at r = 2.02 sits at z = -0.315 (local),
// and the groove cuts 0.058 deeper — so a segment placed 0.300 proud of the gate
// plane lands INSIDE the groove: a recessed indicator slit, not a proud amber pip.
// ---- ROUND 9: THE YELLOW DASHED RING IS DEAD. --------------------------------
// Tim rejected the "cartoony" gate FOUR times, and this was the loudest offender:
// 48 amber segments, each covering ~53% of its arc, ringing the gate face. That is
// a DASHED YELLOW RING. It reads as hazard/caution tape wrapped around the portal.
// It is not in the reference footage, and no amount of "recessing" it in a groove
// was ever going to fix a repeating yellow pattern.
//
// What survives is what the brief actually permits: "a thin recessed light line."
// The segments now BUTT (half-extent 0.140 > the 0.132 half-pitch => ~6% overlap =>
// ONE CONTINUOUS ring, no gaps, no dashes), they are much THINNER in section, they
// are DIM, and they are COOL BLUE-WHITE instead of amber — so the accent belongs to
// the gate's own blue key light instead of fighting it with a second, warmer hue.
// The entity span and the tick() animation are unchanged, so the state machine and
// the self-tests still have a track to drive.
constexpr uint32_t kTrackSegs      = 48;   // finer track on the bigger ring
constexpr float    kTrackR         = 2.02f;   // segment center radius (rides the cut groove)
constexpr float    kTrackProud     = 0.300f;  // hub-side offset from the gate plane
constexpr float    kTrackHalfTan   = 0.140f;  // > half-pitch (0.132) => CONTINUOUS. NO DASHES.
constexpr float    kTrackHalfRad   = 0.022f;  // a thin LINE, not a fat pip
constexpr float    kTrackHalfDep   = 0.012f;
constexpr float    kTrimCool[3]    = { 0.46f, 0.66f, 1.00f };  // cool blue-white trim light (was traffic amber)
constexpr float    kTrackEmIdle    = 0.10f;   // subtle: it is trim lighting, not a light show
constexpr float    kTrackEmOpen    = 0.34f;
constexpr float    kTrackChase     = 0.55f;   // added peak at the chase crest (was 2.00 = a strobing halo)
constexpr float    kTrackChaseRadS = 9.0f;    // chase sweep speed (rad/s)
constexpr float    kTrackEmCap     = 0.80f;

// ---- Conduits + coils + TEAL holo screens (dressing; ROUND 2 pipe rebuild) -----
// Round 1 drew each conduit segment as a flat traffic-cone-orange emissive
// square tube. Round 2: the conduit is a REAL PIPE — near-black gunmetal
// PBR-textured body (two boxes, the second rotated 45 deg about the pipe axis
// -> octagonal profile) with dark steel COLLARS at every bend, and only a
// thin amber CORE LINE riding the pipe's hub-facing surface emits (the
// power-flow pulse tick() phases along the run lives on THAT strip).
constexpr float    kConduitOrange[3] = { 1.00f, 0.29f, 0.035f };
constexpr float    kConduitEmBase    = 0.65f;
constexpr float    kConduitFlowAmp   = 0.42f;
constexpr float    kConduitEmCap     = 1.30f;   // orange must stay ORANGE (never yellow-clips)
constexpr float    kConduitFlowHz    = 0.55f;   // flow pulse rate
constexpr float    kConduitFlowK     = 1.60f;   // phase step per segment (the travel)
constexpr float    kConduitHalf      = 0.014f;  // emissive CORE LINE half-thickness (thin)
constexpr float    kConduitDark[3]   = { 0.06f, 0.035f, 0.015f };  // core unlit = dark glass
constexpr float    kPipeHalf         = 0.055f;  // gunmetal pipe body half-thickness
constexpr float    kPipeTint[3]      = { 0.46f, 0.48f, 0.52f };    // near-black over the dark trim_a set
constexpr float    kCollarHalf       = 0.085f;  // bend collar half-extent
constexpr float    kCollarHalfDep    = 0.048f;  // bend collar half-length along the pipe
constexpr float    kCoilOrangeEm     = 0.85f;
constexpr float    kHoloTeal[3]      = { 0.12f, 0.85f, 0.75f };
constexpr float    kHoloEm           = 0.25f;   // round 2: teal dimmed ~30% (was 0.35)
                                                // — the TEXTURE carries the read

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

// ---- Floor plate (octagonal industrial deck; ROUND 2) --------------------------
// 8 box wedges in a ring on the floor. Round 1 tinted the whole deck in the
// portal's saturated identity color at emissive 0.45 — "solid plastic identity
// plates" clashing violently. Round 2: the wedges are DARK textured metal deck
// (grate set, near-zero emissive); the identity lives ONLY on a thin emissive
// TRIM RING at the deck's outer edge, its color desaturated ~50%.
constexpr uint32_t kPlateSegments  = 8;
constexpr float    kPlateRingR     = 1.70f;  // plate ring radius (center of each wedge)
constexpr float    kPlateHalfY     = 0.05f;  // plate slab half-Y (flat)
constexpr float    kPlateBoxThick  = 0.50f;  // plate wedge half-extent (radial)
constexpr float    kPlateDeckTint[3] = { 0.34f, 0.36f, 0.38f };  // dark deck metal
constexpr float    kTrimRingInnerR = 2.06f;  // identity trim ring (thin, outer edge)
constexpr float    kTrimRingOuterR = 2.18f;
constexpr float    kTrimRingEm     = 0.60f;  // capped accent — a trim, not a beacon
                                             // (round 3: dimmed; the F_2 hoop was loud)
constexpr float    kTrimDesat      = 0.50f;  // identity colors desaturated ~50%
// Floor wedges butt at their outer edge — half-tangent = R * sin(pi/8) * 1.04 overlap.
inline float plateHalfTangent() {
    const float pi = 3.14159265358979f;
    return kPlateRingR * std::sin(pi / (float)kPlateSegments) * 1.04f;
}

// ---- (ROUND 6) THE FAKE CENTER DOT IS DELETED ----------------------------------
// v1 drew an "energy core hot spot" at the exact ring center — two small bright
// blue-white disks composited ON TOP of the membrane. Once the membrane became
// the owner's REAL FOOTAGE, that sprite was simply a hand-drawn dot painted over
// video: "The swirling one looks fake.. Why the dot in the middle?" The footage
// carries its own center (the OPEN throat converges to a natural bright core, the
// SURGE has a dark eye). The core disks, their geometry constants, their emissive
// pulse and their cap are GONE. What survives — and must — is the blue POINT LIGHT
// each gate casts into its bay (kCoreLight* below): that is LIGHTING, not a sprite.
//
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
// (kCoreBlue / kCoreInner* / kCoreBlueMinEm / kCoreBlueMaxEm / kCoreEmCap /
//  kCoreFreqHz are DELETED with the fake center dot — round 6.)

// ---- Blue CORE point light (casts the event horizon onto the grey stone) ------
// The step-4 resolution of the blue-vs-grey conflict: keep the grey STONE ring,
// but drive a cool-blue point light from each ring center that lights the stone
// (+ floor + chevrons) blue, pulsing slowly with the hum so the gate breathes.
constexpr float    kCoreLightBlue[3]  = { 0.52f, 0.72f, 1.00f };  // cool blue, de-saturated so it
                                                                //  LIGHTS the steel instead of painting it
// ROUND 5: the ART TARGET says "each gate's membrane is the KEY LIGHT of its bay"
// — and it never was. At base 2.2 / range 7 the core light delivered ~0.05
// irradiance to the tube crest 2.35 m away: nothing. The gate hardware was lit by
// literally nothing and only "read" because it carried a fake self-emissive.
// mesh.frag's PBR lobe is energy-conserving (diffuse = albedo/PI) and pointAtten
// is 1/(d^2+1), so a light 2-3 m off a 0.13-linear metal needs INTENSITY IN THE
// TENS to land a highlight. It does now: the membrane floods its own ring with
// blue, exactly like the reference stills.
constexpr float    kCoreLightBase     = 11.0f;          // base intensity multiplier
constexpr float    kCoreLightMin      = 9.0f;          // pulse-with-hum floor
constexpr float    kCoreLightMax      = 15.0f;          // pulse-with-hum peak
constexpr float    kCoreLightFreqHz   = 1.1f;           // slow hum-synced breathe
constexpr float    kCoreLightRange    = 9.5f;           // reaches the gate + plate
// How far HUB-SIDE of the gate plane the membrane's key light sits. A glowing disc
// radiates forward; its point stand-in must sit in front of the plane or the tube's
// entire front face shades at N.L ~= 0 (see the light-build block). 1.35 m puts the
// blue at a raking angle across the tube face — it MODELS the round form instead of
// skimming past it — while staying inside the ring's own aperture.

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
// ROUND 5 (owner: "make the portal effect BIGGER"): the disk now fills the gate's
// BORE. The authored GLB's inner throat barrel runs r 1.66..1.73, so 1.655 is the
// largest disk that clears it (was 1.58 — a 0.08 m dead ring of barrel wall showed
// between the storm and the ratchet track). +9.7% membrane area, zero clip.
constexpr float    kMembraneR         = 1.895f;          // disk radius (the ROUNDED throat opens at 1.90)
constexpr uint32_t kMembraneDiskSegs  = 48;              // fan segments (smooth silhouette)
// Plasma layer: deep blue, saturated. Texture carries the white-blue filament
// detail; the per-entity emissive tint keeps the whole sheet blue-dominant.
constexpr float    kPlasmaBlue[3]     = { 0.24f, 0.52f, 1.00f };
constexpr float    kPlasmaEmBase      = 1.45f;           // IDLE steady-state strength
// ROUND 6: 1.90 was tuned for the PROCEDURAL throat map (a dim math texture that
// needed pushing). The OPEN state now plays the reference FOOTAGE, whose frames are
// already bright — at 1.90 the video's deep-navy channels lifted to mid-blue and the
// throat washed toward cyan-white, i.e. we were destroying the very contrast we went
// to the video for. Back it off so the footage's own value range reads.
constexpr float    kPlasmaEmBaseOpen  = 1.58f;           // OPEN throat (footage-calibrated)
constexpr float    kPlasmaEmWobble    = 0.30f;           // organic breathe amplitude
constexpr float    kPlasmaEmCap      = 2.40f;            // HARD CAP (blue must survive tonemap)
constexpr float    kPlasmaSpinRadS    = 0.22f;           // slow storm rotation (rad/s)
// ROUND 6: the OPEN disk used to spin 2.6x to make the PROCEDURAL spokes "stream".
// The footage streams on its own; spinning it fast on top just reads as a rotating
// texture (fake). A slow drift under the film is enough.
constexpr float    kPlasmaSpinOpenX   = 1.15f;           // OPEN: barely faster than idle
// (The VISTA layer is GONE — round 5. An opaque disk parked behind an opaque
// disk shows nothing from the front and BLACKS OUT the portal from the back;
// see the membrane authoring block. A real see-through vista = render-to-texture
// portal view, the future upgrade in docs/RIFTHUB_ART_TARGET.md.)
// ---- Membrane FLIPBOOK (ROUND 4 J2): the IDLE plasma layer plays the baked
// reference-video frames (8x6 atlas -> 48 tiles, loop-blended in the bake so a
// plain modulo loop has no wrap pop). 18 fps lands in the task's 16-24 band;
// the per-portal phase offset keeps the 8 gates from strobing in unison.
// ROUND 6: the SAME machinery now plays the SURGE and OPEN atlases too — the
// membrane never shows a hand-coded texture again while the atlases are present.
// The OPEN atlas loops at kFlipFps like the idle one; the SURGE atlas is a
// ONE-SHOT played across the kawoosh (progress-mapped: frame = surge progress *
// N, clamped to the last frame), so the vortex ring collapses into the throat in
// exactly the time the flash lasts and hands off to the OPEN loop on the frame it
// was baked to hand off on (surge span ends t 8.30, open span starts t 8.40).
constexpr uint32_t kFlipCols          = 8;
constexpr uint32_t kFlipRows          = 6;
constexpr float    kFlipFps           = 18.0f;
constexpr float    kFlipPortalPhase   = 0.61f;           // seconds per portal index
// Flip-mode emissive tint: the baked frames already CARRY the reference's deep
// blue, so the procedural layer's kPlasmaBlue would double-blue (and dim) them
// — a paler, still blue-dominant tint lets the video's own color speak. The
// strength cap (kPlasmaEmCap) + blue-dominance law are unchanged.
constexpr float    kFlipTint[3]       = { 0.70f, 0.83f, 1.00f };
// Fresnel rim (ROUND 2): round 1's single fat tube read as a uniform neon
// sign. Now a FALLOFF STACK — a thin bright CONTACT ring right at the
// membrane edge (the one deliberately hot line, blue-tinted, dimmer than
// before), plus two static dimmer/thinner shells stepped hub-side so the
// glow visibly decays away from the membrane (the fresnel read).
constexpr float    kRimR              = 1.868f;          // contact ring centerline radius
                                                         // (tube 0.03 -> outer 1.658 = the
                                                         //  new membrane edge; still inside
                                                         //  the 1.66 bore, never buried in it)
constexpr float    kRimTubeR          = 0.030f;          // THIN tube
constexpr float    kRimBlue[3]        = { 0.32f, 0.60f, 1.00f };
constexpr float    kRimEmBase         = 1.10f;
constexpr float    kRimShimmerAmp     = 0.22f;
constexpr float    kRimShimmerHz      = 0.90f;
constexpr float    kRimEmCap          = 1.70f;   // v1 lesson: >2.9 reads WHITE after ACES
constexpr float    kRimFallRA         = 1.845f;  // falloff shell A (mid)
constexpr float    kRimFallTubeA      = 0.021f;
constexpr float    kRimFallOffA       = 0.055f;  // hub-side offset from the membrane
constexpr float    kRimFallEmA        = 0.42f;
constexpr float    kRimFallRB         = 1.825f;  // falloff shell B (outermost, faintest)
constexpr float    kRimFallTubeB      = 0.015f;
constexpr float    kRimFallOffB       = 0.115f;
constexpr float    kRimFallEmB        = 0.16f;
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
constexpr float    kArcThickness      = 0.013f;          // beam half-thickness (m) — round 5: thinner; the taper does the rest
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

// ===========================================================================
// ROUND 8 — THE OPERATOR PANEL, THE CONSOLE, AND THE CATASTROPHES
// ===========================================================================
// The tube's recessed bay (LCD + chunky buttons + LED readout strips) ships INSIDE
// gate_ring.glb as two extra material groups. The engine's job is to make them a
// LIVE part of the telegraph: the LCD and the LEDs shift green -> amber -> red and
// pulse harder as the console's instability climbs, so the tube itself tells the
// player the rift is getting angry — before anything blows.
constexpr float kPanelScreenCalm[3] = { 0.20f, 0.85f, 0.95f };   // LCD: cool blue-cyan
constexpr float kPanelScreenHot[3]  = { 1.00f, 0.22f, 0.12f };   // LCD: alarm red
constexpr float kPanelScreenEm      = 1.55f;
constexpr float kPanelScreenCap     = 2.20f;
constexpr float kPanelLedCalm[3]    = { 0.25f, 1.00f, 0.55f };   // LEDs: green = nominal
constexpr float kPanelLedHot[3]     = { 1.00f, 0.30f, 0.10f };   // LEDs: red = danger
constexpr float kPanelLedEm         = 1.20f;
constexpr float kPanelLedCap        = 2.00f;

// The HANGING HOLOTERMINAL console in front of each rift (the canonical holo look:
// black glass slab + round-pipe frame + a single support pipe to the ceiling).
// Replaces the floating flat teal sign rectangles, which are DELETED.
// ROUND 8, second pass: the first cut hung a 1.30 x 0.86 m slab DEAD CENTRE of the
// approach at 4.35 m — which, from the player's eyeline, is a billboard parked in front
// of the gate. It occluded the very thing it is a console FOR. It now stands OFF TO THE
// SIDE, closer, and smaller: you walk up to it and the rift stays in view behind it.
constexpr float kConsoleStandoff    = 3.60f;  // hub-side of the gate plane
constexpr float kConsoleSideOff     = 2.05f;  // off the centreline (right of approach)
constexpr float kConsoleY           = 1.45f;  // glass centre height (readable standing)
// R9: the glass grew (was 0.86 x 0.58). A rift console has a LOT more to say than the
// cell terminal — destination, status and five parameter rows — and at the old size the
// readout was legible only with your nose against it. Sized so it reads from [E] range.
constexpr float kConsoleW           = 1.16f;
constexpr float kConsoleH           = 0.78f;
constexpr float kConsoleUseR        = 3.4f;   // [E] range

// ---- CATASTROPHE timings + magnitudes -------------------------------------
constexpr float kImplodeDur     = 2.40f;   // the collapse (membrane inverts + sucks in)
constexpr float kImplodePullA   = 26.0f;   // inward acceleration on loose motes (m/s^2)
constexpr float kImplodeShake   = 0.55f;   // shockwave camera shake (m)
constexpr float kImplodeDamage  = 0.85f;   // red damage flash at the shockwave
constexpr float kWarpDur        = 9.0f;    // ROOM WARP (disorienting, survivable)
constexpr float kWarpAmp        = 0.85f;   // prop bow/drift amplitude (m)
constexpr float kWarpWaveK      = 0.55f;   // ripple wavenumber (rad/m)
constexpr float kWarpWaveHz     = 0.42f;   // ripple travel rate
constexpr float kWarpFovDeg     = 22.0f;   // peak lens breathe (deg, added to FOV)
constexpr float kTemporalDur    = 8.5f;    // TEMPORAL RIFT
constexpr float kTemporalSlow   = 0.22f;   // deepest slow-motion
constexpr float kTemporalStutHz = 3.1f;    // the stutter (time stops agreeing with itself)
constexpr float kShakeDecay     = 2.2f;    // 1/s
constexpr float kFlashDecay     = 1.4f;    // 1/s
// The alarm strobe the hall lights inherit while a catastrophe runs.
constexpr float kAlarmRed[3]    = { 1.00f, 0.16f, 0.10f };
constexpr float kAlarmI         = 34.0f;
constexpr float kAlarmHz        = 1.9f;

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
// THE EIGHT GATES' DEFAULT AIM. These are DESTINATION-REGISTRY KEYS now
// (app/destinations.h), not `--world` names — the hub is fast travel to PLACES.
// It used to be { act2caves, act2, valley, cliffs, club, destruct, ragdoll,
// terrain }: four dev benches and two worlds (act2 / act2caves) that have had NO
// host since the Act-2 split, so a quarter of the hub signposted nothing.
//
// Eight gates is no longer eight destinations: each gate's console can be
// re-aimed at ANY of the registry's places (type it, or walk the PREV/NEXT cycle),
// so the hub reaches everything. These are just where they POINT ON ARRIVAL —
// deliberately one per region of the world, so the ring reads as a map.
constexpr PortalSpec kPortalTable[] = {
    { "club",    (uint32_t)RifthubTrigger::Act2Caves, { 1.00f, 0.20f, 0.85f } }, // magenta
    { "crystal", (uint32_t)RifthubTrigger::Act2,      { 0.75f, 0.30f, 1.00f } }, // violet
    { "crash",   (uint32_t)RifthubTrigger::Valley,    { 1.00f, 0.55f, 0.15f } }, // orange-amber
    { "city",    (uint32_t)RifthubTrigger::Cliffs,    { 1.00f, 0.92f, 0.65f } }, // white-gold
    { "river",   (uint32_t)RifthubTrigger::Club,      { 0.20f, 0.85f, 1.00f } }, // cyan
    { "ridge",   (uint32_t)RifthubTrigger::Destruct,  { 0.45f, 0.70f, 1.00f } }, // sky-blue
    { "f1",      (uint32_t)RifthubTrigger::Ragdoll,   { 0.30f, 1.00f, 0.40f } }, // green
    { "f7",      (uint32_t)RifthubTrigger::Terrain,   { 1.00f, 0.20f, 0.15f } }, // red
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

// ---- Fake-volumetric light shafts (ROUND 3 workstream 2) -------------------------
// Per-shaft particle column constants. A GLASS cone was tried first and failed:
// the glass fallback path lifts output alpha by fresnel, so a big low-opacity
// shell reads as a SOLID matte sail at grazing angles (F_3 first capture).
// Additive billboards have no such term — soft, accumulative, bloom-fed.
constexpr int   kShaftParticles   = 26;     // billboards per shaft column
constexpr float kShaftTopR        = 0.22f;  // column radius at the fixture (m)
constexpr float kShaftDriftRadS   = 0.22f;  // slow swirl of the dust column

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

// OPEN-THROAT emissive map — THE MISSING-ATLAS FALLBACK ONLY (round 6).
//
// This function used to BE the OPEN state, and that is exactly what the owner
// saw and rejected: "The swirling one looks fake." It is hand-coded math — a
// polar ridged-noise spiral — sitting next to seven other gates playing real
// footage. The OPEN state now plays membrane_flipbook_open.png (the reference
// video's own throat, t 8.4-9.95 s). This map survives ONLY so a fresh clone
// with LFS-stub textures still shows a lit, blue, streaming membrane instead of
// a black disk. Its hot-center BURST TERM (exp(-r*r*9) painted white into the
// middle) is deleted too — that was the other half of "why the dot in the
// middle?" — so even the fallback has no fake core.
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
            // Radial envelope: alive to near the rim. NO hot-center burst term
            // (round 6: the fake dot is gone — from the fallback too).
            float env = 1.0f - r;
            if (env < 0.0f) env = 0.0f;
            float t = spokes - 0.42f;
            if (t < 0.0f) t = 0.0f;
            t *= 2.2f; if (t > 1.0f) t = 1.0f;
            const float e2 = 0.30f + 0.70f * env;
            const float baseR = 10.0f + 30.0f * body, baseG = 30.0f + 66.0f * body,
                        baseB = 110.0f + 110.0f * body;
            const float filR = 205.0f, filG = 230.0f, filB = 255.0f;
            float R = (baseR + (filR - baseR) * t) * e2;
            float G = (baseG + (filG - baseG) * t) * e2;
            float B = (baseB + (filB - baseB) * t) * e2;
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

// ---- MEMBRANE FLIPBOOK loader (ROUND 6: ONE path, THREE states) ---------------
// Slice an 8x6 flipbook atlas (tools/make_membrane_flipbook.py, baked from a span
// of the owner's reference video) into kFlipCols*kFlipRows per-frame textures.
// Any failure — file absent, Git-LFS pointer stub instead of pixels, dimensions
// not divisible by the grid — returns EMPTY, and that membrane state falls back
// to its procedural map. The world never breaks on a fresh clone.
std::vector<x3::rhi::TextureHandle> loadFlipbookAtlas(x3::rhi::IRenderDevice& device,
                                                     const std::string& path,
                                                     const char* label) {
    std::vector<x3::rhi::TextureHandle> out;
    int aw = 0, ah = 0;
    const std::vector<uint8_t> atlas = decodePngRGBA8(path, aw, ah);
    if (!atlas.empty() && aw > 0 && ah > 0 &&
        (uint32_t)aw % kFlipCols == 0 && (uint32_t)ah % kFlipRows == 0) {
        const uint32_t tw = (uint32_t)aw / kFlipCols;
        const uint32_t th = (uint32_t)ah / kFlipRows;
        std::vector<uint8_t> tile((size_t)tw * th * 4u);
        out.reserve((size_t)kFlipCols * kFlipRows);
        for (uint32_t f = 0; f < kFlipCols * kFlipRows; ++f) {
            const uint32_t r0 = (f / kFlipCols) * th;
            const uint32_t c0 = (f % kFlipCols) * tw;
            for (uint32_t y = 0; y < th; ++y)
                std::memcpy(&tile[(size_t)y * tw * 4u],
                            &atlas[(((size_t)r0 + y) * (size_t)aw + c0) * 4u],
                            (size_t)tw * 4u);
            out.push_back(device.createTexture(tile.data(), tw, th, true));
        }
    }
    x3::logInfo(std::string("[rifthub] membrane flipbook (") + label + "): " +
                (out.empty() ? "atlas absent -> procedural fallback"
                             : std::to_string(out.size()) + " frames of the "
                               "reference video"));
    return out;
}

// Which flipbook frame does this membrane state show right now?
//   IDLE  / OPEN : a LOOP at kFlipFps (per-portal phase so the 8 gates don't
//                  strobe in unison; both atlases are loop-blended in the bake,
//                  so a plain modulo has no wrap pop).
//   SURGE        : ONE-SHOT — the 48 frames are mapped across the kawoosh's
//                  LINEAR progress (0 at the flash, 1 at the hand-off), clamped
//                  to the last frame, so the vortex ring finishes collapsing
//                  exactly as the gate opens (NOT the exponential brightness
//                  envelope: that would stall the film on its last frames).
uint32_t flipFrameIndex(uint32_t n, float time, uint32_t portalIdx,
                        bool surge, float surgeProg) {
    if (n == 0) return 0;
    if (surge) {
        float prog = surgeProg;
        if (prog < 0.0f) prog = 0.0f;
        if (prog > 1.0f) prog = 1.0f;
        const uint32_t f = (uint32_t)(prog * (float)n);
        return f >= n ? n - 1 : f;
    }
    return (uint32_t)((time + (float)portalIdx * kFlipPortalPhase) * kFlipFps) % n;
}

} // namespace

float Rifthub::ringWorldY() const { return m_desc.origin.y + kRingY; }

void Rifthub::build(Scene& scene, x3::rhi::IRenderDevice& device,
                    x3::phys::IPhysicsWorld& physics, TriggerSystem& triggers,
                    const Desc& desc) {
    if (m_built) return;
    m_desc = desc;

    // W-RIFT: the REGION ORIGIN. Every world position below is authored hub-LOCAL
    // and lifted by (OX,OY,OZ) exactly once, at the point where it becomes a world
    // coordinate (a mesh center, a collision vert, a trigger AABB, a light, the
    // spawn). Default origin {0,0,0} => the `--world rifthub` authoring is
    // unchanged, to the float.
    const float OX = m_desc.origin.x;
    const float OY = m_desc.origin.y;
    const float OZ = m_desc.origin.z;

    // ===== Spawn point (center of the ring) =====
    m_spawn = x3::phys::Vec3{ OX, OY + kSpawnFeetY, OZ };

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
        // MR texel: glTF packing G=roughness B=metallic -> fully rough dielectric.
        const uint8_t mrPx[4] = { 0, 255, 0, 255 };
        m_mrFlat = device.createTexture(mrPx, 1, 1, false);
        // (The teal holo data-screen textures are GONE with the floating signs —
        //  round 8-C. The consoles are real HoloTerminals, which bake their own
        //  glass texture from the readout text.)
        // MEMBRANE FLIPBOOKS (ROUND 6): all THREE membrane states play the
        // owner's reference video. One loader, three atlases (idle / surge /
        // open); each falls back on its own to the procedural map if its atlas
        // is missing (fresh clone with LFS stubs).
        const std::string fbDir = assetRoot() + "/textures/rifthub/";
        m_flipTex      = loadFlipbookAtlas(device, fbDir + "membrane_flipbook.png",      "IDLE");
        m_flipSurgeTex = loadFlipbookAtlas(device, fbDir + "membrane_flipbook_surge.png","SURGE");
        m_flipOpenTex  = loadFlipbookAtlas(device, fbDir + "membrane_flipbook_open.png", "OPEN");
    }

    // ===== Curated PBR surface sets — loaded FIRST (the floor + hall use them).
    // The library owns the textures (freed in shutdown via destroyAll).
    // LFS-budget note (2026-07-11): the intro-cockpit rusted/patina sets could
    // NOT be harvested (GitHub LFS budget exhausted), so everything dresses
    // from the 24 sets already materialized on this branch, tinted toward the
    // locked teal-oxide patina.
    m_surf.mount(assetRoot() + "/surface_library");
    const SurfaceSet& sPlate = m_surf.get(device, "mw_metal_panels_a");  // riveted industrial panels
    const SurfaceSet& sDark  = m_surf.get(device, "mw_metal_trim_a");    // TRUE dark gunmetal (round 2; sr_metal_b was near-white metal)
    const SurfaceSet& sTrim  = m_surf.get(device, "mw_metal_trim_b");    // trim (plates variant)
    const SurfaceSet& sFloor = m_surf.get(device, "sr_concrete_01");     // hall floor concrete
    const SurfaceSet& sWall  = m_surf.get(device, "mw_concrete_panels_b"); // hall walls
    const SurfaceSet& sGrate = m_surf.get(device, "mw_metal_grate");     // deck plates (round 2)
    // ROUND 4 lane 1 — the SD-FORGED gate sets (tools/forge_gate_textures.py;
    // landed per docs/FORGE_GATE_TEXTURES.md with the ghost-glass roughness
    // floor rough >= 0.62 BAKED into mr.png at copy time, so the forged
    // per-texel MR is safe on the reflection path). GROUP-MAPPING NOTE: the
    // bpy 'patina' group is the gate's DOMINANT plate surface, so it wears the
    // riveted-armor set (gate_ring_plate) and the teal accent set
    // (gate_patina_plate) goes on the smaller 'steel' group — dark steel
    // dominant, teal as accent (the locked palette).
    // ROUND 8 — THE TUBE SETS. The gate is now ONE smooth machined tube, so these
    // maps ARE the detail: every rivet, plate join, weld bead, vent slat, rust run
    // and stencil the reference has lives in gate_tube_hull's normal/height (forged
    // img2img from a crop of the owner's own reference frame, nstr 21 — roughly 2x
    // the relief of the R5 sets). Missing set -> the R5 sets -> the curated sets:
    // the gate always has a surface.
    const SurfaceSet& sTubeHull   = m_surf.get(device, "gate_tube_hull");
    const SurfaceSet& sTubeSten   = m_surf.get(device, "gate_tube_stencil");
    const SurfaceSet& sForgePlate = sTubeHull.ok ? sTubeHull
                                                 : m_surf.get(device, "gate_ring_plate");
    const SurfaceSet& sForgeTeal  = sTubeSten.ok ? sTubeSten
                                                 : m_surf.get(device, "gate_patina_plate");
    const SurfaceSet& sForgeDark  = m_surf.get(device, "gate_piston_steel");
    // Wet-floor MR texel (glTF packing G=rough B=metal): low roughness => the
    // dark concrete takes tight specular + IBL sheen (the wet reflective read).
    {
        const uint8_t wetPx[4] = { 0, 72, 24, 255 };   // round 2: a touch rougher so diffuse reads
        m_mrWet = device.createTexture(wetPx, 1, 1, false);
    }
    // Gate-GLB MR overrides (ROUND 4 "ghost-glass" fix — see m_mrGate in the
    // header). ROOT CAUSE, diagnosed with reflection A/Bs (G_0_* shots): NOT
    // alpha/blending — the GLB exports OPAQUE and the entities are opaque. The
    // curated sets' MR maps are polished metal (panels_a rough .45/metal .85,
    // trim_b .32/.98, trim_a .25), all inside mesh.frag's mirror-reflection
    // gate (1 - smoothstep(.25,.6,rough)); the SSR/RT pass's half-res depth
    // march is wrong on the gate's dense thin-plate geometry (2 m first step +
    // 0.5 m thickness tunnel straight through the plates), so every polished
    // gate texel SPECULARLY showed the bright emitters behind it (ratchet
    // dashes / membrane / trim hoop) — an X-ray read on opaque geometry.
    // Each override keeps the set's metallic CHARACTER (plates/steel stay
    // metal, hardware stays the painted near-dielectric trim_a is) but lifts
    // roughness just past the 0.6 cutoff, so the wrong radiance never lands
    // and the gate keeps its weathered-metal light response.
    {
        // ROUND 9 — METALLIC CLAMP (KNOWN_BUGS L5, and the other half of the black
        // tube). metal .80/.85 deletes 80-85% of the diffuse lobe, and a near-FULL
        // metal has to get its light from somewhere else: reflections. But roughness
        // is held at >= 0.62 here ON PURPOSE (the ghost-glass/SSR fix above), which
        // is PAST mesh.frag's mirror gate `1 - smoothstep(.25,.6,rough)` — so the
        // reflection lobe is switched OFF too. Full metal + no reflections + no
        // diffuse = a black object, by construction. It could never have worked.
        //
        // Clamped to .35: the tube keeps a real specular sheen (F0 = mix(.04, albedo,
        // .35) ~= .24 — it still glints along the crest) while regaining a genuine
        // diffuse lobe, so the blue membrane light lands on it and MODELS the round
        // form. Roughness stays >= .62, so the ghost-glass fix is untouched.
        // Metal enough to READ as metal (a real specular crest under the blue key),
        // dielectric enough to keep a diffuse lobe so the round form still models.
        // Roughness stays >= .62 — past mesh.frag's mirror gate — so the ghost-glass
        // fix above is untouched.
        // ---- R10: THE ROUGHNESS FLOOR IS LIFTED (carefully) --------------------
        // rough >= 0.62 was a WORKAROUND, not a material decision: it parked the gate
        // past mesh.frag's SSR gate (1 - smoothstep(0.25, 0.6, rough)) so the buggy
        // half-res depth march could never contribute. Its ROOT CAUSE was explicitly
        // "the gate's dense THIN-PLATE geometry (2 m first step + 0.5 m thickness
        // tunnels straight through the plates)" -- i.e. the R5 gate's 233 scattered
        // thin parts. The R10 tube is ONE CLOSED HULL 1.3 m thick. The march has
        // nothing thin left to tunnel through, so the workaround is buying nothing and
        // costing everything: at 0.62 the prefiltered env is sampled at a near-top mip,
        // which is a BLURRED SMEAR -- a broad soft gradient, never the thin bright
        // chamfer LINE that is the entire visual signature of machined steel.
        // 0.38: a real machined (not mirror-polished) steel. The chamfers resolve a
        // crisp reflection of the overcast dome; the flats stay matte enough to read as
        // worked metal rather than chrome. Verified against the ghost-glass symptom
        // (see docs/screenshots/rifthub_r10) -- no X-ray, because the geometry that
        // caused it is gone.
        const uint8_t platePx[4] = { 0,  97, 166, 255 };  // rough .38, metal .65 (the TUBE)
        const uint8_t steelPx[4] = { 0,  92, 176, 255 };  // rough .36, metal .69 (bolts/housings)
        const uint8_t darkPx[4]  = { 0, 163,  26, 255 };  // rough .64, metal .10 (hardware: unchanged)
        m_mrGate[0] = device.createTexture(platePx, 1, 1, false);
        m_mrGate[1] = device.createTexture(steelPx, 1, 1, false);
        m_mrGate[2] = device.createTexture(darkPx,  1, 1, false);
    }

    // ===== ROUND 3: Blender-authored GATE GLB (the density round) ==============
    // tools/build_rifthub_gate.py authors ONE dense industrial gate (segmented
    // stacked ring plates, 9 chamfered clamp housings + jaws + pivot bosses,
    // piston rods, rim pipe runs w/ collars, bolt rings, vents, base skirt) and
    // exports assets/converted_glb/rifthub/gate_ring.glb — three material-group
    // nodes (gate_patina/gate_steel/gate_dark) this loader maps onto the curated
    // surface sets. All 8 portals instance the SAME model as Scene entities at
    // portalXform * nodeTransform. GRACEFUL FALLBACK: missing/failed GLB (or a
    // headless load that yields no drawables) keeps the full procedural ring —
    // the world never breaks. The engine-side membrane / ratchet track / chevron
    // slits / state machine / audio / triggers are untouched either way.
    m_gateGlbActive = false;
    m_gateAssets.reset(x3::asset::createAssetSource());
    if (m_gateAssets && m_gateAssets->mountDir(convertedGlbRoot(), 0)) {
        m_gateLoader.reset(x3::asset::createModelLoader(&device, m_gateAssets.get()));
        m_gateModel = m_gateLoader->load("rifthub/gate_ring.glb");
        if (m_gateModel.ok) {
            m_gateDrawables = x3::asset::makeDrawablesNamed(m_gateModel, m_gateNames);
            m_gateGlbActive = !m_gateDrawables.empty();
        }
    }
    x3::logInfo(std::string("[rifthub] gate mesh: ") +
                (m_gateGlbActive
                     ? ("Blender-authored GLB (" +
                        std::to_string(m_gateDrawables.size()) + " material groups)")
                     : "procedural fallback ring (gate_ring.glb absent/empty)"));

    // ===== Ground (static collision + the WET CONCRETE floor) =====
    // 40x40 m flat slab at y=-0.10 so the slab TOP sits at y=0 (the world Y=0
    // plane every other graybox uses). Phase C: the dev checker is gone — dark
    // concrete albedo+normal from the library with the glossy wet MR override.
    {
        x3::prims::PrimMesh g = x3::prims::makeBox(kHubHalf, 0.10f, kHubHalf,
                                                    OX, OY - 0.10f, OZ, 0.22f);
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
        // NOTE (W-RIFT): cx0/cy0/cz0 are hub-LOCAL; the region origin is added here,
        // once, so the mesh verts, the collision verts and the warp base all agree.
        auto hallBox = [&](float lx, float ly, float lz, float hx, float hy, float hz,
                           const SurfaceSet* sf, const float tint[3], float uv,
                           bool collide, float em = 0.0f) {
            const float cx0 = lx + OX, cy0 = ly + OY, cz0 = lz + OZ;
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
            const uint32_t id = scene.add(e);
            // ROUND 8 / ROOM WARP: remember this prop and the CENTER it was authored
            // at. The hall's meshes are baked in WORLD space (identity transform), so
            // there is no translation to read back at warp time — we have to record
            // the center here or the ripple has no per-prop phase and the whole hall
            // would just slide as one block. tick() writes the displacement into
            // transform[12..14] (which start at 0) and zeroes it again when the warp
            // ends, so nothing drifts permanently.
            m_warpEnts.push_back(id);
            m_warpBase.push_back(cx0);
            m_warpBase.push_back(cy0);
            m_warpBase.push_back(cz0);
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
        // ---- The -Z (south) wall. With `doorway` it is authored as TWO JAMB
        // segments + a LINTEL over the opening — the approach corridor
        // (rift_depths.*) seals its mouth onto this face. SEAM LAW holds: the
        // jambs' inner faces stay on the -kHubHalf plane, the segments butt
        // exactly at the opening edges (no gap, no overlap), and the lintel
        // spans the full opening width from doorH to the wall top. Everything
        // still collides — you can only pass through the hole.
        if (m_desc.doorway) {
            const float dcx = m_desc.doorCenterX;          // hub-local X
            const float dhw = m_desc.doorHalfW;
            const float dh  = m_desc.doorH;
            const float xL0 = -nsHalfX, xL1 = dcx - dhw;   // left jamb span
            const float xR0 = dcx + dhw, xR1 = nsHalfX;    // right jamb span
            if (xL1 > xL0) {
                const float hx = (xL1 - xL0) * 0.5f;
                hallBox((xL0 + xL1) * 0.5f, wallMidY, -wallC, hx, wallMidY + 0.10f,
                        kHallWallT * 0.5f, &sWall, kConcreteTint, 0.25f, /*collide=*/true);
            }
            if (xR1 > xR0) {
                const float hx = (xR1 - xR0) * 0.5f;
                hallBox((xR0 + xR1) * 0.5f, wallMidY, -wallC, hx, wallMidY + 0.10f,
                        kHallWallT * 0.5f, &sWall, kConcreteTint, 0.25f, /*collide=*/true);
            }
            // Lintel: from the top of the opening to the wall top (the wall box is
            // authored 0.20 m proud of kHallWallH — keep that lid overlap).
            const float topY = kHallWallH + 0.20f;
            if (topY > dh) {
                const float hy = (topY - dh) * 0.5f;
                hallBox(dcx, dh + hy, -wallC, dhw, hy, kHallWallT * 0.5f,
                        &sWall, kConcreteTint, 0.25f, /*collide=*/true);
            }
            // Door FRAME: a lit steel surround so the opening reads as a way OUT
            // (and, from the corridor, as the mouth you are walking toward).
            hallBox(dcx, dh + 0.06f, -kHubHalf + 0.06f, dhw + 0.16f, 0.06f, 0.06f,
                    &sTrim, kStripTint, 0.5f, /*collide=*/false, /*em=*/1.30f);
        } else {
            hallBox(0.0f, wallMidY, -wallC, nsHalfX, wallMidY + 0.10f, kHallWallT * 0.5f,
                    &sWall, kConcreteTint, 0.25f, /*collide=*/true);
        }
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
            // Dark housing channel above the lit strip (round 2: a FIXTURE has
            // a body — the bare glowing bar read as a floating neon stick).
            hallBox(sx, kBeamY - 0.31f, sz, 1.68f, 0.055f, 0.21f,
                    &sDark, kGunTint, 0.5f, /*collide=*/false);
            x3::prims::PrimMesh b = x3::prims::makeBox(1.6f, 0.045f, 0.16f,
                                                       sx + OX, kBeamY - 0.38f + OY, sz + OZ);
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
        // ---- FAKE VOLUMETRICS (round 3 WS2): soft light shafts under the
        // ceiling strip fixtures + two angled shafts over the machinery.
        // Registered here; RENDERED as additive dust-billboard columns by
        // drawFx() each frame (glass cones read solid — see the Shaft note).
        {
            auto addShaft = [&](const x3::phys::Vec3& top, const x3::phys::Vec3& bot,
                                float width, float alpha) {
                Shaft s;   // hub-LOCAL in, WORLD out (drawFx submits these raw)
                s.top[0] = top.x + OX; s.top[1] = top.y + OY; s.top[2] = top.z + OZ;
                s.bot[0] = bot.x + OX; s.bot[1] = bot.y + OY; s.bot[2] = bot.z + OZ;
                s.width = width; s.alpha = alpha;
                float ax = bot.x - top.x, ay = bot.y - top.y, az = bot.z - top.z;
                const float len = std::sqrt(ax * ax + ay * ay + az * az);
                if (len > 1e-6f) { ax /= len; ay /= len; az /= len; }
                if (std::fabs(ay) < 0.99f) {   // u = normalize(axis x up)
                    const float ul = std::sqrt(az * az + ax * ax);
                    s.ux = az / ul; s.uy = 0; s.uz = -ax / ul;
                } else { s.ux = 1; s.uy = 0; s.uz = 0; }
                s.vx = s.uy * az - s.uz * ay;
                s.vy = s.uz * ax - s.ux * az;
                s.vz = s.ux * ay - s.uy * ax;
                m_shafts.push_back(s);
            };
            for (uint32_t s3 = 0; s3 < kStripCount; ++s3) {
                const float ang = ((float)s3 + 0.5f) * (6.2831853f / (float)kStripCount);
                const float sx = std::cos(ang) * 10.0f;
                const float sz = std::sin(ang) * 10.0f;
                addShaft({ sx, kBeamY - 0.42f, sz }, { sx, 0.05f, sz }, 0.85f, 0.085f);
            }
            // Two angled shafts raking the perimeter machinery silhouettes.
            for (uint32_t k = 0; k < 2; ++k) {
                const uint32_t mc = (k == 0) ? 2u : 6u;
                const float ang = (float)mc * (6.2831853f / 8.0f) + 0.39f;
                const float px = std::cos(ang) * (kHubHalf - 2.2f);
                const float pz = std::sin(ang) * (kHubHalf - 2.2f);
                addShaft({ px * 0.80f, 8.6f, pz * 0.80f }, { px * 1.02f, 0.4f, pz * 1.02f },
                         1.10f, 0.065f);
            }
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
            x3::phys::Vec3 prev{ x0 + OX, kBeamY - kBeamHalfH + OY, z0 + OZ };
            for (uint32_t s3 = 1; s3 <= kCableSegs; ++s3) {
                const float t = (float)s3 / (float)kCableSegs;
                // Quadratic drape: max sag at the middle.
                const float y = kBeamY - kBeamHalfH - sag * 4.0f * t * (1.0f - t);
                x3::phys::Vec3 pt{ x0 + (x1 - x0) * t + OX, y + OY, z0 + (z1 - z0) * t + OZ };
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
            // Keep the DOORWAY's threshold clear: the machinery ring seats two
            // clusters ~3.6 m off the -Z wall, and one of them lands right in front
            // of the opening. A door you have to squeeze around is not a door.
            if (m_desc.doorway) {
                const float ddx = mx - m_desc.doorCenterX;
                const float ddz = mz - (-kHubHalf);
                if (ddx * ddx + ddz * ddz < 7.0f * 7.0f) continue;
            }
            const float bw = 0.9f + 1.4f * h01(0x66u);
            const float bh = 1.6f + 2.6f * h01(0x77u);
            const float bd = 0.7f + 1.1f * h01(0x88u);
            hallBox(mx, bh * 0.5f, mz, bw, bh * 0.5f, bd,
                    &sDark, kGunTint, 0.5f, /*collide=*/true, /*em=*/0.02f);
            // A smaller unit stacked on top + a vent pipe to break the box read.
            hallBox(mx + bw * 0.3f, bh + 0.45f, mz, bw * 0.45f, 0.45f, bd * 0.6f,
                    &sDark, kGunTint, 0.5f, /*collide=*/false, /*em=*/0.02f);
            hallBox(mx - bw * 0.5f, bh + 0.9f, mz, 0.09f, 0.9f, 0.09f,
                    &sDark, kGunTint, 0.5f, /*collide=*/false, /*em=*/0.02f);
        }
    }

    // ===== Portals (clockwise ring around the spawn) =====
    m_portals.clear();
    m_portals.reserve(kPortalCount);
    // Each portal authors: the gate (GLB groups, or 1 fallback torus) +
    // kPlateSegments floor wedges + kTrackSegs indicator segments, plus the
    // shared-mesh membrane entities. (The chevrons are GONE — round 7.)
    m_portalMeshes.reserve(kPortalCount * (1 + kPlateSegments + kTrackSegs + 2));

    const float twoPi = 6.2831853f;
    for (uint32_t i = 0; i < kPortalCount; ++i) {
        const PortalSpec& sp = kPortalTable[i];
        // Hub angle for portal i — clockwise starting at +X.
        const float hubAng = (float)i * (twoPi / (float)kPortalCount);
        const float lx = std::cos(hubAng) * kRingRadius;   // hub-LOCAL ring seat
        const float lz = std::sin(hubAng) * kRingRadius;
        const float cx = lx + OX;                          // WORLD gate center
        const float cz = lz + OZ;
        const float RY = kRingY + OY;                      // WORLD ring-center height

        RiftPortal p;
        p.worldName  = sp.worldName;
        p.triggerId  = sp.triggerId;
        p.worldPos   = x3::phys::Vec3{ cx, OY, cz };
        p.tint[0] = sp.tint[0]; p.tint[1] = sp.tint[1]; p.tint[2] = sp.tint[2];
        p.activated  = false;
        p.destination = sp.worldName;      // round 8: re-targetable via the console
        p.console.reset();

        // ---- Portal-local basis ---------------------------------------------
        // The "outward" axis (radial from hub center to portal center) is the
        // ring's NORMAL — the ring's plane is perpendicular to it, so the
        // portal's doorway face points back toward the hub center. Up is world
        // +Y. "Right" (along the ring's plane, horizontal) is up x outward.
        const float invR = 1.0f / kRingRadius;
        const float outwardX = lx * invR;   // radial from the HUB CENTER (region-safe)
        const float outwardZ = lz * invR;
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
        if (m_gateGlbActive) {
            // ---- ROUND 3: instance the Blender-authored gate (one entity per
            // material-group drawable, at portalXform * nodeTransform). The GLB's
            // local contract matches the membrane basis: XY gate plane, hole
            // along +Z(outward), FRONT (clamp caps / track bed) at -Z (hub-side).
            //
            // ================= ROUND 9: THE GATE WAS MIRRORED. =================
            // THIS is why the tube was black — not the albedo, and not the lights.
            //
            // right = (-outwardZ, 0, outwardX), up = (0,1,0), out = (outwardX, 0, outwardZ).
            // For portal 0 (out = +X) that basis is right=(0,0,1), up=(0,1,0), out=(1,0,0):
            //     | 0 0 1 |
            //     | 0 1 0 |  =  det -1
            //     | 1 0 0 |
            // A NEGATIVE determinant is a REFLECTION, not a rotation. Every gate GLB was
            // instanced through a MIRROR. That reverses triangle winding, so back-face
            // culling threw away the tube's OUTER shell and drew its INNER shell instead:
            // we were looking at the INSIDE of the tube, whose normals point AWAY from
            // every light in the room. Hence a perfect torus silhouette, correct albedo
            // and normal-map relief (texture lookups don't care about winding), coherent
            // specular — and NO diffuse, at ANY albedo, under ANY light. That is exactly
            // the black void ring Tim kept rejecting, and exactly why "5x the key light
            // barely moved it".
            //
            // PROOF (shots/r9): a GLB cube carrying the tube's EXACT material renders
            // blown-out white in this same room, and a 120-intensity probe light 3 m from
            // the tube blows out the floor while leaving the tube black. Only the mirror
            // explains both. (tools/build_rifthub_gate.py even logs "TUBE inside-out N/M"
            // — the mirror was being fought at export time instead of at the transform.)
            //
            // The fix is ONE SIGN. Negating local X makes the basis RIGHT-handed (det +1),
            // so the gate instances as a true rotation, winding is preserved, and the
            // OUTER shell — the machined metal — is what the player actually sees.
            const float locX[3] = { -rightX, 0.0f, -rightZ };
            const float locY[3] = { 0.0f,   1.0f, 0.0f    };
            const float locZ[3] = { outwardX, 0.0f, outwardZ };
            float gateXf[16];
            makeXform(gateXf, locX, locY, locZ, cx, RY, cz);
            for (size_t d = 0; d < m_gateDrawables.size(); ++d) {
                const x3::asset::ModelDrawable& dr = m_gateDrawables[d];
                const std::string& nm = (d < m_gateNames.size()) ? m_gateNames[d]
                                                                 : std::string();
                // Material-group -> surface set + tint. ROUND 4 lane 1: prefer
                // the SD-FORGED gate set (its mr.png carries the baked
                // rough >= 0.62 floor, so the forged PER-TEXEL MR rides the
                // reflection path safely); fall back to the round-2 curated set
                // + the 1x1 m_mrGate override when the forged set is absent
                // (fresh clone before LFS pull) — the ghost-glass fix holds on
                // BOTH paths.
                // ---- ROUND 8: the OPERATOR PANEL groups (gate_screen / gate_led).
                // These are the LCD and the LED strips / lit button caps set INTO
                // the tube's recessed bay. They are the ONLY emissive parts of the
                // gate (R8-B: "emissive confined to the screen + LEDs, never the
                // tube body") and tick() drives them off the console's instability.
                const bool isScreen = nm.find("screen") != std::string::npos;
                const bool isLed    = nm.find("led")    != std::string::npos;
                if (isScreen || isLed) {
                    Entity pe;
                    pe.mesh = x3::rhi::MeshHandle{ dr.meshId };
                    const float* col = isScreen ? kPanelScreenCalm : kPanelLedCalm;
                    // Near-black glass body: an UNLIT indicator must read as dark
                    // glass, never as coloured plastic (the round-2 law).
                    pe.baseColor[0] = col[0] * 0.06f; pe.baseColor[1] = col[1] * 0.06f;
                    pe.baseColor[2] = col[2] * 0.06f; pe.baseColor[3] = 1.0f;
                    pe.emissive[0] = col[0]; pe.emissive[1] = col[1]; pe.emissive[2] = col[2];
                    pe.emissive[3] = isScreen ? kPanelScreenEm : kPanelLedEm;
                    pe.tag = (uint32_t)Tag::Prop;
                    x3::asset::mulMat4(gateXf, dr.nodeTransform, pe.transform);
                    const uint32_t pid = scene.add(pe);
                    if (isScreen) p.panelScreenEnt = pid; else p.panelLedEnt = pid;
                    p.hasPanel = true;
                    continue;
                }
                const SurfaceSet* sf    = &sForgeDark;
                const SurfaceSet* fall  = &sDark;
                const float*      tint  = kForgeDarkTint;
                const float*      fallT = kGateDarkTint;
                uint32_t          mrIdx = 2;   // dark hardware override
                if (nm.find("patina") != std::string::npos) {
                    sf = &sForgePlate; fall = &sPlate;
                    tint = kForgePlateTint; fallT = kGatePlateTint; mrIdx = 0;
                } else if (nm.find("steel") != std::string::npos) {
                    sf = &sForgeTeal; fall = &sTrim;
                    tint = kForgeTealTint; fallT = kGateSteelTint; mrIdx = 1;
                }
                Entity e;
                e.mesh = x3::rhi::MeshHandle{ dr.meshId };
                if (sf->ok) {
                    e.tex = sf->albedo; e.normalTex = sf->normal; e.mrTex = sf->mr;
                } else if (fall->ok) {
                    tint = fallT;
                    e.tex = fall->albedo; e.normalTex = fall->normal;
                    e.mrTex = m_mrGate[mrIdx];   // weathered override (ghost-glass fix)
                }
                e.baseColor[0] = tint[0]; e.baseColor[1] = tint[1];
                e.baseColor[2] = tint[2]; e.baseColor[3] = 1.0f;
                // NO EMISSIVE. The gate is a hunk of weathered METAL: it is lit by
                // the hall rig + its own blue portal key, full stop.
                //
                // Rounds 3-5 propped it up with a self-emissive (first flat, then
                // albedo-texture-gated at kGateAmbient) because GLB meshes shaded
                // ~1/PI (metal: ~1/30) of the prims beside them — the mesh.frag
                // shading-path energy bug, fixed in this same commit. With the
                // engine honest that crutch DOUBLE-COUNTS: the gate blew out to a
                // flat white sculpture the moment real light landed on it (see
                // docs/screenshots/rifthub/J_1_gate_before_crutch_blowout.png).
                // Zero emissive on the gate BODY — glow belongs only to the
                // chevron slits / ratchet track, which are separate entities.
                e.emissive[0] = e.emissive[1] = e.emissive[2] = e.emissive[3] = 0.0f;
                e.tag = (uint32_t)Tag::Prop;
                x3::asset::mulMat4(gateXf, dr.nodeTransform, e.transform);
                scene.add(e);   // mesh owned by the LOADER — not m_portalMeshes
            }
            p.ringEntFirst = ringEntFirst;
            p.ringEntCount = (uint32_t)m_gateDrawables.size();
        } else {
            x3::prims::PrimMesh torus =
                x3::prims::makeTorus(kRingR, kRingTubeR, kRingMajorSeg, kRingMinorSeg);
            // Tile the metal set around the ring: u wraps the 12.9 m major
            // circumference ONCE by default (a smeared stretch) — rescale so a
            // tile lands roughly every 1.3 m major / 1.25 m minor.
            for (auto& v : torus.verts) { v.uv[0] *= 10.0f; v.uv[1] *= 2.0f; }
            // MIRROR (KNOWN_BUGS R3): [right, up, outward] with right=(-outZ,0,outX)
            // is det -1 — a REFLECTION. The fallback torus was instanced inside-out
            // and could not be lit. -right is the right-handed lateral (det +1); it
            // is what basisFromOutward() returns for this outward vector.
            const float locX[3] = { -rightX, 0.0f, -rightZ };  // ring "right" (det +1)
            const float locY[3] = { 0.0f,   1.0f, 0.0f    };   // world up
            const float locZ[3] = { outwardX, 0.0f, outwardZ };// outward (hole axis)
            AddedEntity ae = addOrientedEmissiveMesh(
                scene, device, torus,
                locX, locY, locZ,
                cx, RY, cz,
                kRingStone, /*emStrength=*/kRingEmissive, &sDark);
            m_portalMeshes.push_back(ae.mesh);
            p.ringEntFirst = ringEntFirst;
            p.ringEntCount = 1;   // one torus entity (was kRingSegments box segments)
        }

        // ---- Ring v2 OVER-PLATES + rivets (industrial armor over the torus) --
        // kPlateArcCount varied plates seated over the tube crest at jittered
        // angular slots + sizes (deterministic per portal+slot hash), skinned
        // from the curated sets, alternating patina/steel tints. Rivet studs
        // dot the hub-facing front face between chevrons.
        // ROUND 3: the authored GLB bakes far denser plate/bolt work — the
        // procedural armor only dresses the fallback torus.
        if (!m_gateGlbActive) {
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
                    cx + seatR * radX, RY + seatR * radY, cz + seatR * radZ,
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
                    RY + kRingR * radY,
                    cz + kRingR * radZ - outwardZ * proud,
                    &sDark, kGunTint, /*emStrength=*/0.03f);
                m_portalMeshes.push_back(ae.mesh);
            }
        }

        // ---- A-frame support CRADLE (the gate is INSTALLED, not floating) ----
        // Base skirt plinth under the ring bottom + two canted A-frame struts
        // grabbing the ring's sides + floor anchor plates at the strut feet.
        // ROUND 3: the authored GLB carries its own plinth + angled shoulder
        // skirt + foot pads, so the box cradle only dresses the fallback ring.
        if (!m_gateGlbActive) {
            // MIRROR (KNOWN_BUGS R3): -right, not right. det +1. The skirt plinth and
            // the floor anchor plates were instanced through a reflection.
            const float locX[3] = { -rightX, 0.0f, -rightZ };
            const float locY[3] = { 0.0f,   1.0f, 0.0f    };
            const float locZ[3] = { outwardX, 0.0f, outwardZ };
            // Skirt plinth (top at 0.50 m — below the 0.55 m ring opening).
            AddedEntity skirt = addOrientedSurfBox(
                scene, device, kSkirtHalfTan, kSkirtHalfY, kSkirtHalfDep,
                locX, locY, locZ,
                cx, kSkirtHalfY + OY, cz,
                &sDark, kGunTint, /*emStrength=*/0.04f, /*uvScale=*/0.6f);
            m_portalMeshes.push_back(skirt.mesh);
            for (int side = -1; side <= 1; side += 2) {
                const float bx = cx + rightX * kStrutBaseOut * (float)side;
                const float bz = cz + rightZ * kStrutBaseOut * (float)side;
                const float tx = cx + rightX * kStrutTopOut * (float)side;
                const float tz = cz + rightZ * kStrutTopOut * (float)side;
                // Canted strut blade: thickness along the gate's depth axis.
                x3::prims::PrimMesh strut = x3::prims::makeCantedStrut(
                    bx, OY, bz, tx, kStrutTopY + OY, tz,
                    kStrutHalfW, kStrutHalfT, outwardX, outwardZ, /*uvScale=*/0.5f);
                const float ident[3][3] = { {1,0,0}, {0,1,0}, {0,0,1} };
                AddedEntity ae = addOrientedEmissiveMesh(
                    scene, device, strut,
                    ident[0], ident[1], ident[2], 0.0f, 0.0f, 0.0f,
                    kGunTint, /*emStrength=*/0.04f, &sDark);
                m_portalMeshes.push_back(ae.mesh);
                // Floor anchor plate at the strut foot.
                AddedEntity anchor = addOrientedSurfBox(
                    scene, device, kAnchorHalfTan, kAnchorHalfY, kAnchorHalfDep,
                    locX, locY, locZ,
                    bx, kAnchorHalfY + OY, bz,
                    &sTrim, kSteelTint, /*emStrength=*/0.04f);
                m_portalMeshes.push_back(anchor.mesh);
            }
        }

        // ---- ORANGE conduits + coil rings + TEAL holo screens (phase D) ------
        // (The dressing's portal-local dX/dY/dZ trio is DELETED: it was dead code —
        // nothing had referenced it since the chevrons were cut — and it was one more
        // copy of the det -1 [right, up, outward] MIRROR idiom (KNOWN_BUGS R3) sitting
        // in the file waiting to be copy-pasted. Anything that needs a basis off an
        // outward vector calls basisFromOutward() in app/basis.h.)
        // World point in the gate plane, nudged hub-side of the ring face.
        auto gatePt = [&](float alongRight, float y, float alongOut) {
            return x3::phys::Vec3{
                cx + rightX * alongRight + outwardX * alongOut,
                y + OY,   // hub-LOCAL height in, WORLD out (W-RIFT region origin)
                cz + rightZ * alongRight + outwardZ * alongOut };
        };
        // Conduit runs: gate -> riser -> floor -> skirt, one per side (the
        // right side rides higher, the left lower — no clone read). ROUND 2:
        // pipe BODIES + bend COLLARS are authored first (static gunmetal PBR
        // geometry, not emissive); the thin amber CORE LINES follow as one
        // contiguous span so tick() can phase the flow pulse ALONG the run.
        {
            // ROUND 3 reroute: the GLB gate is WIDER (rim to ~2.62, pipes 2.7+)
            // — the round-2 runs at +/-2.46 floated as a detached orange frame
            // IN FRONT of the plates (F_1 close shot). The runs now hug the
            // lower FLANKS outside the rim (the reference's glowing hairpins),
            // dropping to the deck and feeding the base skirt.
            // F_2 lesson: runs that climb past the gate's midline read as an
            // orange SCAFFOLD FRAME around the ring. The reference conduits are
            // tight lower-flank HAIRPINS — so the runs stay below y~1.5.
            const float off = -0.35f;   // just hub-side of the gate midplane
            const x3::phys::Vec3 runA[5] = {
                gatePt( 3.30f, 1.55f, off), gatePt( 3.62f, 1.15f, off),
                gatePt( 3.62f, 0.45f, off), gatePt( 2.95f, 0.16f, off),
                gatePt( 1.40f, 0.16f, off),
            };
            const x3::phys::Vec3 runB[5] = {
                gatePt(-3.25f, 1.35f, off), gatePt(-3.55f, 1.00f, off),
                gatePt(-3.55f, 0.40f, off), gatePt(-2.80f, 0.16f, off),
                gatePt(-1.35f, 0.16f, off),
            };
            // Orthonormal basis for a pipe segment a->b: u/v span the cross-
            // section, d runs along the pipe. Mirrors beamXform's frame math.
            auto segBasis = [](const x3::phys::Vec3& a, const x3::phys::Vec3& b,
                               float u[3], float v[3], float d[3]) -> float {
                float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
                const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
                const float inv = (len > 1e-6f) ? 1.0f / len : 0.0f;
                d[0] = dx * inv; d[1] = dy * inv; d[2] = dz * inv;
                // ref = world up unless the pipe is near-vertical (then +X).
                float rx = 0.0f, ry = 1.0f, rz = 0.0f;
                if (std::fabs(d[1]) >= 0.99f) { rx = 1.0f; ry = 0.0f; }
                // u = normalize(ref x d), v = d x u.
                u[0] = ry * d[2] - rz * d[1];
                u[1] = rz * d[0] - rx * d[2];
                u[2] = rx * d[1] - ry * d[0];
                const float ul = std::sqrt(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
                const float uinv = (ul > 1e-6f) ? 1.0f / ul : 0.0f;
                u[0] *= uinv; u[1] *= uinv; u[2] *= uinv;
                v[0] = d[1] * u[2] - d[2] * u[1];
                v[1] = d[2] * u[0] - d[0] * u[2];
                v[2] = d[0] * u[1] - d[1] * u[0];
                return len;
            };
            auto addPipeRun = [&](const x3::phys::Vec3* run) {
                for (int s3 = 0; s3 < 4; ++s3) {
                    float u[3], v[3], d[3];
                    const float len = segBasis(run[s3], run[s3 + 1], u, v, d);
                    const float mx = (run[s3].x + run[s3 + 1].x) * 0.5f;
                    const float my = (run[s3].y + run[s3 + 1].y) * 0.5f;
                    const float mz = (run[s3].z + run[s3 + 1].z) * 0.5f;
                    // Pipe body + its 45-deg twin (octagonal cross-section read).
                    AddedEntity b0 = addOrientedSurfBox(
                        scene, device, kPipeHalf, kPipeHalf,
                        len * 0.5f + kPipeHalf * 0.4f,
                        u, v, d, mx, my, mz, &sDark, kPipeTint,
                        /*emStrength=*/0.0f, /*uvScale=*/0.8f);
                    m_portalMeshes.push_back(b0.mesh);
                    const float r2 = 0.7071068f;
                    const float u45[3] = { (u[0] + v[0]) * r2, (u[1] + v[1]) * r2,
                                           (u[2] + v[2]) * r2 };
                    const float v45[3] = { (v[0] - u[0]) * r2, (v[1] - u[1]) * r2,
                                           (v[2] - u[2]) * r2 };
                    AddedEntity b1 = addOrientedSurfBox(
                        scene, device, kPipeHalf * 0.92f, kPipeHalf * 0.92f,
                        len * 0.5f,
                        u45, v45, d, mx, my, mz, &sDark, kPipeTint,
                        /*emStrength=*/0.0f, /*uvScale=*/0.8f);
                    m_portalMeshes.push_back(b1.mesh);
                }
                // Steel collars at every waypoint (run ends + the bends).
                for (int w = 0; w < 5; ++w) {
                    const int sref = (w < 4) ? w : 3;
                    float u[3], v[3], d[3];
                    segBasis(run[sref], run[sref + 1], u, v, d);
                    AddedEntity col = addOrientedSurfBox(
                        scene, device, kCollarHalf, kCollarHalf, kCollarHalfDep,
                        u, v, d, run[w].x, run[w].y, run[w].z,
                        &sTrim, kSteelTint, /*emStrength=*/0.0f, /*uvScale=*/1.0f);
                    m_portalMeshes.push_back(col.mesh);
                }
            };
            addPipeRun(runA);
            addPipeRun(runB);
            // Thin amber CORE LINES riding the pipe's hub-facing surface (the
            // animated span — proud of the body so the glow strip reads).
            p.conduitEntFirst = scene.size();
            const float coreOut = kPipeHalf + kConduitHalf * 0.5f;
            auto corePt = [&](const x3::phys::Vec3& w) {
                return x3::phys::Vec3{ w.x - outwardX * coreOut, w.y,
                                       w.z - outwardZ * coreOut };
            };
            auto addCoreRun = [&](const x3::phys::Vec3* run) {
                for (int s3 = 0; s3 < 4; ++s3) {
                    Entity e;
                    e.mesh = m_fxBeamMesh;   // SHARED unit box
                    e.baseColor[0] = kConduitDark[0]; e.baseColor[1] = kConduitDark[1];
                    e.baseColor[2] = kConduitDark[2]; e.baseColor[3] = 1.0f;
                    e.emissive[0] = kConduitOrange[0]; e.emissive[1] = kConduitOrange[1];
                    e.emissive[2] = kConduitOrange[2]; e.emissive[3] = kConduitEmBase;
                    e.tag = (uint32_t)Tag::Prop;
                    beamXform(e.transform, corePt(run[s3]), corePt(run[s3 + 1]),
                              kConduitHalf);
                    scene.add(e);
                }
            };
            addCoreRun(runA);
            addCoreRun(runB);
        }
        p.conduitEntCount = 8;
        // Coil rings on the right riser (warm glow on a DARK body — round 2:
        // the coil no longer reads as solid orange plastic when dim). ROUND 3:
        // moved onto the rerouted flank riser.
        for (int coil = 0; coil < 2; ++coil) {
            x3::prims::PrimMesh torus = x3::prims::makeTorus(0.14f, 0.032f, 20, 8);
            // Ring horizontal around the vertical riser: local X = gate right,
            // local Y = outward, local Z (hole axis) = world up.
            const float locX3[3] = { rightX, 0.0f, rightZ };
            const float locY3[3] = { outwardX, 0.0f, outwardZ };
            const float locZ3[3] = { 0.0f, 1.0f, 0.0f };
            const x3::phys::Vec3 at = gatePt(3.62f, 0.95f - 0.30f * (float)coil, -0.35f);
            Entity e;
            e.mesh = device.createMesh(torus.verts.data(), (uint32_t)torus.verts.size(),
                                       torus.index.data(), (uint32_t)torus.index.size());
            m_portalMeshes.push_back(e.mesh);
            e.baseColor[0] = kConduitDark[0]; e.baseColor[1] = kConduitDark[1];
            e.baseColor[2] = kConduitDark[2]; e.baseColor[3] = 1.0f;
            e.emissive[0] = kConduitOrange[0]; e.emissive[1] = kConduitOrange[1];
            e.emissive[2] = kConduitOrange[2]; e.emissive[3] = kCoilOrangeEm;
            e.tag = (uint32_t)Tag::Prop;
            makeXform(e.transform, locX3, locY3, locZ3, at.x, at.y, at.z);
            scene.add(e);
        }
        // ---- THE FLOATING TEAL SIGNS ARE DELETED (round 8-C) ------------------
        // Two flat teal rectangles used to hover either side of the approach. The
        // owner's call: they go, and a real HOLOTERMINAL takes their place — the
        // project's canonical holo language (BLACK GLASS slab, glowing blue/green
        // text, a shiny metallic ROUND-PIPE frame around the glass, and a single
        // support pipe running to the ceiling so it HANGS rather than floats).
        // That object already exists as a platform (app/holo_terminal.*, the cell
        // terminal), so the hub REUSES it instead of reinventing a sign: one per
        // rift, standing in the approach, reading out where this portal goes — and
        // it is the thing the player presses [E] on to open the control surface.
        //
        // (Authored after the loop, once every portal's basis is known — see the
        //  holoterminal block below the portal loop.)

        // ---- Octagonal floor plate (8 wedge boxes; ROUND 2 industrial deck) --
        // Each wedge is a thin box tangent to a circle of radius kPlateRingR,
        // axis-aligned in Y (flat on the ground) — DARK textured metal deck
        // now, not identity-colored plastic. The identity accent is the thin
        // desaturated trim ring authored right after the wedges.
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
            AddedEntity ae = addOrientedSurfBox(
                scene, device,
                plateHalfTangent(), kPlateHalfY, kPlateBoxThick,
                locX, locY, locZ,
                wcx, wcy, wcz,
                &sGrate, kPlateDeckTint, /*emStrength=*/0.02f, /*uvScale=*/0.7f);
            m_portalMeshes.push_back(ae.mesh);
        }
        // Identity TRIM RING: a thin flat emissive annulus at the deck's outer
        // edge — the ONLY identity-colored element, desaturated ~50% so it
        // reads as a powered marker light, not colored plastic.
        {
            float trim[3];
            const float luma = 0.299f * sp.tint[0] + 0.587f * sp.tint[1]
                             + 0.114f * sp.tint[2];
            for (int c3 = 0; c3 < 3; ++c3)
                trim[c3] = sp.tint[c3] + (luma - sp.tint[c3]) * kTrimDesat;
            x3::prims::PrimMesh ring =
                x3::prims::makeRing(kTrimRingInnerR, kTrimRingOuterR, 64);
            Entity e;
            e.mesh = device.createMesh(ring.verts.data(), (uint32_t)ring.verts.size(),
                                       ring.index.data(), (uint32_t)ring.index.size());
            m_portalMeshes.push_back(e.mesh);
            e.baseColor[0] = trim[0] * 0.15f; e.baseColor[1] = trim[1] * 0.15f;
            e.baseColor[2] = trim[2] * 0.15f; e.baseColor[3] = 1.0f;
            e.emissive[0] = trim[0]; e.emissive[1] = trim[1]; e.emissive[2] = trim[2];
            e.emissive[3] = kTrimRingEm;
            e.tag = (uint32_t)Tag::Prop;
            const float ident3[3][3] = { {1,0,0}, {0,1,0}, {0,0,1} };
            makeXform(e.transform, ident3[0], ident3[1], ident3[2],
                      cx, kPlateHalfY * 2.0f + 0.004f, cz);
            scene.add(e);
        }

        // ---- CHEVRONS: GONE (round 7 addendum 2 — "No chevrons needed") -------
        // Nine clamp housings + nine amber slit cores used to be authored here (and
        // nine more box housings for the fallback ring). All deleted. The gate is
        // ONE TUBE; its detail is cut INTO the tube and carried by the forged
        // normal/height maps. Nothing is bolted to its face any more.


        // ---- The recessed INDICATOR LINE (inner front edge; the video's
        // activation-feedback detail). ROUND 9: this used to be 48 amber DASHES —
        // a caution-tape ring. It is now one CONTINUOUS, thin, dim, cool trim line
        // seated in the machined groove. Contiguous span for tick()'s chase.
        p.trackEntFirst = scene.size();
        for (uint32_t t3 = 0; t3 < kTrackSegs; ++t3) {
            const float th = (float)t3 * (twoPi / (float)kTrackSegs);
            const float ct = std::cos(th), st = std::sin(th);
            const float radX = ct * rightX, radY = st, radZ = ct * rightZ;
            const float locX[3] = { -st * rightX, ct, -st * rightZ };
            const float locY[3] = { radX, radY, radZ };
            const float locZ[3] = { outwardX, 0.0f, outwardZ };
            // ROUND 8: seat the segment INSIDE the groove machined into the tube
            // (a recessed indicator slit — never a proud amber pip).
            const float proud = kTrackProud;
            AddedEntity ae = addOrientedEmissiveBox(
                scene, device, kTrackHalfTan, kTrackHalfRad, kTrackHalfDep,
                locX, locY, locZ,
                cx + kTrackR * radX - outwardX * proud,
                RY + kTrackR * radY,
                cz + kTrackR * radZ - outwardZ * proud,
                kTrimCool, /*emStrength=*/kTrackEmIdle, kChevSlitDark);
            m_portalMeshes.push_back(ae.mesh);
        }
        p.trackEntCount = kTrackSegs;

        // ---- (ROUND 6) NO CORE DISKS. The two bright blue-white disks that used
        //      to float at the exact ring center are DELETED — they were a v1
        //      procedural leftover painted on top of the reference footage, and
        //      they are what the owner saw: "Why the dot in the middle?" The
        //      membrane's own frames carry its center. The gate's blue POINT
        //      LIGHT (below) is untouched: light in the room, not a sprite.

        // ---- Event-horizon membrane v3 (the DEEP-BLUE PLASMA STORM) -----------
        // TWO entities on SHARED meshes/textures, authored contiguously
        // (plasma, rim — the span tick() animates).
        //
        // THE VISTA LAYER IS GONE (round-5 bug 1+2 root cause). It was an OPAQUE
        // disk of the same radius parked kVistaDepth OUTWARD of the plasma, and
        // opaque geometry does not "show through" the storm:
        //   * from the HUB side the plasma disk (opaque, same radius) occluded it
        //     completely — it never contributed a single pixel of parallax;
        //   * from the OUTWARD side it was the NEAR surface — a near-black
        //     starscape (albedo 0.2, emissive 0.42 idle / 0.08 once the gate
        //     OPENed) that occluded the plasma. That is the owner's "activated
        //     portal goes BLACK, only the rim ring remains": he walks THROUGH the
        //     gate (the trigger fires 2.5 m out), ends up on the far side, and is
        //     looking at the dead vista disk. Same defect = "no swirl from behind".
        // The membrane disk mesh is already double-wound (makeMembraneDisk emits
        // both windings) and emissive is view-independent, so with the vista gone
        // the ONE plasma disk reads identically from BOTH sides. A true
        // see-through vista needs a render-to-texture portal view, not an opaque
        // disk behind an opaque disk (noted in the art target as the future
        // upgrade).
        p.rightX = rightX; p.rightZ = rightZ;
        p.outX   = outwardX; p.outZ = outwardZ;
        const uint32_t membraneEntFirst = scene.size();
        // MIRROR (KNOWN_BUGS R3): -right, not right — det +1. The plasma disk, the
        // Fresnel contact rim and the two falloff shells were all instanced through a
        // reflection. (They are emissive, so the mirror did not black them out the way
        // it blacked out the gate's metal — but a reflected disk presents its BACK face
        // to the hub, and the rim/shell tori were being drawn inside-out.)
        const float locX[3] = { -rightX, 0.0f, -rightZ };
        const float locY[3] = { 0.0f,   1.0f, 0.0f    };
        const float locZ[3] = { outwardX, 0.0f, outwardZ };
        // [0] PLASMA disk (filament emissive map; mrTex forces the PBR route so
        //     the emissive texture is honoured; deep-blue tint, capped strength).
        //     ROUND 4: with the flipbook loaded, the IDLE layer IS the baked
        //     reference-video frame (tick() advances it); the procedural nebula
        //     is the fallback when the atlas is absent.
        {
            const x3::rhi::TextureHandle idleTex =
                m_flipTex.empty() ? m_plasmaTex : m_flipTex[0];
            Entity e;
            e.mesh = m_diskMesh;
            e.tex  = idleTex;                     // albedo: the same web, dim under no light
            e.mrTex = m_mrFlat;
            e.emissiveTex = idleTex;
            e.baseColor[0] = 0.10f; e.baseColor[1] = 0.14f; e.baseColor[2] = 0.30f;
            e.baseColor[3] = 1.0f;
            e.emissive[0] = kPlasmaBlue[0]; e.emissive[1] = kPlasmaBlue[1];
            e.emissive[2] = kPlasmaBlue[2]; e.emissive[3] = kPlasmaEmBase;
            e.tag = (uint32_t)Tag::Prop;
            makeXform(e.transform, locX, locY, locZ, cx, RY, cz);
            scene.add(e);
        }
        // [1] FRESNEL CONTACT ring (the one hot line — thin, blue, shimmer in
        //     tick(), brightest exactly where the membrane meets the ring).
        {
            Entity e;
            e.mesh = m_rimMesh;
            e.baseColor[0] = 0.08f; e.baseColor[1] = 0.14f; e.baseColor[2] = 0.28f;
            e.baseColor[3] = 1.0f;
            e.emissive[0] = kRimBlue[0]; e.emissive[1] = kRimBlue[1];
            e.emissive[2] = kRimBlue[2]; e.emissive[3] = kRimEmBase;
            e.tag = (uint32_t)Tag::Prop;
            makeXform(e.transform, locX, locY, locZ, cx, RY, cz);
            scene.add(e);
        }
        p.membraneEntFirst = membraneEntFirst;
        p.membraneEntCount = 2;   // [0] plasma, [1] rim (the vista layer is gone)
        // FALLOFF shells (static, outside the animated span): two dimmer +
        // thinner rings stepped hub-side of the contact line, so the rim glow
        // visibly decays away from the membrane instead of reading neon.
        {
            const struct { float R, tube, off, em; } fall[2] = {
                { kRimFallRA, kRimFallTubeA, kRimFallOffA, kRimFallEmA },
                { kRimFallRB, kRimFallTubeB, kRimFallOffB, kRimFallEmB },
            };
            for (int f = 0; f < 2; ++f) {
                x3::prims::PrimMesh torus =
                    x3::prims::makeTorus(fall[f].R, fall[f].tube, 48, 8);
                Entity e;
                e.mesh = device.createMesh(torus.verts.data(), (uint32_t)torus.verts.size(),
                                           torus.index.data(), (uint32_t)torus.index.size());
                m_portalMeshes.push_back(e.mesh);
                e.baseColor[0] = 0.05f; e.baseColor[1] = 0.09f; e.baseColor[2] = 0.18f;
                e.baseColor[3] = 1.0f;
                e.emissive[0] = kRimBlue[0]; e.emissive[1] = kRimBlue[1];
                e.emissive[2] = kRimBlue[2]; e.emissive[3] = fall[f].em;
                e.tag = (uint32_t)Tag::Prop;
                makeXform(e.transform, locX, locY, locZ,
                          cx - outwardX * fall[f].off, RY,
                          cz - outwardZ * fall[f].off);
                scene.add(e);
            }
        }
        p.throatOn = false;

        m_portals.push_back(p);

        // Trigger volume: wider than the ring so the player only needs to
        // step into the plate area (not thread the ring) to fire the rift.
        const x3::phys::Vec3 tmin{ cx - kTrigHalfXZ, OY - kTrigHalfY, cz - kTrigHalfXZ };
        const x3::phys::Vec3 tmax{ cx + kTrigHalfXZ, OY + kTrigHalfY, cz + kTrigHalfXZ };
        triggers.add(tmin, tmax, sp.triggerId, /*enabled=*/true);
    }

    // ===== ROUND 8-C: THE HANGING HOLOTERMINAL CONSOLES ========================
    // One per rift, standing in the approach ~4.3 m hub-side of the gate. This is
    // the project's CANONICAL holo object, reused wholesale from the platform that
    // already ships it (app/holo_terminal.*): a BLACK GLASS slab with a shiny
    // metallic ROUND-PIPE frame and a single support pipe running up to the
    // ceiling, so it HANGS rather than floats. The readout is baked ON the glass
    // (it tilts with the panel; it is not a camera-facing overlay), in the blue/green
    // ink the holo language calls for.
    //
    // The glass says WHERE THIS PORTAL GOES. Press [E] and it becomes the control
    // surface (updateConsole()).
    m_holos.clear();
    m_holos.resize(m_portals.size());
    for (uint32_t i = 0; i < m_portals.size(); ++i) {
        const RiftPortal& p = m_portals[i];
        // Face the hub center: the terminal sits between the player and the gate,
        // its glass turned back toward the approach.
        const float hx = p.worldPos.x - p.outX * kConsoleStandoff + p.rightX * kConsoleSideOff;
        const float hz = p.worldPos.z - p.outZ * kConsoleStandoff + p.rightZ * kConsoleSideOff;
        // Yaw so the panel's front face looks back along -outward, i.e. at the player
        // walking up from the hub center. (NOTE: this is the OPPOSITE facing to the
        // detention-cell terminal, which is built at yaw=PI. That difference is what
        // used to blank these screens — the platform had two panes offset along LOCAL
        // +Z which, at THIS facing, landed in front of the glass and depth-occluded
        // the readout. Those panes are gone; see holo_terminal.cpp build().)
        const float yaw = std::atan2(-p.outX, -p.outZ);
        m_holos[i].build(scene, device, x3::phys::Vec3{ hx, kConsoleY + OY, hz }, yaw,
                         kConsoleW, kConsoleH, /*ceilingY=*/kHallWallH + OY);
        // TEXT-FIRST layout: this glass exists to be READ (where the portal goes, and
        // five live parameter rows), so the cell terminal's center schematic gives way
        // to type at roughly twice the size.
        m_holos[i].setLayout(HoloTerminal::Layout::Readout);
        // ROUND 9 — THE CYAN IS GONE. This used to call
        //     setTextColor(0.42f, 1.0f, 0.78f)
        // which is a MINT/CYAN (g > b), the one ink the canon explicitly bans
        // ("BLUE, NOT CYAN" — DECISIONS.md, Tim's own words). Worse, setTextColor
        // sets m_inkOverride, which FLATTENS every body row to that single colour and
        // switches OFF the keyword-driven statusInk() palette entirely — so the
        // console lost its blue/green/orange status language as well as its canon hue.
        // Saying nothing is correct: the default path bakes blue-white structure with
        // GREEN for OPEN/STABLE and ORANGE for FAIL/LOCKED, which IS the canon palette.
        m_holos[i].setLines(consoleReadout(i));
    }

    // ===== Per-portal blue CORE lights (cast the event horizon onto the stone) =====
    m_lights.clear();
    m_lights.reserve(m_portals.size() + 5);
    for (const auto& p : m_portals) {
        x3::rhi::PointLight L;
        // The CORE light stays exactly where it has always been: at the gate centre,
        // in the gate plane. It owns the bore, the floor pool and the membrane read —
        // all of which already look right, and none of which the tube fix may disturb.
        // (Tried and reverted: pushing this light hub-side to rake the tube. It dims
        // the bore, which is one of the good things, and it does NOT fix the tube —
        // see the kGateLowI note, which is where the tube's light actually comes from.)
        L.pos[0] = p.worldPos.x;
        L.pos[1] = kRingY + OY;
        L.pos[2] = p.worldPos.z;
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
        // 3x3 overhead grid (was 5 lights): even coverage so the deck, beams and
        // columns all catch something no matter where the player stands.
        for (int gz = -1; gz <= 1; ++gz) {
            for (int gx = -1; gx <= 1; ++gx) {
                x3::rhi::PointLight L;
                L.pos[0] = (float)gx * 12.0f + OX; L.pos[1] = 6.8f + OY;
                L.pos[2] = (float)gz * 12.0f + OZ;
                L.range  = kHallLightRange;
                L.color[0] = kHallLightColor[0] * kHallLightI;
                L.color[1] = kHallLightColor[1] * kHallLightI;
                L.color[2] = kHallLightColor[2] * kHallLightI;
                m_lights.push_back(L);
            }
        }
        // Wall-wash accents: one warm dim light over each perimeter machinery
        // cluster (same deterministic angles as the silhouette pass) so the
        // shapes READ against the wall instead of vanishing into black.
        for (uint32_t mc = 0; mc < 8; ++mc) {
            const float ang = (float)mc * (6.2831853f / 8.0f) + 0.39f;
            x3::rhi::PointLight L;
            L.pos[0] = std::cos(ang) * (kHubHalf - 3.4f) + OX;
            L.pos[1] = 4.6f + OY;
            L.pos[2] = std::sin(ang) * (kHubHalf - 3.4f) + OZ;
            L.range  = kWashRange;
            L.color[0] = kWashColor[0] * kWashI;
            L.color[1] = kWashColor[1] * kWashI;
            L.color[2] = kWashColor[2] * kWashI;
            m_lights.push_back(L);
        }
        // Mid-floor DECK fills: 8 low cool lights ringing the hub floor between
        // the gate bays (round 3 had 4 at 2.4, round 4 nudged them to 3.2 and
        // the owner still saw NOTHING). 8 at 9.2 with a 14 m reach: the wet
        // deck, the reflections and the machinery feet now READ.
        for (uint32_t f = 0; f < kDeckFills; ++f) {
            const float ang = ((float)f + 0.5f) * (6.2831853f / (float)kDeckFills);
            x3::rhi::PointLight L;
            L.pos[0] = std::cos(ang) * 7.5f + OX;
            L.pos[1] = 2.6f + OY;
            L.pos[2] = std::sin(ang) * 7.5f + OZ;
            L.range  = kDeckRange;
            L.color[0] = kDeckColor[0] * kDeckI;
            L.color[1] = kDeckColor[1] * kDeckI;
            L.color[2] = kDeckColor[2] * kDeckI;
            m_lights.push_back(L);
        }
        // Per-gate KEY lights: above + hub-side of each gate, aimed at the ring
        // face. The reference's ring is carved by a hard overhead key; ours had
        // only its own blue core light (which lights the membrane's own plane,
        // so the plates got nothing). This is the highlight/shadow separation.
        for (const auto& p : m_portals) {
            const float ux = p.outX;   // outward unit (XZ) — region-safe
            const float uz = p.outZ;
            x3::rhi::PointLight L;
            L.pos[0] = p.worldPos.x - ux * kGateKeyHubOff;
            L.pos[1] = kGateKeyUp + OY;
            L.pos[2] = p.worldPos.z - uz * kGateKeyHubOff;
            L.range  = kGateKeyRange;
            L.color[0] = kGateKeyColor[0] * kGateKeyI;
            L.color[1] = kGateKeyColor[1] * kGateKeyI;
            L.color[2] = kGateKeyColor[2] * kGateKeyI;
            m_lights.push_back(L);
            // Warm under-fill: lifts the lower plates + the cradle out of black so
            // the gate reads as a whole machine, not a lit crown on a void.
            x3::rhi::PointLight W;
            W.pos[0] = p.worldPos.x - ux * kGateFillHubOff;
            W.pos[1] = kGateFillUp + OY;
            W.pos[2] = p.worldPos.z - uz * kGateFillHubOff;
            W.range  = kGateFillRange;
            W.color[0] = kGateFillColor[0] * kGateFillI;
            W.color[1] = kGateFillColor[1] * kGateFillI;
            W.color[2] = kGateFillColor[2] * kGateFillI;
            m_lights.push_back(W);
            // LOW KEY: rakes the tube's underside so the body reads as a whole machine
            // instead of a lit crown sitting on a black belly.
            x3::rhi::PointLight LO;
            LO.pos[0] = p.worldPos.x - ux * kGateLowHubOff;
            LO.pos[1] = kGateLowUp + OY;
            LO.pos[2] = p.worldPos.z - uz * kGateLowHubOff;
            LO.range  = kGateLowRange;
            LO.color[0] = kGateKeyColor[0] * kGateLowI;
            LO.color[1] = kGateKeyColor[1] * kGateLowI;
            LO.color[2] = kGateKeyColor[2] * kGateLowI;
            m_lights.push_back(LO);
        }
    }

    // ===== THE OPS STATION (owner, 2026-08-30) ==============================
    // A seated operator desk on the SOUTH-WEST side of the ring (clear of the
    // -Z doorway at doorCenterX=+7), facing the gates. Authored in the hub's
    // own holo language: dark steel base, angled black-glass top with a low
    // teal standby glow, two hanging side panes, and a fixed chair. The desk
    // base COLLIDES (containment); the chair does not (walking into the seat
    // must never wedge the player — sitting is the [E] interaction).
    {
        const float kOpsX = -7.0f, kOpsZ = -15.4f;         // hub-local, inside the shell
        const float ax[3] = { 1, 0, 0 }, ay[3] = { 0, 1, 0 }, az[3] = { 0, 0, 1 };
        const float steel[3]  = { 0.05f, 0.055f, 0.06f };  // near-black steel body
        const float glassT[3] = { 0.10f, 0.55f, 0.60f };   // hub teal, standby glow
        const float amber[3]  = { 0.85f, 0.55f, 0.18f };   // seat accent
        // Desk base: 1.9 x 0.9 x 0.55 m steel pedestal block, front edge at
        // kOpsZ (operator side is -Z of it).
        {
            x3::prims::PrimMesh b = x3::prims::makeBox(0.95f, 0.45f, 0.28f,
                                                       0.0f, 0.0f, 0.0f);
            Entity e;
            e.mesh = device.createMesh(b.verts.data(), (uint32_t)b.verts.size(),
                                       b.index.data(), (uint32_t)b.index.size());
            e.baseColor[0] = steel[0]; e.baseColor[1] = steel[1];
            e.baseColor[2] = steel[2]; e.baseColor[3] = 1.0f;
            e.tag = (uint32_t)Tag::Prop;
            makeXform(e.transform, ax, ay, az, OX + kOpsX, OY + 0.45f, OZ + kOpsZ);
            scene.add(e);
            m_portalMeshes.push_back(e.mesh);
            // Collision: a static box where the desk stands (world-space verts).
            const float cx = OX + kOpsX, cy = OY, cz = OZ + kOpsZ;
            const float hx = 0.95f, hz = 0.28f, hy = 0.9f;
            const float cv[24] = {
                cx-hx, cy,    cz-hz,  cx+hx, cy,    cz-hz,
                cx+hx, cy,    cz+hz,  cx-hx, cy,    cz+hz,
                cx-hx, cy+hy, cz-hz,  cx+hx, cy+hy, cz-hz,
                cx+hx, cy+hy, cz+hz,  cx-hx, cy+hy, cz+hz };
            const uint32_t ci[36] = { 0,1,2, 0,2,3,  4,6,5, 4,7,6,
                                      0,4,5, 0,5,1,  1,5,6, 1,6,2,
                                      2,6,7, 2,7,3,  3,7,4, 3,4,0 };
            physics.addStaticMesh(cv, 8, ci, 36);
        }
        // Angled glass top: thin slab pitched ~32 deg toward the operator.
        {
            const float c = std::cos(0.56f), s = std::sin(0.56f);
            const float gy[3] = { 0.0f,  c,  s };     // tilted up-vector
            const float gz[3] = { 0.0f, -s,  c };     // tilted normal (faces operator/up)
            AddedEntity g = addOrientedEmissiveBox(scene, device,
                0.92f, 0.02f, 0.34f, ax, gy, gz,
                OX + kOpsX, OY + 1.02f, OZ + kOpsZ - 0.02f,
                glassT, 0.55f, steel);
            m_portalMeshes.push_back(g.mesh);
        }
        // Two hanging side panes (the holoterminal silhouette, smaller).
        for (int sside = -1; sside <= 1; sside += 2) {
            AddedEntity pn = addOrientedEmissiveBox(scene, device,
                0.02f, 0.26f, 0.34f, ax, ay, az,
                OX + kOpsX + (float)sside * 1.25f, OY + 1.55f, OZ + kOpsZ + 0.10f,
                glassT, 0.35f, steel);
            m_portalMeshes.push_back(pn.mesh);
            AddedEntity pipe = addOrientedEmissiveBox(scene, device,
                0.03f, (kHallWallH - 1.8f) * 0.5f, 0.03f, ax, ay, az,
                OX + kOpsX + (float)sside * 1.25f,
                OY + 1.8f + (kHallWallH - 1.8f) * 0.5f, OZ + kOpsZ + 0.10f,
                steel, 0.0f, steel);
            m_portalMeshes.push_back(pipe.mesh);
        }
        // The chair: pedestal + cushion + low back, amber piping on the seat lip.
        const float seatZ = kOpsZ - 1.05f;
        {
            AddedEntity ped = addOrientedEmissiveBox(scene, device,
                0.10f, 0.22f, 0.10f, ax, ay, az,
                OX + kOpsX, OY + 0.22f, OZ + seatZ, steel, 0.0f, steel);
            m_portalMeshes.push_back(ped.mesh);
            AddedEntity cush = addOrientedEmissiveBox(scene, device,
                0.30f, 0.05f, 0.28f, ax, ay, az,
                OX + kOpsX, OY + 0.49f, OZ + seatZ, amber, 0.10f, steel);
            m_portalMeshes.push_back(cush.mesh);
            AddedEntity back = addOrientedEmissiveBox(scene, device,
                0.30f, 0.26f, 0.04f, ax, ay, az,
                OX + kOpsX, OY + 0.80f, OZ + seatZ - 0.26f, steel, 0.0f, steel);
            m_portalMeshes.push_back(back.mesh);
        }
        m_opsDesk = x3::phys::Vec3{ OX + kOpsX, OY + 1.02f, OZ + kOpsZ };
        m_opsSeat = x3::phys::Vec3{ OX + kOpsX, OY,          OZ + seatZ };
        // Flux telemetry ring: one lane per portal, seeded mid-scale.
        m_flux.assign(m_portals.size() * kFluxSamples, 0.5f);
        m_fluxHead = 0;
    }

    // Snapshot the authored light colours: the ALARM strobes them red during a
    // catastrophe and must restore them exactly (see tick()).
    m_lightBase.clear();
    m_lightBase.reserve(m_lights.size() * 3);
    for (const auto& L : m_lights) {
        m_lightBase.push_back(L.color[0]);
        m_lightBase.push_back(L.color[1]);
        m_lightBase.push_back(L.color[2]);
    }

    physics.optimizeBroadphase();
    m_built = true;
    x3::logInfo("[rifthub] hub built with " + std::to_string(m_portals.size()) +
                " rifts (ROUND 8: ONE-TUBE gate" +
                (m_gateGlbActive ? std::string(" [authored GLB]")
                                 : std::string(" [procedural fallback]")) +
                ", NO chevrons, operator panel + hanging holoterminal console per "
                "rift, PLASMA-STORM membrane, capped emissive)");
}

void Rifthub::tick(float dt, Scene& scene) {
    if (!m_built) return;
    m_time += dt;

    // ---- OPS STATION flux telemetry (30 Hz ring, one lane per portal) ------
    // The "workings" the analytics surface graphs: a live composite of each
    // gate's REAL state — membrane spin + aperture carry the baseline, a
    // kawoosh spikes it, snarl and implosion tear it, a dead gate flatlines.
    if (!m_flux.empty()) {
        m_fluxAccum += dt;
        const float kFluxDt = 1.0f / 30.0f;
        while (m_fluxAccum >= kFluxDt) {
            m_fluxAccum -= kFluxDt;
            m_fluxHead = (m_fluxHead + 1u) % kFluxSamples;
            for (size_t i = 0; i < m_portals.size(); ++i) {
                const RiftPortal& p = m_portals[i];
                float v = 0.5f + 0.16f * std::sin(m_time * (1.1f + 0.13f * (float)i))
                               + 0.07f * std::sin(m_time * (3.7f + 0.29f * (float)i));
                v *= p.aperture;
                v += p.kawoosh * 0.65f + p.snarl * 0.4f + p.implode * 0.9f;
                if (p.dead) v = 0.02f;
                m_flux[i * kFluxSamples + m_fluxHead] = std::clamp(v, 0.0f, 1.0f);
            }
        }
    }
    m_uiClock += dt;

    const float twoPi = 6.2831853f;

    // ===== ROUND 8: THE CATASTROPHES ==========================================
    // Decay the hub-wide events, then (below) apply what they do to the world.
    // NOTE: dt here is the RAW frame time — a TEMPORAL RIFT must not slow down its
    // own countdown, or it would never end.
    if (m_warp     > 0.0f) { m_warp     -= dt; if (m_warp     < 0.0f) m_warp     = 0.0f; }
    if (m_temporal > 0.0f) { m_temporal -= dt; if (m_temporal < 0.0f) m_temporal = 0.0f; }
    if (m_shake    > 0.0f) { m_shake -= m_shake * kShakeDecay * dt; if (m_shake < 1e-3f) m_shake = 0.0f; }
    if (m_flash    > 0.0f) { m_flash -= m_flash * kFlashDecay * dt; if (m_flash < 1e-3f) m_flash = 0.0f; }
    const bool anyCatastrophe = m_warp > 0.0f || m_temporal > 0.0f;
    bool anyImploding = false;
    for (const auto& pp : m_portals) if (pp.implode > 0.0f) anyImploding = true;
    if (!anyCatastrophe && !anyImploding && m_shake <= 0.0f) m_alarm.clear();
    m_alarmT += dt;
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
        float kawooshEm = 0.0f, surge01 = 0.0f, surgeProg = 0.0f;
        if (p.kawoosh > 0.0f) {
            p.kawoosh -= dt;
            if (p.kawoosh < 0.0f) p.kawoosh = 0.0f;
            const float tSince = kKawooshDur - p.kawoosh;          // seconds since the flash
            kawooshEm = kKawooshPeakEm * std::exp(-kKawooshDecay * tSince);
            surge01   = kawooshEm / kKawooshPeakEm;
            // LINEAR progress through the surge — this is what the SURGE flipbook
            // is played against (the exponential envelope drives BRIGHTNESS only).
            surgeProg = tSince / kKawooshDur;
        }
        // --- ROUND 8: the SNARL. The rift's live anger, read straight off the
        //     console's dialled-in parameters. This is what makes danger READABLE:
        //     the membrane, the tube's panel LEDs, the indicator track and the gate
        //     light all ride this value, so the rift visibly gets angry WHILE the
        //     player is still turning the knob — before anything blows. The active
        //     console's portal snarls; the others settle back to calm.
        {
            const float want = (p.dead || p.implode > 0.0f)
                                 ? 0.0f
                                 : (((int)i == m_activeConsole) ? p.console.instability()
                                                                : 0.0f);
            const float k = 1.0f - std::exp(-6.0f * dt);   // frame-rate independent ease
            p.snarl += (want - p.snarl) * k;
        }

        // --- IMPLOSION (the most spectacular consequence, and a PERMANENT one).
        //     The membrane inverts: it collapses inward, spinning up, until it eats
        //     itself. Then the shockwave lands and the gate is DEAD — forever.
        if (p.implode > 0.0f) {
            p.implode -= dt;
            if (p.implode <= 0.0f) {
                p.implode = 0.0f;
                p.dead      = true;      // PERSISTENT: a collapsed gate stays collapsed
                p.activated = false;
                p.throatOn  = false;
                p.kawoosh   = 0.0f;
                p.snarl     = 0.0f;
                m_shake = kImplodeShake;   // the shockwave
                m_flash = kImplodeDamage;  // ...and it hurts
                // The blast throws whatever the collapse had dragged in back OUT.
                for (int b = 0; b < 64; ++b) {
                    const float th = frand() * twoPi;
                    Mote mo;
                    mo.px = p.worldPos.x; mo.py = ringWorldY(); mo.pz = p.worldPos.z;
                    const float sp2 = 6.0f + 9.0f * frand();
                    mo.vx = (std::cos(th) * p.rightX - p.outX * 0.5f) * sp2 + frandSym();
                    mo.vy = std::sin(th) * sp2 * 0.5f + 1.5f;
                    mo.vz = (std::cos(th) * p.rightZ - p.outZ * 0.5f) * sp2 + frandSym();
                    mo.maxLife = mo.life = 0.7f + 1.0f * frand();
                    mo.size = 0.02f + 0.05f * frand();
                    mo.r = 1.0f; mo.g = 0.55f + 0.3f * frand(); mo.b = 0.35f;
                    spawnMote(mo);
                }
                x3::logInfo(std::string("[rifthub] IMPLOSION: the ") + p.destination +
                            " rift has COLLAPSED. That gate is dead.");
            }
        }

        // Membrane/gate state (PortalAnimated.mp4 arc), derived from the
        // existing gameplay latches: IDLE / SURGE (kawoosh) / OPEN (settled).
        const bool surging = p.kawoosh > 0.0f;
        const bool open    = p.activated && !surging;
        const bool imploding = p.implode > 0.0f;

        // NOTE: the ring itself is NOT animated (metal doesn't pulse).

        // --- Blue core light: slow hum-synced breathe onto the gate metal.
        //     ROUND 8: a DEAD gate casts nothing (the bay goes black — the most
        //     legible possible statement that the rift is gone), and a snarling one
        //     drags its own key light toward angry red.
        if (i < m_lights.size()) {
            if (p.dead) {
                m_lights[i].color[0] = m_lights[i].color[1] = m_lights[i].color[2] = 0.0f;
            } else {
                const float lS  = std::sin(m_time * (twoPi * kCoreLightFreqHz) + phase);
                const float l01 = 0.5f * (lS + 1.0f);
                float lI  = kCoreLightMin + (kCoreLightMax - kCoreLightMin) * l01;
                lI += surge01 * 8.0f;   // the kawoosh also floods the bay with light
                if (imploding) {
                    // The collapse pulls the light in with it, then flares.
                    const float t = 1.0f - p.implode / kImplodeDur;
                    lI *= 0.25f + 2.6f * t * t * t;
                }
                const float d = p.snarl;
                for (int c3 = 0; c3 < 3; ++c3) {
                    const float calm = kCoreLightBlue[c3];
                    const float hot  = kAlarmRed[c3];
                    m_lights[i].color[c3] = (calm + (hot - calm) * d) * lI;
                }
            }
        }

        // --- THE OPERATOR PANEL ON THE TUBE (round 8-B) is part of the TELEGRAPH.
        //     Its LCD and its LED strips are driven off p.snarl — the console's live
        //     instability — so the tube itself goes green -> amber -> red and starts
        //     to flicker under the player's hand, faster and hotter the closer the
        //     rift gets to catastrophe. This is the same readable-danger contract the
        //     glowing controls obey, mirrored onto the machine itself.
        //     A DEAD gate's panel is simply off. Forever.
        {
            const float d = p.dead ? 0.0f : p.snarl;
            const float hz = 0.7f + 8.0f * d * d;
            const float blink = 0.5f * (std::sin(m_time * twoPi * hz + phase) + 1.0f);
            const float depth = 0.55f * d * d;
            const float mod = (1.0f - depth) + depth * blink;
            if (p.panelScreenEnt && p.panelScreenEnt < sceneN) {
                Entity& se = ents[p.panelScreenEnt];
                for (int c3 = 0; c3 < 3; ++c3)
                    se.emissive[c3] = kPanelScreenCalm[c3] +
                        (kPanelScreenHot[c3] - kPanelScreenCalm[c3]) * d;
                se.emissive[3] = p.dead ? 0.0f
                    : capped((kPanelScreenEm + 0.5f * d + surge01 * 0.3f) * mod,
                             kPanelScreenCap);
            }
            if (p.panelLedEnt && p.panelLedEnt < sceneN) {
                Entity& le = ents[p.panelLedEnt];
                for (int c3 = 0; c3 < 3; ++c3)
                    le.emissive[c3] = kPanelLedCalm[c3] +
                        (kPanelLedHot[c3] - kPanelLedCalm[c3]) * d;
                le.emissive[3] = p.dead ? 0.0f
                    : capped((kPanelLedEm + 0.6f * d + surge01 * 0.4f) * mod, kPanelLedCap);
            }
        }

        // --- The recessed INDICATOR LINE: dim when dormant; a CHASE sweeps the
        //     circumference during the surge; steady powered glow once OPEN.
        //     (ROUND 9: cool blue-white and continuous — no longer an amber dash ring.)
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
                em = kTrackEmIdle + surge01 * (0.30f + kTrackChase * cph);
            }
            ents[e].emissive[3] = p.dead ? 0.0f : capped(em, kTrackEmCap);
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
            ents[ce].emissive[3] = p.dead ? 0.0f : capped(em, kConduitEmCap);
        }

        // (ROUND 6: the energy-core disks are GONE — see the header. No emissive
        //  pokes here, no dot on the membrane. The gate's blue point light above
        //  still breathes with the hum: that is the only "core" the player sees.)

        // --- MEMBRANE STATE MACHINE (PortalAnimated.mp4 animation arc) -------
        // p.throatOn latches the OPEN state (the portal has settled after its
        // surge). The TEXTURE for every state is chosen below in one place.
        if (p.activated && !surging) p.throatOn = true;

        // --- PLASMA-STORM membrane: breathe + rotate, every write capped -----
        const float rightv[3] = { p.rightX, 0.0f, p.rightZ };
        const float upv[3]    = { 0.0f, 1.0f, 0.0f };
        const float outv[3]   = { p.outX, 0.0f, p.outZ };
        const float cx = p.worldPos.x, cz = p.worldPos.z;
        // [0] plasma: IDLE = the flipbook churn; OPEN = the throat streaming
        //     (faster spin + hotter base, texture already swapped); SURGE rides
        //     the kawoosh envelope on top. Organic two-sine breathe; the surge
        //     tint slides deep blue -> pale blue (NOT white); always capped.
        //     The disk is DOUBLE-WOUND, so this one entity is the portal from
        //     both sides (the round-5 two-sided law).
        if (p.membraneEntFirst + 0 < sceneN) {
            Entity& e = ents[p.membraneEntFirst + 0];
            // ---- FLIPBOOK PLAYBACK — ONE PATH, ALL THREE STATES (round 6) ----
            // Pick this state's atlas; if it is present the membrane is REAL
            // FOOTAGE (loop for IDLE/OPEN, one-shot across the kawoosh for
            // SURGE). Only when an atlas is MISSING (fresh clone / LFS stub)
            // does that state fall back to a procedural map:
            //   IDLE  -> m_flipTex      | fallback m_plasmaTex (nebula)
            //   SURGE -> m_flipSurgeTex | fallback: the OPEN book / throat map
            //   OPEN  -> m_flipOpenTex  | fallback m_throatTex (procedural spiral)
            const std::vector<x3::rhi::TextureHandle>* book = &m_flipTex;   // IDLE
            bool oneShot = false;
            if (surging) {
                if (!m_flipSurgeTex.empty()) { book = &m_flipSurgeTex; oneShot = true; }
                else                         { book = &m_flipOpenTex; }
            } else if (p.throatOn) {
                book = &m_flipOpenTex;
            }
            if (!book->empty()) {
                const uint32_t f = flipFrameIndex((uint32_t)book->size(), m_time, i,
                                                  oneShot, surgeProg);
                e.tex = (*book)[f];
                e.emissiveTex = (*book)[f];
            } else {
                const x3::rhi::TextureHandle fb =
                    (p.throatOn || surging) ? m_throatTex : m_plasmaTex;
                e.tex = fb;
                e.emissiveTex = fb;
            }
            const bool footage = !book->empty();
            const float dir = (i & 1u) ? -1.0f : 1.0f;   // alternate spin direction
            // ROUND 8: an IMPLODING membrane spins UP violently as it collapses.
            float spin = kPlasmaSpinRadS * (open ? kPlasmaSpinOpenX : 1.0f);
            if (imploding) {
                const float t = 1.0f - p.implode / kImplodeDur;   // 0 -> 1
                spin *= 1.0f + 34.0f * t * t;
            }
            // Continuous angle across the state change: integrate instead of
            // evaluating a*t (a rate jump would snap the disk).
            p.spinAngle += spin * dir * dt;
            const float a = p.spinAngle + phase;
            const float ca = std::cos(a), sa = std::sin(a);
            // ---- APERTURE / INVERSION SCALE ------------------------------------
            // A NOMINAL rift dialled wide really IS wider (p.aperture, set by the
            // console). An IMPLODING one INVERTS: the disk is sucked in toward
            // nothing. A DEAD one has no disk at all. This is the same basis matrix
            // the membrane always used, with a scalar on its two in-plane axes.
            float ms = p.aperture;
            if (imploding) {
                const float t = 1.0f - p.implode / kImplodeDur;
                ms *= (1.0f - t) * (1.0f - t);      // eats itself, accelerating
            }
            if (p.dead) ms = 0.0f;
            if (ms < 0.0f) ms = 0.0f;
            const float rx[3] = { (ca * rightv[0] + sa * upv[0]) * ms,
                                  (ca * rightv[1] + sa * upv[1]) * ms,
                                  (ca * rightv[2] + sa * upv[2]) * ms };
            const float ry[3] = { (-sa * rightv[0] + ca * upv[0]) * ms,
                                  (-sa * rightv[1] + ca * upv[1]) * ms,
                                  (-sa * rightv[2] + ca * upv[2]) * ms };
            makeXform(e.transform, rx, ry, outv, cx, ringWorldY(), cz);
            const float wob = kPlasmaEmWobble *
                (0.62f * std::sin(m_time * 1.15f * twoPi * 0.31f + phase) +
                 0.38f * std::sin(m_time * 1.15f * twoPi * 0.53f + phase * 2.1f));
            const float base = open || surging ? kPlasmaEmBaseOpen : kPlasmaEmBase;
            // The collapse burns hotter as it shrinks; a dead gate emits NOTHING.
            float lift = 0.0f;
            if (imploding) {
                const float t = 1.0f - p.implode / kImplodeDur;
                lift = 0.9f * t * t;
            }
            e.emissive[3] = p.dead ? 0.0f
                                   : capped(base + wob + kawooshEm + lift, kPlasmaEmCap);
            // Surge tint: deep blue -> pale blue (NOT white) with the envelope.
            // FOOTAGE rides the paler kFlipTint base (the video's frames carry
            // the reference's own blue and would double-blue under kPlasmaBlue);
            // a procedural fallback map keeps kPlasmaBlue.
            const float* baseTint = footage ? kFlipTint : kPlasmaBlue;
            for (int c3 = 0; c3 < 3; ++c3)
                e.emissive[c3] = baseTint[c3] +
                                 (kKawooshTint[c3] - baseTint[c3]) * surge01;
            // ROUND 8 — THE MEMBRANE TELEGRAPHS ITS OWN INSTABILITY. The storm slides
            // from blue toward angry red as the console is dialled into danger (and
            // all the way there during a collapse). Cap law intact: we SHIFT the hue,
            // we never lift all three channels (that is how you get white).
            const float ang = imploding ? 1.0f : p.snarl;
            if (ang > 0.001f)
                for (int c3 = 0; c3 < 3; ++c3)
                    e.emissive[c3] += (kAlarmRed[c3] - e.emissive[c3]) * ang;
        }
        // [1] fresnel rim: slow shimmer + kawoosh lift + a touch hotter when
        //     OPEN (the throat's grazing edge), capped.
        if (p.membraneEntFirst + 1 < sceneN) {
            Entity& e = ents[p.membraneEntFirst + 1];
            const float shim = 0.5f * (std::sin(m_time * twoPi * kRimShimmerHz + phase) + 1.0f);
            const float lift = open ? 0.25f : 0.0f;
            e.emissive[3] = p.dead ? 0.0f
                : capped(kRimEmBase + lift + kRimShimmerAmp * shim + kawooshEm * 0.55f,
                         kRimEmCap);
        }

        // --- Lightning-arc spawner — SURGE ONLY (round 5, owner's verdict on the
        //     idle bolts: "2nd grade crayon"). He is right, and the reason is
        //     structural: the FLIPBOOK IS THE OWNER'S REFERENCE VIDEO, and its
        //     pixels already contain real, beautifully-simulated lightning
        //     filaments. Drawing uniform-width, hard-cornered procedural
        //     polylines ON TOP of that footage can only VANDALIZE it — the
        //     membrane's own lightning is strictly better than anything we stamp
        //     over it. So IDLE draws NO arcs (the flipbook speaks) and OPEN draws
        //     NO arcs (the throat's radial streaming is baked into its texture).
        //     Arcs survive ONLY for the 1.6 s activation SURGE, where they are a
        //     rim-orbit VORTEX RING the flipbook does not contain — an EVENT, not
        //     a decoration — and drawFx() now tapers them (width falls off toward
        //     the tips, per-segment brightness jitter) so they read as electricity
        //     instead of crayon.
        p.arcCooldown -= dt;
        for (uint32_t a = 0; a < RiftPortal::kMaxArcs; ++a)
            if (p.arcs[a].life > 0.0f) p.arcs[a].life -= dt;
        uint32_t live = 0;
        for (uint32_t a = 0; a < RiftPortal::kMaxArcs; ++a)
            if (p.arcs[a].life > 0.0f) ++live;
        if (surging && live < RiftPortal::kMaxArcs) {
            spawnArc(p, /*mode=*/1);   // the vortex ring
            p.arcCooldown = 0.02f;
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
            mo.py = ringWorldY() + rr * std::sin(th);
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
    // ROUND 8: while a gate IMPLODES it does not merely LOOK wrong — it PULLS. Every
    // loose mote in the hub accelerates toward the collapsing membrane, so the debris
    // visibly streams into the throat (the brief: "props/debris drag toward it").
    for (int m = 0; m < kMaxMotes; ++m) {
        Mote& mo = m_motes[m];
        if (mo.life <= 0.0f) continue;
        mo.life -= dt;
        for (const auto& pp : m_portals) {
            if (pp.implode <= 0.0f) continue;
            const float dx = pp.worldPos.x - mo.px;
            const float dy = ringWorldY()        - mo.py;
            const float dz = pp.worldPos.z - mo.pz;
            const float d2 = dx * dx + dy * dy + dz * dz;
            const float d  = std::sqrt(d2 > 1e-4f ? d2 : 1e-4f);
            // 1/r falloff, clamped near the throat so nothing goes ballistic.
            const float g = kImplodePullA / (d > 1.2f ? d : 1.2f);
            mo.vx += (dx / d) * g * dt;
            mo.vy += (dy / d) * g * dt;
            mo.vz += (dz / d) * g * dt;
            if (d < 0.35f) mo.life = 0.0f;   // swallowed
        }
        mo.px += mo.vx * dt; mo.py += mo.vy * dt; mo.pz += mo.vz * dt;
        mo.vx -= mo.vx * 0.6f * dt; mo.vz -= mo.vz * 0.6f * dt;   // gentle drag
    }

    // ===== ROOM WARP — the hub physically BENDS ================================
    // A travelling radial ripple out of the gate that tore it: every hall prop is
    // pushed along its own outward direction (walls bow, columns lean, beams and
    // machinery drift and sway), amplitude falling with distance, eased in and out
    // so nothing snaps. The LENS half of the effect is fovOffset(), which the host
    // adds to the camera FOV.
    //
    // Displacements are written ABSOLUTELY from the authored base each frame, never
    // accumulated, so when the warp ends the hall is restored EXACTLY. (These hall
    // meshes are baked in world space, so their base translation is 0.)
    {
        auto& wents = scene.entities();
        const uint32_t wn = (uint32_t)m_warpEnts.size();
        const bool warping = m_warp > 0.0f;
        float env = 0.0f;
        if (warping) {
            const float t = 1.0f - m_warp / kWarpDur;          // 0 -> 1
            env = (t < 0.12f) ? (t / 0.12f)
                              : (t > 0.75f ? (1.0f - t) / 0.25f : 1.0f);
            if (env < 0.0f) env = 0.0f;
        }
        if (warping || m_warpWasOn) {
            for (uint32_t k = 0; k < wn; ++k) {
                const uint32_t e = m_warpEnts[k];
                if (e >= (uint32_t)wents.size()) continue;
                const float bx = m_warpBase[k * 3 + 0];
                const float by = m_warpBase[k * 3 + 1];
                const float bz = m_warpBase[k * 3 + 2];
                float ox = 0.0f, oy = 0.0f, oz = 0.0f;
                if (warping) {
                    const float dx = bx - m_warpSrc[0];
                    const float dz = bz - m_warpSrc[2];
                    const float d  = std::sqrt(dx * dx + dz * dz);
                    const float ux = (d > 1e-3f) ? dx / d : 1.0f;
                    const float uz = (d > 1e-3f) ? dz / d : 0.0f;
                    const float w = std::sin(d * kWarpWaveK - m_time * twoPi * kWarpWaveHz);
                    const float fall = 1.0f / (1.0f + d * 0.045f);
                    const float amp = kWarpAmp * env * fall;
                    ox = ux * w * amp;
                    oz = uz * w * amp;
                    oy = std::cos(d * kWarpWaveK * 0.8f - m_time * twoPi * kWarpWaveHz * 0.7f)
                         * amp * 0.55f * (by > 0.5f ? 1.0f : 0.35f);
                }
                wents[e].transform[12] = ox;
                wents[e].transform[13] = oy;
                wents[e].transform[14] = oz;
            }
        }
        m_warpWasOn = warping;
    }

    // ===== THE HANGING CONSOLES: bake + shimmer ===============================
    // HoloTerminal::setLines only marks the glass DIRTY — regenTexture() runs inside
    // update(). Without this the readout never rasterizes and the console renders as a
    // featureless blue slab. (Exactly what the first R8 capture showed. Look at your
    // own screenshots.)
    for (auto& h : m_holos) h.update(dt);

    // ===== ALARM: the hall LIGHTING reacts ====================================
    // Any catastrophe strobes the hall's fill rig red; the authored colours are
    // restored exactly from m_lightBase the moment the event ends.
    {
        const bool alarming = m_warp > 0.0f || m_temporal > 0.0f || anyImploding;
        const uint32_t first = (uint32_t)m_portals.size();   // [0..N) are the gate lights
        const float strobe = 0.5f * (std::sin(m_alarmT * twoPi * kAlarmHz) + 1.0f);
        for (uint32_t k = first; k < m_lights.size() && (k * 3 + 2) < m_lightBase.size(); ++k)
            for (int c3 = 0; c3 < 3; ++c3) {
                const float base = m_lightBase[k * 3 + c3];
                m_lights[k].color[c3] = alarming
                    ? (base * 0.30f + kAlarmRed[c3] * kAlarmI * strobe * 0.5f)
                    : base;
            }
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
                ringWorldY() + rad * s,
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
            // ROUND 5 arc rebuild (the owner's "crayon" note): a real bolt is not
            // a uniform-width tube. Width TAPERS to nothing at both tips, each
            // segment carries its own brightness, and the jitter runs finer — so
            // the surge vortex reads as electricity whipping the rim, not as a
            // stroke drawn with a marker.
            const uint32_t segs = 18u;                  // finer polyline (was 12)
            const float jamp = 0.045f;                  // finer jitter
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
                // TAPER: sin-shaped width envelope over the arc's length (fat in
                // the belly, vanishing at both tips) x the life envelope.
                const float tm = (float)s3 / (float)segs;
                const float taper = std::sin(3.14159265f * tm);
                const float thick = kArcThickness * (0.35f + 0.65f * env)
                                  * (0.15f + 0.85f * taper * taper);
                // Per-segment brightness: the belly burns, the tips fade out.
                float segEm[4] = { em[0], em[1], em[2],
                                   em[3] * (0.30f + 0.70f * taper) * (0.75f + 0.25f * jr()) };
                float model[16];
                beamXform(model, prev, pt, thick);
                device.drawMeshEmissive(frame, m_fxBeamMesh, x3::rhi::TextureHandle{},
                                        kArcColor, segEm, model);
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
                    beamXform(model, fp, pt, kArcThickness * 0.45f * (1.0f - 0.6f * (float)fs / 3.0f));
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

    // ---- Light-shaft dust columns (WS2 fake volumetrics): a second additive
    // batch — soft billboards stacked down each registered shaft, radius and
    // size growing toward the deck, alpha fading, the whole column swirling
    // almost imperceptibly. Deterministic per (shaft, i); reuses m_moteScratch
    // (submitParticles copies immediately).
    uint32_t sn = 0;
    for (uint32_t si = 0; si < m_shafts.size() && sn < (uint32_t)kMaxMotes; ++si) {
        const Shaft& s = m_shafts[si];
        for (int i = 0; i < kShaftParticles && sn < (uint32_t)kMaxMotes; ++i) {
            const float t = ((float)i + 0.5f) / (float)kShaftParticles;   // 0 top -> 1 bot
            const float h01 = x3::prims::detail::hash01((uint32_t)i * 7u + si,
                                                        si * 13u + 3u, 4096u, 0x5AF7u);
            // COHERENT column (first capture wiggled like smoke snakes): the
            // offset follows one slow helix down the axis, not per-particle
            // random scatter — heavy billboard overlap fuses it into a beam.
            const float swirl = si * 2.1f + (float)i * 0.34f + m_time * kShaftDriftRadS;
            const float rad = (kShaftTopR + (s.width - kShaftTopR) * t) * 0.45f;
            const float ox = std::cos(swirl) * rad, oz = std::sin(swirl) * rad;
            x3::rhi::IRenderDevice::ParticleInstance& inst = m_moteScratch[sn++];
            inst.pos[0] = s.top[0] + (s.bot[0] - s.top[0]) * t + s.ux * ox + s.vx * oz;
            inst.pos[1] = s.top[1] + (s.bot[1] - s.top[1]) * t + s.uy * ox + s.vy * oz;
            inst.pos[2] = s.top[2] + (s.bot[2] - s.top[2]) * t + s.uz * ox + s.vz * oz;
            inst.size = 0.48f + 0.85f * t;                 // widens toward the deck
            const float fade = 0.30f + 0.70f * (1.0f - t) * (1.0f - t); // hot at the fixture
            inst.color[0] = 0.55f; inst.color[1] = 0.66f; inst.color[2] = 0.95f;
            inst.color[3] = s.alpha * fade;
            inst.pos[1] += 0.03f * std::sin(m_time * 0.5f + h01 * 9.0f);  // breath
        }
    }
    if (sn) device.submitParticles(m_moteScratch, sn,
                                   x3::rhi::IRenderDevice::ParticleBlend::Additive);
}

void Rifthub::applyAtmosphere(x3::rhi::IRenderDevice& device) const {
    // Industrial hall haze: enough that the far wall's machinery reads as
    // silhouettes and the gate light shafts get body, never a milky wash.
    x3::rhi::IRenderDevice::FogParams fog;
    fog.enabled  = true;
    fog.color[0] = 0.020f; fog.color[1] = 0.030f; fog.color[2] = 0.050f;   // cold blue haze
    fog.density  = 0.015f;   // ROUND 2: thin enough that the far shell READS
    fog.start    = 2.5f;
    fog.maxOpacity = 0.70f;
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
    // ROUND 2: ambient roughly doubled — the owner's read was "pitch black
    // behind the gates". The hall must READ (columns/beams/machinery as lit
    // silhouettes) while staying a moody dark-industrial grade.
    // ROUND 5 (owner: "we did NOT get any more light in the room" — he wants to
    // SEE the wet floor): the extra light comes from the rebuilt POINT-LIGHT rig,
    // NOT from ambient. Ambient is omnidirectional: raising it lifts every surface
    // uniformly, which lights the room by DESTROYING its contrast — it washed the
    // gate's forged plates into flat grey cardboard (measured: a 45% ambient lift
    // turned the ring's albedo variation into a neutral 0.31 wash). So ambient goes
    // DOWN from the round-2 value, the directional fills carry the room, and the
    // gate hardware gets its highlight/shadow separation back.
    // ROUND 9 (owner: "now it's TOO bright in the portal room"). The GLB/PBR shading
    // bug that every earlier round was compensating for is FIXED, so this ambient —
    // itself roughly doubled in round 2 to fight it — is now stacked on top of honest
    // lighting and is flattening the hall into a pale warehouse. It comes DOWN HARDEST,
    // for the reason spelled out directly above: ambient is omnidirectional, so it
    // lights the room BY DESTROYING ITS CONTRAST. The point rig and the MEMBRANES carry
    // this hall; ambient only has to keep the deepest corners from going pure void.
    device.setAmbient(0.032f, 0.036f, 0.046f);
    // ===== R10 — THE GATE HAD NOTHING TO REFLECT ==============================
    // Owner, holding up a reference render of a machined-steel structure:
    //   "There are no lights. just shiny reflections from the white cloudy sky."
    //   "We need that kind of lighting model."
    // He is describing an ENVIRONMENT-driven metal: no emissives on the object at
    // all, its entire read coming from a big bright dome reflected in its chamfers.
    // That is exactly what mesh.frag's iblAmbient() does -- specular IBL samples the
    // PREFILTERED env cube along R at mip = roughness, which works at ANY roughness
    // (rough 0.62 just picks a blurrier mip = a broad, soft, overcast reflection).
    // Note the SSR roughness gate (1 - smoothstep(0.25, 0.6, rough)) applies ONLY to
    // the reflTex blend weight -- so the ghost-glass/SSR fix is UNTOUCHED by this.
    //
    // The bug: setIblProbe(true) bakes the env cube FROM THE SCENE, and this scene is
    // a deliberately DARK hall. So the gate's mirror was pointed at a black room. Its
    // prefiltered specular came back ~0, the metal fell through to the flat
    // ambient-specular FLOOR (a constant), and 90k triangles of freshly-machined
    // chamfers rendered as GREY MUSH. Nine rounds of art, and the last thing missing
    // was something for the steel to look at.
    //
    // The fix is not a light and not an albedo: give the room an ENVIRONMENT. The
    // hall's own point rig, ambient, fog, grade, shafts and membranes are all
    // unchanged -- we only change what the metal SEES. A big soft overcast dome is
    // also what a real industrial hall's overhead diffusers would give you.
    {
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled      = true;
        // A WHITE CLOUDY SKY: no visible sun disc, heavy haze, near-neutral and
        // bright from zenith to horizon -- a softbox the size of the world. The sun
        // term stays low so this reads as OVERCAST, not as a second key light.
        sp.sunDir[0] = 0.30f; sp.sunDir[1] = 0.82f; sp.sunDir[2] = -0.48f;
        sp.sunColor[0] = 1.00f; sp.sunColor[1] = 0.99f; sp.sunColor[2] = 0.97f;
        sp.sunIntensity = 0.12f;      // overcast: the DOME lights it, not the sun
        sp.haze         = 1.00f;
        sp.exposure     = 1.00f;
        sp.zenith[0]  = 0.62f; sp.zenith[1]  = 0.66f; sp.zenith[2]  = 0.72f;
        sp.horizon[0] = 0.78f; sp.horizon[1] = 0.80f; sp.horizon[2] = 0.84f;
        device.setSkyParams(sp);
    }
    // Bake the env cube from that DOME, not from the black room. (Scene-probe off:
    // its whole purpose was "reflect the hall not an open sky", and the hall turned
    // out to be the problem -- a mirror aimed at a void.)
    device.setIblProbe(false);
    // ROUND 5 — THE GHOST-GATE CURE. The hub never called setIblIntensity, so it
    // ran at 1.0: FULL environment IBL on every surface. mesh.frag's iblAmbient()
    // adds prefiltered env specular weighted by the split-sum BRDF *bias* term
    // (`ab.y`), which is ALBEDO-INDEPENDENT — so a bright env cube paints a
    // neutral grey wash over a surface no matter what its albedo says. On the
    // gate's dense, curved, grazing-angle plates that wash WON: the ring rendered
    // as a pale translucent grey shell and four rounds of texture work (curated
    // sets, SD3.5 text-forge, now img2img-from-reference) were invisible under it.
    // Proof: forcing the gate's baseColor to pure RED did not change a pixel; the
    // albedo term simply wasn't in the fight.
    // This is exactly the SEAM-2 knob the interface documents for a mood-calibrated
    // interior. At 0.30 the gate's own forged albedo + the point-light rig carry the
    // metal, and the wet floor keeps its reflections (they come from the probe, not
    // from this scale).
    // R9: the IBL bias term (`ab.y`) is ALBEDO-INDEPENDENT — it paints a neutral grey
    // wash regardless of what the surface says it is. That is precisely the "washed-out
    // pale concrete" failure mode, and it too was tuned under the broken path. Down.
    // THE ENV DIFFUSE STAYS DOWN. This scale drives the irradiance the room's
    // CONCRETE drinks, and the hall is meant to be dark -- raising it to feed the
    // steel washed the whole hall into a pale warehouse (the exact failure mode
    // rounds 2/5/9 kept re-learning). It stays at the calibrated interior value.
    // The dome is FAR brighter than the black room this was calibrated against, so
    // holding 0.18 would now pump real irradiance into every wall and wash the hall
    // pale -- the exact failure rounds 2/5/9 kept re-learning. It comes DOWN to keep
    // the hall's DIFFUSE response where the owner signed it off.
    const float kIblInterior = 0.07f;
    device.setIblIntensity(kIblInterior);
    // ...AND THE ENV SPECULAR COMES UP. New knob (r_iblspec): it scales the
    // prefiltered environment reflection ALONE. Metals are kD ~ 0 -- reflection IS
    // their entire ambient response -- so this feeds the gate's chamfers a bright
    // overcast dome to catch, while the hall's dielectrics (kD ~ 1, F0 = 0.04) barely
    // register it and keep their darkness. That is the owner's reference exactly:
    // "there are no lights, just shiny reflections from the white cloudy sky."
    // No emissive, no over-unity albedo, no key-light crank: the metal is lit by
    // being SHINY AT SOMETHING BRIGHT, which is how metal has always worked.
    device.setIblSpecular(1.15f);
    // R9: exposure bias was pushed to 1.24 to lift a scene the renderer was under-
    // lighting by ~PI. With the renderer honest, a >1 bias is just a second blowout
    // multiplier stacked on the first. Back to neutral — and slightly under, because
    // auto-exposure already meters the (very bright) membranes, and the lesson this
    // room keeps re-learning is that fixing VALUE beats adding lumens.
    device.setExposure(0.98f);
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
    if (m_mrFlat.valid())     { device.destroyTexture(m_mrFlat);     m_mrFlat     = {}; }
    if (m_mrWet.valid())      { device.destroyTexture(m_mrWet);      m_mrWet      = {}; }
    for (auto& h : m_mrGate)
        if (h.valid()) { device.destroyTexture(h); h = {}; }
    for (auto& h : m_holos) h.shutdown(device);   // round 8: the hanging consoles
    m_holos.clear();
    for (auto& h : m_flipTex) if (h.valid()) device.destroyTexture(h);
    m_flipTex.clear();
    for (auto& h : m_flipSurgeTex) if (h.valid()) device.destroyTexture(h);
    m_flipSurgeTex.clear();
    for (auto& h : m_flipOpenTex) if (h.valid()) device.destroyTexture(h);
    m_flipOpenTex.clear();
    m_surf.destroyAll(device);   // curated PBR sets (ring plates / housings / hall)
    m_shafts.clear();
    // ROUND 3 gate GLB: the LOADER owns its GPU handles — unload once, then drop
    // the loader/source (gate entities referenced these meshes, never owned them).
    if (m_gateLoader && m_gateModel.ok) m_gateLoader->unload(m_gateModel);
    m_gateDrawables.clear();
    m_gateNames.clear();
    m_gateLoader.reset();
    m_gateAssets.reset();
    m_gateGlbActive = false;
    for (int m = 0; m < kMaxMotes; ++m) m_motes[m].life = 0.0f;
    m_portals.clear();
    m_lights.clear();
    m_time = 0.0f;
    m_built = false;
}

// The readable name of whatever a rift is currently aimed at. The gates hold
// registry KEYS ("f7"); the glass and the HUD must say "Facility F7 - Executive".
// Falls back to the raw string so a hand-set destination never blanks the readout.
static std::string riftDestName(const std::string& key) {
    const Destination* d = findDestination(key);
    return d ? std::string(d->name) : key;
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
                    mo.py = ringWorldY() + rr * std::sin(th);
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
    // Say where it goes NOW (a re-targeted gate must never advertise its old aim),
    // in READABLE words, not the registry key.
    const std::string where = riftDestName(p.destination);
    if (p.dead) {
        outPrompt = "Rift " + std::to_string(bestIdx + 1) + ": COLLAPSED — it goes nowhere";
    } else if (p.activated) {
        outPrompt = "Rift OPEN -> " + where + " — step through";
    } else {
        outPrompt = "Rift -> " + where + " — walk in to activate";
    }
    return true;
}

bool Rifthub::allActivated() const {
    for (const auto& p : m_portals) if (!p.activated) return false;
    return !m_portals.empty();
}

// ===========================================================================
// W-RIFT — THE PAYOFF: stepping THROUGH an open rift.
// ===========================================================================
// The gate's throat is a disk of radius kMembraneR standing at ringWorldY() in
// the plane whose normal is the portal's outward axis. A traversal is the eye
// being INSIDE that opening: close to the gate plane (|along outward| small) and
// close to the axis in the ring's own plane. A gate only takes you somewhere when
// it is ACTIVATED (a live membrane) and not DEAD (an imploded gate is a hole full
// of nothing — the console's consequences have to MEAN something).
int Rifthub::traversalPortal(const x3::phys::Vec3& eye, float radiusM) const {
    if (!m_built) return -1;
    for (uint32_t i = 0; i < m_portals.size(); ++i) {
        const RiftPortal& p = m_portals[i];
        if (!p.activated || p.dead || p.implode > 0.0f) continue;
        const float dx = eye.x - p.worldPos.x;
        const float dz = eye.z - p.worldPos.z;
        const float dy = eye.y - ringWorldY();
        // Split into the gate's own frame: `along` runs through the ring (its hole
        // axis), `side` runs across the ring in the horizontal.
        const float along = dx * p.outX   + dz * p.outZ;
        const float side  = dx * p.rightX + dz * p.rightZ;
        if (std::fabs(along) > 0.9f) continue;                     // not in the throat
        if (side * side + dy * dy > radiusM * radiusM) continue;   // not in the opening
        return (int)i;
    }
    return -1;
}

void Rifthub::setDestination(uint32_t portalIdx, const std::string& dest) {
    if (portalIdx >= m_portals.size() || dest.empty()) return;
    // CANONICALISE: store the registry KEY when the string resolves to one, so the
    // gate, the console's cycle, the holo glass and the host's fast-travel resolver
    // are all talking about the same row of the same table. (Legacy strings like
    // "the river valley" resolve; anything unknown is kept verbatim and the resolver
    // will REFUSE it out loud rather than silently going nowhere.)
    const Destination* d = findDestination(dest);
    m_portals[portalIdx].destination = d ? std::string(d->key) : dest;
    if (portalIdx < m_holos.size()) m_holos[portalIdx].setLines(consoleReadout(portalIdx));
    x3::logInfo("[rifthub] rift " + std::to_string(portalIdx + 1) + " (" +
                (m_portals[portalIdx].worldName ? m_portals[portalIdx].worldName : "?") +
                ") re-aimed at: " + dest);
}

// ===========================================================================
// ROUND 8 — THE CONSOLES + THE CONSEQUENCES
// ===========================================================================
int Rifthub::consoleInRange(const x3::phys::Vec3& eye, float radiusM) const {
    if (!m_built) return -1;
    int   best = -1;
    float bestD2 = radiusM * radiusM;
    for (uint32_t i = 0; i < m_portals.size(); ++i) {
        const RiftPortal& p = m_portals[i];
        // The console HANGS in the approach, hub-side of the gate — that is what the
        // player walks up to, not the gate itself.
        const float hx = p.worldPos.x - p.outX * kConsoleStandoff;
        const float hz = p.worldPos.z - p.outZ * kConsoleStandoff;
        const float dx = eye.x - hx, dz = eye.z - hz;
        const float d2 = dx * dx + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; best = (int)i; }
    }
    return best;
}

void Rifthub::openConsole(int portalIdx) {
    if (portalIdx < 0 || portalIdx >= (int)m_portals.size()) return;
    m_activeConsole = portalIdx;
    if ((size_t)portalIdx < m_holos.size()) m_holos[(size_t)portalIdx].setActive(true);
}

void Rifthub::closeConsole() {
    if (m_activeConsole >= 0 && (size_t)m_activeConsole < m_holos.size())
        m_holos[(size_t)m_activeConsole].setActive(false);
    m_activeConsole = -1;
}

void Rifthub::applyOutcome(uint32_t portalIdx, RiftOutcome outcome) {
    if (portalIdx >= m_portals.size()) return;
    RiftPortal& p = m_portals[portalIdx];
    if (p.dead) return;   // a collapsed gate does not answer any more. Ever.

    switch (outcome) {
    case RiftOutcome::Nominal: {
        // ---- WONDERFUL. Open + stabilise. -----------------------------------
        p.activated = true;
        p.kawoosh   = kKawooshDur;     // it surges, then settles into the OPEN throat
        // RE-TARGET: a typed TARGET naming a real slice re-points the rift. A real,
        // visible consequence — the hanging glass and the HUD prompt both change, and
        // the gate now signposts somewhere else entirely.
        if (const char* w = p.console.targetWorld()) {
            if (p.destination != w) {
                x3::logInfo(std::string("[rifthub] RE-TARGET: rift ") +
                            std::to_string(portalIdx + 1) + "  " + p.destination +
                            " -> " + w);
                p.destination = w;
                p.worldName   = w;   // both are static literals (kWorlds / kPortalTable)
            }
        }
        // APERTURE: a stable rift dialled wide really IS wider and brighter — the
        // membrane disk is scaled by this (0.72x .. 1.18x of the throat).
        p.aperture = 0.72f + 0.46f * p.console.value[RP_Aperture];
        m_alarm.clear();
        break;
    }
    case RiftOutcome::Misfire:
        // It tries, and it fails: a surge that never settles into an open throat.
        p.kawoosh = kKawooshDur;
        m_alarm = "MISFIRE";
        break;

    case RiftOutcome::Implosion:
        // ---- DISASTROUS, and PERMANENT. -------------------------------------
        p.implode = kImplodeDur;
        p.kawoosh = 0.0f;
        m_alarm   = "IMPLOSION - CONTAINMENT LOST";
        x3::logInfo("[rifthub] IMPLOSION armed — the membrane is inverting.");
        break;

    case RiftOutcome::RoomWarp:
        m_warp = kWarpDur;
        m_warpSrc[0] = p.worldPos.x;
        m_warpSrc[1] = ringWorldY();
        m_warpSrc[2] = p.worldPos.z;
        p.kawoosh = kKawooshDur;
        m_alarm   = "SPATIAL DISTORTION";
        x3::logInfo("[rifthub] ROOM WARP — the hall is bending.");
        break;

    case RiftOutcome::TemporalRift:
        m_temporal = kTemporalDur;
        p.kawoosh  = kKawooshDur;
        m_alarm    = "TEMPORAL RIFT";
        x3::logInfo("[rifthub] TEMPORAL RIFT — time has stopped agreeing with itself.");
        break;

    case RiftOutcome::Breach:
        // *** STUBBED, AND SAID SO. *** The bookkeeping, the alarm, the hall's red
        // strobe and the destabilised gate are all LIVE. What is NOT authored is the
        // thing that walks out. When it is, it hangs off exactly this case: spawn a
        // hostile on the membrane plane (worldPos - outward*0.5, y = kRingY) and hand
        // it to the host's enemy system. Nothing else has to change — the rule is
        // already in kRiftRules, the console already reaches it, the player already
        // gets the alarm.
        p.kawoosh = kKawooshDur;
        m_alarm   = "CONTAINMENT BREACH - INBOUND MASS";
        x3::logInfo("[rifthub] CONTAINMENT BREACH (STUB: alarm + destabilise; the thing "
                    "that comes THROUGH is not authored yet)");
        break;

    default:
        break;
    }
    p.console.lastOutcome = outcome;
}

// THE HANGING GLASS SAYS WHERE THIS PORTAL GOES. One builder, used BOTH at build time
// and after every ENGAGE, so the two can never drift apart (they used to: the build
// path wrote a hardcoded "STATUS DORMANT / BEARING 000.0" placeholder, the engage path
// wrote something else entirely, and neither carried the dialled-in parameters).
//
// Line 0 is the header title; lines 1+ are the left-column data rows. Content, per the
// brief: the DESTINATION, the STATUS, and the live PARAMETER READOUTS in real units.
std::vector<std::string> Rifthub::consoleReadout(uint32_t idx) const {
    if (idx >= m_portals.size()) return { "RIFT" };
    const RiftPortal& p = m_portals[idx];
    const RiftConsole& c = p.console;

    auto row = [&](uint32_t id, const char* label) {
        const RiftParamSpec& s = riftParamSpec(id);
        const float real = s.unitMin + (s.unitMax - s.unitMin) * c.value[id];
        char b[64];
        std::snprintf(b, sizeof(b), "%-6s %7.1f %s", label, real, s.unit);
        return std::string(b);
    };

    const std::string status =
        p.dead      ? std::string("DESTROYED")
      : p.activated ? std::string("OPEN")
      : (c.lastOutcome != RiftOutcome::None ? std::string(c.status)
                                            : std::string("DORMANT"));

    // Kept to EIGHT rows on purpose. Every row costs type size (the bake fits the rows
    // to the glass), and this panel's whole job is to be READ from where the player is
    // standing. Destination and status first — that is the question the console answers.
    return {
        std::string("RIFT ") + std::to_string(idx + 1) + " / " +
            std::to_string(m_portals.size()),
        std::string("DEST   ") + riftDestName(p.destination),
        std::string("STATUS ") + status,
        row(RP_Power,       "PWR"),
        row(RP_Frequency,   "FREQ"),
        row(RP_Aperture,    "APER"),
        row(RP_Containment, "CONT"),
        "[E] OPERATE",
    };
}

bool Rifthub::updateConsole(x3::ui::UiContext& ui, float dt) {
    if (m_activeConsole < 0 || (size_t)m_activeConsole >= m_portals.size()) return false;
    m_uiClock += dt;
    RiftPortal& p = m_portals[(uint32_t)m_activeConsole];

    char title[32];
    std::snprintf(title, sizeof(title), "%u/%u", (unsigned)m_activeConsole + 1u,
                  (unsigned)m_portals.size());
    const std::string where = riftDestName(p.destination);
    const bool engaged = drawRiftConsole(ui, p.console, title, where.c_str(),
                                         m_uiClock, p.dead);
    if (engaged) {
        applyOutcome((uint32_t)m_activeConsole, p.console.lastOutcome);
        // Push the result onto the hanging glass, so the WORLD says it too — not just
        // the overlay the player happens to be looking through. Same builder as build().
        if ((size_t)m_activeConsole < m_holos.size())
            m_holos[(size_t)m_activeConsole].setLines(consoleReadout((uint32_t)m_activeConsole));
    }
    return engaged;
}

// ===========================================================================
// THE OPS STATION (owner, 2026-08-30) — seat FSM, seat camera, analytics.
// ===========================================================================
bool Rifthub::opsInRange(const x3::phys::Vec3& eye, float radiusM) const {
    if (!m_built) return false;
    const float dx = eye.x - m_opsSeat.x, dz = eye.z - m_opsSeat.z;
    return dx * dx + dz * dz <= radiusM * radiusM;
}

void Rifthub::opsEye(float outPos[3], float& outYaw, float& outPitch) const {
    // Seated eye: over the chair, pulled slightly back from the desk glass,
    // aimed across the desk at the gate ring (the desk faces +Z, hub-north).
    outPos[0] = m_opsSeat.x;
    outPos[1] = m_opsSeat.y + 1.24f;
    outPos[2] = m_opsSeat.z - 0.10f;
    const float dx = m_opsDesk.x - outPos[0];
    const float dz = (m_opsDesk.z + 0.6f) - outPos[2];
    outYaw   = std::atan2(dz, dx);
    outPitch = -0.10f;   // a hair down onto the glass
}

float Rifthub::fluxSample(uint32_t portalIdx, uint32_t sampleIdx) const {
    if (m_flux.empty() || portalIdx >= m_portals.size()) return 0.0f;
    return m_flux[portalIdx * kFluxSamples + (sampleIdx % kFluxSamples)];
}

void Rifthub::updateOps(x3::ui::UiContext& ui, float dt) {
    if (!m_opsSeated || m_portals.empty()) return;
    m_uiClock += dt;

    // The surface: a dark glass sheet across the lower 46% of the frame — the
    // seated camera looks over it at the real ring, so the analytics never
    // hide the machines they describe.
    const float W = (float)ui.screenW(), H = (float)ui.screenH();
    const float px = W * 0.045f, pw = W * 0.91f;
    const float py = H * 0.52f,  ph = H * 0.44f;
    const float glass[4] = { 0.015f, 0.035f, 0.045f, 0.90f };
    const float hair[4]  = { 0.10f, 0.55f, 0.60f, 0.55f };
    const float txt[4]   = { 0.62f, 0.92f, 0.95f, 1.0f };
    const float dim[4]   = { 0.35f, 0.55f, 0.58f, 1.0f };
    const float bad[4]   = { 0.95f, 0.35f, 0.25f, 1.0f };
    const float ok[4]    = { 0.35f, 0.95f, 0.55f, 1.0f };
    ui.quad(px, py, pw, ph, glass);
    ui.quad(px, py, pw, 2.0f, hair);
    ui.text("RIFT OPERATIONS — GATE ANALYTICS", px + 14.0f, py + 10.0f, 17.0f, txt);
    ui.text("select gate: click (TAB+ENTER)    [E] stand up",
            px + pw - 340.0f, py + 12.0f, 12.0f, dim);

    // LEFT: one status row per gate — real BUTTONS (click / TAB+ENTER selects).
    // Name from the SAME resolver the hanging glass uses; health condensed to a
    // word + a stability percentage read straight off the live flux lane
    // (mean of the last 30 samples).
    const float rowX = px + 14.0f, rowW = pw * 0.36f;
    float rowY = py + 40.0f;
    for (int i = 0; i < (int)m_portals.size(); ++i) {
        const RiftPortal& p = m_portals[(size_t)i];
        float mean = 0.0f;
        for (uint32_t s = 0; s < 30u; ++s)
            mean += fluxSample((uint32_t)i, m_fluxHead + kFluxSamples - s);
        mean /= 30.0f;
        const bool hot = p.kawoosh > 0.0f || p.snarl > 0.0f || p.implode > 0.0f;
        char row[96];
        std::snprintf(row, sizeof(row), "G%d  %-14s %s %3d%%",
                      i + 1, riftDestName(p.destination).c_str(),
                      p.dead ? "DOWN" : (hot ? "SURGE" : "STABLE"),
                      (int)std::lround(mean * 100.0f));
        if (i == m_opsSel) {
            const float sel[4] = { 0.08f, 0.22f, 0.26f, 0.9f };
            ui.quad(rowX - 6.0f, rowY - 3.0f, rowW, 21.0f, sel);
        }
        if (ui.button(row, rowX, rowY - 3.0f, rowW - 8.0f, 21.0f))
            m_opsSel = i;
        (void)bad; (void)txt;
        rowY += 22.0f;
    }

    // RIGHT: the selected gate, deep — the readout builder's OWN lines (one
    // truth with the hanging glass) + the live flux waveform.
    const float dx0 = px + pw * 0.42f, dw = pw * 0.55f;
    float dy = py + 40.0f;
    ui.text(riftDestName(m_portals[(size_t)m_opsSel].destination).c_str(),
            dx0, dy, 16.0f, txt);
    dy += 26.0f;
    for (const std::string& line : consoleReadout((uint32_t)m_opsSel)) {
        ui.text(line.c_str(), dx0, dy, 12.0f, dim);
        dy += 17.0f;
        if (dy > py + ph - 96.0f) break;   // leave the waveform its band
    }
    // Waveform: 96 bars, oldest -> newest left to right, head marked.
    const float wy = py + ph - 84.0f, wh = 64.0f;
    const float bw = dw / (float)kFluxSamples;
    ui.quad(dx0, wy, dw, wh, glass);
    ui.quad(dx0, wy + wh, dw, 1.0f, hair);
    for (uint32_t s = 0; s < kFluxSamples; ++s) {
        const uint32_t idx = (m_fluxHead + 1u + s) % kFluxSamples;
        const float v = fluxSample((uint32_t)m_opsSel, idx);
        const float bh = std::max(1.5f, v * wh);
        const bool newest = (idx == m_fluxHead);
        ui.quad(dx0 + (float)s * bw, wy + wh - bh, std::max(1.0f, bw - 1.0f), bh,
                newest ? ok : hair);
    }
    ui.text("FLUX  (live, 3.2 s window)", dx0, wy - 16.0f, 11.0f, dim);
}

float Rifthub::timeScale() const {
    if (m_temporal <= 0.0f) return 1.0f;
    // TEMPORAL RIFT: deep slow-motion with a STUTTER — time does not merely slow, it
    // catches and skips. Eased in and out so the world never snaps back to speed.
    const float t = 1.0f - m_temporal / kTemporalDur;      // 0 -> 1
    float env = (t < 0.10f) ? (t / 0.10f)
                            : (t > 0.80f ? (1.0f - t) / 0.20f : 1.0f);
    if (env < 0.0f) env = 0.0f;
    const float stut = std::sin(m_time * 6.2831853f * kTemporalStutHz);
    // A hard quantized hitch ON TOP of the slow-mo: the world advances in visible
    // lurches (the brief's "stuttered/echoed motion").
    const float hitch = (stut > 0.55f) ? 2.6f : (stut < -0.75f ? 0.15f : 1.0f);
    const float slow  = 1.0f + (kTemporalSlow - 1.0f) * env;
    return slow * hitch;
}

float Rifthub::fovOffset() const {
    if (m_warp <= 0.0f) return 0.0f;
    // ROOM WARP's LENS: the FOV breathes, so what the player looks at stretches and
    // compresses even where it is not physically moving. Paired with the props'
    // travelling ripple in tick(), this is what makes the hall feel BENT.
    const float t = 1.0f - m_warp / kWarpDur;
    float env = (t < 0.12f) ? (t / 0.12f)
                            : (t > 0.75f ? (1.0f - t) / 0.25f : 1.0f);
    if (env < 0.0f) env = 0.0f;
    const float breathe = std::sin(m_time * 6.2831853f * 0.33f);
    return kWarpFovDeg * env * (0.35f + 0.5f * breathe);
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
// (This used to be a hand-copied 8-name list that had drifted: it swore act2caves
//  and act2 were real `--world` targets long after their hosts were gone, so T2
//  PASSED while two gates signposted worlds that could not be launched. It now asks
//  the DESTINATION REGISTRY, which self-tests its own world flags against the real
//  dispatch (runDestinationsSelfTest D2). One list, and it is checked.)
bool isKnownWorldTarget(const char* w) {
    return w && findDestination(w) != nullptr;
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

    // T1 — each portal owns valid, in-range spans for the gate, the recessed
    //      indicator track, and the membrane. (ROUND 6 killed the core disks;
    //      ROUND 7 killed the CHEVRONS — so neither has a span any more, and this
    //      test is the thing that would notice if one crept back.)
    {
        const uint32_t sceneN = scene.size();
        bool ok = sceneN > 0;
        for (uint32_t i = 0; i < hub.portalCount(); ++i) {
            const RiftPortal& p = hub.portal(i);
            if (p.ringEntCount == 0) ok = false;
            if (p.membraneEntCount == 0 || p.trackEntCount == 0) ok = false;
            if (p.ringEntFirst + p.ringEntCount > sceneN)         ok = false;
            if (p.trackEntFirst + p.trackEntCount > sceneN)       ok = false;
            if (p.membraneEntFirst + p.membraneEntCount > sceneN) ok = false;
        }
        rhCheck(ok, "T1 every portal owns valid gate/track/membrane entity spans");
    }

    // T2 — every gate's default aim is a REAL place in the destination registry.
    {
        bool ok = true;
        for (uint32_t i = 0; i < hub.portalCount(); ++i)
            if (!isKnownWorldTarget(hub.portal(i).worldName) ||
                !findDestination(hub.destination(i))) ok = false;
        rhCheck(ok, "T2 all 8 gates default-aim at real registry destinations");
    }

    // T2b — THE REGISTRY ITSELF (D0..D6): the one table the menu, the consoles and
    //       the host's fast-travel resolver all read.
    rhCheck(runDestinationsSelfTest(), "T2b the destination registry self-test passes");

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

    // T10 — MEMBRANE FLIPBOOK (ROUND 4 J2), tested while every portal is still
    //       DORMANT: with the atlas present, ticking advances the plasma disk's
    //       emissive texture through the flip frames (the handle CHANGES across
    //       0.5 s at kFlipFps); with the atlas absent (fresh clone / LFS stub)
    //       the procedural nebula must HOLD (same handle) — the graceful
    //       fallback seam is exercised on whichever side this box is on.
    {
        const uint32_t plasmaEnt = hub.portal(0).membraneEntFirst + 0;   // v3: [0] = plasma
        const uint32_t tex0 = scene.entities()[plasmaEnt].emissiveTex.id;
        for (int s = 0; s < 30; ++s) hub.tick(1.0f / 60.0f, scene);
        const uint32_t tex1 = scene.entities()[plasmaEnt].emissiveTex.id;
        const bool ok = (hub.flipbookFrames() > 0) ? (tex1 != tex0 && tex1 != 0)
                                                   : (tex1 == tex0 && tex1 != 0);
        rhCheck(ok, hub.flipbookFrames() > 0
                        ? "T10 membrane flipbook: IDLE plasma frame advances (atlas active)"
                        : "T10 membrane flipbook: atlas absent -> procedural nebula holds");
    }

    // Capture the IDLE-state plasma texture id before any trigger fires (T8
    // asserts the OPEN state swapped it to the throat texture).
    const uint32_t idlePlasmaTexId =
        scene.entities()[hub.portal(0).membraneEntFirst + 0].emissiveTex.id;

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
            // The activated prompt must say it is OPEN *and* name where it goes — the
            // old "Rift activated: <world flag>" said neither truthfully once gates
            // could be re-aimed.
            if (!sawId || !hub.portal(i).activated ||
                prompt.find("Rift OPEN") == std::string::npos ||
                prompt.find(riftDestName(hub.destination(i))) == std::string::npos)
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
        const uint32_t plasmaEnt = p.membraneEntFirst + 0;
        const uint32_t rimEnt    = p.membraneEntFirst + 1;
        const float ring0 = scene.entities()[p.ringEntFirst].emissive[3];
        const float trk0  = scene.entities()[p.trackEntFirst].emissive[3];
        const float rim0  = scene.entities()[rimEnt].emissive[3];
        const float mem0  = scene.entities()[plasmaEnt].emissive[3];
        const float rot0  = scene.entities()[plasmaEnt].transform[1];  // basis X.y (spins in-plane)
        // Advance ~a tenth of a second — enough for the pulse sines + the
        // kPlasmaSpinRadS rotation to move appreciably.
        hub.tick(0.1f, scene);
        const float ring1 = scene.entities()[p.ringEntFirst].emissive[3];
        const float trk1  = scene.entities()[p.trackEntFirst].emissive[3];
        const float rim1  = scene.entities()[rimEnt].emissive[3];
        const float mem1  = scene.entities()[plasmaEnt].emissive[3];
        const float rot1  = scene.entities()[plasmaEnt].transform[1];
        // (ROUND 6: the core-disk pulse is gone with the core disks; the RIM
        //  shimmer is now the second animated membrane layer this samples.)
        bool moved = std::fabs(rim1 - rim0) > 1e-4f &&
                     std::fabs(mem1 - mem0) > 1e-4f;
        // The TUBE is authored once and never animated: metal doesn't pulse. (The
        // only emissive parts of the gate are the panel's LCD + LEDs, which are
        // separate entities — see T14.)
        bool ringStatic = std::fabs(ring1 - ring0) < 1e-6f;
        // The recessed indicator track holds its OPEN glow under its cap.
        bool bounded = trk1 <= kTrackEmCap + 0.01f && trk0 <= kTrackEmCap + 0.01f;
        // The storm rotates: the plasma disk's basis actually turned.
        bool rotates = std::fabs(rot1 - rot0) > 1e-5f;
        rhCheck(moved && ringStatic && bounded && rotates,
                "T5 tick() advances rim + plasma storm; the TUBE stays static; disk rotates");
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
            const float plasma = ents[p1.membraneEntFirst + 0].emissive[3];
            const float rim    = ents[p1.membraneEntFirst + 1].emissive[3];
            if (plasma > kPlasmaEmCap + 1e-4f) underCap = false;
            if (rim    > kRimEmCap    + 1e-4f) underCap = false;
            // The recessed indicator track's chase obeys its cap too.
            for (uint32_t t3 = 0; t3 < p1.trackEntCount; ++t3)
                if (ents[p1.trackEntFirst + t3].emissive[3] > kTrackEmCap + 1e-4f)
                    underCap = false;
            // Phase D: the orange conduit flow obeys its cap under the surge.
            for (uint32_t t3 = 0; t3 < p1.conduitEntCount; ++t3)
                if (ents[p1.conduitEntFirst + t3].emissive[3] > kConduitEmCap + 1e-4f)
                    underCap = false;
            const Entity& pe = ents[p1.membraneEntFirst + 0];
            if (!(pe.emissive[2] > pe.emissive[0] && pe.emissive[2] > pe.emissive[1]))
                blueDominant = false;
        }
        rhCheck(underCap && blueDominant,
                "T6 emissive cap law: kawoosh surge stays <= caps, blue dominant");
    }

    // T7 — membrane FX law (ROUND 5, the owner's "2nd grade crayon" verdict on the
    //      procedural bolts): the baked FLIPBOOK already carries the reference
    //      video's own lightning, so NO procedural arcs may be drawn over a settled
    //      membrane. Arcs are a SURGE-ONLY event (the rim vortex the flipbook does
    //      not contain). Assert both halves:
    //        (a) after the T6 surges have decayed, every portal has ZERO live arcs
    //            (idle + open membranes are left alone), while spark motes still
    //            bleed off the storm;
    //        (b) firing a fresh kawoosh spawns arcs again within a tick.
    {
        uint32_t settledArcs = 0;
        for (uint32_t i = 0; i < hub.portalCount(); ++i) settledArcs += hub.liveArcCount(i);
        const bool motesAlive = hub.liveMoteCount() > 0;
        RiftPortal& p2 = const_cast<RiftPortal&>(hub.portal(2));
        p2.kawoosh = kKawooshDur;
        hub.tick(1.0f / 60.0f, scene);
        const bool surgeArcs = hub.liveArcCount(2) > 0;
        rhCheck(settledArcs == 0 && motesAlive && surgeArcs,
                "T7 FX law: no arcs over a settled membrane (the flipbook's own "
                "lightning reads); the SURGE vortex still fires; motes alive");
    }

    // T8 — MEMBRANE STATE MACHINE (PortalAnimated.mp4 arc): after activation +
    //      surge decay the portal is OPEN — the plasma disk swapped from the
    //      idle book onto the THROAT (round 6: the OPEN flipbook, or the
    //      procedural throat map if that atlas is absent) AND the swapped-in
    //      texture is a VALID handle that still glows (the round-5 "activated
    //      portal goes black" regression: a throat swap that lands an invalid or
    //      unlit disk must FAIL here, not in the owner's face). All portals were
    //      entered in T4, so all 8 must be in the OPEN state by now.
    {
        bool swapped = true, lit = true;
        for (uint32_t i = 0; i < hub.portalCount(); ++i) {
            const RiftPortal& p = hub.portal(i);
            const Entity& plasma = scene.entities()[p.membraneEntFirst + 0];
            if (plasma.emissiveTex.id == idlePlasmaTexId) swapped = false;
            if (!p.throatOn) swapped = false;
            // The OPEN membrane must still be an EMITTER: valid emissive map,
            // non-trivial strength, blue-dominant tint.
            if (!plasma.emissiveTex.valid() || !plasma.tex.valid())   lit = false;
            // Floor = the OPEN base at the BOTTOM of its breathe wobble (round 6
            // re-tuned the base DOWN to 1.58 so the footage's own contrast reads;
            // the old kPlasmaEmBase floor sat above the wobble trough).
            if (plasma.emissive[3] < kPlasmaEmBaseOpen - kPlasmaEmWobble - 1e-3f)
                lit = false;
            if (!(plasma.emissive[2] > plasma.emissive[0]))           lit = false;
        }
        rhCheck(swapped && lit,
                "T8 state machine: OPEN throat swapped in AND still lit (never a black void)");
    }

    // T12 — THE OPEN STATE IS REAL FOOTAGE (round 6; the owner's "the swirling one
    //       looks fake"). OPEN used to BE makeThroatRGBA() — a hand-coded polar
    //       spiral — playing next to seven gates running his reference video. It
    //       now plays membrane_flipbook_open.png (the video's own throat span,
    //       t 8.4-9.95 s) through the SAME playback path as IDLE. Both sides of
    //       the LFS seam are asserted:
    //         atlas PRESENT -> the OPEN membrane's texture ADVANCES between ticks
    //                          (it is a film, not a still) and stays a valid,
    //                          blue-dominant emitter;
    //         atlas ABSENT  -> it HOLDS one valid handle (the procedural throat
    //                          fallback): degraded, never broken or black.
    //       Portal 0 has been OPEN since T4 (its surge decayed inside T5).
    {
        const uint32_t plasmaEnt = hub.portal(0).membraneEntFirst + 0;
        const uint32_t t0 = scene.entities()[plasmaEnt].emissiveTex.id;
        for (int s = 0; s < 12; ++s) hub.tick(1.0f / 60.0f, scene);  // 0.2 s: >=3 flip frames
        const Entity& e = scene.entities()[plasmaEnt];
        const uint32_t t1 = e.emissiveTex.id;
        const bool openState = hub.portal(0).throatOn;
        const bool livesOn   = e.emissiveTex.valid() && e.tex.valid() &&
                               e.emissive[2] > e.emissive[0];
        const bool ok = openState && livesOn &&
                        ((hub.openFlipbookFrames() > 0) ? (t1 != t0) : (t1 == t0));
        rhCheck(ok, hub.openFlipbookFrames() > 0
                        ? "T12 OPEN plays the reference-video OPEN flipbook (frame "
                          "advances; no hand-coded spiral)"
                        : "T12 OPEN atlas absent -> procedural throat fallback holds "
                          "(degraded, never black)");
    }

    // T13 — THE SURGE IS REAL FOOTAGE TOO, and it is a ONE-SHOT: with the surge
    //       atlas present, re-firing a kawoosh must put the membrane on a frame of
    //       the SURGE book that is NOT where it settles, and the film must run
    //       FORWARD (a later sample differs from an earlier one) before handing
    //       back to the OPEN loop when the surge ends. With the atlas absent the
    //       surge simply shows the OPEN/throat texture — never a black disk.
    {
        RiftPortal& p3 = const_cast<RiftPortal&>(hub.portal(3));
        p3.kawoosh = kKawooshDur;
        const uint32_t plasmaEnt = p3.membraneEntFirst + 0;
        hub.tick(1.0f / 60.0f, scene);
        const uint32_t s0 = scene.entities()[plasmaEnt].emissiveTex.id;
        for (int s = 0; s < 30; ++s) hub.tick(1.0f / 60.0f, scene);   // mid-surge
        const uint32_t s1 = scene.entities()[plasmaEnt].emissiveTex.id;
        const bool valid = scene.entities()[plasmaEnt].emissiveTex.valid();
        // Let the surge finish and confirm the gate returns to the OPEN state.
        for (int s = 0; s < 150; ++s) hub.tick(1.0f / 60.0f, scene);
        const bool backOpen = hub.portal(3).throatOn && p3.kawoosh <= 0.0f &&
                              scene.entities()[plasmaEnt].emissiveTex.valid();
        const bool ok = valid && backOpen &&
                        ((hub.surgeFlipbookFrames() > 0) ? (s1 != s0) : true);
        rhCheck(ok, hub.surgeFlipbookFrames() > 0
                        ? "T13 SURGE plays the reference-video SURGE flipbook once, "
                          "then hands back to the OPEN loop"
                        : "T13 SURGE atlas absent -> falls back to the OPEN/throat map "
                          "(never black), then settles OPEN");
    }

    // T11 — TWO-SIDED MEMBRANE LAW (round 5, the owner's "from behind there is
    //       no portal swirl" + "the activated portal is a BLACK VOID"): ONE root
    //       cause — an opaque VISTA disk parked outward of the plasma disk. Two
    //       assertions lock the fix:
    //       (a) the membrane span is exactly [plasma, rim] — no third opaque
    //           disk may be re-introduced outward of the storm;
    //       (b) the shared membrane disk mesh is DOUBLE-WOUND (every front
    //           triangle has its reverse-wound twin), so the one plasma entity
    //           renders from both sides.
    {
        bool spanOk = true;
        for (uint32_t i = 0; i < hub.portalCount(); ++i)
            if (hub.portal(i).membraneEntCount != 2) spanOk = false;
        // (b) verify the mesh the hub authors, straight from the generator.
        const x3::prims::PrimMesh disk = makeMembraneDisk(kMembraneR, kMembraneDiskSegs);
        bool twoSided = (disk.index.size() == (size_t)kMembraneDiskSegs * 6u);
        for (uint32_t s = 0; s < kMembraneDiskSegs && twoSided; ++s) {
            const uint32_t* f = &disk.index[(size_t)s * 6u];
            // front (0,a,b) then back (0,b,a) — the reversed winding.
            if (!(f[0] == f[3] && f[1] == f[5] && f[2] == f[4])) twoSided = false;
        }
        rhCheck(spanOk && twoSided,
                "T11 two-sided membrane: span is [plasma,rim] only + the disk is double-wound");
    }

    // T9 — GATE MESH SOURCE (ROUND 3): the gate NEVER has an empty ring span.
    //      If the Blender-authored GLB loaded (gateGlbActive), every portal's
    //      ring span is the same drawable count (all 8 instance ONE model);
    //      otherwise the procedural fallback authored exactly its 1 torus
    //      entity per portal — i.e. a missing/failed gate_ring.glb degrades
    //      gracefully to the round-2 ring instead of breaking the world.
    {
        bool ok = true;
        const uint32_t expect = hub.gateGlbActive() ? hub.portal(0).ringEntCount : 1u;
        if (expect == 0) ok = false;
        for (uint32_t i = 0; i < hub.portalCount(); ++i)
            if (hub.portal(i).ringEntCount != expect) ok = false;
        rhCheck(ok, hub.gateGlbActive()
                        ? "T9 gate GLB active: uniform authored-mesh span on all 8 portals"
                        : "T9 gate GLB absent: procedural fallback ring authored (1 span each)");
    }

    // ======================= ROUND 8 ======================================

    // T14 — NO CHEVRONS, AND AN OPERATOR PANEL INSTEAD (round 7 addendum 2 + 8-B).
    //       The gate is ONE TUBE. Two things are asserted:
    //         (a) the RiftPortal struct no longer carries a chevron span at all
    //             (that is a compile-time fact — this test exists so the INTENT is
    //             recorded), and the tube body is NOT an emitter: with the GLB
    //             active, the first gate group's emissive is the texture-gated
    //             ambient term, never a self-lit glow;
    //         (b) when the authored GLB is live it ships the operator panel (an LCD
    //             + LED groups), those entities exist, and they ARE emissive — the
    //             R8-B law that emissive is confined to the screen and the LEDs.
    {
        bool ok = true;
        for (uint32_t i = 0; i < hub.portalCount(); ++i) {
            const RiftPortal& p = hub.portal(i);
            if (hub.gateGlbActive()) {
                if (!p.hasPanel) ok = false;                       // the bay must be there
                if (!p.panelScreenEnt || !p.panelLedEnt) ok = false;
                if (p.panelScreenEnt >= scene.size()) ok = false;
                if (p.panelLedEnt    >= scene.size()) ok = false;
                if (scene.entities()[p.panelScreenEnt].emissive[3] <= 0.0f) ok = false;
                if (scene.entities()[p.panelLedEnt].emissive[3]    <= 0.0f) ok = false;
            }
            // The tube body itself must never be a lamp.
            if (scene.entities()[p.ringEntFirst].emissive[3] > 0.35f) ok = false;
        }
        rhCheck(ok, hub.gateGlbActive()
                    ? "T14 one-tube gate: no chevrons, an LCD+LED operator panel, and "
                      "the tube body is not self-lit"
                    : "T14 one-tube gate (procedural fallback): the tube body is not self-lit");
    }

    // T15 — THE CONSOLES EXIST AND ARE REACHABLE (round 8-C/D). One hanging
    //       holoterminal per rift; standing at it puts it in [E] range; standing in
    //       the middle of the hub puts you at none of them.
    {
        bool ok = true;
        for (uint32_t i = 0; i < hub.portalCount(); ++i) {
            const RiftPortal& p = hub.portal(i);
            // The console hangs kConsoleStandoff hub-side of the gate.
            const x3::phys::Vec3 at{ p.worldPos.x - p.outX * 4.35f, 1.6f,
                                     p.worldPos.z - p.outZ * 4.35f };
            if (hub.consoleInRange(at) != (int)i) ok = false;
        }
        if (hub.consoleInRange(x3::phys::Vec3{ 0, 1.6f, 0 }) != -1) ok = false;  // hub center
        // Open / close bookkeeping.
        hub.openConsole(0);
        if (!hub.consoleOpen() || hub.activeConsole() != 0) ok = false;
        hub.closeConsole();
        if (hub.consoleOpen()) ok = false;
        rhCheck(ok, "T15 one console per rift: [E] range resolves to the right one, "
                    "open/close latches");
    }

    // T16 — THE TELEGRAPH IS WIRED TO THE MACHINE (addendum 3's whole point).
    //       Dial the OPEN console into danger and the TUBE's own panel must react:
    //       the LEDs shift toward red and the membrane's tint follows. This is what
    //       makes the danger readable BEFORE anything blows — if this test fails,
    //       the player gets no warning.
    {
        bool ok = true;
        const uint32_t idx = 5;
        hub.openConsole((int)idx);
        RiftPortal& p5 = const_cast<RiftPortal&>(hub.portal(idx));
        p5.console.reset();
        for (int s = 0; s < 60; ++s) hub.tick(1.0f / 60.0f, scene);   // settle calm
        const float ledG0 = p5.panelLedEnt ? scene.entities()[p5.panelLedEnt].emissive[1] : 1.0f;
        const float ledR0 = p5.panelLedEnt ? scene.entities()[p5.panelLedEnt].emissive[0] : 0.0f;
        const float snarl0 = p5.snarl;
        // Now wind the containment field off under full power — the coupling that
        // kills you. instability() saturates; the panel must go red.
        p5.console.value[RP_Power] = 0.95f;
        p5.console.value[RP_Containment] = 0.05f;
        for (int s = 0; s < 120; ++s) hub.tick(1.0f / 60.0f, scene);
        const float ledG1 = p5.panelLedEnt ? scene.entities()[p5.panelLedEnt].emissive[1] : 0.0f;
        const float ledR1 = p5.panelLedEnt ? scene.entities()[p5.panelLedEnt].emissive[0] : 1.0f;
        if (!(p5.snarl > snarl0 + 0.5f)) ok = false;          // the rift IS angrier
        if (p5.panelScreenEnt) {                              // ...and it SHOWS it
            if (!(ledR1 > ledR0 + 0.3f)) ok = false;          // red climbs
            if (!(ledG1 < ledG0 - 0.2f)) ok = false;          // green falls
        }
        hub.closeConsole();
        rhCheck(ok, "T16 telegraph: dialling into danger makes the tube's panel LEDs "
                    "go red and the rift snarl BEFORE anything fires");
    }

    // T17 — IMPLOSION IS REAL, AND IT IS PERMANENT (the headline catastrophe).
    //       Fire it on rift 6 and assert the whole chain:
    //         * the membrane INVERTS (its disk scale collapses toward zero),
    //         * debris is DRAGGED IN (a mote parked next to the gate accelerates
    //           toward it),
    //         * a shockwave lands (camera shake + damage flash),
    //         * and afterwards the gate is DEAD — dark membrane, dark track, no
    //           light in its bay — and it STAYS dead: a second engage does nothing.
    {
        const uint32_t idx = 6;
        RiftPortal& p6 = const_cast<RiftPortal&>(hub.portal(idx));
        const uint32_t plasmaEnt = p6.membraneEntFirst + 0;
        const float scale0 = std::sqrt(
            scene.entities()[plasmaEnt].transform[0] * scene.entities()[plasmaEnt].transform[0] +
            scene.entities()[plasmaEnt].transform[1] * scene.entities()[plasmaEnt].transform[1] +
            scene.entities()[plasmaEnt].transform[2] * scene.entities()[plasmaEnt].transform[2]);

        hub.applyOutcome(idx, RiftOutcome::Implosion);
        const bool armed = p6.implode > 0.0f;

        // Mid-collapse: the disk must be visibly SMALLER than it was.
        for (int s = 0; s < 100; ++s) hub.tick(1.0f / 60.0f, scene);   // ~1.7 s in
        const float scaleMid = std::sqrt(
            scene.entities()[plasmaEnt].transform[0] * scene.entities()[plasmaEnt].transform[0] +
            scene.entities()[plasmaEnt].transform[1] * scene.entities()[plasmaEnt].transform[1] +
            scene.entities()[plasmaEnt].transform[2] * scene.entities()[plasmaEnt].transform[2]);
        const bool inverting = scaleMid < scale0 * 0.6f && p6.implode > 0.0f;

        // Let it finish: shockwave + a dead gate.
        for (int s = 0; s < 60; ++s) hub.tick(1.0f / 60.0f, scene);
        const bool blew  = hub.shake() > 0.0f && hub.damageFlash() > 0.0f;
        const bool dead  = p6.dead && !p6.activated;
        const bool darkM = scene.entities()[plasmaEnt].emissive[3] <= 1e-4f;
        const bool darkT = scene.entities()[p6.trackEntFirst].emissive[3] <= 1e-4f;
        const bool darkL = hub.pointLights()[idx].color[0] <= 1e-4f &&
                           hub.pointLights()[idx].color[2] <= 1e-4f;
        // PERSISTENT: engaging a corpse changes nothing.
        hub.applyOutcome(idx, RiftOutcome::Nominal);
        hub.tick(1.0f / 60.0f, scene);
        const bool stillDead = p6.dead && !p6.activated &&
                              scene.entities()[plasmaEnt].emissive[3] <= 1e-4f;

        rhCheck(armed && inverting && blew && dead && darkM && darkT && darkL && stillDead,
                "T17 IMPLOSION: membrane inverts, shockwave lands, the gate goes DARK "
                "— and stays dead");
    }

    // T18 — ROOM WARP bends the hall AND puts it back. The props must physically
    //       move while it runs (and the lens must breathe), and when it ends every
    //       prop must be EXACTLY where it started — a warp that leaks drift would
    //       slowly destroy the level.
    {
        // Snapshot EVERY entity's translation. Do NOT guess which index is a hall
        // prop — the hub authors hundreds of entities and the layout moves with the
        // art (the first cut of this test hardcoded 40, which was a strip-light bar,
        // and the test failed for a reason that had nothing to do with the warp).
        const uint32_t n = scene.size();
        std::vector<float> before(n * 3);
        for (uint32_t e = 0; e < n; ++e) {
            before[e * 3 + 0] = scene.entities()[e].transform[12];
            before[e * 3 + 1] = scene.entities()[e].transform[13];
            before[e * 3 + 2] = scene.entities()[e].transform[14];
        }
        hub.applyOutcome(0, RiftOutcome::RoomWarp);
        bool lensMoved = false;
        for (int s = 0; s < 240; ++s) {                 // 4 s into a 9 s warp
            hub.tick(1.0f / 60.0f, scene);
            if (std::fabs(hub.fovOffset()) > 0.5f) lensMoved = true;
        }
        uint32_t movedCount = 0;
        for (uint32_t e = 0; e < n; ++e) {
            const float dx = scene.entities()[e].transform[12] - before[e * 3 + 0];
            const float dy = scene.entities()[e].transform[13] - before[e * 3 + 1];
            const float dz = scene.entities()[e].transform[14] - before[e * 3 + 2];
            if (std::fabs(dx) > 0.05f || std::fabs(dy) > 0.05f || std::fabs(dz) > 0.05f)
                ++movedCount;
        }
        // Run it out and confirm the hall is restored EXACTLY. A warp that leaked
        // drift would slowly destroy the level, one catastrophe at a time.
        for (int s = 0; s < 400; ++s) hub.tick(1.0f / 60.0f, scene);
        bool restored = std::fabs(hub.fovOffset()) < 1e-5f;
        for (uint32_t e = 0; e < n && restored; ++e)
            if (std::fabs(scene.entities()[e].transform[12] - before[e * 3 + 0]) > 1e-5f ||
                std::fabs(scene.entities()[e].transform[13] - before[e * 3 + 1]) > 1e-5f ||
                std::fabs(scene.entities()[e].transform[14] - before[e * 3 + 2]) > 1e-5f)
                restored = false;
        rhCheck(movedCount >= 12 && lensMoved && restored,
                "T18 ROOM WARP: the hall's props ripple + the lens breathes, and it "
                "all snaps back exactly when the warp ends");
    }

    // T19 — TEMPORAL RIFT distorts time, then gives it back. timeScale() must leave
    //       1.0 (slow-motion + a visible stutter: it is NOT a constant multiplier)
    //       and must return to exactly 1.0 when the event is over.
    {
        hub.applyOutcome(1, RiftOutcome::TemporalRift);
        float lo = 99.0f, hi = -99.0f;
        for (int s = 0; s < 240; ++s) {
            hub.tick(1.0f / 60.0f, scene);
            const float ts = hub.timeScale();
            if (ts < lo) lo = ts;
            if (ts > hi) hi = ts;
        }
        const bool slowed   = lo < 0.6f;              // it really does drag
        const bool stutters = (hi - lo) > 0.5f;       // ...and it lurches
        for (int s = 0; s < 600; ++s) hub.tick(1.0f / 60.0f, scene);
        const bool restored = std::fabs(hub.timeScale() - 1.0f) < 1e-6f;
        rhCheck(slowed && stutters && restored,
                "T19 TEMPORAL RIFT: time slows AND stutters, then returns to normal");
    }

    // T20 — THE OUTCOME TABLE IS THE GAMEPLAY (rift_console.cpp's own suite, run
    //       from here so one gate covers the whole feature).
    rhCheck(runRiftConsoleSelfTest(),
            "T20 console parameter->outcome table (data-driven; see riftconsole above)");

    // ======================= ROUND 9 ======================================
    // T21 — THE CONSOLE SCREENS ARE NOT BLANK. ***THE GUARD THAT SHOULD HAVE
    //       EXISTED NINE FIXES AGO.*** These consoles have rendered as featureless
    //       blue slabs over and over, and nothing ever failed, because "the panel
    //       exists" and "the panel SHOWS SOMETHING" were never the same assertion.
    //       They are now. Three things, on every one of the eight:
    //         (a) the screen entity carries the BAKED READOUT TEXTURE, actually bound
    //             (HoloTerminal::screenHasContent() — no texture, no lines, no
    //             rasterized glyphs => false);
    //         (b) the readout SAYS WHERE THE PORTAL GOES: the destination string is
    //             really on the glass, alongside a status and the parameter rows;
    //         (c) the baked pixels carry INK — the text zone of the hologram is
    //             measurably lit. A blank/flat panel probes at ~0 and FAILS here.
    {
        bool ok = true;
        if (hub.holoCount() != hub.portalCount()) ok = false;
        for (uint32_t i = 0; i < hub.holoCount() && ok; ++i) {
            const HoloTerminal& h = hub.holo(i);
            // (a) a real, bound, baked screen.
            if (!h.built() || !h.screenHasContent()) ok = false;
            // (b) it says WHERE THIS PORTAL GOES.
            const std::vector<std::string>& L = h.lines();
            bool saysDest = false, saysStatus = false, saysParam = false;
            for (const std::string& s : L) {
                // The glass carries the READABLE name of where the gate goes, not the
                // registry key it stores.
                if (s.find(riftDestName(hub.portal(i).destination)) != std::string::npos &&
                    s.find("DEST") != std::string::npos) saysDest = true;
                if (s.find("STATUS") != std::string::npos) saysStatus = true;
                if (s.find("CONT")   != std::string::npos) saysParam = true;
            }
            if (!saysDest || !saysStatus || !saysParam) ok = false;
            // (d) it is the TEXT-FIRST layout (the readout must be legible from [E]
            //     range, not a stamp in the corner of a schematic).
            if (h.layout() != HoloTerminal::Layout::Readout) ok = false;
            // (c) the BAKE actually puts ink on the glass (this is the anti-blank
            //     assertion: it fails on a panel that renders as a flat slab).
            if (holoReadoutInkFraction(L, "", /*wide*/true) < 0.01f) ok = false;
        }
        // ...and the probe must be able to FAIL — an empty readout has no ink. Without
        // this, (c) could be a test that passes on anything.
        if (holoReadoutInkFraction({ "", "" }, "", /*wide*/true) > 0.002f) ok = false;
        rhCheck(ok, "T21 the 8 console screens are NOT BLANK: baked readout texture "
                    "bound, destination/status/params on the glass, ink in the pixels");
    }

    hub.shutdown(device);
    phys->shutdown();

    // =======================================================================
    // W-RIFT — THE HUB IS A PLACE IN THE WORLD: the region build (origin +
    // doorway), the approach corridor's seams, the elevator's RIFT stop with its
    // 4790 lock, and the traversal query that makes the gates mean something.
    // A fresh physics world + scene: this is the CANON configuration, not the dev
    // world (which the 21 assertions above have already proven).
    // =======================================================================
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> p2(x3::phys::createPhysicsWorld());
        p2->init();
        HeadlessRenderDevice dev2;
        Scene sc2;
        TriggerSystem tr2;
        Rifthub hub2;

        // The canon placement, expressed the way app_run.cpp expresses it: the shaft at
        // the origin, the rift level at Y = -78, the hub 44.5 m west and 46 m north.
        const float SX = 0.0f, SZ = 0.0f, FY = -78.0f;
        Rifthub::Desc hd;
        hd.origin      = { SX - 44.5f, FY, SZ + 46.0f };
        hd.doorway     = true;
        hd.doorCenterX = 7.0f;
        hd.doorHalfW   = 1.7f;
        hd.doorH       = 3.4f;
        hub2.build(sc2, dev2, *p2, tr2, hd);

        // ---- T22: the REGION ORIGIN moved the whole hub, not just its meshes. -----
        {
            bool ok = hub2.built() && hub2.portalCount() == kPortalCount;
            const x3::phys::Vec3 sp = hub2.spawn();
            ok = ok && std::fabs(sp.x - hd.origin.x) < 0.01f &&
                       std::fabs(sp.z - hd.origin.z) < 0.01f &&
                       std::fabs(sp.y - (FY + 0.05f)) < 0.01f;
            // Every gate sits on the 14 m ring AROUND THE NEW CENTER (not the old one).
            for (uint32_t i = 0; i < hub2.portalCount(); ++i) {
                const RiftPortal& p = hub2.portal(i);
                const float dx = p.worldPos.x - hd.origin.x;
                const float dz = p.worldPos.z - hd.origin.z;
                const float r  = std::sqrt(dx * dx + dz * dz);
                if (std::fabs(r - kRingRadius) > 0.05f) ok = false;
                if (std::fabs(p.worldPos.y - FY) > 0.01f) ok = false;
                // The cached outward basis must still be radial from the hub center: the
                // membrane transform, the key lights and traversal all read it.
                if (std::fabs(p.outX - dx / kRingRadius) > 0.02f) ok = false;
                if (std::fabs(p.outZ - dz / kRingRadius) > 0.02f) ok = false;
            }
            // The light rig came with it (a rig left at the world origin lights nothing).
            bool lightsMoved = !hub2.pointLights().empty();
            for (const auto& L : hub2.pointLights())
                if (L.pos[1] > FY + 12.0f || L.pos[1] < FY - 2.0f) lightsMoved = false;
            ok = ok && lightsMoved;
            rhCheck(ok, "T22 REGION ORIGIN: the hub (gates, basis, spawn, light rig) is "
                        "authored at the canon origin - one build path, two worlds");
        }

        // ---- T23: the DOORWAY is a real hole in a still-solid wall. --------------
        {
            const x3::phys::Vec3 door = hub2.doorCenter();
            const x3::phys::RayHit thru = p2->rayCast(
                { door.x, FY + 1.6f, door.z - 2.0f }, { 0, 0, 1 }, 4.0f, x3::phys::Layer::Static);
            const x3::phys::RayHit jamb = p2->rayCast(
                { door.x + 6.0f, FY + 1.6f, door.z - 2.0f }, { 0, 0, 1 }, 4.0f, x3::phys::Layer::Static);
            const x3::phys::RayHit lintel = p2->rayCast(
                { door.x, FY + 5.0f, door.z - 2.0f }, { 0, 0, 1 }, 4.0f, x3::phys::Layer::Static);
            rhCheck(!thru.hit && jamb.hit && lintel.hit,
                    "T23 the hub's -Z wall carries a DOORWAY: open through the opening, "
                    "solid at the jamb, solid over the lintel");
        }

        // ---- T24: THE APPROACH. The landing + corridor seal onto that doorway. ----
        RiftDepths depths;
        DoorSystem depthDoors;   // T32: the side rooms author REAL registry doors
        {
            RiftDepths::Desc dd;
            dd.shaft     = { SX, FY, SZ };
            dd.hubDoor   = hub2.doorCenter();
            dd.doorHalfW = hd.doorHalfW;
            dd.doorH     = hd.doorH;
            dd.doors     = &depthDoors;
            depths.build(sc2, dev2, *p2, dd);
            const std::string seam = depths.selfCheck();
            rhCheck(depths.built() && seam.empty(),
                    "T24 THE APPROACH: landing + L-corridor authored, seams CLEAN "
                    "(floor continuous from the cab well to the hub threshold)");
        }

        // ---- T32: THE SIDE ROOMS + CARD READERS (owner 2026-08-30). --------------
        // Two rooms off leg A, each behind a keycard-locked slider with a reader
        // whose LED tracks the door's LIVE lock state — asserted, not assumed.
        {
            const auto& rds = depths.readers();
            rhCheck(rds.size() == 2 && depthDoors.count() >= 2,
                    "T32a two side rooms: two card readers, two registry doors");
            bool lockedRight = true, ledTracks = true;
            for (const auto& r : rds) {
                const Door& d = depthDoors.at(r.doorIdx);
                lockedRight = lockedRight && d.isLocked() &&
                              d.keycard == kKeycardSecurity;
                // LED: red while locked, green after a card-unlock.
                depths.syncReaders(sc2, depthDoors);
                const Entity& led0 = sc2.get(r.ledEnt);
                const bool redFirst = led0.emissive[0] > 0.8f && led0.emissive[1] < 0.3f;
                depthDoors.unlock(depthDoors.at(r.doorIdx));
                depths.syncReaders(sc2, depthDoors);
                const Entity& led1 = sc2.get(r.ledEnt);
                const bool greenAfter = led1.emissive[1] > 0.8f && led1.emissive[0] < 0.3f;
                depthDoors.lock(depthDoors.at(r.doorIdx));   // restore for later Ts
                depths.syncReaders(sc2, depthDoors);
                ledTracks = ledTracks && redFirst && greenAfter;
            }
            rhCheck(lockedRight, "T32b both side doors locked on the Security keycard");
            rhCheck(ledTracks,   "T32c reader LEDs track the LIVE lock: red locked, green unlocked");
        }

        // ---- T25: CONNECTIVITY - you can actually WALK it. -----------------------
        // Cast DOWN at every step along the route (floor under every footfall) and
        // FORWARD along each leg (nothing standing in the corridor). This is the
        // walkable proof the geometric lint gate gives the canon rooms.
        {
            bool ok = true;
            const auto& rt = depths.route();
            ok = ok && rt.size() >= 5;
            for (size_t i = 0; i + 1 < rt.size() && ok; ++i) {
                const x3::phys::Vec3 a = rt[i], b = rt[i + 1];
                const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
                const int steps = std::max(2, (int)(len / 1.0f));
                for (int st = 0; st <= steps; ++st) {
                    const float t = (float)st / (float)steps;
                    const float x = a.x + (b.x - a.x) * t;
                    const float z = a.z + (b.z - a.z) * t;
                    const x3::phys::RayHit down = p2->rayCast(
                        { x, FY + 1.2f, z }, { 0, -1, 0 }, 3.0f, x3::phys::Layer::Static);
                    if (!down.hit) { ok = false; break; }   // a hole in the walk
                }
                const float dxl = b.x - a.x, dzl = b.z - a.z;
                const float ln = std::sqrt(dxl * dxl + dzl * dzl);
                if (ln > 1.0f) {
                    const x3::phys::RayHit fwd = p2->rayCast(
                        { a.x, FY + 1.4f, a.z }, { dxl / ln, 0.0f, dzl / ln },
                        ln - 0.3f, x3::phys::Layer::Static);
                    if (fwd.hit) ok = false;   // something is standing in the corridor
                }
            }
            rhCheck(ok, "T25 CONNECTIVITY: elevator landing -> corridor -> bend -> hub "
                        "threshold is walkable (floor under every step, nothing in the way)");
        }

        // ---- T26: THE ELEVATOR'S RIFT STOP + the 4790 lock. ----------------------
        {
            ElevatorSystem lift;
            std::vector<float> stops = { FY + 0.15f, 0.15f, 5.15f, 10.15f };   // RIFT, F1, F2, F3
            lift.build(sc2, dev2, *p2, SX, SZ, 1.4f, 0.15f, 1.4f, stops, /*startStop*/1);
            lift.enableFsm(true);
            lift.setFloorLabels({ "RIFT", "F1", "F2", "F3" });
            lift.setRiftStop(0);
            bool ok = lift.built() && lift.stopLocked(0) && !lift.riftUnlocked();
            lift.callTo(0);                       // a call to the buried floor goes nowhere
            ok = ok && lift.targetStop() != 0;
            for (int i = 0; i < 6; ++i) lift.callNext();   // cycling never lands on it
            ok = ok && lift.targetStop() != 0;
            lift.keypadDigit(4); lift.keypadDigit(7); lift.keypadDigit(9);
            const bool wrong = lift.keypadDigit(1);        // 4791: still locked
            ok = ok && !wrong && !lift.riftUnlocked();
            lift.keypadDigit(4); lift.keypadDigit(7); lift.keypadDigit(9);
            const bool right = lift.keypadDigit(0);        // 4790: the floor appears
            ok = ok && right && lift.riftUnlocked() && !lift.stopLocked(0) &&
                 lift.targetStop() == 0;
            for (int i = 0; i < 6000 && std::fabs(lift.cabCenter().y - stops[0]) > 0.2f; ++i)
                lift.update(1.0f / 60.0f, sc2, *p2);
            const float arrY = lift.cabCenter().y;
            const float lip  = lift.cabTopY() - FY;   // cab deck vs landing deck
            ok = ok && std::fabs(arrY - stops[0]) < 0.25f;
            // The cab's floor must meet the landing at a STEP DOWN, never a climb. The
            // canon stop convention (cab center = floorY + cabHalfY) parks the cab deck
            // one cab thickness proud (0.30 m), and the FSM's arrival window adds a few
            // more centimetres — the same lip EVERY floor in this tower has. What matters
            // is that the rift deck is on the walk-off side of the door, not below it.
            ok = ok && lip > -0.02f && lip < 0.55f;
            if (!ok) x3::logWarn("[rifthub-test] T26 diag: target=" +
                                 std::to_string(lift.targetStop()) + " unlocked=" +
                                 std::to_string((int)lift.riftUnlocked()) + " arrY=" +
                                 std::to_string(arrY) + " want=" + std::to_string(stops[0]) +
                                 " lip=" + std::to_string(lip));
            rhCheck(ok, "T26 the RIFT STOP is a real floor behind code 4790: locked out of "
                        "the panel, opened by the code, and the cab rides all the way down to it, "
                        "level with the landing deck");
        }

        // ---- T27: TRAVERSAL. An OPEN rift takes you; a dormant/dead one does not. -
        {
            const RiftPortal& p0 = hub2.portal(0);
            const x3::phys::Vec3 inThroat{ p0.worldPos.x, FY + 2.2f, p0.worldPos.z };
            bool ok = hub2.traversalPortal(inThroat) < 0;          // dormant: no ride
            hub2.onTrigger(p0.triggerId);                          // walk in -> KAWOOSH
            for (int i = 0; i < 200; ++i) hub2.tick(1.0f / 60.0f, sc2);   // settle to OPEN
            ok = ok && hub2.traversalPortal(inThroat) == 0;        // open: it takes you
            ok = ok && hub2.traversalPortal({ hd.origin.x, FY + 1.6f, hd.origin.z }) < 0;
            // The host re-aims them with legacy prose; the gate CANONICALISES it onto
            // the registry key, so everything downstream is talking about one row.
            hub2.setDestination(0, "club 1127");
            ok = ok && hub2.destination(0) == "club";
            hub2.applyOutcome(0, RiftOutcome::Implosion);          // a COLLAPSED gate
            for (int i = 0; i < 400; ++i) hub2.tick(1.0f / 60.0f, sc2);
            ok = ok && hub2.portal(0).dead && hub2.traversalPortal(inThroat) < 0;
            rhCheck(ok, "T27 TRAVERSAL: an OPEN rift's throat resolves to its destination; "
                        "a dormant one and a COLLAPSED one take you nowhere");
        }

        // ---- W-MENU: THE HUB REACHES EVERY PLACE ------------------------------------
        // T28 — not "8 gates, 8 destinations". ONE gate, walked through the registry
        //       by the console's PREV/NEXT cycle, can be aimed at EVERY place the game
        //       has — and the hanging glass reads out each one truthfully.
        {
            bool ok = true;
            std::string cur = hub2.destination(1);
            for (uint32_t n = 0; n < x3::game::destinationCount(); ++n) {
                const Destination& d = x3::game::cycleDestination(cur, 1);
                hub2.setDestination(1, d.key);
                if (hub2.destination(1) != std::string(d.key)) ok = false;
                const std::vector<std::string> ro = hub2.consoleReadout(1);
                if (ro.size() < 2 || ro[1].find(d.name) == std::string::npos) ok = false;
                cur = d.key;
            }
            rhCheck(ok, "T28 one gate cycles onto EVERY registry destination and its "
                        "holoterminal reads out each one truthfully");
        }

        // T29 — a RE-TARGETED gate's traversal follows its NEW aim. (The payoff would
        //       be a lie if the gate still took you where it used to point.)
        {
            const RiftPortal& p2 = hub2.portal(2);
            const x3::phys::Vec3 inThroat{ p2.worldPos.x, FY + 2.2f, p2.worldPos.z };
            hub2.setDestination(2, "The Magma Zone");     // by READABLE NAME this time
            hub2.onTrigger(p2.triggerId);
            for (int i = 0; i < 200; ++i) hub2.tick(1.0f / 60.0f, sc2);
            const int tp = hub2.traversalPortal(inThroat);
            rhCheck(tp == 2 && hub2.destination(2) == "magma" &&
                    findDestination(hub2.destination(2)) != nullptr,
                    "T29 stepping through a RE-TARGETED gate resolves its NEW destination");
        }

        // T30 — the HUD prompt never lies about where a gate goes: re-aim it and the
        //       prompt the player reads while standing there changes with it.
        {
            hub2.setDestination(3, "city");
            std::string prompt;
            const RiftPortal& p3 = hub2.portal(3);
            const bool got = hub2.hudPromptForEye({ p3.worldPos.x, FY + 1.6f, p3.worldPos.z },
                                                  prompt, 6.0f);
            rhCheck(got && prompt.find("The City") != std::string::npos,
                    "T30 the HUD prompt names the gate's CURRENT destination");
        }

        // T31 — THE OPS STATION stands inside the shell, seats, and its flux
        //       telemetry LIVES. Containment: the seat is within the hub floor
        //       (|local| < kHubHalfM). FSM: in-range at the chair, out of range
        //       at the ring center; sit/stand round-trips. Telemetry: a second
        //       of tick() advances the ring head and the selected gate's lane
        //       holds real (non-constant) samples; a KILLED gate flatlines.
        {
            const auto seatEyeProbe = [&]() {
                float sp[3]; float yy = 0, pp = 0;
                hub2.opsEye(sp, yy, pp);
                return x3::phys::Vec3{ sp[0], sp[1], sp[2] };
            };
            const x3::phys::Vec3 se = seatEyeProbe();
            const bool contained =
                std::fabs(se.x - hub2.origin().x) < Rifthub::kHubHalfM &&
                std::fabs(se.z - hub2.origin().z) < Rifthub::kHubHalfM;
            rhCheck(contained, "T31a ops seat is INSIDE the hub shell (containment)");
            rhCheck(hub2.opsInRange({ se.x, se.y, se.z }) &&
                    !hub2.opsInRange({ hub2.origin().x, se.y, hub2.origin().z }),
                    "T31b ops [E] range: true at the chair, false at ring center");
            rhCheck(!hub2.opsSeated(), "T31c not seated at boot");
            hub2.sitOps();
            rhCheck(hub2.opsSeated(), "T31d sitOps seats");
            const uint32_t h0 = hub2.fluxHead();
            for (int i = 0; i < 60; ++i) hub2.tick(1.0f / 60.0f, sc2);
            rhCheck(hub2.fluxHead() != h0, "T31e flux ring advances under tick()");
            // Earlier Ts KILL gates (implosion consequences) and a dead gate
            // correctly FLATLINES — so assert the waveform on a LIVING gate.
            int live = -1;
            for (uint32_t gi = 0; gi < hub2.portalCount(); ++gi)
                if (!hub2.portal(gi).dead) { live = (int)gi; break; }
            float mn = 1e9f, mx = -1e9f;
            if (live >= 0) {
                for (uint32_t s = 0; s < Rifthub::kFluxSamples; ++s) {
                    const float v = hub2.fluxSample((uint32_t)live, s);
                    mn = std::min(mn, v); mx = std::max(mx, v);
                }
            }
            rhCheck(live >= 0 && mx > mn + 0.01f,
                    "T31f a LIVING gate's flux lane carries a live waveform "
                    "(dead gates flatline by design)");
            hub2.standOps();
            rhCheck(!hub2.opsSeated(), "T31g standOps stands");
        }

        depths.shutdown(dev2);
        hub2.shutdown(dev2);
        p2->shutdown();
    }

    x3::logInfo("rifthub: " + std::to_string(rh_pass) + "/" +
                std::to_string(rh_pass + rh_fail) + " passed");
    return rh_fail == 0;
}

} // namespace x3::game
