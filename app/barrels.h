#pragma once
// Explosive barrels (Tim: "barrels need to violently explode if shot"). Built on
// the engine's DestructibleManager (fracture/chunks) + radial-impulse explosion +
// a damage callback the host wires to enemies/player. Shooting a barrel shatters
// it, which detonates an explosion at its center: it scatters its own chunks,
// deals AoE damage, spawns FX, and BREAKS NEARBY BARRELS — a chain reaction.
//
// Game/slice code only; engine/ stays pure. Built from IRenderDevice +
// IPhysicsWorld + DestructibleManager interfaces.
#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/physics/Destruction.h"

#include <functional>
#include <vector>
#include <cstdint>

namespace x3::game {

class BarrelSystem {
public:
    // AoE damage sink: "an explosion of `damage` happened at `center` with `radius`"
    // — the host applies it to monster managers + the player. Optional.
    using DamageFn = std::function<void(const float center[3], float radius, int damage)>;
    // FX sink: "spawn an explosion at `center`" — host wires CombatFx. Optional.
    using FxFn     = std::function<void(const float center[3], float radius)>;

    void setDamageSink(DamageFn fn) { m_damage = std::move(fn); }
    void setFxSink(FxFn fn)         { m_fx = std::move(fn); }

    // Init the destructible manager + a barrel fracture asset. Call once.
    void init(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics);

    // Place a barrel (its base on the floor at x,floorY,z). Returns its index.
    uint32_t spawn(float x, float floorY, float z);

    // A weapon ray was fired (eye -> dir, up to `range`). If it hits a barrel, the
    // barrel breaks (and detonates on the next update). Returns true if one was hit.
    bool onShot(const float eye[3], const float dir[3], float range = 200.0f);

    // Advance: step the manager, then detonate any barrel that broke this frame
    // (scatter + AoE damage + FX + chain). Call once per frame AFTER physics->step().
    void update(float dt);

    // Draw the barrels (intact) + their tumbling debris. Between begin/endFrame.
    void render(const x3::rhi::FrameContext& frame) const;

    void shutdown();

    uint32_t count() const { return (uint32_t)m_barrels.size(); }
    // Test/HUD: how many barrels have exploded, and live debris chunk count.
    uint32_t explodedCount() const;
    uint32_t activeDebris() const;

private:
    struct Barrel {
        x3::phys::DestructibleId id = 0;
        float center[3] = { 0, 0, 0 };
        bool  exploded  = false;        // detonation already processed
    };
    void detonate(Barrel& b);

    x3::rhi::IRenderDevice*       m_device  = nullptr;
    x3::phys::IPhysicsWorld*      m_physics = nullptr;
    x3::phys::DestructibleManager m_destr;
    x3::phys::FractureAssetId     m_asset = 0;
    std::vector<Barrel>           m_barrels;

    x3::rhi::MeshHandle    m_cube;
    x3::rhi::TextureHandle m_tex;

    DamageFn m_damage;
    FxFn     m_fx;

    // Explosion tuning.
    float m_blastRadius   = 4.5f;   // AoE + chain radius (m)
    float m_blastStrength = 28.0f;  // radial impulse — violent scatter (~50 m/s chunks)
                                    // without launching 140 m/s shrapnel across the map
    int   m_blastDamage   = 90;     // AoE damage at the center
};

// Headless self-test (--test-barrels): shooting a barrel detonates it (debris),
// the blast CHAINS to nearby barrels (all in a cluster go), a far barrel stays
// intact, and the damage sink fires once per explosion. Asserts B0-B3. No window.
bool runBarrelSelfTest();

} // namespace x3::game
