# Clean-room process — building X3Native from specs

X3Native is **original work, written clean-room from scratch.** No id Tech /
RBDOOM / Doom 3 (or any other engine) source was ever forked, copied, or
consulted. This doc describes the spec-driven discipline used to build it — which
doubles as the evidence record of independent creation (`PROVENANCE.md`).

> **Historical note:** an earlier plan proposed forking RBDOOM (GPL) and then
> "de-GPL-ing" it behind interfaces, guarded by a two-machine information barrier
> and a `GPL_DEBT.md` ledger. **That fork never happened** — the engine went
> straight to clean implementations. So the "barrier" is moot (there was never any
> GPL source to wall off), but the spec-first method below is exactly how the
> engine was built.

## What's here

| File | Purpose | Who edits it |
|---|---|---|
| `PROVENANCE.md` (repo root) | Originality record + per-subsystem implementation status. | Updated as subsystems land |
| `specs/README.md` | The spec-writing protocol — what may/may not go in a spec. **Read first.** | — |
| `specs/_TEMPLATE.spec.md` | Template for a new subsystem spec. | Spec author |
| `specs/D1-render-device.spec.md` (etc.) | Worked specs the implementations are built from. | Spec author |

## The method

1. **Define the interface** (`engine/<sys>/I*.h`) — clean, original, third-party
   types kept out of the header.
2. **Write a behavioral spec** (`specs/<name>.spec.md`) — what the subsystem does:
   interface contract, inputs/outputs, edge cases, perf targets, acceptance tests.
   Original prose + citations to **public** references only; never any third-party
   engine source.
3. **Implement** behind the interface, from the spec + public references + permissive
   libraries. Add the in-file note (`// No id Tech / RBDOOM source consulted.`) — it
   is part of the provenance record.
4. **Test** against the spec's acceptance cases (the `--test-*` / `--smoketest`
   self-tests in the app).
5. **Record** it in `PROVENANCE.md`. git history shows where/when it was authored.

## The one rule

Implement only from `specs/*.spec.md` + **public** references + permissive-library
docs. Never read, request, or transcribe any third-party game-engine source. Studying
public talks/papers/specs is fine and encouraged; copying engine code is not (and has
not been done).
