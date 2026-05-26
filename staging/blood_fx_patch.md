# Task #19 — blood: it's NOT boxes (those were barrels). Enhance for visibility + a pool

CLARIFICATION: `CombatFx::spawnBlood` (app/fx.cpp:246) already spawns 12 dark-red alpha
particle droplets — it is NOT the red squares Tim saw. Those squares are the rusty
**barrel cubes** (fixed by `barrels_glb_patch.md`). So blood isn't broken — but it's
small (0.05 m), short (0.35-0.7 s), dark-on-dark, so it barely reads. This patch makes
blood obviously visible + leaves a ground pool.

## app/fx.cpp — beef up `spawnBlood` + drop a blood-pool decal
```cpp
void CombatFx::spawnBlood(const x3::phys::Vec3& pos, const x3::phys::Vec3& dir) {
    x3::phys::Vec3 d = normalize(dir);
    const int n = 22;                       // was 12 — denser spray
    for (int i = 0; i < n; ++i) {
        Particle p;
        p.pos = pos;
        const float speed = 2.0f + frand() * 5.0f;
        p.vel = x3::phys::Vec3{ d.x * speed + frandSym() * 2.5f,
                                d.y * speed + frandSym() * 2.5f + 1.5f,
                                d.z * speed + frandSym() * 2.5f };
        p.life = p.maxLife = 0.5f + frand() * 0.5f;     // longer
        p.size0 = 0.09f + frand() * 0.06f;              // ~2x bigger
        p.size1 = 0.05f;
        p.r = 0.6f; p.g = 0.02f; p.b = 0.02f;
        p.a0 = 0.9f;
        p.gravity = 1.4f; p.drag = 1.1f; p.additive = false;
        spawnParticle(p);
    }
    // Ground pool: drop a dark-red decal just below the hit, facing up, so a kill
    // leaves a mark (uses the existing decal ring — see fx.h spawnDecal/decalAt).
    // Signature per fx.h line ~132 "Drop a scorch decal at hit point+normal":
    spawnDecal(x3::phys::Vec3{ pos.x, pos.y - 0.4f, pos.z },
               x3::phys::Vec3{ 0.0f, 1.0f, 0.0f },   // up-facing
               /*radius*/ 0.4f + frand() * 0.3f,
               /*rgb*/ 0.35f, 0.02f, 0.02f);
}
```
VERIFY the exact `spawnDecal` signature in fx.h (it may be `spawnDecal(pos, normal)` with
a fixed scorch look + a color/size variant, or `decalAt(...)`). If the decal API has no
color param, add a `bloodColor` overload or just rely on the bigger particle spray. Keep
`update()`/`render()` untouched. No new allocations (uses the bounded pool + decal ring).
```
