// S11 — Ship node-transform animation runtime. See ship_anim.h.
#include "ship_anim.h"

#include "../scene.h"

#include <algorithm>
#include <cstring>

namespace x3::space {

// Column-major 4x4 multiply: out = a * b (glTF/glm convention). out must not
// alias a or b. Local copy so this translation unit has no cross-lane dep.
static void mulMat4(const float a[16], const float b[16], float out[16]) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            out[c * 4 + r] =
                a[0 * 4 + r] * b[c * 4 + 0] +
                a[1 * 4 + r] * b[c * 4 + 1] +
                a[2 * 4 + r] * b[c * 4 + 2] +
                a[3 * 4 + r] * b[c * 4 + 3];
        }
    }
}

void lerpMat4(const float a[16], const float b[16], float t, float out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
}

ShipNodeAnim::Part* ShipNodeAnim::find(const char* name) {
    for (auto& p : m_parts) if (p.name == name) return &p;
    return nullptr;
}
const ShipNodeAnim::Part* ShipNodeAnim::find(const char* name) const {
    for (auto& p : m_parts) if (p.name == name) return &p;
    return nullptr;
}

void ShipNodeAnim::bind(x3::game::Scene& scene, uint32_t shipEntity) {
    m_ship = shipEntity;
    if (shipEntity < scene.size()) {
        std::memcpy(m_rootWorld, scene.get(shipEntity).transform, sizeof(m_rootWorld));
    }
}

void ShipNodeAnim::addPart(const char* nodeName,
                           const float poseA[16], const float poseB[16],
                           uint32_t childEntity) {
    Part* existing = find(nodeName);
    Part& p = existing ? *existing : (m_parts.push_back(Part{}), m_parts.back());
    p.name  = nodeName;
    std::memcpy(p.poseA, poseA, sizeof(p.poseA));
    std::memcpy(p.poseB, poseB, sizeof(p.poseB));
    p.child = childEntity;
    // t preserved on re-add so a redefinition doesn't snap the part back.
}

void ShipNodeAnim::setPart(const char* nodeName, float t) {
    Part* p = find(nodeName);
    if (!p) return;
    p->t = std::clamp(t, 0.0f, 1.0f);
    // No Scene at hand here; partValue() reflects it immediately and update()
    // pushes it to the child entity. (The showcase calls update() every frame.)
}

float ShipNodeAnim::partValue(const char* nodeName) const {
    const Part* p = find(nodeName);
    return p ? p->t : -1.0f;
}

void ShipNodeAnim::apply(x3::game::Scene& scene, const Part& p) {
    if (p.child >= scene.size()) return;
    // local = lerp(retracted, deployed) in ship-local space.
    float local[16];
    lerpMat4(p.poseA, p.poseB, p.t, local);
    // world = rootWorld * local, so the part rides the ship's placement.
    float world[16];
    mulMat4(m_rootWorld, local, world);
    std::memcpy(scene.get(p.child).transform, world, sizeof(world));
}

void ShipNodeAnim::update(float /*dt*/, x3::game::Scene& scene) {
    if (m_ship >= scene.size()) return;
    // Re-read the root each frame so an external turntable rotation carries the
    // articulated parts. (The host writes the root transform; we compose onto it.)
    std::memcpy(m_rootWorld, scene.get(m_ship).transform, sizeof(m_rootWorld));
    for (const Part& p : m_parts) apply(scene, p);
}

} // namespace x3::space
