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

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace x3::game {

namespace {

constexpr float kTwoPi = 6.2831853f;

// ---- Palette (matches factory_annex.cpp's shell constants) -----------------
// Phase 5: these are MULTIPLIERS over curated fa_* albedos now. fa_copper_aged
// already carries its orange patina in the albedo, so the copper tint is a
// light warm lift (the old 0.74/0.40/0.26 flat tint over it went mud);
// fa_brass_worn is a neutral bright metal, so the brass hue stays in the tint.
constexpr float kCopperTint[3] = { 1.00f, 0.88f, 0.78f };   // over fa_copper_aged
constexpr float kBrassTint[3]  = { 0.69f, 0.55f, 0.34f };   // brass #b08d57 over fa_brass_worn
constexpr float kBrassGlow[3]  = { 1.00f, 0.78f, 0.42f };   // warm brass emissive
constexpr float kDarkIron[3]   = { 0.15f, 0.13f, 0.18f };   // aubergine iron (dark furniture)
constexpr float kEnamelTint[3] = { 1.00f, 0.94f, 0.80f };   // cream over fa_enamel_cream
constexpr float kWoodTint[3]   = { 0.95f, 0.85f, 0.74f };   // warm over fa_wood_planks
constexpr float kArchTint[3]   = { 1.00f, 0.82f, 0.62f };   // Glimvale arch: grey stone -> sandstone

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
    e.baseColor[0] = glow[0] * 0.14f; e.baseColor[1] = glow[1] * 0.14f;
    e.baseColor[2] = glow[2] * 0.14f; e.baseColor[3] = 1.0f;   // darker body: the
    // stud reads as a lit jewel on the metal, not a pastel taffy chunk
    e.emissive[0] = glow[0]; e.emissive[1] = glow[1]; e.emissive[2] = glow[2];
    e.emissive[3] = emStrength;
    e.tag = (uint32_t)Tag::Prop;
    // xA = tangent, zA = -radial so xA x yA = zA (det +1).
    const float xA[3] = { -s, 0, c }, yA[3] = { 0, 1, 0 }, zA[3] = { -c, 0, -s };
    makeXform(e.transform, xA, yA, zA,
              ringX + radius * c, y, ringZ + radius * s);
    return ctx.scene.add(e);
}

// ============================================================================
// Phase-5 GLB authoring (Glimvale dressing + StarForge hero hooks). One
// build-lifetime drawable cache per annex build (lives on FactoryArtHooks via
// the annex's m_models registry + this TU-local cache keyed per build) — each
// GLB is loaded ONCE, then instanced per placement; a missing file is recorded
// once so 30 flower placements on an asset-less clone log one warn, not 30.
// ============================================================================
struct GlbCacheEntry {
    std::string path;
    bool ok = false;
    std::vector<x3::asset::ModelDrawable> draws;
};
// Cleared by FactoryAnnex::build via factoryGlbCacheReset() so a rebuild
// (self-test F15 builds twice) starts fresh — the cached meshIds are only
// valid for the build whose Models are still loaded.
std::vector<GlbCacheEntry> g_glbCache;

const GlbCacheEntry& glbLookup(FactoryRoomCtx& ctx, const char* relPath) {
    for (const GlbCacheEntry& e : g_glbCache)
        if (e.path == relPath) return e;
    GlbCacheEntry e;
    e.path = relPath;
    if (ctx.art && ctx.art->loader) {
        x3::asset::Model m = ctx.art->loader->load(relPath);
        if (m.ok) {
            e.draws = x3::asset::makeDrawables(m);
            e.ok = !e.draws.empty();
            ctx.art->models->push_back(std::move(m));   // annex unloads at shutdown
        }
    }
    g_glbCache.push_back(std::move(e));
    return g_glbCache.back();
}

// Author one Scene entity per drawable of `relPath` at obj = T(x,y,z) * R_y(yaw)
// * S(scale) (each drawable's baked nodeTransform rides on top — the M2 loader
// convention). Loader-owned meshes are NOT pushed into ctx.meshes; the annex
// unloads the Models instead. Returns the entity count (0 = missing/failed).
// When outXf != null the per-drawable node transforms are appended (16 floats
// each) so tick() can re-pose animated heroes: entity = liveObj * outXf[i].
int addModelEntities(FactoryRoomCtx& ctx, const char* relPath,
                     float x, float y, float z, float yaw, float scale,
                     bool isProp, std::vector<float>* outXf = nullptr,
                     const float* tint = nullptr) {
    const GlbCacheEntry& g = glbLookup(ctx, relPath);
    if (!g.ok) return 0;
    const float c = std::cos(yaw), sn = std::sin(yaw), s = scale;
    const float obj[16] = { c*s, 0, -sn*s, 0,  0, s, 0, 0,  sn*s, 0, c*s, 0,
                            x, y, z, 1 };
    int n = 0;
    for (const auto& d : g.draws) {
        if (!d.meshId) continue;
        Entity e;
        e.mesh        = x3::rhi::MeshHandle{ d.meshId };
        e.tex         = x3::rhi::TextureHandle{ d.baseColorTexId };
        e.normalTex   = x3::rhi::TextureHandle{ d.normalTexId };
        e.mrTex       = x3::rhi::TextureHandle{ d.mrTexId };
        e.emissiveTex = x3::rhi::TextureHandle{ d.emissiveTexId };
        for (int i = 0; i < 4; ++i) e.baseColor[i] = d.baseColorFactor[i];
        if (tint) {   // hue nudge over the authored texture (e.g. the grey
                      // stone arch warmed toward sandstone)
            e.baseColor[0] *= tint[0]; e.baseColor[1] *= tint[1];
            e.baseColor[2] *= tint[2];
        }
        e.alphaBlend = d.alphaBlend;
        e.tag = (uint32_t)(isProp ? Tag::Prop : Tag::Static);
        x3::asset::mulMat4(obj, d.nodeTransform, e.transform);
        ctx.scene.add(e);
        if (outXf) outXf->insert(outXf->end(), d.nodeTransform, d.nodeTransform + 16);
        ++n;
    }
    return n;
}

// Glimvale set dressing (tallied, never warns per-placement — an asset-less
// clone builds clean with dressEntities == 0).
int dress(FactoryRoomCtx& ctx, const char* relPath,
          float x, float y, float z, float yaw = 0.0f, float scale = 1.0f,
          const float* tint = nullptr) {
    const int n = addModelEntities(ctx, relPath, x, y, z, yaw, scale, false,
                                   nullptr, tint);
    if (ctx.art) ctx.art->dressEntities += (uint32_t)n;
    return n;
}

// A little deterministic hash jitter for organic dressing (flowers/grass) —
// no RNG dependency, stable across builds (deterministic captures).
inline float jit(int i, int salt) {
    const float v = std::sin((float)(i * 37 + salt * 101) * 12.9898f) * 43758.547f;
    return v - std::floor(v);   // [0,1)
}

} // namespace

// Called by FactoryAnnex::build() before the room hooks run: the drawable
// cache from a previous build points at unloaded Models' mesh ids.
void factoryGlbCacheReset() { g_glbCache.clear(); }

// STARFORGE HERO HOOK (load-if-present; the barrels.cpp Barrel.glb pattern).
// Returns >0 (entities authored, heroPresent++) when the forge has delivered
// the GLB; 0 (heroFallback++, LOG-WARN with the exact probed path so the forge
// knows the contract) when the caller must build its procedural fallback.
// When the forge delivers, the hero auto-appears on rebuild — zero code change.
// EXTERNAL linkage: the self-test (factory_annex.cpp F16) probes both branches
// directly.
int heroHook(FactoryRoomCtx& ctx, const char* relPath, const char* what,
             float x, float y, float z, float yaw, float scale,
             bool isProp, std::vector<float>* outXf = nullptr) {
    const int n = addModelEntities(ctx, relPath, x, y, z, yaw, scale, isProp, outXf);
    if (!ctx.art) return n;
    if (n > 0) {
        ctx.art->heroPresent += 1;
        char b[192];
        std::snprintf(b, sizeof(b), "[factory] hero %s <- %s (%d prim entities)",
                      what, relPath, n);
        x3::logInfo(b);
    } else {
        ctx.art->heroFallback += 1;
        // Once per path per build the loader remembers the miss, but the WARN
        // fires per hook site so the forge sees every open slot.
        x3::logWarn(std::string("[factory] hero ") + what + " not delivered (" +
                    relPath + ") — procedural fallback stands in");
    }
    return n;
}

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
    const SurfaceSet& sCopper = ctx.surf.get(ctx.device, "fa_copper_aged");
    const SurfaceSet& sBrass  = ctx.surf.get(ctx.device, "fa_brass_worn");
    const SurfaceSet& sIron   = ctx.surf.get(ctx.device, "mw_metal_panels_a");
    const SurfaceSet& sWood   = ctx.surf.get(ctx.device, "fa_wood_planks");

    const float rx0 = ax + FactoryAnnex::kRiverX0, rx1 = ax + FactoryAnnex::kRiverX1;
    const float rcx = (rx0 + rx1) * 0.5f, rhx = (rx1 - rx0) * 0.5f;
    const float bedY = FactoryAnnex::kRiverBedY;                  // 0.65

    // ---- The river channel: bed + banks + end culverts (all physical) ------
    // Bed: a full-channel slab whose top is the wading floor (water is ~0.9 m
    // deep — you can fall in, and you can climb back out on the east steps).
    // Phase 5: the bed is CHOCOLATE — under a metre of raspberry syrup the
    // old dark-iron tint read as a black pit.
    constexpr float kCocoa[3] = { 0.34f, 0.19f, 0.13f };
    addBox(ctx, rcx, bedY - 0.30f, az, rhx, 0.30f, 20.0f, 0.0f, &sIron, kCocoa);
    ctx.physics.addBox({ rhx, 0.30f, 20.0f }, { rcx, bedY - 0.30f, az },
                       0.0f, x3::phys::Layer::Static);
    // Banks: bed-to-slab walls on both sides (the slab itself covers 1.5..2.0).
    for (int sgn = -1; sgn <= 1; sgn += 2) {
        const float bx = (sgn < 0) ? rx0 - 0.15f : rx1 + 0.15f;
        const float hy = (1.5f - bedY) * 0.5f, cy = (1.5f + bedY) * 0.5f;
        addBox(ctx, bx, cy, az, 0.15f, hy, 20.0f, 0.0f, &sIron, kCocoa);
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

    // ---- Two footbridges over the river (z = -10 / +10): DARK WOOD PLANK
    // decks (Phase 5 art direction) on three stepped boxes each (0.3 m risers
    // — walkable without ramp physics) + brass rails.
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
                   &sWood, kWoodTint, 0.0f, nullptr, false, 0.45f);
            ctx.physics.addBox({ d.hx, 0.15f, 1.3f }, { d.cx, d.topY - 0.15f, bz },
                               0.0f, x3::phys::Layer::Static);
        }
        // Brass rails: worn-brass PBR posts + a faint warm-glow thread per
        // side (the glow ACCENTS the rail, it is not the rail).
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            addBox(ctx, rcx, 3.35f, bz + sgn * 1.25f, rhx + 1.6f, 0.05f, 0.05f,
                   0.0f, &sBrass, kBrassTint, 0.14f, kBrassGlow);
            for (int p = -1; p <= 1; ++p)   // crown rail posts (feet ON the deck)
                addBox(ctx, rcx + (float)p * 2.0f, 2.975f, bz + sgn * 1.25f,
                       0.045f, 0.375f, 0.045f, 0.0f, &sBrass, kBrassTint);
        }
    }

    // ---- Six copper vats: STARFORGE HERO HOOK first (FactoryProps/
    // CopperVat.glb — real turned-copper body when the forge delivers), else
    // the Phase-3 tangent-box cylinder re-skinned in AGED-COPPER PBR. The
    // physics block, stir arm and rim-stud ring are shared by both branches.
    for (int v = 0; v < 6; ++v) {
        const float vx = ax + kVatPos[v][0], vz = az + kVatPos[v][1];
        const bool hero = heroHook(ctx, "FactoryProps/CopperVat.glb", "CopperVat",
                                   vx, y0, vz, (float)v * 1.047f, 1.0f,
                                   /*isProp*/false) > 0;
        if (!hero) {
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
            // Brass rim collar caps the segment tops (the raw box ends read
            // graybox from the bridges — the collar finishes the silhouette).
            {
                x3::prims::PrimMesh pm = x3::prims::makeTorus(kVatR + 0.02f, 0.09f, 28, 10);
                Entity e;
                e.mesh = ctx.device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                               pm.index.data(), (uint32_t)pm.index.size());
                ctx.meshes.push_back(e.mesh);
                if (sBrass.ok) { e.tex = sBrass.albedo; e.normalTex = sBrass.normal; e.mrTex = sBrass.mr; }
                e.baseColor[0] = kBrassTint[0]; e.baseColor[1] = kBrassTint[1];
                e.baseColor[2] = kBrassTint[2]; e.baseColor[3] = 1.0f;
                e.tag = (uint32_t)Tag::Static;
                const float xA[3] = { 1, 0, 0 }, yA[3] = { 0, 0, -1 }, zA[3] = { 0, 1, 0 };
                makeXform(e.transform, xA, yA, zA, vx, y0 + kVatH, vz);
                s.add(e);
            }
            // Syrup surface just under the rim — raspberry glow WITHIN the vat,
            // restrained (warm <= 0.45; the old 0.55 disc read as a pink light).
            {
                x3::prims::PrimMesh pm = x3::prims::makeCylinder(1.35f, 1.35f, 0.06f, 20);
                Entity e;
                e.mesh = ctx.device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                               pm.index.data(), (uint32_t)pm.index.size());
                ctx.meshes.push_back(e.mesh);
                e.baseColor[0] = room.accent[0] * 0.30f; e.baseColor[1] = room.accent[1] * 0.30f;
                e.baseColor[2] = room.accent[2] * 0.30f; e.baseColor[3] = 1.0f;
                e.emissive[0] = room.accent[0]; e.emissive[1] = room.accent[1];
                e.emissive[2] = room.accent[2]; e.emissive[3] = 0.42f;
                e.tag = (uint32_t)Tag::Static;
                const float xA[3] = { 1, 0, 0 }, yA[3] = { 0, 1, 0 }, zA[3] = { 0, 0, 1 };
                makeXform(e.transform, xA, yA, zA, vx, y0 + kVatH - 0.35f, vz);
                s.add(e);
            }
        }
        // Physics: one square-footprint block per vat (climb-proof, walk-solid).
        ctx.physics.addBox({ kVatR, kVatH * 0.5f, kVatR },
                           { vx, y0 + kVatH * 0.5f, vz }, 0.0f,
                           x3::phys::Layer::Static);
        // Canopy drop-pipe into the vat (aged copper, visual only, overhead).
        addBox(ctx, vx, (y0 + kVatH + 11.3f) * 0.5f, vz,
               0.16f, (11.3f - kVatH) * 0.5f + 0.4f, 0.16f, 0.0f,
               &sCopper, kCopperTint);
    }

    // ---- Pipe canopy: an AGED-COPPER square RINGING the center at y ~+11.5
    // + two outer runs (Phase 5: copper PBR throughout; visual only, overhead).
    {
        const float py = y0 + 9.5f;
        addBox(ctx, ax, py, az + 5.0f, 18.0f, 0.25f, 0.25f, 0.0f, &sCopper, kCopperTint);
        addBox(ctx, ax, py, az - 5.0f, 18.0f, 0.25f, 0.25f, 0.0f, &sCopper, kCopperTint);
        addBox(ctx, ax + 5.0f, py, az, 0.25f, 0.25f, 18.0f, 0.0f, &sCopper, kCopperTint);
        addBox(ctx, ax - 5.0f, py, az, 0.25f, 0.25f, 18.0f, 0.0f, &sCopper, kCopperTint);
        addBox(ctx, ax, py + 0.8f, az + 12.0f, 18.0f, 0.22f, 0.22f, 0.0f, &sCopper, kCopperTint);
        addBox(ctx, ax, py + 0.8f, az - 12.0f, 18.0f, 0.22f, 0.22f, 0.0f, &sCopper, kCopperTint);
        // Brass junction collars where the ring meets the outer runs — pipe
        // hardware, not floating boxes.
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            addBox(ctx, ax + 5.0f, py, az + sgn * 5.0f, 0.34f, 0.34f, 0.34f, 0.0f,
                   &sBrass, kBrassTint);
            addBox(ctx, ax - 5.0f, py, az + sgn * 5.0f, 0.34f, 0.34f, 0.34f, 0.0f,
                   &sBrass, kBrassTint);
        }
    }

    // ---- GLIMVALE DRESSING (Phase 5): the river banks bloom — sunflower
    // stands + blue-flower plants down both banks with low pink/red/yellow
    // blooms sunk into the deck seams (the kit's grass/wheat cards ship as
    // UNTINTED alpha masks — shader-tinted in Unity, black-and-white here —
    // so the garden is built from the PLANTS, which carry a real painted
    // atlas). Crates + a barrel in the north-east work corner, twin lamps at
    // the bridge heads, an arch framing the east walk. All Static, ringing
    // the center per the shell law.
    {
        const char* lowBloom[3] = { "Glimvale/SM_Pink_Flower.glb",
                                    "Glimvale/SM_Red_Flower.glb",
                                    "Glimvale/SM_Yellow_Flower.glb" };
        for (int i = 0; i < 9; ++i) {   // west bank: between the wall and the channel
            const float gz = az - 16.0f + 4.0f * (float)i + 1.6f * jit(i, 1);
            const float gx = rx0 - 0.9f - 1.6f * jit(i, 2);
            dress(ctx, "Glimvale/SM_Blue_Flower.glb", gx, y0, gz, jit(i, 3) * kTwoPi,
                  1.4f + 0.5f * jit(i, 12));
            // Low bloom sunk so its floating head reads as ground cover.
            dress(ctx, lowBloom[i % 3], gx + 0.35f, y0 - 0.11f, gz + 0.25f,
                  jit(i, 4) * kTwoPi, 1.3f);
            if (i % 3 == 1)
                dress(ctx, "Glimvale/SM_Sunflower.glb", gx - 0.4f, y0, gz - 0.5f,
                      jit(i, 5) * kTwoPi);
        }
        for (int i = 0; i < 7; ++i) {   // east bank: along the vat walk
            const float gz = az - 13.0f + 4.3f * (float)i + 1.2f * jit(i, 6);
            const float gx = rx1 + 0.85f + 1.1f * jit(i, 7);
            dress(ctx, "Glimvale/SM_Blue_Flower.glb", gx, y0, gz, jit(i, 8) * kTwoPi,
                  1.3f + 0.5f * jit(i, 13));
            dress(ctx, lowBloom[(i + 1) % 3], gx + 0.3f, y0 - 0.11f, gz - 0.3f,
                  jit(i, 9) * kTwoPi, 1.3f);
        }
        dress(ctx, "Glimvale/SM_Sunflower.glb", rx0 - 0.6f, y0, az - 15.6f, 0.7f);
        dress(ctx, "Glimvale/SM_Sunflower.glb", rx1 + 0.7f, y0, az + 15.8f, 2.4f);
        dress(ctx, "Glimvale/SM_Sunflower.glb", rcx - 2.0f, y0, az + 18.4f, 1.1f);
        dress(ctx, "Glimvale/SM_Sunflower.glb", rcx + 2.4f, y0, az - 18.4f, 2.8f);
        // NE work corner: crate stack + barrel (one catch-all physics block).
        dress(ctx, "Glimvale/SM_Stylized_Box_var2.glb", ax + 17.2f, y0 + 0.35f, az + 17.0f, 0.3f);
        dress(ctx, "Glimvale/SM_Stylized_Box_var1.glb", ax + 16.6f, y0 + 0.25f, az + 15.6f, 1.2f);
        dress(ctx, "Glimvale/SM_Stylized_Box.glb",      ax + 17.6f, y0 + 0.70f, az + 16.9f, 0.9f);
        dress(ctx, "Glimvale/SM_Stylized_Barrel.glb",   ax + 15.4f, y0, az + 17.6f, 0.0f);
        ctx.physics.addBox({ 1.6f, 0.6f, 1.5f }, { ax + 16.6f, y0 + 0.6f, az + 16.6f },
                           0.0f, x3::phys::Layer::Static);
        // Bridge-head lamps (east side) + the east-walk arch.
        dress(ctx, "Glimvale/SM_Stylized_Lamp_A.glb", rx1 + 1.6f, y0, az - 10.0f, 1.5708f);
        dress(ctx, "Glimvale/SM_Stylized_Lamp_A.glb", rx1 + 1.6f, y0, az + 10.0f, 1.5708f);
        // The arch's stone atlas is grey — warm it toward sandstone.
        dress(ctx, "Glimvale/SM_Stylized_Arch.glb", ax + 6.0f, y0, az, 0.0f, 0.85f,
              kArchTint);
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
    // River under-glow: 8 NARROW RIBBONS running WITH the flow, staggered
    // across the channel, below the surface — glowing veins IN the syrup.
    // (The Phase-3 full-width slabs read as a pink lightbox under the water —
    // the exact neon-slab failure the Phase-5 art direction bans.)
    constexpr float kRiverGlowCol[3] = { 0.85f, 0.08f, 0.26f };
    for (int i = 0; i < 8; ++i) {
        const float gz = az - 15.5f + 4.5f * (float)i + 1.3f * jit(i, 30);
        const float gx = rcx + ((i & 1) ? 2.1f : -1.9f) + 1.4f * (jit(i, 31) - 0.5f);
        addGlow(ctx, gx, bedY + 0.10f, gz, 0.28f, 0.04f, 2.1f, 0.35f * jit(i, 32),
                kRiverGlowCol, 0.30f);
    }
    room.glowEntCount = s.size() - room.glowEntFirst;

    // ---- THE RIVER WATER (host applies; plan T7 step 2) ---------------------
    if (ctx.river) {
        auto& wp = *ctx.river;
        wp.enabled    = true;
        wp.seaLevel   = FactoryAnnex::kRiverSurfY;
        // NEAR-FLAT heavy syrup: steep Gerstner normals were tilting every
        // facet into the grazing band, so the whole channel MIRRORED the sky
        // and rendered bubblegum — flat syrup keeps the facets face-on, which
        // is where the raspberry-chocolate body color lives.
        wp.amplitude  = 0.022f;
        wp.steepness  = 0.10f;
        wp.waveLength = 5.5f;
        wp.speed      = 0.35f;
        // Phase-5 art direction: deeper raspberry-CHOCOLATE swirl. These are
        // MUCH darker than they look — the water pass sits pre-tonemap and
        // the channel is shallow (85% shallowColor), so ~0.06-linear lands as
        // a deep plum body on screen with cream reflection swirls (probed
        // through green/dark calibration shots; 0.28-linear rendered candy-
        // floss pink).
        wp.deepColor[0]    = 0.028f; wp.deepColor[1]    = 0.008f; wp.deepColor[2]    = 0.006f;
        wp.shallowColor[0] = 0.060f; wp.shallowColor[1] = 0.007f; wp.shallowColor[2] = 0.020f;
        // Sun matches applyAtmosphere's toffee-dusk sun.
        wp.sunDir[0] = -0.25f; wp.sunDir[1] = 0.72f; wp.sunDir[2] = 0.35f;
        wp.specular = 1.8f;
        wp.fresnel  = 0.010f;
        // The far patch fades into COCOA, not the toffee sky — from the glass
        // curtain the outside reads as the confection sea, not milk (the first
        // Phase-5 capture showed the glancing sky-fade washing it white).
        wp.horizonColor[0] = 0.085f; wp.horizonColor[1] = 0.035f; wp.horizonColor[2] = 0.038f;
    }
}

void tickRoomMixture(Scene& scene, AnnexRoom& room, float t) {
    if (room.propEntCount == 0) return;
    // Stir arms: yaw = t*0.8 + i*1.047 (plan numbers) — rotation-basis poke,
    // translation untouched (the arm spins where it stands).
    for (uint32_t i = 0; i < room.propEntCount; ++i)
        pokeYaw(scene.get(room.propEntFirst + i).transform,
                t * 0.8f + (float)i * 1.047f);
    // Rim rings: per-vat/per-stud phased breathe. Phase-5 EMISSIVE RESTRAINT:
    // raspberry is WARM, so the studs hold the <= 0.45 accent band — they are
    // jewelry on the copper rim now, not the light source of the room.
    for (uint32_t i = 0; i < room.glowEntCount; ++i) {
        Entity& e = scene.get(room.glowEntFirst + i);
        if (i < 48) {
            e.emissive[3] = 0.36f + 0.09f * std::sin(t * 1.1f
                              + (float)(i >> 3) * 0.7f + (float)(i & 7) * 0.35f);
        } else {
            // River under-glow: the 0.18 Hz swell, held inside the warm band —
            // a glow WITHIN the syrup, never a lightbox under it.
            e.emissive[3] = 0.34f + 0.10f * std::sin(t * (0.18f * kTwoPi)
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
    const SurfaceSet& sIron   = ctx.surf.get(ctx.device, "fa_iron_wall");
    const SurfaceSet& sBrass  = ctx.surf.get(ctx.device, "fa_brass_worn");
    const SurfaceSet& sCop    = ctx.surf.get(ctx.device, "fa_copper_aged");
    const SurfaceSet& sEnamel = ctx.surf.get(ctx.device, "fa_enamel_cream");
    const SurfaceSet& sRubber = ctx.surf.get(ctx.device, "sr_rubberfloor");
    const SurfaceSet& sCheck  = ctx.surf.get(ctx.device, "fa_tile_checker");
    constexpr float kIronBody[3]   = { 0.42f, 0.37f, 0.48f };   // aubergine over corrugated
    constexpr float kRubberTint[3] = { 1.0f, 1.0f, 1.0f };      // the 4% albedo IS the belt
    auto physBox = [&](float wx, float wy, float wz, float hx, float hy, float hz) {
        ctx.physics.addBox({ hx, hy, hz }, { wx, wy, wz }, 0.0f,
                           x3::phys::Layer::Static);
    };
    // Phase 5: every machine body is a STARFORGE HERO HOOK first (the forge's
    // contraption-body contract: FactoryProps/Machine_<Slug>.glb, metres,
    // origin at the floor contact, -Z facing); the procedural fallback bodies
    // below are re-dressed in the brass/enamel mix per the spec table. Physics
    // + movers + glow studs are shared by both branches.
    const char* kMachineGlb[8] = {
        "FactoryProps/Machine_GumStretcher.glb",
        "FactoryProps/Machine_FizzCompressor.glb",
        "FactoryProps/Machine_IdeaBellows.glb",
        "FactoryProps/Machine_SprocketFountain.glb",
        "FactoryProps/Machine_WobbleBoiler.glb",
        "FactoryProps/Machine_ButtonOrgan.glb",
        "FactoryProps/Machine_NotionCentrifuge.glb",
        "FactoryProps/Machine_MaybeMachine.glb",
    };
    const char* kMachineName[8] = {
        "Gum-Stretcher", "Fizz Compressor", "Idea Bellows", "Sprocket Fountain",
        "Wobble Boiler", "Button Organ", "Notion Centrifuge", "The Maybe Machine",
    };
    bool heroBody[8];
    for (int m = 0; m < 8; ++m)
        heroBody[m] = heroHook(ctx, kMachineGlb[m], kMachineName[m],
                               ax + kInvPos[m][0], y0, az + kInvPos[m][1],
                               0.0f, 1.0f, /*isProp*/false) > 0;

    // ---- Machine bodies (static furniture; the movers land in the prop span).
    // 0 Gum-Stretcher: cream-enamel pillars + brass crown; the piston bobs between.
    if (!heroBody[0]) {
        const float mx = ax + kInvPos[0][0], mz = az + kInvPos[0][1];
        for (int sgn = -1; sgn <= 1; sgn += 2)
            addBox(ctx, mx + sgn * 1.8f, y0 + 1.5f, mz, 0.22f, 1.5f, 0.35f, 0.0f,
                   &sEnamel, kEnamelTint);
        addBox(ctx, mx, y0 + 3.05f, mz, 2.1f, 0.15f, 0.45f, 0.0f, &sBrass, kBrassTint);
    }
    { const float mx = ax + kInvPos[0][0], mz = az + kInvPos[0][1];
      physBox(mx, y0 + 1.5f, mz, 2.0f, 1.5f, 1.5f); }
    // 1 Fizz Compressor: aged-copper drum + brass dome cap; rotor spins above.
    if (!heroBody[1]) {
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
    }
    { const float mx = ax + kInvPos[1][0], mz = az + kInvPos[1][1];
      physBox(mx, y0 + 1.4f, mz, 1.5f, 1.4f, 1.5f); }
    // 2 Idea Bellows: enamel plinth + brass cap; the bellows squashes between.
    if (!heroBody[2]) {
        const float mx = ax + kInvPos[2][0], mz = az + kInvPos[2][1];
        addBox(ctx, mx, y0 + 0.25f, mz, 1.0f, 0.25f, 1.0f, 0.0f, &sEnamel, kEnamelTint);
        addBox(ctx, mx, y0 + 3.6f, mz, 0.8f, 0.12f, 0.8f, 0.6f, &sBrass, kBrassTint);
    }
    { const float mx = ax + kInvPos[2][0], mz = az + kInvPos[2][1];
      physBox(mx, y0 + 1.0f, mz, 1.0f, 1.0f, 1.0f); }
    // 3 Sprocket Fountain: brass fluted column on an enamel base.
    if (!heroBody[3]) {
        const float mx = ax + kInvPos[3][0], mz = az + kInvPos[3][1];
        addBox(ctx, mx, y0 + 2.75f, mz, 0.45f, 2.75f, 0.45f, 0.785f, &sBrass, kBrassTint);
        addBox(ctx, mx, y0 + 0.3f, mz, 1.4f, 0.3f, 1.4f, 0.0f, &sEnamel, kEnamelTint);
    }
    { const float mx = ax + kInvPos[3][0], mz = az + kInvPos[3][1];
      physBox(mx, y0 + 2.75f, mz, 0.7f, 2.75f, 0.7f); }
    // 4 Wobble Boiler: enamel plinth — the 4x4x4 copper tank ITSELF sways.
    if (!heroBody[4]) {
        const float mx = ax + kInvPos[4][0], mz = az + kInvPos[4][1];
        addBox(ctx, mx, y0 + 0.2f, mz, 2.2f, 0.2f, 2.2f, 0.0f, &sEnamel, kEnamelTint);
    }
    { const float mx = ax + kInvPos[4][0], mz = az + kInvPos[4][1];
      physBox(mx, y0 + 2.2f, mz, 2.0f, 2.0f, 2.0f); }
    // 5 Button Organ: cream-enamel console + ranked aged-copper pipes.
    if (!heroBody[5]) {
        const float mx = ax + kInvPos[5][0], mz = az + kInvPos[5][1];
        addBox(ctx, mx, y0 + 0.8f, mz, 1.0f, 0.8f, 3.0f, 0.0f, &sEnamel, kEnamelTint);
        for (int i = 0; i < 5; ++i) {
            const float ph = 0.9f + 0.45f * (float)i;   // ranked pipe heights
            addBox(ctx, mx + 0.55f, y0 + 1.6f + ph * 0.5f, mz - 2.0f + (float)i * 1.0f,
                   0.28f, ph * 0.5f, 0.28f, 0.0f, &sCop, kCopperTint);
        }
    }
    { const float mx = ax + kInvPos[5][0], mz = az + kInvPos[5][1];
      physBox(mx, y0 + 1.2f, mz, 1.2f, 1.2f, 3.2f); }
    // 6 Notion Centrifuge: low wide enamel drum; the arm spins fast above it.
    if (!heroBody[6]) {
        const float mx = ax + kInvPos[6][0], mz = az + kInvPos[6][1];
        x3::prims::PrimMesh pm = x3::prims::makeCylinder(2.2f, 2.2f, 0.5f, 18, 0.3f);
        Entity e;
        e.mesh = ctx.device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                       pm.index.data(), (uint32_t)pm.index.size());
        ctx.meshes.push_back(e.mesh);
        if (sEnamel.ok) { e.tex = sEnamel.albedo; e.normalTex = sEnamel.normal; e.mrTex = sEnamel.mr; }
        e.baseColor[0] = kEnamelTint[0]; e.baseColor[1] = kEnamelTint[1];
        e.baseColor[2] = kEnamelTint[2]; e.baseColor[3] = 1.0f;
        e.tag = (uint32_t)Tag::Static;
        yawXform(e.transform, 0.0f, mx, y0 + 0.5f, mz);
        s.add(e);
    }
    { const float mx = ax + kInvPos[6][0], mz = az + kInvPos[6][1];
      physBox(mx, y0 + 0.5f, mz, 2.2f, 0.5f, 2.2f); }
    // 7 The Maybe Machine: the one DARK cabinet — corrugated aubergine iron
    // with brass edge banding (it might do anything; it says nothing).
    if (!heroBody[7]) {
        const float mx = ax + kInvPos[7][0], mz = az + kInvPos[7][1];
        addBox(ctx, mx, y0 + 3.5f, mz, 1.0f, 3.5f, 1.0f, 0.3f, &sIron, kIronBody);
        addBox(ctx, mx, y0 + 7.05f, mz, 1.08f, 0.06f, 1.08f, 0.3f, &sBrass, kBrassTint);
        addBox(ctx, mx, y0 + 0.08f, mz, 1.08f, 0.08f, 1.08f, 0.3f, &sBrass, kBrassTint);
    }
    { const float mx = ax + kInvPos[7][0], mz = az + kInvPos[7][1];
      physBox(mx, y0 + 3.5f, mz, 1.2f, 3.5f, 1.2f); }

    // ---- Conveyor frame: brass side rails + iron legs + a walk-solid body.
    {
        const float hz = kConvLen * 0.5f + 0.2f;
        for (int sgn = -1; sgn <= 1; sgn += 2)
            addBox(ctx, ax + kConvX + sgn * 1.05f, y0 + 0.78f, az,
                   0.12f, 0.14f, hz, 0.0f, &sBrass, kBrassTint);
        for (int i = 0; i < 4; ++i)
            addBox(ctx, ax + kConvX, y0 + 0.35f, az - 6.0f + (float)i * 4.0f,
                   0.9f, 0.35f, 0.18f, 0.0f, &sIron, kIronBody);
        physBox(ax + kConvX, y0 + 0.45f, az, 1.15f, 0.45f, hz);
    }

    // ---- Checkered-tile INLAY zone (Phase 5): a glazed checker apron under
    // the conveyor + organ walk — a real material change in the floor, clear
    // of the center cylinder, the bore strip and the chute drop column.
    addBox(ctx, ax + 8.0f, y0 + 0.015f, az, 5.5f, 0.015f, 6.5f, 0.0f,
           &sCheck, kWhite, 0.0f, nullptr, false, 0.5f);

    // ---- GLIMVALE DRESSING: the workshop arch over the bore approach (the
    // cab glides in under it), invention-clutter crates in the north corner,
    // and timber wall sections breaking the long east iron wall.
    dress(ctx, "Glimvale/SM_Stylized_Arch.glb", ax - 7.0f, y0, az, 0.0f, 1.15f, kArchTint);
    dress(ctx, "Glimvale/SM_Stylized_Box_var1.glb", ax - 16.4f, y0 + 0.25f, az + 16.4f, 0.5f);
    dress(ctx, "Glimvale/SM_Stylized_Box_var2.glb", ax - 15.2f, y0 + 0.35f, az + 17.2f, 1.9f);
    dress(ctx, "Glimvale/SM_Stylized_Box.glb", ax - 16.8f, y0 + 0.75f, az + 16.6f, 0.2f);
    dress(ctx, "Glimvale/SM_Stylized_Barrel.glb", ax - 17.3f, y0, az + 14.8f, 0.0f);
    ctx.physics.addBox({ 1.6f, 0.6f, 1.6f }, { ax - 16.2f, y0 + 0.6f, az + 16.2f },
                       0.0f, x3::phys::Layer::Static);
    // Timber wall sections along the north/south iron walls, slotted BETWEEN
    // the brass columns (the accent cove owns the east wall). The wall piece's
    // geometry runs local z 0..4 / x -0.35..0; yaw +-pi/2 lays it flush along
    // the +-Z walls with its face into the room.
    dress(ctx, "Glimvale/SM_Large_Wall_01.glb", ax - 13.5f, y0, az - 19.72f, 1.5708f);
    dress(ctx, "Glimvale/SM_Large_Wall_02.glb", ax + 1.5f,  y0, az - 19.72f, 1.5708f);
    dress(ctx, "Glimvale/SM_Large_Wall_01.glb", ax + 5.5f,  y0, az + 19.72f, -1.5708f);

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
           0.9f, 1.0f, 0.9f, 0.0f, &sEnamel, kEnamelTint, 0.0f, nullptr, true);
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
    // Conveyor slats (scroll along Z; wrap at the belt end) — DARK RUBBER
    // (the shooting-range rubber-floor set: 4% albedo + tread relief IS a
    // conveyor belt; Phase-5 art direction "dark rubber-ish surface").
    for (int i = 0; i < kConvSlats; ++i) {
        const float z0 = -kConvLen * 0.5f + ((float)i + 0.5f) * (kConvLen / kConvSlats);
        addBox(ctx, ax + kConvX, y0 + kConvTopY, az + z0,
               0.92f, 0.05f, 0.24f, 0.0f, &sRubber, kRubberTint, 0.0f, nullptr, true);
    }
    // Gizmo cubes riding the belt (emissive whatsits — mint/amber alternating;
    // Phase-5 restraint: amber is warm -> 0.42, mint is cool -> 0.8).
    for (int i = 0; i < kConvGizmos; ++i) {
        const float z0 = -kConvLen * 0.5f + ((float)i + 0.5f) * (kConvLen / kConvGizmos);
        addGlow(ctx, ax + kConvX, y0 + kConvTopY + 0.34f, az + z0,
                0.26f, 0.26f, 0.26f, (float)i * 0.7f,
                (i & 1) ? kAmber : kMint, (i & 1) ? 0.42f : 0.80f);
    }
    room.propEntCount = s.size() - room.propEntFirst;

    // ---- GLOW SPAN: 5 machine studs, 8 organ keys, centrifuge, maybe-panel --
    // Phase-5 restraint: warm accents (amber/brass/raspberry/gold) <= 0.45,
    // cool (mint/violet/cyan/white) <= 0.9 — accents over PBR bodies.
    room.glowEntFirst = s.size();
    addGlow(ctx, ax + kInvPos[0][0], y0 + 3.25f, az + kInvPos[0][1],
            1.6f, 0.06f, 0.10f, 0.0f, kMint, 0.80f);                     // 0 gum
    addGlow(ctx, ax + kInvPos[1][0], y0 + 2.55f, az + kInvPos[1][1],
            1.35f, 0.07f, 0.07f, 0.0f, kAmber, 0.42f);                   // 1 fizz
    addGlow(ctx, ax + kInvPos[2][0], y0 + 3.35f, az + kInvPos[2][1],
            0.7f, 0.06f, 0.7f, 0.6f, kViolet, 0.80f);                    // 2 bellows
    addGlow(ctx, ax + kInvPos[3][0], y0 + 5.35f, az + kInvPos[3][1],
            0.3f, 0.3f, 0.3f, 0.785f, kBrassGlow, 0.40f);                // 3 sprocket
    addGlow(ctx, ax + kInvPos[4][0], y0 + 4.55f, az + kInvPos[4][1],
            1.2f, 0.07f, 1.2f, 0.0f, kRasp, 0.42f);                      // 4 boiler
    for (int k = 0; k < 8; ++k)                                          // 5..12 keys
        addGlow(ctx, ax + kInvPos[5][0] - 1.06f, y0 + 1.15f,
                az + kInvPos[5][1] - 2.45f + (float)k * 0.7f,
                0.05f, 0.16f, 0.28f, 0.0f, kWhite, 0.20f);
    addGlow(ctx, ax + kInvPos[6][0], y0 + 1.06f, az + kInvPos[6][1],
            2.25f, 0.05f, 2.25f, 0.785f, kCyan, 0.80f);                  // 13 centrifuge
    addGlow(ctx, ax + kInvPos[7][0], y0 + 4.2f, az + kInvPos[7][1] - 1.02f,
            0.6f, 1.4f, 0.05f, 0.3f, kGold, 0.35f);                      // 14 maybe
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
        e.emissive[3] = 0.14f + 0.30f * h * h;   // gold is warm: flicker <= ~0.45
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
    // the maybe-panel flickers with its beacon. Phase-5 restraint: steady
    // warm <= 0.45 / cool <= 0.9; only the chase LEAD blinks brighter for a
    // step (a moving highlight, not a lightbox).
    const uint32_t g0 = room.glowEntFirst;
    const float breathe[5] = { 0.80f, 0.42f, 0.80f, 0.40f, 0.42f };
    for (int i = 0; i < 5; ++i)
        scene.get(g0 + i).emissive[3] =
            breathe[i] * (0.8f + 0.2f * std::sin(t * 0.9f + (float)i * 1.2f));
    {
        const int lit = (int)(t / 0.12f) % 8;
        for (int k = 0; k < 8; ++k)
            scene.get(g0 + 5 + (uint32_t)k).emissive[3] = (k == lit) ? 0.85f : 0.16f;
    }
    scene.get(g0 + 13).emissive[3] = 0.80f * (0.8f + 0.2f * std::sin(t * 1.7f));
    {
        const float h = std::fabs(std::sin(std::floor(t * 13.7f) * 12.9898f));
        scene.get(g0 + 14).emissive[3] = 0.10f + 0.32f * h;
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
    const SurfaceSet& sBrass = ctx.surf.get(ctx.device, "fa_brass_worn");
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

    // ---- GLIMVALE GREENERY (Phase 5, sparingly): each column grows a little
    // garden at its plinth foot — grass tufts + one bloom on the checker —
    // and one lounge corner gets a crate + barrel to sit on. The soda garden.
    for (int c = 0; c < 4; ++c) {
        const float cx = ax + kFizzColPos[c][0], cz = az + kFizzColPos[c][1];
        const float px = (kFizzColPos[c][0] > 0) ? -1.55f : 1.55f;   // inner corner
        const float pz = (kFizzColPos[c][1] > 0) ? -1.55f : 1.55f;
        dress(ctx, "Glimvale/SM_Grass_A.glb", cx + px, y0, cz + pz, jit(c, 20) * kTwoPi);
        dress(ctx, "Glimvale/SM_Grass_B.glb", cx + px + 0.5f, y0, cz + pz - 0.4f,
              jit(c, 21) * kTwoPi);
        dress(ctx, (c & 1) ? "Glimvale/SM_Pink_Flower.glb"
                           : "Glimvale/SM_Yellow_Flower.glb",
              cx + px + 0.2f, y0, cz + pz + 0.3f, jit(c, 22) * kTwoPi);
    }
    dress(ctx, "Glimvale/SM_Stylized_Barrel.glb", ax + 17.4f, y0, az - 17.0f, 0.4f);
    dress(ctx, "Glimvale/SM_Stylized_Box_var2.glb", ax + 16.2f, y0 + 0.35f, az - 17.6f, 1.1f);
    dress(ctx, "Glimvale/SM_Sunflower.glb", ax + 18.2f, y0, az - 15.8f, 1.9f);
    ctx.physics.addBox({ 1.4f, 0.55f, 1.2f }, { ax + 16.8f, y0 + 0.55f, az - 17.2f },
                       0.0f, x3::phys::Layer::Static);

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
            // Phase-5 restraint: the old 1.3/1.0 bubbles ACES-clipped to
            // identical cream balls — at 0.85/0.60 the amber ones stay AMBER.
            addShared(ctx, sphMesh, xA, yA, zA, cx, by, cz,
                      white ? colW : room.accent, white ? 0.85f : 0.60f, nullptr);
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

    // ---- GLOW SPAN: one amber fizz collar per column base (warm <= 0.45) ---
    room.glowEntFirst = s.size();
    for (int c = 0; c < 4; ++c)
        addGlow(ctx, ax + kFizzColPos[c][0], y0 + 0.62f, az + kFizzColPos[c][1],
                1.15f, 0.10f, 1.15f, 0.0f, room.accent, 0.40f);
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
    // Collars: a slow amber fizz-breathe held in the warm accent band.
    for (uint32_t i = 0; i < room.glowEntCount; ++i)
        scene.get(room.glowEntFirst + i).emissive[3] =
            0.36f + 0.09f * std::sin(t * 0.7f + (float)i * 1.4f);
}

// ============================================================================
// FLOOR D (y=41) — THE SORTING HALL (Task 10)
// ============================================================================
// A 12-orb ring (10 gold, 2 dull duds) orbiting the center at r=8, 0.5 rad/s,
// two brass sorter arms sweeping +-1.2 rad @ 0.7 Hz over the ring, and the
// CHUTE OF DUBIOUS QUALITY: stand on the 2x2 hatch at local (8,8) — trigger
// 310 — for 1.5 s and it slides open (moved-static physics, the elevator-cab
// technique), dropping you down a GLASS shaft through floors C and B into a
// padded room on Floor A, where a box-serif sign renders its verdict:
// QUALITY: DUBIOUS. (The plan offered font-atlas quads or box letters; box
// letters are cheaper — no atlas dependency.)
constexpr int   kSortOrbs   = 12;
constexpr float kSortOrbR   = 8.0f;       // ring radius (plan)
constexpr float kSortOrbW   = 0.5f;       // rad/s (plan)
constexpr float kSortOrbY   = 1.3f;       // orb height above baseY
constexpr int   kSortDud[2] = { 3, 8 };   // the two dud indices
constexpr float kSortArmPivot[2][2] = { { 0.0f, 11.0f }, { 0.0f, -11.0f } };
constexpr float kHatchSlide = 2.4f;       // hatch open slide distance (+X)
constexpr float kHatchDelay = 1.5f;       // stand-on beat before it opens
constexpr float kHatchOpenT = 0.8f;       // slide duration

// ---- Box-serif stroke font (Task 10's sign; axis-aligned strokes in a unit
// cell, x = width along the sign line, y = height). ORIGINAL glyphs.
struct SignStroke { float cx, cy, hw, hh; };
namespace signfont {
constexpr SignStroke Q[] = { {0.50f,0.93f,0.42f,0.07f}, {0.50f,0.07f,0.42f,0.07f},
                             {0.08f,0.50f,0.08f,0.36f}, {0.92f,0.50f,0.08f,0.36f},
                             {0.78f,0.10f,0.14f,0.10f} };
constexpr SignStroke U[] = { {0.08f,0.55f,0.08f,0.38f}, {0.92f,0.55f,0.08f,0.38f},
                             {0.50f,0.07f,0.42f,0.07f} };
constexpr SignStroke A[] = { {0.08f,0.45f,0.08f,0.45f}, {0.92f,0.45f,0.08f,0.45f},
                             {0.50f,0.93f,0.42f,0.07f}, {0.50f,0.50f,0.34f,0.06f} };
constexpr SignStroke L[] = { {0.08f,0.50f,0.08f,0.43f}, {0.50f,0.07f,0.42f,0.07f} };
constexpr SignStroke I[] = { {0.50f,0.50f,0.08f,0.36f}, {0.50f,0.93f,0.30f,0.07f},
                             {0.50f,0.07f,0.30f,0.07f} };
constexpr SignStroke T[] = { {0.50f,0.93f,0.42f,0.07f}, {0.50f,0.43f,0.08f,0.43f} };
constexpr SignStroke Y[] = { {0.15f,0.78f,0.08f,0.22f}, {0.85f,0.78f,0.08f,0.22f},
                             {0.50f,0.52f,0.36f,0.06f}, {0.50f,0.25f,0.08f,0.25f} };
constexpr SignStroke Colon[] = { {0.50f,0.68f,0.09f,0.09f}, {0.50f,0.28f,0.09f,0.09f} };
constexpr SignStroke D[] = { {0.10f,0.50f,0.08f,0.43f}, {0.38f,0.93f,0.26f,0.07f},
                             {0.38f,0.07f,0.26f,0.07f}, {0.70f,0.50f,0.08f,0.30f} };
constexpr SignStroke B[] = { {0.10f,0.50f,0.08f,0.43f}, {0.50f,0.93f,0.36f,0.07f},
                             {0.50f,0.50f,0.36f,0.06f}, {0.50f,0.07f,0.36f,0.07f},
                             {0.88f,0.50f,0.08f,0.34f} };
constexpr SignStroke O[] = { {0.50f,0.93f,0.42f,0.07f}, {0.50f,0.07f,0.42f,0.07f},
                             {0.08f,0.50f,0.08f,0.36f}, {0.92f,0.50f,0.08f,0.36f} };
constexpr SignStroke S[] = { {0.50f,0.93f,0.42f,0.07f}, {0.50f,0.50f,0.42f,0.06f},
                             {0.50f,0.07f,0.42f,0.07f}, {0.10f,0.72f,0.08f,0.15f},
                             {0.90f,0.28f,0.08f,0.15f} };
struct Glyph { const SignStroke* s; int n; };
inline Glyph glyph(char c) {
    switch (c) {
    case 'Q': return { Q, 5 };  case 'U': return { U, 3 };
    case 'A': return { A, 4 };  case 'L': return { L, 2 };
    case 'I': return { I, 3 };  case 'T': return { T, 2 };
    case 'Y': return { Y, 4 };  case ':': return { Colon, 2 };
    case 'D': return { D, 4 };  case 'B': return { B, 5 };
    case 'O': return { O, 4 };  case 'S': return { S, 5 };
    default:  return { nullptr, 0 };                       // space etc.
    }
}
} // namespace signfont

// Box-serif text on a plane of constant world X, facing -X: for a viewer
// looking +X, screen-right is world +Z, so the line advances +Z (verified on
// the padded-room capture — the -Z variant renders mirrored). Phase 5: the
// letters are ENAMEL-bodied (cream PBR strokes) with a restrained warm glow —
// a painted verdict sign that happens to be lit, not floating neon.
// Returns the stroke count it authored (glow span).
int addSignText(FactoryRoomCtx& ctx, const char* text, float wallX,
                float centerZ, float baseTextY, float cellW, float cellH,
                const float glow[3], float strength) {
    const SurfaceSet& sEn = ctx.surf.get(ctx.device, "fa_enamel_cream");
    int total = 0, n = 0;
    for (const char* p = text; *p; ++p) ++n;
    const float adv   = cellW * 1.18f;
    const float lineW = (float)n * adv - (adv - cellW);
    for (int i = 0; i < n; ++i) {
        const signfont::Glyph g = signfont::glyph(text[i]);
        for (int k = 0; k < g.n; ++k) {
            const SignStroke& st = g.s[k];
            x3::prims::PrimMesh pm = x3::prims::makeBox(0.03f, st.hh * cellH,
                                                        st.hw * cellW, 0, 0, 0);
            Entity e;
            e.mesh = ctx.device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                           pm.index.data(), (uint32_t)pm.index.size());
            ctx.meshes.push_back(e.mesh);
            if (sEn.ok) { e.tex = sEn.albedo; e.normalTex = sEn.normal; e.mrTex = sEn.mr; }
            e.baseColor[0] = kEnamelTint[0]; e.baseColor[1] = kEnamelTint[1];
            e.baseColor[2] = kEnamelTint[2]; e.baseColor[3] = 1.0f;
            e.emissive[0] = glow[0]; e.emissive[1] = glow[1]; e.emissive[2] = glow[2];
            e.emissive[3] = strength;
            e.tag = (uint32_t)Tag::Prop;
            yawXform(e.transform, 0.0f, wallX,
                     baseTextY + st.cy * cellH,
                     centerZ - lineW * 0.5f + ((float)i * adv + st.cx * cellW));
            ++total;
            ctx.scene.add(e);
        }
    }
    return total;
}

void buildRoomSorting(FactoryRoomCtx& ctx, AnnexRoom& room) {
    Scene& s = ctx.scene;
    const float ax = ctx.centerX, az = ctx.centerZ, y0 = room.baseY;   // 41
    const float aY = FactoryAnnex::kFloorBaseY[0];                     // 2 (pad room)
    const float hx = ax + FactoryAnnex::kChuteX, hz = az + FactoryAnnex::kChuteZ;
    const float hole = FactoryAnnex::kChuteHoleHalf;                   // 1.1
    const SurfaceSet& sBrass = ctx.surf.get(ctx.device, "fa_brass_worn");
    const SurfaceSet& sIron  = ctx.surf.get(ctx.device, "mw_metal_panels_a");
    // Phase 5: the padded room wears the Industrial Fabric Pack's strapped
    // bale weave (fa_fabric_pad) — real cloth folds under the verdict sign.
    const SurfaceSet& sPad   = ctx.surf.get(ctx.device, "fa_fabric_pad");
    constexpr float kPadTint[3] = { 1.00f, 0.96f, 0.86f };   // buttercream cloth

    // ---- The orbit track: one flat brass torus under the orb ring ----------
    {
        x3::prims::PrimMesh pm = x3::prims::makeTorus(kSortOrbR, 0.07f, 64, 10);
        Entity e;
        e.mesh = ctx.device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                       pm.index.data(), (uint32_t)pm.index.size());
        ctx.meshes.push_back(e.mesh);
        if (sBrass.ok) { e.tex = sBrass.albedo; e.normalTex = sBrass.normal; e.mrTex = sBrass.mr; }
        e.baseColor[0] = kBrassTint[0]; e.baseColor[1] = kBrassTint[1];
        e.baseColor[2] = kBrassTint[2]; e.baseColor[3] = 1.0f;
        e.emissive[0] = kBrassGlow[0]; e.emissive[1] = kBrassGlow[1];
        e.emissive[2] = kBrassGlow[2]; e.emissive[3] = 0.16f;
        e.tag = (uint32_t)Tag::Static;
        // Torus is authored in XY (hole axis +Z); lay it flat (hole axis +Y).
        const float xA[3] = { 1, 0, 0 }, yA[3] = { 0, 0, -1 }, zA[3] = { 0, 1, 0 };
        makeXform(e.transform, xA, yA, zA, ax, y0 + kSortOrbY - 0.45f, az);
        s.add(e);
    }
    // ---- Sorter arm pivot posts (static; arms are prop span) ---------------
    for (int a = 0; a < 2; ++a) {
        const float px = ax + kSortArmPivot[a][0], pz = az + kSortArmPivot[a][1];
        addBox(ctx, px, y0 + 0.9f, pz, 0.30f, 0.9f, 0.30f, 0.4f, &sBrass, kBrassTint);
        ctx.physics.addBox({ 0.35f, 0.9f, 0.35f }, { px, y0 + 0.9f, pz }, 0.0f,
                           x3::phys::Layer::Static);
    }

    // ---- THE GLASS DROP SHAFT: four near-clear panes INSIDE the chute hole
    // column, floor A up to floor D's slab — falling through the Fizz Gallery
    // and the Invention Works behind glass is the whole joke.
    {
        const float sy0 = aY + 3.0f;             // above the padded room's door
        const float sy1 = y0 - 0.5f;             // floor D slab bottom
        const float cy = (sy0 + sy1) * 0.5f, hy = (sy1 - sy0) * 0.5f;
        const float in = hole - 0.14f;           // panes just inside the hole
        struct Pane { float ox, oz, hx2, hz2; };
        const Pane panes[4] = {
            {  in, 0, 0.06f, in }, { -in, 0, 0.06f, in },
            { 0,  in, in, 0.06f }, { 0, -in, in, 0.06f },
        };
        for (const Pane& p : panes) {
            x3::prims::PrimMesh pm = x3::prims::makeBox(p.hx2, hy, p.hz2, 0, 0, 0);
            Entity e;
            e.mesh = ctx.device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                           pm.index.data(), (uint32_t)pm.index.size());
            ctx.meshes.push_back(e.mesh);
            e.baseColor[0] = 0.72f; e.baseColor[1] = 0.88f; e.baseColor[2] = 0.94f;
            e.baseColor[3] = 1.0f;
            e.transparent = true;
            e.glass.opacity = 0.06f; e.glass.refraction = 0.010f;
            e.glass.roughness = 0.05f; e.glass.specular = 0.6f;
            e.glass.tint[0] = 0.72f; e.glass.tint[1] = 0.88f; e.glass.tint[2] = 0.94f;
            e.tag = (uint32_t)Tag::Static;
            yawXform(e.transform, 0.0f, hx + p.ox, cy, hz + p.oz);
            s.add(e);
            ctx.physics.addBox({ p.hx2, hy, p.hz2 }, { hx + p.ox, cy, hz + p.oz },
                               0.0f, x3::phys::Layer::Static);
        }
    }

    // ---- THE PADDED ROOM (Floor A, under the shaft): quilted walls, soft
    // mats, a west doorway back into the Mixture Atrium, and the verdict sign.
    {
        const float wallH = 1.5f, wallCy = aY + 1.5f;   // walls y 2..5
        // East / north / south walls (full), west wall split around the door.
        struct W { float cx, cz, hx2, hz2; };
        const W walls[3] = {
            { hx + 2.5f, hz, 0.15f, 2.65f },            // east (the sign wall)
            { hx, hz + 2.5f, 2.65f, 0.15f },            // north
            { hx, hz - 2.5f, 2.65f, 0.15f },            // south
        };
        for (const W& w : walls) {
            addBox(ctx, w.cx, wallCy, w.cz, w.hx2, wallH, w.hz2, 0.0f, &sPad, kPadTint,
                   0.0f, nullptr, false, 0.6f);
            ctx.physics.addBox({ w.hx2, wallH, w.hz2 }, { w.cx, wallCy, w.cz },
                               0.0f, x3::phys::Layer::Static);
        }
        // West wall: two segments + lintel (1.4 m doorway at the middle).
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            addBox(ctx, hx - 2.5f, wallCy, hz + sgn * 1.725f, 0.15f, wallH, 0.925f,
                   0.0f, &sPad, kPadTint, 0.0f, nullptr, false, 0.6f);
            ctx.physics.addBox({ 0.15f, wallH, 0.925f },
                               { hx - 2.5f, wallCy, hz + sgn * 1.725f }, 0.0f,
                               x3::phys::Layer::Static);
        }
        addBox(ctx, hx - 2.5f, aY + 2.6f, hz, 0.15f, 0.4f, 0.8f, 0.0f, &sPad, kPadTint,
               0.0f, nullptr, false, 0.6f);
        // Roof ring (y 5) leaving the shaft mouth open in the middle.
        struct R { float ox, oz, hx2, hz2; };
        const float rIn = hole + 0.05f;
        const R roof[4] = {
            { 0,  (2.65f + rIn) * 0.5f, 2.65f, (2.65f - rIn) * 0.5f },
            { 0, -(2.65f + rIn) * 0.5f, 2.65f, (2.65f - rIn) * 0.5f },
            {  (2.65f + rIn) * 0.5f, 0, (2.65f - rIn) * 0.5f, rIn },
            { -(2.65f + rIn) * 0.5f, 0, (2.65f - rIn) * 0.5f, rIn },
        };
        for (const R& r : roof) {
            addBox(ctx, hx + r.ox, aY + 3.05f, hz + r.oz, r.hx2, 0.08f, r.hz2,
                   0.0f, &sPad, kPadTint, 0.0f, nullptr, false, 0.6f);
            ctx.physics.addBox({ r.hx2, 0.08f, r.hz2 },
                               { hx + r.ox, aY + 3.05f, hz + r.oz }, 0.0f,
                               x3::phys::Layer::Static);
        }
        // Soft landing mats (two stacked, slightly offset — quilted look).
        addBox(ctx, hx, aY + 0.18f, hz, 2.3f, 0.18f, 2.3f, 0.0f, &sPad, kPadTint,
               0.0f, nullptr, false, 0.8f);
        addBox(ctx, hx + 0.2f, aY + 0.50f, hz - 0.15f, 1.7f, 0.14f, 1.7f, 0.06f,
               &sPad, kPadTint, 0.0f, nullptr, false, 0.8f);
        ctx.physics.addBox({ 2.3f, 0.34f, 2.3f }, { hx, aY + 0.34f, hz }, 0.0f,
                           x3::phys::Layer::Static);
    }

    // ---- PROP SPAN: 12 orbs, 2 sorter arms, 1 hatch (contiguous) -----------
    x3::prims::PrimMesh sph = x3::prims::makeUVSphere(12, 18);
    x3::rhi::MeshHandle orbMesh = ctx.device.createMesh(
        sph.verts.data(), (uint32_t)sph.verts.size(),
        sph.index.data(), (uint32_t)sph.index.size());
    ctx.meshes.push_back(orbMesh);   // shared: ONCE
    room.propEntFirst = s.size();
    // Phase 5: the orbs are GOLD-PBR spheres (worn-metal set under a gold
    // tint) with a RESTRAINED inner glow — treasure, not lightbulbs. The two
    // duds stay dull dielectric grey (no glow at all: the joke reads better).
    constexpr float kGoldOrb[3] = { 1.00f, 0.84f, 0.30f };
    constexpr float kDudOrb[3]  = { 0.35f, 0.33f, 0.30f };
    for (int i = 0; i < kSortOrbs; ++i) {
        const bool dud = (i == kSortDud[0] || i == kSortDud[1]);
        const float r = 0.35f;
        const float xA[3] = { r, 0, 0 }, yA[3] = { 0, r, 0 }, zA[3] = { 0, 0, r };
        const float a = ((float)i / kSortOrbs) * kTwoPi;
        addShared(ctx, orbMesh, xA, yA, zA,
                  ax + kSortOrbR * std::cos(a), y0 + kSortOrbY, az + kSortOrbR * std::sin(a),
                  dud ? kDudOrb : kGoldOrb, dud ? 0.0f : 0.26f, nullptr,
                  &sBrass);
    }
    // Sorter arms: STARFORGE HERO HOOK (FactoryProps/SorterArm.glb — contract:
    // metres, PIVOT at the origin, arm reaching +X, sweep height baked at 0 so
    // the hook lifts it to the post top). Fallback: the Phase-3 offset box in
    // worn-brass PBR. Both branches: base yaw aims the arm at the center;
    // tick() sweeps obj(yaw) [* heroXf per drawable when the hero is real].
    for (int a = 0; a < 2; ++a) {
        const float px = ax + kSortArmPivot[a][0], pz = az + kSortArmPivot[a][1];
        const float baseYaw = (a == 0) ? 1.5708f : -1.5708f;
        const int n = heroHook(ctx, "FactoryProps/SorterArm.glb", "SorterArm",
                               px, y0 + 1.9f, pz, baseYaw, 1.0f, /*isProp*/true,
                               &room.heroXf);
        if (n > 0) { room.heroArmPrims = (uint32_t)n; continue; }
        x3::prims::PrimMesh pm = x3::prims::makeBox(1.9f, 0.08f, 0.22f,
                                                    2.2f, 0.0f, 0.0f, 0.35f);
        Entity e;
        e.mesh = ctx.device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                       pm.index.data(), (uint32_t)pm.index.size());
        ctx.meshes.push_back(e.mesh);
        if (sBrass.ok) { e.tex = sBrass.albedo; e.normalTex = sBrass.normal; e.mrTex = sBrass.mr; }
        e.baseColor[0] = kBrassTint[0]; e.baseColor[1] = kBrassTint[1];
        e.baseColor[2] = kBrassTint[2]; e.baseColor[3] = 1.0f;
        e.emissive[0] = kBrassGlow[0]; e.emissive[1] = kBrassGlow[1];
        e.emissive[2] = kBrassGlow[2]; e.emissive[3] = 0.25f;
        e.tag = (uint32_t)Tag::Prop;
        yawXform(e.transform, baseYaw, px, y0 + 1.9f, pz);
        s.add(e);
    }
    // The hatch: a brass plate over the chute hole; its MOVED-STATIC physics
    // body slides aside with the visual (tickRoomSorting does the poke).
    {
        addBox(ctx, hx, y0 + 0.07f, hz, hole + 0.05f, 0.07f, hole + 0.05f, 0.0f,
               &sBrass, kBrassTint, 0.20f, kBrassGlow, /*isProp*/true);
        room.physRef   = &ctx.physics;
        room.hatchBody = ctx.physics.addBox({ hole + 0.05f, 0.07f, hole + 0.05f },
                                            { hx, y0 + 0.07f, hz }, 0.0f,
                                            x3::phys::Layer::Static);
    }
    room.propEntCount = s.size() - room.propEntFirst;

    // ---- GLOW SPAN: 4 hatch rim strips + the sign's strokes (warm <= 0.45) --
    room.glowEntFirst = s.size();
    constexpr float kGold2[3] = { 1.00f, 0.84f, 0.30f };
    for (int i = 0; i < 4; ++i) {
        const float o = hole + 0.18f;
        const bool xSide = (i < 2);
        const float sgn2 = (i & 1) ? 1.0f : -1.0f;
        addGlow(ctx, hx + (xSide ? sgn2 * o : 0.0f), y0 + 0.05f,
                hz + (xSide ? 0.0f : sgn2 * o),
                xSide ? 0.06f : o, 0.05f, xSide ? o : 0.06f, 0.0f, kGold2, 0.40f);
    }
    // The verdict, two lines on the padded room's east wall (facing the mats;
    // wall runs y 2..5, mats top ~2.7 — the lines sit in the clear band).
    // Enamel strokes, judicial gold glow held at the warm accent cap.
    addSignText(ctx, "QUALITY:", hx + 2.30f, hz, aY + 1.90f, 0.42f, 0.55f, kGold2, 0.45f);
    addSignText(ctx, "DUBIOUS",  hx + 2.30f, hz, aY + 1.05f, 0.42f, 0.55f, kGold2, 0.45f);
    room.glowEntCount = s.size() - room.glowEntFirst;

    // ---- GLIMVALE DRESSING: sorting overflow — crates the duds ended up in,
    // by the south wall, well off the orbit ring.
    dress(ctx, "Glimvale/SM_Stylized_Box_var1.glb", ax - 15.8f, y0 + 0.25f, az - 16.8f, 0.9f);
    dress(ctx, "Glimvale/SM_Stylized_Box.glb", ax - 16.9f, y0 + 0.0f, az - 17.4f, 2.1f);
    dress(ctx, "Glimvale/SM_Stylized_Barrel.glb", ax - 17.6f, y0, az - 15.4f, 1.2f);
    ctx.physics.addBox({ 1.5f, 0.55f, 1.4f }, { ax - 16.6f, y0 + 0.55f, az - 16.4f },
                       0.0f, x3::phys::Layer::Static);
    dress(ctx, "Glimvale/SM_Large_Wall_02.glb", ax - 6.5f, y0, az - 19.72f, 1.5708f);
    dress(ctx, "Glimvale/SM_Large_Wall_01.glb", ax + 1.5f, y0, az + 19.72f, -1.5708f);
}

void tickRoomSorting(Scene& scene, AnnexRoom& room, float t) {
    if (room.propEntCount == 0) return;
    const float ax = room.centerX, az = room.centerZ, y0 = room.baseY;
    // Orbs: orbit the center at 0.5 rad/s with a light per-orb bob.
    for (int i = 0; i < kSortOrbs; ++i) {
        Entity& e = scene.get(room.propEntFirst + (uint32_t)i);
        const float a = ((float)i / kSortOrbs) * kTwoPi + kSortOrbW * t;
        e.transform[12] = ax + kSortOrbR * std::cos(a);
        e.transform[13] = y0 + kSortOrbY + 0.15f * std::sin(t * 2.0f + (float)i);
        e.transform[14] = az + kSortOrbR * std::sin(a);
    }
    // Sorter arms: sweep +-1.2 rad @ 0.7 Hz, phase-offset, about their pivots.
    // Hero branch: each arm is heroArmPrims drawable entities re-posed as
    // obj(yaw at pivot) * heroXf[i] (node transforms baked at build; zero
    // per-frame heap — mulMat4 into the entity transform in place).
    const uint32_t armN = room.heroArmPrims ? room.heroArmPrims : 1u;
    for (int a = 0; a < 2; ++a) {
        const float base = (a == 0) ? 1.5708f : -1.5708f;
        const float yaw  = base + 1.2f * std::sin(t * (0.7f * kTwoPi) + (float)a * 2.4f);
        if (room.heroArmPrims == 0) {
            pokeYaw(scene.get(room.propEntFirst + 12u + (uint32_t)a).transform, yaw);
        } else {
            const float px = ax + kSortArmPivot[a][0], pz = az + kSortArmPivot[a][1];
            const float c = std::cos(yaw), sn = std::sin(yaw);
            const float obj[16] = { c,0,-sn,0, 0,1,0,0, sn,0,c,0, px, y0 + 1.9f, pz, 1 };
            for (uint32_t j = 0; j < armN; ++j) {
                Entity& e = scene.get(room.propEntFirst + 12u + (uint32_t)a * armN + j);
                x3::asset::mulMat4(obj, &room.heroXf[((size_t)a * armN + j) * 16], e.transform);
            }
        }
    }
    // The hatch: stateA (fed by FactoryAnnex::tick once trigger 310 latches)
    // counts the stand-on beat; after kHatchDelay it slides open over
    // kHatchOpenT seconds — visual AND physics (moved-static).
    {
        Entity& e = scene.get(room.propEntFirst + 12u + 2u * armN);
        float openT = (room.stateA - kHatchDelay) / kHatchOpenT;
        openT = openT < 0.0f ? 0.0f : (openT > 1.0f ? 1.0f : openT);
        const float ease = openT * openT * (3.0f - 2.0f * openT);   // smoothstep
        const float hx = ax + FactoryAnnex::kChuteX + kHatchSlide * ease;
        e.transform[12] = hx;
        if (openT > 0.0f && openT < 1.001f && room.physRef && room.hatchBody.valid() &&
            room.stateB < 1.5f) {
            room.physRef->setBodyPosition(room.hatchBody,
                { hx, y0 + 0.07f, az + FactoryAnnex::kChuteZ });
            if (openT >= 1.0f) {
                // Fully open exactly once: bump the host-audible event (buzz).
                room.stateB = 2.0f;
                room.eventCount += 1;
                room.eventPos[0] = ax + FactoryAnnex::kChuteX;
                room.eventPos[1] = y0 + 0.5f;
                room.eventPos[2] = az + FactoryAnnex::kChuteZ;
            }
        }
    }
    // Glow: hatch rims pulse gold — urgently while the stand-on beat runs
    // (a transient event pulse may briefly top the steady cap); the sign
    // strokes hold a steady judicial glow with a faint shimmer, in the warm
    // accent band.
    for (uint32_t i = 0; i < room.glowEntCount; ++i) {
        Entity& e = scene.get(room.glowEntFirst + i);
        if (i < 4) {
            const bool arming = room.stateA > 0.0f && room.stateA < kHatchDelay;
            e.emissive[3] = arming ? 0.55f + 0.35f * std::sin(t * 14.0f)
                                   : 0.34f + 0.10f * std::sin(t * 1.3f + (float)i);
        } else {
            e.emissive[3] = 0.42f + 0.05f * std::sin(t * 2.1f + (float)i * 0.9f);
        }
    }
}

// ============================================================================
// FLOOR E (y=54) — THE TUBE JUNCTION + burst lobby (Task 11)
// ============================================================================
// Five glass transport tubes (1.2 m dia) fan from the west wall base up to the
// ceiling; a pneumatic brass capsule whooshes along a fixed 4-point polyline
// (6 m/s, 2 s pauses at both ends, ping-pong; arrival bumps eventCount for the
// host's doorThunk cue). Center: the GOLDEN BURST DAIS under the roof
// extension — trigger 312 arms the keypad hint ("the roof is not the limit —
// 9999", already wired in host_factory's HUD title). Trigger 313 marks the
// tube boarding platform. All local offsets; capsule motion is deterministic
// in t, so tick() carries no integration state.
constexpr float kTubeBase[5][2] = {   // (z at x=-19, floor end)
    { -16.0f, 0 }, { -8.0f, 0 }, { 0.0f, 0 }, { 8.0f, 0 }, { 16.0f, 0 },
};
constexpr float kTubeTopZ[5] = { -10.0f, -5.0f, 0.0f, 5.0f, 10.0f };
constexpr float kCapsPath[4][3] = {   // local {x, y-above-base, z}
    { -17.5f, 1.6f, -13.0f }, { -12.0f, 4.2f, -6.0f },
    {  -8.5f, 7.2f,  2.0f }, {  -6.5f, 9.8f,  8.5f },
};
constexpr float kCapsSpeed = 6.0f, kCapsPause = 2.0f;

// Capsule path metrics (compile-time-ish; computed on first use, POD only).
inline float capsSegLen(int i) {
    const float dx = kCapsPath[i + 1][0] - kCapsPath[i][0];
    const float dy = kCapsPath[i + 1][1] - kCapsPath[i][1];
    const float dz = kCapsPath[i + 1][2] - kCapsPath[i][2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
inline float capsTotalLen() { return capsSegLen(0) + capsSegLen(1) + capsSegLen(2); }

void buildRoomTube(FactoryRoomCtx& ctx, AnnexRoom& room) {
    Scene& s = ctx.scene;
    const float ax = ctx.centerX, az = ctx.centerZ, y0 = room.baseY;   // 54
    const SurfaceSet& sBrass  = ctx.surf.get(ctx.device, "fa_brass_worn");
    const SurfaceSet& sIron   = ctx.surf.get(ctx.device, "mw_metal_panels_a");
    const SurfaceSet& sMarble = ctx.surf.get(ctx.device, "fa_marble_white");
    constexpr float kGold[3] = { 1.00f, 0.84f, 0.30f };

    // ---- The five glass tubes: tilted near-clear cylinders, wall to ceiling.
    for (int i = 0; i < 5; ++i) {
        const float bx = ax - 19.0f, bz = az + kTubeBase[i][0], by = y0 + 0.3f;
        const float tx = ax - 6.0f,  tz = az + kTubeTopZ[i],    ty = y0 + 10.7f;
        float dyv[3] = { tx - bx, ty - by, tz - bz };
        const float len = std::sqrt(dyv[0]*dyv[0] + dyv[1]*dyv[1] + dyv[2]*dyv[2]);
        dyv[0] /= len; dyv[1] /= len; dyv[2] /= len;
        // Basis: local +Y along the tube axis; +X level (up x axis), +Z = X x Y.
        float xA[3] = { dyv[2], 0.0f, -dyv[0] };
        const float xl = std::sqrt(xA[0]*xA[0] + xA[2]*xA[2]);
        xA[0] /= xl; xA[2] /= xl;
        const float zA[3] = { xA[1]*dyv[2] - xA[2]*dyv[1],
                              xA[2]*dyv[0] - xA[0]*dyv[2],
                              xA[0]*dyv[1] - xA[1]*dyv[0] };
        x3::prims::PrimMesh pm = x3::prims::makeCylinder(0.6f, 0.6f, len * 0.5f, 18);
        Entity e;
        e.mesh = ctx.device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                       pm.index.data(), (uint32_t)pm.index.size());
        ctx.meshes.push_back(e.mesh);
        e.baseColor[0] = 0.72f; e.baseColor[1] = 0.88f; e.baseColor[2] = 0.94f;
        e.baseColor[3] = 1.0f;
        e.transparent = true;
        e.glass.opacity = 0.055f; e.glass.refraction = 0.010f;
        e.glass.roughness = 0.05f; e.glass.specular = 0.6f;
        e.glass.tint[0] = 0.72f; e.glass.tint[1] = 0.88f; e.glass.tint[2] = 0.94f;
        e.tag = (uint32_t)Tag::Static;
        makeXform(e.transform, xA, dyv, zA,
                  (bx + tx) * 0.5f, (by + ty) * 0.5f, (bz + tz) * 0.5f);
        s.add(e);
        // Wall socket at the base (brass) — the tube visibly PLUGS IN.
        addBox(ctx, bx - 0.1f, by + 0.4f, bz, 0.35f, 0.95f, 0.95f, 0.0f,
               &sBrass, kBrassTint);
    }
    // ---- The boarding platform (trigger 313's volume sits over it).
    {
        addBox(ctx, ax - 12.0f, y0 + 0.15f, az - 12.0f, 2.0f, 0.15f, 2.0f, 0.0f,
               &sIron, kDarkIron);
        ctx.physics.addBox({ 2.0f, 0.15f, 2.0f }, { ax - 12.0f, y0 + 0.15f, az - 12.0f },
                           0.0f, x3::phys::Layer::Static);
    }
    // ---- The golden burst dais (Phase 5): a POLISHED MARBLE inlay disc
    // around the cab opening, ringed by the gold-tinted brass torus — the one
    // precious surface in the works, under the roof it is about to lose.
    {
        // Marble annulus: 12 tangent slabs ringing the cab opening (the
        // opening itself stays OPEN — the cab rises through it). Visual
        // inlay 7 cm proud of the deck; the slab keeps the physics.
        for (int i = 0; i < 12; ++i) {
            const float a = ((float)i + 0.5f) * (kTwoPi / 12.0f);
            const float c = std::cos(a), sn = std::sin(a);
            x3::prims::PrimMesh pm = x3::prims::makeBox(1.02f, 0.035f, 0.85f,
                                                        0, 0, 0, 0.22f);
            Entity e;
            e.mesh = ctx.device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                           pm.index.data(), (uint32_t)pm.index.size());
            ctx.meshes.push_back(e.mesh);
            if (sMarble.ok) { e.tex = sMarble.albedo; e.normalTex = sMarble.normal; e.mrTex = sMarble.mr; }
            e.baseColor[0] = 1.0f; e.baseColor[1] = 0.98f; e.baseColor[2] = 0.94f;
            e.baseColor[3] = 1.0f;
            e.tag = (uint32_t)Tag::Static;
            const float xA[3] = { -sn, 0, c }, yA[3] = { 0, 1, 0 }, zA[3] = { -c, 0, -sn };
            // Tiny per-segment y stagger: the tangent slabs overlap near the
            // inner radius; 0.2 mm steps kill the coplanar z-fight invisibly.
            makeXform(e.transform, xA, yA, zA,
                      ax + 3.75f * c, y0 + 0.035f + 0.0002f * (float)i, az + 3.75f * sn);
            s.add(e);
        }
        // Outer gold trim ring seating the marble into the deck.
        x3::prims::PrimMesh tr = x3::prims::makeTorus(4.6f, 0.055f, 48, 8);
        Entity t2;
        t2.mesh = ctx.device.createMesh(tr.verts.data(), (uint32_t)tr.verts.size(),
                                        tr.index.data(), (uint32_t)tr.index.size());
        ctx.meshes.push_back(t2.mesh);
        if (sBrass.ok) { t2.tex = sBrass.albedo; t2.normalTex = sBrass.normal; t2.mrTex = sBrass.mr; }
        t2.baseColor[0] = kGold[0]; t2.baseColor[1] = kGold[1]; t2.baseColor[2] = kGold[2];
        t2.baseColor[3] = 1.0f;
        t2.emissive[0] = kGold[0]; t2.emissive[1] = kGold[1]; t2.emissive[2] = kGold[2];
        t2.emissive[3] = 0.22f;
        t2.tag = (uint32_t)Tag::Static;
        const float xA2[3] = { 1, 0, 0 }, yA2[3] = { 0, 0, -1 }, zA2[3] = { 0, 1, 0 };
        makeXform(t2.transform, xA2, yA2, zA2, ax, y0 + 0.10f, az);
        s.add(t2);
        // The brass ring torus (gold-tinted, restrained glow — the studs chase).
        x3::prims::PrimMesh pm2 = x3::prims::makeTorus(3.2f, 0.16f, 48, 10);
        Entity e2;
        e2.mesh = ctx.device.createMesh(pm2.verts.data(), (uint32_t)pm2.verts.size(),
                                        pm2.index.data(), (uint32_t)pm2.index.size());
        ctx.meshes.push_back(e2.mesh);
        if (sBrass.ok) { e2.tex = sBrass.albedo; e2.normalTex = sBrass.normal; e2.mrTex = sBrass.mr; }
        e2.baseColor[0] = kGold[0] * 0.8f; e2.baseColor[1] = kGold[1] * 0.8f;
        e2.baseColor[2] = kGold[2] * 0.8f; e2.baseColor[3] = 1.0f;
        e2.emissive[0] = kGold[0]; e2.emissive[1] = kGold[1]; e2.emissive[2] = kGold[2];
        e2.emissive[3] = 0.30f;
        e2.tag = (uint32_t)Tag::Static;
        const float xA[3] = { 1, 0, 0 }, yA[3] = { 0, 0, -1 }, zA[3] = { 0, 1, 0 };
        makeXform(e2.transform, xA, yA, zA, ax, y0 + 0.18f, az);
        s.add(e2);
    }

    // ---- GLIMVALE DRESSING: dispatch clutter by the boarding platform, and
    // an arch framing the walk from the dais toward the tube fan.
    dress(ctx, "Glimvale/SM_Stylized_Box_var2.glb", ax + 16.8f, y0 + 0.35f, az + 16.6f, 0.8f);
    dress(ctx, "Glimvale/SM_Stylized_Box_var1.glb", ax + 17.6f, y0 + 0.25f, az + 15.2f, 2.6f);
    dress(ctx, "Glimvale/SM_Stylized_Barrel.glb", ax + 15.2f, y0, az + 17.4f, 2.0f);
    ctx.physics.addBox({ 1.5f, 0.55f, 1.5f }, { ax + 16.6f, y0 + 0.55f, az + 16.4f },
                       0.0f, x3::phys::Layer::Static);
    dress(ctx, "Glimvale/SM_Stylized_Arch.glb", ax - 7.5f, y0, az - 4.0f, 0.6f, 0.85f, kArchTint);
    dress(ctx, "Glimvale/SM_Large_Wall_01.glb", ax + 1.5f, y0, az - 19.72f, 1.5708f);

    // ---- PROP SPAN: the pneumatic capsule. STARFORGE HERO HOOK first
    // (FactoryProps/PneumaticCapsule.glb — contract: metres, origin at the
    // capsule CENTER, travel axis +Y so the path basis carries it); fallback:
    // the Phase-3 brass cylinder in worn-brass PBR. Pose = path formula.
    room.propEntFirst = s.size();
    {
        const int n = heroHook(ctx, "FactoryProps/PneumaticCapsule.glb",
                               "PneumaticCapsule",
                               ax + kCapsPath[0][0], y0 + kCapsPath[0][1],
                               az + kCapsPath[0][2], 0.0f, 1.0f, /*isProp*/true,
                               &room.heroXf);
        if (n > 0) {
            room.heroCapsPrims = (uint32_t)n;
        } else {
            x3::prims::PrimMesh pm = x3::prims::makeCylinder(0.30f, 0.30f, 0.45f, 14);
            Entity e;
            e.mesh = ctx.device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                           pm.index.data(), (uint32_t)pm.index.size());
            ctx.meshes.push_back(e.mesh);
            if (sBrass.ok) { e.tex = sBrass.albedo; e.normalTex = sBrass.normal; e.mrTex = sBrass.mr; }
            e.baseColor[0] = kBrassTint[0]; e.baseColor[1] = kBrassTint[1];
            e.baseColor[2] = kBrassTint[2]; e.baseColor[3] = 1.0f;
            e.emissive[0] = kBrassGlow[0]; e.emissive[1] = kBrassGlow[1];
            e.emissive[2] = kBrassGlow[2]; e.emissive[3] = 0.42f;
            e.tag = (uint32_t)Tag::Prop;
            yawXform(e.transform, 0.0f, ax + kCapsPath[0][0], y0 + kCapsPath[0][1],
                     az + kCapsPath[0][2]);
            s.add(e);
        }
    }
    room.propEntCount = s.size() - room.propEntFirst;

    // ---- GLOW SPAN: 12 dais studs (chase) + 5 tube base collars ------------
    // Studs authored at the warm cap (the chase lead brightens transiently);
    // collars are cyan (cool) and live under the 0.9 cool cap.
    room.glowEntFirst = s.size();
    for (int i = 0; i < 12; ++i)
        addRingGlow(ctx, ax, y0 + 0.32f, az, 3.2f, ((float)i / 12.0f) * kTwoPi,
                    0.22f, 0.05f, 0.07f, kGold, 0.40f);
    for (int i = 0; i < 5; ++i)
        addGlow(ctx, ax - 18.7f, y0 + 1.75f, az + kTubeBase[i][0],
                0.10f, 0.10f, 0.75f, 0.0f, room.accent, 0.75f);
    room.glowEntCount = s.size() - room.glowEntFirst;
}

void tickRoomTube(Scene& scene, AnnexRoom& room, float t) {
    if (room.propEntCount == 0) return;
    const float ax = room.centerX, az = room.centerZ, y0 = room.baseY;
    // ---- Capsule: 6 m/s along the polyline, 2 s pause at each end, ping-pong.
    // Deterministic in t: phase -> arclength s -> segment -> pose.
    const float L = capsTotalLen();
    const float T = L / kCapsSpeed;
    const float P = 2.0f * (T + kCapsPause);
    const float phase = std::fmod(t, P);
    float sArc; int phState;                      // 0 pause@0, 1 fwd, 2 pause@L, 3 back
    if      (phase < kCapsPause)            { sArc = 0.0f; phState = 0; }
    else if (phase < kCapsPause + T)        { sArc = (phase - kCapsPause) * kCapsSpeed; phState = 1; }
    else if (phase < 2.0f * kCapsPause + T) { sArc = L; phState = 2; }
    else                                    { sArc = L - (phase - 2.0f * kCapsPause - T) * kCapsSpeed; phState = 3; }
    int seg = 0;
    float s0 = 0.0f;
    while (seg < 2 && sArc > s0 + capsSegLen(seg)) { s0 += capsSegLen(seg); ++seg; }
    const float segL = capsSegLen(seg);
    const float u = segL > 1e-5f ? (sArc - s0) / segL : 0.0f;
    float d[3] = { kCapsPath[seg + 1][0] - kCapsPath[seg][0],
                   kCapsPath[seg + 1][1] - kCapsPath[seg][1],
                   kCapsPath[seg + 1][2] - kCapsPath[seg][2] };
    const float dl = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    d[0] /= dl; d[1] /= dl; d[2] /= dl;
    // Basis: +Y along travel; +X level; +Z = X x Y (det +1).
    float xA[3] = { d[2], 0.0f, -d[0] };
    const float xl = std::sqrt(xA[0]*xA[0] + xA[2]*xA[2]);
    xA[0] /= (xl > 1e-5f ? xl : 1.0f); xA[2] /= (xl > 1e-5f ? xl : 1.0f);
    const float zA[3] = { xA[1]*d[2] - xA[2]*d[1],
                          xA[2]*d[0] - xA[0]*d[2],
                          xA[0]*d[1] - xA[1]*d[0] };
    const float capX = ax + kCapsPath[seg][0] + (kCapsPath[seg + 1][0] - kCapsPath[seg][0]) * u;
    const float capY = y0 + kCapsPath[seg][1] + (kCapsPath[seg + 1][1] - kCapsPath[seg][1]) * u;
    const float capZ = az + kCapsPath[seg][2] + (kCapsPath[seg + 1][2] - kCapsPath[seg][2]) * u;
    if (room.heroCapsPrims == 0) {
        Entity& cap = scene.get(room.propEntFirst);
        makeXform(cap.transform, xA, d, zA, capX, capY, capZ);
    } else {
        // Hero capsule: re-pose every drawable as obj(path basis) * heroXf[j]
        // (baked node transforms; no per-frame heap — mulMat4 in place).
        float obj[16];
        makeXform(obj, xA, d, zA, capX, capY, capZ);
        for (uint32_t j = 0; j < room.heroCapsPrims; ++j) {
            Entity& e = scene.get(room.propEntFirst + j);
            x3::asset::mulMat4(obj, &room.heroXf[(size_t)j * 16], e.transform);
        }
    }
    // Arrival edge (fwd -> pause@L, back -> pause@0): bump the host's cue
    // counter once per docking. stateB remembers the previous phase state.
    if ((float)phState != room.stateB) {
        const int prev = (int)room.stateB;
        if ((prev == 1 && phState == 2) || (prev == 3 && phState == 0)) {
            room.eventCount += 1;
            const int endIdx = (phState == 2) ? 3 : 0;
            room.eventPos[0] = ax + kCapsPath[endIdx][0];
            room.eventPos[1] = y0 + kCapsPath[endIdx][1];
            room.eventPos[2] = az + kCapsPath[endIdx][2];
        }
        room.stateB = (float)phState;
    }
    // Glow: the dais studs run a golden CHASE (the burst invitation — the
    // moving lead is a transient highlight over the marble, tail in the warm
    // band); the tube collars breathe cyan under the cool cap.
    for (uint32_t i = 0; i < room.glowEntCount; ++i) {
        Entity& e = scene.get(room.glowEntFirst + i);
        if (i < 12) {
            const int lead = (int)(t * 6.0f) % 12;
            const int dist = ((int)i - lead + 12) % 12;
            e.emissive[3] = (dist == 0) ? 0.85f : (dist < 3 ? 0.42f : 0.16f);
        } else {
            e.emissive[3] = 0.70f + 0.15f * std::sin(t * 1.1f + (float)i * 1.9f);
        }
    }
}

} // namespace x3::game
