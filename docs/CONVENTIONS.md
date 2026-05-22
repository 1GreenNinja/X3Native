# X3Native — Coordinate & Math Conventions (SET IN STONE)

> **Status: CANONICAL / LOCKED (2026-05-21, Tim's call).** This is the single source of truth.
> Any code, asset, shader, or spec that disagrees with this document is a **bug** — fix the code, not this doc.
> Decided: handedness = **right-handed, −Z forward** (matches the glTF asset pipeline + glm; zero churn).

---

## 1. The world coordinate system

**Right-handed, Y-up.**

| Axis | Direction | Tim's words |
|------|-----------|-------------|
| **+X** | **right** (−X = left) | "X being left and right" |
| **+Y** | **up** (−Y = down) | "Y being up and down" |
| **+Z** | **toward the viewer / out of the screen** | "Z being depth" |
| **−Z** | **forward** — INTO the screen / the default direction things face | |

- **Handedness:** right-handed. `+X × +Y = +Z`. Equivalent to glTF / OpenGL / glm defaults.
- **Up vector:** `+Y` = `(0, 1, 0)`. Confirmed in `VulkanRenderDevice` (`up = vec3(0,1,0)`).
- **Gravity:** `−Y` = `(0, −9.81, 0)`. Confirmed in `JoltPhysicsWorld`.
- **Units:** **meters.** (e.g. player capsule ≈ 1.8 m tall, door opening ≈ 1.2 m wide, terrain tiles = 32 m.)
- **Default facing:** an unrotated model/entity faces **−Z** (glTF convention). To make entity A face point P, orient A so its **local −Z** points from A to P.

```
        +Y (up)
         |
         |
         |________ +X (right)
        /
       /
     +Z (toward viewer)         ... so −Z is "forward / into the scene"
```

## 2. Rotations & matrices

- **Quaternions:** glTF order **(x, y, z, w)**, w last. Used by the animation/skinning path and physics get/setBodyRotation.
- **Matrices:** 4×4, **column-major** (glm layout — element `m[12..14]` is the translation column). Composition is `parent * child` (TRS = `T * R * S`).
- **Interpolation:** translation/scale = linear; rotation = SLERP (nlerp fast-path when near-parallel). (See `app/anim.cpp`.)

## 3. The camera / look basis (control parameterization)

Camera orientation is driven by **yaw + pitch** (radians). The forward vector — shared identically by the camera, audio listener, weapon/viewmodel, and FX (`VulkanRenderDevice`, `IRenderDevice.h`, `IAudioSystem.h`, `main.cpp`, `fx.cpp`):

```
forward = ( cos(pitch)·cos(yaw),  sin(pitch),  cos(pitch)·sin(yaw) )
right   = normalize( cross(forward, +Y) )
up      = cross(right, forward)
```

Consequences (memorize these — they're the source of past facing bugs):
- **pitch > 0 looks up** (+Y); pitch < 0 looks down. pitch ∈ (−π/2, +π/2).
- **yaw is measured in the XZ plane from +X toward +Z.** So **yaw = 0 ⇒ forward = +X**, yaw = +π/2 ⇒ forward = +Z, yaw = π ⇒ forward = −X, yaw = −π/2 (or 3π/2) ⇒ forward = −Z.
- ⚠️ **Note the offset:** the *world* "default facing" is **−Z**, but the *camera yaw zero* points **+X**. They are not the same number. When you compute a yaw to "face along world-forward (−Z)", that is **yaw = −π/2**, NOT yaw = 0. Code that wants an entity to face a target should compute `yaw = atan2(dz, dx)` from this same basis (dx, dz = target − self in the XZ plane).

## 4. Asset import (the only sanctioned fixups)

- **glTF / GLB:** already right-handed, +Y up, −Z forward → **imported as-is, no axis conversion.** This is why the engine standardized on glTF.
- **Z-up assets** (some Unity-FBX→glTF exports come out Z-up / lying flat): corrected by a **single −90° rotation about X** at load time (`standUpZtoY = true`). This is the **only** permitted import fixup. Rigged sources that are already Y-up set `standUpZtoY = false`.
- Do **not** introduce per-asset ad-hoc rotations elsewhere; if an asset looks wrong, fix it at import via the standUpZtoY path or re-export it correctly.

## 5. Clip space / NDC (Vulkan specifics — do not "fix" by hand)

- World space is right-handed as defined above. The **view → clip** transform (built with glm in the renderer's projection setup) accounts for Vulkan's clip-space conventions (Y-down in NDC, depth range **0..1**). Front-face winding follows from the right-handed world + that projection — **do not** flip winding or negate axes ad hoc to "make it look right." If something renders inside-out or upside-down, fix the projection setup in one place, not the geometry.

## 6. Audit status (2026-05-21)

Consistent with this doc: world axes, gravity, up vector, the shared forward basis (camera/audio/fx/weapon), glTF import, the standUpZtoY fixup, terrain, lighting/shadow sun direction.

**To reconcile (tracked for the D facing pass):**
- `app/env_art.cpp` has a comment "facing −Z (yaw 180)" — by the §3 basis, **yaw = π gives −X**, not −Z. The comment (and any code relying on it) must be corrected to the §3 rule (face −Z ⇒ yaw = −π/2).
- Monster facing (the new combat-AI work) must derive yaw from the §3 basis (`atan2(dz, dx)`), so "advance/attack faces the player, retreat faces away," etc., are computed against this one convention.
