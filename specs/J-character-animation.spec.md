# Spec: Character Animation, Skinning, IK & Active Ragdoll (J / D8)

> Written by the SPEC TEAM. Implemented by the CLEAN-ROOM TEAM from THIS FILE + public refs ONLY.
> ❌ No GPL source, no transcribed function bodies, no RBDOOM identifiers/paths below this line.
>
> **Clean-room basis:** glTF 2.0 spec (skins, inverse-bind, animation channels, morph targets), Jolt docs/samples (`CharacterVirtualTest.cpp`, Ragdoll/Rig tests, Rouwe "Architecting Jolt Physics for Horizon Forbidden West" GDC 2022), public game-feel & animation literature (cited §19). No id Tech/RBDOOM source; no purchased C# copied — behaviors/values only.

- **Ledger ID:** D8 (animation/skinning) — also defines the new `IAnimSystem` + `IPhysicsWorld` character/ragdoll extensions
- **Implements interface:** `IAnimSystem` (`engine/anim/IAnimSystem.h`) + extensions to `IPhysicsWorld`
- **Status:** WIP — **J1 (CPU skinning + Idle clip on rigged glTF) is SHIPPING** (`app/anim.*`, Martinez=`chief_martinez.glb`). This v2 spec is the full target; implement in the tier order below.
- **Spec author machine:** 14900K · **Clean-room target machine:** 13700K

## 0. Feature tiers (the bar: idTech 8 feature-for-feature, then beyond)

| Tier | Scope | Why |
|---|---|---|
| **T0 — shipped** | glTF skeleton load, single-clip sampling, **CPU** linear-blend skinning, Idle on a rigged character | proves the skin/pose path end-to-end |
| **T1 — near** | clip crossfade + **inertialization** blends, 1D/2D blend trees, additive + masked (upper/lower) layers, root motion, two-bone + foot IK, notify/event tracks | the "character v2" game-feel layer |
| **T2 — idTech 8 parity** | **GPU compute skinning** into the GPU-driven renderer (bone palettes in SSBOs, bindless), parallel pose sampling on the **job system**, animation LOD/update-rate scaling, **active/powered ragdoll** (PD motors) with physical hit-reactions + get-up, morph-target/facial, **VAT crowd path** for massive on-screen counts | idTech 8 pillar #1 (jobs-everything) + #4 (massive GPU animation/ragdoll counts) |
| **T3 — beyond** | **motion matching** locomotion, full-body IK pass (pelvis + spine + look-at + hand-to-weapon), GPU-skinned ragdolls at crowd scale, animation-driven cloth/secondary motion hooks, deterministic replay of the anim graph | exceed idTech 8's published feature set |

Everything below is firewalled behind clean interfaces; `JPH::`/Vulkan types never leak into public headers (opaque handles + POD structs).

---

## 1. Purpose
Turn rigged glTF assets into animated, skinned, physically-reactive characters: sample skeletal clips into poses, blend them (graph/state machine/inertialization), optionally solve IK, skin the mesh (CPU now, GPU at scale), and hand off to/from a ragdoll for hits and death. One system serves the player, NPCs, and crowd-scale background actors via LOD tiers.

---

## 2. Interface contract (clean, authored fresh)

```cpp
// engine/anim/IAnimSystem.h — opaque handles; no JPH/Vulkan/glTF types in this header
using SkeletonHandle = uint32_t;   // 0 == invalid
using ClipHandle     = uint32_t;
using InstanceHandle = uint32_t;
using GraphHandle    = uint32_t;

struct JointPose { float t[3]; float q[4]; float s[3]; };          // TRS, quat xyzw
struct AnimBudget { uint32_t maxFullRate; uint32_t maxHalfRate; uint32_t maxPosed; };

class IAnimSystem {
public:
    virtual ~IAnimSystem() = default;

    // ---- assets (data comes from the glTF/M2 loader as POD, not glTF types) ----
    virtual SkeletonHandle createSkeleton(const SkeletonDesc&) = 0;   // joints, parents, inverse-bind
    virtual ClipHandle     createClip(const ClipDesc&) = 0;           // sampled/keyframe channels, duration, loop, root-motion track
    virtual void           destroySkeleton(SkeletonHandle) = 0;
    virtual void           destroyClip(ClipHandle) = 0;

    // ---- instances ----
    virtual InstanceHandle createInstance(SkeletonHandle) = 0;
    virtual void           destroyInstance(InstanceHandle) = 0;
    virtual void           setLodTier(InstanceHandle, uint8_t tier) = 0;   // 0=full,1=half,2=posed-only,3=culled
    virtual void           setBudget(const AnimBudget&) = 0;

    // ---- playback (simple API; the graph API §5 sits on top) ----
    virtual void  play(InstanceHandle, ClipHandle, float fadeSec = 0.15f, bool loop = true) = 0;
    virtual void  setPlayRate(InstanceHandle, float rate) = 0;
    virtual float clipDuration(ClipHandle) const = 0;

    // ---- graph (state machine + blend trees, §5) ----
    virtual GraphHandle createGraph(const GraphDesc&) = 0;
    virtual void        bindGraph(InstanceHandle, GraphHandle) = 0;
    virtual void        setParam(InstanceHandle, uint32_t paramId, float value) = 0;     // blend params, triggers
    virtual void        setParamVec2(InstanceHandle, uint32_t paramId, float x, float y) = 0;

    // ---- per-frame ----
    // sampleAll() is job-parallel internally; safe to call once per frame from the main thread.
    virtual void update(float dt) = 0;

    // ---- pose / skinning output ----
    virtual const JointPose* localPose(InstanceHandle, uint32_t& count) const = 0;       // post-graph, pre-IK
    virtual const float*     skinningMatrices(InstanceHandle, uint32_t& count) const = 0; // 4x3 row-major, model space (CPU path)
    virtual bool             gpuSkinningEnabled() const = 0;                              // T2: matrices uploaded to SSBO instead
    virtual uint32_t         bonePaletteSSBO(InstanceHandle) const = 0;                   // opaque buffer id for the renderer (T2)

    // ---- IK (§9) ----
    virtual void setIkTwoBone(InstanceHandle, uint32_t endJoint, const float target[3], float weight) = 0;
    virtual void setIkLookAt (InstanceHandle, uint32_t headJoint, const float target[3], float weight) = 0;
    virtual void setFootIkEnabled(InstanceHandle, bool) = 0;

    // ---- ragdoll handoff (§10) ----
    virtual void setRagdollBlend(InstanceHandle, float physics01) = 0;   // 0=animation .. 1=full ragdoll
    virtual void poweredRagdollDriveToCurrentPose(InstanceHandle) = 0;   // active-ragdoll target = current anim pose

    // ---- events (§12) ----
    using NotifyFn = void(*)(InstanceHandle, uint32_t notifyId, const float pos[3], void* user);
    virtual void setNotifyCallback(NotifyFn, void* user) = 0;
};
```

```cpp
// engine/physics/IPhysicsWorld.h — additions required by J (still opaque; no JPH types)
// Character:
virtual void  setCharacterShape(BodyId, ShapeId, bool onlyIfClear) = 0;   // crouch swap (returns via out-param/event)
virtual int   getCharacterGroundState(BodyId) = 0;                        // 0=onGround,1=inAir,2=onSteep
virtual void  characterExtendedUpdate(BodyId, const CharUpdateParams&) = 0;// move+stairs+stickToFloor+edge-removal
virtual bool  castRay(const float o[3], const float d[3], float maxT, RayHit& out) = 0;     // ledge/foot IK
virtual bool  collideShape(ShapeId, const float xform[16], ShapeHit& out) = 0;
// Ragdoll (new):
virtual RagdollId createRagdoll(const RagdollDesc&) = 0;     // bodies + constraints from a skeleton mapping
virtual void      ragdollActivateFromPose(RagdollId, const JointPose* worldPose, uint32_t n) = 0;
virtual void      ragdollDriveToPose(RagdollId, const JointPose* targetPose, uint32_t n, float strength01) = 0; // PD motors
virtual void      ragdollGetPose(RagdollId, JointPose* out, uint32_t n) = 0;
virtual void      destroyRagdoll(RagdollId) = 0;
```

---

## 3. Skeleton & pose representation
- Joints as flat arrays: `parentIndex[]` (topologically sorted, parent < child), bind `JointPose[]`, `inverseBind[]` (4x3). From glTF skin (M2 already parses skins/inverse-bind/animation channels).
- Pose pipeline per instance per frame: **sample → blend (graph) → IK → local→model accumulate → × inverseBind = skinning matrices**.
- Model-space accumulation is a single pass over the sorted joint array (each joint composes with its parent's already-computed model matrix).

## 4. Clip sampling + compression
- Channels: translation, rotation (quaternion, **nlerp** with sign-correction for speed; slerp only where flagged), scale. Sample by binary-search on keyframe times with cached cursor (clips advance monotonically → O(1) amortized).
- **Compression (T2):** quantize rotations (e.g. smallest-three, 48-bit) and translations (16-bit per-channel ranges); drop constant tracks to a single key; optional curve-fit tolerance. Decompress in the sampler. Target ≥5× size reduction vs raw float keys with no visible error at gameplay distance.
- Looping clips wrap phase; one-shots clamp + emit an end event.

## 5. Animation graph (state machine + blend trees + inertialization)
- **State machine:** nodes = states (each a clip or a blend tree); edges = transitions with conditions on params/triggers, duration, and a blend mode.
- **Blend trees:** 1D (e.g. speed→idle/walk/run) and 2D (directional locomotion, e.g. velocity x/y over a clip set). Barycentric/bilinear weight from the param point.
- **Layers:** additive layers (e.g. aim offset, breathing) summed onto the base; **masked layers** drive a joint subset (upper body shoots while lower body runs) via a per-joint weight mask.
- **Inertialization (preferred transition blend):** instead of cross-fading two full poses, capture the pose+velocity delta at transition start and decay it to zero over the transition window (Bobby Anguelov / "Inertialization" GDC 2018, Gears). Cheaper and pop-free; falls back to linear crossfade if a node opts out.
- The simple `play()` API (§2) is sugar over a two-state graph.

## 6. Motion matching (T3 — beyond)
- Optional locomotion node: a feature database (root trajectory + foot positions/velocities sampled from a motion set) queried each frame against the desired trajectory (from input/AI) → pick best frame, inertialize to it. Replaces hand-built locomotion blend trees for high-fidelity movement. Ubisoft "Motion Matching" (GDC 2016) + Holden "Learned Motion Matching" as references. Gated behind the graph node interface so it's swappable per-character.

## 7. Root motion
- Clips may carry a root-motion track; per frame extract the delta transform of the root joint and forward it to the controller (`CharacterVirtual` velocity) instead of moving the visual root. Toggle per-clip (locomotion uses root motion; aim offsets don't).

## 8. Skinning — CPU (shipped) → GPU compute (idTech 8 parity)
- **T0 CPU path (shipping):** compute `skinningMatrices[]` on the CPU, linear-blend skin on upload or in the vertex shader from a per-instance UBO. Fine for low counts.
- **T2 GPU path:** upload bone palettes to per-instance **SSBO** slices (`DEVICE_LOCAL`, double-buffered); a **compute pre-pass** skins vertices into a transformed vertex buffer consumed by the GPU-driven renderer (subsystems C/D), OR skin in the vertex shader reading the palette via **bindless** SSBO index. Compute-skinning is required to feed multidraw-indirect + culling without a CPU bottleneck and to share skinned verts across shadow + depth + main passes.
- **Dual-quaternion skinning** optional for twisty joints (flagged per mesh); linear-blend default.
- Pose sampling + matrix build run on the **job system** (one job per instance, batched), per idTech 8 pillar #1.

## 9. IK suite
- **Two-bone IK** (analytic) for limbs: given end target + pole vector, solve hip/knee or shoulder/elbow.
- **FABRIK** for longer chains (spine, tails) where analytic doesn't fit.
- **Foot IK:** raycast down from each foot's predicted plant; place foot at hit + lift, rotate to ground normal; **pelvis adjustment** lowers the hips so both feet can reach on slopes/steps; blend by ground-state + speed. Footstep timing → notify (§12) → audio.
- **Look-at IK:** head + spine chain aims at a target with per-joint weight + clamp cone.
- **Hand-to-weapon IK (T3):** off-hand snaps to a weapon grip socket; respects two-handed poses during ADS.
- IK runs after graph blend, before model-space accumulation; each has a 0..1 weight for smooth enable/disable.

## 10. Ragdoll + active/powered ragdoll
- **Ragdoll asset:** low-detail body+constraint skeleton (capsules/boxes + swing-twist/hinge/6DOF limits) mapped to the animation skeleton. Built via `createRagdoll` from a `RagdollDesc` (clean POD, no `JPH::`).
- **Death/hard hit:** `ragdollActivateFromPose(currentWorldPose)` so the ragdoll starts matching the current animation (no snap), then `setRagdollBlend → 1` over 0.3–0.8 s; `ragdollGetPose` feeds back into the skinning pose.
- **Active/powered ragdoll:** `ragdollDriveToPose(animTargetPose, strength)` uses **PD motors** to pull joints toward the animated pose — enables stagger/flinch that stays partly animated, and physically plausible balance. Per-joint strength mask (e.g. legs stiffer than torso during a hit).
- **Physical hit reactions (T2):** on damage, raise physics blend on the impacted bone-region only for ~0.2–0.5 s (additive physical layer) while the base animation keeps playing — "get shot in the shoulder, shoulder reacts, character keeps running."
- **Get-up:** when ragdoll settles (low KE), pick a get-up clip matching final orientation (face-up/face-down), `ragdollActivateFromPose`-blend back to animation.
- **GPU-skinned ragdolls at crowd scale (T3):** ragdoll poses written to the same bone-palette SSBOs so dozens/hundreds of dying actors skin on the GPU.

## 11. Morph targets / facial (T2)
- glTF morph targets (blend shapes): per-instance weight array applied in the skinning compute/vertex stage. Used for facial expression, visemes, and mesh damage states. Weights animatable as graph params.

## 12. Notify / event tracks
- Clips carry a timeline of notifies `{time, id, optional joint}`. On crossing a notify time during `update`, fire `NotifyFn(instance, id, worldPos, user)`. Standard ids: footstep L/R, weapon_fire, sheathe, hit-frame (melee active window), vfx spawn, sound cue. Decouples animation from gameplay/audio/VFX.

## 13. Crowd & instanced animation — VAT (T2/T3, "massive counts")
- For background/crowd actors beyond the per-instance skinning budget: bake clips to a **Vertex Animation Texture** (VAT) or bone-matrix texture; the GPU samples the texture by `(instanceId, time)` so thousands of actors animate with ~zero CPU and one draw per mesh+clip. LOD switches a character from full graph/skinning (near) → VAT (far). This is the path to idTech 8-scale on-screen counts.

## 14. LOD + budgeting (jobs-everything)
- **Update-rate scaling:** tier 0 full-rate sampling; tier 1 half-rate with inertialized interpolation between updates; tier 2 frozen/posed-only; tier 3 culled. Tier from screen size + distance + visibility.
- **Bone LOD:** distant instances skin a reduced bone set (merge fingers/toes).
- **Budget:** `AnimBudget` caps full-rate instances; overflow demotes by priority (player/visible/near first). All sampling jobs submit to the one engine job system (no private thread pool) so anim never fights physics/render for cores — idTech 8 pillar #1.

## 15. Controller core (preserved from seed; near-term cheap wins)
- `CharacterVirtual` (kinematic, query-only) is the player/NPC controller. Upgrade S3's manual move to Jolt **`ExtendedUpdate`** (move + `WalkStairs` + `StickToFloor` + internal-edge removal); tune `mPredictiveContactDistance`, `mPenetrationRecoverySpeed`, `mMaxSlopeAngle`; optional inner body so debris can push the player.
- **State machine:** `Normal · Crouched · LedgeGrabbing · Vaulting · Ragdoll · Dead` with smooth blend factors; camera height follows crouch/recoil.
- **Crouch:** two capsules (stand r0.5 h0.95, crouch r0.5 h0.45); `setCharacterShape(crouch)`; stand only if headroom clear.
- **Ledge grab/vault:** forward+up ray probe (`fwd*0.7 + up*0.4`); clear standing space above → `LedgeGrabbing` (freeze+anchor); jump → `Vaulting` (~0.6 s) → `Normal`.
- **Health/damage/weapon:** `applyDamage(amount,hitPoint,hitDir)` → flinch (physical hit-reaction §10) or death (full ragdoll); weapons honor fire-rate, recoil pushes camera, ADS lowers spread. All sound via notifies (§12) → audio M9.

## 16. Edge cases & error handling
- Mesh with no skin → render static, skip anim instance (warn once).
- Clip/skeleton joint-count mismatch → reject clip at `createClip`, return invalid handle + log.
- NaN/denormal in a pose (bad data) → clamp/identity that joint, log once per asset.
- Ragdoll requested before physics ready → no-op + warn; auto-bind when physics comes up.
- Budget overflow → graceful demotion, never a hitch; never heap-allocate in `update`.
- Device loss (GPU skinning) → renderer recreates SSBOs; anim re-uploads palettes next frame.
- Foot-IK ray misses (gap/ledge) → fall back to animated foot, no snap.

## 17. Performance targets
- **CPU sample+blend+matrix:** ≤ **6 µs** per full-rate humanoid (~70 joints) single-thread; fully parallel across the job system. 200 full-rate characters ≤ **1.5 ms** total wall-time on the 14900K.
- **GPU compute skinning:** ≤ **0.4 ms** for 200 skinned characters @ ~70 bones, 1080 Ti; ≤ 0.15 ms on the 5090.
- **Crowd VAT:** 2,000+ actors at < 0.5 ms CPU, one draw per mesh+clip.
- **Ragdolls:** 32 simultaneous active ragdolls within the physics frame budget on the 1080 Ti.
- **No per-frame heap allocation** in the sample/blend/skin hot path. Stable frame time (idTech 8 pillar #1) — variance, not just average, is a gate.

## 18. Acceptance tests (clean-room team implements these)
1. **T1 — skeleton load:** a rigged glTF → skeleton with correct joint count + parent topology; bind pose matches DCC.
2. **T2 — clip sample:** Idle plays, loops seamlessly (no phase pop at wrap), correct duration.
3. **T3 — CPU skin parity:** skinned mesh matches a reference frame within tolerance (already at T0).
4. **T4 — crossfade/inertialization:** idle→run transition shows no foot-snap pop; transition completes in the requested window.
5. **T5 — blend tree:** speed param sweep 0→1 blends idle→walk→run monotonically; 2D directional blend has no discontinuity at quadrant boundaries.
6. **T6 — masked layer:** upper-body fire while lower-body runs; lower body unaffected by the fire clip.
7. **T7 — root motion:** a root-motion locomotion clip moves the `CharacterVirtual` the authored distance ±2%.
8. **T8 — two-bone IK:** end-effector reaches a reachable target within ε; unreachable target → straight limb, no jitter.
9. **T9 — foot IK:** on a 20° slope and a stair step, both feet plant on the surface with pelvis adjust; no floating/sinking.
10. **T10 — notifies:** footstep notifies fire at the authored times (L/R alternate); a one-shot end event fires exactly once.
11. **T11 — ragdoll activate:** on death, ragdoll starts at the current animated pose (no snap), settles; `ragdollGetPose` drives the skin.
12. **T12 — powered ragdoll:** drive-to-pose with strength 0.5 keeps a hit character partially upright then recovers via a get-up clip.
13. **T13 — GPU skinning:** GPU-skinned output matches the CPU path within tolerance; renderer consumes the bone-palette SSBO via bindless.
14. **T14 — LOD/budget:** with budget below the active count, demotion occurs by priority, frame time stays flat (no hitch); promoted-back instances resync without pop.
15. **T15 — crowd VAT:** 1,000 VAT actors animate from texture, one draw per mesh+clip, CPU cost under target.
16. **T16 — determinism (T3):** same inputs + fixed dt → identical graph param/pose stream across two runs (replay).

## 19. Public references
- glTF 2.0 spec — skins, inverse-bind matrices, animation channels/samplers, morph targets.
- Jolt Physics docs + samples — `CharacterVirtual` / `ExtendedUpdate`, Ragdoll, constraint motors; Rouwe, "Architecting Jolt Physics for Horizon Forbidden West," GDC 2022.
- Anguelov, "Inertialization: High-Performance Animation Transitions" (GDC 2018, Gears of War).
- Clavet, "Motion Matching and The Road to Next-Gen Animation" (GDC 2016); Holden et al., "Learned Motion Matching" (SIGGRAPH 2020).
- "Real-Time Rendering, 4th ed." — skinning, dual-quaternion skinning.
- Aristidou & Lasenby, "FABRIK: A fast, iterative solver for the Inverse Kinematics problem."
- Vertex Animation Textures: public GDC/UE/Houdini VAT material.
- Vulkan spec — compute pipelines, SSBOs, descriptor indexing (bindless), timeline semaphores.

## 20. Suggested permissive libraries (clean IP)
- Jolt Physics (MIT) — character, ragdoll, constraints.
- cgltf (MIT) — glTF parse (via M2 loader; anim consumes POD, not cgltf types).
- glm (MIT) — math. Optionally **ozz-animation (MIT)** as a reference/■fast-path for sampling, compression, blending, and IK if hand-rolling proves slow — it is permissive and shippable; evaluate vs. in-house.
- meshoptimizer (MIT) — vertex/animation optimization.

## 21. Build order & dependencies
- **Now (T1):** `ExtendedUpdate` + crouch (small `IPhysicsWorld` ext) → crossfade/inertialization → blend trees + masked layers → root motion → two-bone + foot IK → notifies. These need only the CPU path already shipping.
- **After renderer core C/D (T2):** GPU compute skinning + bone-palette SSBOs + job-parallel sampling + LOD/budget; morph targets; VAT crowd path.
- **After ragdoll subsystem in `IPhysicsWorld` (T2):** active/powered ragdoll + physical hit-reactions + get-up.
- **T3 (beyond):** motion matching, full-body IK, GPU-skinned crowd ragdolls, deterministic replay.
- Depends on: M2 (glTF skins, done), the job system A (for parallel sampling), C/D (GPU-driven renderer) for the GPU path, audio M9 (notify→sound), and the new ragdoll extensions to `IPhysicsWorld`.
