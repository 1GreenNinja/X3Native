// --world space host — the Act-3 6DOF space-pilot showcase. RE-HOMED from the
// pre-split main() `if (worldMode == "space") { ... }` inline block (feat/
// cockpit-vattalus) into the #28 deep-split world-host registry. The body is the
// VERBATIM space host loop; the ONLY edits are reaching shared state via the
// HostContext (`hc.device` is a raw IRenderDevice*, so the pre-split
// `device.get()`/`device->` become `device`/`device->`), mirroring host_drive.cpp.
#include "world_host_common.h"
#include "engine/asset/IAssetSource.h"
#include "engine/asset/IModelLoader.h"
#include "../scene.h"
#include "../mesh_prims.h"
#include "../fx.h"
#include "../asset_root.h"
#include "../space_pilot.h"
#include "../settings_io.h"   // readFlightMode (persisted Settings-menu / console pick)
#include "../audio_root.h"    // resolveAudio(...) — flight engine hum / boost / mode blip WAVs
#include "engine/audio/IAudioSystem.h"
#include <filesystem>

namespace x3 { namespace apphost {

// ---------------------------------------------------------------------------
// Local render-only helpers for the space HUD + sense-of-speed FX layer. These
// are PURE PRESENTATION (never touch the pilot sim state that --test-space
// hashes), so the determinism gate is unaffected. Kept file-local (anon ns).
// ---------------------------------------------------------------------------
namespace {
// Column-major 4x4 from three (already-scaled-direction) basis columns + origin.
inline void composeBasis(float m[16],
                         const x3::phys::Vec3& cx, const x3::phys::Vec3& cy,
                         const x3::phys::Vec3& cz, float sx, float sy, float sz,
                         const x3::phys::Vec3& t) {
    m[0]=cx.x*sx; m[1]=cx.y*sx; m[2]=cx.z*sx; m[3]=0;
    m[4]=cy.x*sy; m[5]=cy.y*sy; m[6]=cy.z*sy; m[7]=0;
    m[8]=cz.x*sz; m[9]=cz.y*sz; m[10]=cz.z*sz; m[11]=0;
    m[12]=t.x;    m[13]=t.y;    m[14]=t.z;     m[15]=1;
}
inline x3::phys::Vec3 vcross(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
    return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
inline x3::phys::Vec3 vnorm(const x3::phys::Vec3& a) {
    float l = std::sqrt(a.x*a.x + a.y*a.y + a.z*a.z);
    if (l < 1e-6f) return { 0, 0, 1 };
    return { a.x/l, a.y/l, a.z/l };
}
// Cheap integer hash -> [0,1) for deterministic dust-field seeding / twinkle.
inline uint32_t hashU(uint32_t x) { x^=x>>16; x*=0x7feb352du; x^=x>>15; x*=0x846ca68bu; x^=x>>16; return x; }
inline float    hashF(uint32_t x) { return (hashU(x) & 0xFFFFFFu) / 16777216.0f; }
} // namespace

int hostSpace(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const std::string& worldMode = hc.worldMode;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const bool shotCamOverride = hc.shotCamOverride;
    const float* shotCam = hc.shotCam;

    // ==== VERBATIM host body (re-homed; device is now a raw pointer) ====
    if (worldMode == "space") {
        x3::logInfo("--world space: building the Act-3 space-pilot showcase");
        std::unique_ptr<x3::phys::IPhysicsWorld> sphys(x3::phys::createPhysicsWorld());
        if (!sphys->init()) {
            x3::logError("--world space: physics init failed");
            device->shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate(); return 1;
        }

        // W3-3 (AD-2 red-line): deep-space STARFIELD. The old host disabled the
        // sky entirely, leaving the flat navy clear color behind the fleet. The
        // analytic sky's procedural starfield is gated to DARK skies and, at
        // haze == 0, paints stars on the FULL sphere (spaceW: a space scene
        // looking "down" sees stars, not a ground plane) — so deep space is the
        // sky ENABLED at near-black with zero haze, exactly like the nightsky
        // host but with no horizon band at all.
        { x3::rhi::IRenderDevice::SkyParams sp{};
          sp.enabled = true;
          sp.sunDir[0] = 0.6f; sp.sunDir[1] = 0.5f; sp.sunDir[2] = 0.62f;   // matches the key light corner
          sp.sunColor[0] = 0.75f; sp.sunColor[1] = 0.82f; sp.sunColor[2] = 1.0f;
          // W6-2: 0.02 -> 0.55 — a cool DIRECTIONAL starlight key. The sky sun feeds
          // the PBR path (the surface tower proves it), and it's the only way hulls
          // get real shading gradients out here: point rigs vanish at capital-ship
          // scale, and a flat ambient floor reads as clay. Still far below daylight.
          sp.sunIntensity = 0.55f;
          sp.haze = 0.0f;                              // haze 0 == DEEP SPACE (stars on the full sphere)
          sp.exposure = 1.0f;
          sp.zenith[0]  = 0.003f; sp.zenith[1]  = 0.003f; sp.zenith[2]  = 0.008f;
          sp.horizon[0] = 0.004f; sp.horizon[1] = 0.005f; sp.horizon[2] = 0.011f;
          device->setSkyParams(sp);
          device->setSkyTime(10.0f);                   // non-zero -> starfield twinkle/rotation phase
          // R2: enabling the sky at near-zero sun replaced whatever ambient the
          // disabled-sky path implied — the fleet went silhouette-black. Explicit
          // cool ambient so hulls read while space stays dark (nightsky's trick).
          device->setAmbient(0.11f, 0.12f, 0.16f); }
        // SSAO + SSGI screen-space passes raster the whole scene to black on a
        // black/empty space background (no nearby geometry to bounce off) -- the
        // 1080 Ti / no-RT fallback path documented in the memory bank. Disable
        // both for the space showcase so the ships actually read against the
        // dark backdrop.
        { x3::rhi::IRenderDevice::SsaoParams ap{}; ap.enabled = false;
          device->setSsaoParams(ap); }
        { x3::rhi::IRenderDevice::GiParams gp{}; gp.enabled = false;
          device->setGiParams(gp); }
        // Sun = the directional sun baked into mesh.frag at +Y-ish; layer on a
        // few BRIGHT point lights NEAR the fleet so the ships read (the analytic
        // sky is OFF -> no atmospheric tint; light only comes from these point
        // lights + the hardcoded sun, attenuated by 1/r^2). The point-light
        // ranges + intensities are intentionally cranked: deep space has zero
        // bounced light, so anything subtle would render the ships as silhouettes.
        // R3: with the REAL black sky in (starfield), the old light rig left the
        // hulls as silhouettes — the navy "readability" of the old shot was just
        // the clear color. Roughly doubled key/fill/rim so the fleet reads as lit
        // metal against the stars.
        // Persistent 5-slot rig: [0..2] STATIC fleet key/fill/rim (set once); [3] a
        // PLAYER-KEY light that FOLLOWS the ship each frame (so its silhouette reads
        // while flying — the static rig doesn't move with the player); [4] a warm
        // SUN-HEAT light that ramps up as you dive toward the star. Slots 3-4 are
        // refreshed every frame via updateDynamicLights() below, which re-uploads all
        // five with setPointLights (the device caches its own copy).
        x3::rhi::PointLight plights[5];
        // Key light: a "sun" anchored near the fleet so attenuation is gentle.
        plights[0].pos[0] =  120.0f; plights[0].pos[1] = 120.0f; plights[0].pos[2] = 120.0f;
        plights[0].range  =  600.0f;
        plights[0].color[0] = 130.0f; plights[0].color[1] = 121.0f; plights[0].color[2] = 104.0f;
        // Fill light from -X/+Y to bring out the camera-facing side.
        plights[1].pos[0] = -80.0f; plights[1].pos[1] =  60.0f; plights[1].pos[2] =  20.0f;
        plights[1].range  = 400.0f;
        plights[1].color[0] = 40.0f; plights[1].color[1] = 46.0f; plights[1].color[2] = 60.0f;
        // Rim/back light from +X/-Y to give the ships shape.
        plights[2].pos[0] =  200.0f; plights[2].pos[1] = -30.0f; plights[2].pos[2] = -50.0f;
        plights[2].range  = 500.0f;
        plights[2].color[0] = 24.0f; plights[2].color[1] = 19.0f; plights[2].color[2] = 13.0f;
        // Slots 3-4 start dark; updateDynamicLights fills them once the pilot exists.
        plights[3] = {}; plights[4] = {};
        device->setPointLights(plights, 5);

        // ---- Player ship (the SpacePilotController) -----------------------
        // FLIGHT MODE: seed from the persisted Settings-menu / console pick (the
        // cfg file is the only bridge into this standalone host — see below), fall
        // back to the shared latch, then apply the mode's feel preset. Live
        // switching is bound to the 1/2/3 keys in the windowed loop.
        {
            x3::game::FlightMode fm{};
            if (x3::game::parseFlightMode(std::to_string(x3::apphost::readFlightMode()), fm))
                x3::game::setRequestedFlightMode(fm);
        }
        x3::game::SpacePilotController pilot;
        pilot.spawn(*sphys, 0.0f, 0.0f, 0.0f);
        pilot.setMode(x3::game::requestedFlightMode());
        x3::logInfo(std::string("--world space: flight mode = ") +
                    x3::game::flightModeName(pilot.mode()) +
                    "  (press 1=Arcade 2=Assist 3=Loose to hot-swap)");

        // ---- Try to load an actual ship GLB. SpaceShip*.glb don't ship in
        //      assets/rigged_glb yet (per the task brief: "4 SpaceShip*.glb
        //      already in rigged_glb" was aspirational — the dir has none on
        //      this baseline). We fall back to DroneOscillating.glb as a
        //      stand-in flying object; if even that fails, we draw the ship
        //      as a procedural box.
        const std::string rigDir = x3::game::riggedGlbRoot();
        std::unique_ptr<x3::asset::IAssetSource> asrc(x3::asset::createAssetSource());
        asrc->mountDir(rigDir, 0);
        std::unique_ptr<x3::asset::IModelLoader> mloader(x3::asset::createModelLoader(device, asrc.get()));
        // Probe candidates in order of preference: real ship asset first, drone fallback.
        const char* kShipCandidates[] = {
            "SpaceShip.glb", "SpaceShip2.glb", "SpaceShip3.glb", "SpaceShip4.glb",
            "DroneOscillating.glb", "DroneExportWMotion.glb"
        };
        x3::asset::Model shipModel{};
        std::string shipFile;
        for (const char* c : kShipCandidates) {
            shipModel = mloader->load(c);
            if (shipModel.ok) { shipFile = c; break; }
        }
        std::vector<x3::asset::ModelDrawable> shipDrawables;
        if (shipModel.ok) shipDrawables = x3::asset::makeDrawables(shipModel);
        x3::logInfo(std::string("--world space: ship model=") + (shipModel.ok ? shipFile : "<procedural-box-fallback>"));

        // Procedural-box fallback (in case the GLB load fails entirely).
        x3::prims::PrimMesh sbm = x3::prims::makeBox(2.0f, 0.6f, 1.2f, 0, 0, 0, 0.25f);
        auto shipBoxMesh = device->createMesh(sbm.verts.data(), (uint32_t)sbm.verts.size(),
                                              sbm.index.data(), (uint32_t)sbm.index.size());
        auto sbTexD = x3::prims::makeCheckerRGBA(64, 8, 180, 190, 210, 60, 70, 90);
        auto shipBoxTex = device->createTexture(sbTexD.data(), 64, 64, true);

        // ---- SENSE-OF-SPEED near-field streak/dust layer -------------------
        // CINEMATIC WARP GRACE (owner playtest: the old stretched-box streaks read
        // "clunky" — hard-edged uniform bars, giant beams, faceted dust chips). The
        // field is now kDust NEAR-CAMERA specks in a cube around the ship that drift
        // OPPOSITE velocity and wrap when they leave the box. Each renders as a SOFT
        // ROUND billboard-ish sphere (low-poly UV sphere, not a box → no facets) that
        // is a tiny dot at rest and, at speed, becomes a TAPERED COMET: 3 collinear
        // emissive segments of decreasing strength+thickness head→tail fake the
        // bright-head/transparent-tail alpha ramp (the emissive path has no per-vertex
        // alpha, so we layer diminishing segments instead). Length follows a SMOOTHSTEP
        // of speed (graceful ramp, capped so no giant beams), per-particle hash drives
        // size (0.5-1.5x)/brightness/colour-temperature, and a boundary-shell fade
        // stops any pop-in/out at the wrap. Pure render (no sim state) → determinism
        // untouched.
        x3::prims::PrimMesh strm = x3::prims::makeUVSphere(8, 12);   // soft round speck / streak seg
        auto dustMesh = device->createMesh(strm.verts.data(), (uint32_t)strm.verts.size(),
                                           strm.index.data(), (uint32_t)strm.index.size());
        const int   kDust = 220;        // <=400 (×3 segments only when moving fast)
        const float kDustR = 65.0f;     // wrap half-box (m) centered on the ship
        std::vector<x3::phys::Vec3> dust((size_t)kDust);
        for (int i = 0; i < kDust; ++i) {
            dust[(size_t)i] = x3::phys::Vec3{
                (hashF((uint32_t)(i*3+1))*2.0f - 1.0f) * kDustR,
                (hashF((uint32_t)(i*3+2))*2.0f - 1.0f) * kDustR,
                (hashF((uint32_t)(i*3+3))*2.0f - 1.0f) * kDustR };
        }
        bool boostActive = false;   // Shift-boost this frame (drives HUD + streak punch)

        // ===================================================================
        // REAL SUN — a physical star you can fly to (and die in). ============
        // ===================================================================
        // The analytic sky paints a sun disc along sp.sunDir (0.6,0.5,0.62). We put a
        // PHYSICAL emissive body along that SAME ray so the painted disc and the real
        // star coincide, and it visibly GROWS as you close (a real destination, not
        // sky paint). Layered emissive spheres beat a fancy shader here (cheap): a
        // white-gold CORE driven well past the bloom threshold (0.92) so it blooms,
        // plus additive translucent corona shells (warm gradient) around it.
        //
        // CHOSEN CONSTANTS (tuned for maxSpeed ~200-340 m/s ships; the dive from the
        // 15 km heat warning to the ~2.5 km surface should take ~1 min at cruise):
        //   kSunDist   20000 m   — spawn→star centre. NB: pulled in from the owner's
        //                          50 km starting point — at 50 km the disc's depth
        //                          compresses into the analytic-sky far-depth and the
        //                          sky overwrites it (empirically: renders < ~25 km,
        //                          vanishes at 50 km with far=60 km). 20 km keeps the
        //                          disc visible from spawn AND a ~90 s cruise journey.
        //   kSunRadius  2000 m   — core radius (real disc from spawn, fills frame near)
        // Heat/death use SURFACE distance  dSurf = |ship-centre| - kSunRadius:
        //   kHeatStart 15000 m   — hull temp begins to climb (NOMINAL→)
        //   kWarnDist   8000 m   — WARNING  (orange, "HULL TEMP RISING")
        //   kCritDist   4000 m   — CRITICAL (red flashing, "PULL AWAY", beep)
        //   kLightDist 10000 m   — warm proximity point-light ramps up (hull heat-glow)
        // Crossing the BODY (dSurf < 0) no longer kills — a shield engages and a 17 s
        // countdown + kill-cam sequence runs (see the Phase machine below).
        const x3::phys::Vec3 kSunDir    = vnorm(x3::phys::Vec3{ 0.6f, 0.5f, 0.62f });
        const float          kSunDist   = 20000.0f;
        const x3::phys::Vec3 kSunCenter = { kSunDir.x*kSunDist, kSunDir.y*kSunDist, kSunDir.z*kSunDist };
        const float          kSunRadius = 2000.0f;
        const float          kHeatStart = 15000.0f;
        const float          kWarnDist  = 8000.0f;
        const float          kCritDist  = 4000.0f;
        const float          kLightDist = 10000.0f;
        // Push the far plane past the star (50 km + radius) so it renders at range.
        device->setCameraFar(60000.0f);
        // Big smooth sphere for the sun core + corona shells + shield + shockwaves.
        x3::prims::PrimMesh sunm = x3::prims::makeUVSphere(48, 96);
        auto sunMesh = device->createMesh(sunm.verts.data(), (uint32_t)sunm.verts.size(),
                                          sunm.index.data(), (uint32_t)sunm.index.size());

        // ---- Local math helpers (presentation only) -----------------------
        auto smooth01 = [](float e0, float e1, float x) -> float {
            // NB: e0>e1 is a valid INVERTED ramp (used by the heat/light curves), so
            // the zero-guard must preserve the denominator's SIGN, not force positive.
            float d = e1 - e0;
            if (std::fabs(d) < 1e-6f) d = (d < 0.0f) ? -1e-6f : 1e-6f;
            float t = (x - e0) / d;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            return t * t * (3.0f - 2.0f * t);
        };
        auto vlen = [](const x3::phys::Vec3& a) {
            return std::sqrt(a.x*a.x + a.y*a.y + a.z*a.z);
        };
        // Build the +90°-yaw-corrected ship matrix (columns -r, u, f) at a pose.
        auto shipMatrix = [](const x3::phys::Vec3& p, const x3::phys::Vec3& f,
                             const x3::phys::Vec3& u, const x3::phys::Vec3& r, float m[16]) {
            m[0]=-r.x; m[1]=-r.y; m[2]=-r.z; m[3]=0;
            m[4]= u.x; m[5]= u.y; m[6]= u.z; m[7]=0;
            m[8]= f.x; m[9]= f.y; m[10]=f.z; m[11]=0;
            m[12]=p.x; m[13]=p.y; m[14]=p.z; m[15]=1;
        };
        // Translate+uniform-scale matrix (for the sun/corona/shield/shockwave spheres).
        auto sphereMatrix = [](const x3::phys::Vec3& c, float s, float m[16]) {
            m[0]=s; m[1]=0; m[2]=0; m[3]=0;  m[4]=0; m[5]=s; m[6]=0; m[7]=0;
            m[8]=0; m[9]=0; m[10]=s; m[11]=0; m[12]=c.x; m[13]=c.y; m[14]=c.z; m[15]=1;
        };

        // ---- SUN-DEATH cinematic PHASE MACHINE (host-side; controller untouched) --
        // Flying → (cross body) → InsideSun[17s shield drain] → Detonation[antimatter
        // blast + coronal ejection, external kill-cam, sun ~half screen] → Rewind[~1s
        // backwards-scrub stinger of the ejection] → TitleCard["30 SECONDS EARLIER…"
        // film card on near-black] → Replay[forward re-entry from the external vantage:
        // the ship cruises in, breaches, entry flash] → Respawn[fade to black, re-seed
        // pose, "HULL LOST TO THE SUN — SHIELD HELD 17.0s", fade in] → Flying. Any key
        // skips straight to Respawn.
        enum class Phase { Flying, InsideSun, Detonation, Rewind, TitleCard, Replay, Respawn };
        Phase phase   = Phase::Flying;
        float phaseT  = 0.0f;                 // seconds elapsed in the current phase
        float shieldPct = 100.0f;
        bool  respawned = false;              // Respawn phase re-seed latch
        const float kShieldSecs   = 17.0f;    // shield holds this long inside the body
        const float kDetonateSecs = 4.5f;     // blast + coronal ejection duration
        const float kRewindSecs   = 1.0f;     // short backwards-scrub stinger
        const float kTitleSecs    = 2.6f;     // "30 SECONDS EARLIER…" card (incl. fades)
        const float kReplaySecs   = 6.5f;     // forward re-entry replay (whole buffer)
        const float kFadeSecs     = 1.0f;     // fade-to-black → respawn → fade-in
        const int   kDebrisCount  = 32;       // coronal-ejection emissive fragments
        x3::phys::Vec3 entryPos{ 0,0,0 };     // surface impact point (debris origin)
        x3::phys::Vec3 entryNrm{ 0,1,0 };     // outward normal at the impact point
        x3::phys::Vec3 cineCamPos{ 0,0,0 };   // frozen external kill-cam position
        float cineYaw = 0.0f, cinePit = 0.0f; // frozen external kill-cam look
        // Trajectory RING BUFFER — record the ship pose continuously so the kill-cam
        // can scrub/replay the fatal approach. 15 Hz × 32 s (>= the 30 s the title card
        // promises), render-only.
        struct TrajSample { x3::phys::Vec3 p, f, u, r; };
        const float kTrajHz = 15.0f;
        const int   kTrajLen = (int)(kTrajHz * 32.0f);   // 480 samples = 32 s
        std::vector<TrajSample> trajRing((size_t)kTrajLen);
        int   trajHead = 0, trajCount = 0;
        float trajTimer = 0.0f;
        std::vector<TrajSample> trajPlay;     // linearised oldest→entry at detonation
        // Loop-shared telemetry the HUD/overlay read (updated each frame in the loop).
        float g_heat = 0.0f;        // 0..1 hull-heat fraction
        float g_sunSurf = 1e9f;     // distance to the SUN SURFACE (m)
        float g_clock = 0.0f;       // presentation clock (blink/pulse/flash phases)

        // ---- Static decor fleet: a wing formation a few dozen meters out
        //      Each is a static placement transform (rotation around +Y for variety).
        //      Coordinates chosen so the headless screenshot camera (at -X behind
        //      the player ship, looking toward +X) sees a tight cluster of ships
        //      filling a good portion of the frame.
        struct DecorShip { float x, y, z, yaw, scale; };
        const DecorShip decor[] = {
            {   30.0f,   2.0f,    8.0f,  0.2f, 18.0f },  // close right
            {   35.0f,   4.0f,  -10.0f, -0.3f, 20.0f },  // close left, up
            {   45.0f,  -2.0f,   18.0f,  0.4f, 22.0f },  // mid-right, down
            {   55.0f,   8.0f,   -4.0f,  0.0f, 24.0f },  // mid lead, up
            {   80.0f,   0.0f,  -25.0f,  0.6f, 28.0f },  // far escort left
            {   80.0f,   2.0f,   25.0f, -0.6f, 28.0f },  // far escort right
        };
        const int kDecorCount = (int)(sizeof(decor) / sizeof(decor[0]));

        // CombatFx for laser tracers + impact decals. Heap-allocated because
        // CombatFx carries ~256 KB of mutable scratch instance arrays; piling
        // another stack copy into main() (which already holds one for the
        // canonplay/level1 paths) overflows the 1 MB default thread stack.
        auto combatFxOwned = std::make_unique<x3::game::CombatFx>();
        x3::game::CombatFx& combatFx = *combatFxOwned;
        combatFx.init(*device);

        const float dt = 1.0f / 60.0f;

        // Draw a ship at a placement matrix (yaw-only for decor; full quat for
        // the player ship). W6-2: ships ride the FULL PBR path now (normal/MR/
        // authored emissive — the same conversion monsters got in Wave 1), so
        // hulls catch the light rig as lit metal instead of the old basic-path
        // "×4 + 0.45 floor" albedo hack that flattened them to silhouettes.
        // `bright` is a gentle exposure assist for deep space (no bounce light),
        // applied as a modest albedo scale, not a floor.
        auto drawShipAt = [&](const x3::rhi::FrameContext& frame,
                              const float xform[16], float bright) {
            if (shipModel.ok) {
                for (const auto& dr : shipDrawables) {
                    float fin[16];
                    x3::asset::mulMat4(xform, dr.nodeTransform, fin);
                    const float b = 1.0f + 1.2f * bright;   // exposure assist (no floor)
                    const float tint[4] = {
                        dr.baseColorFactor[0] * b,
                        dr.baseColorFactor[1] * b,
                        dr.baseColorFactor[2] * b,
                        dr.baseColorFactor[3]
                    };
                    // Authored emissive (canopies/engine glow) at full strength, PLUS a
                    // faint cool STARLIGHT AMBIENT floor: deep space has no bounce term,
                    // so pure PBR renders near-black hulls invisible. The floor keeps the
                    // silhouette readable as dim metal while normals/MR still shade from
                    // the real light rig. Kept far below bloom threshold (bible: no blobs).
                    const float amb = 0.020f * (1.0f + bright);   // R3: halved — the
                                                                  // directional key carries
                                                                  // the shading now
                    const float emis[4] = { dr.emissiveFactor[0] + amb,
                                            dr.emissiveFactor[1] + amb * 1.05f,
                                            dr.emissiveFactor[2] + amb * 1.25f, 1.0f };
                    device->drawMeshPBR(frame, x3::rhi::MeshHandle{ dr.meshId },
                                        x3::rhi::TextureHandle{ dr.baseColorTexId },
                                        x3::rhi::TextureHandle{ dr.normalTexId },
                                        x3::rhi::TextureHandle{ dr.mrTexId },
                                        tint, emis, fin,
                                        dr.alphaMask, dr.alphaBlend,
                                        x3::rhi::TextureHandle{ dr.emissiveTexId },
                                        x3::rhi::TextureHandle{ dr.detailTexId },
                                        dr.detailUvScale);
                }
            } else {
                const float white[4] = { bright, bright, bright, 1.0f };
                device->drawMesh(frame, shipBoxMesh, shipBoxTex, white, xform);
            }
        };

        auto drawScene = [&](const x3::rhi::FrameContext& frame) {
            // Player ship: build a 4x4 from quaternion + position (visible only
            // in 3P; in 1P it would clip the near plane — host gates visuals).
            if (pilot.isThirdPerson()) {
                const x3::phys::Vec3 p = pilot.pos();
                const x3::phys::Vec3 f = pilot.forward();
                const x3::phys::Vec3 r = pilot.right();
                const x3::phys::Vec3 u = pilot.up();
                // +90deg yaw correction: the Minerva GLB is authored with its
                // NOSE along model +Z (where the engine convention puts the right
                // wing), so mapping model +X->forward rendered it yawed 90deg to
                // starboard. Remap so the model's +Z nose points along forward():
                // columns (f,u,r) -> (-r,u,f). This is a proper rotation (det +1,
                // (-r)x u = f), so the hull is rotated, NOT mirrored.
                float m[16] = {
                    -r.x, -r.y, -r.z, 0,  // col 0 = model +X  (<- -right)
                    u.x,  u.y,  u.z,  0,  // col 1 = model +Y  (up)
                    f.x,  f.y,  f.z,  0,  // col 2 = model +Z = nose -> forward
                    p.x,  p.y,  p.z,  1
                };
                drawShipAt(frame, m, 1.5f);
            }
            // Decor fleet.
            for (int i = 0; i < kDecorCount; ++i) {
                const float c = std::cos(decor[i].yaw), s = std::sin(decor[i].yaw);
                const float S = decor[i].scale;
                float m[16] = {
                    c*S, 0,  -s*S, 0,
                    0,   S,  0,   0,
                    s*S, 0,  c*S, 0,
                    decor[i].x, decor[i].y, decor[i].z, 1
                };
                drawShipAt(frame, m, 1.0f);
            }
        };

        // ---- Advance the dust field (drift opposite velocity + wrap) -------
        auto updateDust = [&](float d) {
            const x3::phys::Vec3 sp = pilot.pos();
            const x3::phys::Vec3 vel = pilot.velocity();
            for (int i = 0; i < kDust; ++i) {
                x3::phys::Vec3& p = dust[(size_t)i];
                p.x -= vel.x * d; p.y -= vel.y * d; p.z -= vel.z * d;
                auto wrap = [&](float& c, float center) {
                    float dd = c - center;
                    while (dd >  kDustR) { c -= 2*kDustR; dd -= 2*kDustR; }
                    while (dd < -kDustR) { c += 2*kDustR; dd += 2*kDustR; }
                };
                wrap(p.x, sp.x); wrap(p.y, sp.y); wrap(p.z, sp.z);
            }
        };

        // ---- Draw the near-field streak/dust specks ------------------------
        auto drawSpeedFx = [&](const x3::rhi::FrameContext& frame) {
            const x3::phys::Vec3 sp   = pilot.pos();
            const x3::phys::Vec3 vel  = pilot.velocity();
            const float spd  = pilot.speed();
            const float maxS = std::max(1.0f, pilot.tuning().maxSpeed);
            const float sf   = std::min(1.0f, spd / maxS);
            // EASED response: length/brightness ride a smoothstep of speed (graceful
            // ramp-in), plus a soft extra kick on boost. Capped so no giant beams.
            const float resp = smooth01(0.0f, 1.0f, sf) * (boostActive ? 1.30f : 1.0f);
            x3::phys::Vec3 vd{ 1, 0, 0 };
            if (spd > 0.3f) vd = x3::phys::Vec3{ vel.x/spd, vel.y/spd, vel.z/spd };
            const x3::phys::Vec3 ref = (std::fabs(vd.y) < 0.95f)
                                     ? x3::phys::Vec3{ 0, 1, 0 } : x3::phys::Vec3{ 1, 0, 0 };
            const x3::phys::Vec3 uax = vnorm(vcross(ref, vd));
            const x3::phys::Vec3 vax = vcross(vd, uax);
            const bool moving = resp > 0.03f;
            // Head→mid→tail segment layout (fakes a bright-head/transparent-tail taper;
            // the emissive path has no per-vertex alpha). offset·halfLen, thick·, str·.
            const float segOff[3]  = { -0.55f, 0.0f, 0.55f };
            const float segThk[3]  = {  1.00f, 0.72f, 0.50f };
            const float segStr[3]  = {  1.00f, 0.55f, 0.28f };
            const float kMaxHalf   = 1.15f;   // cap: total streak <= ~2.5 m at scale
            for (int i = 0; i < kDust; ++i) {
                const x3::phys::Vec3& p = dust[(size_t)i];
                // Per-particle hash variance: size 0.5-1.5, brightness 0.7-1.2, temp.
                const float hsz  = 0.5f + hashF((uint32_t)(i*7 + 5));
                const float hbr  = 0.7f + 0.5f * hashF((uint32_t)(i*11 + 3));
                const float htmp = hashF((uint32_t)(i*13 + 9));
                // Boundary-shell fade: nothing pops in/out at the wrap (fades over the
                // outer ~18% of the box). Chebyshev distance to the wrap boundary.
                const float dcx = std::fabs(p.x - sp.x), dcy = std::fabs(p.y - sp.y),
                            dcz = std::fabs(p.z - sp.z);
                const float dedge = std::max(dcx, std::max(dcy, dcz)) / kDustR;
                const float fade = 1.0f - smooth01(0.82f, 1.0f, dedge);
                if (fade < 0.02f) continue;
                // Colour temperature: cool blue-white → warm white per particle.
                const float base[4] = {
                    0.55f + 0.45f * htmp,
                    0.80f + 0.15f * htmp,
                    1.00f - 0.15f * htmp, 1.0f };
                const float halfLen = std::min(kMaxHalf, 0.06f + kMaxHalf * resp * hsz);
                const float thick   = (0.012f + 0.020f * sf) * hsz;
                const float S = (0.22f + 2.6f * resp) * hbr * fade;
                const int segs = moving ? 3 : 1;
                for (int s = 0; s < segs; ++s) {
                    const float str = S * segStr[s];
                    if (str < 0.015f) continue;
                    const x3::phys::Vec3 c{ p.x + vd.x*halfLen*segOff[s],
                                            p.y + vd.y*halfLen*segOff[s],
                                            p.z + vd.z*halfLen*segOff[s] };
                    float m[16];
                    composeBasis(m, uax, vax, vd, thick*segThk[s], thick*segThk[s],
                                 std::max(thick, halfLen*0.42f), c);
                    const float emis[4] = { base[0], base[1], base[2], str };
                    device->drawMeshEmissive(frame, dustMesh, x3::rhi::TextureHandle{}, base, emis, m);
                }
            }
        };

        // ---- REAL SUN render: white-gold core + additive corona shells ----------
        // Core emissive strength is driven well past the bloom threshold (0.92) so it
        // blooms; corona shells are translucent (drawMeshGlass) so the core shows
        // through, drawn largest→smallest (rough back-to-front) with a warm gradient.
        auto drawSun = [&](const x3::rhi::FrameContext& frame) {
            float m[16];
            // Corona shells (outer, faint, warm) — translucent additive glow.
            const struct { float s, r, g, b, str, op; } shells[3] = {
                { 2.6f, 1.00f, 0.42f, 0.12f, 0.9f, 0.10f },   // deep orange, big
                { 1.7f, 1.00f, 0.62f, 0.28f, 1.6f, 0.16f },   // amber
                { 1.28f,1.00f, 0.86f, 0.55f, 3.0f, 0.28f },   // gold, tight
            };
            for (const auto& sh : shells) {
                sphereMatrix(kSunCenter, kSunRadius * sh.s, m);
                const float bc[4]  = { sh.r, sh.g, sh.b, 1.0f };
                const float em[4]  = { sh.r, sh.g, sh.b, sh.str };
                x3::rhi::IRenderDevice::GlassMaterial gm{};
                gm.opacity = sh.op; gm.roughness = 1.0f; gm.specular = 0.0f;
                gm.tint[0] = sh.r; gm.tint[1] = sh.g; gm.tint[2] = sh.b;
                device->drawMeshGlass(frame, sunMesh, x3::rhi::TextureHandle{}, bc, em, gm, m);
            }
            // White-gold core (opaque emissive, far past bloom threshold).
            sphereMatrix(kSunCenter, kSunRadius, m);
            const float cbc[4] = { 1.0f, 0.93f, 0.78f, 1.0f };
            const float cem[4] = { 1.0f, 0.93f, 0.78f, 6.0f };
            device->drawMeshEmissive(frame, sunMesh, x3::rhi::TextureHandle{}, cbc, cem, m);
        };

        // ---- Dynamic point lights: player-key (follows ship) + sun-heat ---------
        auto updateDynamicLights = [&](const x3::phys::Vec3& sPos, const x3::phys::Vec3& f,
                                       const x3::phys::Vec3& u, const x3::phys::Vec3& r) {
            // [3] PLAYER KEY — offset up + behind + to the camera side so the hull
            // silhouette reads while flying. Subtle (lit metal, not a floodlight).
            const x3::phys::Vec3 kp{ sPos.x + u.x*6.0f - f.x*4.0f + r.x*5.0f,
                                     sPos.y + u.y*6.0f - f.y*4.0f + r.y*5.0f,
                                     sPos.z + u.z*6.0f - f.z*4.0f + r.z*5.0f };
            plights[3].pos[0]=kp.x; plights[3].pos[1]=kp.y; plights[3].pos[2]=kp.z;
            plights[3].range = 42.0f;
            plights[3].color[0]=16.0f; plights[3].color[1]=17.0f; plights[3].color[2]=22.0f;
            // [4] SUN HEAT — warm light between ship and sun that ramps up only within
            // kLightDist of the surface (far-away look unchanged), so hulls/cockpit
            // visibly heat-glow on the dive. Independent smoothstep on surface range.
            const float lightRamp = smooth01(kLightDist, kSunRadius, g_sunSurf);
            const x3::phys::Vec3 hp{ sPos.x + kSunDir.x*30.0f,
                                     sPos.y + kSunDir.y*30.0f,
                                     sPos.z + kSunDir.z*30.0f };
            const float hi = lightRamp * lightRamp * 110.0f;
            plights[4].pos[0]=hp.x; plights[4].pos[1]=hp.y; plights[4].pos[2]=hp.z;
            plights[4].range = 120.0f;
            plights[4].color[0]=hi*1.0f; plights[4].color[1]=hi*0.45f; plights[4].color[2]=hi*0.12f;
            device->setPointLights(plights, 5);
        };

        // ---- PLAYER-SHIP lights: engine glow + red/green/white nav beacons ------
        // Anchored to the pilot basis (forward/right/up). The Minerva's wings run
        // along ±right() in world (model +X → -right in the compose above), so the
        // wingtip nav dots sit on the real wings. All emissive/render-only.
        auto drawShipLights = [&](const x3::rhi::FrameContext& frame, float thrust01,
                                  float blinkT) {
            const x3::phys::Vec3 p = pilot.pos();
            const x3::phys::Vec3 f = pilot.forward();
            const x3::phys::Vec3 r = pilot.right();
            const x3::phys::Vec3 u = pilot.up();
            auto dot = [&](const x3::phys::Vec3& c, float rad, float cr, float cg,
                           float cb, float str) {
                float m[16]; sphereMatrix(c, rad, m);
                const float bc[4] = { cr, cg, cb, 1.0f };
                const float em[4] = { cr, cg, cb, str };
                device->drawMeshEmissive(frame, dustMesh, x3::rhi::TextureHandle{}, bc, em, m);
            };
            // ENGINE GLOW: twin nozzles at the rear (−forward), brightness scales with
            // thrust/speed (+ boost handled by the caller folding it into thrust01).
            const float eStr = 0.8f + 4.5f * thrust01;
            const float eRad = 0.16f + 0.10f * thrust01;
            for (float side : { -1.0f, 1.0f }) {
                const x3::phys::Vec3 e{ p.x - f.x*1.4f + r.x*0.55f*side - u.x*0.1f,
                                        p.y - f.y*1.4f + r.y*0.55f*side - u.y*0.1f,
                                        p.z - f.z*1.4f + r.z*0.55f*side - u.z*0.1f };
                dot(e, eRad, 0.45f, 0.85f, 1.0f, eStr);   // cyan-white engine plume
            }
            // NAV/RUNNING lights at the wingtips: RED port (left = −right), GREEN
            // starboard (right = +right); WHITE tail beacon that blinks slowly.
            const float span = 1.6f, back = 0.6f;
            const x3::phys::Vec3 portTip{ p.x - r.x*span - f.x*back, p.y - r.y*span - f.y*back, p.z - r.z*span - f.z*back };
            const x3::phys::Vec3 stbdTip{ p.x + r.x*span - f.x*back, p.y + r.y*span - f.y*back, p.z + r.z*span - f.z*back };
            dot(portTip, 0.11f, 1.0f, 0.10f, 0.10f, 2.6f);   // RED  (port)
            dot(stbdTip, 0.11f, 0.10f, 1.0f, 0.20f, 2.6f);   // GREEN(starboard)
            // WHITE tail beacon — slow deterministic blink (on ~half the cycle).
            const float blink = (std::sin(blinkT * 3.2f) > 0.2f) ? 1.0f : 0.12f;
            const x3::phys::Vec3 tail{ p.x - f.x*1.7f + u.x*0.5f, p.y - f.y*1.7f + u.y*0.5f, p.z - f.z*1.7f + u.z*0.5f };
            dot(tail, 0.10f, 1.0f, 1.0f, 1.0f, 3.0f * blink);
        };

        // ---- SHIELD shell (engages when inside the sun body) --------------------
        // No engine shield-material path is reachable from this host, so the shield is
        // an additive translucent emissive sphere around the ship (drains with % ).
        auto drawShield = [&](const x3::rhi::FrameContext& frame, float pct, float pulseT) {
            const x3::phys::Vec3 p = pilot.pos();
            const float k = std::max(0.0f, pct) / 100.0f;
            const float pulse = 0.75f + 0.25f * std::sin(pulseT * 6.0f);
            float m[16]; sphereMatrix(p, 2.6f, m);
            const float bc[4] = { 0.35f, 0.75f, 1.0f, 1.0f };
            const float em[4] = { 0.35f, 0.75f, 1.0f, (0.6f + 2.2f * k) * pulse };
            x3::rhi::IRenderDevice::GlassMaterial gm{};
            gm.opacity = 0.18f + 0.22f * k; gm.roughness = 0.4f; gm.specular = 0.4f;
            gm.tint[0]=0.4f; gm.tint[1]=0.75f; gm.tint[2]=1.0f;
            device->drawMeshGlass(frame, sunMesh, x3::rhi::TextureHandle{}, bc, em, gm, m);
        };

        // ---- CORONAL EJECTION: superheated ship debris + shockwave shells -------
        // Deterministic (hash-seeded direction/speed), render-only. `t` = seconds into
        // the Detonation phase; `rev` reverses time for the backwards-scrub stinger.
        auto drawEjecta = [&](const x3::rhi::FrameContext& frame, float t) {
            // Expanding emissive shockwave shells at the impact point.
            for (int s = 0; s < 2; ++s) {
                const float t0 = (float)s * 0.6f;
                const float lt = t - t0;
                if (lt < 0.0f || lt > 2.4f) continue;
                const float rad = kSunRadius * (0.25f + lt * 0.9f);
                const float a   = (1.0f - lt / 2.4f);
                float m[16]; sphereMatrix(entryPos, rad, m);
                const float bc[4] = { 1.0f, 0.7f, 0.35f, 1.0f };
                const float em[4] = { 1.0f, 0.7f, 0.35f, 2.5f * a };
                x3::rhi::IRenderDevice::GlassMaterial gm{};
                gm.opacity = 0.14f * a; gm.roughness = 1.0f; gm.specular = 0.0f;
                gm.tint[0]=1.0f; gm.tint[1]=0.7f; gm.tint[2]=0.35f;
                device->drawMeshGlass(frame, sunMesh, x3::rhi::TextureHandle{}, bc, em, gm, m);
            }
            // Debris fragments launched outward in a cone about the surface normal,
            // decelerating; each drawn as a warm emissive streak along its velocity.
            const x3::phys::Vec3 up = (std::fabs(entryNrm.y) < 0.95f)
                                    ? x3::phys::Vec3{ 0,1,0 } : x3::phys::Vec3{ 1,0,0 };
            const x3::phys::Vec3 ta = vnorm(vcross(up, entryNrm));
            const x3::phys::Vec3 tb = vcross(entryNrm, ta);
            for (int i = 0; i < kDebrisCount; ++i) {
                const float h1 = hashF((uint32_t)(i*5+1)), h2 = hashF((uint32_t)(i*5+2));
                const float h3 = hashF((uint32_t)(i*5+3)), h4 = hashF((uint32_t)(i*5+4));
                const float ang  = h1 * 6.2831853f;
                const float cone = 0.35f + 0.5f * h2;              // spread off the normal
                x3::phys::Vec3 dir = vnorm(x3::phys::Vec3{
                    entryNrm.x + (ta.x*std::cos(ang) + tb.x*std::sin(ang)) * cone,
                    entryNrm.y + (ta.y*std::cos(ang) + tb.y*std::sin(ang)) * cone,
                    entryNrm.z + (ta.z*std::cos(ang) + tb.z*std::sin(ang)) * cone });
                const float v0   = kSunRadius * (0.9f + 1.6f * h3);  // launch speed
                // Decelerating travel: distance = v0*(t - 0.5*k*t^2), clamped.
                const float tt = std::min(t, 3.5f);
                const float dist = v0 * (tt - 0.12f * tt * tt);
                const x3::phys::Vec3 c{ entryPos.x + dir.x*dist,
                                        entryPos.y + dir.y*dist,
                                        entryPos.z + dir.z*dist };
                const float life = std::min(1.0f, t / 3.5f);
                const float str  = (3.5f - 3.0f * life) * (0.6f + 0.6f * h4);
                if (str < 0.05f) continue;
                const x3::phys::Vec3 uax = vnorm(vcross(dir, up));
                const x3::phys::Vec3 vax = vcross(dir, uax);
                float m[16];
                composeBasis(m, uax, vax, dir, 40.0f, 40.0f, 130.0f + 90.0f*h3, c);
                const float col[4] = { 1.0f, 0.55f + 0.35f*h4, 0.20f, 1.0f };
                const float em[4]  = { col[0], col[1], col[2], str };
                device->drawMeshEmissive(frame, dustMesh, x3::rhi::TextureHandle{}, col, em, m);
            }
        };

        // ---- Flight HUD: active-mode readout + telemetry (reuses the engine's
        //      FontRole HUD text path — device->drawHudTextF / drawHudQuad, the
        //      SAME mechanism the ship-windows cockpit uses). Neon cyberpunk feel.
        auto drawHud = [&](const x3::rhi::FrameContext& frame, float W, float H) {
            using x3::rhi::FontRole;
            const x3::game::FlightMode fm = pilot.mode();
            const char* nm = "ARCADE"; const char* tag = "RESPONSIVE";
            float acc[4] = { 0.30f, 0.95f, 1.0f, 1.0f };            // cyan (Arcade)
            if (fm == x3::game::FlightMode::Assist) {
                nm = "ASSIST"; tag = "WEIGHTY";
                acc[0]=1.0f; acc[1]=0.35f; acc[2]=0.85f;            // magenta (Assist)
            } else if (fm == x3::game::FlightMode::Loose) {
                nm = "LOOSE"; tag = "DRIFT";
                acc[0]=0.75f; acc[1]=0.55f; acc[2]=1.0f;            // violet (Loose)
            }
            // Active-mode readout: prominent, top-center, Orbitron (Title role).
            const float titlePx = 40.0f;
            const float tw = device->textAdvance(FontRole::Title, nm, titlePx);
            const float tx = W * 0.5f - tw * 0.5f;
            const float ty = 22.0f;
            const float bg[4] = { 0.02f, 0.03f, 0.05f, 0.55f };
            device->drawHudQuad(frame, tx - 20.0f, ty - 8.0f, tw + 40.0f, titlePx + 30.0f, bg);
            const float ul[4] = { acc[0], acc[1], acc[2], 0.9f };
            device->drawHudQuad(frame, tx - 20.0f, ty + titlePx + 14.0f, tw + 40.0f, 3.0f, ul);
            device->drawHudTextF(frame, FontRole::Title, nm, tx, ty, titlePx, acc);
            // FIX (Integrator): drop the tagline a few px BELOW the underline so it no
            // longer clips into it (underline bottom ≈ ty+titlePx+17; tag now at +22).
            const float tagPx = 14.0f;
            const float tgw = device->textAdvance(FontRole::HudMono, tag, tagPx);
            const float dim[4] = { acc[0]*0.85f, acc[1]*0.85f, acc[2]*0.85f, 0.85f };
            device->drawHudTextF(frame, FontRole::HudMono, tag,
                                 W * 0.5f - tgw * 0.5f, ty + titlePx + 22.0f, tagPx, dim);

            // Telemetry corner (bottom-left): speed, heading, hull temp, sun range, pos.
            const float spd = pilot.speed();
            const x3::phys::Vec3 pp = pilot.pos();
            // FIX (Integrator): wrap heading to [0,360) so it never reads "+406".
            float hdg = std::fmod(pilot.yaw() * 57.29578f, 360.0f);
            if (hdg < 0.0f) hdg += 360.0f;
            // Hull temp: ambient baseline far away → climbs with the heat fraction.
            const float tempC = 22.0f + g_heat * 3180.0f;
            const bool  warn = (g_sunSurf < kWarnDist);
            const bool  crit = (g_sunSurf < kCritDist);
            char l1[64], l2[64], l3[80], l4[64], l5[64];
            std::snprintf(l1, sizeof(l1), "SPD %6.1f m/s", spd);
            std::snprintf(l2, sizeof(l2), "HDG %03.0f   BOOST %s", (double)hdg, boostActive ? "ON " : "off");
            const char* tState = crit ? "CRITICAL  PULL AWAY" : warn ? "RISING" : "NOMINAL";
            std::snprintf(l3, sizeof(l3), "HULL TEMP %4.0fC  %s", (double)tempC, tState);
            const float sunKm = std::max(0.0f, g_sunSurf) / 1000.0f;
            std::snprintf(l4, sizeof(l4), "SUN %7.1f km", (double)sunKm);
            std::snprintf(l5, sizeof(l5), "POS %5.0f %5.0f %5.0f", (double)pp.x, (double)pp.y, (double)pp.z);
            const float tpx = 16.0f;
            const float bx = 20.0f, by = H - 132.0f;
            const float tbg[4] = { 0.02f, 0.03f, 0.05f, 0.5f };
            device->drawHudQuad(frame, bx - 10.0f, by - 8.0f, 320.0f, 124.0f, tbg);
            const float cyan[4] = { 0.60f, 0.90f, 1.0f, 0.95f };
            // Temp line colour shifts orange (WARNING) → flashing red (CRITICAL).
            float tcol[4] = { 0.60f, 0.90f, 1.0f, 0.95f };
            if (crit) { const float fl = (std::sin(g_clock * 9.0f) > 0.0f) ? 1.0f : 0.45f;
                        tcol[0]=1.0f; tcol[1]=0.20f; tcol[2]=0.15f; tcol[3]=fl; }
            else if (warn) { tcol[0]=1.0f; tcol[1]=0.55f; tcol[2]=0.15f; }
            device->drawHudText(frame, l1, bx, by,          tpx, cyan);
            device->drawHudText(frame, l2, bx, by + 20.0f,  tpx, cyan);
            device->drawHudText(frame, l3, bx, by + 40.0f,  tpx, tcol);
            device->drawHudText(frame, l4, bx, by + 60.0f,  tpx, cyan);
            device->drawHudText(frame, l5, bx, by + 80.0f,  tpx, cyan);
            const float barW = 290.0f, barH = 6.0f, barY = by + 108.0f;
            const float bbg[4] = { 0.10f, 0.15f, 0.20f, 0.7f };
            device->drawHudQuad(frame, bx, barY, barW, barH, bbg);
            const float frac = std::min(1.0f, spd / std::max(1.0f, pilot.tuning().maxSpeed));
            const float fill[4] = { acc[0], acc[1], acc[2], 0.95f };
            device->drawHudQuad(frame, bx, barY, barW * frac, barH, fill);
            // SHIELD readout while it is engaged (inside the star).
            if (phase == Phase::InsideSun) {
                char sl[48]; std::snprintf(sl, sizeof(sl), "SHIELD %3.0f%%", (double)std::max(0.0f, shieldPct));
                const float sc[4] = { 0.4f, 0.8f, 1.0f, 1.0f };
                device->drawHudTextF(frame, FontRole::HudMono, sl, bx, by - 30.0f, 18.0f, sc);
            }
        };

        // ---- CINEMATIC OVERLAY: molten wash, shield countdown, kill-cam flash,
        //      rewind tag, "30 SECONDS EARLIER…" title card, fade-to-black. Drawn
        //      after the HUD when the death sequence is active. -------------------
        auto drawCinematic = [&](const x3::rhi::FrameContext& frame, float W, float H) {
            using x3::rhi::FontRole;
            auto full = [&](float r, float g, float b, float a) {
                if (a <= 0.001f) return; const float c[4] = { r, g, b, a };
                device->drawHudQuad(frame, 0, 0, W, H, c);
            };
            auto center = [&](FontRole role, const char* s, float px, float y, const float col[4]) {
                const float w = device->textAdvance(role, s, px);
                device->drawHudTextF(frame, role, s, W*0.5f - w*0.5f, y, px, col);
            };
            if (phase == Phase::InsideSun) {
                // Molten warm wash, gently pulsing; keep the HUD readable underneath.
                const float pulse = 0.42f + 0.10f * std::sin(g_clock * 5.0f);
                full(1.0f, 0.45f, 0.12f, pulse);
                // Big centre-low shield-failing countdown, red-shifting as it drops.
                const float rem = std::max(0.0f, kShieldSecs - phaseT);
                const float k = rem / kShieldSecs;
                char cd[48]; std::snprintf(cd, sizeof(cd), "SHIELD FAILING IN %4.1fs", (double)rem);
                const float col[4] = { 1.0f, 0.30f + 0.55f*k, 0.20f*k, 1.0f };
                center(FontRole::Title, cd, 40.0f, H*0.62f, col);
            } else if (phase == Phase::Detonation) {
                // Blinding white flash on the blast, decaying over ~0.6s.
                const float fl = 1.0f - smooth01(0.0f, 0.6f, phaseT);
                full(1.0f, 0.96f, 0.9f, fl * 0.95f);
            } else if (phase == Phase::Rewind) {
                // Short backwards-scrub stinger: desaturating dark vignette + tag.
                full(0.0f, 0.0f, 0.02f, 0.28f);
                const float col[4] = { 0.8f, 0.85f, 1.0f, 0.9f };
                center(FontRole::Title, "<< REWIND", 30.0f, H*0.12f, col);
            } else if (phase == Phase::TitleCard) {
                // Film card on near-black: fade in, hold, fade out.
                const float aIn  = smooth01(0.0f, 0.6f, phaseT);
                const float aOut = 1.0f - smooth01(kTitleSecs - 0.6f, kTitleSecs, phaseT);
                const float a = std::min(aIn, aOut);
                full(0.0f, 0.0f, 0.0f, 0.72f + 0.28f * a);
                const float col[4] = { 0.92f, 0.90f, 0.85f, a };
                center(FontRole::Title, "3 0   S E C O N D S   E A R L I E R", 30.0f, H*0.46f, col);
            } else if (phase == Phase::Replay) {
                // Entry flash near the end as the replayed ship breaches the surface.
                const float fl = smooth01(kReplaySecs - 0.5f, kReplaySecs, phaseT);
                full(1.0f, 0.9f, 0.7f, fl * 0.85f);
            } else if (phase == Phase::Respawn) {
                // Fade to black → hold (respawn) → fade back in.
                float a;
                if (phaseT < kFadeSecs)                 a = smooth01(0.0f, kFadeSecs, phaseT);
                else if (phaseT < kFadeSecs + 0.8f)     a = 1.0f;
                else                                    a = 1.0f - smooth01(kFadeSecs + 0.8f, 2.0f*kFadeSecs + 0.8f, phaseT);
                full(0.0f, 0.0f, 0.0f, a);
                if (phaseT > kFadeSecs * 0.7f) {
                    const float col[4] = { 0.95f, 0.55f, 0.35f, std::min(1.0f, a) };
                    center(FontRole::Title, "HULL LOST TO THE SUN", 26.0f, H*0.44f, col);
                    const float col2[4] = { 0.8f, 0.85f, 1.0f, std::min(1.0f, a) };
                    center(FontRole::HudMono, "SHIELD HELD 17.0s", 16.0f, H*0.44f + 40.0f, col2);
                }
            }
        };

        // ---- Draw the recorded ship pose (kill-cam rewind/replay). `g` 0→1 walks the
        //      snapshot oldest→entry; lerp position + basis for smooth playback. ----
        auto lerp3 = [](const x3::phys::Vec3& a, const x3::phys::Vec3& b, float t) {
            return x3::phys::Vec3{ a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t };
        };
        auto drawReplayShip = [&](const x3::rhi::FrameContext& frame, float g) {
            if (trajPlay.size() < 2) return;
            g = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
            const float fi = g * (float)(trajPlay.size() - 1);
            int i0 = (int)fi; if (i0 < 0) i0 = 0;
            int i1 = std::min(i0 + 1, (int)trajPlay.size() - 1);
            const float fr = fi - (float)i0;
            const auto& A = trajPlay[(size_t)i0]; const auto& B = trajPlay[(size_t)i1];
            const x3::phys::Vec3 p = lerp3(A.p, B.p, fr);
            const x3::phys::Vec3 f = vnorm(lerp3(A.f, B.f, fr));
            const x3::phys::Vec3 u = vnorm(lerp3(A.u, B.u, fr));
            const x3::phys::Vec3 r = vnorm(lerp3(A.r, B.r, fr));
            float m[16]; shipMatrix(p, f, u, r, m);
            drawShipAt(frame, m, 1.5f);
        };

        // ---- Snapshot the ring buffer (oldest→newest) into trajPlay. ------------
        auto snapshotTraj = [&]() {
            trajPlay.clear();
            for (int i = 0; i < trajCount; ++i) {
                const int idx = ((trajHead - trajCount + i) % kTrajLen + kTrajLen) % kTrajLen;
                trajPlay.push_back(trajRing[(size_t)idx]);
            }
        };

        // ---- Frame the external kill-cam so the sun fills ~half the screen and the
        //      impact point is centred (camera ~1.6 sun-radii off the surface). -----
        auto setupKillCam = [&](const x3::phys::Vec3& shipPos) {
            entryNrm = vnorm(x3::phys::Vec3{ shipPos.x - kSunCenter.x,
                                             shipPos.y - kSunCenter.y,
                                             shipPos.z - kSunCenter.z });
            entryPos = x3::phys::Vec3{ kSunCenter.x + entryNrm.x*kSunRadius,
                                       kSunCenter.y + entryNrm.y*kSunRadius,
                                       kSunCenter.z + entryNrm.z*kSunRadius };
            const x3::phys::Vec3 wup = (std::fabs(entryNrm.y) < 0.95f)
                                     ? x3::phys::Vec3{ 0,1,0 } : x3::phys::Vec3{ 1,0,0 };
            const x3::phys::Vec3 lat = vnorm(vcross(entryNrm, wup));
            const x3::phys::Vec3 cdir = vnorm(x3::phys::Vec3{
                entryNrm.x*0.55f + lat.x*0.83f, entryNrm.y*0.55f + lat.y*0.83f,
                entryNrm.z*0.55f + lat.z*0.83f });
            const float camDist = kSunRadius + 3200.0f;   // ~5200 m from centre
            cineCamPos = x3::phys::Vec3{ kSunCenter.x + cdir.x*camDist,
                                         kSunCenter.y + cdir.y*camDist,
                                         kSunCenter.z + cdir.z*camDist };
            const x3::phys::Vec3 d = vnorm(x3::phys::Vec3{ entryPos.x - cineCamPos.x,
                                                           entryPos.y - cineCamPos.y,
                                                           entryPos.z - cineCamPos.z });
            cinePit = std::asin(std::max(-1.0f, std::min(1.0f, d.y)));
            cineYaw = std::atan2(d.z, d.x);
        };

        // ---- Advance the sun-death PHASE MACHINE (windowed only). Computes heat,
        //      records the trajectory, drives transitions/timers, respawns. Returns
        //      TRUE while the pilot sim should be frozen (sequence active). `skip`
        //      (any key during the cinematic) jumps straight to Respawn. -----------
        auto advanceSequence = [&](float fdt, bool skip) -> bool {
            const x3::phys::Vec3 sPos = pilot.pos();
            const float distC = vlen(x3::phys::Vec3{ sPos.x-kSunCenter.x,
                                     sPos.y-kSunCenter.y, sPos.z-kSunCenter.z });
            g_sunSurf = distC - kSunRadius;
            // Heat curve: 0 far, smoothstep up as the surface distance falls from
            // kHeatStart to the body; a touch of inverse-square bite near the surface.
            const float prox = smooth01(kHeatStart, kSunRadius, g_sunSurf);
            g_heat = prox * prox;
            if (phase == Phase::Flying) {
                // Record the approach at kTrajHz.
                trajTimer += fdt;
                if (trajTimer >= 1.0f / kTrajHz) {
                    trajTimer = 0.0f;
                    trajRing[(size_t)trajHead] = { sPos, pilot.forward(), pilot.up(), pilot.right() };
                    trajHead = (trajHead + 1) % kTrajLen;
                    if (trajCount < kTrajLen) ++trajCount;
                }
                if (distC < kSunRadius) {           // breached the body → shield engages
                    phase = Phase::InsideSun; phaseT = 0.0f; shieldPct = 100.0f;
                    setupKillCam(sPos);
                }
                return false;
            }
            // --- Sequence active (pilot frozen) ---
            phaseT += fdt;
            if (skip && phase != Phase::Respawn) { phase = Phase::Respawn; phaseT = 0.0f; respawned = false; }
            switch (phase) {
                case Phase::InsideSun:
                    shieldPct = 100.0f * std::max(0.0f, 1.0f - phaseT / kShieldSecs);
                    if (phaseT >= kShieldSecs) { snapshotTraj(); phase = Phase::Detonation; phaseT = 0.0f; }
                    break;
                case Phase::Detonation:
                    if (phaseT >= kDetonateSecs) { phase = Phase::Rewind; phaseT = 0.0f; }
                    break;
                case Phase::Rewind:
                    if (phaseT >= kRewindSecs) { phase = Phase::TitleCard; phaseT = 0.0f; }
                    break;
                case Phase::TitleCard:
                    if (phaseT >= kTitleSecs) { phase = Phase::Replay; phaseT = 0.0f; }
                    break;
                case Phase::Replay:
                    if (phaseT >= kReplaySecs) { phase = Phase::Respawn; phaseT = 0.0f; respawned = false; }
                    break;
                case Phase::Respawn:
                    if (!respawned && phaseT >= kFadeSecs) {   // re-seed at black
                        const x3::game::FlightMode keep = pilot.mode();
                        pilot.spawn(*sphys, 0.0f, 0.0f, 0.0f);
                        pilot.setMode(keep);
                        trajHead = trajCount = 0; trajTimer = 0.0f;
                        respawned = true;
                    }
                    if (phaseT >= 2.0f * kFadeSecs + 0.8f) { phase = Phase::Flying; phaseT = 0.0f; g_heat = 0.0f; }
                    break;
                default: break;
            }
            return true;
        };

        // ---- Pause menu overlay (ESC). RESUME / FLIGHT MODE / QUIT. Same HUD
        //      text mechanism; keyboard-navigated (sel = highlighted row). ------
        auto drawPauseMenu = [&](const x3::rhi::FrameContext& frame, float W, float H, int sel) {
            using x3::rhi::FontRole;
            const float ov[4] = { 0.0f, 0.0f, 0.0f, 0.55f };
            device->drawHudQuad(frame, 0, 0, W, H, ov);
            const float pw = 440.0f, ph = 250.0f;
            const float px = W * 0.5f - pw * 0.5f, py = H * 0.5f - ph * 0.5f;
            const float pbg[4] = { 0.03f, 0.05f, 0.08f, 0.92f };
            device->drawHudQuad(frame, px, py, pw, ph, pbg);
            const float acc[4] = { 0.30f, 0.95f, 1.0f, 1.0f };
            device->drawHudQuad(frame, px, py, pw, 4.0f, acc);
            const char* title = "PAUSED";
            const float tw = device->textAdvance(FontRole::Title, title, 30.0f);
            device->drawHudTextF(frame, FontRole::Title, title, px + pw*0.5f - tw*0.5f, py + 18.0f, 30.0f, acc);
            char midItem[64];
            std::snprintf(midItem, sizeof(midItem), "FLIGHT MODE:  %s", x3::game::flightModeName(pilot.mode()));
            const char* items[3] = { "RESUME", midItem, "QUIT TO DESKTOP" };
            float iy = py + 78.0f;
            for (int i = 0; i < 3; ++i) {
                const bool on = (i == sel);
                if (on) {
                    const float selbg[4] = { acc[0]*0.25f, acc[1]*0.25f, acc[2]*0.30f, 0.55f };
                    device->drawHudQuad(frame, px + 20.0f, iy - 5.0f, pw - 40.0f, 30.0f, selbg);
                }
                float col[4];
                if (on) { col[0]=1.0f; col[1]=0.90f; col[2]=0.40f; col[3]=1.0f; }        // amber highlight
                else    { col[0]=0.60f; col[1]=0.80f; col[2]=0.95f; col[3]=0.85f; }
                device->drawHudTextF(frame, FontRole::Menu, items[i], px + 34.0f, iy, 22.0f, col);
                iy += 46.0f;
            }
            const float hint[4] = { 0.5f, 0.6f, 0.7f, 0.8f };
            device->drawHudTextF(frame, FontRole::HudMono, "UP/DOWN + ENTER   ESC=RESUME",
                                 px + 34.0f, py + ph - 26.0f, 12.0f, hint);
        };

        // ===== Headless capture (--world space --screenshot <path>) ========
        if (headless) {
            // The frustum-cull pass on this baseline tests AABBs that may be
            // wrong for a deeply-nested GLB drawable transform. Disable for the
            // capture so the test screenshot is robust against that; it is a
            // VISUAL gate, not a perf gate. (Windowed path leaves it default-on.)
            device->setFrustumCullEnabled(false);
            // Camera behind the player ship looking toward +X (yaw=0 -> +X is
            // the device's "forward 0" per Player::camera()), slight downward
            // pitch to catch the slight-Y staggered decor ships. The fleet
            // cluster sits at x=60..200 with +/-Z flanks within FOV.
            float cam[5] = { -25.0f, 6.0f, 0.0f, 0.0f, -0.05f };
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            const std::string outPath = screenshot ? screenshotPath : std::string("G:/X3Native/captures/space.png");
            // Heat telemetry for the HUD (the sequence NEVER runs in headless — the
            // spawn is 48 km off the surface, so heat is ~0 and no death triggers).
            {
                const x3::phys::Vec3 sp = pilot.pos();
                g_sunSurf = vlen(x3::phys::Vec3{ sp.x-kSunCenter.x, sp.y-kSunCenter.y, sp.z-kSunCenter.z }) - kSunRadius;
                const float prox = smooth01(kHeatStart, kSunRadius, g_sunSurf); g_heat = prox*prox;
            }
            // Settle: a few frames so the lights register + the meshes upload.
            const int kFrames = 16;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                sphys->step(dt);
                combatFx.update(dt);
                updateDynamicLights(pilot.pos(), pilot.forward(), pilot.up(), pilot.right());
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 65.0f);
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    drawScene(frame);
                    combatFx.submit(*device, frame);
                    drawSpeedFx(frame);
                    drawSun(frame);
                    drawShipLights(frame, 0.15f, 1.0f);
                    uint32_t hw = 0, hh = 0; device->hudSize(hw, hh);
                    drawHud(frame, (float)hw, (float)hh);
                }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world space: wrote " + outPath);
            else       x3::logError("--world space: capture FAILED");
            combatFx.shutdown(*device);
            device->destroyMesh(dustMesh); device->destroyMesh(sunMesh);
            device->destroyMesh(shipBoxMesh); device->destroyTexture(shipBoxTex);
            if (shipModel.ok) mloader->unload(shipModel);
            sphys->shutdown(); device->shutdown();
            if (window) glfwDestroyWindow(window); glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: 6DOF pilot, mouse + WASD + Q/E + V ==
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        bool prevV = false, prevLmb = false;

        // ---- Flight AUDIO (reuses the drive-host loop-voice pattern) --------
        // Private audio system for this standalone host (graceful-silent with no
        // device). Engine HUM = a looping voice whose vol+pitch track speed; BOOST
        // = a second pitched loop gated on Shift; MODE BLIP = a one-shot chime at a
        // per-mode pitch. Presentation-only — never touches the sim hash.
        std::unique_ptr<x3::audio::IAudioSystem> saudio(x3::audio::createAudioSystem());
        saudio->init();
        x3::audio::SoundHandle humSnd  = saudio->load(x3::game::resolveAudio("vehicles/engine_loop.wav"));
        x3::audio::SoundHandle blipSnd = saudio->load(x3::game::resolveAudio("interact/chime.wav"));
        x3::audio::LoopHandle  humLoop{};
        x3::audio::LoopHandle  boostLoop{};
        x3::audio::LoopHandle  warnLoop{};   // CRITICAL hull-temp / shield warning beep
        if (humSnd.valid()) humLoop = saudio->startLoop(humSnd, 0.12f, 0.6f);
        x3::logInfo(std::string("--world space: engine audio ") +
                    (humSnd.valid() ? "ON" : "absent (silent)"));

        // ---- PAUSE MENU state (ESC opens it; it NO LONGER exits) -----------
        bool paused = false;
        int  menuSel = 0;
        bool prevEsc = false, prevUp = false, prevDown = false, prevEnter = false;
        bool prevAnyKey = false;   // rising-edge latch so a held key can't insta-skip

        x3::logInfo("--world space: WASD thrust, mouse look, Q/E roll, Space/Ctrl up/down, Shift boost, V camera, LMB laser, 1/2/3 mode, Esc=pause menu");
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            double now = glfwGetTime(); float fdt = (float)(now - prevTime); prevTime = now;
            if (fdt > 0.1f) fdt = 0.1f;
            g_clock += fdt;                       // presentation clock (blink/pulse/flash)
            double mx, my; glfwGetCursorPos(window, &mx, &my);
            auto kd = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };

            // Is the sun-death cinematic running? (pilot input + pause are suppressed.)
            const bool seq = (phase != Phase::Flying);
            // "Any key" skips the cinematic straight to Respawn.
            const bool anyKey = seq && (kd(GLFW_KEY_SPACE) || kd(GLFW_KEY_ENTER) ||
                kd(GLFW_KEY_ESCAPE) || kd(GLFW_KEY_W) || kd(GLFW_KEY_A) || kd(GLFW_KEY_S) ||
                kd(GLFW_KEY_D) || kd(GLFW_KEY_Q) || kd(GLFW_KEY_E) || kd(GLFW_KEY_LEFT_SHIFT) ||
                glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);

            // ESC toggles the pause menu (rising edge) ONLY while flying — during the
            // death cinematic ESC is a skip, not a pause. Resync the mouse anchor so
            // resume doesn't jump the view.
            const bool escNow = kd(GLFW_KEY_ESCAPE);
            if (!seq && escNow && !prevEsc) {
                paused = !paused;
                glfwSetInputMode(window, GLFW_CURSOR, paused ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
                lastMX = mx; lastMY = my;
            }
            prevEsc = escNow;

            if (seq) {
                // Death cinematic: freeze the pilot, advance the phase machine (which
                // handles the shield drain, blast, rewind, title card, replay, respawn).
                // Skip is EDGE-triggered so a key still held from flying-in can't insta-skip.
                boostActive = false; lastMX = mx; lastMY = my;
                advanceSequence(fdt, anyKey && !prevAnyKey);
            } else if (!paused) {
                float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
                lastMX = mx; lastMY = my;

                // Build a PlayerInput for the controller. jumpPressed re-purposed as
                // "up impulse this frame" while Space is held; sprint = boost.
                x3::game::PlayerInput in{};
                in.moveFwd    = (kd(GLFW_KEY_W) ?  1.0f : 0.0f) + (kd(GLFW_KEY_S) ? -1.0f : 0.0f);
                in.moveStrafe = (kd(GLFW_KEY_D) ?  1.0f : 0.0f) + (kd(GLFW_KEY_A) ? -1.0f : 0.0f);
                in.sprint     = kd(GLFW_KEY_LEFT_SHIFT);
                in.jumpPressed= kd(GLFW_KEY_SPACE);   // held = up impulse
                in.lookDX     = ddx;
                in.lookDY     = ddy;
                boostActive   = in.sprint;

                // Q/E roll axis: +1 for Q, -1 for E (or the other way; either is fine).
                float rollAxis = (kd(GLFW_KEY_Q) ? -1.0f : 0.0f) + (kd(GLFW_KEY_E) ? 1.0f : 0.0f);
                pilot.setRollInput(rollAxis);

                // FLIGHT MODE hot-swap while flying: 1=Arcade, 2=Assist, 3=Loose (the
                // in-space equivalent of the `flightmode` console command). Writes the
                // shared latch; the change is applied + blipped below (common path).
                if (kd(GLFW_KEY_1)) x3::game::setRequestedFlightMode(x3::game::FlightMode::Arcade);
                if (kd(GLFW_KEY_2)) x3::game::setRequestedFlightMode(x3::game::FlightMode::Assist);
                if (kd(GLFW_KEY_3)) x3::game::setRequestedFlightMode(x3::game::FlightMode::Loose);

                pilot.update(in, fdt, *sphys);

                // V to toggle 1P / 3P (rising edge).
                bool vNow = kd(GLFW_KEY_V);
                if (vNow && !prevV) pilot.toggleCameraMode();
                prevV = vNow;

                // LMB laser (rising edge -> fire one bolt, log on success).
                bool lmbNow = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                if (lmbNow && !prevLmb && pilot.fireLaser(fdt)) {
                    const x3::phys::Vec3 pos = pilot.pos();
                    const x3::phys::Vec3 fwd = pilot.forward();
                    x3::phys::Vec3 muzzle{ pos.x + fwd.x * 2.5f,
                                           pos.y + fwd.y * 2.5f,
                                           pos.z + fwd.z * 2.5f };
                    x3::phys::Vec3 hit{ pos.x + fwd.x * 400.0f,
                                        pos.y + fwd.y * 400.0f,
                                        pos.z + fwd.z * 400.0f };
                    combatFx.addTracer(muzzle, hit);
                }
                prevLmb = lmbNow;

                sphys->step(fdt);
                combatFx.update(fdt);
                updateDust(fdt);
                // Record the approach + detect crossing INTO the sun body (which flips
                // the phase machine to InsideSun and engages the shield). Returns false
                // while flying; computes g_heat / g_sunSurf for the HUD + heat light.
                advanceSequence(fdt, false);
            } else {
                // Paused: keep the mouse anchor synced (no view drift), no flight
                // sim / physics step (visuals freeze behind the menu). Menu nav:
                lastMX = mx; lastMY = my;
                boostActive = false;
                const bool upNow = kd(GLFW_KEY_UP) || kd(GLFW_KEY_W);
                const bool dnNow = kd(GLFW_KEY_DOWN) || kd(GLFW_KEY_S);
                const bool enNow = kd(GLFW_KEY_ENTER) || kd(GLFW_KEY_SPACE);
                if (upNow && !prevUp)   menuSel = (menuSel + 2) % 3;
                if (dnNow && !prevDown) menuSel = (menuSel + 1) % 3;
                if (enNow && !prevEnter) {
                    if (menuSel == 0) {                 // RESUME
                        paused = false;
                        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                        lastMX = mx; lastMY = my;
                    } else if (menuSel == 1) {          // FLIGHT MODE: cycle (same setMode path)
                        int nxt = ((int)pilot.mode() + 1) % 3;
                        x3::game::setRequestedFlightMode((x3::game::FlightMode)nxt);
                    } else {                            // QUIT TO DESKTOP
                        glfwSetWindowShouldClose(window, GLFW_TRUE);
                    }
                }
                prevUp = upNow; prevDown = dnNow; prevEnter = enNow;
            }
            prevAnyKey = anyKey;

            // ---- Common: apply a mode change from EITHER source (1/2/3 keys or the
            //      menu cycle) and BLIP a per-mode chime so the switch is audible. --
            if (x3::game::requestedFlightMode() != pilot.mode()) {
                pilot.setMode(x3::game::requestedFlightMode());
                x3::logInfo(std::string("--world space: flight mode -> ") +
                            x3::game::flightModeName(pilot.mode()));
                if (blipSnd.valid()) {
                    const float bp = (pilot.mode() == x3::game::FlightMode::Assist) ? 0.7f
                                   : (pilot.mode() == x3::game::FlightMode::Loose)  ? 1.4f : 1.0f;
                    saudio->playSound2D(blipSnd, 0.7f, bp);
                }
            }

            // ---- Flight audio: hum vol+pitch track speed; boost layer gated on Shift.
            {
                const float spd  = pilot.speed();
                const float maxS = std::max(1.0f, pilot.tuning().maxSpeed);
                const float sf   = std::min(1.0f, spd / maxS);
                if (humLoop.valid()) {
                    const float hv = paused ? 0.05f : (0.12f + 0.30f * sf);
                    const float hp = 0.60f + 0.85f * sf;
                    saudio->setLoopParams(humLoop, hv, hp);
                }
                if (boostActive && humSnd.valid()) {
                    if (!boostLoop.valid()) boostLoop = saudio->startLoop(humSnd, 0.0f, 1.6f);
                    if (boostLoop.valid())  saudio->setLoopParams(boostLoop, 0.22f + 0.18f * sf, 1.6f + 0.6f * sf);
                } else if (boostLoop.valid()) {
                    saudio->stopLoop(boostLoop); boostLoop = {};
                }
                // CRITICAL warning beep: a fast high chime loop when hull-temp is
                // critical on approach, or while the shield drains inside the star.
                const bool wantWarn = (phase == Phase::Flying && g_sunSurf < kCritDist) ||
                                       phase == Phase::InsideSun;
                if (wantWarn && blipSnd.valid()) {
                    if (!warnLoop.valid()) warnLoop = saudio->startLoop(blipSnd, 0.35f, 1.9f);
                } else if (warnLoop.valid()) {
                    saudio->stopLoop(warnLoop); warnLoop = {};
                }
            }

            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw>0 && chh>0) device->onResize((uint32_t)cw, (uint32_t)chh);

            // Re-read the kill-cam flag from the LIVE phase (advanceSequence may have
            // transitioned this frame) so the camera never lags a frame at a boundary.
            const bool cineNow = (phase == Phase::Detonation || phase == Phase::Rewind ||
                                  phase == Phase::TitleCard  || phase == Phase::Replay);
            // Camera: the EXTERNAL kill-cam during the blast/rewind/card/replay, else
            // the pilot camera (FOV widens with speed + boost per mode).
            float cx, cy, cz, cyaw, cpit;
            if (cineNow) {
                cx = cineCamPos.x; cy = cineCamPos.y; cz = cineCamPos.z; cyaw = cineYaw; cpit = cinePit;
                device->setCamera(cx, cy, cz, cyaw, cpit, 60.0f);
            } else {
                pilot.camera(cx, cy, cz, cyaw, cpit);
                device->setCamera(cx, cy, cz, cyaw, cpit, pilot.fov());
            }
            saudio->setListener(cx, cy, cz, cyaw, cpit);
            saudio->update(fdt);

            // Player-key + sun-heat follow lights (refreshed each frame).
            updateDynamicLights(pilot.pos(), pilot.forward(), pilot.up(), pilot.right());

            auto frame = device->beginFrame();
            if (frame.valid) {
                // World: the near-field scene only when NOT on the external kill-cam.
                if (!cineNow) {
                    drawScene(frame);
                    combatFx.submit(*device, frame);
                    drawSpeedFx(frame);
                }
                drawSun(frame);                       // the star renders in every phase
                // Phase-specific world elements.
                if (phase == Phase::Flying) {
                    const float thrust01 = std::min(1.0f,
                        std::min(1.0f, pilot.speed()/std::max(1.0f, pilot.tuning().maxSpeed))
                        + (boostActive ? 0.45f : 0.0f));
                    drawShipLights(frame, thrust01, g_clock);
                } else if (phase == Phase::InsideSun) {
                    drawShield(frame, shieldPct, g_clock);
                    drawShipLights(frame, 0.2f, g_clock);
                } else if (phase == Phase::Detonation) {
                    drawEjecta(frame, phaseT);
                } else if (phase == Phase::Rewind) {
                    // Blast retracts (reverse) while the ship sits at the entry point.
                    drawEjecta(frame, kDetonateSecs * (1.0f - phaseT / kRewindSecs));
                    drawReplayShip(frame, 1.0f);
                } else if (phase == Phase::Replay) {
                    drawReplayShip(frame, phaseT / kReplaySecs);   // fly the approach in
                }
                drawHud(frame, (float)cw, (float)chh);
                if (paused) drawPauseMenu(frame, (float)cw, (float)chh, menuSel);
                drawCinematic(frame, (float)cw, (float)chh);       // no-op while Flying
            }
            device->endFrame(frame);
        }
        if (warnLoop.valid())  saudio->stopLoop(warnLoop);
        if (boostLoop.valid()) saudio->stopLoop(boostLoop);
        if (humLoop.valid())   saudio->stopLoop(humLoop);
        saudio->shutdown();
        combatFx.shutdown(*device);
        device->destroyMesh(dustMesh); device->destroyMesh(sunMesh);
        device->destroyMesh(shipBoxMesh); device->destroyTexture(shipBoxTex);
        if (shipModel.ok) mloader->unload(shipModel);
        sphys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
        return 0;
    }
    return -1;
}

}} // namespace x3::apphost
