// app/space/wormhole.h
//
// SPACE WORMHOLES — the Bajoran read.
//
// WHAT THIS IS, AND WHAT IT IS NOT
// --------------------------------
// `app/space/wormhole_vfx.*` (the Salvari crystal-matrix TUNNEL) already existed
// on this line: it is the INSIDE of the jump — a faceted tube the camera flies
// down while `wormhole_transit` runs. It is kept, used, and not rewritten.
//
// What did NOT exist was the wormhole as a THING IN SPACE — an object you fly
// toward, that OPENS in front of you as a staged event, lights your hull as it
// does, and can be entered. `--world space` had no wormhole entities at all, so
// the comms device's wormhole-stability advisory (feat/ship-comms) was exercised
// only by the rift hub and by tests. This file is that missing entity.
//
// WHAT WAS LIFTED FROM THE RIFT HUB (app/rifthub.*), NOT DUPLICATED
// -----------------------------------------------------------------
// The rift hub's Stargate gates are the house style for "a portal that opens":
//   * The STAGED-EVENT SHAPE. The hub's membrane is a 3-state machine
//     (IDLE -> SURGE -> OPEN) driven by a decaying `kawoosh` timer. That is the
//     right idea with the wrong granularity for a thing seen from 800 m in open
//     space, where the opening IS the shot. Generalised here into an explicit
//     6-phase machine (Dormant/Spark/Bloom/Unfurl/Held/Closing) that carries the
//     hub's arc — a surge that escalates ONCE, then a settled throat that loops —
//     and adds the two beats space needs: the point of light before the bloom,
//     and a real collapse at the end.
//   * The EMISSIVE CAP LAW. The hub learned the hard way (its "blown-white v1
//     fix") that every emissive write must be clamped to a per-layer cap or the
//     tonemapper paints a flat white disc. kLayerEmissiveCap / kCoreEmissiveCap
//     below are that law, restated for this effect. Drive HARD, but drive capped.
//   * The PER-GATE POINT LIGHT. The hub pulses a blue light per portal so the
//     gate lights the room. That coupling is most of why the shot reads real, so
//     it is kept and made the headline feature (see `collectLights`).
//   * PROCEDURAL BAKES over an asset. The hub's real-footage flipbook atlas is
//     an authored asset keyed to an interior gate; its procedural nebula/throat
//     maps are the fallback. In space there is no atlas to key to, so the
//     procedural path is the ONLY path here — same technique, new formula.
//
// WHAT IS GENUINELY NEW
// ---------------------
//   * REAL THROAT DEPTH. The hub's membrane is one flat double-wound disk — it
//     is seen through a torus you stand next to, so a disk is enough. Seen from
//     open space it would read as a billboard sticker. So the throat here is a
//     STACK of kThroatLayers concentric annuli receding along the axis with
//     shrinking radii: a funnel of real geometry at real depths. Each layer gets
//     its own rotation rate and its own spectral tint, so parallax and rolling
//     internal motion fall out of the LAYER TRANSFORMS rather than out of a
//     shader the RHI does not expose (drawMesh*/drawMeshGlass only — same
//     constraint sky_stars and wormhole_vfx were built under).
//   * STABILITY AS A VISUAL. `Destination::stable` existed as a data field that
//     only the comms text read. Here an unstable wormhole physically wavers: the
//     aperture radius beats against itself, the core intensity drops out, and the
//     spectral ramp pushes off blue toward violet/magenta. Stable is steady and
//     inviting; unstable reads as dangerous.
//
// DT LAW (165 Hz)
// ---------------
// Every animated quantity is a function of ACCUMULATED TIME, never of a frame
// count. The phase machine DRAINS its dt in a loop, carrying the remainder into
// the next phase, so one 100 ms hitch steps through as many phases as it should
// and lands at exactly the same phase time a 165 Hz run would. The waver/flicker
// noise is a deterministic hash of accumulated time, not a per-frame rand(), so
// two framerates sample the SAME curve. `--test-wormholes` asserts a 60 Hz run
// and a 165 Hz run agree to within float tolerance at matched wall-clock times.
//
// Clean-room: original procedural work built on X3Native's own systems.

#pragma once

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <vector>

namespace x3::game { struct CommsPortal; }

namespace x3::space {

// ---------------------------------------------------------------------------
// THE PHASE MACHINE — the staged opening. This is the whole point: a wormhole
// must OPEN as an event, not exist as a prop.
//
//   Dormant : not there. Nothing drawn, no light, no comms row.
//   Spark   : a single point of light. Sub-pixel-ish core, no throat yet.
//   Bloom   : the point flares OUT — bright, radially symmetric, still no depth.
//   Unfurl  : the aperture opens; the layered throat scales in from nothing and
//             the interior becomes visible. This is the "unfurling" beat.
//   Held    : the settled throat. Loops until close() (or heldSec elapses).
//   Closing : the aperture collapses back down and the light dies.
//
// The arc is deliberately the rift hub's IDLE->SURGE->OPEN escalation with the
// two space-specific beats added at the ends.
// ---------------------------------------------------------------------------
enum class WormholePhase : uint8_t {
    Dormant = 0,
    Spark   = 1,
    Bloom   = 2,
    Unfurl  = 3,
    Held    = 4,
    Closing = 5,
};

const char* wormholePhaseName(WormholePhase p);

// How many concentric annuli make the throat. Each is real geometry at a real
// depth — this is what buys parallax instead of a billboard.
//
// WAS 30, NOW 14. Thirty was the count the OPAQUE layered throat needed, and it
// still stepped: opaque annuli OVERWRITE rather than composite, so every ring's
// inner edge was a hard discontinuity and you could count the rings. Piling on
// more rings could only make the steps finer, never remove them. Now that the
// throat draws through the alpha-blended MEMBRANE path (drawMeshGlass with
// GlassMaterial::lens/::shimmer), the rings COMPOSITE, so the count is free to
// drop to whatever tiles the disc — and 14 wide rings on a GEOMETRIC taper tile
// it with no gaps at the deep end, which 30 narrow rings on a LINEAR taper did
// not (see kRingInner in wormhole.cpp for that arithmetic). Fewer, better layers.
constexpr int kThroatLayers = 14;

// ---- THE PER-LAYER MEMBRANE LAW (the refraction pass, P1.5) ---------------
// Geometry + refraction weights for one throat annulus. A PURE FUNCTION of the
// layer index: no time, no frame counter, no wormhole state. That is what makes
// it testable, and it is why a 60 Hz and a 165 Hz run build byte-identical
// membranes (the only time-varying part of the effect is the shader's
// turbulence, which is driven by ACCUMULATED SECONDS, not by frames).
//
// `outerFrac`/`innerFrac` are the ring's radii as a fraction of the MOUTH
// radius. They exist so the tiling law can be asserted rather than asserted-in-a-
// comment: consecutive rings must OVERLAP for every layer, or the throat shows
// the concentric gaps that the 30-layer linear taper had at its deep end.
struct ThroatLayerMembrane {
    float depth01;      // 0 = mouth, 1 = the convergence end
    float outerFrac;    // ring outer radius / mouth radius
    float innerFrac;    // ring inner radius / mouth radius
    float alpha;        // blend coverage handed to GlassMaterial::opacity
    float lens;         // gravitational-lens weight  (GlassMaterial::lens)
    float shimmer;      // turbulence weight          (GlassMaterial::shimmer)
    float roughness;    // frost / scatter through the membrane
};
ThroatLayerMembrane throatLayerMembrane(int layer);

// ---------------------------------------------------------------------------
// THE EVENT HORIZON — WHY A WORMHOLE IS A SPHERE AND NOT A DISC
// ---------------------------------------------------------------------------
// The refraction pass closed the stepping and the discrete-hoop artifacts and
// then reported the one defect it could not close: at exactly 90 degrees the
// wormhole was essentially invisible, light spill only. `facingFade` is what
// makes that true — and it was the RIGHT fix for a stack of parallel discs,
// because a stack of parallel discs seen edge-on genuinely is a slinky and
// fading it out was better than showing it.
//
// But it treats the symptom. A real wormhole HAS NO EDGE-ON VIEW. It is not an
// aperture cut in a plane; it is a SPHERICAL EVENT HORIZON — a ball of radius
// r_s whose surface carries the gravitationally lensed image of the far side.
// There is no orientation from which it foreshortens, because a sphere's
// silhouette is a circle of the same radius from every direction. That is
// exactly why Interstellar's Gargantua and the Bajoran wormhole read as holes in
// space rather than as portals on a wall, and it is the whole difference between
// "it vanishes at 90 degrees" and "it reads as a hole from anywhere".
//
// THE GEOMETRY CHOSEN: A BILLBOARDED IMPOSTOR, WHICH IS NOT AN APPROXIMATION.
// A sphere of radius R projects to a disc of radius R for every camera. So a
// CAMERA-FACING disc of radius R is not a cheap stand-in for the sphere's
// silhouette — it IS the sphere's silhouette, exactly, at every angle, for one
// draw instead of a tessellated shell's hundreds. Three properties fall out that
// a real shell would have had to fight for:
//
//   * NO DEGENERATE VIEW BY CONSTRUCTION. There is no angle at which a billboard
//     foreshortens, so 0 / 45 / 70 / 90 / 180 degrees are geometrically the same
//     shot. That is the acceptance test, satisfied by the geometry rather than
//     by a tuning curve.
//   * THE MEMBRANE SHADER'S RADIAL FRAME NEVER COLLAPSES. glass.frag takes the
//     screen-space gradient of v as its outward radial axis. On the axis-aligned
//     throat annuli that gradient degenerates as the ring goes edge-on — which is
//     the deeper reason the side view had nothing to show. On a billboard, v's
//     screen-space gradient is the true screen radial direction always.
//   * A REAL SHELL WOULD HAVE BROKEN THAT FRAME. A UV sphere's v is latitude
//     from a pole; seen from the side its screen gradient runs top-to-bottom, not
//     outward from the silhouette centre, so the lens would smear vertically
//     instead of wrapping. The impostor is the only form of this that keeps the
//     angle-agnostic property the refraction lane was built on.
//
// WHAT THE THROAT IS FOR NOW. The layered funnel is kept, unchanged, with its
// grazing fade intact: head-on it is the THROAT YOU FALL INTO, and the staged
// opening and the approach shot are its job. The horizon is the object; the
// throat is what you see through it when you are looking down it. They
// cross-fade: the throat owns the head-on read, the horizon interior ramps up as
// the throat's read runs out, and the ring and halo below never fade at all.
// ---------------------------------------------------------------------------

// The impostor disc's radius as a fraction of the mouth radius. 1.0 — the
// horizon is the aperture, not a shell around it.
constexpr float kHorizonDiscFrac = 1.00f;

// The LENSING HALO is one annulus spanning kHaloInnerFrac..kHaloOuterFrac of the
// mouth radius, and it carries the EINSTEIN RING as its own brightest band
// (peaking at kEinsteinFrac). They are one object because they are one physical
// thing: the ring is not a decoration drawn on the halo, it is the place in the
// halo where the 1/theta deflection law piles a wide annulus of background sky
// into a narrow bright band. Splitting them into two draws would have meant two
// meshes, two textures and a hand-tuned brightness relationship between them.
//
// The inner edge sits slightly INSIDE the horizon so the deflection's peak lands
// under the silhouette edge rather than beside it — the smear has to cross the
// rim, or the ring reads as a painted-on outline.
constexpr float kHaloInnerFrac = 0.86f;
constexpr float kHaloOuterFrac = 1.62f;
constexpr float kEinsteinFrac  = 1.02f;

// Interior coverage of the horizon disc. HEAD-ON it is a faint wash BEHIND the
// throat (the throat is the read at that angle and the disc must not veil it);
// GRAZING it is the whole wormhole. The ramp is 1 - facingFade, so it is exactly
// complementary to the throat's own fade and the two never both fall away.
constexpr float kHorizonAlphaHeadOn  = 0.20f;
constexpr float kHorizonAlphaGrazing = 0.74f;

// Coverage of the lensing halo / Einstein ring. FLAT IN ANGLE — no facingFade
// term reaches these, by design. This is the element that answers "make sure the
// wormhole is visible from the event horizon much like a real one": a real one's
// ring does not know which way you are looking at it.
constexpr float kEinsteinAlpha = 0.60f;

// EMISSIVE CAP LAW (lifted from the rift hub's blown-white fix). Every emissive
// strength written by this file is clamped to one of these. They are ABOVE 1.0
// on purpose — the engine has a real bloom chain and this effect is supposed to
// drive it — but they are FINITE, so the throat keeps its internal contrast
// instead of tonemapping to a flat white disc.
constexpr float kLayerEmissiveCap = 6.0f;   // a throat annulus
constexpr float kCoreEmissiveCap  = 14.0f;  // the convergence core / spark
constexpr float kRimEmissiveCap   = 9.0f;   // the event-horizon rim

// Bound on the lights ONE wormhole may contribute, and on the field as a whole.
// The device caps at 64 and `--world space` already spends 5; a runaway light
// count is a real failure mode for an effect that emits light, so it is bounded
// here by construction and asserted by the suite.
constexpr int kLightsPerWormhole = 1;
constexpr int kMaxWormholeLights = 4;

// ---------------------------------------------------------------------------
// Per-wormhole authored data.
// ---------------------------------------------------------------------------
struct WormholeTuning {
    float radius      = 26.0f;   // aperture radius at full open (world units)
    // How far the layer stack recedes along the axis. WAS 42 against a 30 m mouth
    // radius, which makes the funnel LONGER THAN ITS MOUTH IS WIDE — so at three
    // quarters the deep end projects 42*sin(45) = 29.7 m sideways against a mouth
    // that has foreshortened to 30*cos(45) = 21 m, and the last third of the
    // stack trails out BESIDE the aperture as a row of discrete rings. That is
    // the tail in the 45-degree captures, and it is the same failure the taper
    // comment warned about: a hole you can see the tunnel sticking out of is not
    // a hole. At 26 the tip projects to 18.4 m and stays inside the silhouette
    // for every angle down to the grazing fade. Head-on this costs nothing — axial
    // depth does not project — it only shortens the parallax as the camera moves.
    float throatDepth = 26.0f;
    float spinRate    = 0.28f;   // rad/s of the outermost layer (deeper = faster)

    // Phase durations, seconds. heldSec < 0 means "hold open forever".
    float sparkSec  = 0.45f;
    float bloomSec  = 0.55f;
    float unfurlSec = 1.25f;
    float heldSec   = -1.0f;
    float closeSec  = 0.95f;

    // The Trek palette: blue-white core, deep blue walls, violet spectral fringe.
    float coreColor[3]   = { 0.72f, 0.92f, 1.35f };
    float wallColor[3]   = { 0.16f, 0.42f, 1.30f };
    float fringeColor[3] = { 0.62f, 0.30f, 1.25f };
};

// ---------------------------------------------------------------------------
// ONE wormhole in space. Pure logic + state: owns no GPU resources (the field
// owns the shared meshes/textures), so this is unit-testable headless.
// ---------------------------------------------------------------------------
class Wormhole {
public:
    // Place it. `axis` is the direction the throat recedes (the mouth faces
    // -axis, i.e. back toward an approaching ship). Normalised internally; a
    // degenerate axis falls back to +Z.
    void configure(const char* name, const float pos[3], const float axis[3],
                   bool stable, int id, const WormholeTuning& t = {});

    // Arm the staged opening from Dormant. A no-op if already opening/open.
    void open();
    // Begin the collapse from any live phase. A no-op if Dormant/Closing.
    void close();
    // Snap straight to a settled throat (tests + the "already open" placement).
    void forceHeld();

    // Advance the phase machine. DRAINS dt across phase boundaries so a hitch
    // cannot desync it from a high-framerate run.
    void update(float dt);

    // ---- State ------------------------------------------------------------
    WormholePhase phase() const { return m_phase; }
    float phaseTime() const     { return m_phaseT; }
    float time() const          { return m_time; }
    bool  live() const          { return m_phase != WormholePhase::Dormant; }
    bool  stable() const        { return m_stable; }
    void  setStable(bool s)     { m_stable = s; }
    int   id() const            { return m_id; }
    const char* name() const    { return m_name; }
    const float* pos() const    { return m_pos; }
    const float* axis() const   { return m_axis; }
    const WormholeTuning& tuning() const { return m_tuning; }

    // ---- Derived visuals (the render path and the tests read these) --------
    // 0..1 aperture openness. 0 in Dormant/Spark, ramps over Unfurl, 1 at Held,
    // falls over Closing. INCLUDES the instability waver, so an unstable hole
    // literally breathes a different radius than a stable one.
    float aperture() const;
    // HDR drive for the convergence core. Peaks during Bloom (the flare) and
    // settles to a live value at Held. Clamped to kCoreEmissiveCap.
    float coreIntensity() const;
    // Emissive drive for one throat annulus, layer 0 = outermost/nearest mouth.
    float layerIntensity(int layer) const;
    // Spectral tint for one annulus: blue-white at the core, deep blue mid,
    // violet fringe at the mouth. Unstable holes push the ramp off blue.
    void  layerTint(int layer, float outRgb[3]) const;
    // 0..1 "how wrong is it right now" — 0 for a stable hole, a live wavering
    // signal for an unstable one. The single knob every instability read uses.
    float instability() const;

    // The light this wormhole spills into the world THIS frame. Returns the
    // number written (0 or kLightsPerWormhole — a Dormant hole emits nothing).
    // Bounded by construction; there is no path that returns more than
    // kLightsPerWormhole.
    int   collectLights(rhi::PointLight* out, int maxOut) const;

    // Is `p` inside the throat mouth (the transit trigger volume)? Only true
    // while the aperture is meaningfully open — you cannot fall into a hole
    // that has not opened yet.
    bool  contains(const float p[3]) const;
    // Distance from `p` to the mouth centre.
    float distanceTo(const float p[3]) const;

    // HOW MUCH OF THE THROAT AN EYE AT `p` CAN SEE, 1 (dead head-on) .. 0
    // (exactly edge-on). This is the fix for the side-on "discrete hoops" read.
    //
    // The throat is a STACK OF PARALLEL DISCS, and no amount of blending turns
    // that into a tunnel: viewed from the side you look ACROSS the stack, every
    // ring presents its own silhouette, and the effect reads as a slinky. But an
    // aperture is a thing you look INTO — its interior is only there to be seen
    // when you can see DOWN it — so the right answer is that the throat fades
    // out as the view goes grazing, and the wormhole collapses to its rim and its
    // light spill. That is also what an edge-on hole in space should look like.
    //
    // It is measured ONCE for the whole hole, from the axis, on the CPU. A
    // per-fragment dot(N,V) in the shader cannot do this job: the throat is
    // ~42 m long, so from a side-on camera 110 m out the MOUTH is edge-on while
    // the convergence is still 20 degrees open, and a per-fragment fade dissolves
    // the near rings while leaving the far ones — a stranger artifact than the
    // one it set out to fix. The aperture has to fade as ONE object.
    float facingFade(const float p[3]) const;

    // ---- THE EVENT HORIZON (see the block comment above kHorizonDiscFrac) ---

    // World radius of the impostor disc, and of the lensing halo's outer edge.
    // Neither takes an eye: a sphere's silhouette radius is not a function of
    // where you stand, and these are the numbers that say so.
    float horizonRadius() const;
    float haloOuterRadius() const;

    // Interior coverage of the horizon disc for an eye at `p`. Ramps from
    // kHorizonAlphaHeadOn to kHorizonAlphaGrazing as the view goes grazing —
    // exactly complementary to facingFade, so the throat handing over its read
    // and the horizon taking it up are the same number.
    float horizonAlpha(const float p[3]) const;

    // Coverage and HDR drive of the Einstein ring / lensing halo. NEITHER TAKES
    // AN EYE. That is the contract, not an oversight: the ring is the element
    // that must read identically at 0 and at 90 degrees.
    float einsteinCoverage() const;
    float einsteinDrive() const;

    // The BILLBOARD BASIS the impostor is drawn with: outX/outY span the impostor
    // plane, outZ points from the wormhole toward `p`. The draw path and the
    // tests build it through this one function so a test cannot pass against
    // geometry the renderer does not actually use.
    void  horizonBasis(const float p[3], float outX[3], float outY[3], float outZ[3]) const;

    // THE NON-DEGENERACY MEASUREMENTS. Both project a circle into the plane
    // perpendicular to the line of sight and return minor/major extent:
    //   * horizonProjectedAspect — the BILLBOARD impostor. 1.0 at EVERY angle.
    //   * mouthProjectedAspect   — the AXIS-ALIGNED mouth ring, i.e. what the
    //     wormhole was before this. |cos(angle off axis)|, which is 0 edge-on.
    // The pair is the measurement, not the claim: the second one is the defect,
    // quantified, and the first is its absence in the same units.
    float horizonProjectedAspect(const float p[3]) const;
    float mouthProjectedAspect(const float p[3]) const;

    // ONE number for "there is a visible hole in space here, seen from `p`":
    // silhouette radius (world units) x peak composited coverage on it. Asserted
    // non-degenerate at 0/45/70/90/180 by --test-wormholes. It is deliberately a
    // product of a SIZE and a COVERAGE — a signature that stayed non-zero only
    // because something drew at one pixel would not be worth asserting.
    float horizonSignature(const float p[3]) const;

private:
    char  m_name[32] = "WORMHOLE";
    float m_pos[3]   = { 0, 0, 0 };
    float m_axis[3]  = { 0, 0, 1 };
    bool  m_stable   = true;
    int   m_id       = -1;
    WormholeTuning m_tuning{};

    WormholePhase m_phase = WormholePhase::Dormant;
    float m_phaseT = 0.0f;   // time inside the current phase
    float m_time   = 0.0f;   // total accumulated live time (drives all motion)

    float phaseDuration(WormholePhase p) const;
    void  enter(WormholePhase p);
};

// ---------------------------------------------------------------------------
// THE FIELD — every wormhole in the world plus the shared GPU resources.
//
// Meshes/textures are created ONCE and instanced across all wormholes and all
// kThroatLayers annuli, so N wormholes cost N*(kThroatLayers+2) draws and
// exactly two GPU allocations. That matters: `allocationCount=0` at teardown is
// a gate condition.
// ---------------------------------------------------------------------------
class WormholeField {
public:
    void init(rhi::IRenderDevice& dev);
    void shutdown(rhi::IRenderDevice& dev);
    bool initialized() const { return m_initialized; }

    // Add a wormhole; returns its index. Capacity is kMaxWormholes.
    int  add(const Wormhole& w);
    int  count() const { return (int)m_holes.size(); }
    Wormhole&       at(int i)       { return m_holes[(size_t)i]; }
    const Wormhole& at(int i) const { return m_holes[(size_t)i]; }

    // Tick every wormhole.
    void update(float dt);

    // Draw every live wormhole: the layered throat, the rim, the core.
    // `eye` is the camera/ship position — layers are drawn far-to-near so the
    // additive stack composites in depth order.
    void render(rhi::IRenderDevice& dev, const rhi::FrameContext& fr,
                const float eye[3]) const;

    // Gather the field's light contribution. NEVER writes more than
    // min(maxOut, kMaxWormholeLights) — the runaway-light-count bound.
    int  collectLights(rhi::PointLight* out, int maxOut) const;

    // Fill CommsPortal rows for the wormhole-proximity advisory. Only LIVE
    // wormholes are published (a dormant hole has nothing to advise about).
    // Returns rows written.
    int  buildCommsRows(x3::game::CommsPortal* rows, int maxRows) const;

    // buildCommsRows + commsBus().publishPortals in one call — the single line
    // a host adds to light up the AEGIS wormhole advisory.
    void publishToComms(const float eye[3]) const;

    // Index of the wormhole whose throat contains `p`, or -1. This is the
    // transit trigger.
    int  entered(const float p[3]) const;

    // Total live (non-Dormant) wormholes.
    int  liveCount() const;

private:
    static constexpr int kMaxWormholes = 8;

    std::vector<Wormhole> m_holes;
    bool m_initialized = false;

    // Shared geometry. `m_ringMesh` is a unit annulus (inner radius
    // kRingInner, outer 1.0) used for every throat layer AND the rim;
    // `m_discMesh` is a unit disc used for the convergence core / spark.
    rhi::MeshHandle    m_ringMesh{};   // wide annulus — the throat layers
    rhi::MeshHandle    m_rimMesh{};    // THIN annulus — the aperture's mouth edge
    rhi::MeshHandle    m_discMesh{};
    // THE HORIZON IMPOSTOR. `m_horizonMesh` is a RADIAL DISC (an annulus with
    // inner radius 0), so its v runs 0 at the centre to 1 at the rim — the
    // membrane shader's required parameterisation, which the inscribed-UV
    // m_discMesh does NOT satisfy (its v is a Cartesian axis, not a radius).
    // `m_haloMesh` is the lensing annulus that carries the Einstein ring.
    rhi::MeshHandle    m_horizonMesh{};
    rhi::MeshHandle    m_haloMesh{};
    rhi::TextureHandle m_throatTex{};   // filamentary swirl (u=angle, v=radius)
    rhi::TextureHandle m_coreTex{};     // radial white-hot falloff
    // The halo/Einstein profile across v. ANGULARLY UNIFORM ON PURPOSE — see
    // the note on the billboard roll in horizonBasis().
    rhi::TextureHandle m_haloTex{};

    void drawOne(rhi::IRenderDevice& dev, const rhi::FrameContext& fr,
                 const Wormhole& w, const float eye[3]) const;
};

// ---------------------------------------------------------------------------
// The authored wormhole roster for `--world space`. Kept here rather than in the
// host so the suite can assert the roster without booting a GPU: one STABLE hole
// and one UNSTABLE one, so the stability advisory has both cases to say out loud
// and the player can see the difference from the cockpit.
// ---------------------------------------------------------------------------
void seedSpaceWormholes(WormholeField& field);

// --test-wormholes: headless self-test of the phase machine (incl. the 60 Hz vs
// 165 Hz equivalence proof), the stable/unstable visual divergence, the light
// bound, the comms rows and the transit trigger. No GPU.
bool runWormholeFieldSelfTest();

} // namespace x3::space
