# Spec: Player Character v2 + Animation/Ragdoll (J) — SEED

> **Status: SEED** (captured 2026-05-20 from Tim's reference dumps). The basic FPS controller already ships (S3, `app/player.*` on Jolt `CharacterVirtual`). This spec captures the *full* player-character vision for later — it depends on subsystems we don't have yet (skeletal **animation/skeleton** + **ragdoll**, and **audio** M9), so it lands after the perf spine. Captured now so nothing's lost.
>
> Clean-room: from Jolt docs/samples (`CharacterVirtualTest.cpp`, the Rig tests, "Architecting Jolt Physics for Horizon Forbidden West" GDC 2022) + public game-feel knowledge. No RBDOOM/id Tech source; no purchased C# copied — values/behaviors only.

## 1. Controller core — `CharacterVirtual` (already chosen for S3)
- `CharacterVirtual` (kinematic, collision-queries-only) over `Character` (full rigid body): lightweight, full control over update timing, has stairs/floor-stick/edge handling. Right call for the player; also fine for many NPCs.
- **Near-term win (can do anytime):** upgrade S3's manual `moveCharacter` to Jolt's **`ExtendedUpdate`** — one call that does movement + `WalkStairs` + `StickToFloor` + enhanced internal-edge removal. Tune `mPredictiveContactDistance` (un-stick from seams), `mPenetrationRecoverySpeed`, `mMaxSlopeAngle`. Optional **inner body** (`mInnerBodyShape`) so dynamic debris can push the player.
- Ground state via `GetGroundState()` (`OnGround`/`InAir`/`OnSteepGround`); jump adds vertical velocity when `OnGround`.

## 2. State machine
`Normal · Crouched · LedgeGrabbing · Vaulting · Ragdoll · Dead`, with smooth blend factors (crouch, ragdoll) lerped per frame; camera height offset follows crouch/recoil.

## 3. Crouch — shape swap
Two capsules (stand `r0.5 h0.95`, crouch `r0.5 h0.45`). `SetShape(crouchShape)` to crouch; to stand, `SetShape(standShape)` **only if it returns true** (clear headroom). Crouch speed multiplier + camera dips. **Needs interface ext: `setCharacterShape`.**

## 4. Ledge grab / vault
No built-in support — build from queries: forward+up ray probe (`GetForward()*0.7 + up*0.4`); if it hits and there's clear standing space above the ledge → enter `LedgeGrabbing` (freeze velocity, anchor). On jump → `Vaulting` (set position to ledge + up), timer (~0.6 s) → back to `Normal`. **Needs narrow-phase queries (`CastRay`, `CollideShape`) exposed.**

## 5. Ragdoll + active-ragdoll animation blending
- Low-detail ragdoll skeleton (physics bodies + swing-twist/hinge constraints) mapped to the high-detail animation skeleton. `RagdollSettings` → `Ragdoll` runtime.
- **Active ragdoll:** drive joints toward the animated pose with motors (`DriveToPoseUsingMotors(animPose)`); per-joint blend 0(physics)→1(animation). Alternatives: soft-keyframed (target velocities), kinematic (hard poses).
- **Blend policy:** normal → full animation; hit/death → ramp to full ragdoll over 0.3–0.8 s; recovery → ramp back while playing a get-up clip. `Ragdoll::GetPose()` feeds back into the anim system.
- **Depends on: the animation/skeleton subsystem (below) + glTF skins (M2 already loads skins/inverse-bind/animation channels).**

## 6. Foot IK
Per foot: raycast down from the foot's local offset, place the foot at the hit + small lift, blend toward it. Footstep timing driven by horizontal speed → footstep sound callback.

## 7. Weapon handling (extends slice S5/S6)
Equip a `Weapon{name, fireRate, recoilImpulse, ...}`; fire respects fire-rate; recoil pushes the character/camera; aim state lowers spread/ADS. Hooks for muzzle flash (have it), ammo, reload.

## 8. Health / damage
`mHealth`, `ApplyDamage(amount, hitPoint, hitDir)`; heavy hit → brief flinch-ragdoll; death → `Dead` + full ragdoll. Impact sound callback.

## 9. Sound callbacks (→ audio M9)
`std::function` hooks: `weapon_fire`, `footstep`, `impact_flesh`, etc. — `(name, position, volume, pitch)`. **Wire to the audio backend when M9 lands; until then these are no-ops.**

## 10. Engine / `IPhysicsWorld` extensions required
- Character: `setCharacterShape` (crouch), ground-state enum, `ExtendedUpdate`-equivalent semantics, optional inner body, a **character contact listener** (push debris / footstep events), narrow-phase queries (`castRay`/`collideShape`) for ledge + foot IK.
- **Ragdoll subsystem** (bodies+constraints from a skeleton) — new.
- **Animation/skeleton subsystem** (D8): sample skeletal animation, pose blending, skinning (glTF skins from M2). Prereq for §5/§6 and GPU-skinned rendering.
- **Audio (M9)** for §9.
- All additions keep `JPH::` types out of the headers (opaque handles + plain structs).

## 11. Build order note
Near-term, cheap: §1 `ExtendedUpdate` polish + §3 crouch (small interface ext). The rest (ragdoll, foot IK, anim blending) waits on the animation subsystem; sound waits on M9. Slots in as "character v2" after the perf spine, alongside subsystem J (GPU animation).
