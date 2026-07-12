// ATTACHMENT PARTS — see attach_view.h.
#include "attach_view.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace x3::game {

namespace {

constexpr float kPi = 3.14159265358979f;

// A capped cylinder along +Z, from z0 to z1, radius r. (mesh_prims has no cylinder;
// this is the one shape every barrel/optic part is built from.)
x3::prims::PrimMesh makeCyl(float r, float z0, float z1, uint32_t seg = 20,
                            float cx = 0.0f, float cy = 0.0f) {
    x3::prims::PrimMesh m;
    auto V = [&](float x, float y, float z, float nx, float ny, float nz, float u, float v) {
        m.verts.push_back({ { cx + x, cy + y, z }, { nx, ny, nz }, { u, v } });
    };
    for (uint32_t i = 0; i < seg; ++i) {
        const float a0 = (float)i       / (float)seg * 2.0f * kPi;
        const float a1 = (float)(i + 1) / (float)seg * 2.0f * kPi;
        const float c0 = std::cos(a0), s0 = std::sin(a0);
        const float c1 = std::cos(a1), s1 = std::sin(a1);
        const uint32_t b = (uint32_t)m.verts.size();
        V(c0 * r, s0 * r, z0, c0, s0, 0, (float)i / seg, 0);
        V(c1 * r, s1 * r, z0, c1, s1, 0, (float)(i + 1) / seg, 0);
        V(c1 * r, s1 * r, z1, c1, s1, 0, (float)(i + 1) / seg, 1);
        V(c0 * r, s0 * r, z1, c0, s0, 0, (float)i / seg, 1);
        m.index.insert(m.index.end(), { b, b + 1, b + 2, b, b + 2, b + 3 });
        // caps
        const uint32_t cA = (uint32_t)m.verts.size();
        V(0, 0, z1, 0, 0, 1, 0.5f, 0.5f);
        V(c0 * r, s0 * r, z1, 0, 0, 1, 0, 0);
        V(c1 * r, s1 * r, z1, 0, 0, 1, 1, 0);
        m.index.insert(m.index.end(), { cA, cA + 1, cA + 2 });
        const uint32_t cB = (uint32_t)m.verts.size();
        V(0, 0, z0, 0, 0, -1, 0.5f, 0.5f);
        V(c1 * r, s1 * r, z0, 0, 0, -1, 1, 0);
        V(c0 * r, s0 * r, z0, 0, 0, -1, 0, 0);
        m.index.insert(m.index.end(), { cB, cB + 1, cB + 2 });
    }
    return m;
}

void append(x3::prims::PrimMesh& dst, const x3::prims::PrimMesh& src) {
    const uint32_t base = (uint32_t)dst.verts.size();
    dst.verts.insert(dst.verts.end(), src.verts.begin(), src.verts.end());
    for (uint32_t i : src.index) dst.index.push_back(base + i);
}

// column-major 4x4 from a basis + origin (the viewmodel frame, scaled).
void composeBasis(float out[16], const x3::phys::Vec3& bx, const x3::phys::Vec3& by,
                  const x3::phys::Vec3& bz, const x3::phys::Vec3& p, float s) {
    out[0] = bx.x * s; out[1] = bx.y * s; out[2] = bx.z * s; out[3] = 0.0f;
    out[4] = by.x * s; out[5] = by.y * s; out[6] = by.z * s; out[7] = 0.0f;
    out[8] = bz.x * s; out[9] = bz.y * s; out[10] = bz.z * s; out[11] = 0.0f;
    out[12] = p.x;     out[13] = p.y;     out[14] = p.z;      out[15] = 1.0f;
}

} // namespace

void AttachView::init(x3::rhi::IRenderDevice& device) {
    if (m_built) return;

    auto mesh = [&](const x3::prims::PrimMesh& p) {
        x3::rhi::MeshHandle h = device.createMesh(p.verts.data(), (uint32_t)p.verts.size(),
                                                  p.index.data(), (uint32_t)p.index.size());
        m_meshes.push_back(h);
        return h;
    };
    // 1x1 metallic-roughness texel (glTF packing: G = roughness, B = metallic).
    // LANDMINE L5: a GLB with no MR map defaults to metallic 1.0 and renders black
    // indoors. These parts ship a REAL MR texel so they shade honestly.
    // LANDMINE L5 (KNOWN_BUGS): FULL METAL HAS NO DIFFUSE LOBE, so a metallic part in a
    // windowless interior renders BLACK. The first capture proved it — the parts were on
    // the gun and invisible. Metalness is clamped to the documented 0.35 ceiling here, at
    // the one place the shader actually reads it, so no item can reintroduce the bug.
    auto mrTex = [&](float metal, float rough) {
        if (metal > 0.35f) metal = 0.35f;
        const uint8_t px[4] = { 255,
                                (uint8_t)(rough * 255.0f + 0.5f),
                                (uint8_t)(metal * 255.0f + 0.5f), 255 };
        x3::rhi::TextureHandle t = device.createTexture(px, 1, 1, /*srgb*/false);
        m_textures.push_back(t);
        return t;
    };

    // A 1x1 WHITE albedo texel. The PBR draw route needs a REAL baseColor texture: with an
    // invalid handle the parts sampled garbage and shaded orange-red. baseColorFactor then
    // carries the honest albedo.
    {
        const uint8_t w[4] = { 255, 255, 255, 255 };
        m_white = device.createTexture(w, 1, 1, /*srgb*/true);
        m_textures.push_back(m_white);
    }

    // All part geometry is authored in a UNIT frame: +Z down-barrel, +Y up, sized
    // ~1.0 across, then scaled to the weapon's own barrel run at draw time.
    // ---- SUPPRESSOR: a long fat can with a knurled collar. ----
    {
        x3::prims::PrimMesh p = makeCyl(0.062f, -0.02f, 0.40f, 22);
        append(p, makeCyl(0.072f, -0.03f, 0.03f, 22));            // mount collar
        append(p, makeCyl(0.070f, 0.30f, 0.325f, 22));            // end band
        m_parts[(int)AttachPart::Suppressor].mesh = mesh(p);
        m_parts[(int)AttachPart::Suppressor].mr   = mrTex(0.35f, 0.50f);
    }
    // ---- COMPENSATOR: a short ported brake — stubby, with side vents. ----
    {
        x3::prims::PrimMesh p = makeCyl(0.055f, -0.01f, 0.13f, 20);
        // three vent fins standing proud on top and both sides
        append(p, x3::prims::makeBox(0.012f, 0.030f, 0.055f, 0.0f,  0.055f, 0.055f));
        append(p, x3::prims::makeBox(0.030f, 0.012f, 0.055f, 0.055f, 0.0f,  0.055f));
        append(p, x3::prims::makeBox(0.030f, 0.012f, 0.055f, -0.055f, 0.0f, 0.055f));
        m_parts[(int)AttachPart::Compensator].mesh = mesh(p);
        m_parts[(int)AttachPart::Compensator].mr   = mrTex(0.35f, 0.40f);
    }
    // ---- HEAVY BARREL: a thick fluted extension. ----
    {
        x3::prims::PrimMesh p = makeCyl(0.070f, -0.06f, 0.30f, 22);
        append(p, makeCyl(0.085f, -0.06f, -0.01f, 22));           // chunky shoulder
        append(p, makeCyl(0.052f, 0.30f, 0.34f, 18));             // crown
        m_parts[(int)AttachPart::HeavyBarrel].mesh = mesh(p);
        m_parts[(int)AttachPart::HeavyBarrel].mr   = mrTex(0.35f, 0.38f);
    }
    // ---- EXTENDED MAG: a deep box that hangs below the receiver. ----
    {
        x3::prims::PrimMesh p = x3::prims::makeBox(0.050f, 0.120f, 0.075f, 0.0f, -0.09f, 0.0f);
        append(p, x3::prims::makeBox(0.058f, 0.048f, 0.082f, 0.0f, 0.030f, 0.0f));   // well lip (INTO the frame)
        append(p, x3::prims::makeBox(0.053f, 0.014f, 0.078f, 0.0f, -0.205f, 0.0f));  // base plate
        m_parts[(int)AttachPart::ExtMag].mesh = mesh(p);
        m_parts[(int)AttachPart::ExtMag].mr   = mrTex(0.25f, 0.70f);
    }
    // ---- FAST MAG: shallow, tapered, with a pull tab. ----
    {
        x3::prims::PrimMesh p = x3::prims::makeBox(0.046f, 0.062f, 0.070f, 0.0f, -0.05f, 0.0f);
        append(p, x3::prims::makeBox(0.056f, 0.046f, 0.078f, 0.0f, 0.026f, 0.0f));   // lip (INTO the frame)
        append(p, x3::prims::makeBox(0.030f, 0.010f, 0.040f, 0.0f, -0.118f, -0.06f));// pull tab
        m_parts[(int)AttachPart::FastMag].mesh = mesh(p);
        m_parts[(int)AttachPart::FastMag].mr   = mrTex(0.30f, 0.60f);
    }
    // ---- REFLEX: a low open frame on a REAL MOUNT that reaches down to the rail. ----
    // The mount matters: an optic drawn floating over the receiver reads as a bug, and it
    // IS one. The post runs from the lens frame down PAST the gun's top face so the part
    // is visibly bolted to the weapon, not hovering above it.
    {
        x3::prims::PrimMesh p;
        append(p, x3::prims::makeBox(0.052f, 0.014f, 0.078f, 0.0f, -0.062f, 0.0f));  // rail plate
        append(p, x3::prims::makeBox(0.030f, 0.055f, 0.052f, 0.0f, -0.120f, 0.0f));  // MOUNT POST (down to the gun)
        append(p, x3::prims::makeBox(0.010f, 0.052f, 0.010f, -0.045f, 0.0f, 0.058f));
        append(p, x3::prims::makeBox(0.010f, 0.052f, 0.010f,  0.045f, 0.0f, 0.058f));
        append(p, x3::prims::makeBox(0.054f, 0.012f, 0.060f, 0.0f, 0.050f, 0.012f));  // hood
        append(p, x3::prims::makeBox(0.052f, 0.010f, 0.010f, 0.0f, 0.0f, -0.058f));   // rear ring
        m_parts[(int)AttachPart::Reflex].mesh = mesh(p);
        m_parts[(int)AttachPart::Reflex].mr   = mrTex(0.30f, 0.45f);
    }
    // ---- SCOPE: a real tube with an objective bell, turret and eyepiece. ----
    // Its LOCAL +Z axis is the sight axis; the length is scaled at draw time by the
    // optic's magnification, so the 4x sniper tube is visibly longer than the 2x.
    {
        x3::prims::PrimMesh p = makeCyl(0.036f, -0.16f, 0.16f, 20);        // tube
        append(p, makeCyl(0.052f, 0.14f, 0.22f, 20));                      // objective bell
        append(p, makeCyl(0.046f, -0.22f, -0.14f, 20));                    // eyepiece
        append(p, x3::prims::makeBox(0.022f, 0.024f, 0.022f, 0.0f, 0.048f, 0.0f));  // elevation turret
        append(p, x3::prims::makeBox(0.024f, 0.020f, 0.022f, 0.046f, 0.0f, 0.0f));  // windage turret
        // RINGS + POSTS that reach DOWN to the receiver (the scope is bolted, not floating).
        append(p, x3::prims::makeBox(0.046f, 0.030f, 0.030f, 0.0f, -0.062f,  0.10f)); // front ring
        append(p, x3::prims::makeBox(0.046f, 0.030f, 0.030f, 0.0f, -0.062f, -0.10f)); // rear ring
        append(p, x3::prims::makeBox(0.030f, 0.060f, 0.036f, 0.0f, -0.140f,  0.10f)); // front post
        append(p, x3::prims::makeBox(0.030f, 0.060f, 0.036f, 0.0f, -0.140f, -0.10f)); // rear post
        m_parts[(int)AttachPart::Scope].mesh = mesh(p);
        m_parts[(int)AttachPart::Scope].mr   = mrTex(0.30f, 0.33f);
    }
    // ---- LASER: a small emitter block clamped under the barrel. ----
    {
        x3::prims::PrimMesh p = x3::prims::makeBox(0.030f, 0.028f, 0.070f, 0.0f, 0.0f, 0.0f);
        append(p, x3::prims::makeBox(0.010f, 0.022f, 0.012f, 0.0f, 0.042f, 0.0f));  // clamp
        m_parts[(int)AttachPart::Laser].mesh = mesh(p);
        m_parts[(int)AttachPart::Laser].mr   = mrTex(0.30f, 0.40f);
    }
    // ---- GRIP: an angled fore-grip. ----
    {
        x3::prims::PrimMesh p = x3::prims::makeBox(0.028f, 0.075f, 0.034f, 0.0f, -0.070f, 0.0f);
        append(p, x3::prims::makeBox(0.034f, 0.014f, 0.045f, 0.0f, 0.010f, 0.0f));   // clamp plate
        append(p, x3::prims::makeBox(0.032f, 0.014f, 0.038f, 0.0f, -0.148f, 0.0f));  // flare
        m_parts[(int)AttachPart::Grip].mesh = mesh(p);
        m_parts[(int)AttachPart::Grip].mr   = mrTex(0.10f, 0.85f);
    }
    // ---- CELL: the Coating slot's energy cell — a housing with a lit window. ----
    {
        x3::prims::PrimMesh p = x3::prims::makeBox(0.028f, 0.055f, 0.085f, 0.0f, 0.0f, 0.0f);
        append(p, x3::prims::makeBox(0.010f, 0.020f, 0.020f, -0.032f, 0.0f, 0.052f)); // feed pipe
        m_parts[(int)AttachPart::Cell].mesh = mesh(p);
        m_parts[(int)AttachPart::Cell].mr   = mrTex(0.30f, 0.32f);
    }
    // ---- Shared bits: the optic's dark glass, and the small LIT CORE. ----
    // The lens is DARK GLASS with a real spec — not a white disc, not an emitter.
    // (Near-white albedo is exactly what made the door pink and the console white.)
    m_lensGlass = mesh(makeCyl(0.049f, 0.218f, 0.224f, 22));
    m_mrGlass   = mrTex(0.10f, 0.06f);              // glassy: low metal, very smooth
    // The only emitters in this whole feature: a laser diode and a cell window.
    // Small lit CORES (the polish recipe), never a whole glowing object.
    m_core = mesh(x3::prims::makeBox(0.010f, 0.010f, 0.006f, 0.0f, 0.0f, 0.0f));

    m_built = true;
    x3::logInfo("[attach] parts built (" + std::to_string(m_meshes.size()) + " meshes, " +
                std::to_string(m_textures.size()) + " MR texels)");
}

void AttachView::shutdown(x3::rhi::IRenderDevice& device) {
    for (auto m : m_meshes)   if (m.valid()) device.destroyMesh(m);
    for (auto t : m_textures) if (t.valid()) device.destroyTexture(t);
    m_meshes.clear();
    m_textures.clear();
    for (int i = 0; i < 16; ++i) m_parts[i] = Part{};
    m_lensGlass = {}; m_core = {}; m_mrGlass = {}; m_white = {};
    m_built = false;
}

// ---------------------------------------------------------------------------
// Draw one fitted attachment. `origin` + (bx,by,bz) + `scale` ARE the viewmodel
// frame (Arsenal::currentViewmodelFrame), so a part is on the gun by construction.
// ---------------------------------------------------------------------------
void AttachView::drawSpec(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                          const WeaponDef& def, const AttachSpec& a,
                          const x3::phys::Vec3& bx, const x3::phys::Vec3& by,
                          const x3::phys::Vec3& bz, const x3::phys::Vec3& origin,
                          float scale) const {
    const int pi = (int)a.part;
    if (pi <= 0 || pi >= 16 || !m_parts[pi].mesh.valid()) return;

    // Mount point (GLB scene space) -> world, through the viewmodel frame.
    const float run = attachBarrelRun(def);          // this gun's own length scale
    const x3::phys::Vec3 L = attachMountLocal(def, a);
    auto toWorld = [&](const x3::phys::Vec3& p) {
        return x3::phys::Vec3{ origin.x + (bx.x * p.x + by.x * p.y + bz.x * p.z) * scale,
                               origin.y + (bx.y * p.x + by.y * p.y + bz.y * p.z) * scale,
                               origin.z + (bx.z * p.x + by.z * p.y + bz.z * p.z) * scale };
    };
    // The part's own size follows the weapon's barrel run — a 4.4 m shotgun gets a
    // big can, a 0.9 m pistol a small one, from ONE unit mesh.
    float partScale = run * scale * 1.15f;   // a part must READ at viewmodel scale
    // Scopes: longer tube for more magnification (14 deg FOV = 4x -> a long tube).
    if (a.part == AttachPart::Scope && a.optic.isOptic) {
        const float mag = kBaseFovDeg / (a.optic.adsFovDeg > 1.0f ? a.optic.adsFovDeg : 1.0f);
        partScale *= 0.85f + 0.10f * mag;            // 2x -> ~1.05, 4.3x -> ~1.28
    }

    if (std::getenv("X3_ATTACH_DEBUG")) {
        static int once = 0;
        if (once++ < 24)
            x3::logInfo("[attach-dbg] " + a.id + " slot=" + std::string(attachSlotName(a.slot)) +
                        " mountLocal=(" + std::to_string(L.x) + "," + std::to_string(L.y) + "," +
                        std::to_string(L.z) + ")  muzzle=(" + std::to_string(def.vmMuzzle.x) + "," +
                        std::to_string(def.vmMuzzle.y) + "," + std::to_string(def.vmMuzzle.z) +
                        ")  run=" + std::to_string(run) + "  partScale=" + std::to_string(partScale) + "  albedo=(" + std::to_string(a.albedo[0]) + "," + std::to_string(a.albedo[1]) + "," + std::to_string(a.albedo[2]) + ")  em=(" + std::to_string(a.emissive[0]) + "," + std::to_string(a.emissive[1]) + "," + std::to_string(a.emissive[2]) + ")");
    }
    const x3::phys::Vec3 w = toWorld(L);
    float M[16];
    composeBasis(M, bx, by, bz, w, partScale);

    const float albedo[4] = { a.albedo[0], a.albedo[1], a.albedo[2], 1.0f };
    const float noEm[4] = { 0, 0, 0, 0 };   // parts do NOT self-emit. Only the lit cores below do.
    // THE DRAW PATH: the full PBR route, with a real 1x1 white albedo texel + the part's
    // own MR texel, and the honest albedo in baseColorFactor.
    //
    // A NOTE ON A MIS-DIAGNOSIS, kept because it is exactly the trap KNOWN_BUGS warns about
    // in reverse: the parts first appeared to render ORANGE-RED, and the reflex was to blame
    // the renderer. They were not red. They are light grey (0.30-0.40 albedo) — and Jake's
    // cell is washed by a RED ALARM light. A grey surface under a red light is red. The very
    // same parts read blue-grey at the weapon bench, under its blue panel glow. The lighting
    // was honest and the parts were honest; the READING was wrong. Verify a material under a
    // NEUTRAL light before you accuse the shader.
    device.drawMeshPBR(frame, m_parts[pi].mesh, m_white,
                       x3::rhi::TextureHandle{}, m_parts[pi].mr,
                       albedo, noEm, M);

    // ---- The optic's GLASS. Dark, glossy — it catches the room, it does not glow.
    if (a.part == AttachPart::Scope && m_lensGlass.valid()) {
        const float glass[4] = { 0.035f, 0.045f, 0.055f, 1.0f };   // dark blue-grey glass
        device.drawMeshPBR(frame, m_lensGlass, m_white,
                           x3::rhi::TextureHandle{}, m_mrGlass, glass, noEm, M);
    }
    // ---- The LIT CORES. The only emitters here: the laser diode and the cell window.
    if (m_core.valid() && (a.emissive[0] + a.emissive[1] + a.emissive[2]) > 0.01f) {
        x3::phys::Vec3 coreL = L;
        float coreScale = partScale;
        if (a.part == AttachPart::Laser) {
            coreL.z += 0.070f * run;        // the diode face, at the emitter's front
            coreScale *= 1.05f;
        } else if (a.part == AttachPart::Cell) {
            // The mount already sits ON the receiver's flank (vmBounds), so the window
            // needs no outward push — it just stands proud of the housing face.
            coreL.x += 0.030f * run;
            coreScale *= 2.2f;
        }
        const x3::phys::Vec3 cw = toWorld(coreL);
        float CM[16];
        composeBasis(CM, bx, by, bz, cw, coreScale);
        const float dark[4] = { 0.04f, 0.04f, 0.04f, 1.0f };       // honest albedo under the glow
        const float em[4]   = { a.emissive[0], a.emissive[1], a.emissive[2], 1.6f };
        device.drawMeshEmissive(frame, m_core, m_white, dark, em, CM);
    }
}

void AttachView::drawFirstPerson(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                                 const Arsenal& arsenal,
                                 float eyeX, float eyeY, float eyeZ, float yaw, float pitch,
                                 float extraYawOff, float extraPitchOff, float extraRollOff,
                                 float extraFwd, float extraRight, float extraDown) const {
    if (!m_built) return;
    const int sel = arsenal.selected();
    if (sel < 0) return;
    const WeaponLoadout& L = arsenal.loadouts().at(sel);
    if (L.fittedCount() == 0) return;

    // THE viewmodel frame — the same one the gun is drawn with and the same one the
    // muzzle is solved in. Parts cannot drift off the gun.
    const Arsenal::VmFrame f = arsenal.currentViewmodelFrame(
        eyeX, eyeY, eyeZ, yaw, pitch,
        extraYawOff, extraPitchOff, extraRollOff, extraFwd, extraRight, extraDown);

    const WeaponDef& def = arsenal.baseDef(sel);   // mount geometry is BASE geometry
    for (int i = 0; i < kAttachSlotCount; ++i) {
        const AttachSpec& a = L.slots[i];
        if (!a.valid) continue;
        drawSpec(device, frame, def, a, f.bx, f.by, f.bz, f.pos, f.scale);
    }
}

void AttachView::drawAt(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                        const Arsenal& arsenal, const float model[16], float modelScale) const {
    if (!m_built) return;
    const int sel = arsenal.selected();
    if (sel < 0) return;
    const WeaponLoadout& L = arsenal.loadouts().at(sel);
    if (L.fittedCount() == 0) return;

    // Decompose the caller's world matrix into a basis + origin (the 3P hand socket).
    const float sx = std::sqrt(model[0]*model[0] + model[1]*model[1] + model[2]*model[2]);
    const float sy = std::sqrt(model[4]*model[4] + model[5]*model[5] + model[6]*model[6]);
    const float sz = std::sqrt(model[8]*model[8] + model[9]*model[9] + model[10]*model[10]);
    if (sx < 1e-6f || sy < 1e-6f || sz < 1e-6f) return;
    const x3::phys::Vec3 bx{ model[0]/sx, model[1]/sx, model[2]/sx };
    const x3::phys::Vec3 by{ model[4]/sy, model[5]/sy, model[6]/sy };
    const x3::phys::Vec3 bz{ model[8]/sz, model[9]/sz, model[10]/sz };
    const x3::phys::Vec3 origin{ model[12], model[13], model[14] };
    const float scale = (modelScale > 1e-6f) ? modelScale : sx;

    const WeaponDef& def = arsenal.baseDef(sel);
    for (int i = 0; i < kAttachSlotCount; ++i) {
        const AttachSpec& a = L.slots[i];
        if (!a.valid) continue;
        drawSpec(device, frame, def, a, bx, by, bz, origin, scale);
    }
}

} // namespace x3::game
