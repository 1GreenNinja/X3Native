// THE CONFECTION ANNEX — per-room content (Phase 3; plan Tasks 7-11). This TU
// exists FROM Phase 2 so the annex core (factory_annex.cpp) never has to be
// touched when the rooms land: build() and tick() already call every hook
// below. Each build hook authors its room's props into ctx.scene (push every
// created mesh handle into ctx.meshes — the annex frees that ONE vector
// uniformly) and records the room's animated/glow entity spans on `room`
// (CONTIGUOUS spans; rifthub law). Each tick hook pokes those spans in place —
// NO per-frame heap.
//
// SPAN DISCIPLINE (every room, same shape): statics first, then the PROP span
// (one contiguous run of animated entities, each authored AT THE ORIGIN and
// placed by its transform so tick() can re-pose by rewriting the matrix), then
// the GLOW span (one contiguous run of emissive-pulse entities). The plan's
// Task-7 exemplar interleaves arm + rim ring per vat; that would fragment both
// spans, so the authoring below runs the same data in span-major order instead
// (all arms, then all rims) — same props, contiguous spans.
//
// CENTER LAW (Phase-2 handoff): the cab's vertical chain passes through every
// room's center — a ~4.4 m clear cylinder at (centerX, centerZ) stays EMPTY,
// full height, in every room. All content RINGS the center.
//
// Room map (plan Tasks 7-11):
//   A (y=2)  MIXTURE ATRIUM  — confection river (sunken channel through the
//            Floor-A slab; raspberry water), 6 copper vats w/ stirring arms +
//            rim glow rings, 2 brass footbridges, pipe canopy.
//   B (y=15) INVENTION WORKS — 8 whimsy machines + the wrapping conveyor.
//   C (y=28) FIZZ GALLERY    — 4 glass bubble columns, 3 ceiling fans,
//            the low-grav zone (trigger 311).
//   D (y=41) SORTING HALL    — orb ring (10 gold, 2 duds), 2 sorter arms,
//            the Chute of Dubious Quality (trigger 310).
//   E (y=54) TUBE JUNCTION   — 5 glass transport tubes, pneumatic capsule,
//            the golden burst dais (trigger 312) + tube ride (313).

#include "factory_annex.h"

#include "mesh_prims.h"

#include <cmath>

namespace x3::game {

namespace {

constexpr float kTwoPi = 6.2831853f;

// ---- Palette (matches factory_annex.cpp's shell constants) -----------------
constexpr float kCopperTint[3] = { 0.74f, 0.40f, 0.26f };   // candy-copper vats
constexpr float kBrassTint[3]  = { 0.69f, 0.55f, 0.34f };   // brass #b08d57
constexpr float kBrassGlow[3]  = { 1.00f, 0.78f, 0.42f };   // warm brass emissive
constexpr float kDarkIron[3]   = { 0.15f, 0.13f, 0.18f };   // aubergine iron

// Column-major basis+translation (the rifthub makeXform, verbatim convention).
inline void makeXform(float m[16],
                      const float xA[3], const float yA[3], const float zA[3],
                      float wx, float wy, float wz) {
    m[0] = xA[0]; m[1] = xA[1]; m[2]  = xA[2]; m[3]  = 0.0f;
    m[4] = yA[0]; m[5] = yA[1]; m[6]  = yA[2]; m[7]  = 0.0f;
    m[8] = zA[0]; m[9] = zA[1]; m[10] = zA[2]; m[11] = 0.0f;
    m[12] = wx;   m[13] = wy;   m[14] = wz;    m[15] = 1.0f;
}

// Rotation about +Y (RH, Y-up): +X -> (cos, 0, -sin), +Z -> (sin, 0, cos).
// det = +1. Writes the FULL matrix (rotation + translation).
inline void yawXform(float m[16], float yaw, float wx, float wy, float wz) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float xA[3] = { c, 0, -s }, yA[3] = { 0, 1, 0 }, zA[3] = { s, 0, c };
    makeXform(m, xA, yA, zA, wx, wy, wz);
}

// Rotation-only yaw poke: rewrite the 3x3 basis IN PLACE, translation kept.
// The tick() workhorse for anything that spins where it stands (stir arms,
// fans, centrifuges): zero recomputation of the anchor.
inline void pokeYaw(float m[16], float yaw) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    m[0] = c;  m[1] = 0; m[2]  = -s;
    m[4] = 0;  m[5] = 1; m[6]  = 0;
    m[8] = s;  m[9] = 0; m[10] = c;
}

// ---- Entity authoring helpers ----------------------------------------------
// EVERY mesh handle goes into ctx.meshes (the annex's ONE uniform vector).
// Animated entities are authored AT THE ORIGIN and placed via transform.

// A PBR/tinted box authored at the origin, placed by yaw+translation.
// tag Prop for animated span members, Static for room furniture.
uint32_t addBox(FactoryRoomCtx& ctx, float wx, float wy, float wz,
                float hx, float hy, float hz, float yaw,
                const SurfaceSet* sf, const float tint[3],
                float emStrength = 0.0f, const float* emColor = nullptr,
                bool isProp = false, float uvScale = 0.35f) {
    x3::prims::PrimMesh pm = x3::prims::makeBox(hx, hy, hz, 0, 0, 0, uvScale);
    Entity e;
    e.mesh = ctx.device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                   pm.index.data(), (uint32_t)pm.index.size());
    ctx.meshes.push_back(e.mesh);
    if (sf && sf->ok) { e.tex = sf->albedo; e.normalTex = sf->normal; e.mrTex = sf->mr; }
    e.baseColor[0] = tint[0]; e.baseColor[1] = tint[1]; e.baseColor[2] = tint[2];
    e.baseColor[3] = 1.0f;
    if (emStrength > 0.0f) {
        const float* g = emColor ? emColor : tint;
        e.emissive[0] = g[0]; e.emissive[1] = g[1]; e.emissive[2] = g[2];
        e.emissive[3] = emStrength;
    }
    e.tag = (uint32_t)(isProp ? Tag::Prop : Tag::Static);
    yawXform(e.transform, yaw, wx, wy, wz);
    return ctx.scene.add(e);
}

// A glow-trim box: dark body + tinted emissive (the rifthub realism law — a
// dim emitter must never read as saturated plastic). Origin-authored.
uint32_t addGlow(FactoryRoomCtx& ctx, float wx, float wy, float wz,
                 float hx, float hy, float hz, float yaw,
                 const float glow[3], float emStrength) {
    x3::prims::PrimMesh pm = x3::prims::makeBox(hx, hy, hz, 0, 0, 0);
    Entity e;
    e.mesh = ctx.device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                   pm.index.data(), (uint32_t)pm.index.size());
    ctx.meshes.push_back(e.mesh);
    e.baseColor[0] = glow[0] * 0.25f; e.baseColor[1] = glow[1] * 0.25f;
    e.baseColor[2] = glow[2] * 0.25f; e.baseColor[3] = 1.0f;
    e.emissive[0] = glow[0]; e.emissive[1] = glow[1]; e.emissive[2] = glow[2];
    e.emissive[3] = emStrength;
    e.tag = (uint32_t)Tag::Prop;
    yawXform(e.transform, yaw, wx, wy, wz);
    return ctx.scene.add(e);
}

// Instance a SHARED mesh handle (sphere/torus — pushed into ctx.meshes ONCE by
// the caller) with an arbitrary basis. Used by bubbles/orbs/capsule.
uint32_t addShared(FactoryRoomCtx& ctx, x3::rhi::MeshHandle mesh,
                   const float xA[3], const float yA[3], const float zA[3],
                   float wx, float wy, float wz,
                   const float tint[3], float emStrength, const float* emColor,
                   const SurfaceSet* sf = nullptr) {
    Entity e;
    e.mesh = mesh;
    if (sf && sf->ok) { e.tex = sf->albedo; e.normalTex = sf->normal; e.mrTex = sf->mr; }
    e.baseColor[0] = tint[0]; e.baseColor[1] = tint[1]; e.baseColor[2] = tint[2];
    e.baseColor[3] = 1.0f;
    if (emStrength > 0.0f) {
        const float* g = emColor ? emColor : tint;
        e.emissive[0] = g[0]; e.emissive[1] = g[1]; e.emissive[2] = g[2];
        e.emissive[3] = emStrength;
    }
    e.tag = (uint32_t)Tag::Prop;
    makeXform(e.transform, xA, yA, zA, wx, wy, wz);
    return ctx.scene.add(e);
}

// A ring-tangent glow stud: thin emissive box whose LONG axis is tangent to
// the circle of `radius` around (ringX, ringZ) at angle `a` (basis built
// directly — no yaw-convention gymnastics; det +1).
uint32_t addRingGlow(FactoryRoomCtx& ctx, float ringX, float y, float ringZ,
                     float radius, float a, float hTan, float hY, float hRad,
                     const float glow[3], float emStrength) {
    const float c = std::cos(a), s = std::sin(a);
    x3::prims::PrimMesh pm = x3::prims::makeBox(hTan, hY, hRad, 0, 0, 0);
    Entity e;
    e.mesh = ctx.device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                   pm.index.data(), (uint32_t)pm.index.size());
    ctx.meshes.push_back(e.mesh);
    e.baseColor[0] = glow[0] * 0.25f; e.baseColor[1] = glow[1] * 0.25f;
    e.baseColor[2] = glow[2] * 0.25f; e.baseColor[3] = 1.0f;
    e.emissive[0] = glow[0]; e.emissive[1] = glow[1]; e.emissive[2] = glow[2];
    e.emissive[3] = emStrength;
    e.tag = (uint32_t)Tag::Prop;
    // xA = tangent, zA = -radial so xA x yA = zA (det +1).
    const float xA[3] = { -s, 0, c }, yA[3] = { 0, 1, 0 }, zA[3] = { -c, 0, -s };
    makeXform(e.transform, xA, yA, zA,
              ringX + radius * c, y, ringZ + radius * s);
    return ctx.scene.add(e);
}

} // namespace

// ============================================================================
// FLOOR A (y=2) — THE MIXTURE ATRIUM (Task 7)
// ============================================================================
// The confection river runs the full Z extent in the sunken channel Floor A's
// slab leaves open at local x [kRiverX0, kRiverX1] (factory_annex.cpp cuts it;
// the plan's "diagonal" band is adapted to the axis-aligned slab + physics —
// IPhysicsWorld::addBox has no yaw). Water surface = FactoryAnnex::kRiverSurfY,
// BELOW every walkable deck, so the global water plane reads as the sunken
// river inside (and the confection sea the annex stands over, outside).
//
// Six copper vats RING the center (the cab chain owns the middle 4.4 m).
// Local-offset table (from the room center); the west bank is the river's.
constexpr float kVatPos[6][2] = {
    { 13.0f,  0.0f }, { 9.5f, -9.5f }, { 0.0f, -13.0f },
    { -2.0f,  7.0f }, { 2.0f,  13.0f }, { 13.0f, 10.0f },
};
constexpr float kVatR      = 1.5f;    // vat body radius (3 m dia)
constexpr float kVatH      = 4.0f;    // vat height
constexpr float kStirY     = 4.2f;    // stir arm above baseY
constexpr int   kMixRimN   = 8;       // rim glow studs per vat
// Span sizes the self-test pins (factory_annex.cpp F1c): prop = 6 stir arms;
// glow = 6*8 rim studs + 8 river under-glow strips = 56.

void buildRoomMixture(FactoryRoomCtx& ctx, AnnexRoom& room) {
    Scene& s = ctx.scene;
    const float ax = ctx.centerX, az = ctx.centerZ, y0 = room.baseY;
    const SurfaceSet& sCopper = ctx.surf.get(ctx.device, "mw_metal_trim_a");
    const SurfaceSet& sBrass  = ctx.surf.get(ctx.device, "mw_metal_trim_b");
    const SurfaceSet& sIron   = ctx.surf.get(ctx.device, "mw_metal_panels_a");

    const float rx0 = ax + FactoryAnnex::kRiverX0, rx1 = ax + FactoryAnnex::kRiverX1;
    const float rcx = (rx0 + rx1) * 0.5f, rhx = (rx1 - rx0) * 0.5f;
    const float bedY = FactoryAnnex::kRiverBedY;                  // 0.65

    // ---- The river channel: bed + banks + end culverts (all physical) ------
    // Bed: a full-channel slab whose top is the wading floor (water is ~0.9 m
    // deep — you can fall in, and you can climb back out on the east steps).
    addBox(ctx, rcx, bedY - 0.30f, az, rhx, 0.30f, 20.0f, 0.0f, &sIron, kDarkIron);
    ctx.physics.addBox({ rhx, 0.30f, 20.0f }, { rcx, bedY - 0.30f, az },
                       0.0f, x3::phys::Layer::Static);
    // Banks: bed-to-slab walls on both sides (the slab itself covers 1.5..2.0).
    for (int sgn = -1; sgn <= 1; sgn += 2) {
        const float bx = (sgn < 0) ? rx0 - 0.15f : rx1 + 0.15f;
        const float hy = (1.5f - bedY) * 0.5f, cy = (1.5f + bedY) * 0.5f;
        addBox(ctx, bx, cy, az, 0.15f, hy, 20.0f, 0.0f, &sIron, kDarkIron);
        ctx.physics.addBox({ 0.15f, hy, 20.0f }, { bx, cy, az },
                           0.0f, x3::phys::Layer::Static);
    }
    // End culverts: the channel meets the +Z/-Z iron walls below the wall base
    // (walls start at y=2) — brass-grated caps so the river reads as flowing
    // in from beyond the works, and nobody swims into the void.
    for (int sgn = -1; sgn <= 1; sgn += 2) {
        const float cz = az + sgn * 19.85f;
        const float hy = (y0 - bedY) * 0.5f + 0.25f, cy = (y0 + bedY) * 0.5f;
        addBox(ctx, rcx, cy, cz, rhx, hy, 0.15f, 0.0f, &sBrass, kBrassTint,
               0.22f, kBrassGlow);
        ctx.physics.addBox({ rhx, hy, 0.15f }, { rcx, cy, cz },
                           0.0f, x3::phys::Layer::Static);
    }
    // Climb-out steps on the east bank (bed 0.65 -> 1.15 -> 1.65 -> deck 2.0).
    {
        const float sx = rx1 - 0.55f;
        const float steps[2][2] = { { 0.90f, 1.6f }, { 1.40f, 0.6f } };  // {cy, cz-off}
        for (auto& st : steps) {
            addBox(ctx, sx, st[0], az + st[1], 0.55f, 0.25f, 0.6f, 0.0f,
                   &sBrass, kBrassTint);
            ctx.physics.addBox({ 0.55f, 0.25f, 0.6f }, { sx, st[0], az + st[1] },
                               0.0f, x3::phys::Layer::Static);
        }
    }

    // ---- Two brass footbridges over the river (z = -10 / +10): three stepped
    // deck boxes each (0.3 m risers — walkable without ramp physics) + rails.
    for (int b = 0; b < 2; ++b) {
        const float bz = az + (b == 0 ? -10.0f : 10.0f);
        struct Deck { float cx, topY, hx; };
        const Deck decks[3] = {
            { rx0 - 0.2f + 2.2f, 2.30f, 2.4f },     // west approach
            { rcx,               2.60f, 2.4f },     // crown
            { rx1 + 0.2f - 2.2f, 2.30f, 2.4f },     // east approach
        };
        for (const Deck& d : decks) {
            addBox(ctx, d.cx, d.topY - 0.15f, bz, d.hx, 0.15f, 1.3f, 0.0f,
                   &sBrass, kBrassTint);
            ctx.physics.addBox({ d.hx, 0.15f, 1.3f }, { d.cx, d.topY - 0.15f, bz },
                               0.0f, x3::phys::Layer::Static);
        }
        // Handrails: one warm-glow strip per side, full span (visual only).
        for (int sgn = -1; sgn <= 1; sgn += 2)
            addBox(ctx, rcx, 3.35f, bz + sgn * 1.25f, rhx + 1.6f, 0.06f, 0.06f,
                   0.0f, &sBrass, kBrassTint, 0.30f, kBrassGlow);
    }

    // ---- Six copper vats (bodies + syrup tops + physics) --------------------
    for (int v = 0; v < 6; ++v) {
        const float vx = ax + kVatPos[v][0], vz = az + kVatPos[v][1];
        for (int i = 0; i < 10; ++i) {   // 10-segment tangent-box cylinder
            const float a = (i / 10.0f) * kTwoPi;
            const float c = std::cos(a), sn = std::sin(a);
            x3::prims::PrimMesh pm = x3::prims::makeBox(0.47f, kVatH * 0.5f, 0.12f,
                                                        0, 0, 0, 0.35f);
            Entity e;
            e.mesh = ctx.device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                           pm.index.data(), (uint32_t)pm.index.size());
            ctx.meshes.push_back(e.mesh);
            if (sCopper.ok) { e.tex = sCopper.albedo; e.normalTex = sCopper.normal; e.mrTex = sCopper.mr; }
            e.baseColor[0] = kCopperTint[0]; e.baseColor[1] = kCopperTint[1];
            e.baseColor[2] = kCopperTint[2]; e.baseColor[3] = 1.0f;
            e.tag = (uint32_t)Tag::Static;
            const float xA[3] = { -sn, 0, c }, yA[3] = { 0, 1, 0 }, zA[3] = { -c, 0, -sn };
            makeXform(e.transform, xA, yA, zA,
                      vx + kVatR * c, y0 + kVatH * 0.5f, vz + kVatR * sn);
            s.add(e);
        }
        // Syrup surface: a glowing accent disc just under the rim (static glow —
        // the ANIMATED glow lives in the rim ring span below).
        {
            x3::prims::PrimMesh pm = x3::prims::makeCylinder(1.35f, 1.35f, 0.06f, 20);
            Entity e;
            e.mesh = ctx.device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                           pm.index.data(), (uint32_t)pm.index.size());
            ctx.meshes.push_back(e.mesh);
            e.baseColor[0] = room.accent[0] * 0.35f; e.baseColor[1] = room.accent[1] * 0.35f;
            e.baseColor[2] = room.accent[2] * 0.35f; e.baseColor[3] = 1.0f;
            e.emissive[0] = room.accent[0]; e.emissive[1] = room.accent[1];
            e.emissive[2] = room.accent[2]; e.emissive[3] = 0.55f;
            e.tag = (uint32_t)Tag::Static;
            const float xA[3] = { 1, 0, 0 }, yA[3] = { 0, 1, 0 }, zA[3] = { 0, 0, 1 };
            makeXform(e.transform, xA, yA, zA, vx, y0 + kVatH - 0.35f, vz);
            s.add(e);
        }
        // Physics: one square-footprint block per vat (climb-proof, walk-solid).
        ctx.physics.addBox({ kVatR, kVatH * 0.5f, kVatR },
                           { vx, y0 + kVatH * 0.5f, vz }, 0.0f,
                           x3::phys::Layer::Static);
        // Canopy drop-pipe into the vat (visual only, overhead).
        addBox(ctx, vx, (y0 + kVatH + 11.3f) * 0.5f, vz,
               0.16f, (11.3f - kVatH) * 0.5f + 0.4f, 0.16f, 0.0f,
               &sCopper, kCopperTint);
    }

    // ---- Pipe canopy: a brass square RINGING the center at y ~+11.5 + two
    // outer runs (visual only — well above head height).
    {
        const float py = y0 + 9.5f;
        addBox(ctx, ax, py, az + 5.0f, 18.0f, 0.25f, 0.25f, 0.0f, &sBrass, kBrassTint);
        addBox(ctx, ax, py, az - 5.0f, 18.0f, 0.25f, 0.25f, 0.0f, &sBrass, kBrassTint);
        addBox(ctx, ax + 5.0f, py, az, 0.25f, 0.25f, 18.0f, 0.0f, &sBrass, kBrassTint);
        addBox(ctx, ax - 5.0f, py, az, 0.25f, 0.25f, 18.0f, 0.0f, &sBrass, kBrassTint);
        addBox(ctx, ax, py + 0.8f, az + 12.0f, 18.0f, 0.22f, 0.22f, 0.0f, &sCopper, kCopperTint);
        addBox(ctx, ax, py + 0.8f, az - 12.0f, 18.0f, 0.22f, 0.22f, 0.0f, &sCopper, kCopperTint);
    }

    // ---- PROP SPAN: the six stir arms (contiguous; tick yaws them in place).
    room.propEntFirst = s.size();
    for (int v = 0; v < 6; ++v) {
        const float vx = ax + kVatPos[v][0], vz = az + kVatPos[v][1];
        addBox(ctx, vx, y0 + kStirY, vz, 1.7f, 0.06f, 0.12f,
               (float)v * 1.047f, &sBrass, kBrassTint, 0.25f, kBrassGlow,
               /*isProp*/true);
    }
    room.propEntCount = s.size() - room.propEntFirst;

    // ---- GLOW SPAN: 6x8 rim studs, then 8 river under-glow strips -----------
    room.glowEntFirst = s.size();
    for (int v = 0; v < 6; ++v) {
        const float vx = ax + kVatPos[v][0], vz = az + kVatPos[v][1];
        for (int i = 0; i < kMixRimN; ++i)
            addRingGlow(ctx, vx, y0 + kStirY - 0.15f, vz, kVatR + 0.05f,
                        (i / (float)kMixRimN) * kTwoPi,
                        0.30f, 0.05f, 0.08f, room.accent, 1.1f);
    }
    // River under-glow: emissive strips on the bed, BELOW the water surface —
    // the raspberry river glows from within (pulse 0.18 Hz in tick()).
    // (Deep-red strips, not the pink accent: under ~1 m of raspberry water the
    // accent's green/blue leak white-clipped the channel on the first capture.)
    constexpr float kRiverGlowCol[3] = { 0.85f, 0.08f, 0.26f };
    for (int i = 0; i < 8; ++i) {
        const float gz = az - 17.5f + 5.0f * (float)i;
        addGlow(ctx, rcx, bedY + 0.10f, gz, rhx - 1.5f, 0.05f, 0.9f, 0.0f,
                kRiverGlowCol, 0.55f);
    }
    room.glowEntCount = s.size() - room.glowEntFirst;

    // ---- THE RIVER WATER (host applies; plan T7 step 2) ---------------------
    if (ctx.river) {
        auto& wp = *ctx.river;
        wp.enabled    = true;
        wp.seaLevel   = FactoryAnnex::kRiverSurfY;
        wp.amplitude  = 0.05f;      // syrup, not surf
        wp.steepness  = 0.30f;
        wp.waveLength = 2.8f;
        wp.speed      = 0.45f;
        // Deep raspberry syrup — the pale first-cut tints read washed-out
        // cream at glancing angles (sky reflection dominates); keep both
        // colors saturated and the glints modest so the sea stays CANDY.
        wp.deepColor[0]    = 0.20f; wp.deepColor[1]    = 0.010f; wp.deepColor[2]    = 0.060f;
        wp.shallowColor[0] = 0.42f; wp.shallowColor[1] = 0.06f;  wp.shallowColor[2] = 0.18f;
        // Sun matches applyAtmosphere's toffee-dusk sun.
        wp.sunDir[0] = -0.25f; wp.sunDir[1] = 0.72f; wp.sunDir[2] = 0.35f;
        wp.specular = 3.5f;
        wp.fresnel  = 0.02f;
    }
}

void tickRoomMixture(Scene& scene, AnnexRoom& room, float t) {
    if (room.propEntCount == 0) return;
    // Stir arms: yaw = t*0.8 + i*1.047 (plan numbers) — rotation-basis poke,
    // translation untouched (the arm spins where it stands).
    for (uint32_t i = 0; i < room.propEntCount; ++i)
        pokeYaw(scene.get(room.propEntFirst + i).transform,
                t * 0.8f + (float)i * 1.047f);
    // Rim rings: per-vat/per-stud phased breathe. Raspberry is warm — capped
    // ~1.6 peak (the ACES-clip law; the plan's 2.0+1.2 clips to cream).
    for (uint32_t i = 0; i < room.glowEntCount; ++i) {
        Entity& e = scene.get(room.glowEntFirst + i);
        if (i < 48) {
            e.emissive[3] = 1.1f + 0.5f * std::sin(t * 1.1f
                              + (float)(i >> 3) * 0.7f + (float)(i & 7) * 0.35f);
        } else {
            // River under-glow: the plan's 0.18 Hz swell (1.2 -> 2.6 adapted
            // WAY down: raspberry is warm, and the first capture showed the
            // strip white-clipping the whole channel — 0.45 -> 1.15 reads as
            // glow WITHIN the syrup instead of a lightbox).
            e.emissive[3] = 0.55f + 0.25f * std::sin(t * (0.18f * kTwoPi)
                              + (float)(i - 48) * 0.65f);
        }
    }
}

// ============================================================================
// FLOOR B (y=15) — THE INVENTION WORKS (Task 8)
// ============================================================================
// Eight whimsy machines (the plan's prop table, transcribed) + the 14 m
// wrapping conveyor. Floor B hosts the BORE MOUTH: the cab's lateral leg runs
// x [-20, 0] at z 0, so the strip z in [-3.2, 3.2] west of center stays CLEAR
// (machines sit north/south of it and east of center). The chute drop-shaft
// column at local (8, 8) is avoided too (Task 10 cuts it through this floor).
//
// Machine layout (local offsets; order == the plan's table == span order):
//   0 Gum-Stretcher    (-10,  9)  piston, amp 0.9 @ 1.4 Hz     mint
//   1 Fizz Compressor  (-10, -9)  spin 2.2 rad/s               amber
//   2 Idea Bellows     ( -3, 13)  squash 0.7..1.15 @ 0.5 Hz    violet
//   3 Sprocket Fountain( -3,-13)  spin 1.1 rad/s + bob 0.4     brass
//   4 Wobble Boiler    ( 10,-10)  sway roll +-0.12 @ 0.9 Hz    raspberry
//   5 Button Organ     ( 12,  0)  key-chase, 8 keys @ 0.12 s   white
//   6 Notion Centrifuge( 13, 11)  spin 4.0 rad/s               cyan
//   7 The Maybe Machine( 16,-16)  random flicker (t*13.7 hash) gold
// Spans: prop = 8 movers + 24 conveyor slats + 8 gizmo cubes = 40;
//        glow = 5 machine studs + 8 organ keys + 1 + 1 = 15.
constexpr float kInvPos[8][2] = {
    { -10.0f,   9.0f }, { -10.0f,  -9.0f }, { -3.0f,  13.0f }, { -3.0f, -13.0f },
    {  10.0f, -10.0f }, {  12.0f,   0.0f }, { 13.0f,  11.0f }, { 16.0f, -16.0f },
};
constexpr float kMint[3]   = { 0.40f, 1.00f, 0.60f };
constexpr float kAmber[3]  = { 1.00f, 0.72f, 0.25f };
constexpr float kViolet[3] = { 0.62f, 0.35f, 1.00f };
constexpr float kRasp[3]   = { 1.00f, 0.35f, 0.55f };
constexpr float kWhite[3]  = { 1.00f, 1.00f, 1.00f };
constexpr float kCyan[3]   = { 0.35f, 0.90f, 1.00f };
constexpr float kGold[3]   = { 1.00f, 0.84f, 0.30f };
// Conveyor: 14 m along Z at local x +5, 24 slats, 1.2 m/s scroll, 8 gizmos.
constexpr float kConvX = 5.0f, kConvLen = 14.0f, kConvTopY = 0.95f;
constexpr int   kConvSlats = 24, kConvGizmos = 8;
constexpr float kConvSpeed = 1.2f;

void buildRoomInvention(FactoryRoomCtx& ctx, AnnexRoom& room) {
    Scene& s = ctx.scene;
    const float ax = ctx.centerX, az = ctx.centerZ, y0 = room.baseY;
    const SurfaceSet& sIron  = ctx.surf.get(ctx.device, "mw_metal_panels_a");
    const SurfaceSet& sBrass = ctx.surf.get(ctx.device, "mw_metal_trim_b");
    const SurfaceSet& sCop   = ctx.surf.get(ctx.device, "mw_metal_trim_a");
    auto physBox = [&](float wx, float wy, float wz, float hx, float hy, float hz) {
        ctx.physics.addBox({ hx, hy, hz }, { wx, wy, wz }, 0.0f,
                           x3::phys::Layer::Static);
    };

    // ---- Machine bodies (static furniture; the movers land in the prop span).
    // 0 Gum-Stretcher: two pillars + crown beam; the piston plate bobs between.
    {
        const float mx = ax + kInvPos[0][0], mz = az + kInvPos[0][1];
        for (int sgn = -1; sgn <= 1; sgn += 2)
            addBox(ctx, mx + sgn * 1.8f, y0 + 1.5f, mz, 0.22f, 1.5f, 0.35f, 0.0f,
                   &sBrass, kBrassTint);
        addBox(ctx, mx, y0 + 3.05f, mz, 2.1f, 0.15f, 0.45f, 0.0f, &sBrass, kBrassTint);
        physBox(mx, y0 + 1.5f, mz, 2.0f, 1.5f, 1.5f);
    }
    // 1 Fizz Compressor: squat drum + dome cap; rotor spins above.
    {
        const float mx = ax + kInvPos[1][0], mz = az + kInvPos[1][1];
        for (int i = 0; i < 8; ++i) {
            const float a = (i / 8.0f) * kTwoPi, c = std::cos(a), sn = std::sin(a);
            const float xA[3] = { -sn, 0, c }, yA[3] = { 0, 1, 0 }, zA[3] = { -c, 0, -sn };
            x3::prims::PrimMesh pm = x3::prims::makeBox(0.62f, 1.25f, 0.14f, 0, 0, 0, 0.35f);
            Entity e;
            e.mesh = ctx.device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                           pm.index.data(), (uint32_t)pm.index.size());
            ctx.meshes.push_back(e.mesh);
            if (sCop.ok) { e.tex = sCop.albedo; e.normalTex = sCop.normal; e.mrTex = sCop.mr; }
            e.baseColor[0] = kCopperTint[0]; e.baseColor[1] = kCopperTint[1];
            e.baseColor[2] = kCopperTint[2]; e.baseColor[3] = 1.0f;
            e.tag = (uint32_t)Tag::Static;
            makeXform(e.transform, xA, yA, zA, mx + 1.3f * c, y0 + 1.25f, mz + 1.3f * sn);
            s.add(e);
        }
        addBox(ctx, mx, y0 + 2.8f, mz, 0.9f, 0.3f, 0.9f, 0.0f, &sBrass, kBrassTint);
        addBox(ctx, mx, y0 + 3.3f, mz, 0.18f, 0.5f, 0.18f, 0.0f, &sBrass, kBrassTint);
        physBox(mx, y0 + 1.4f, mz, 1.5f, 1.4f, 1.5f);
    }
    // 2 Idea Bellows: plinth + cap; the bellows block squashes between them.
    {
        const float mx = ax + kInvPos[2][0], mz = az + kInvPos[2][1];
        addBox(ctx, mx, y0 + 0.25f, mz, 1.0f, 0.25f, 1.0f, 0.0f, &sIron, kDarkIron);
        addBox(ctx, mx, y0 + 3.6f, mz, 0.8f, 0.12f, 0.8f, 0.6f, &sBrass, kBrassTint);
        physBox(mx, y0 + 1.0f, mz, 1.0f, 1.0f, 1.0f);
    }
    // 3 Sprocket Fountain: a fluted column; the sprocket spins + bobs on it.
    {
        const float mx = ax + kInvPos[3][0], mz = az + kInvPos[3][1];
        addBox(ctx, mx, y0 + 2.75f, mz, 0.45f, 2.75f, 0.45f, 0.785f, &sBrass, kBrassTint);
        addBox(ctx, mx, y0 + 0.3f, mz, 1.4f, 0.3f, 1.4f, 0.0f, &sIron, kDarkIron);
        physBox(mx, y0 + 2.75f, mz, 0.7f, 2.75f, 0.7f);
    }
    // 4 Wobble Boiler: plinth only — the 4x4x4 tank ITSELF sways (prop span).
    {
        const float mx = ax + kInvPos[4][0], mz = az + kInvPos[4][1];
        addBox(ctx, mx, y0 + 0.2f, mz, 2.2f, 0.2f, 2.2f, 0.0f, &sIron, kDarkIron);
        physBox(mx, y0 + 2.2f, mz, 2.0f, 2.0f, 2.0f);
    }
    // 5 Button Organ: console + five ranked pipes (a machine that plays ideas).
    {
        const float mx = ax + kInvPos[5][0], mz = az + kInvPos[5][1];
        addBox(ctx, mx, y0 + 0.8f, mz, 1.0f, 0.8f, 3.0f, 0.0f, &sBrass, kBrassTint);
        for (int i = 0; i < 5; ++i) {
            const float ph = 0.9f + 0.45f * (float)i;   // ranked pipe heights
            addBox(ctx, mx + 0.55f, y0 + 1.6f + ph * 0.5f, mz - 2.0f + (float)i * 1.0f,
                   0.28f, ph * 0.5f, 0.28f, 0.0f, &sCop, kCopperTint);
        }
        physBox(mx, y0 + 1.2f, mz, 1.2f, 1.2f, 3.2f);
    }
    // 6 Notion Centrifuge: low wide drum; the arm spins fast above it.
    {
        const float mx = ax + kInvPos[6][0], mz = az + kInvPos[6][1];
        x3::prims::PrimMesh pm = x3::prims::makeCylinder(2.2f, 2.2f, 0.5f, 18, 0.3f);
        Entity e;
        e.mesh = ctx.device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                       pm.index.data(), (uint32_t)pm.index.size());
        ctx.meshes.push_back(e.mesh);
        if (sIron.ok) { e.tex = sIron.albedo; e.normalTex = sIron.normal; e.mrTex = sIron.mr; }
        e.baseColor[0] = kDarkIron[0]; e.baseColor[1] = kDarkIron[1];
        e.baseColor[2] = kDarkIron[2]; e.baseColor[3] = 1.0f;
        e.tag = (uint32_t)Tag::Static;
        yawXform(e.transform, 0.0f, mx, y0 + 0.5f, mz);
        s.add(e);
        physBox(mx, y0 + 0.5f, mz, 2.2f, 0.5f, 2.2f);
    }
    // 7 The Maybe Machine: a tall enigmatic cabinet (it might do anything).
    {
        const float mx = ax + kInvPos[7][0], mz = az + kInvPos[7][1];
        addBox(ctx, mx, y0 + 3.5f, mz, 1.0f, 3.5f, 1.0f, 0.3f, &sIron, kDarkIron);
        physBox(mx, y0 + 3.5f, mz, 1.2f, 3.5f, 1.2f);
    }

    // ---- Conveyor frame (rails + legs + a walk-solid underbody) -------------
    {
        const float hz = kConvLen * 0.5f + 0.2f;
        for (int sgn = -1; sgn <= 1; sgn += 2)
            addBox(ctx, ax + kConvX + sgn * 1.05f, y0 + 0.78f, az,
                   0.12f, 0.14f, hz, 0.0f, &sBrass, kBrassTint);
        for (int i = 0; i < 4; ++i)
            addBox(ctx, ax + kConvX, y0 + 0.35f, az - 6.0f + (float)i * 4.0f,
                   0.9f, 0.35f, 0.18f, 0.0f, &sIron, kDarkIron);
        physBox(ax + kConvX, y0 + 0.45f, az, 1.15f, 0.45f, hz);
    }

    // ---- PROP SPAN: 8 machine movers, 24 slats, 8 gizmos (contiguous) -------
    room.propEntFirst = s.size();
    // 0 gum piston plate (mint-lit from the glow strip above)
    addBox(ctx, ax + kInvPos[0][0], y0 + 1.6f, az + kInvPos[0][1],
           1.4f, 0.15f, 1.0f, 0.0f, &sBrass, kBrassTint, 0.0f, nullptr, true);
    // 1 fizz rotor
    addBox(ctx, ax + kInvPos[1][0], y0 + 3.9f, az + kInvPos[1][1],
           1.5f, 0.10f, 0.26f, 0.0f, &sBrass, kBrassTint, 0.0f, nullptr, true);
    // 2 bellows block (squash pose: tick rewrites scaleY + re-anchors)
    addBox(ctx, ax + kInvPos[2][0], y0 + 1.5f, az + kInvPos[2][1],
           0.9f, 1.0f, 0.9f, 0.0f, &sCop, kCopperTint, 0.0f, nullptr, true);
    // 3 sprocket (spin + bob)
    addBox(ctx, ax + kInvPos[3][0], y0 + 4.0f, az + kInvPos[3][1],
           1.3f, 0.08f, 0.4f, 0.0f, &sBrass, kBrassTint, 0.0f, nullptr, true);
    // 4 wobble boiler tank (the machine IS the mover)
    addBox(ctx, ax + kInvPos[4][0], y0 + 2.4f, az + kInvPos[4][1],
           2.0f, 2.0f, 2.0f, 0.0f, &sCop, kCopperTint, 0.0f, nullptr, true);
    // 5 organ metronome wand
    addBox(ctx, ax + kInvPos[5][0], y0 + 2.1f, az + kInvPos[5][1],
           0.12f, 0.45f, 0.12f, 0.0f, &sBrass, kBrassTint, 0.0f, nullptr, true);
    // 6 centrifuge arm (fast)
    addBox(ctx, ax + kInvPos[6][0], y0 + 1.35f, az + kInvPos[6][1],
           2.1f, 0.10f, 0.30f, 0.0f, &sBrass, kBrassTint, 0.0f, nullptr, true);
    // 7 maybe-machine beacon (the flicker; emissive poked in tick)
    addBox(ctx, ax + kInvPos[7][0], y0 + 7.3f, az + kInvPos[7][1],
           0.32f, 0.32f, 0.32f, 0.0f, nullptr, kGold, 0.4f, kGold, true);
    // Conveyor slats (scroll along Z; wrap at the belt end).
    for (int i = 0; i < kConvSlats; ++i) {
        const float z0 = -kConvLen * 0.5f + ((float)i + 0.5f) * (kConvLen / kConvSlats);
        addBox(ctx, ax + kConvX, y0 + kConvTopY, az + z0,
               0.92f, 0.05f, 0.24f, 0.0f, &sIron, kDarkIron, 0.0f, nullptr, true);
    }
    // Gizmo cubes riding the belt (emissive whatsits — mint/amber alternating).
    for (int i = 0; i < kConvGizmos; ++i) {
        const float z0 = -kConvLen * 0.5f + ((float)i + 0.5f) * (kConvLen / kConvGizmos);
        addGlow(ctx, ax + kConvX, y0 + kConvTopY + 0.34f, az + z0,
                0.26f, 0.26f, 0.26f, (float)i * 0.7f,
                (i & 1) ? kAmber : kMint, 1.0f);
    }
    room.propEntCount = s.size() - room.propEntFirst;

    // ---- GLOW SPAN: 5 machine studs, 8 organ keys, centrifuge, maybe-panel --
    room.glowEntFirst = s.size();
    addGlow(ctx, ax + kInvPos[0][0], y0 + 3.25f, az + kInvPos[0][1],
            1.6f, 0.06f, 0.10f, 0.0f, kMint, 1.6f);                      // 0 gum
    addGlow(ctx, ax + kInvPos[1][0], y0 + 2.55f, az + kInvPos[1][1],
            1.35f, 0.07f, 0.07f, 0.0f, kAmber, 1.0f);                    // 1 fizz
    addGlow(ctx, ax + kInvPos[2][0], y0 + 3.35f, az + kInvPos[2][1],
            0.7f, 0.06f, 0.7f, 0.6f, kViolet, 1.6f);                     // 2 bellows
    addGlow(ctx, ax + kInvPos[3][0], y0 + 5.35f, az + kInvPos[3][1],
            0.3f, 0.3f, 0.3f, 0.785f, kBrassGlow, 0.6f);                 // 3 sprocket
    addGlow(ctx, ax + kInvPos[4][0], y0 + 4.55f, az + kInvPos[4][1],
            1.2f, 0.07f, 1.2f, 0.0f, kRasp, 1.1f);                       // 4 boiler
    for (int k = 0; k < 8; ++k)                                          // 5..12 keys
        addGlow(ctx, ax + kInvPos[5][0] - 1.06f, y0 + 1.15f,
                az + kInvPos[5][1] - 2.45f + (float)k * 0.7f,
                0.05f, 0.16f, 0.28f, 0.0f, kWhite, 0.35f);
    addGlow(ctx, ax + kInvPos[6][0], y0 + 1.06f, az + kInvPos[6][1],
            2.25f, 0.05f, 2.25f, 0.785f, kCyan, 1.5f);                   // 13 centrifuge
    addGlow(ctx, ax + kInvPos[7][0], y0 + 4.2f, az + kInvPos[7][1] - 1.02f,
            0.6f, 1.4f, 0.05f, 0.3f, kGold, 0.5f);                       // 14 maybe
    room.glowEntCount = s.size() - room.glowEntFirst;
}

void tickRoomInvention(Scene& scene, AnnexRoom& room, float t) {
    if (room.propEntCount == 0) return;
    const float y0 = room.baseY, az = room.centerZ;
    const uint32_t p0 = room.propEntFirst;
    // 0 Gum-Stretcher piston: Y bob, amp 0.9 @ 1.4 Hz.
    scene.get(p0 + 0).transform[13] = y0 + 1.6f + 0.9f * std::sin(t * (1.4f * kTwoPi));
    // 1 Fizz Compressor rotor: 2.2 rad/s.
    pokeYaw(scene.get(p0 + 1).transform, t * 2.2f);
    // 2 Idea Bellows: scaleY squash 0.7..1.15 @ 0.5 Hz, bottom anchored.
    {
        Entity& e = scene.get(p0 + 2);
        const float sc = 0.925f + 0.225f * std::sin(t * (0.5f * kTwoPi));
        e.transform[5]  = sc;                       // basis Y column scale
        e.transform[13] = y0 + 0.5f + sc * 1.0f;    // bottom stays on the plinth
    }
    // 3 Sprocket Fountain: spin 1.1 rad/s + bob 0.4 m.
    {
        Entity& e = scene.get(p0 + 3);
        pokeYaw(e.transform, t * 1.1f);
        e.transform[13] = y0 + 4.0f + 0.4f * std::sin(t * (0.8f * kTwoPi) * 0.5f);
    }
    // 4 Wobble Boiler: roll (about Z) +-0.12 rad @ 0.9 Hz.
    {
        Entity& e = scene.get(p0 + 4);
        const float r = 0.12f * std::sin(t * (0.9f * kTwoPi));
        const float c = std::cos(r), sn = std::sin(r);
        float* m = e.transform;
        m[0] = c;  m[1] = sn; m[2]  = 0;
        m[4] = -sn; m[5] = c; m[6]  = 0;
        m[8] = 0;  m[9] = 0;  m[10] = 1;
    }
    // 5 Button Organ wand: a gentle conductor bob.
    scene.get(p0 + 5).transform[13] = y0 + 2.1f + 0.15f * std::sin(t * 3.0f);
    // 6 Notion Centrifuge: 4.0 rad/s.
    pokeYaw(scene.get(p0 + 6).transform, t * 4.0f);
    // 7 The Maybe Machine beacon: random flicker (the plan's t*13.7 hash).
    {
        Entity& e = scene.get(p0 + 7);
        const float h = std::fabs(std::sin(std::floor(t * 13.7f) * 12.9898f));
        e.emissive[3] = 0.25f + 1.05f * h * h;
        pokeYaw(e.transform, t * 0.6f);
    }
    // Conveyor slats + gizmos: pose-scroll at 1.2 m/s, wrapping at the ends.
    for (int i = 0; i < kConvSlats; ++i) {
        const float z0 = ((float)i + 0.5f) * (kConvLen / kConvSlats);
        scene.get(p0 + 8 + i).transform[14] =
            az - kConvLen * 0.5f + std::fmod(z0 + kConvSpeed * t, kConvLen);
    }
    for (int i = 0; i < kConvGizmos; ++i) {
        Entity& e = scene.get(p0 + 8 + kConvSlats + i);
        const float z0 = ((float)i + 0.5f) * (kConvLen / kConvGizmos);
        e.transform[14] = az - kConvLen * 0.5f + std::fmod(z0 + kConvSpeed * t, kConvLen);
        pokeYaw(e.transform, t * 1.3f + (float)i * 0.7f);
    }
    // Glow span: machine studs breathe; the organ keys CHASE (0.12 s step);
    // the maybe-panel flickers with its beacon.
    const uint32_t g0 = room.glowEntFirst;
    const float breathe[5] = { 1.6f, 1.0f, 1.6f, 0.6f, 1.1f };
    for (int i = 0; i < 5; ++i)
        scene.get(g0 + i).emissive[3] =
            breathe[i] * (0.8f + 0.25f * std::sin(t * 0.9f + (float)i * 1.2f));
    {
        const int lit = (int)(t / 0.12f) % 8;
        for (int k = 0; k < 8; ++k)
            scene.get(g0 + 5 + (uint32_t)k).emissive[3] = (k == lit) ? 2.4f : 0.30f;
    }
    scene.get(g0 + 13).emissive[3] = 1.5f * (0.8f + 0.25f * std::sin(t * 1.7f));
    {
        const float h = std::fabs(std::sin(std::floor(t * 13.7f) * 12.9898f));
        scene.get(g0 + 14).emissive[3] = 0.2f + 0.8f * h;
    }
}

// ============================================================================
// FLOOR C (y=28) — THE FIZZ GALLERY (Task 9)
// ============================================================================
// Four glass bubble columns (2 m dia, floor-to-ceiling) at local (+-11, +-11),
// each with 10 rising emissive bubbles (mesh_prims HAS a real sphere —
// makeUVSphere — so the plan's 6-box fallback is not needed; ONE shared unit
// sphere, instanced 40x, per-bubble radius via basis scale). Bubble motion is
// DETERMINISTIC in t (y = fmod(rise), XZ sine wobble), so tick() rebuilds each
// transform from formula — no per-bubble state, no heap. Three 3-blade ceiling
// fans at 0.6 rad/s. The low-grav zone is trigger 311 (Phase-2 shell) — the
// host scales jump x1.8 while inside (verified wired in host_factory.cpp).
constexpr float kFizzColPos[4][2] = {
    { -11.0f, -11.0f }, { 11.0f, -11.0f }, { -11.0f, 11.0f }, { 11.0f, 11.0f },
};
constexpr float kFizzFanPos[3][2] = { { 0.0f, -12.0f }, { -11.0f, 5.0f }, { 11.0f, -3.0f } };
constexpr int   kFizzBubbles = 10;          // per column
constexpr float kFizzRise    = 1.1f;        // m/s (plan)
constexpr float kFizzSpan    = 10.0f;       // rise span before the ceiling wrap
constexpr float kFizzFanY    = 10.3f;       // fan hub above baseY
// Bubble radius: deterministic per bubble index (tick re-derives it when it
// rebuilds the scaled basis).
inline float fizzBubbleR(int j) { return 0.13f + 0.045f * (float)(j % 5); }

void buildRoomFizz(FactoryRoomCtx& ctx, AnnexRoom& room) {
    Scene& s = ctx.scene;
    const float ax = ctx.centerX, az = ctx.centerZ, y0 = room.baseY;
    const SurfaceSet& sBrass = ctx.surf.get(ctx.device, "mw_metal_trim_b");
    const SurfaceSet& sIron  = ctx.surf.get(ctx.device, "mw_metal_panels_a");

    // ---- Glass columns (the real glass pipeline, near-clear) + base plinths.
    for (int c = 0; c < 4; ++c) {
        const float cx = ax + kFizzColPos[c][0], cz = az + kFizzColPos[c][1];
        x3::prims::PrimMesh pm = x3::prims::makeCylinder(1.0f, 1.0f, 5.4f, 22);
        Entity e;
        e.mesh = ctx.device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                       pm.index.data(), (uint32_t)pm.index.size());
        ctx.meshes.push_back(e.mesh);
        e.baseColor[0] = 0.72f; e.baseColor[1] = 0.88f; e.baseColor[2] = 0.94f;
        e.baseColor[3] = 1.0f;
        e.transparent = true;
        e.glass.opacity    = 0.05f;    // the bubbles must READ through it
        e.glass.refraction = 0.010f;
        e.glass.roughness  = 0.05f;
        e.glass.specular   = 0.6f;
        e.glass.tint[0] = 0.72f; e.glass.tint[1] = 0.88f; e.glass.tint[2] = 0.94f;
        e.tag = (uint32_t)Tag::Static;
        yawXform(e.transform, 0.0f, cx, y0 + 5.6f, cz);
        s.add(e);
        // Base plinth + top collar (brass), physics around the column.
        addBox(ctx, cx, y0 + 0.25f, cz, 1.25f, 0.25f, 1.25f, 0.0f, &sIron, kDarkIron);
        addBox(ctx, cx, y0 + 10.85f, cz, 1.2f, 0.15f, 1.2f, 0.0f, &sBrass, kBrassTint);
        ctx.physics.addBox({ 1.05f, 5.5f, 1.05f }, { cx, y0 + 5.5f, cz }, 0.0f,
                           x3::phys::Layer::Static);
    }
    // ---- Fan hubs + drop rods (static; the blades are the animated span).
    for (int f = 0; f < 3; ++f) {
        const float fx = ax + kFizzFanPos[f][0], fz = az + kFizzFanPos[f][1];
        addBox(ctx, fx, y0 + kFizzFanY + 0.45f, fz, 0.10f, 0.35f, 0.10f, 0.0f,
               &sBrass, kBrassTint);
        addBox(ctx, fx, y0 + kFizzFanY, fz, 0.28f, 0.14f, 0.28f, 0.0f,
               &sBrass, kBrassTint, 0.30f, kBrassGlow);
    }

    // ---- PROP SPAN: 40 bubbles (ONE shared sphere, scaled per instance),
    // then 9 fan blades. Contiguous.
    x3::prims::PrimMesh sph = x3::prims::makeUVSphere(10, 14);   // small: 40 instances
    x3::rhi::MeshHandle sphMesh = ctx.device.createMesh(
        sph.verts.data(), (uint32_t)sph.verts.size(),
        sph.index.data(), (uint32_t)sph.index.size());
    ctx.meshes.push_back(sphMesh);   // shared: pushed ONCE
    room.propEntFirst = s.size();
    for (int c = 0; c < 4; ++c) {
        const float cx = ax + kFizzColPos[c][0], cz = az + kFizzColPos[c][1];
        for (int j = 0; j < kFizzBubbles; ++j) {
            const float r = fizzBubbleR(j);
            const float xA[3] = { r, 0, 0 }, yA[3] = { 0, r, 0 }, zA[3] = { 0, 0, r };
            const float by = y0 + 0.6f + ((float)j / kFizzBubbles) * kFizzSpan;
            const bool white = (j % 3) == 0;
            const float colW[3] = { 1.0f, 0.97f, 0.90f };
            addShared(ctx, sphMesh, xA, yA, zA, cx, by, cz,
                      white ? colW : room.accent, white ? 1.3f : 1.0f, nullptr);
        }
    }
    for (int f = 0; f < 3; ++f) {
        const float fx = ax + kFizzFanPos[f][0], fz = az + kFizzFanPos[f][1];
        for (int b = 0; b < 3; ++b) {
            // Blade authored with its mesh OFFSET from the origin (the hub):
            // rotating the transform sweeps it around the hub.
            x3::prims::PrimMesh pm = x3::prims::makeBox(1.1f, 0.04f, 0.32f,
                                                        1.65f, 0.0f, 0.0f, 0.35f);
            Entity e;
            e.mesh = ctx.device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                           pm.index.data(), (uint32_t)pm.index.size());
            ctx.meshes.push_back(e.mesh);
            if (sBrass.ok) { e.tex = sBrass.albedo; e.normalTex = sBrass.normal; e.mrTex = sBrass.mr; }
            e.baseColor[0] = kBrassTint[0]; e.baseColor[1] = kBrassTint[1];
            e.baseColor[2] = kBrassTint[2]; e.baseColor[3] = 1.0f;
            e.tag = (uint32_t)Tag::Prop;
            yawXform(e.transform, (float)b * (kTwoPi / 3.0f), fx, y0 + kFizzFanY, fz);
            s.add(e);
        }
    }
    room.propEntCount = s.size() - room.propEntFirst;

    // ---- GLOW SPAN: one amber fizz collar per column base ------------------
    room.glowEntFirst = s.size();
    for (int c = 0; c < 4; ++c)
        addGlow(ctx, ax + kFizzColPos[c][0], y0 + 0.62f, az + kFizzColPos[c][1],
                1.15f, 0.10f, 1.15f, 0.0f, room.accent, 1.2f);
    room.glowEntCount = s.size() - room.glowEntFirst;
}

void tickRoomFizz(Scene& scene, AnnexRoom& room, float t) {
    if (room.propEntCount == 0) return;
    const float ax = room.centerX, az = room.centerZ, y0 = room.baseY;
    // Bubbles: rise at 1.1 m/s, wrap at the ceiling, gentle XZ wobble. Fully
    // deterministic in t — the transform is REBUILT from formula each tick
    // (scaled basis re-derived from the per-index radius).
    for (int c = 0; c < 4; ++c) {
        const float cx = ax + kFizzColPos[c][0], cz = az + kFizzColPos[c][1];
        for (int j = 0; j < kFizzBubbles; ++j) {
            Entity& e = scene.get(room.propEntFirst + (uint32_t)(c * kFizzBubbles + j));
            const float r  = fizzBubbleR(j);
            const float ph = (float)j * 1.7f + (float)c * 0.9f;
            const float by = y0 + 0.6f + std::fmod(
                ((float)j / kFizzBubbles) * kFizzSpan + kFizzRise * t, kFizzSpan);
            const float wx = 0.22f * std::sin(t * 1.3f + ph);
            const float wz = 0.22f * std::cos(t * 1.1f + ph * 1.3f);
            const float xA[3] = { r, 0, 0 }, yA[3] = { 0, r, 0 }, zA[3] = { 0, 0, r };
            makeXform(e.transform, xA, yA, zA, cx + wx, by, cz + wz);
        }
    }
    // Fans: 0.6 rad/s, blades 120 degrees apart (rotation-basis poke).
    for (int f = 0; f < 3; ++f)
        for (int b = 0; b < 3; ++b)
            pokeYaw(scene.get(room.propEntFirst + 40u + (uint32_t)(f * 3 + b)).transform,
                    t * 0.6f + (float)b * (kTwoPi / 3.0f) + (float)f * 0.5f);
    // Collars: a slow amber fizz-breathe.
    for (uint32_t i = 0; i < room.glowEntCount; ++i)
        scene.get(room.glowEntFirst + i).emissive[3] =
            1.0f + 0.4f * std::sin(t * 0.7f + (float)i * 1.4f);
}

// ============================================================================
// FLOOR D (y=41) — THE SORTING HALL (Task 10): lands with its task.
// ============================================================================
void buildRoomSorting  (FactoryRoomCtx& ctx, AnnexRoom& room) { (void)ctx; (void)room; }
void tickRoomSorting   (Scene& scene, AnnexRoom& room, float t) { (void)scene; (void)room; (void)t; }

// ============================================================================
// FLOOR E (y=54) — THE TUBE JUNCTION (Task 11): lands with its task.
// ============================================================================
void buildRoomTube     (FactoryRoomCtx& ctx, AnnexRoom& room) { (void)ctx; (void)room; }
void tickRoomTube      (Scene& scene, AnnexRoom& room, float t) { (void)scene; (void)room; (void)t; }

} // namespace x3::game
