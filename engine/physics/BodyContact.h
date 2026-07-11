#pragma once
// ============================================================================
// BodyContact — bone-surface contact solving + soft-surface indentation.
// X3 ENGINE FEATURE (owner order, 2026-07-11): characters control their bodies;
// body parts must not pass through any surface, but body WEIGHT may indent
// soft surfaces (mattresses, pillows, cushions).
//
// Two layers, both PURE and deterministic (pose in -> corrected pose out; verts
// in -> displaced verts out) so the headless gate simulates exactly what the
// live path runs:
//
//   1) solveBodyContact — a lightweight positional solver over a posed skeleton
//      expressed as contact spheres (one per load-bearing bone: head, spine,
//      pelvis, elbows, hands, knees, heels). Spheres penetrating a RIGID
//      surface are projected out along the contact normal; SOFT surfaces admit
//      penetration up to an indent budget scaled by the bone's mass fraction
//      (the body settles IN, never THROUGH). A damped distance-constraint pass
//      (position-based, 2 sweeps/iteration) re-links the parent chain so the
//      correction propagates instead of stretching joints. NOT a full IK
//      product — a contact pass; run it AFTER animation/pose, before draw.
//      Shaped for both uses: solve ONCE at placement (static staging — the F2
//      rescue captives) or per frame with a small iteration budget (live
//      characters later; cost is O(iterations * bones * surfaces)).
//
//   2) bakeSoftIndentation — vertex displacement for a tessellated soft patch
//      (a mattress overlay built from prims; licensed GLBs stay untouched):
//      each resolved soft contact presses a smooth Wendland-kernel dent of
//      depth (indentBudget * massFrac) into the patch within a falloff radius;
//      overlapping dents take the max (bodies don't double-dent), and normals
//      are re-derived from the indexed faces. Static staging bakes once and
//      uploads via createMesh/updateMesh; the same call per frame gives a
//      moving surface (a struggling body) at updateMesh cost.
//
// CLEAN-ROOM: designed from first principles (position-based projection +
// distance constraints) against the engine's own IPhysicsWorld surface data.
// ============================================================================

#include "IPhysicsWorld.h"   // Vec3 + IPhysicsWorld (surface discovery helper)

#include <cstdint>

namespace x3 { namespace phys {

// One load-bearing bone of a posed skeleton, expressed as a contact sphere.
// `pos` is IN/OUT: the solver corrects it in place. `parent` indexes the bones
// array (-1 = root); trees are fine (two legs, two arms) — each bone links to
// exactly one parent and rest lengths are captured from the INPUT pose.
struct ContactBone {
    int   parent   = -1;
    Vec3  pos{};
    float radius   = 0.06f;   // contact sphere radius (m)
    float massFrac = 0.10f;   // share of body weight this bone carries [0,1]
};

// A surface the body can rest on: a plane through `point` with unit `normal`,
// optionally FINITE (a slab top): when halfU > 0 the surface only exists within
// +-halfU/+-halfV along the two tangent axes. Soft surfaces admit penetration
// up to indentBudget * bone.massFrac; rigid surfaces admit none.
struct ContactSurface {
    Vec3  point{};
    Vec3  normal{ 0.0f, 1.0f, 0.0f };
    Vec3  uAxis{ 1.0f, 0.0f, 0.0f };  // tangent axes (used when finite)
    Vec3  vAxis{ 0.0f, 0.0f, 1.0f };
    float halfU = -1.0f;              // <= 0 -> infinite plane
    float halfV = -1.0f;
    bool  soft  = false;
    float indentBudget = 0.06f;       // max penetration at massFrac == 1 (m)
};

struct BodySolveParams {
    int   iterations = 4;     // outer loops (contact pass + 2 distance sweeps)
    float damping    = 0.8f;  // distance-correction share applied per sweep
    float epsilon    = 1e-4f; // acceptable residual rigid penetration (m)
};

struct BodySolveStats {
    int   contactsResolved = 0;        // sphere-surface projections applied
    float maxRigidPenetration = 0.0f;  // residual after solve (m; want <= eps)
    float maxSoftPenetration  = 0.0f;  // residual soft depth (m; <= budget*frac)
};

// Correct `bones` so no sphere penetrates a rigid surface beyond epsilon and
// soft penetration stays within each bone's budget, preserving parent-child
// distances (within solver tolerance) via damped propagation. Deterministic:
// fixed evaluation order, no RNG, no time dependence.
BodySolveStats solveBodyContact(ContactBone* bones, uint32_t boneCount,
                                const ContactSurface* surfaces, uint32_t surfaceCount,
                                const BodySolveParams& params = {});

// Convenience: discover RIGID support surfaces under a posed body by casting
// straight down from each bone through the physics world (Layer::Static).
// Near-duplicate hits (same plane within `mergeDist`) are merged. Returns the
// number of surfaces written (<= maxOut). Soft surfaces are app data (physics
// has no softness tag) — append those yourself.
uint32_t discoverRigidSurfaces(IPhysicsWorld& world,
                               const ContactBone* bones, uint32_t boneCount,
                               ContactSurface* out, uint32_t maxOut,
                               float castDist = 2.0f, float mergeDist = 0.02f);

struct IndentParams {
    float falloffRadius = 0.22f;  // dent kernel radius around each contact (m)
};

// Press the resolved body into a tessellated SOFT patch: displaces vertex
// positions along -surface.normal with a smooth kernel (depth = indentBudget *
// massFrac at the contact center, feathering to 0 at falloffRadius; overlaps
// take the max). Vertices are raw floats with `strideFloats` between vertices
// (position at offset 0, normal at `normalOffset` floats; pass normalOffset < 0
// to skip normal re-derivation). When `indices` is non-null, normals are
// re-derived from the indexed triangles after displacement. Returns the number
// of displaced vertices. Pure + deterministic.
uint32_t bakeSoftIndentation(const ContactBone* bones, uint32_t boneCount,
                             const ContactSurface& softSurface,
                             float* verts, uint32_t vertCount, uint32_t strideFloats,
                             int normalOffset,
                             const uint32_t* indices, uint32_t indexCount,
                             const IndentParams& params = {});

// Headless self-test (--test-bodycontact): rigid rest, soft settle, indent
// bake shape/scaling, finite-extent gating, determinism. >= 5 checks.
bool runBodyContactSelfTest();

}} // namespace x3::phys
