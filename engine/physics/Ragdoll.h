#pragma once
// Ragdoll — physics-driven skeletal bodies blended with animation (Physics §2).
//
// Clean-room: built ONLY from the Jolt public API (Ragdoll / RagdollSettings /
// Skeleton / SkeletonPose / SwingTwistConstraint — Jolt is MIT, already a
// dependency) plus public physics references. NO Unreal / id Tech / RBDOOM or any
// other game-engine source consulted.
//
// WHAT IT DOES
//   A ragdoll IS a constraint chain: one rigid body (a capsule) per skeleton bone,
//   each connected to its parent bone's body by a cone/swing-twist joint so the
//   limbs bend like joints but can't pull apart. Build one from a bone description
//   (name + parent + world-space bind transform + a bone length/radius), drop it
//   into an IPhysicsWorld, and it falls + collapses naturally under gravity.
//
//   The SAME skinned mesh follows it: read each bone's physics WORLD transform back
//   out (getBoneWorldTransforms) and feed it into the Skinner's external-pose path
//   (anim::Skinner::applyExternalPose) so the GPU-skinned character tracks the
//   ragdoll. A blend weight (0=animated pose .. 1=full ragdoll) supports the
//   death/impact transition AND blend-back / partial (physical-animation) ragdoll.
//
//   JPH:: types are confined to Ragdoll.cpp — this header is plain structs + an
//   opaque interface, exactly like IPhysicsWorld.h.
//
// SCOPE: a handful of ragdolls (CPU; Jolt). No new solver — Jolt drives it.

#include "IPhysicsWorld.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x3::phys {

// One bone of the ragdoll skeleton. The caller supplies these in an order where a
// parent ALWAYS precedes its children (Jolt requires this; build() validates it).
struct RagdollBoneDesc {
    std::string name;                 // bone name (for mapping back to the model skin)
    int   parent = -1;                // index of the parent bone in the desc array, -1 = root
    // Bind-pose WORLD transform of the bone (column-major 4x4). The body is placed
    // here at creation; the constraint to the parent is anchored at this joint.
    float bindWorld[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    // Capsule collider for this bone, authored along the bone's local +Y axis
    // (origin -> child). halfHeight is the cylinder half-length; if 0 the bone is a
    // leaf/extremity and a small sphere-ish capsule is used.
    float halfHeight = 0.1f;          // capsule cylinder half-length (m)
    float radius     = 0.06f;         // capsule radius (m)
    float mass       = 1.0f;          // body mass (kg)
    // Joint limit to the parent (cone half-angle, radians) — how far this bone may
    // swing away from its bind direction. Small = stiff, large = floppy. The root
    // has no parent joint (ignored). Twist is limited to +-twistLimit.
    float coneHalfAngle = 0.7f;       // ~40 deg
    float twistLimit    = 0.3f;       // ~17 deg
};

// Opaque physics ragdoll. Owns a Jolt Ragdoll + its RagdollSettings; add it to an
// IPhysicsWorld (which must outlive it) to simulate. All transforms crossing this
// boundary are column-major 4x4 world matrices, 16 floats per bone, in the bone
// order passed to build().
class IRagdoll {
public:
    virtual ~IRagdoll() = default;

    // Number of bones (== the desc count passed to the factory).
    virtual uint32_t boneCount() const = 0;
    // Bone name by index ("" if out of range) — lets the caller map ragdoll bones
    // back onto the model skin's joints by name.
    virtual std::string_view boneName(uint32_t bone) const = 0;

    // Add the ragdoll's bodies + constraints to the world and (optionally) activate
    // them so it starts simulating immediately. Idempotent-safe: a second call is a
    // no-op while already in the world.
    virtual void addToWorld(bool activate = true) = 0;
    // Remove the ragdoll's bodies + constraints from the world (leaves the object
    // valid; can be re-added). Called automatically on destruction.
    virtual void removeFromWorld() = 0;
    virtual bool inWorld() const = 0;

    // Snap the ragdoll to a pose given as per-bone WORLD matrices (column-major 4x4,
    // 16 floats each, boneCount() of them). Use this to align the ragdoll to the
    // animated pose at the instant of the death/impact trigger so the transition is
    // seamless. Zeroes velocities. No-op if matrices==null.
    virtual void setPoseWorld(const float* worldMatrices) = 0;

    // Read each bone's CURRENT simulated WORLD transform into `outWorldMatrices`
    // (caller sizes it to boneCount()*16 floats, column-major). This is what the
    // Skinner consumes to make the skinned mesh follow the ragdoll.
    virtual void getBoneWorldTransforms(float* outWorldMatrices) const = 0;

    // Apply an impulse to every body (a uniform "blast"/hit) or to one named bone.
    virtual void applyImpulseAll(Vec3 impulse) = 0;
    virtual void applyImpulseBone(uint32_t bone, Vec3 impulse) = 0;

    // True if any body is still awake (the ragdoll is still moving). Once false the
    // ragdoll has settled into a stable collapsed pose.
    virtual bool isActive() const = 0;

    // Axis-aligned world bounds of all bodies (min/max into 3-float arrays). Lets a
    // caller cull / place the character. Safe before addToWorld (returns the bind
    // bounds).
    virtual void worldBounds(float outMin[3], float outMax[3]) const = 0;
};

// Build a ragdoll from a bone description over an existing world. Returns null if
// the description is empty, mis-ordered (a child before its parent), or the Jolt
// ragdoll fails to create. The ragdoll does NOT auto-add to the world — call
// addToWorld(). `world` must outlive the returned ragdoll.
IRagdoll* createRagdoll(IPhysicsWorld& world,
                        const RagdollBoneDesc* bones, uint32_t boneCount);

// Convenience: build a simple humanoid-ish test skeleton (pelvis -> spine -> head,
// + two arms + two legs = 11 bones) standing upright with its pelvis at `originY`,
// into `outBones`. Used by --world ragdoll and --test-ragdoll so both share one
// canonical rig. Returns the bone count.
uint32_t makeHumanoidRagdollBones(float originY, std::vector<RagdollBoneDesc>& outBones);

// Physics §2 self-test (--test-ragdoll): build a ragdoll from a small synthetic
// skeleton, step the sim, and assert the bones fall + settle into a stable pose
// under gravity (bounded, no NaN), the constraint chain holds (bone lengths
// preserved within tol), and an anim<->ragdoll blend weight 0->1 interpolates the
// joint palette monotonically. Prints "ragdoll: X/Y passed", returns true iff all
// pass. Implemented in Ragdoll.cpp.
bool runRagdollSelfTest();

} // namespace x3::phys
