#pragma once
// PERFORMANCE SHOP — drive in, build your car. "LATE NIGHT SPEED".
//
// A drive-in garage in the --world drive terrain: floor/walls/bay-door/lift/
// toolboxes authored as a LevelDoc (assets/levels/perfshop.leveldoc.json,
// loaded through the REAL data-driven LevelDocWorld loader via buildFromDoc —
// the doc is authored in a LOCAL frame and translated onto the terrain at a
// runtime-picked flat site near spawn), plus code-built EMISSIVE extras the
// LevelDoc format has no words for yet: the 'LATE NIGHT SPEED' neon sign and
// the big garage TERMINAL GLASS (the shop UI / dyno screen, baked with the
// holo-terminal text-on-glass technique: stb_truetype + an additive canvas
// re-uploaded on UI change).
//
// FLOW: drive through the bay door -> car detected stopped on the LIFT PAD ->
// the camera swings to a slow ORBIT around the car -> shop mode. The terminal
// shows PARTS (category list -> parts with stats/prices/installed -> buy /
// sell-back) and the DYNO (boost/fuel/timing sliders; SPACE runs a PULL that
// sweeps the RPM and draws the live torque+power curves on the glass; tunes
// past the ECU's safe thresholds LIMIT-POP the engine: bang, power penalty
// until repaired). Every purchase re-composes the VehicleBuild and lowers it
// onto the LIVE Jolt car (DriveDemo::applyTuning) — throttle out of the bay
// and FEEL it.
//
// Game/slice code only — engine/ stays pure.

#include "scene.h"
#include "vehparts.h"
#include "vehicle.h"
#include "leveldoc_world.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/audio/IAudioSystem.h"

#include <string>
#include <vector>

namespace x3::game {

class PerfShop {
public:
    // Build the shop into the live drive world. Picks the FLATTEST site from a
    // small candidate ring around (aheadX, aheadZ) (terrain-sampled), translates
    // the LevelDoc onto it, builds it through LevelDocWorld, then adds the neon
    // sign + the terminal glass. `catalog`/`build` are borrowed (host-owned) and
    // must outlive the shop. Returns false on doc/loader failure.
    bool build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics,
               vehparts::Catalog* catalog, vehparts::VehicleBuild* buildState,
               float aheadX, float aheadZ);

    void shutdown(Scene& scene, x3::rhi::IRenderDevice& device,
                  x3::phys::IPhysicsWorld& physics);

    // ---- Site queries -------------------------------------------------------
    // World-space lift-pad center / shop origin (floor-top Y at [1]).
    void liftCenter(float out[3]) const { out[0]=m_lift[0]; out[1]=m_lift[1]; out[2]=m_lift[2]; }
    void shopOrigin(float out[3]) const { out[0]=m_site[0]; out[1]=m_site[1]; out[2]=m_site[2]; }
    // Is a car position on the lift pad (XZ box around the pad)?
    bool onLiftPad(const float carPos[3]) const;

    // ---- Shop mode (host drives the transitions) ----------------------------
    bool shopMode() const { return m_shopMode; }
    void setShopMode(bool on);
    // Orbit camera while in shop mode: returns pos+yaw+pitch looking at the lift.
    void orbitCam(float out5[5]) const;

    // ---- UI input (rising-edge calls from the host) -------------------------
    void uiUp();
    void uiDown();
    void uiSelect();    // enter category / buy / sell-back
    void uiBack();      // part list -> category list
    void uiTab();       // PARTS <-> DYNO
    // Dyno sliders: slider 0=boost 1=fuel 2=timing; dir = -1/+1 steps.
    void adjustTune(int slider, int dir);
    void startPull();   // run a dyno pull (no-op while one is running)
    void repairEngine();    // pay the ECU's repair cost, clear the damage
    void refillNitrous();   // pay the kit's refill cost, fill the tank

    // ---- Per-frame ----------------------------------------------------------
    // Advances the orbit + dyno sweep + neon flicker, re-bakes the terminal
    // texture when dirty, fires the POP (bang one-shot via `audio`/`bangSfx`),
    // and re-applies tuning to `car` whenever the build changed. Any of
    // audio/car may be null (headless / on foot).
    void update(float dt, DriveDemo* car,
                x3::audio::IAudioSystem* audio, x3::audio::SoundHandle bangSfx);

    // The current composed build (tuning + audio profile + dyno data).
    const vehparts::ComposedBuild& composed() const { return m_composed; }
    // Recompose from the live VehicleBuild + apply to the car (host calls once
    // at world start; the UI paths call it internally on every change).
    void recompose(DriveDemo* car);

    // Build/tune changed since the last consumeNeedSave() (host persists).
    bool consumeNeedSave() { const bool b = m_needSave; m_needSave = false; return b; }

    // Doc point lights (host feeds device.setPointLights each frame).
    uint32_t selectLights(float ex, float ey, float ez,
                          std::vector<x3::rhi::PointLight>& out, uint32_t maxLights) const {
        return m_world.built() ? m_world.selectLights(ex, ey, ez, out, maxLights) : 0u;
    }

    // Dyno pull state (the headless proof poses a mid-pull frame).
    bool  pullRunning() const { return m_pullT >= 0.0f; }
    float pullProgress() const { return m_pullT; }

private:
    enum class Mode { Parts, Dyno };
    enum class Focus { Categories, Parts };

    void markUiDirty() { m_texDirty = true; }
    void bakeTerminal(x3::rhi::IRenderDevice& device);   // re-bake + re-upload the glass
    void applyAndSave(DriveDemo* car);                   // recompose + applyTuning + needSave

    // Borrowed state (host-owned).
    vehparts::Catalog*      m_catalog = nullptr;
    vehparts::VehicleBuild* m_build   = nullptr;
    vehparts::ComposedBuild m_composed;

    // Site + doc world.
    LevelDocWorld m_world;
    float m_site[3] = { 0, 0, 0 };       // shop origin (floor top at m_site[1])
    float m_lift[3] = { 0, 0, 0 };       // lift pad center (world)
    Scene*                  m_scene  = nullptr;
    x3::rhi::IRenderDevice* m_device = nullptr;

    // Code-built extras (sign + terminal glass + lift posts): scene slots + GPU
    // resources we own (freed in shutdown).
    std::vector<uint32_t>               m_extraSlots;
    std::vector<x3::rhi::MeshHandle>    m_extraMeshes;
    std::vector<x3::rhi::TextureHandle> m_extraTex;
    uint32_t                m_screenSlot = kNoLink;   // the terminal glass entity
    x3::rhi::TextureHandle  m_screenTex{};            // live UI texture (re-baked)
    uint32_t                m_texN = 768;

    // Shop mode + orbit.
    bool  m_shopMode = false;
    float m_orbitA   = 0.0f;             // orbit angle (rad, advances with dt)

    // UI state.
    Mode  m_mode  = Mode::Parts;
    Focus m_focus = Focus::Categories;
    int   m_catCursor  = 0;
    int   m_partCursor = 0;
    std::string m_status;                // one-line feedback ("INSTALLED", "NO CREDITS")
    bool  m_texDirty = true;
    float m_bakeCooldown = 0.0f;         // pull-time re-bake throttle (~12 Hz)

    // Dyno pull.
    float m_pullT = -1.0f;               // <0 idle; else 0..1 sweep progress
    bool  m_pullPopped = false;          // this pull already popped
    float m_lastPeakTq = 0.0f, m_lastPeakKw = 0.0f;   // last completed pull
    bool  m_havePull = false;

    bool  m_needSave = false;
    bool  m_carDirty = false;            // tuning changed; apply on next update()

    // ---- Screen-content probe (the rifthub holoReadoutInkFraction technique) -----
    // Measured by bakeTerminal() over the REAL pixels it is about to upload, so the
    // test inspects what the player actually sees rather than a re-derived guess:
    //   m_screenInk  = fraction of the panel body that is LIT INK (the UI strokes).
    //                  A blank / failed bake probes ~0.
    //   m_screenDark = fraction that is genuinely DARK (the OLED substrate).
    //                  A WASHED, flooded screen probes ~0 — that is the exact failure
    //                  this pass fixed, so the test must be able to see it.
    // A display needs BOTH: something lit, on something black.
    float m_screenInk  = 0.0f;
    float m_screenDark = 0.0f;

public:
    // The terminal glass entity (kNoLink until build()). Exposed for the self-test:
    // asserting a screen EXISTS and asserting it SHOWS SOMETHING are different checks.
    uint32_t screenSlot() const { return m_screenSlot; }
    float screenInkFraction() const  { return m_screenInk; }
    float screenDarkFraction() const { return m_screenDark; }
};

// Headless self-test for `--test-perfshop`: build the shop with a stub render/physics
// device, then assert THE SCREENS ARE DISPLAYS — the terminal glass is textured, rides
// the per-texel emissive path, and its baked UI has real ink on a genuinely dark
// substrate; the neon sign's glow is gated by its texture (and therefore carries an MR
// map, without which Scene::submit() SILENTLY DROPS the emissive map). Carries a
// negative control proving the ink probe can fail. Logs "perfshop: X/Y passed".
bool runPerfShopSelfTest();

} // namespace x3::game
