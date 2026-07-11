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
        { x3::rhi::PointLight pl[3];
          // Key light: a "sun" anchored near the fleet so attenuation is gentle.
          pl[0].pos[0] =  120.0f; pl[0].pos[1] = 120.0f; pl[0].pos[2] = 120.0f;
          pl[0].range  =  600.0f;
          pl[0].color[0] = 130.0f; pl[0].color[1] = 121.0f; pl[0].color[2] = 104.0f;
          // Fill light from -X/+Y to bring out the camera-facing side.
          pl[1].pos[0] = -80.0f; pl[1].pos[1] =  60.0f; pl[1].pos[2] =  20.0f;
          pl[1].range  = 400.0f;
          pl[1].color[0] = 40.0f; pl[1].color[1] = 46.0f; pl[1].color[2] = 60.0f;
          // Rim/back light from +X/-Y to give the ships shape.
          pl[2].pos[0] =  200.0f; pl[2].pos[1] = -30.0f; pl[2].pos[2] = -50.0f;
          pl[2].range  = 500.0f;
          pl[2].color[0] = 24.0f; pl[2].color[1] = 19.0f; pl[2].color[2] = 13.0f;
          device->setPointLights(pl, 3); }

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
        // SkyStars (the static dome) can't stretch its baked stars, and this host
        // draws the analytic-sky starfield anyway — so the "whoosh" + spatial-
        // reference cue is a dedicated NEAR-CAMERA speck field: kDust points in a
        // cube around the ship. Each frame they drift OPPOSITE the velocity vector
        // and wrap when they leave the box (recycled), and they render as emissive
        // boxes that are tiny dots at rest and ELONGATED cyan streaks along the
        // velocity axis at speed/boost. Pure render (no sim state) -> determinism
        // untouched. Uses a shared unit box mesh stretched per speck.
        x3::prims::PrimMesh ubm = x3::prims::makeBox(0.5f, 0.5f, 0.5f, 0, 0, 0, 1.0f);
        auto dustMesh = device->createMesh(ubm.verts.data(), (uint32_t)ubm.verts.size(),
                                           ubm.index.data(), (uint32_t)ubm.index.size());
        const int   kDust = 300;
        const float kDustR = 65.0f;     // wrap half-box (m) centered on the ship
        std::vector<x3::phys::Vec3> dust((size_t)kDust);
        for (int i = 0; i < kDust; ++i) {
            dust[(size_t)i] = x3::phys::Vec3{
                (hashF((uint32_t)(i*3+1))*2.0f - 1.0f) * kDustR,
                (hashF((uint32_t)(i*3+2))*2.0f - 1.0f) * kDustR,
                (hashF((uint32_t)(i*3+3))*2.0f - 1.0f) * kDustR };
        }
        bool boostActive = false;   // Shift-boost this frame (drives HUD + streak punch)

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
            const x3::phys::Vec3 vel = pilot.velocity();
            const float spd  = pilot.speed();
            const float maxS = std::max(1.0f, pilot.tuning().maxSpeed);
            const float sf   = std::min(1.0f, spd / maxS);          // 0..1 speed fraction
            const float punch = boostActive ? 1.5f : 1.0f;
            x3::phys::Vec3 vd{ 1, 0, 0 };
            if (spd > 0.3f) vd = x3::phys::Vec3{ vel.x/spd, vel.y/spd, vel.z/spd };
            const float streak = (0.28f + sf*sf * 12.0f) * punch;   // dot at rest -> long streak
            const float thick  = 0.05f + 0.05f * sf;
            const x3::phys::Vec3 ref = (std::fabs(vd.y) < 0.95f)
                                     ? x3::phys::Vec3{ 0, 1, 0 } : x3::phys::Vec3{ 1, 0, 0 };
            const x3::phys::Vec3 u = vnorm(vcross(ref, vd));
            const x3::phys::Vec3 v = vcross(vd, u);
            const float base[4] = { 0.55f, 0.85f, 1.0f, 1.0f };     // cool cyan
            for (int i = 0; i < kDust; ++i) {
                const x3::phys::Vec3& p = dust[(size_t)i];
                const float tw = 0.5f + 0.5f * hashF((uint32_t)(i*7 + 5));
                const float strength = (0.30f + 2.4f * sf) * tw * punch;
                float m[16];
                composeBasis(m, u, v, vd, thick, thick, streak, p);
                const float emis[4] = { base[0], base[1], base[2], strength };
                device->drawMeshEmissive(frame, dustMesh, x3::rhi::TextureHandle{}, base, emis, m);
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
            const float tagPx = 14.0f;
            const float tgw = device->textAdvance(FontRole::HudMono, tag, tagPx);
            const float dim[4] = { acc[0]*0.85f, acc[1]*0.85f, acc[2]*0.85f, 0.85f };
            device->drawHudTextF(frame, FontRole::HudMono, tag,
                                 W * 0.5f - tgw * 0.5f, ty + titlePx + 2.0f, tagPx, dim);

            // Telemetry corner (bottom-left): speed, heading, boost, position + bar.
            const float spd = pilot.speed();
            const x3::phys::Vec3 pp = pilot.pos();
            const float hdg = pilot.yaw() * 57.29578f;
            char l1[64], l2[64], l3[64];
            std::snprintf(l1, sizeof(l1), "SPD %6.1f m/s", spd);
            std::snprintf(l2, sizeof(l2), "HDG %+04.0f   BOOST %s", (double)hdg, boostActive ? "ON " : "off");
            std::snprintf(l3, sizeof(l3), "POS %5.0f %5.0f %5.0f", (double)pp.x, (double)pp.y, (double)pp.z);
            const float tpx = 16.0f;
            const float bx = 20.0f, by = H - 88.0f;
            const float tbg[4] = { 0.02f, 0.03f, 0.05f, 0.5f };
            device->drawHudQuad(frame, bx - 10.0f, by - 8.0f, 310.0f, 80.0f, tbg);
            const float cyan[4] = { 0.60f, 0.90f, 1.0f, 0.95f };
            device->drawHudText(frame, l1, bx, by,          tpx, cyan);
            device->drawHudText(frame, l2, bx, by + 20.0f,  tpx, cyan);
            device->drawHudText(frame, l3, bx, by + 40.0f,  tpx, cyan);
            const float barW = 290.0f, barH = 6.0f, barY = by + 64.0f;
            const float bbg[4] = { 0.10f, 0.15f, 0.20f, 0.7f };
            device->drawHudQuad(frame, bx, barY, barW, barH, bbg);
            const float frac = std::min(1.0f, spd / std::max(1.0f, pilot.tuning().maxSpeed));
            const float fill[4] = { acc[0], acc[1], acc[2], 0.95f };
            device->drawHudQuad(frame, bx, barY, barW * frac, barH, fill);
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
            // Settle: a few frames so the lights register + the meshes upload.
            const int kFrames = 16;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                sphys->step(dt);
                combatFx.update(dt);
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 65.0f);
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    drawScene(frame);
                    combatFx.submit(*device, frame);
                    drawSpeedFx(frame);
                    uint32_t hw = 0, hh = 0; device->hudSize(hw, hh);
                    drawHud(frame, (float)hw, (float)hh);
                }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world space: wrote " + outPath);
            else       x3::logError("--world space: capture FAILED");
            combatFx.shutdown(*device);
            device->destroyMesh(dustMesh);
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
        if (humSnd.valid()) humLoop = saudio->startLoop(humSnd, 0.12f, 0.6f);
        x3::logInfo(std::string("--world space: engine audio ") +
                    (humSnd.valid() ? "ON" : "absent (silent)"));

        // ---- PAUSE MENU state (ESC opens it; it NO LONGER exits) -----------
        bool paused = false;
        int  menuSel = 0;
        bool prevEsc = false, prevUp = false, prevDown = false, prevEnter = false;

        x3::logInfo("--world space: WASD thrust, mouse look, Q/E roll, Space/Ctrl up/down, Shift boost, V camera, LMB laser, 1/2/3 mode, Esc=pause menu");
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            double now = glfwGetTime(); float fdt = (float)(now - prevTime); prevTime = now;
            if (fdt > 0.1f) fdt = 0.1f;
            double mx, my; glfwGetCursorPos(window, &mx, &my);
            auto kd = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };

            // ESC toggles the pause menu (rising edge). It no longer breaks the
            // loop — only the menu's QUIT item closes the window. Release/capture
            // the cursor + resync the mouse anchor so resume doesn't jump the view.
            const bool escNow = kd(GLFW_KEY_ESCAPE);
            if (escNow && !prevEsc) {
                paused = !paused;
                glfwSetInputMode(window, GLFW_CURSOR, paused ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
                lastMX = mx; lastMY = my;
            }
            prevEsc = escNow;

            if (!paused) {
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
            }

            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw>0 && chh>0) device->onResize((uint32_t)cw, (uint32_t)chh);

            float cx, cy, cz, cyaw, cpit;
            pilot.camera(cx, cy, cz, cyaw, cpit);
            // FOV-BY-SPEED: the controller widens FOV with speed + boost per mode.
            device->setCamera(cx, cy, cz, cyaw, cpit, pilot.fov());
            saudio->setListener(cx, cy, cz, cyaw, cpit);
            saudio->update(fdt);

            auto frame = device->beginFrame();
            if (frame.valid) {
                drawScene(frame);
                combatFx.submit(*device, frame);
                drawSpeedFx(frame);
                drawHud(frame, (float)cw, (float)chh);
                if (paused) drawPauseMenu(frame, (float)cw, (float)chh, menuSel);
            }
            device->endFrame(frame);
        }
        if (boostLoop.valid()) saudio->stopLoop(boostLoop);
        if (humLoop.valid())   saudio->stopLoop(humLoop);
        saudio->shutdown();
        combatFx.shutdown(*device);
        device->destroyMesh(dustMesh);
        device->destroyMesh(shipBoxMesh); device->destroyTexture(shipBoxTex);
        if (shipModel.ok) mloader->unload(shipModel);
        sphys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
        return 0;
    }
    return -1;
}

}} // namespace x3::apphost
