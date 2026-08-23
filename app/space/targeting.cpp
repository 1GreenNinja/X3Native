// app/space/targeting.cpp — S9 targeting / radar / lock-on implementation.
#include "targeting.h"

#include <cmath>
#include <cstring>

namespace x3::space {

namespace {

inline void sub3(const float a[3], const float b[3], float o[3]) {
    o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2];
}
inline float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline float len3(const float a[3]) { return std::sqrt(dot3(a, a)); }

// Normalize into `o`; returns the original length (0 -> o left as a safe +Z).
inline float norm3(const float a[3], float o[3]) {
    float L = len3(a);
    if (L > 1e-8f) { o[0] = a[0] / L; o[1] = a[1] / L; o[2] = a[2] / L; }
    else           { o[0] = 0.0f; o[1] = 0.0f; o[2] = 1.0f; }
    return L;
}

} // namespace

void TargetingSystem::setContacts(const Contact* c, uint32_t n) {
    if (n > kMaxContacts) n = kMaxContacts;
    count_ = n;
    if (n && c) std::memcpy(contacts_, c, sizeof(Contact) * n);

    // Auto-drop the lock if its contact is no longer present — or if the host
    // has since marked it lock-EXEMPT. The capital is fed as a hostile contact
    // every frame (radar blip, HUD bracket, shootable) and must still never be
    // a legal lock, so eligibility is re-checked on every feed, not just once.
    if (locked_) {
        bool stillLegal = false;
        for (uint32_t i = 0; i < count_; ++i)
            if (contacts_[i].id == lockedId_) {
                stillLegal = contacts_[i].lockable;
                break;
            }
        if (!stillLegal) clearLock();
    }
}

void TargetingSystem::setLockEnabled(bool on) {
    lockEnabled_ = on;
    // OFF means OFF: drop any standing lock so no consumer (nose hold, camera
    // look-bias, lead pip, HUD bracket) can read one and contribute anything.
    if (!on) clearLock();
}

int TargetingSystem::lockedIndex() const {
    if (!locked_) return -1;
    for (uint32_t i = 0; i < count_; ++i)
        if (contacts_[i].id == lockedId_) return (int)i;
    return -1;
}

void TargetingSystem::clearLock() {
    locked_ = false;
    lockedId_ = 0;
}

void TargetingSystem::cycleTarget(int dir) {
    if (dir == 0) return;
    if (!lockEnabled_) { clearLock(); return; }   // master switch OFF
    // Build the ordered list of LOCKABLE hostile contact indices.
    int hostileIdx[kMaxContacts];
    int h = 0;
    int curPos = -1; // position of the current lock within the hostile list
    for (uint32_t i = 0; i < count_; ++i) {
        if (!contacts_[i].hostile || !contacts_[i].lockable) continue;
        if (locked_ && contacts_[i].id == lockedId_) curPos = h;
        hostileIdx[h++] = (int)i;
    }
    if (h == 0) { return; } // nothing lockable; keep state untouched.

    int next;
    if (curPos < 0) {
        // No active lock among the hostiles: +1 -> first, -1 -> last.
        next = (dir > 0) ? 0 : (h - 1);
    } else {
        next = (curPos + (dir > 0 ? 1 : -1)) % h;
        if (next < 0) next += h;
    }
    locked_ = true;
    lockedId_ = contacts_[hostileIdx[next]].id;
}

void TargetingSystem::lockNearest(const float fromPos[3], const float fwd[3]) {
    if (!lockEnabled_) { clearLock(); return; }   // master switch OFF
    float f[3];
    norm3(fwd, f);

    int best = -1;
    float bestCos = -2.0f;   // larger = more on-axis
    float bestDist = 0.0f;
    for (uint32_t i = 0; i < count_; ++i) {
        if (!contacts_[i].hostile || !contacts_[i].lockable) continue;
        float to[3];
        sub3(contacts_[i].pos, fromPos, to);
        float n[3];
        float dist = norm3(to, n);
        if (dist < 1e-6f) continue;       // sitting on top of us; skip
        float c = dot3(f, n);
        if (c <= 0.0f) continue;          // behind / perpendicular -> out of cone
        // Pick the most on-axis; tie-break to the nearer one.
        if (c > bestCos + 1e-6f ||
            (std::fabs(c - bestCos) <= 1e-6f && dist < bestDist)) {
            bestCos = c; best = (int)i; bestDist = dist;
        }
    }
    if (best >= 0) {
        locked_ = true;
        lockedId_ = contacts_[best].id;
    }
}

LeadSolution TargetingSystem::computeLead(const float shooterPos[3],
                                          float projSpeed) const {
    LeadSolution sol{};
    sol.valid = false;
    sol.aimPoint[0] = sol.aimPoint[1] = sol.aimPoint[2] = 0.0f;
    sol.distance = 0.0f;

    int li = lockedIndex();
    if (li < 0 || projSpeed <= 1e-6f) return sol;

    const Contact& t = contacts_[(uint32_t)li];

    // Relative position of the target w.r.t. the shooter, and the target's
    // (constant) velocity. We want the smallest t>=0 with
    //   | r + v*t | = projSpeed * t
    // => (v.v - s^2) t^2 + 2 (r.v) t + (r.r) = 0, with s = projSpeed.
    float r[3];
    sub3(t.pos, shooterPos, r);
    const float* v = t.vel;

    const float a = dot3(v, v) - projSpeed * projSpeed;
    const float b = 2.0f * dot3(r, v);
    const float cc = dot3(r, r);

    float tHit = -1.0f;
    if (std::fabs(a) < 1e-6f) {
        // Degenerate (target closing speed ~= projectile speed): linear in t.
        if (std::fabs(b) > 1e-9f) tHit = -cc / b;
    } else {
        const float disc = b * b - 4.0f * a * cc;
        if (disc >= 0.0f) {
            const float sq = std::sqrt(disc);
            const float t1 = (-b + sq) / (2.0f * a);
            const float t2 = (-b - sq) / (2.0f * a);
            // Smallest strictly-positive root is the earliest intercept.
            if (t1 > 1e-6f && (tHit < 0.0f || t1 < tHit)) tHit = t1;
            if (t2 > 1e-6f && (tHit < 0.0f || t2 < tHit)) tHit = t2;
        }
    }

    if (tHit <= 0.0f) return sol; // no forward-in-time intercept.

    // Aim where the target will be at t = tHit.
    sol.aimPoint[0] = t.pos[0] + v[0] * tHit;
    sol.aimPoint[1] = t.pos[1] + v[1] * tHit;
    sol.aimPoint[2] = t.pos[2] + v[2] * tHit;
    float d[3];
    sub3(sol.aimPoint, shooterPos, d);
    sol.distance = len3(d);
    sol.valid = true;
    return sol;
}

uint32_t TargetingSystem::radarBlips(Blip* out, uint32_t max,
                                     const float playerPos[3],
                                     const float playerFwd[3],
                                     float radarRange) const {
    if (!out || max == 0 || radarRange <= 1e-6f) return 0;

    // Build a 2D radar basis from the player's facing, projected onto the
    // horizontal (X/Z) plane: forward -> radar +Y (up), right -> radar +X.
    float fwd2[2] = { playerFwd[0], playerFwd[2] };
    float fl = std::sqrt(fwd2[0] * fwd2[0] + fwd2[1] * fwd2[1]);
    if (fl > 1e-6f) { fwd2[0] /= fl; fwd2[1] /= fl; }
    else            { fwd2[0] = 0.0f; fwd2[1] = 1.0f; } // looking straight up/down
    // Right vector on the plane (90deg clockwise of forward in X/Z).
    const float right2[2] = { fwd2[1], -fwd2[0] };

    uint32_t n = 0;
    for (uint32_t i = 0; i < count_ && n < max; ++i) {
        float rel[3];
        sub3(contacts_[i].pos, playerPos, rel);
        // Range gate uses the planar distance (radar is a top-down disc).
        const float planar[2] = { rel[0], rel[2] };
        const float dist = std::sqrt(planar[0] * planar[0] + planar[1] * planar[1]);
        if (dist > radarRange) continue;

        // Project onto the radar basis, then normalize by range to [-1,1].
        const float fwdComp   = planar[0] * fwd2[0]  + planar[1] * fwd2[1];
        const float rightComp = planar[0] * right2[0] + planar[1] * right2[1];
        Blip& bl = out[n++];
        bl.id = contacts_[i].id;
        bl.radarXY[0] = rightComp / radarRange; // +X = right
        bl.radarXY[1] = fwdComp   / radarRange; // +Y = up (forward)
        bl.hostile = contacts_[i].hostile;
        bl.locked  = (locked_ && contacts_[i].id == lockedId_);
    }
    return n;
}

} // namespace x3::space
