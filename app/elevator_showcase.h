#pragma once
// THE CENTERPIECE — a SELF-CONTAINED elevator showcase module ("the core of the
// building"). Builds a THICK, NICE, premium multi-floor elevator with a DARK
// SMOKED-GLASS cab interior, glowing holo controls + accent light strips, per-floor
// sliding doors + realistic call-panel keypads, a glass-bottom strata descent, an
// entertainment screen, warm interior lighting, and a descent to Club 1127 at the
// bottom (Tim's art direction: "THICK elevator. NICE elevator. Dark glass interior").
//
// DECOUPLED BY DESIGN: this module does NOT touch level1.cpp / the Spire / any
// specific building geometry (that layout is being rebuilt in parallel). It accepts
// its placement as DATA via PlacementSpec (a shaft XZ + an ordered floor list it
// turns into stops). The real level wires it in later through build() — exactly the
// clean spawn-point + floor-list interface the task asks for.
//
// It drives the existing x3::game::ElevatorSystem (the proven FSM + carry + keypad +
// strata + disco-to-1127), wrapping it with the high-poly, artifact-free SHELL +
// CAB geometry (x3::elevmesh) and the holo control panel (HoloTerminal). The host
// (`--world elevator` in main.cpp) ticks update() and carries the rider.

#include "scene.h"
#include "elevator.h"
#include "holo_terminal.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <string>
#include <vector>

namespace x3::audio { class IAudioSystem; }

namespace x3::game {

// One served floor: its world-Y (cab-CENTER height) + a directory label. The real
// level passes a list of these; the showcase synthesizes a default tower otherwise.
struct ShowcaseFloor {
    float       centerY;     // cab-center world Y at this stop
    std::string label;       // directory text (e.g. "F3 — DETENTION", "CLUB 1127")
    bool        isClub = false;   // the bottom stop = Club 1127 (descent destination)
};

// PLACEMENT INTERFACE (the clean data contract the real level uses). All optional —
// build() synthesizes a sensible default showcase tower when floors is empty.
struct PlacementSpec {
    float shaftX = 0.0f, shaftZ = 0.0f;     // shaft center XZ in world space
    std::vector<ShowcaseFloor> floors;       // ordered low->high; empty => default
    int   startStop = 0;                     // which stop the cab idles at on boot
    bool  buildShaftShell = true;            // build the shaft tube + per-floor lobbies
};

class ElevatorShowcase {
public:
    // Build the entire showcase (shaft shell + per-floor doors/call-panels + the
    // thick dark-glass cab interior + holo control panel + lights) into scene/physics
    // through device. Idempotent. `audio` may be null (procedural hooks no-op).
    bool build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics,
               const PlacementSpec& spec, x3::audio::IAudioSystem* audio);

    // Advance one frame: ticks the elevator FSM (returns cab vertical delta for the
    // host to carry the rider), animates the per-floor + cab sliding doors to match
    // the FSM door %, drives the accent strips / vent / entertainment screen / holo
    // shimmer, and re-poses the interior point lights. The host pushes pointLights()
    // and calls drawHolo()/the holo overlay each frame.
    float update(float dt, Scene& scene, x3::rhi::IRenderDevice& device,
                 x3::phys::IPhysicsWorld& physics);

    // ---- Rider carry (host loop) ----
    bool playerRiding(const x3::phys::Vec3& feet) const { return m_elev.playerRiding(feet); }
    x3::phys::Vec3 cabCenter() const { return m_elev.cabCenter(); }
    float cabTopY() const { return m_elev.cabTopY(); }

    // ---- Calls / floor select (host routes input here) ----
    // Send the cab to a stop index (0 = lowest). Used by the holo floor buttons.
    void callTo(int stopIndex) { m_elev.callTo(stopIndex); }
    void callNext() { m_elev.callNext(); }
    // Push a keypad digit (1-1-2-7 = DISCO + descend to Club 1127).
    bool keypadDigit(int d) { return m_elev.keypadDigit(d); }
    // Rider craft: proximity auto-open (idle cab + sealed doors + feet nearby).
    void autoOpenFor(const x3::phys::Vec3& f) { m_elev.autoOpenFor(f); }
    // Drive straight to the Club 1127 stop (the "CLUB 1127" holo button).
    void callClub();

    // ---- State / display read-back (host HUD + holo readout) ----
    int   stopCount() const { return m_elev.stopCount(); }
    int   targetStop() const { return m_elev.targetStop(); }
    ElevState state() const { return m_elev.state(); }
    const char* stateName() const { return m_elev.stateName(); }
    const char* currentStratum() const { return m_elev.currentStratum(); }
    bool  disco() const { return m_elev.disco(); }
    bool  moving() const { return m_elev.moving(); }
    float odometer() const { return m_elev.odometer(); }
    // Nearest stop index to the cab right now (the "current floor" for the indicator).
    int   currentFloorIndex() const;
    const std::vector<ShowcaseFloor>& floors() const { return m_floors; }

    // Descent progress 0..1 (surface Y=0 -> Club 1127 at Y=-200), clamped. Drives the
    // depth readout, the geology-glow swell, and the club-music crossfade. 0 above ground.
    float descentProgress() const;
    // STRATA STREAMING HOOK: the real streamed planet-strata (built separately) declares
    // itself LIVE here; the placeholder liner then yields (its own glow eases off) so the
    // streamed layers own the view. No-op today (self-contained placeholder).
    void  setStrataStreamed(bool on) { m_strataStreamed = on; }
    bool  strataStreamed() const { return m_strataStreamed; }

    // The interior + accent + disco point lights the host pushes via setPointLights
    // every frame (the disco cue + door-glow animate them in update()).
    const std::vector<x3::rhi::PointLight>& pointLights() const { return m_lights; }

    // The holo control panel (host renders its on-glass text + routes E-to-activate).
    HoloTerminal& holo() { return m_holo; }
    x3::phys::Vec3 holoAnchor() const { return m_holo.anchor(); }

    // Player spawn (feet) just inside the cab at boot, facing the holo panel.
    x3::phys::Vec3 spawn() const { return m_spawn; }
    // A good headless showcase camera (x,y,z,yaw,pitch) framing the cab interior +
    // holo panel + the dark glass + strata below. variant: 0=interior, 1=exterior
    // shaft, 2=strata-descent (mid-shaft looking down through the glass floor).
    void showcaseCamera(int variant, float out[5]) const;

    // Fixture census for the --test-elevator-showcase self-test.
    struct Stats {
        int   entities      = 0;
        int   floors        = 0;
        int   shaftDoors    = 0;   // per-floor sliding door leaves (2 per floor)
        int   callPanels    = 0;   // per-floor exterior keypads
        int   callButtons   = 0;   // round buttons across all call panels
        int   holoButtons   = 0;   // interior floor-select round buttons
        bool  hasDarkGlass  = false;
        bool  hasHandrail   = false;
        bool  hasGlassFloor = false;
        bool  hasHoloPanel  = false;
        bool  hasEntScreen  = false;
        bool  hasVent       = false;
        bool  hasClubStop   = false;
        float clubStopY     = 0.0f;
    };
    const Stats& stats() const { return m_stats; }
    bool built() const { return m_built; }

private:
    // Add a solid static box (render + collision + Scene entity). Returns entity id.
    uint32_t addSolid(Scene& scene, x3::rhi::IRenderDevice& device,
                      x3::phys::IPhysicsWorld& physics,
                      const struct ElevPrim& prim, const float color[4],
                      const float emissive[4], bool collide, uint32_t tag);
    // Add a purely-visual mesh (no collision) — for thin trims, buttons, decals.
    uint32_t addDecor(Scene& scene, x3::rhi::IRenderDevice& device,
                      const struct ElevPrim& prim, const float color[4],
                      const float emissive[4], uint32_t tag);
    // Add a DARK SMOKED-GLASS panel (transparent pass, dark tint, see-through).
    uint32_t addDarkGlass(Scene& scene, x3::rhi::IRenderDevice& device,
                          const struct ElevPrim& prim, float opacity,
                          const float tint[3], const float emissive[4]);

    void buildShaft(Scene& scene, x3::rhi::IRenderDevice& device,
                    x3::phys::IPhysicsWorld& physics);
    // THE DESCENT (goal #3): line the shaft interior faces with depth-tinted, glowing
    // GEOLOGY bands (limestone -> granite -> basalt -> obsidian -> the club's crystal
    // glow), one segment per ~depth band, hugging just inside the walls (outside the
    // cab, so nothing clips through the interior). World-FIXED, so as the cab descends
    // the bands slide past the dark-glass — the "rock layers rushing past" read. update()
    // scrolls a bright seam down them + swells their glow while travelling.
    void buildStrataLiner(Scene& scene, x3::rhi::IRenderDevice& device);
    void buildCabInterior(Scene& scene, x3::rhi::IRenderDevice& device);
    void buildHoloPanel(Scene& scene, x3::rhi::IRenderDevice& device);
    void layoutCab(Scene& scene);            // reposition cab-child entities each frame
    void animateDoors(Scene& scene);         // slide leaves to the FSM door %

    // ===================================================================================
    // THE HERO SET PIECE — "an elevator you DON'T WANT TO GET OFF" (Tim's north star).
    // Four layers, ALL driven off ONE beat clock (m_time at the elevator club-track
    // tempo — no second clock) so the whole cab pulses as one instrument. The show runs
    // CONTINUOUSLY whenever a rider is aboard (idle, stopped, or descending) so standing
    // in the cab is a private micro-concert that keeps evolving and never gets boring.
    //   1. THE VEGAS SPHERE  — a wraparound faceted dome + upper-wall band of emissive
    //      facets playing beat-synced visualizer "scenes" that cycle so it never repeats.
    //   2. MUSIC-VIDEO GLASS — translucent holo panels flipping through a baked strip of
    //      procedural "music video" frames (dancer silhouettes, equalizer, lyric cards,
    //      cityscape) cut on the beat.
    //   3. CONCERT PA        — a real line-array + subwoofers with cones that PUMP on the
    //      bass and driver lenses that STROBE on the beat (a private PA firing at you).
    //   4. 5-STAR LUXURY     — polished marble floor, brushed-gold trim, a plush bench, a
    //      warm chandelier + warm key light layered UNDER the show (the Ritz, not a lift).
    // ===================================================================================
    void buildLuxury(Scene& scene, x3::rhi::IRenderDevice& device,
                     x3::phys::IPhysicsWorld& physics);
    void buildSphere(Scene& scene, x3::rhi::IRenderDevice& device);
    void buildMusicVideoGlass(Scene& scene, x3::rhi::IRenderDevice& device);
    void buildConcertPA(Scene& scene, x3::rhi::IRenderDevice& device);
    void animateShow(float dt, Scene& scene);   // per-frame beat-driven show (all 4 layers)

    // ONE beat grid (reused everywhere; derived from m_time at kBeatBpm). beatCount() is
    // the absolute beat position; beatThump() the sharp per-beat kick envelope (0..1) the
    // club's subs/tiles/dancers already ride, hoisted here so the Sphere, MV glass, PA
    // and concert-wash lights all share it. discoBoost() ramps the whole show up on 1127.
    float beatCount() const;
    float beatThump() const;
    float discoBoost() const;

    bool m_built = false;
    ElevatorSystem m_elev;
    HoloTerminal   m_holo;
    x3::audio::IAudioSystem* m_audio = nullptr;
    PlacementSpec  m_spec;
    std::vector<ShowcaseFloor> m_floors;
    float m_shaftX = 0.0f, m_shaftZ = 0.0f;
    float m_cabHX = 1.55f, m_cabHY = 0.18f, m_cabHZ = 1.55f;   // cab platform half-extents (THICK)
    int   m_clubStop = -1;
    x3::phys::Vec3 m_spawn{};
    Stats m_stats{};

    std::vector<x3::rhi::PointLight> m_lights;

    // --- Animated cab-child entities (offset around the cab center each frame) ---
    static constexpr int kWallGlass = 4;            // 4 dark-glass wall panels
    uint32_t m_eWall[kWallGlass]    = {kNoLink,kNoLink,kNoLink,kNoLink};
    uint32_t m_eCeil = kNoLink, m_eGlassFloor = kNoLink, m_eRailEnts[4] = {kNoLink,kNoLink,kNoLink,kNoLink};
    uint32_t m_eAccent[4] = {kNoLink,kNoLink,kNoLink,kNoLink};   // glowing accent strips
    uint32_t m_eEntScreen = kNoLink, m_eVent = kNoLink, m_eDiscoBall = kNoLink;
    uint32_t m_eCabDoorL = kNoLink, m_eCabDoorR = kNoLink;       // inner cab door leaves
    uint32_t m_eHoloButtons[16] = {kNoLink};                     // interior floor-select buttons
    int      m_holoButtonCount = 0;
    uint32_t m_eStrata = kNoLink;                                // the strata plane seen below
    // Shaft-interior GEOLOGY liner (world-FIXED strata bands; the descent streams past
    // these). Each entry carries its band's seam Y so update() can scroll a bright edge.
    std::vector<uint32_t> m_eStrataBands;    // tinted glowing wall segments down the shaft
    std::vector<float>    m_eStrataBandY;    // world-Y center of each band segment
    std::vector<float>    m_eStrataBandEm;   // each band's base emissive strength (for the pulse)
    std::vector<float>    m_eStrataBandTint; // 3 floats/band: the band's SELF-LIT rock hue
    // STRATA STREAMING HOOK (for the separately-built streamed planet-strata): today the
    // liner is a self-contained placeholder. When the real streamed geology exists, a host
    // sets this true and feeds live layers by re-tinting m_eStrataBands (or swaps the liner
    // for the streamed meshes) — the cab, glass wall, camera + descent timing are already
    // sized for Y=0..-200, so the streamed strata drops straight into this view.
    bool m_strataStreamed = false;
    // Per-floor exterior sliding door leaves (2 per floor) for door animation.
    std::vector<uint32_t> m_shaftDoorL, m_shaftDoorR;
    std::vector<float>    m_shaftDoorY;        // each floor's door center Y (cab-center)

    float m_time = 0.0f;
    float m_entScroll = 0.0f;
    // 1x1 dielectric MR texel (glTF: B=metallic 0, G=roughness) that routes the cab's
    // brushed-steel handrail through the PBR path so it can carry a CLEARCOAT lobe (goal
    // #1: premium clearcoated metal, reusing the engine's x3Clearcoat drawMeshPBR arg).
    x3::rhi::TextureHandle m_mrSteel{};
    // Live holo DEPTH readout (goal #2): the panel's last line is an animated status row
    // — current floor / depth counting down to -200 / stratum / state. Re-baked onto the
    // glass only when the formatted text actually changes (stb_truetype bakes are dear).
    std::string m_holoStatusLine;

    // ===================================================================================
    // HERO SET-PIECE STATE (the 4 layers above). All entities here RIDE the cab: they are
    // authored at a fixed local offset from the interior origin and re-posed each frame by
    // a single ride-along loop in layoutCab (m_ride). Per-frame EMISSIVE / COLOR / PUMP is
    // driven in animateShow() off the shared beat clock.
    // ===================================================================================
    // A cab-child entity that rides the interior origin at a fixed offset, plus an optional
    // per-beat PUMP translation (the speaker cones punch outward on the bass). p* = pump
    // axis * amplitude (metres at full thump); {0,0,0} = a static ride-along.
    struct RideEnt { uint32_t id; float ox, oy, oz; float px, py, pz; };
    std::vector<RideEnt> m_ride;
    void addRide(uint32_t id, float ox, float oy, float oz,
                 float px = 0.0f, float py = 0.0f, float pz = 0.0f) {
        if (id != kNoLink) m_ride.push_back({id, ox, oy, oz, px, py, pz});
    }

    // 1. VEGAS SPHERE facets. Each facet stores its angle around the cab (0..2pi) and a
    //    vertical coord v (0 = low wall band, 1 = dome crown) so the visualizer can paint
    //    flowing patterns across the wraparound surface.
    std::vector<uint32_t> m_eSphere;
    std::vector<float>    m_eSphereAng;
    std::vector<float>    m_eSphereV;

    // 2. MUSIC-VIDEO glass. A baked strip of procedural MV frames (dancer silhouettes /
    //    equalizer / lyric card / cityscape), cut on the beat onto translucent holo panes.
    static constexpr int kMvFrames = 8;
    x3::rhi::TextureHandle m_mvFrame[kMvFrames] = {};
    x3::rhi::TextureHandle m_mvMr{};             // glossy dielectric MR -> PBR route (emissiveTex honored)
    std::vector<uint32_t>  m_eMvPanel;           // holo MV panes (emissiveTex swapped per beat)
    int                    m_mvLastFrame = -1;   // last frame index assigned (change-gate the swap)

    // 3. CONCERT PA. Sub cones that pump on the bass + driver lenses that strobe on the beat.
    std::vector<uint32_t> m_eSubCone;            // pumping woofer/sub cones (m_ride carries the pump)
    std::vector<uint32_t> m_eDriver;             // driver/tweeter lenses (emissive strobe)

    // 4. LUXURY. Warm chandelier drop beads (breathe warm under the show) + gold trim.
    std::vector<uint32_t> m_eChandelier;

    // Shared 1x1 PBR texels: polished marble (low-rough dielectric), brushed gold (metal),
    // glossy screen panel (dielectric). Created once; drive the premium material routes.
    x3::rhi::TextureHandle m_mrMarble{}, m_mrGold{}, m_mrPanel{};
    // The current bass-pump amount (0..~1), written by animateShow() and read by the
    // ride-along loop in layoutCab() so the speaker cones punch outward on the beat.
    float m_showPump = 0.0f;
};

// Headless self-test (--test-elevator-showcase): build the showcase with a default
// tower (incl. Club 1127 at the bottom) under a stub device/physics, assert the
// shell + thick dark-glass cab + holo panel + per-floor doors/call-panels exist,
// drive the FSM to every stop (incl. the club via the 1127 keypad) with no leaks,
// and confirm doors animate + the rider is carried. Prints "elevshowcase: X/Y".
bool runElevatorShowcaseSelfTest();

} // namespace x3::game
