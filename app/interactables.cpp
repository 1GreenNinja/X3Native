// INTERACTABLES framework impl (W9-1). See interactables.h.
#include "interactables.h"

#include "engine/core/x3_log.h"

namespace x3::game {

// ---------------------------------------------------------------------------
// ItemStore
// ---------------------------------------------------------------------------
void ItemStore::add(std::string_view id, int n) {
    if (n <= 0) return;
    for (Row& r : rows)
        if (r.id == id) { r.count += n; return; }
    rows.push_back(Row{ std::string(id), n });
}

int ItemStore::count(std::string_view id) const {
    for (const Row& r : rows)
        if (r.id == id) return r.count;
    return 0;
}

bool ItemStore::consume(std::string_view id, int n) {
    if (n <= 0) return false;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].id != id) continue;
        if (rows[i].count < n) return false;
        rows[i].count -= n;
        if (rows[i].count == 0) rows.erase(rows.begin() + i);
        return true;
    }
    return false;
}

bool ItemStore::empty() const {
    for (const Row& r : rows) if (r.count > 0) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Interactables
// ---------------------------------------------------------------------------
uint32_t Interactables::add(InteractPoint p) {
    m_points.push_back(std::move(p));
    return (uint32_t)m_points.size() - 1;
}

namespace {
// Squared distance eye<->point (full 3D so stacked floors never cross-trigger:
// the same XZ 13 m up is a different room).
float dist2(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}
} // namespace

const InteractPoint* Interactables::nearest(const x3::phys::Vec3& eye) const {
    const InteractPoint* best = nullptr;
    float bestD2 = 1e30f;
    for (const InteractPoint& p : m_points) {
        if (p.used && p.oneShot) continue;
        const float d2 = dist2(eye, p.pos);
        if (d2 > p.radius * p.radius) continue;
        if (d2 < bestD2) { bestD2 = d2; best = &p; }
    }
    return best;
}

bool Interactables::onUse(const x3::phys::Vec3& eye, StoryFlags& flags,
                          std::string* barkOut) {
    const InteractPoint* hit = nearest(eye);
    if (!hit) return false;
    InteractPoint& p = m_points[(size_t)(hit - m_points.data())];

    // Flag gate: every required flag must be set, else surface the missing bark.
    for (const std::string& f : p.requiresFlags) {
        if (!flags.has(f)) {
            if (barkOut) *barkOut = p.missingBark;
            x3::logInfo("[interact] '" + p.id + "' gated (missing flag " + f + ")");
            return true;   // the E was consumed by this point (it responded)
        }
    }

    std::string bark;
    if (p.onUse) bark = p.onUse(flags);
    if (p.oneShot) p.used = true;
    if (barkOut) *barkOut = bark;
    x3::logInfo("[interact] '" + p.id + "' used" + (p.oneShot ? " (one-shot)" : ""));
    return true;
}

std::string Interactables::prompt(const x3::phys::Vec3& eye) const {
    const InteractPoint* p = nearest(eye);
    return p ? p->prompt : std::string();
}

InteractPoint* Interactables::find(std::string_view id) {
    for (InteractPoint& p : m_points)
        if (p.id == id) return &p;
    return nullptr;
}

const InteractPoint* Interactables::find(std::string_view id) const {
    for (const InteractPoint& p : m_points)
        if (p.id == id) return &p;
    return nullptr;
}

// ---------------------------------------------------------------------------
// StatusEffects
// ---------------------------------------------------------------------------
void StatusEffects::setChill(bool on) {
    if (on == m_chill) return;
    m_chill = on;
    m_chillT = on ? kChillPeriod : 0.0f;   // first tick lands a period after onset
    x3::logInfo(std::string("[status] chill ") + (on ? "APPLIED (source: cold room)"
                                                     : "cleared"));
}

void StatusEffects::infect() {
    if (m_infected) return;
    m_infected = true;
    m_infectT  = kInfectPeriod;
    x3::logInfo("[status] infection APPLIED (source: creature attack)");
}

void StatusEffects::cureInfection() {
    if (!m_infected) return;
    m_infected = false;
    m_infectT  = 0.0f;
    x3::logInfo("[status] infection CURED");
}

void StatusEffects::tick(float dt, IDamageSink* player) {
    if (dt <= 0.0f) return;
    if (m_chill) {
        m_chillT -= dt;
        while (m_chillT <= 0.0f) {
            m_chillT += kChillPeriod;
            if (player && player->isAlive() && player->takeDamage(kChillDamage)) {
                m_chillDealt += kChillDamage;
                x3::logInfo("[status] chill tick: -" + std::to_string(kChillDamage) +
                            " HP (cold room)");
            }
        }
    }
    if (m_infected) {
        m_infectT -= dt;
        while (m_infectT <= 0.0f) {
            m_infectT += kInfectPeriod;
            if (player && player->isAlive() && player->takeDamage(kInfectDamage)) {
                m_infectDealt += kInfectDamage;
                x3::logInfo("[status] infection tick: -" + std::to_string(kInfectDamage) +
                            " HP (infection)");
            }
        }
    }
}

} // namespace x3::game
