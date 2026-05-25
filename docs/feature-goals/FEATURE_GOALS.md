# X3Native — Feature Goals / To-Add

Captured **2026-05-23** while running the UE 5.7 "Introduction to Unreal Engine"
tutorial on `i5000`. These are behaviors we want X3Native's own engine to match.
Reference screenshots live in [`screenshots/`](screenshots/).

---

## 1. Physics — interactive constrained bodies
**Ref:** `screenshots/03-physics-hanging-cubes.png` (UE tutorial "Physics" 9/16)

Goal: replicate the tutorial's **cubes hanging from a point above** that **swing when a
player walks into them**, then settle.

Two primitives:
- **Suspended body** = a rigid body held by a **point/ball joint** to a fixed anchor
  above → pendulum swing under gravity.
- **Player response** = player is a **kinematic capsule**; on contact, apply a
  **collision impulse** to the body; **damping** settles the swing.

Systems required:
- [ ] Rigid-body dynamics (mass, inertia tensor, linear/angular velocity, gravity, semi-implicit Euler)
- [ ] Broadphase (AABB grid / sweep-and-prune / BVH)
- [ ] Narrowphase contacts (box–box, capsule–box) → points + normal + penetration depth
- [ ] Constraint solver resolving **joints + contacts** together
- [ ] Point/ball joint (the "hang from above")
- [ ] Linear + angular damping
- [ ] Solver approach: **XPBD** (preferred — unified, stable) or sequential-impulse (Box2D/Catto)

## 2. Ragdoll / skeletal animation
**Ref:** `screenshots/05-game-animation-sample.png` (UE "Game Animation Sample")

Goal: physics-driven ragdoll blended with skeletal animation.
- [ ] Skeletal bodies + per-bone joint constraints (a ragdoll *is* a constraint chain)
- [ ] Blend animation → physics-driven ragdoll (on death/impact)
- [ ] Blend back / partial ragdoll (physical animation on a live skeleton)
- [ ] Reuse the same constraint solver from §1

---

## Engine-capability reference (UE samples, for scope/quality bar)
- `screenshots/01-content-drawer.png` — content browsing / drag-to-place workflow
- `screenshots/02-details-panel.png` — per-actor property inspection/editing
- `screenshots/04-sample-city.png`, `06-sample-hillside.png`, `07-samples-overview.png` — UE showcase samples (visual/scale bar to aim at)

> See the engine physics discussion notes; XPBD is the recommended foundation since it
> handles both the suspended-body joints (§1) and the ragdoll constraint chains (§2)
> with one stable solver.
