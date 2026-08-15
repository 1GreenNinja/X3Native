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
// FLOOR B (y=15) — THE INVENTION WORKS (Task 8): lands with its task.
// ============================================================================
void buildRoomInvention(FactoryRoomCtx& ctx, AnnexRoom& room) { (void)ctx; (void)room; }
void tickRoomInvention (Scene& scene, AnnexRoom& room, float t) { (void)scene; (void)room; (void)t; }

// ============================================================================
// FLOOR C (y=28) — THE FIZZ GALLERY (Task 9): lands with its task.
// ============================================================================
void buildRoomFizz     (FactoryRoomCtx& ctx, AnnexRoom& room) { (void)ctx; (void)room; }
void tickRoomFizz      (Scene& scene, AnnexRoom& room, float t) { (void)scene; (void)room; (void)t; }

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
