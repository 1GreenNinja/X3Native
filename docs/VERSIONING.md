# X3Native — Versioning

**Problem this solves (2026-05-23 incident):** five sessions across the fleet built
the engine in parallel; nobody could tell two `X3Engine.exe` builds apart, a Feb
**BabylonJS** screenshot got mistaken for a native build, and a ~49-commit lead on
the 14900k couldn't be located against `main`. The engine had **no version string,
no `--version`** — two builds were indistinguishable.

**The fix (as built on `feat/versioning`):** every build carries a compiled-in
version derived from git, shown on `--version`, in the console (`version` command),
on the main menu, and as a small HUD watermark — so any build, log, or screenshot
self-identifies.

---

## 1. The scheme: `MAJOR.MINOR.BUILD` = `0.3.NNNNN`

```
X3 v0.3.00284 (c3c74e1)
       ^MAJOR.MINOR.BUILD   ^short git hash
```

| Field | Source | Notes |
|---|---|---|
| `MAJOR.MINOR` = `0.3` | the `X3_VERSION_MAJOR` / `X3_VERSION_MINOR` CMake cache vars (root `CMakeLists.txt`) | **single editable place**; `0` = pre-release, `3` = current line |
| `BUILD` = `NNNNN` | `git rev-list --count HEAD`, **zero-padded to 5 digits** | the total commit count — monotonic, no hand-editing, maps a build to a point in history |
| `(hash)` | `git rev-parse --short HEAD` | the exact commit; `nogit` if git is unavailable |

The canonical strings (compiled in via the generated header):

- `X3_VERSION_STRING` = `"0.3.00284"` (the `MAJOR.MINOR.BUILD` triple)
- `X3_VERSION_FULL`   = `"0.3.00284 (c3c74e1)"` (triple + short hash)

### Why the git commit count for BUILD?

It's **automatic and monotonic** — every commit bumps it with zero manual work, so
two builds are never accidentally stamped the same number, and a higher BUILD always
means "more commits / further along." It needs no release process and no file to keep
in sync. Combined with the short hash it's a precise, human-readable build id that
maps straight back to source. (Caveat: the count is per-branch/per-history — a feature
branch and `main` at the same calendar moment can show different counts; the hash
disambiguates.)

### How to bump MINOR (0.3 → 0.4)

Edit the **one** place — the cache var in the root `CMakeLists.txt`:

```cmake
set(X3_VERSION_MINOR 3 CACHE STRING "...")   # change 3 -> 4
```

(or pass `-D X3_VERSION_MINOR=4` at configure). Bump MINOR on a new Act or a major
engine system (Act-2 world go-live, GPU-driven culling, netcode-go-live). MAJOR stays
`0` until the first public release. BUILD is never edited by hand. **Note:** if you
bump MINOR, update the `--test-version` regex (`^0\.3\.\d{5}$`) in `app/main.cpp` to
match the new line.

---

## 2. How it's injected (CMake — clean-room, no new deps)

- **Template (committed):** `engine/core/version.h.in` — has `@X3_VERSION_*@`
  placeholders.
- **Generator (committed):** `cmake/GitVersion.cmake` — runs `git rev-list --count
  HEAD` + `git rev-parse --short HEAD`, zero-pads BUILD, composes the strings, and
  `configure_file()`s the template to the generated header.
- **Generated header (gitignored, never committed):**
  `<build>/generated/engine/core/version.h` — included in code as
  `#include "engine/core/version.h"` (the build dir's `generated/` is on the engine
  target's PUBLIC include path).

Root `CMakeLists.txt` runs the generator **twice**:

1. **at configure time** (`include(cmake/GitVersion.cmake)`) so the header exists for
   the first build;
2. **at every build** via `add_custom_target(x3_version ALL ... -P GitVersion.cmake)`
   so the hash + commit-count stay fresh when **HEAD moves** without re-configuring.
   `configure_file` only rewrites the header when its contents change, so this does
   **not** force a full rebuild each time. `x3engine` (and therefore `X3Engine`)
   `add_dependencies(... x3_version)`, so the stamp is current before anything that
   includes the header compiles.

**Graceful fallback:** if git is missing or this isn't a repo, BUILD = `00000` and the
hash = `"nogit"`, so the build **never breaks** (you get `0.3.00000 (nogit)`).

The generated header is gitignored (it lives under `/build/` which is already ignored,
plus an explicit `**/generated/engine/core/version.h` rule). Only the `.in` template
and the CMake glue are committed.

---

## 3. Where the version shows up

| Surface | Where | Output |
|---|---|---|
| **`--version` CLI flag** | `app/main.cpp` | prints `X3 v0.3.00284 (c3c74e1)`, exits 0 |
| **Console `version` command** | registered in `app/hud.cpp` (`Hud::init`, the cvar/command system) | prints `X3 v0.3.00284 (c3c74e1)` to the console |
| **Main menu** | `x3::ui::MainMenu` (`app/ui.cpp`) | small dim `X3 v0.3.00284 (c3c74e1)` line under the title/subtitle |
| **Production HUD** | `x3::ui::GameHud` (`app/ui.cpp`) | tiny `v0.3.00284` watermark, bottom-right corner (gated by `HudModel::showVersion`, default on) |
| **Startup log** | `app/main.cpp`, first log line | `X3 v0.3.00284 (c3c74e1)` before "X3Engine starting..." — so `--smoketest` / `--bench` logs self-identify |

In code, include `engine/core/version.h` and use the macros (`X3_VERSION_STRING`,
`X3_VERSION_FULL`, `X3_GIT_HASH`, `X3_VERSION_MAJOR/MINOR/BUILD`) or the helpers
`x3::versionString()` / `x3::versionFull()`.

---

## 4. Self-test: `--test-version`

`X3Engine.exe --test-version` runs offline (no window/Vulkan) and asserts:

1. `X3_VERSION_STRING` is non-empty;
2. it's well-formed — matches `^0\.3\.\d{5}$`;
3. `X3_VERSION_FULL` is well-formed (`<string> (<hash>)`);
4. the console `version` command (registered exactly as the engine does) reports
   `X3_VERSION_FULL`;
5. the `--version` code path produces the same line and exits 0.

It prints `version: X/Y passed` and exits 0 only if all pass — the same shape as the
engine's other `--test-*` gates.

---

## 5. Versioning policy (0.x pre-release)

- **MAJOR** stays `0` until the first public release.
- **MINOR** bumps on a new Act or a major engine system; it's the only hand-set field.
- **BUILD** is the git commit count — never edited.
- Tagging a release is optional but recommended: `git tag v0.3 && git push origin v0.3`.

---

## STATUS
<!-- branch HEAD, files changed, gate confirmation, READY FOR INTEGRATION / BLOCKED -->
- 2026-05-25 (13700K, `feat/versioning`): **IMPLEMENTED.** Scheme `0.3.<commit-count>`
  wired end to end — `cmake/GitVersion.cmake` + `engine/core/version.h.in` (generated
  header gitignored), root-CMake MAJOR/MINOR cache vars + `x3_version` custom target,
  `version` console command (`app/hud.cpp`), main-menu version line + HUD watermark
  (`app/ui.cpp`/`ui.h`), `--version` + `--test-version` + startup banner
  (`app/main.cpp`). Gated: all `--test-*` exit 0, `--test-version` passes, `--version`
  works, Release + Debug `--smoketest` 0 VUID, Debug leak-clean. READY FOR INTEGRATION.
- 2026-05-27 (13700K SNAKE, `feat/version-bump-0.4`): **BUMPED 0.3 → 0.4.** New engine systems: `SwimController` + `NPCSystem` (both shipped on their own feat lanes today). `--test-version` regex updated to `^0\.4\.\d{5}$`. Release notes in `docs/RELEASES.md`. Gated: `--test-version` green, Release + Debug `--smoketest` 0 VUID alloc=0. READY FOR INTEGRATION.
