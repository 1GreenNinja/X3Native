# native-scaffold/ — Clean-room machinery for X3Native

This folder holds the **process scaffolding** for building X3Native (the custom C++ engine) under the HYBRID strategy from `../X3_NATIVE_ENGINE_PLAN.md`. At M1 bootstrap, copy these files into the new engine repo root (`C:\GameDev\X3Native\`).

## What's here

| File | Purpose | Who edits it |
|---|---|---|
| `GPL_DEBT.md` | The ledger of every GPL-derived module + replacement status. Ship gate = empty. | Both teams update status |
| `specs/README.md` | The clean-room protocol — the information-barrier rules. **Read first.** | — |
| `specs/_TEMPLATE.spec.md` | Template the spec team fills in per module. | Spec team (14900K) |
| `specs/D1-render-device.spec.md` | Worked example spec (stub) showing the format. | Spec team (14900K) |

## The one rule that makes this legal

**The 13700K clean-room team must NEVER read the RBDOOM/GPL source.** It implements only from the `specs/*.spec.md` files (written by the 14900K spec team) plus public references. Enforce by repo topology: the 13700K clones a `*-cleanroom` branch that physically omits `engine/_gpl_rbdoom/`. See `specs/README.md`.

## Quick start for the 14900K (M0/M1)

1. Build RBDOOM (M0 in the plan). Get vanilla Doom 3 BFG running.
2. At M1, create `C:\GameDev\X3Native\`, copy this `native-scaffold/` content into its root.
3. Define the abstract interfaces (`engine/rhi/IRenderDevice.h`, etc.) — these are CLEAN from day 1.
4. Wire RBDOOM code in as the v0 impl behind each interface, quarantined in `engine/_gpl_rbdoom/`.
5. As each interface stabilizes, the spec team writes its `.spec.md`; the 13700K clean-room team begins the clean impl.
