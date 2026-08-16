# Note to DS-Vehicle — read before you trust another `--test-vehicle` number

From the 14900K session (host-shell + HUD lane), 2026-08-15.

## Your clutch-slip diagnosis looks right, and it is independent of everything below

`fwdSpeed=6.7 m/s, rpm=6000` in 1st is definitive. At 15 mph the crank should be
near 2,580 rpm; 6,000 means ~3,400 rpm of pure slip, and Jolt's clutch is
viscous (torque proportional to slip), not a locking friction clutch. That also
explains the symptom Tim reported for days and I kept mis-attributing to
gearing — "It literally shifts five times before it actually changes a gear" —
the engine was free-revving through the ratios while the car sat still. Nothing
in this note weakens that finding. Chase it.

## But I changed torque delivery under you at commit b15c4eeb — RE-BASELINE FIRST

If your 31.8 m / 15.0 m/s run was taken at or after `b15c4eeb`, it was measured
against a **detuned** engine and the absolute number is not comparable to
anything you measured before it.

`DriveDemo::preStep` now calls `updateTurbo(dt)`, which multiplies engine torque
by a manifold-pressure model:

* `floorTorque = 0.60` — with no boost the engine makes **60% of the curve**.
* Boost needs revs (`spoolStartRpm 1800`, `spoolFullRpm 4200`) AND time
  (`spoolTau 0.45 s`), so a standing-start 0-60 spends its first second near
  that floor.
* The headless test path goes through `preStep`, so **`--test-vehicle` is
  running the turbo model too.** It is not a physics-only bypass.

To measure the drivetrain without me in the way:

```
turbo 0            # console, in --world tunnel
```
or in code, `car.setTurboEnabled(false)` before the run — that pins the
multiplier at 1.0 and restores pre-b15c4eeb torque exactly.

**Please re-run your baseline with `turbo 0`.** If 31.8 m holds with the turbo
disabled, the clutch is the whole story and my model is not muddying it. If the
number moves a lot, we need to separate the two effects before either of us
tunes anything.

## Two other semantic changes in the same commit

1. **`setTorqueBoost()` no longer writes through to the controller.** It sets
   `m_userTorqueMult`, and `updateTurbo` each step applies
   `m_ctl->setTorqueBoost(m_userTorqueMult * m_turboMult)`. If you call
   `m_ctl->setTorqueBoost()` directly it will be overwritten on the next step.
   Go through `DriveDemo`, or disable the turbo.

2. **`m_effThrottle` bug fix.** It was assigned only inside the
   `if (m_tcEnabled && throttle > 0)` branch, so with TC off — or the instant
   you lifted — it held its last value. It is now assigned unconditionally
   after the TC block. Engine audio reads it as load; if you have any telemetry
   keyed off it, it changed (for the better).

## Lane split so we do not collide

* **You own** `app/vehicle.cpp` engine/drivetrain internals,
  `engine/physics/JoltVehicle.cpp`, `engine/physics/IVehicle.h` — clutch,
  gearbox, torque, the `--test-vehicle` harness.
* **I own** `app/world_hosts/*` (the new `host_shell.{h,cpp}` and the fan-out to
  ~30 hosts), the HUD/gauge art in `assets/ui` + `tools/compose_gauge_dial.py`.
* **Shared, coordinate before touching:** `app/vehicle.h` — I added a
  `TurboParams` block and four members to it. Ping before restructuring.

If the clutch fix lands and the car finally pulls, the turbo `floorTorque` will
almost certainly want raising (0.60 was chosen against a car that felt slow for
a reason we now know was not the engine). That is a joint call once you have a
clean number.

## What is live to tune with

`--world tunnel`, then `~` for the console:

```
turbo 0|1     turbo_max     turbo_spool   turbo_dump
turbo_start   turbo_full    turbo_floor   turbo_vacuum
car_torque    car_redline   car_grip      car_mass
car_brake     car_ride      car_springfreq  car_springdamp
car_tc        car_reset     car            (prints gear/rpm/mph/psi/TC)
```

Tim's framing, and it is the right one: get the 993 correct and every other car
and truck is a parameter variation off it.
