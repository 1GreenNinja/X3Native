// app/space/targeting.h
//
// S9 · Targeting / radar / lock-on — the dogfight HUD layer.
//
// The player-facing targeting system: pick a hostile contact, hold a lock,
// compute the lead-the-target firing solution, and project every contact onto
// a 2D radar disc for the HUD.
//
// INDEPENDENT by design. It targets a generic list of "contacts" (id + world
// position + velocity + hostile flag), fed by the host each frame. It has NO
// dependency on the S8 enemy-ship lane — S8 simply becomes one producer of
// contacts when it lands. Pure headless logic; no GPU.
#pragma once
#include <cstdint>

namespace x3::space {

// One radar/targeting contact. The host rebuilds the contact list every frame
// (positions/velocities in world space). `hostile` gates which contacts are
// lockable + how a blip is drawn.
struct Contact {
    uint32_t id;
    float    pos[3];
    float    vel[3];
    bool     hostile;
};

// The lead-the-target firing solution for a projectile of a given speed: where
// to aim so the projectile intercepts the locked contact. `valid` is false when
// no real, forward-in-time intercept exists (e.g. projectile too slow to catch
// a receding target).
struct LeadSolution {
    bool  valid;
    float aimPoint[3]; // world-space point to put the reticle on
    float distance;    // shooter -> aimPoint distance (range to the lead point)
};

class TargetingSystem {
public:
    // ---- Contact feed -----------------------------------------------------
    // Host pushes the current radar contacts each frame. A copy is taken (the
    // pointer need not outlive the call). If the previously locked id is gone
    // from the new list, the lock is cleared automatically.
    void setContacts(const Contact* c, uint32_t n);

    uint32_t contactCount() const { return count_; }
    const Contact& contact(uint32_t i) const { return contacts_[i]; }

    // ---- Lock management --------------------------------------------------
    // Cycle the lock to the next (+1) / previous (-1) HOSTILE contact, skipping
    // friendlies. With no lock, +1 picks the first hostile, -1 the last. A no-op
    // when there are no hostile contacts.
    void cycleTarget(int dir);

    // Lock the "best" hostile inside the forward cone from `fromPos` looking
    // along `fwd`: the most on-axis hostile (tightest angle to the boresight),
    // ties broken by nearer range. No-op if none are in front.
    void lockNearest(const float fromPos[3], const float fwd[3]);

    void     clearLock();
    bool     hasLock() const { return locked_; }
    uint32_t lockedId() const { return lockedId_; }

    // ---- Firing solution --------------------------------------------------
    // Classic projectile lead: solve the quadratic for the time-to-intercept of
    // a projectile of `projSpeed` fired from `shooterPos` against the locked
    // contact's constant-velocity motion, and return the world-space aim point.
    // `valid=false` if there is no lock or no positive-time intercept exists.
    LeadSolution computeLead(const float shooterPos[3], float projSpeed) const;

    // ---- Radar ------------------------------------------------------------
    // One radar blip: a contact projected into the normalized radar disc
    // (radarXY in [-1,1], forward = up/+Y, right = +X), plus hostile/locked flags.
    struct Blip {
        uint32_t id;
        float    radarXY[2];
        bool     hostile;
        bool     locked;
    };
    // Project every contact within `radarRange` of the player into the radar
    // disc, relative to the player's facing (forward projects to +Y == up).
    // Writes up to `max` blips into `out`; returns the number written.
    uint32_t radarBlips(Blip* out, uint32_t max, const float playerPos[3],
                        const float playerFwd[3], float radarRange) const;

private:
    static constexpr uint32_t kMaxContacts = 256;
    Contact  contacts_[kMaxContacts]{};
    uint32_t count_ = 0;
    bool     locked_ = false;
    uint32_t lockedId_ = 0;

    // index of lockedId_ in contacts_, or -1.
    int lockedIndex() const;
};

} // namespace x3::space
