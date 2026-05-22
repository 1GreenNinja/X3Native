# specs/ — Clean-room spec protocol

These specs are the **source of truth** the engine was built from. Implementing
from an original behavioral spec + public references (not from any third-party
engine source) is what makes the implementation independently created — original
work, not a derivative. That is how X3Native was built; these specs are part of
the provenance record (`PROVENANCE.md`).

> **Historical note:** the spec format was originally framed as an "information
> barrier" between a GPL RBDOOM source (on one machine) and a clean implementation
> (on another). **No RBDOOM/GPL source was ever used**, so there is nothing to wall
> off — but the rule "implement only from the spec + public refs, never from foreign
> engine source" still holds and was followed.

## How a spec is used

1. **Define the interface** first (`engine/<sys>/I*.h`) — original, clean.
2. **Write the spec** (`<name>.spec.md`): behavior, inputs/outputs, edge cases,
   perf targets, and acceptance tests — original prose with citations to public
   references only.
3. **Implement** behind the interface from the spec + public refs + permissive libs.
4. **Test** against the spec's acceptance cases; record in `PROVENANCE.md`.

## What may and may not appear in a spec

✅ Allowed:
- The interface API (signatures the impl must satisfy — defined clean, they're yours)
- Behavioral description in prose
- Input/output contracts, value ranges, units
- Edge cases and error handling expectations
- Performance targets
- Acceptance test cases (inputs → expected outputs)
- Citations to **public** references (Vulkan spec, *Real-Time Rendering*, GPU Gems,
  glTF spec, library docs, public GDC/SIGGRAPH talks)

❌ Never:
- Source copy-pasted or transcribed from any third-party game engine
- Another engine's exact internal data-structure layouts or identifier names
- "Look here" pointers into third-party engine source

## Why this is solid

Every implementation traces to an original spec + public knowledge, was authored on
machines that never held a third-party engine checkout (git history shows it), and
carries in-file "no foreign source consulted" notes. That's the gold-standard record
of independent creation — see `PROVENANCE.md`.
