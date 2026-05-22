# Spec: Advanced Elevator (vertical transit for The Spire)

> SPEC TEAM (5090). Game-layer (`app/elevator.*`), engine/ stays pure. Drives the B1→F7 vertical traversal in `EFLZ_SPIRE_7FLOOR.spec.md`.

## 1. Purpose
A multi-floor elevator that **physically carries the player** (and ride-along bodies) between floor heights, called from a panel, with per-floor doors, smooth accel/decel, audio, and clearance/keycode gating. The spine that connects the 7 floors + the basement + (later) the deep tunnels.

## 2. Core model
- **Cab platform** = a Static-layer body (mass 0) moved by `setBodyPosition` each frame (same proven technique as `DoorSystem`). It is the floor the player stands on; while still it blocks like ground.
- **Floor stops**: an ordered list of world Y heights (one per served floor), with a floor label/index. `currentFloor`, `targetFloor`.
- **State machine:** `Idle → DoorsClosing → Traveling → Arriving → DoorsOpening → Idle`. Travel uses ease-in/ease-out (accel then decel) over the height delta, capped at a max speed; `Arriving` snaps to the exact stop Y.

## 3. Player-carry (the make-or-break piece)
Per frame, in the host loop, in this order:
1. `player.update(...)` (gravity + input → moveCharacter).
2. `elevator.update(dt)` — moves the cab platform; returns this frame's cab **ΔY**.
3. **Carry:** if the player is *riding* (feet XZ within the cab footprint **and** feet Y within ~0.3 m of the cab top **and** grounded), add the cab ΔY to the player: `physics.setBodyPosition(player.body(), {px, py + ΔY, pz})`. (Ride-along enemies/props: same test, same add.)
4. `physics.step(dt)`; `scene.update()`.
Carry only the vertical delta (horizontal is the player's own). Going down, the platform stays under the player (grounded keeps contact); going up, the explicit ΔY add prevents the platform shooting out from under the capsule. Tune the ride feel live (max speed, accel) — verify by riding on the 5090.

## 4. Call + selection (the "advanced" UI)
- **Call panel** (interact, E) at each floor lobby + **inside the cab**. Outside: call the cab to this floor. Inside: a floor list — press the floor's number (reuse the keypad digit-capture) or look-at + E a floor button → set `targetFloor`.
- HUD while in the panel: `ELEVATOR — FLOOR: B1  ▸ select 1-7`. Reuse the door-keypad input pattern (digits/Enter).
- **Gating:** a floor may require **clearance/keycode** (ties to the door-code keypad) — e.g. F6/F7 locked until the player has the exec keycard or enters a code. Reuse `Door`-style `locked`/`code`.

## 5. Per-floor doors
Each stop has a door pair (reuse `DoorSystem` / the real `SM_Door`/`SM_DoorPanel` meshes) that **open only when the cab is present + stopped** at that floor, and close before travel. Cab inner doors + floor outer doors animate together.

## 6. Interface (clean, game-layer)
```cpp
// app/elevator.h
struct ElevatorStop { float y; int label; bool locked = false; int code = 0; };
class ElevatorSystem {
public:
    uint32_t build(Scene&, x3::rhi::IRenderDevice&, x3::phys::IPhysicsWorld&,
                   const x3::phys::Vec3& shaftXZ, float cabHalfX, float cabHalfZ,
                   const std::vector<ElevatorStop>& stops);
    void callTo(uint32_t elevator, int floorLabel);          // request a stop (gating checked)
    bool playerRiding(uint32_t elevator, const x3::phys::Vec3& feet) const;
    // Advance; returns the cab's vertical delta this frame (host applies carry).
    float update(float dt, Scene&, x3::phys::IPhysicsWorld&);
    int   currentFloor(uint32_t elevator) const;
    bool  isMoving(uint32_t elevator) const;
};
```

## 7. Acceptance
1. Cab idles at B1; player steps on; calls F1 → doors close, cab eases up, **player rides up smoothly** (no fall-through, no launch), arrives, doors open.
2. Calling down works; player stays on the descending cab.
3. A locked floor (code/clearance) refuses until satisfied (keypad).
4. Per-floor doors open only when stopped at that floor.
5. Stable FPS; capture via `--screenshot` at a stop.

## 8. Build order
1. **Core**: one elevator, 2 stops, cab platform + carry + call (E cycles to next stop). Test shaft in Level 1's existing elevator room. ← *first, on a branch.*
2. Accel/decel + multi-stop floor selection (keypad-style).
3. Per-floor doors + audio.
4. Clearance/keycode gating.
5. Drop into the 7-floor Spire as the B1↔F7 spine (with the farm's geometry).

## 9. Notes
- Reuses: `DoorSystem` motion pattern (moved static body), the door-code keypad input/HUD, `setBodyPosition` for carry (proven by respawn).
- The full 7-stop version lands with the Spire geometry (farm); the **core mechanism is engine-side and standalone-testable now**.
