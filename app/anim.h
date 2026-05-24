#pragma once
// Skeletal animation + CPU skinning runtime (J1). See app/anim.cpp.
//
// Clean-room: built only from the engine's own IModelLoader Model data (skins /
// nodes / animation clips parsed by the M2 cgltf loader), public glTF 2.0 +
// linear-blend-skinning math, and the IRenderDevice::updateMesh re-upload hook.
// No GPL / id Tech / RBDOOM source consulted.
//
// WHAT IT DOES
//   Given a loaded skinned Model + an active clip + a playback time, it:
//     1. samples each animated node's local Translation/Rotation/Scale from the
//        clip (LINEAR for T/S, SLERP for rotation quats), falling back to the
//        node's bind-pose TRS for un-animated channels;
//     2. composes global node matrices down the hierarchy;
//     3. builds jointMatrix[j] = globalNode[joint[j]] * inverseBind[j];
//     4. CPU-skins every skinned primitive's bind-pose vertices
//        (p' = sum_i w_i * jointMatrix[idx_i] * p, normal with the upper 3x3)
//        and re-uploads them via IRenderDevice::updateMesh.
//
// SCOPE: CPU skinning for a HANDFUL of characters. It deliberately does NOT touch
// the GPU-driven multidraw/bindless path or add a second pipeline — it just
// rewrites each animated mesh's vertex buffer. Unskinned models / models with no
// animation are left untouched (the caller keeps drawing them statically).

#include "engine/asset/IModelLoader.h"
#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace x3::anim {

// A reusable per-character skinning state. Construct once over a loaded Model
// (cheap: it caches sizes + scratch buffers), then call update() each frame with
// the chosen clip + time. Holds no GPU resources of its own — it drives the
// device's existing meshes through updateMesh.
class Skinner {
public:
    // Bind to a model. Returns true if the model is actually skinnable (has a skin
    // with joints, at least one animation clip, and at least one skinned primitive
    // whose mesh was uploaded to a real device). If false, the caller should keep
    // drawing the model statically (no regression). Safe to call with a model that
    // has no skin/anim — it just reports false.
    bool bind(const x3::asset::Model& model);

    bool valid() const { return m_valid; }

    // ======================================================================
    // GPU compute skinning (GPU SKINNING OF MODELS).
    // ======================================================================
    // After bind(), call enableGpuSkinning() ONCE with the device to register every
    // skinned primitive's bind-pose verts + per-vertex joint idx/weights with the
    // device's compute-skinning path (IRenderDevice::registerSkinnedMesh). When this
    // succeeds, apply()/applyLocomotion() switch from per-frame CPU linear-blend-
    // skinning + full updateMesh() (the "doesn't scale past a handful of NPCs"
    // bottleneck) to: compute the joint palette on the CPU (cheap, unchanged) and
    // UPLOAD it to the GPU (setSkinnedPalette); the device's compute pre-pass skins
    // on the GPU. The existing draw passes draw the SAME mesh handles unchanged.
    //
    // Returns true if GPU skinning was enabled for at least one primitive (the device
    // supports it AND a real device mesh exists). Returns false on a headless / non-
    // compute device, in which case the CPU path stays in effect (no regression — so
    // --test-anim / --test-locomotion, which run headless palette-only, are unchanged).
    // Safe to call when !valid() (returns false). Idempotent.
    bool enableGpuSkinning(x3::rhi::IRenderDevice& device, const x3::asset::Model& model);

    // True if enableGpuSkinning() registered at least one primitive (apply/
    // applyLocomotion will upload the palette + skin on the GPU instead of CPU LBS).
    bool gpuSkinning() const { return m_gpuSkin; }

    // Number of clips + a clip's name/duration (for selecting idle vs walk and for
    // logging). clipIndex is clamped/ignored if out of range.
    uint32_t   clipCount() const { return (uint32_t)m_clipDurations.size(); }
    float      clipDuration(uint32_t clip) const;
    std::string_view clipName(uint32_t clip) const;

    // Find a clip whose (lower-cased) name contains ANY of the given substrings;
    // returns its index or -1 if none match. Used to locate an "idle" / "walk"
    // clip by fuzzy name (clip names vary by exporter). Order of `keys` is a
    // priority list (first match wins).
    int findClip(std::initializer_list<const char*> keys) const;

    // Advance + apply: sample `clip` at `timeSec` (looped over the clip duration),
    // recompute the joint palette, CPU-skin every skinned primitive, and re-upload
    // each via device.updateMesh. No-op if !valid(). `timeSec` may grow unbounded
    // (it is wrapped internally). Cheap enough for a few characters at 60 Hz.
    void apply(const x3::asset::Model& model, x3::rhi::IRenderDevice& device,
               uint32_t clip, float timeSec);

    // Diagnostic for the self-test: compute the joint palette at a time WITHOUT
    // touching the device, copying it into `outPalette` (16 floats per joint,
    // column-major). Returns the joint count. Lets --test-anim assert the palette
    // changes between two times.
    uint32_t computePalette(const x3::asset::Model& model, uint32_t clip,
                            float timeSec, std::vector<float>& outPalette) const;

    // ======================================================================
    // Ragdoll blend — drive the skin from an EXTERNAL physics pose (Physics §2).
    // ======================================================================
    // An external pose source (a phys::IRagdoll) supplies per-bone WORLD transforms
    // (column-major 4x4, 16 floats each) keyed by bone NAME. resolveExternalBones()
    // matches each external bone name to a model skin-joint (case-insensitive) ONCE
    // after bind(); the per-frame blend then needs no string work. The blend weight
    // (0 = pure animated pose, 1 = pure ragdoll) is applied per matched joint by
    // interpolating the model-space joint transform between the animated global and
    // the ragdoll world transform (translation lerp + rotation nlerp), so the SAME
    // skinned mesh follows the ragdoll. Unmatched joints keep the animated pose, so a
    // PARTIAL ragdoll (e.g. only the limbs physical) blends naturally.
    //
    // IMPORTANT: the ragdoll's world transforms must be in the SAME space the skin
    // joints' model-space globals live in (i.e. the character authored at world
    // origin, or the caller pre-transforms ragdoll->model space). For the demo +
    // self-test the character is authored at the origin so model==world.

    // Resolve the external bone names to model skin joints (once, after bind()).
    // `boneNames`/`count` is the ragdoll's bone list (order matches the world-matrix
    // array passed to applyRagdollBlend). Returns the number of bones that matched a
    // skin joint. Safe to call when !valid() (returns 0). Idempotent.
    uint32_t resolveExternalBones(const x3::asset::Model& model,
                                  const char* const* boneNames, uint32_t count);

    // Number of external bones resolved by the last resolveExternalBones().
    uint32_t externalBonesResolved() const { return m_extResolvedCount; }

    // Compute the blended palette at (clip,timeSec) with the external ragdoll world
    // transforms `extWorld` (count*16 floats, same order as resolveExternalBones())
    // mixed in by `weight` (clamped 0..1), into outPalette. WITHOUT a device — the
    // self-test uses this to assert the blend interpolates monotonically. Returns the
    // joint count. weight<=0 -> pure animated palette; weight>=1 -> pure ragdoll for
    // matched joints. extWorld may be null (treated as weight 0).
    uint32_t computeRagdollBlendedPalette(const x3::asset::Model& model, uint32_t clip,
                                          float timeSec, const float* extWorld,
                                          uint32_t extCount, float weight,
                                          std::vector<float>& outPalette) const;

    // Apply the ragdoll-blended pose: compute the blended palette and upload/skin it
    // exactly like apply() (GPU palette upload when enabled, else CPU LBS + updateMesh).
    // No-op if !valid(). This is the runtime path a death/impact ragdoll drives each
    // frame; ramp `weight` 0->1 to fall into ragdoll and 1->0 to blend back.
    void applyRagdollBlend(const x3::asset::Model& model, x3::rhi::IRenderDevice& device,
                           uint32_t clip, float timeSec, const float* extWorld,
                           uint32_t extCount, float weight);

    // ======================================================================
    // T1 — locomotion blend + crossfade / inertialization (Animation T1).
    // ======================================================================
    // The blend layer sits on top of the clip sampler. It keeps a single
    // normalized PHASE (0..1) so the bracketing locomotion clips stay foot-
    // synced as the speed param sweeps Idle->Walk->Run, blends their LOCAL poses
    // per node (lerp T/S, slerp R), and — on a discrete state change such as
    // entering Jump — decays the previous pose toward the new one over a short
    // window (timed crossfade with smoothstep easing, an inertialization-style
    // pop-free transition). All blend math reuses member scratch (no per-frame
    // heap alloc in the steady path). Characters with only an Idle clip degrade
    // gracefully (the blend collapses to Idle).

    // Register the locomotion clip set + the authored ground speed (m/s) each
    // clip represents. Any index may be -1 (absent); the blend degenerates
    // gracefully. `walkSpeed`/`runSpeed` are the m/s the Walk/Run clips were
    // authored for and define where the 1D blend reaches "pure walk"/"pure run".
    // Call once after bind().
    void setLocomotionClips(int idleClip, int walkClip, int runClip,
                            float walkSpeed = 1.5f, float runSpeed = 4.0f);

    // Set the desired planar movement speed (m/s). Mapped internally to the 1D
    // blend weight across Idle(0) -> Walk(walkSpeed) -> Run(runSpeed). Cheap;
    // stores the target (advanceBlend does the work).
    void setLocomotionSpeed(float speedMetersPerSec);

    // Set the blend by a normalized 0..1 param directly (0=idle, ~0.5=walk,
    // 1=run), bypassing the m/s mapping. Used by --test-locomotion.
    void setLocomotion01(float speed01);

    // Trigger a one-shot crossfade toward a discrete clip (e.g. Jump) over
    // `fadeSec`: the current blended pose decays out as the target clip fades in,
    // so there's no snap. Non-looping targets play once then auto-return to the
    // locomotion blend; pass loop=true for a looping target. clip<0 cancels back
    // to the locomotion blend (also crossfaded).
    void triggerClip(int clip, float fadeSec = 0.2f, bool loop = false);

    // Advance the blend by dt and CPU-skin + re-upload via the device. The
    // locomotion-aware analogue of apply(): samples/blends the active locomotion
    // clips (phase-continuous) and any active crossfade, builds the palette from
    // the BLENDED local pose, and skins. No-op if !valid().
    void applyLocomotion(const x3::asset::Model& model, x3::rhi::IRenderDevice& device,
                         float dt);

    // Diagnostic (--test-locomotion): advance the blend by dt and compute the
    // resulting joint palette WITHOUT a device, into `outPalette`. Returns the
    // joint count. Lets the test assert the blended palette tracks the speed
    // param + differs across speeds, and that a crossfade is continuous.
    uint32_t advanceAndComputePalette(const x3::asset::Model& model, float dt,
                                      std::vector<float>& outPalette);

    // Current internal 1D blend weight in [0,1] (0=idle .. 1=run) and the active
    // crossfade weight in [0,1] (1 = fully on the triggered clip). For tests/HUD.
    float locomotionWeight() const { return m_locoW; }
    float crossfadeWeight() const { return m_xfadeActive ? m_xfadeW : 0.0f; }
    bool  hasLocomotion()   const { return m_idleClip >= 0 || m_walkClip >= 0 || m_runClip >= 0; }

    // ======================================================================
    // Foot IK — GENERAL character grounding (slopes, stairs, uneven terrain).
    // ======================================================================
    // Sits AFTER the pose blend and BEFORE the final model-space accumulate. For
    // each resolved leg (hip->knee->foot) it: (1) raycasts DOWN from the foot's
    // current model-space plant position to find the ground, (2) plants the foot
    // at the hit + a small lift and aligns its sole to the ground normal, and (3)
    // lowers the PELVIS (model root) so both feet can reach on a slope/step (the
    // lower foot governs), smoothed to avoid pops and clamped to a sane range.
    // The leg is then solved with an analytic TWO-BONE solver. Each contribution
    // is scaled by a smoothed 0..1 weight. If the rig's leg/hips bones don't
    // resolve by name the whole pass gracefully no-ops (no regression).
    //
    // The raycast is supplied as a callback so anim never depends on the physics
    // lib directly: given a world-space origin + a (downward) dir + maxDist, fill
    // hitY/normal and return true on a hit. `worldFromModel` (the character's
    // placement matrix, column-major) maps the model-space foot to world for the
    // ray and the hit back to model space. Pass an identity matrix if the model is
    // already authored in world space.
    struct GroundRay {
        // origin/dir are WORLD space; maxDist meters. On a hit, write the world-space
        // hit point into hit[3] and the surface normal into normal[3] and return true.
        using Fn = bool(*)(const float origin[3], const float dir[3], float maxDist,
                           float hit[3], float normal[3], void* user);
        Fn    fn   = nullptr;
        void* user = nullptr;
    };

    // Enable/disable foot IK and supply the ground-ray callback. `worldFromModel`
    // is the character's model->world transform (column-major 4x4); the IK uses it
    // to cast in world space and bring hits back to model space. When `ray.fn` is
    // null the pass is inert. Cheap; resolution happens lazily on the next apply.
    void setFootIk(bool enabled, const GroundRay& ray,
                   const float worldFromModel[16] = nullptr);

    // Update just the placement matrix (e.g. the character moved) without changing
    // the enabled state or callback. Column-major 4x4.
    void setFootIkWorldFromModel(const float worldFromModel[16]);

    bool  footIkEnabled() const { return m_footIkEnabled; }
    // Did the LAST bind() resolve at least one full leg chain (hip+knee+foot)? For
    // tests/HUD. Resolution is by humanoid bone name (see resolveFootIkBones()).
    bool  footIkResolved() const { return m_legResolved; }
    // Resolved bone node-name for the test report; side 0=left,1=right,
    // part 0=hip(upperleg) 1=knee(lowerleg) 2=foot 3=pelvis/hips. "" if unresolved.
    std::string_view footIkBoneName(int side, int part) const;
    // The smoothed per-leg IK weight and the pelvis drop applied last frame (HUD).
    float footIkLegWeight(int side) const { return (side >= 0 && side < 2) ? m_legW[side] : 0.0f; }
    float footIkPelvisDrop() const { return m_pelvisDropSmoothed; }

    // ---- Analytic two-bone IK (public + static so --test-footik can exercise it
    // in isolation). Given the hip joint world position, the rest world positions
    // of the knee + foot (which define the two bone lengths), a foot TARGET, and a
    // knee POLE hint, compute the corrected WORLD positions of the knee + foot so
    // the foot reaches the target with a natural bend in the pole direction. The
    // bone lengths are taken from |knee-hip| and |foot-knee|. An unreachable target
    // yields a straight (fully extended) leg pointing at the target (no jitter).
    // outKnee/outFoot may equal the corresponding inputs. Pure geometry; no rig.
    static void solveTwoBone(const float hip[3], const float knee[3],
                             const float foot[3], const float target[3],
                             const float pole[3], float outKnee[3], float outFoot[3]);

    // Current shared normalized locomotion phase in [0,1). Foot-synced across the
    // bracketing clips, so a caller can detect a FOOT-PLANT by watching this wrap
    // (or cross the 0.0 / 0.5 half-cycle marks) and fire a footstep cue. Read-only
    // (the blend owns advancement); 0 when the blend isn't driving (idle collapse).
    float locomotionPhase() const { return m_phase; }

private:
    // Sample one node's local TRS from the clip at time t (bind-pose fallback),
    // composing the result into a column-major 4x4.
    void sampleNodeLocal(const x3::asset::Model& m, uint32_t clip, int node,
                         float t, float out[16]) const;

    // Sample one node's local TRS from `clip` at time `t` into raw T/R/S (bind-
    // pose fallback). The decomposed half of sampleNodeLocal(), used by the blend
    // layer so it can mix two clips' local poses BEFORE composing to matrices.
    void sampleNodeTRS(const x3::asset::Model& m, uint32_t clip, int node, float t,
                       float T[3], float R[4], float S[3]) const;

    // Sample EVERY node's local pose from `clip` at `t` into the flat caller
    // arrays (3,4,3 floats per node). Reused, not reallocated, in the steady path.
    void sampleClipPose(const x3::asset::Model& m, uint32_t clip, float t,
                        std::vector<float>& poseT, std::vector<float>& poseR,
                        std::vector<float>& poseS) const;

    // Build global node matrices + the joint palette from a set of per-node LOCAL
    // poses (the blend output) into outPalette, reusing the member scratch.
    // Mirrors computeGlobals()+computePalette() but consumes blended T/R/S.
    // Returns the joint count.
    uint32_t paletteFromPose(const x3::asset::Model& m,
                             const std::vector<float>& poseT,
                             const std::vector<float>& poseR,
                             const std::vector<float>& poseS,
                             std::vector<float>& outPalette) const;

    // Advance the blend by dt and produce the blended per-node LOCAL pose into
    // the member m_blendT/R/S scratch. Shared by applyLocomotion() and
    // advanceAndComputePalette(). Returns false if !valid().
    bool advanceBlend(const x3::asset::Model& model, float dt);

    // Build global node matrices for all nodes at time t into `globals` (caller
    // sizes it to nodeCount*16; reused, not reallocated). Resolves parents on the
    // fly (nodes may be in any order). Uses the member done/inprog/stack scratch
    // so the steady per-frame path performs no heap allocation.
    void computeGlobals(const x3::asset::Model& m, uint32_t clip, float t,
                        std::vector<float>& globals) const;

    // ---- Foot IK internals ----
    // Resolve the humanoid leg + hips bones by name (case-insensitive, common
    // conventions: UpperLeg/thigh/UpLeg, LowerLeg/shin/Leg, Foot/ankle, Hips/pelvis;
    // .L/.R or Left/Right). Called from bind(); sets m_legResolved + the bone-length
    // members. Idempotent; a rig that doesn't resolve leaves m_legResolved=false so
    // the IK pass no-ops.
    void resolveFootIkBones(const x3::asset::Model& m);

    // Run the foot-IK pass over the blended LOCAL pose (m_blendT/R/S) IN PLACE:
    // accumulate globals, plant each foot via the ground ray, adjust the pelvis,
    // and solve each leg analytically, writing corrected hip/knee/foot LOCAL
    // rotations + the pelvis offset back into m_blendT/R. dt drives the weight +
    // pelvis smoothing. No-op (returns) if !enabled / unresolved / no ray. Reuses
    // m_globalScratch + member IK scratch (no per-frame heap alloc in the steady path).
    void applyFootIk(const x3::asset::Model& m, float dt);

    bool                  m_valid = false;
    // GPU compute skinning: set by enableGpuSkinning(). When true, apply/
    // applyLocomotion upload the joint palette (setSkinnedPalette) instead of CPU-
    // LBS + updateMesh. Holds no GPU resources itself (the device owns them, keyed
    // by mesh handle, freed on destroyMesh / unregisterSkinnedMesh).
    bool                  m_gpuSkin = false;
    int                   m_skinIndex = -1;
    std::vector<float>    m_clipDurations;     // seconds, per clip
    std::vector<std::string> m_clipNames;
    // Per-clip, per-node channel lookup: for each node, the index of its T/R/S
    // channel in the clip (-1 = none). Flattened [clip][node][3].
    std::vector<int>      m_channelLut;        // size = clipCount * nodeCount * 3
    uint32_t              m_nodeCount = 0;
    // Scratch reused across apply() to avoid per-frame allocation. All sized in
    // bind(); per-call code only resets/clears (no realloc) in the steady path.
    mutable std::vector<float>             m_globalScratch;  // nodeCount * 16
    mutable std::vector<float>             m_palette;        // jointCount * 16
    mutable std::vector<x3::rhi::MeshVertex> m_vertScratch;  // per-primitive
    // computeGlobals() hierarchy-resolve scratch (sized to nodeCount in bind()).
    mutable std::vector<char>              m_resolveDone;    // nodeCount
    mutable std::vector<char>              m_resolveInProg;  // nodeCount
    mutable std::vector<int>               m_resolveStack;   // up to nodeCount

    // ---- T1 locomotion-blend state. Sized in bind(); the steady per-frame
    // blend path only reads/writes these (no realloc). ----
    int    m_idleClip   = -1;     // locomotion clip indices (-1 = absent)
    int    m_walkClip   = -1;
    int    m_runClip    = -1;
    float  m_walkSpeed  = 1.5f;   // m/s the Walk clip is authored for
    float  m_runSpeed   = 4.0f;   // m/s the Run clip is authored for
    float  m_speedCmd   = 0.0f;   // requested planar speed (m/s); -1 sentinel via setLocomotion01
    float  m_loco01Cmd  = -1.0f;  // direct 0..1 override (>=0 used instead of m_speedCmd)
    float  m_locoW      = 0.0f;   // current smoothed 1D blend weight [0,1] (0=idle,1=run)
    float  m_phase      = 0.0f;   // shared normalized locomotion phase [0,1)

    // One-shot crossfade (discrete transitions, e.g. -> Jump). When active, the
    // base locomotion pose is blended toward the triggered clip's pose by a
    // smoothstepped weight that ramps 0->1 over m_xfadeDur, then (for a non-loop
    // target) plays out and ramps back to the locomotion blend.
    bool   m_xfadeActive = false;
    int    m_xfadeClip   = -1;    // the discrete target clip
    bool   m_xfadeLoop   = false; // does the target loop?
    float  m_xfadeW      = 0.0f;  // current crossfade weight [0,1] (1 = fully on target)
    float  m_xfadeDur    = 0.2f;  // ramp duration (s)
    float  m_xfadeTime   = 0.0f;  // time accumulated in the ramp-in
    float  m_xfadeClipT  = 0.0f;  // playback time within the target clip
    bool   m_xfadeOut    = false; // ramping back out to locomotion (non-loop done)

    // Blend scratch (per-node local pose). poseA/poseB are the two bracketing
    // clips; m_blend* is the mixed result handed to paletteFromPose().
    mutable std::vector<float> m_poseAT, m_poseAR, m_poseAS;  // bracket A T/R/S
    mutable std::vector<float> m_poseBT, m_poseBR, m_poseBS;  // bracket B T/R/S
    mutable std::vector<float> m_blendT, m_blendR, m_blendS;  // blended T/R/S
    mutable std::vector<float> m_xfadeT, m_xfadeR, m_xfadeS;  // crossfade target pose

    // ---- Foot-IK state. Resolved once in bind(); the per-frame pass reads these
    // + writes corrected rotations into m_blend*. No per-frame heap alloc. ----
    bool        m_footIkEnabled = false;
    GroundRay   m_groundRay{};
    float       m_worldFromModel[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    bool        m_legResolved = false;        // any full leg chain resolved?
    // Per side (0=L,1=R): node indices for hip(upperleg)/knee(lowerleg)/foot, the
    // pelvis (shared), and the rest bone lengths (knee = |knee-hip|, foot=|foot-knee|).
    int         m_hipNode[2]  = { -1, -1 };
    int         m_kneeNode[2] = { -1, -1 };
    int         m_footNode[2] = { -1, -1 };
    int         m_pelvisNode  = -1;
    int         m_rootNode    = -1;           // pelvis or topmost ancestor used for the lift
    float       m_upperLen[2] = { 0, 0 };     // |knee - hip| in rest pose (model units)
    float       m_lowerLen[2] = { 0, 0 };     // |foot - knee| in rest pose
    bool        m_sideOk[2]   = { false, false };
    std::string m_boneName[2][4];             // [side][hip,knee,foot,pelvis] for the report
    // Smoothed per-leg weights (ramp in/out so enabling/disabling never pops) and
    // the smoothed pelvis drop (model units, >=0 lowers the hips).
    float       m_legW[2] = { 0, 0 };
    float       m_pelvisDropSmoothed = 0.0f;
    // Upload (GPU) or CPU-LBS + updateMesh every skinned primitive from m_palette.
    // The shared tail of apply()/applyRagdollBlend (avoids duplicating the skin loop).
    void skinWithCurrentPalette(const x3::asset::Model& model,
                                x3::rhi::IRenderDevice& device, uint32_t jcount);

    // IK scratch (sized in bind()): a working copy of the blended globals.
    mutable std::vector<float> m_ikGlobals;   // nodeCount * 16

    // ---- Ragdoll-blend state (Physics §2). resolveExternalBones() fills a map
    // from external-bone index -> skin-joint index (-1 if unmatched). The per-frame
    // blend reads it; no string work in the steady path. ----
    std::vector<int> m_extToJoint;       // size = external bone count; joint idx or -1
    uint32_t         m_extResolvedCount = 0;  // # matched bones
};

// Headless self-test (--test-anim): synthesize a tiny rigged GLB (1 bone bending
// over time), load it, and assert (a) the loader reports a skin + a clip, (b) the
// joint palette differs between t=0 and t=mid, and (c) a known skinned vertex
// actually moves. Returns true iff all checks pass. No window / Vulkan.
//
// T1 EXTENSION: when a multi-clip locomotion GLB is present on disk (default
// chief_martinez_anim.glb), it ALSO drives the locomotion blend through a speed
// sweep and asserts: speed=0 -> ~Idle pose; mid -> Walk-dominant; high -> Run-
// dominant (the blended palette tracks the param + differs across speeds), and a
// Jump crossfade produces no per-frame discontinuity. Falls back to the
// synthetic-only checks (still PASS) if the asset is absent (clean checkout).
bool runAnimSelfTest();

// Headless self-test (--test-locomotion <glb>): load a multi-clip locomotion GLB
// and exercise the 1D blend + crossfade explicitly (idle/walk/run dominance +
// monotonic sweep + pop-free Jump crossfade). Returns true iff all checks pass.
// If `glbPath` is empty it defaults to chief_martinez_anim.glb. No window/Vulkan.
bool runLocomotionSelfTest(const std::string& glbPath);

// Headless self-test (--test-footik): exercises the foot-IK suite with NO window:
//   F1 two-bone solver reaches a REACHABLE target within epsilon (foot==target,
//      both bone lengths preserved, knee bent toward the pole).
//   F2 an UNREACHABLE target yields a straight (fully-extended) leg pointing at
//      the target, with no jitter (two distinct over-reach targets give a stable,
//      colinear hip->knee->foot).
//   F3 a synthetic two-leg rig resolves hip/knee/foot/pelvis by bone name, and a
//      flat-ground plant + a sloped plant leave the feet on the surface (not
//      floating/sinking) with a bounded, smoothed pelvis drop on the step.
//   F4 (present-asset) the REAL rig (chief_martinez_anim.glb when on disk) resolves
//      at least one full leg chain; the resolved bone names are logged. Skipped
//      (still PASS) on a clean checkout where the asset is absent.
// Returns true iff all checks pass. No window / Vulkan.
bool runFootIkSelfTest();

} // namespace x3::anim
