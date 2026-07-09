// LEVEL 4.5 — THE NEXUS CHAMBER build (see canon_45.h). Geometry rides the loader's
// exported brush path (canonAddBrush) so shell/stairs get scene + collision + room-
// tagged vis exactly like level geometry. All coordinates derive from the authored
// platform rooms — nothing here is hardcoded to magic world positions except the
// scaffold layout, which is authored relative to the platforms it serves.
#include "canon_45.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace x3::game {

namespace {

// Doctrine stairs: riser 0.22 m (auto-step clears 0.4), tread 0.30 m, width 1.6 m.
constexpr float kRiser = 0.22f, kTread = 0.30f, kStairW = 1.6f;

struct BrushCtx {
    Scene* scene; x3::rhi::IRenderDevice* device; x3::phys::IPhysicsWorld* physics;
    x3::rhi::TextureHandle rockTex, steelTex;
    x3::rhi::TextureHandle rockNrm;          // normal map (invalid ok)
    uint32_t room = kNoRoom;                 // vis room id for everything we place
};

uint32_t brush(BrushCtx& c, float hx, float hy, float hz, float cx, float cy, float cz,
               x3::rhi::TextureHandle tex, const float col[4]) {
    uint32_t id = canonAddBrush(*c.scene, *c.device, *c.physics,
                                hx, hy, hz, cx, cy, cz, tex, col, c.room);
    return id;
}

// A straight stair flight from (sx,sz) at floor y0 running along `axis` (0=X, 1=Z)
// with direction `dir` (+1/-1), climbing `rise` meters. Returns the (x,z,y) of the
// TOP landing edge (where the next deck/flight continues).
void flight(BrushCtx& c, float sx, float sz, float y0, int axis, float dir,
            float rise, const float col[4], float* outX, float* outZ, float* outY) {
    const int n = std::max(1, (int)std::ceil(rise / kRiser));
    float x = sx, z = sz, y = y0;
    for (int i = 0; i < n; ++i) {
        y += kRiser;
        if (axis == 0) x += dir * kTread; else z += dir * kTread;
        // Each step: a full box from a bit below the tread top (chunky scaffold read).
        const float hy = 0.5f * std::min(0.9f, y - y0 + 0.3f);
        if (axis == 0)
            brush(c, kTread * 0.5f, hy, kStairW * 0.5f, x - dir * kTread * 0.5f, y - hy, z,
                  c.steelTex, col);
        else
            brush(c, kStairW * 0.5f, hy, kTread * 0.5f, x, y - hy, z - dir * kTread * 0.5f,
                  c.steelTex, col);
    }
    *outX = x; *outZ = z; *outY = y;
}

// A flat deck (landing / catwalk segment): full box, top at `yTop`.
void deck(BrushCtx& c, float x0, float x1, float z0, float z1, float yTop,
          const float col[4]) {
    brush(c, (x1 - x0) * 0.5f, 0.15f, (z1 - z0) * 0.5f,
          (x0 + x1) * 0.5f, yTop - 0.15f, (z0 + z1) * 0.5f, c.steelTex, col);
}

// A thin emissive vein strip lying on a platform edge / wall face. Decoration only:
// NO collision (a 5 cm lip that snags feet is worse than none).
void vein(BrushCtx& c, float hx, float hy, float hz, float cx, float cy, float cz,
          float r, float g, float b, float strength) {
    const float col[4] = { r * 0.25f, g * 0.25f, b * 0.25f, 1.0f };
    uint32_t id = canonAddBrush(*c.scene, *c.device, *c.physics, hx, hy, hz, cx, cy, cz,
                                x3::rhi::TextureHandle{}, col, c.room,
                                /*collide*/false, /*visible*/true);
    Entity& e = c.scene->get(id);
    e.emissive[0] = r; e.emissive[1] = g; e.emissive[2] = b; e.emissive[3] = strength;
}

} // namespace

void Canon45::build(CanonFloor& floor, Scene& scene, x3::rhi::IRenderDevice& device,
                    x3::phys::IPhysicsWorld& physics, std::string_view riggedModelDir,
                    const std::string& surfaceLibRoot, std::vector<CanonLight>& canonLights) {
    // ---- Find the authored pieces. ----
    uint32_t access = kNoRoom;
    std::vector<uint32_t> plats;
    for (uint32_t i = 0; i < floor.rooms.size(); ++i) {
        if (floor.rooms[i].platform) plats.push_back(i);
        if (floor.rooms[i].openCeiling) access = i;
    }
    if (plats.empty() || access == kNoRoom) {
        x3::logInfo("[canon45] no Nexus platforms/access in this floor — skipped");
        return;
    }
    auto byName = [&](const char* s) -> const CanonRoom* {
        for (uint32_t p : plats)
            if (floor.rooms[p].name.find(s) != std::string::npos) return &floor.rooms[p];
        return nullptr;
    };
    const CanonRoom& A  = floor.rooms[access];
    const CanonRoom* t1 = byName("Tier 1"); const CanonRoom* t2 = byName("Tier 2");
    const CanonRoom* t3 = byName("Tier 3"); const CanonRoom* t4 = byName("Tier 4");
    const CanonRoom* t5 = byName("Tier 5"); const CanonRoom* ep = byName("Entry Platform");
    if (!t1 || !t2 || !t3 || !t4 || !t5) {
        x3::logWarn("[canon45] tier set incomplete — cavern skipped");
        return;
    }

    // ---- Cavern envelope: platforms + access + margin. ----
    float x0 = A.x0(), x1 = A.x1(), z0 = A.z0(), z1 = A.z1();
    float top = A.y1();
    for (uint32_t p : plats) {
        const CanonRoom& r = floor.rooms[p];
        x0 = std::min(x0, r.x0()); x1 = std::max(x1, r.x1());
        z0 = std::min(z0, r.z0()); z1 = std::max(z1, r.z1());
        top = std::max(top, r.y1());
    }
    x0 -= 2.0f; x1 += 2.0f; z0 -= 2.0f; z1 += 2.0f;
    const float fy = A.y0();          // cavern floor plane = access floor (flush)
    const float cy = top + 5.0f;      // rock ceiling: 5 m of dark above the apex
    m_x0 = x0; m_x1 = x1; m_y0 = fy; m_y1 = cy; m_z0 = z0; m_z1 = z1;
    m_whisperX = t1->cx; m_whisperY = t1->y1(); m_whisperZ = t1->cz;
    m_apexX = t5->cx; m_apexY = t5->y1(); m_apexZ = t5->cz;

    // ---- Materials. ----
    m_lib.mount(surfaceLibRoot);
    const SurfaceSet& rock  = m_lib.get(device, "sr_concrete_01");
    const SurfaceSet& steel = m_lib.get(device, "mw_metal_grate");
    BrushCtx c{ &scene, &device, &physics,
                rock.ok ? rock.albedo : x3::rhi::TextureHandle{},
                steel.ok ? steel.albedo : x3::rhi::TextureHandle{},
                rock.ok ? rock.normal : x3::rhi::TextureHandle{},
                access };
    const float rockCol[4]  = { 0.34f, 0.31f, 0.27f, 1.0f };   // black-brown organic base
    const float darkCol[4]  = { 0.22f, 0.21f, 0.19f, 1.0f };
    const float steelCol[4] = { 0.38f, 0.40f, 0.42f, 1.0f };   // scaffold

    // ---- SHELL: rock floor annulus around the access footprint, walls, ceiling. ----
    // Floor strips (top flush with fy, 0.5 thick), skipping the access room's own slab.
    auto strip = [&](float sx0, float sx1, float sz0, float sz1) {
        if (sx1 - sx0 < 0.05f || sz1 - sz0 < 0.05f) return;
        brush(c, (sx1 - sx0) * 0.5f, 0.25f, (sz1 - sz0) * 0.5f,
              (sx0 + sx1) * 0.5f, fy - 0.25f, (sz0 + sz1) * 0.5f, c.rockTex, rockCol);
    };
    strip(x0, A.x0(), z0, z1);                 // west of access
    strip(A.x1(), x1, z0, z1);                 // east
    strip(A.x0(), A.x1(), z0, A.z0());         // south band
    strip(A.x0(), A.x1(), A.z1(), z1);         // north band
    // Walls (fy -> cy) + ceiling. Dark rock — the void should swallow light.
    const float wh = cy - fy;
    brush(c, 0.4f, wh * 0.5f, (z1 - z0) * 0.5f, x0 - 0.4f, fy + wh * 0.5f, (z0 + z1) * 0.5f, c.rockTex, darkCol);
    brush(c, 0.4f, wh * 0.5f, (z1 - z0) * 0.5f, x1 + 0.4f, fy + wh * 0.5f, (z0 + z1) * 0.5f, c.rockTex, darkCol);
    brush(c, (x1 - x0) * 0.5f + 0.8f, wh * 0.5f, 0.4f, (x0 + x1) * 0.5f, fy + wh * 0.5f, z0 - 0.4f, c.rockTex, darkCol);
    brush(c, (x1 - x0) * 0.5f + 0.8f, wh * 0.5f, 0.4f, (x0 + x1) * 0.5f, fy + wh * 0.5f, z1 + 0.4f, c.rockTex, darkCol);
    brush(c, (x1 - x0) * 0.5f + 0.8f, 0.4f, (z1 - z0) * 0.5f + 0.8f, (x0 + x1) * 0.5f, cy + 0.4f, (z0 + z1) * 0.5f, c.rockTex, darkCol);

    // ---- THE CLIMB. Scaffold tower inside the access room's NE corner rising over its
    // open ceiling to Tier 1, then L-stairs tier to tier, catwalk to the Entry Platform.
    float px, pz, py;
    // Tower: 3 straight flights zig-zagging in the corner (each ~1/3 of the rise), with
    // small decks. Access interior corner near (x1-2.5, z1-1.5).
    const float towerRise = (t1->y1() - fy);
    const float seg = towerRise / 3.0f;
    float sx = A.x1() - 6.0f, sz = A.z1() - 1.2f, sy = fy;
    flight(c, sx, sz, sy, 0, +1.0f, seg, steelCol, &px, &pz, &py);          // east
    deck(c, px, px + 1.7f, pz - 0.9f, pz + 0.9f, py, steelCol);
    flight(c, px + 0.4f, pz - 0.8f, py, 1, -1.0f, seg, steelCol, &px, &pz, &py);  // south, clears the +X wall
    deck(c, px - 0.9f, px + 0.9f, pz - 1.7f, pz, py, steelCol);
    flight(c, px - 0.8f, pz - 0.9f, py, 0, -1.0f, towerRise - 2.0f * seg, steelCol, &px, &pz, &py); // west
    // Bridge walkway across to Tier 1's -Z edge (long deck; 2 cm lip kills coplanar tops).
    deck(c, px - 0.9f, px + 0.9f, pz - 0.9f, t1->z0() + 1.0f, t1->y1() + 0.02f, steelCol);

    // Tier links: L-stairs (out along Z, then along X) + arrival deck. Helper.
    auto link = [&](const CanonRoom& lo, const CanonRoom& hi) {
        const float rise = hi.y1() - lo.y1();
        const float half = rise * 0.5f;
        const float zSide = (lo.cz <= hi.cz ? 1.0f : 1.0f);   // run out the +Z side (open cavern)
        float lx = (hi.cx > lo.cx) ? lo.x1() - 0.9f : lo.x0() + 0.9f;   // start near the facing edge
        float lz = lo.z1() - 0.2f;
        float ox, oz, oy;
        flight(c, lx, lz, lo.y1(), 1, +zSide, half, steelCol, &ox, &oz, &oy);       // out +Z, half rise
        deck(c, ox - 0.9f, ox + 0.9f, oz, oz + 1.7f, oy, steelCol);
        const float xDir = (hi.cx > lo.cx) ? +1.0f : -1.0f;
        flight(c, ox + xDir * 0.9f, oz + 0.85f, oy, 0, xDir, rise - half, steelCol, &ox, &oz, &oy);
        // Arrival catwalk over the high platform's +Z edge. Top rides 2 cm ABOVE the
        // platform top: the overlap region would otherwise be two coplanar top faces
        // (z-fight); a 2 cm lip is auto-stepped and reads as a bolted deck plate.
        deck(c, std::min(ox - 0.9f, hi.cx), std::max(ox + 0.9f, hi.cx),
             std::min(oz - 0.9f, hi.z1() - 0.6f), oz + 0.9f, hi.y1() + 0.02f, steelCol);
    };
    link(*t1, *t2);
    link(*t2, *t3);
    link(*t3, *t4);
    link(*t4, *t5);
    // Entry Platform catwalk from Tier 2 (near-flat: flat walk + a 5-step rise at the end).
    if (ep) {
        const float wy = t2->y1();
        deck(c, t2->cx + 1.0f, t2->cx + 3.0f, t2->z1() - 0.6f, ep->z0() + 1.0f, wy + 0.02f, steelCol);
        float ox, oz, oy;
        flight(c, t2->cx + 2.0f, ep->z0() + 1.0f, wy, 1, +1.0f, ep->y1() - wy, steelCol, &ox, &oz, &oy);
        deck(c, ox - 0.9f, ox + 0.9f, oz - 0.3f, oz + 1.2f, ep->y1() + 0.02f, steelCol);
    }

    // ---- DRESSING: biolume veins (green) on tier edges; blood-red only at the apex;
    // three abandoned work lights (warm practicals). Two accents, never elsewhere. ----
    for (const CanonRoom* t : { t1, t2, t3, t4 }) {
        vein(c, t->w * 0.42f, 0.05f, 0.09f, t->cx, t->y1() + 0.05f, t->z0() + 0.15f,
             0.18f, 1.05f, 0.40f, 1.35f);
        vein(c, 0.09f, 0.05f, t->d * 0.35f, t->x0() + 0.15f, t->y1() + 0.05f, t->cz,
             0.18f, 1.05f, 0.40f, 1.05f);
    }
    // Apex: red organic accents ringing the arena + a green counter-vein (the exception).
    vein(c, t5->w * 0.46f, 0.06f, 0.10f, t5->cx, t5->y1() + 0.06f, t5->z0() + 0.2f,
         1.05f, 0.10f, 0.07f, 1.6f);
    vein(c, t5->w * 0.46f, 0.06f, 0.10f, t5->cx, t5->y1() + 0.06f, t5->z1() - 0.2f,
         1.05f, 0.10f, 0.07f, 1.6f);
    vein(c, 0.10f, 0.06f, t5->d * 0.40f, t5->x0() + 0.2f, t5->y1() + 0.06f, t5->cz,
         0.18f, 1.05f, 0.40f, 1.2f);
    // Work lights: small warm practical boxes at access mouth + T3 + apex approach.
    auto workLight = [&](float wx, float wy, float wz) {
        const float col[4] = { 0.5f, 0.45f, 0.35f, 1.0f };
        uint32_t id = brush(c, 0.18f, 0.14f, 0.18f, wx, wy, wz, c.steelTex, col);
        Entity& e = scene.get(id);
        e.emissive[0] = 1.0f; e.emissive[1] = 0.82f; e.emissive[2] = 0.55f; e.emissive[3] = 1.4f;
    };
    workLight(A.cx - 3.0f, fy + 1.1f, A.cz);
    workLight(t3->cx, t3->y1() + 0.6f, t3->cz);
    workLight(t5->x0() + 1.2f, t5->y1() + 0.6f, t5->z0() + 1.2f);

    // ---- LIGHTS (canonLights feed, room-gated like everything else). Sparse: the dark
    // is the point. One warm pool per work light, a dim green wash at the whisper tier,
    // a red-leaning dim at the apex. ----
    auto addL = [&](float lx, float ly, float lz, float range, float r, float g, float b) {
        CanonLight cl; cl.room = access;
        cl.light.pos[0] = lx; cl.light.pos[1] = ly; cl.light.pos[2] = lz;
        cl.light.range = range;
        cl.light.color[0] = r; cl.light.color[1] = g; cl.light.color[2] = b;
        canonLights.push_back(cl);
    };
    addL(A.cx - 3.0f, fy + 1.6f, A.cz, 5.0f, 1.5f, 1.2f, 0.8f);            // access work light
    addL(t1->cx, t1->y1() + 1.2f, t1->cz, 6.0f, 0.22f, 0.75f, 0.35f);      // whisper green
    addL(t3->cx, t3->y1() + 1.3f, t3->cz, 5.0f, 1.2f, 1.0f, 0.7f);         // mid work light
    addL(t5->cx, t5->y1() + 1.6f, t5->cz, 7.0f, 1.1f, 0.22f, 0.16f);       // apex blood-red
    addL(t5->x0() + 1.2f, t5->y1() + 1.0f, t5->z0() + 1.2f, 4.0f, 1.2f, 1.0f, 0.7f);

    // ---- CREATURES: sparse. Two prowlers (T2 / T4), one dormant APEX STAND-IN.
    // TIM'S RULING (2026-07-08): the apex IS **THE CHORUS AMALGAM** — the source
    // of 4.5's whispers, a fused 70-80 ft mass of the facility's failed
    // experiments. The Whisper Gallery, the spare-a-voice mechanic, and this
    // creature are ONE story: every voice the player spares is a voice inside
    // it. This body is still a placeholder rig (scaled hard, boss-tuned, asleep
    // until approach); the true amalgam model is the future asset. ----
    {
        MonsterSystem::Tuning prowler = tuningFor(EnemyType::Verthani);
        prowler.patrolRadius = 2.5f;   // slow prowl on the platform
        m_creatures.spawn(scene, device, physics, riggedModelDir,
                          x3::phys::Vec3{ t2->cx, t2->y1() + 0.1f, t2->cz }, prowler);
        m_creatures.spawn(scene, device, physics, riggedModelDir,
                          x3::phys::Vec3{ t4->cx, t4->y1() + 0.1f, t4->cz }, prowler);
        MonsterSystem::Tuning apex = tuningFor(EnemyType::Verthani);
        apex.type = MonsterType::Boss;
        apex.hp = 900; apex.damage = 22; apex.chaseSpeed = 3.0f;
        apex.attackRange = 3.2f; apex.attackCooldown = 1.3f; apex.attackWindup = 0.5f;
        apex.modelScale = 3.0f;
        apex.tint[0] = 0.85f; apex.tint[1] = 0.75f; apex.tint[2] = 0.70f; apex.tint[3] = 1.0f;
        m_apexIdx = (int)m_creatures.spawn(scene, device, physics, riggedModelDir,
                        x3::phys::Vec3{ t5->cx, t5->y1() + 0.1f, t5->cz - 2.0f }, apex);
    }

    m_built = true;
    x3::logInfo("[canon45] NEXUS CHAMBER built: cavern " +
                std::to_string((int)(x1 - x0)) + "x" + std::to_string((int)(cy - fy)) +
                "x" + std::to_string((int)(z1 - z0)) + " m, " +
                std::to_string(plats.size()) + " tiers, scaffold climb, " +
                std::to_string(m_creatures.count()) + " creatures (apex dormant)");
}

void Canon45::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                     const x3::phys::Vec3& playerPos, IDamageSink* playerSink,
                     const AttackFxFn& fx,
                     x3::audio::IAudioSystem* audio,
                     x3::audio::SoundHandle whisperQuiet, x3::audio::SoundHandle whisperCall) {
    if (!m_built) return;
    const bool inCavern = playerPos.x > m_x0 && playerPos.x < m_x1 &&
                          playerPos.y > m_y0 - 1.0f && playerPos.y < m_y1 &&
                          playerPos.z > m_z0 && playerPos.z < m_z1;

    // Creatures: prowlers always tick (cheap, room-gated draw); the APEX sleeps until
    // the player closes on the arena — then it wakes once, permanently.
    if (!m_apexAwake && m_apexIdx >= 0) {
        const float dx = playerPos.x - m_apexX, dy = playerPos.y - m_apexY,
                    dz = playerPos.z - m_apexZ;
        if (dx * dx + dy * dy + dz * dz < 14.0f * 14.0f) {
            m_apexAwake = true;
            x3::logInfo("[canon45] THE APEX WAKES");
        }
    }
    // While dormant, hold the apex out of the AI update by zeroing dt for it: the
    // manager has no per-monster gate, so tick everything only once awake; before
    // that, tick with a null target so nothing attacks (prowlers still roam).
    if (m_apexAwake)
        m_creatures.update(dt, scene, physics, playerPos, playerSink, fx);
    else
        m_creatures.update(dt, scene, physics, playerPos, nullptr, AttackFxFn{});

    // Whisper dread — only while the player is inside the cavern. Quiet murmurs on a
    // 9-16 s jitter anywhere below the player; the NAME-CALL (VIGIL's warning made
    // real) on a rare 40-75 s jitter, always from the dark ABOVE. Stand-in take: the
    // creature-bucket vocal at low gain/pitch (no VO exists in the packs — documented).
    if (!inCavern || !audio) return;
    auto lcg = [&]() { m_rng = m_rng * 1664525u + 1013904223u; return (m_rng >> 8) & 0xFFFF; };
    auto frand = [&](float lo, float hi) { return lo + (hi - lo) * (lcg() / 65535.0f); };
    m_whisperT -= dt; m_callT -= dt;
    if (m_whisperT <= 0.0f) {
        m_whisperT = frand(9.0f, 16.0f);
        audio->playSound3D(whisperQuiet,
                           frand(m_x0 + 2, m_x1 - 2), playerPos.y - frand(1, 4),
                           frand(m_z0 + 2, m_z1 - 2), 0.30f, 0.62f);
    }
    if (m_callT <= 0.0f) {
        m_callT = frand(40.0f, 75.0f);
        audio->playSound3D(whisperCall,
                           m_whisperX + frand(-3, 3), playerPos.y + frand(3, 7),
                           m_whisperZ + frand(-3, 3), 0.42f, 0.50f);
    }
}

} // namespace x3::game
