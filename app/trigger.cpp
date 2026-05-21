// Trigger volumes (Level 1 / §6.3). See app/trigger.h.
//
// Clean-room: built from the Vec3 interface + std only.
#include "trigger.h"

#include "engine/core/x3_log.h"

#include <string>

namespace x3::game {

bool pointInBox(const x3::phys::Vec3& p, const x3::phys::Vec3& min, const x3::phys::Vec3& max) {
    return p.x >= min.x && p.x <= max.x &&
           p.y >= min.y && p.y <= max.y &&
           p.z >= min.z && p.z <= max.z;
}

uint32_t TriggerSystem::add(const x3::phys::Vec3& min, const x3::phys::Vec3& max,
                            uint32_t id, bool enabled) {
    uint32_t i = (uint32_t)m_vols.size();
    TriggerVolume v;
    v.min = min; v.max = max; v.id = id; v.fired = false; v.enabled = enabled;
    m_vols.push_back(v);
    return i;
}

TriggerVolume* TriggerSystem::findById(uint32_t id) {
    for (TriggerVolume& v : m_vols)
        if (v.id == id) return &v;
    return nullptr;
}

const TriggerVolume* TriggerSystem::findById(uint32_t id) const {
    for (const TriggerVolume& v : m_vols)
        if (v.id == id) return &v;
    return nullptr;
}

void TriggerSystem::setEnabled(uint32_t id, bool enabled) {
    if (TriggerVolume* v = findById(id)) v->enabled = enabled;
}

std::vector<uint32_t> TriggerSystem::update(const x3::phys::Vec3& point) {
    std::vector<uint32_t> fired;
    for (TriggerVolume& v : m_vols) {
        if (v.fired || !v.enabled) continue;
        if (pointInBox(point, v.min, v.max)) {
            v.fired = true;
            fired.push_back(v.id);
        }
    }
    return fired;
}

// ===========================================================================
// Headless self-test (folded into --test-level1). No physics/device required.
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[trigger-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[trigger-test] FAIL ") + name); }
}
} // namespace

bool runTriggerSelfTest() {
    g_pass = g_fail = 0;

    // pointInBox boundary sanity.
    {
        x3::phys::Vec3 mn{ 0, 0, 0 }, mx{ 2, 2, 2 };
        bool in   = pointInBox(x3::phys::Vec3{ 1, 1, 1 }, mn, mx);
        bool edge = pointInBox(x3::phys::Vec3{ 0, 2, 0 }, mn, mx);  // on the boundary
        bool out  = pointInBox(x3::phys::Vec3{ 3, 1, 1 }, mn, mx);
        check(in && edge && !out, "pointInBox: inside/edge in, outside out");
    }

    TriggerSystem ts;
    const uint32_t kStrength = 1, kWin = 2;
    ts.add(x3::phys::Vec3{ 0, 0, 0 }, x3::phys::Vec3{ 2, 3, 2 }, kStrength, /*enabled*/true);
    ts.add(x3::phys::Vec3{ 10, 0, 0 }, x3::phys::Vec3{ 12, 3, 2 }, kWin, /*enabled*/false);

    // Far away: nothing fires.
    {
        auto f = ts.update(x3::phys::Vec3{ -5, 1, -5 });
        check(f.empty(), "far position fires nothing");
    }

    // Enter the strength trigger: fires once with id kStrength.
    {
        auto f = ts.update(x3::phys::Vec3{ 1, 1, 1 });
        check(f.size() == 1 && f[0] == kStrength, "entering strength trigger fires once");
    }

    // Stay inside: does NOT fire again (latched).
    {
        auto f = ts.update(x3::phys::Vec3{ 1, 1, 1 });
        check(f.empty(), "staying inside does not re-fire");
    }

    // The disabled win trigger does NOT fire even when the point is inside it.
    {
        auto f = ts.update(x3::phys::Vec3{ 11, 1, 1 });
        check(f.empty(), "disabled trigger does not fire");
    }

    // Enable the win trigger, then enter: fires once.
    {
        ts.setEnabled(kWin, true);
        auto f = ts.update(x3::phys::Vec3{ 11, 1, 1 });
        check(f.size() == 1 && f[0] == kWin, "enabling then entering fires the win trigger");
    }

    x3::logInfo(std::string("[trigger-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
