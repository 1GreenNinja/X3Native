#pragma once
// S11 — Ship NODE-TRANSFORM animation (Act-3 space engine, Wave-1 Lane D).
//
// Ultra-detailed ships are RIGID hulls with a handful of ARTICULATED parts
// (landing gear, hull panels, turrets) that move by driving a named child
// NODE's local transform — NOT by skeletal skinning. ShipNodeAnim is the tiny
// runtime that lerps each named part between two authored key-poses and writes
// the result onto that part's child entity in the Scene.
//
// Authoring contract (see tools/ship_node_anim.py + the doc in that file):
//   A ship GLB carries the hull on its root node plus one child node per moving
//   part, named "landing_gear" / "panel_A" / "turret_main" / etc. Each part node
//   has TWO key-poses: pose 0 (retracted, t=0) and pose 1 (deployed, t=1). The
//   host loads the GLB, finds each named child node, spawns a child Scene entity
//   for it, and registers it here with addPart(name, poseRetracted, poseDeployed,
//   childEntity). setPart(name, t) then lerps 0..1 and update() applies it.
//
// The four SHIPPED SpaceShip*.glb assets are single-hull (their only nodes are
// Root / Character / Armature — no authored articulated parts yet), so the
// --world shipanim showcase registers a SYNTHETIC landing_gear part on a child
// box entity to prove the driver. When real authored ships drop in (Task D4),
// the exact same addPart/setPart/update path drives their named nodes unchanged.
//
// Clean-room: C++ standard library + the engine's own Scene only.

#include <cstdint>
#include <string>
#include <vector>

namespace x3::game { class Scene; }

namespace x3::space {

// Drives named child-node transforms on a loaded ship (gear up/down, panel open).
class ShipNodeAnim {
public:
    // Bind to the ship's ROOT entity in `scene`. Captures the root's current
    // transform as the ship-local frame every part pose is composed under, so
    // moving the whole ship (turntable) carries the articulated parts with it.
    void bind(x3::game::Scene& scene, uint32_t shipEntity);

    // Register an articulated part: a named child node whose child Scene entity is
    // `childEntity`, lerping between poseA (the t=0 / retracted local transform)
    // and poseB (the t=1 / deployed local transform). Both are 4x4 column-major,
    // expressed in the SHIP-LOCAL frame (relative to the bound root). Re-adding the
    // same name replaces its definition. This is the seam an authored GLB uses: the
    // host resolves the named glTF node + its two key-poses and calls this once.
    void addPart(const char* nodeName,
                 const float poseA[16], const float poseB[16],
                 uint32_t childEntity);

    // 0..1 animates a named part (e.g. "landing_gear", "panel_A"). t is clamped to
    // [0,1]; an unknown name is a no-op. The new value is applied on the next
    // update() (and immediately to the child entity so a query right after setPart
    // already reflects it).
    void setPart(const char* nodeName, float t);

    // Current lerp value of a named part, or -1 if no such part is registered.
    float partValue(const char* nodeName) const;

    // Number of registered parts (diagnostics / self-test).
    uint32_t partCount() const { return (uint32_t)m_parts.size(); }

    // Re-read the bound root's transform (the ship may have moved/rotated this
    // frame) and re-compose every part's child-entity transform = rootWorld *
    // lerp(poseA, poseB, t). Cheap: a few mat-muls per part. Safe to call every
    // frame. No-op until bind() + at least one addPart().
    void update(float dt, x3::game::Scene& scene);

    // The bound ship root entity id (0xFFFFFFFF if unbound).
    uint32_t shipEntity() const { return m_ship; }

private:
    struct Part {
        std::string name;
        float poseA[16];
        float poseB[16];
        uint32_t child = 0xFFFFFFFFu;
        float t = 0.0f;
    };
    // Linear search by name: a ship has a handful of parts, not thousands.
    Part*       find(const char* name);
    const Part* find(const char* name) const;
    // Recompose one part's child entity transform from the bound root + its lerp.
    void apply(x3::game::Scene& scene, const Part& p);

    uint32_t           m_ship = 0xFFFFFFFFu;
    float              m_rootWorld[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::vector<Part>  m_parts;
};

// Linear interpolation of two column-major 4x4 transforms, component-wise.
// Adequate for the small translate/rotate deltas a gear/panel/turret travels
// between two key-poses (the rotational error of lerping a matrix over a modest
// angle is visually negligible for this articulation scale, and it keeps the
// driver dependency-free). out may alias neither a nor b. t is NOT clamped here.
void lerpMat4(const float a[16], const float b[16], float t, float out[16]);

} // namespace x3::space
