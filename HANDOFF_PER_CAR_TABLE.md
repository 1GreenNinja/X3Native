# Handoff — the PER-CAR character table

**Tim's ruling:** *"ALL the cars **NEED** his fixes"* and *"Every car gets the
method, with its own variables."*

DS's vehicle work (`laexe/vehicle-feel-gauges`, merged at `5dcc6be8`) built the
METHOD: a real engine model, forced induction with manifold pressure and lag, a
boost gauge, TC deadband, clutch lock, drive telemetry. It is currently ONE car's
character — a 993 Turbo, 2388 lb, flat-six — applied to whatever GLB is loaded.

**Today that means an E46 body making Porsche noises.** The job is to make the
variables per-vehicle while the method stays shared.

---

## Where the pieces already are

| thing | where |
|---|---|
| the knobs | `engine/physics/IVehicle.h` → `struct WheeledTuning` — torque, redline, an 8-point normalised **torque curve**, mass, grip, brake, CoM |
| how a car is tuned live | `car.applyTuning(t)` — already used by `shell.addFloatCommand("car_torque"…)` in `host_tunnel.cpp` |
| the current hardcoded 993 | `app/vehicle.cpp:183` (`800 Nm`), `engine/physics/JoltVehicle.cpp:820,858`, and `host_tunnel.cpp` ~950 |
| the fleet list | `host_tunnel.cpp`, `kFleet[]` — 11 cars, `{glb, display name}` today |
| the parts/upgrade path | `app/vehparts.cpp:459` already writes `tuning.maxEngineTorque` — the per-car base must COMPOSE with this, not fight it |

## The shape of the work

1. **Widen `kFleet[]` into a real table**: per car, a `WheeledTuning` base
   (torque, redline, curve, mass, grip, CoM) + an audio profile + a display name.
   Eleven entries: E46, CTR, M3 E36, E30, Coupe, Muscle, Skyline, Pickup, Jeep,
   Truck, F1.
2. **Apply on load/switch**, so picking a car changes how it DRIVES, not just how
   it looks. That is also what makes the garage screen worth having.
3. **Compose with `vehparts`** — the per-car base is the STOCK figure; upgrades
   modify it. Do not let the table overwrite a tuned build.
4. **Audio per car.** DS's flat-six is a 993's voice. A truck must not have it.

## Rules this codebase holds you to

- Put the table where it is **testable headless** and give it a `--test-*` with a
  **NEGATIVE CONTROL** and a non-empty assertion. Register it across the six
  files the way `--test-tunnelfitout` is.
- **Determinism**: no `rand()`, no clock reads.
- **Units: Tim reads FEET / MPH / FT-LB.** Engine data stays SI; convert at the
  boundary. `car_torque` is already exposed in ft-lb — match that.
- **Anti-slop**: every car's numbers should come from something real. A truck is
  not "the car with numbers scaled down" — it is heavy, low-revving, high-torque,
  soft-sprung. If the table reads as one row multiplied, it has failed.
- Build via **PowerShell + `vcvars64.bat`**. An identical test number twice
  running means it did not rebuild.

## Two traps that already bit, here, today

1. **A green suite proves less than it looks.** The whole merge was green while
   weather was completely dead — two `IConsole` objects existed, `wx` was
   registered on the invisible one, and nothing on screen could reach it. No test
   opens a console. **Drive the thing.**
2. **Prove it is not already built.** Eight times on 2026-08-15 a "missing"
   feature turned out to exist under another name — including the console I
   hand-rolled into one host while `HostShell` was doing it for all 31. Grep the
   CONCEPT and its synonyms before writing anything.

## Where to start reading
`docs/design/VEHICLE_UPGRADES.md` (§2 parts, §3 shop UX, §4 progression) — it
already calls out *"Multiple owned vehicles — `VehicleBuild` is per-car today;
needs a garage collection"*, which is this job's other half.
