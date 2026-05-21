# specs/ — Clean-room protocol

These specs are the **information barrier** between the GPL source and the clean implementation. Get this right and the clean impl is independently created (not a derivative work). Get it wrong and you've just retyped GPL code.

## The two teams

### Spec team — runs on the 14900K (has the RBDOOM source)
- Reads the GPL module.
- Writes a `*.spec.md` describing **what it does and the interface contract** — behavior, inputs, outputs, edge cases, perf characteristics, acceptance tests.
- **Writes NO clean implementation code.** Does not write the `.cpp` that will ship.
- Pseudocode is allowed ONLY as algorithm *description* (e.g., "split the view frustum into N cascades by logarithmic depth"), never as a transcription of the GPL source structure/naming.

### Clean-room team — runs on the 13700K (does NOT have the RBDOOM source)
- Clones a `*-cleanroom` branch/checkout that **physically omits `engine/_gpl_rbdoom/`**.
- Reads ONLY: the `.spec.md` files + public references (Vulkan spec, "Real-Time Rendering 4th ed", GPU Gems, glTF 2.0 spec, library docs).
- Writes the clean `.cpp`/`.h` behind the interface.
- Writes its own tests from the spec's acceptance-test section.
- Commits land on the cleanroom branch — git history shows they were authored on a machine with no GPL checkout (independent-creation evidence).

## What may and may not appear in a spec

✅ Allowed in a `.spec.md`:
- The interface API (function signatures the clean impl must satisfy — these are *yours*, defined clean from day 1)
- Behavioral description in prose
- Input/output contracts, value ranges, units
- Edge cases and error handling expectations
- Performance targets
- Acceptance test cases (inputs → expected outputs)
- Citations to PUBLIC references

❌ NEVER in a `.spec.md`:
- Copy-pasted GPL source
- Function bodies transcribed from RBDOOM
- RBDOOM's exact internal data-structure layouts or identifier names
- RBDOOM file paths as "look here" pointers (defeats the barrier)

## Workflow per module
1. Spec team picks a `TODO` row from `../GPL_DEBT.md`, flips it to `SPEC`.
2. Spec team copies `_TEMPLATE.spec.md` → `D#-<name>.spec.md`, fills it in, commits/pushes. Flips row to `WIP`.
3. Clean-room team (13700K) implements from the spec, writes tests, flips row to `VERIFY`.
4. Swap the interface binding from GPL v0 → clean impl. Run acceptance tests.
5. Green → delete the GPL v0 impl for that module, flip row to `DONE-CLEAN`, record SHA + machine in the audit trail.

## Why the barrier is by repo topology, not honor system
The 13700K's checkout literally cannot contain `engine/_gpl_rbdoom/`. Use one of:
- A separate `x3native-cleanroom` repo that has only `engine/` (clean) + `specs/` + interfaces, no quarantine dir.
- OR git sparse-checkout on the 13700K excluding `engine/_gpl_rbdoom/`.
The clean-room agents physically cannot read what isn't on disk.
