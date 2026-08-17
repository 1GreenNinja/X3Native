// ROADSIDE CAMPFIRES — implementation. See campfire.h for the contract and
// every provenance note; this file is the arithmetic.

#include "campfire.h"

#include "asset_root.h"
#include "mesh_prims.h"
#include "terrain.h"
#include "town.h"              // townPedClipTable() — the MEASURED roster table

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdio>
#include <filesystem>

namespace x3::game {

namespace {

constexpr uint32_t kMaxFires   = 4;      // "a handful" of the bench sites
constexpr float    kSpacingM   = 300.0f; // fires spread along the road, not clustered
constexpr float    kActiveM    = 280.0f; // people tick within this of the camera
constexpr float    kFxM        = 350.0f; // particles submit within this
constexpr float    kLightM     = 400.0f; // fire lights upload within this
constexpr float    kRingR      = 0.55f;  // stone ring radius (m)
constexpr float    kFireFromBench = 2.10f; // ring centre this far in front of the bench
constexpr float    kPeopleR    = 1.80f;  // standers this far from the flames

constexpr float kPi = 3.14159265358979f;

// Deterministic LCG (the road_trees.cpp family — no rand()).
struct Lcg {
    uint32_t s;
    explicit Lcg(uint32_t seed) : s(seed ? seed : 1u) {}
    float next() { s = s * 1664525u + 1013904223u;
                   return (float)((s >> 8) & 0xFFFFFFu) / 16777216.0f; }
};

// Stateless per-particle hash (i, salt, seed) -> [0,1).
inline float phash(uint32_t i, uint32_t salt, uint32_t seed) {
    uint32_t h = i * 374761393u + salt * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

inline float clamp01f(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
inline float sstep(float a, float b, float x) {
    float u = clamp01f((x - a) / (b - a)); return u * u * (3.0f - 2.0f * u);
}

// Column-major basis+translation into a 16-float transform.
inline void basisM(const float X[3], const float Y[3], const float Z[3],
                   const float T[3], float out[16]) {
    out[0]=X[0]; out[1]=X[1]; out[2] =X[2];  out[3]=0;
    out[4]=Y[0]; out[5]=Y[1]; out[6] =Y[2];  out[7]=0;
    out[8]=Z[0]; out[9]=Z[1]; out[10]=Z[2];  out[11]=0;
    out[12]=T[0]; out[13]=T[1]; out[14]=T[2]; out[15]=1;
}

} // namespace

// ---------------------------------------------------------------------------
// BUILD
// ---------------------------------------------------------------------------
uint32_t Campfires::build(Scene& scene, x3::rhi::IRenderDevice& device,
                          x3::phys::IPhysicsWorld& phys,
                          const std::vector<RoadTrees::BenchSite>& benches,
                          x3::audio::IAudioSystem* audio) {
    if (m_built) return (uint32_t)m_fires.size();
    m_built = true;
    if (benches.empty()) {
        x3::logWarn("campfire: no bench sites recorded — no fires (see road_trees)");
        return 0;
    }

    // ---- Pick spaced sites: greedy accept when far from every accepted one.
    std::vector<const RoadTrees::BenchSite*> picked;
    for (const RoadTrees::BenchSite& b : benches) {
        // (`spaced`, not `far` — windef.h #defines `far` away on MSVC.)
        bool spaced = true;
        for (const RoadTrees::BenchSite* p : picked) {
            const float dx = p->x - b.x, dz = p->z - b.z;
            if (dx * dx + dz * dz < kSpacingM * kSpacingM) { spaced = false; break; }
        }
        if (spaced) picked.push_back(&b);
        if (picked.size() >= kMaxFires) break;
    }
    if (picked.empty()) picked.push_back(&benches.front());

    // ---- Shared meshes (three jittered stones, one log, ember disc, stick).
    for (int v = 0; v < 3; ++v) {
        x3::prims::PrimMesh pm = x3::prims::makeFacetedCrystal(
            0.12f, 0.055f, 0.030f, 91u + (uint32_t)v * 37u);
        m_stoneMesh[v] = device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                           pm.index.data(), (uint32_t)pm.index.size());
    }
    {
        x3::prims::PrimMesh pm = x3::prims::makeBox(0.045f, 0.045f, 0.23f, 0, 0, 0, 1.0f);
        m_logMesh = device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                      pm.index.data(), (uint32_t)pm.index.size());
    }
    {
        // Ember bed: a low, wide octagon-ish slab (a squashed crystal reads as
        // a mound of coals better than a box).
        x3::prims::PrimMesh pm = x3::prims::makeFacetedCrystal(0.30f, 0.030f, 0.015f, 7u);
        m_emberMesh = device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                        pm.index.data(), (uint32_t)pm.index.size());
    }
    {
        x3::prims::PrimMesh pm = x3::prims::makeBox(0.008f, 0.008f, 0.45f, 0, 0, 0, 1.0f);
        m_stickMesh = device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                        pm.index.data(), (uint32_t)pm.index.size());
        x3::prims::PrimMesh hd = x3::prims::makeBox(0.024f, 0.024f, 0.070f, 0, 0, 0, 1.0f);
        m_hotdogMesh = device.createMesh(hd.verts.data(), (uint32_t)hd.verts.size(),
                                         hd.index.data(), (uint32_t)hd.index.size());
    }

    // ---- The REAL rock surface for the stones (rule 5: no flat-tint stand-in).
    m_surf.mount(assetRoot() + "/surface_library");
    const SurfaceSet& rock = m_surf.get(device, "cv_rock_flume");
    if (!rock.ok)
        x3::logWarn("campfire: cv_rock_flume set missing — stones fall back to tint");

    // ---- Sound: one committed synthesized loop, positioned per fire.
    x3::audio::SoundHandle crackleSnd{};
    if (audio) {
        const std::string wav = (std::filesystem::path(assetRoot()) /
                                 "audio/ambient/campfire_crackle_loop.wav").string();
        crackleSnd = audio->load(wav);
        if (!crackleSnd.valid())
            x3::logWarn("campfire: campfire_crackle_loop.wav missing — silent fires "
                        "(run tools/gen_campfire_audio.py)");
    }

    uint32_t fireIdx = 0;
    for (const RoadTrees::BenchSite* b : picked) {
        Fire f{};
        f.benchYaw     = b->yaw;
        f.towardRoadX  = b->towardRoadX;
        f.towardRoadZ  = b->towardRoadZ;
        f.seed         = 0xF17E5EEDu + fireIdx * 977u;
        f.lightPhase   = (float)fireIdx * 2.13f;

        // Ring centre: in front of the bench, on ground FLAT ENOUGH for the
        // ring + the standers (same honesty test the bench itself passed).
        bool ok = false;
        for (float off = kFireFromBench; off >= 1.5f && !ok; off -= 0.3f) {
            const float fx = b->x + b->towardRoadX * off;
            const float fz = b->z + b->towardRoadZ * off;
            const float y0 = terrainHeightAtWorld(fx, fz);
            float hMin = y0, hMax = y0;
            for (int c = 0; c < 4; ++c) {
                const float sx = (c & 1) ? 0.8f : -0.8f;
                const float sz = (c & 2) ? 0.8f : -0.8f;
                const float h = terrainHeightAtWorld(fx + sx, fz + sz);
                hMin = std::fmin(hMin, h); hMax = std::fmax(hMax, h);
            }
            if (hMax - hMin > 0.40f) continue;
            f.x = fx; f.z = fz; f.y = (hMin + hMax) * 0.5f;
            ok = true;
        }
        if (!ok) continue;   // batter too steep here — skip the site, keep going

        buildRing(scene, device, f);
        spawnPeople(f, fireIdx, device, phys, *b);

        if (audio && crackleSnd.valid())
            f.crackle = audio->startLoop3D(crackleSnd, f.x, f.y + 0.4f, f.z,
                                           0.55f, 1.0f - 0.06f * (float)fireIdx);

        char lb[160];
        std::snprintf(lb, sizeof(lb),
                      "campfire: fire %u at (%.1f, %.1f, %.1f) — %u people",
                      fireIdx, f.x, f.y, f.z, (uint32_t)f.people.size());
        x3::logInfo(lb);

        m_fires.push_back(std::move(f));
        ++fireIdx;
    }

    x3::logInfo("campfire: " + std::to_string(m_fires.size()) + " fires lit, " +
                std::to_string(peopleCount()) + " people warming up");
    return (uint32_t)m_fires.size();
}

void Campfires::buildRing(Scene& scene, x3::rhi::IRenderDevice& device, Fire& f) {
    const SurfaceSet& rock = m_surf.get(device, "cv_rock_flume");
    Lcg rng(f.seed);

    // STONES: 9-11 around the ring, girdle ON the ground (the faceted mesh's
    // pavilion point extends BELOW y=0, so each stone is genuinely bedded in
    // the earth — rule 4 contact, not a pebble balanced on a lawn).
    const int nStones = 9 + (int)(rng.next() * 3.0f);
    for (int i = 0; i < nStones; ++i) {
        const float a  = ((float)i + rng.next() * 0.55f) * (2.0f * kPi / (float)nStones);
        const float rr = kRingR * (0.92f + rng.next() * 0.18f);
        const float sx = f.x + std::cos(a) * rr;
        const float sz = f.z + std::sin(a) * rr;
        const float sy = terrainHeightAtWorld(sx, sz);
        const float sc  = 0.75f + rng.next() * 0.75f;      // fist to melon
        const float yaw = rng.next() * 2.0f * kPi;
        const float c = std::cos(yaw), sn = std::sin(yaw);
        Entity e;
        e.mesh = m_stoneMesh[i % 3];
        if (rock.ok) { e.tex = rock.albedo; e.mrTex = rock.mr; e.normalTex = rock.normal; }
        // Warm grey granite; the texture carries the detail (valueTint keeps
        // the albedo in the believable band).
        const float vt = rock.ok ? rock.valueTint() : 1.0f;
        e.baseColor[0] = 0.86f * vt; e.baseColor[1] = 0.84f * vt;
        e.baseColor[2] = 0.80f * vt; e.baseColor[3] = 1.0f;
        const float sxz = sc, sy2 = sc * (0.55f + rng.next() * 0.30f); // squat rocks
        e.transform[0] = c * sxz;  e.transform[2]  = -sn * sxz;
        e.transform[5] = sy2;
        e.transform[8] = sn * sxz; e.transform[10] = c * sxz;
        e.transform[12] = sx; e.transform[13] = sy - 0.015f; e.transform[14] = sz;
        e.tag = (uint32_t)Tag::Prop;
        scene.add(e);
    }

    // CHARRED LOGS: five, leaning inward over the embers (a loose teepee).
    for (int i = 0; i < 5; ++i) {
        const float a  = ((float)i + rng.next() * 0.4f) * (2.0f * kPi / 5.0f);
        const float rb = 0.34f + rng.next() * 0.08f;               // outer end
        const float bx = f.x + std::cos(a) * rb;
        const float bz = f.z + std::sin(a) * rb;
        const float by = terrainHeightAtWorld(bx, bz) + 0.03f;
        const float apex[3] = { f.x, f.y + 0.30f + rng.next() * 0.08f, f.z };
        float Z[3] = { apex[0] - bx, apex[1] - by, apex[2] - bz };
        const float zl = std::sqrt(Z[0]*Z[0] + Z[1]*Z[1] + Z[2]*Z[2]);
        Z[0] /= zl; Z[1] /= zl; Z[2] /= zl;
        float X[3] = { -Z[2], 0.0f, Z[0] };
        const float xl = std::sqrt(X[0]*X[0] + X[2]*X[2]);
        X[0] /= xl; X[2] /= xl;
        const float Y[3] = { Z[1]*X[2] - Z[2]*X[1], Z[2]*X[0] - Z[0]*X[2],
                             Z[0]*X[1] - Z[1]*X[0] };
        const float mid[3] = { (bx + apex[0]) * 0.5f, (by + apex[1]) * 0.5f,
                               (bz + apex[2]) * 0.5f };
        Entity e;
        e.mesh = m_logMesh;
        // Charcoal over bark: near-black, faint warm undertone. Dielectric —
        // never the untextured-metal black trap (rule 5).
        e.baseColor[0] = 0.070f; e.baseColor[1] = 0.055f; e.baseColor[2] = 0.045f;
        basisM(X, Y, Z, mid, e.transform);
        e.tag = (uint32_t)Tag::Prop;
        scene.add(e);
    }

    // EMBER BED: squat glowing mound under the flames. Emissive strength 0.45
    // — under the 0.5 ACES clip law (rule 5); the additive fire + the point
    // light carry the real glow.
    {
        Entity e;
        e.mesh = m_emberMesh;
        e.baseColor[0] = 0.10f; e.baseColor[1] = 0.045f; e.baseColor[2] = 0.02f;
        e.emissive[0] = 1.0f; e.emissive[1] = 0.34f; e.emissive[2] = 0.07f;
        e.emissive[3] = 0.45f;
        e.transform[0] = 1.0f; e.transform[5] = 1.0f; e.transform[10] = 1.0f;
        e.transform[12] = f.x; e.transform[13] = f.y + 0.015f; e.transform[14] = f.z;
        e.tag = (uint32_t)Tag::Prop;
        scene.add(e);
    }
}

// ---------------------------------------------------------------------------
// PEOPLE — the shared AnimatedCharacter module on the CIVILIAN roster (the
// six CityPerson_* rigs the town walks, + AnnaCasual for the seated/stick
// roles). Read the cast comment below before changing a name.
// ---------------------------------------------------------------------------
void Campfires::spawnPeople(Fire& f, uint32_t idx, x3::rhi::IRenderDevice& device,
                            x3::phys::IPhysicsWorld& phys,
                            const RoadTrees::BenchSite& bench) {
    const std::string dir = riggedGlbRoot();

    // MEASURED tables (town.cpp's receipt — exact names or a sliding statue).
    const CharacterClipTable standTable = townPedClipTable();
    CharacterClipTable sitTable = standTable;
    sitTable.idle = "Sit";                    // AnnaCasual only; hips perch ~0.44 m
    sitTable.idleVariant = nullptr;           // never drift out of the pose
    CharacterClipTable carryTable = standTable;
    carryTable.idle = "CarryIdle";            // AnnaCasual: hands carrying at the waist
    carryTable.idleVariant = nullptr;

    struct Cast { const char* rig; const CharacterClipTable* table;
                  bool onBench; bool stick; };
    // ---- THE CAST IS CIVILIANS. -------------------------------------------
    // This list was first written off CrowdSkin::defaultRigs() —
    // AnnaCasual_anim / marcus_webb_anim / chief_martinez_anim — which is cast
    // for the CLUB scene and picked purely on "has Idle/Walk/Run". Rendered,
    // that roster is a civilian woman, A CLAWED GREEN-VEINED MUTANT, and a
    // black-clad SWAT operator (tools/glb_contact_sheet.py; W-TOWN caught the
    // identical defect on Main Street and app/town.cpp:spawnPedestrians
    // carries the full receipt). A monster and a tactical officer roasting hot
    // dogs at a roadside bench is exactly the class of slop NO_SLOP rule 2
    // exists for — it compiles, the numbers are right, and it is wrong.
    //
    // So the fires cast from the same six ORDINARY PEOPLE the town walks:
    // CityPerson_* (licensed `City People FREE Samples`, built by
    // tools/town_people.py, each carrying Idle/Walk/Run/LookAround under those
    // EXACT names — the contract townPedClipTable() reads). PAIRED VALUES
    // (rule 4): this roster and town.cpp's kRigs are the same six files; a
    // rig removed there must be removed here.
    //
    // AnnaCasual stays for the SEATED and STICK roles and only those: she is a
    // civilian too, and she is the ONLY rig in the tree that owns real `Sit`
    // and `CarryIdle` clips. The CityPerson rigs have no seated pose, so they
    // STAND — two people standing beat one broken sitter (the directive).
    const Cast cast0[] = { { "AnnaCasual_anim.glb",         &sitTable,   true,  true  },
                           { "CityPerson_ManJacket.glb",    &standTable, false, false },
                           { "CityPerson_WomanCoat.glb",    &standTable, false, false } };
    const Cast cast1[] = { { "AnnaCasual_anim.glb",         &carryTable, false, true  },
                           { "CityPerson_ManCasual.glb",    &standTable, false, false },
                           { "CityPerson_WomanCasual.glb",  &standTable, false, false } };
    const Cast cast2[] = { { "AnnaCasual_anim.glb",         &sitTable,   true,  false },
                           { "CityPerson_Elder.glb",        &standTable, false, false },
                           { "CityPerson_Boy.glb",          &standTable, false, false } };
    const Cast cast3[] = { { "CityPerson_ManJacket.glb",    &standTable, false, false },
                           { "CityPerson_WomanCasual.glb",  &standTable, false, false } };
    const Cast* casts[4]  = { cast0, cast1, cast2, cast3 };
    const uint32_t nCast[4] = { 3, 3, 3, 2 };

    const Cast* cast = casts[idx % 4];
    const uint32_t n  = nCast[idx % 4];

    // Standers ring the fire on the slots the bench does not occupy. Angle 0 =
    // the toward-road axis (bench -> fire direction); the bench sits at 180.
    const float baseA = std::atan2(f.towardRoadZ, f.towardRoadX);
    const float standAngles[3] = { baseA + 0.55f, baseA - 0.75f, baseA + 2.30f };
    uint32_t standSlot = 0;

    for (uint32_t i = 0; i < n; ++i) {
        Person p;
        p.rig = std::make_unique<AnimatedCharacter>();
        if (!p.rig->load(device, dir, cast[i].rig, *cast[i].table)) {
            x3::logWarn(std::string("campfire: rig unavailable: ") + cast[i].rig);
            continue;
        }
        float px, pz;
        if (cast[i].onBench) {
            // ON THE BENCH: feet just in front of the seat plank so the Sit
            // clip's 0.44 m hip perch lands on the ~0.45 m seat. The bench IS
            // the seat prop (crowd_skin's updateSeat receipt for the height).
            px = bench.x + f.towardRoadX * 0.32f;
            pz = bench.z + f.towardRoadZ * 0.32f;
        } else {
            const float a = standAngles[standSlot % 3]; ++standSlot;
            px = f.x + std::cos(a) * kPeopleR;
            pz = f.z + std::sin(a) * kPeopleR;
        }
        const float py = terrainHeightAtWorld(px, pz);
        p.body = std::make_unique<Player>();
        p.body->spawn(phys, px, py + 0.35f, pz);

        // Everyone faces the FIRE. The rig's own yaw stays 0 while idle (no
        // turn clips in these tables), so the full facing rides yawTrim —
        // AXES LAW: yaw = atan2(-dx, -dz) toward the flames.
        const float dx = f.x - px, dz = f.z - pz;
        p.yawTrim = std::atan2(-dx, -dz);
        p.lookYaw = std::atan2(dz, dx);          // device convention for update()
        p.stick   = cast[i].stick;
        f.people.push_back(std::move(p));
    }
}

// ---------------------------------------------------------------------------
// UPDATE / DRAW
// ---------------------------------------------------------------------------
void Campfires::update(float dt, float camX, float camZ,
                       x3::phys::IPhysicsWorld& phys, x3::rhi::IRenderDevice& device) {
    m_clock += dt;
    for (Fire& f : m_fires) {
        const float dx = f.x - camX, dz = f.z - camZ;
        if (dx * dx + dz * dz > kActiveM * kActiveM) continue;
        for (Person& p : f.people) {
            if (!p.body || !p.rig) continue;
            p.body->setLook(p.lookYaw, 0.0f);
            PlayerInput in{};                       // stationary: warming hands
            p.body->update(in, dt, phys);
            AnimatedCharacter::Intent it{};
            p.rig->update(*p.body, it, p.lookYaw, dt, phys, device);
        }
    }
}

void Campfires::drawCharacters(const x3::rhi::FrameContext& frame,
                               x3::rhi::IRenderDevice& device) {
    for (Fire& f : m_fires) {
        for (Person& p : f.people) {
            if (!p.body || !p.rig) continue;
            p.rig->draw(frame, device, *p.body, p.yawTrim, 0.0f, true);

            // ---- The roasting stick: through the rig's OWN hand bone, an
            // authored pose doing the job. If no hand bone resolves, no stick
            // is drawn — never a floating prop (probe once, log the result).
            if (!p.stick) continue;
            if (p.boneProbed == 0) continue;
            float hand[16];
            bool have = false;
            if (p.boneProbed == 1) {
                have = p.rig->boneWorld(p.boneName.c_str(), *p.body, p.yawTrim, 0.0f, hand);
            } else {
                static const char* kHands[] = {
                    "mixamorig:RightHand", "mixamorig1:RightHand", "RightHand",
                    "hand_r", "Hand_R", "hand.R", "R_Hand", "Bip001 R Hand",
                };
                for (const char* cand : kHands) {
                    if (p.rig->boneWorld(cand, *p.body, p.yawTrim, 0.0f, hand)) {
                        p.boneName = cand; p.boneProbed = 1; have = true;
                        x3::logInfo(std::string("campfire: stick hand bone = ") + cand);
                        break;
                    }
                }
                if (!have && p.rig->animated()) {
                    p.boneProbed = 0;
                    x3::logWarn("campfire: no hand bone resolved — stick dropped "
                                "(pose stays authored, prop stays unshipped)");
                }
            }
            if (!have) continue;

            // Stick runs from the hand toward a point over the embers.
            const float hx = hand[12], hy = hand[13], hz = hand[14];
            float Z[3] = { f.x - hx, (f.y + 0.42f) - hy, f.z - hz };
            const float zl = std::sqrt(Z[0]*Z[0] + Z[1]*Z[1] + Z[2]*Z[2]);
            if (zl < 0.3f) continue;
            Z[0] /= zl; Z[1] /= zl; Z[2] /= zl;
            float X[3] = { -Z[2], 0.0f, Z[0] };
            float xl = std::sqrt(X[0]*X[0] + X[2]*X[2]);
            if (xl < 1e-4f) { X[0] = 1; X[2] = 0; xl = 1; }
            X[0] /= xl; X[2] /= xl;
            const float Y[3] = { Z[1]*X[2] - Z[2]*X[1], Z[2]*X[0] - Z[0]*X[2],
                                 Z[0]*X[1] - Z[1]*X[0] };
            float m[16];
            const float mid[3] = { hx + Z[0]*0.45f, hy + Z[1]*0.45f, hz + Z[2]*0.45f };
            basisM(X, Y, Z, mid, m);
            const float stickCol[4]  = { 0.30f, 0.20f, 0.11f, 1.0f };  // hazel switch
            device.drawMesh(frame, m_stickMesh, x3::rhi::TextureHandle{}, stickCol, m);
            const float tip[3] = { hx + Z[0]*0.86f, hy + Z[1]*0.86f, hz + Z[2]*0.86f };
            basisM(X, Y, Z, tip, m);
            const float dogCol[4] = { 0.52f, 0.17f, 0.07f, 1.0f };     // the hot dog
            device.drawMesh(frame, m_hotdogMesh, x3::rhi::TextureHandle{}, dogCol, m);
        }
    }
}

// ---------------------------------------------------------------------------
// FX — stateless flame/ember/smoke from the fire clock. Additive keeps a glow
// floor by construction (colors only ever ADD light — rule 5's VFX law).
// ---------------------------------------------------------------------------
void Campfires::submitFx(x3::rhi::IRenderDevice& device, float camX, float camZ) {
    if (m_fires.empty()) return;
    using PI = x3::rhi::IRenderDevice::ParticleInstance;
    constexpr int kFlames = 18, kEmbers = 12, kSmoke = 10;
    static std::vector<PI> add, alpha;
    add.clear(); alpha.clear();
    const float t = m_clock;

    for (const Fire& f : m_fires) {
        const float ddx = f.x - camX, ddz = f.z - camZ;
        if (ddx * ddx + ddz * ddz > kFxM * kFxM) continue;

        // FLAME TONGUES — additive, hot core fading to deep orange, shrinking
        // as they rise. Life/phase/size all hashed per particle index.
        for (uint32_t i = 0; i < (uint32_t)kFlames; ++i) {
            const float h1 = phash(i, 1, f.seed), h2 = phash(i, 2, f.seed);
            const float h3 = phash(i, 3, f.seed), h4 = phash(i, 4, f.seed);
            const float L  = 0.55f + 0.55f * h1;
            const float u  = t / L + h2; const float uf = u - std::floor(u);
            const float r0 = 0.17f * std::sqrt(h3) * (1.0f - uf * 0.75f);
            const float a  = h4 * 2.0f * kPi + 0.6f * std::sin(t * 2.7f + (float)i);
            PI p;
            p.pos[0] = f.x + std::cos(a) * r0 + 0.045f * std::sin(t * 9.1f + (float)i * 2.4f);
            p.pos[2] = f.z + std::sin(a) * r0 + 0.045f * std::cos(t * 8.3f + (float)i * 1.9f);
            p.pos[1] = f.y + 0.10f + uf * (0.55f + 0.40f * h1);
            p.size   = (0.15f + 0.11f * h2) * (1.0f - 0.55f * uf);
            const float fadeIn  = sstep(0.0f, 0.12f, uf);
            const float fade    = fadeIn * std::pow(1.0f - uf, 1.25f);
            const float core    = (1.0f - uf) * (1.0f - uf);
            const float flick   = 0.82f + 0.18f * std::sin(t * 13.0f + (float)i * 3.1f);
            const float r = (2.6f + 1.7f * core) * flick;
            const float g = (0.80f + 2.20f * core) * flick;
            const float b = (0.16f + 1.10f * core * core) * flick;
            p.color[0] = r; p.color[1] = g; p.color[2] = b; p.color[3] = fade;
            add.push_back(p);
        }

        // EMBER SPARKS — tiny, bright, spiralling up and winking out.
        for (uint32_t i = 0; i < (uint32_t)kEmbers; ++i) {
            const float h1 = phash(i, 11, f.seed), h2 = phash(i, 12, f.seed);
            const float h3 = phash(i, 13, f.seed);
            const float L  = 1.3f + 1.1f * h1;
            const float u  = t / L + h2; const float uf = u - std::floor(u);
            const float a  = h3 * 2.0f * kPi + uf * (2.5f + 3.0f * h1);
            const float rr = 0.10f + 0.30f * uf;
            PI p;
            p.pos[0] = f.x + std::cos(a) * rr;
            p.pos[2] = f.z + std::sin(a) * rr;
            p.pos[1] = f.y + 0.25f + uf * (0.9f + 0.9f * h1);
            p.size   = 0.018f + 0.016f * h2;
            const float wink = 0.55f + 0.45f * std::sin(t * 21.0f + (float)i * 7.3f);
            const float life = (1.0f - uf);
            p.color[0] = 3.2f * wink * life; p.color[1] = 1.05f * wink * life;
            p.color[2] = 0.18f * wink * life; p.color[3] = life;
            add.push_back(p);
        }

        // SMOKE — alpha, grey-brown, growing and thinning, drifting downwind.
        for (uint32_t i = 0; i < (uint32_t)kSmoke; ++i) {
            const float h1 = phash(i, 21, f.seed), h2 = phash(i, 22, f.seed);
            const float L  = 3.6f + 2.0f * h1;
            const float u  = t / L + h2; const float uf = u - std::floor(u);
            PI p;
            p.pos[0] = f.x + 0.35f * uf * uf * 2.0f + 0.10f * std::sin(t * 0.9f + (float)i);
            p.pos[2] = f.z + 0.13f * uf * uf * 2.0f + 0.10f * std::cos(t * 0.8f + (float)i);
            p.pos[1] = f.y + 0.55f + uf * 3.0f;
            p.size   = 0.26f + 0.85f * uf;
            const float aA = 0.15f * sstep(0.0f, 0.22f, uf) * (1.0f - uf);
            p.color[0] = 0.13f; p.color[1] = 0.125f; p.color[2] = 0.12f; p.color[3] = aA;
            alpha.push_back(p);
        }
    }

    if (!add.empty())
        device.submitParticles(add.data(), (uint32_t)add.size(),
                               x3::rhi::IRenderDevice::ParticleBlend::Additive);
    if (!alpha.empty())
        device.submitParticles(alpha.data(), (uint32_t)alpha.size(),
                               x3::rhi::IRenderDevice::ParticleBlend::Alpha);
}

// ---------------------------------------------------------------------------
// LIGHTS — one warm flickering point per fire, distance-gated.
// ---------------------------------------------------------------------------
uint32_t Campfires::lights(x3::rhi::PointLight* out, uint32_t max,
                           const float camPos[3]) const {
    uint32_t n = 0;
    for (const Fire& f : m_fires) {
        if (n >= max) break;
        const float dx = f.x - camPos[0], dz = f.z - camPos[2];
        if (dx * dx + dz * dz > kLightM * kLightM) continue;
        // Two incommensurate sines + a fast shimmer = firelight, not a strobe.
        const float ph = f.lightPhase;
        const float k  = 0.78f + 0.13f * std::sin(m_clock * 7.3f + ph)
                                + 0.06f * std::sin(m_clock * 11.9f + ph * 1.7f)
                                + 0.03f * std::sin(m_clock * 23.7f + ph * 3.1f);
        x3::rhi::PointLight& l = out[n++];
        l.pos[0] = f.x; l.pos[1] = f.y + 0.55f; l.pos[2] = f.z;
        l.range  = 13.0f;
        l.color[0] = 2.60f * k; l.color[1] = 1.10f * k; l.color[2] = 0.32f * k;
    }
    return n;
}

uint32_t Campfires::peopleCount() const {
    uint32_t n = 0;
    for (const Fire& f : m_fires) n += (uint32_t)f.people.size();
    return n;
}

bool Campfires::firePos(uint32_t i, float out[3]) const {
    if (i >= m_fires.size()) return false;
    out[0] = m_fires[i].x; out[1] = m_fires[i].y; out[2] = m_fires[i].z;
    return true;
}

bool Campfires::showcaseCamera(uint32_t i, float out[5]) const {
    if (i >= m_fires.size()) return false;
    const Fire& f = m_fires[i];
    // Three-quarter from beside the ring: along the bench axis, pulled off the
    // road side, low — flames, stones, sitter and standers all in frame.
    const float tX = std::sin(f.benchYaw), tZ = std::cos(f.benchYaw);   // bench long axis
    float dX = tX * 0.80f - f.towardRoadX * 0.60f;
    float dZ = tZ * 0.80f - f.towardRoadZ * 0.60f;
    const float dl = std::sqrt(dX * dX + dZ * dZ);
    dX /= dl; dZ /= dl;
    const float cx = f.x + dX * 5.0f, cz = f.z + dZ * 5.0f;
    out[0] = cx;
    out[1] = terrainHeightAtWorld(cx, cz) + 1.55f;
    out[2] = cz;
    out[3] = std::atan2(f.z - cz, f.x - cx);   // device yaw: fwd = (cos, ., sin)
    out[4] = -0.10f;
    return true;
}

void Campfires::shutdown(x3::rhi::IRenderDevice& device) {
    for (Fire& f : m_fires) f.people.clear();
    m_fires.clear();
    for (int v = 0; v < 3; ++v)
        if (m_stoneMesh[v].valid()) { device.destroyMesh(m_stoneMesh[v]); m_stoneMesh[v] = {}; }
    if (m_logMesh.valid())    { device.destroyMesh(m_logMesh);    m_logMesh = {}; }
    if (m_emberMesh.valid())  { device.destroyMesh(m_emberMesh);  m_emberMesh = {}; }
    if (m_stickMesh.valid())  { device.destroyMesh(m_stickMesh);  m_stickMesh = {}; }
    if (m_hotdogMesh.valid()) { device.destroyMesh(m_hotdogMesh); m_hotdogMesh = {}; }
    m_surf.destroyAll(device);
    m_built = false;
}

} // namespace x3::game
