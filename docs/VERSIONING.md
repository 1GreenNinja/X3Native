# X3Native — Version & Feature Stamping Standard

**Problem this solves (2026-05-23 incident):** five sessions across the fleet built
the engine in parallel; nobody could tell two `X3Engine.exe` builds apart, a Feb
**BabylonJS** screenshot got mistaken for a native build, and a ~49-commit lead on
the 14900k couldn't be located against `main`. The engine today has **no version
string, no `--version`, no `--help`** — two builds are indistinguishable.

**The fix:** every build carries a compiled-in identity (version + commit + build
date + machine + branch + dirty flag) and a human-readable feature manifest. The
identity prints on `--version`, on the HUD watermark (so screenshots self-identify),
and in the `--smoketest`/`--bench` headers (so logs self-identify). The fleet posts
this string on every push/build.

> **Fleet push model:** the three primary rigs — **13700K, 14900K, and the laptop
> (OG)** — push `main` directly (fetch → rebase → push, small & often). Worker
> machines (garage **DJBOOTH** 4790K, second-screen **Snake**) push **feature
> branches**; a primary rig merges + re-gates. `VERSION` is bumped by whoever does
> the `main` merge.

---

## 1. The version string (canonical format)

```
X3Native v0.4.0+c0d3c62 (built 2026-05-25 07:30, I9DevPC, main)
                 ^hash         ^UTC build time   ^machine ^branch
```

Dirty (uncommitted edits at build time) appends `-dirty`:
```
X3Native v0.4.0+c0d3c62-dirty (built 2026-05-25 07:31, p13700k, feat/act2-world)
```

Components — **all compiled in at build time**, never hand-edited:

| Field | Source | Why it matters |
|---|---|---|
| `MAJOR.MINOR.PATCH` | the `VERSION` file at repo root | human-meaningful release number; integrator bumps it |
| `+<gitshort>` | `git rev-parse --short HEAD` | exact commit — maps any build back to source |
| `-dirty` | `git status --porcelain` non-empty | the build came from **uncommitted edits** — the #1 "which build is this?" trap |
| build time (UTC) | CMake configure/build timestamp | orders builds in time |
| machine | `$env:COMPUTERNAME` (Win) / `hostname` | which rig produced it (I9DevPC, p13700k, the 14900k, garage 4790K) |
| branch | `git rev-parse --abbrev-ref HEAD` | which lane — instantly flags an off-`main` build |

`VERSION` file (repo root, single source of truth, one line):
```
0.4.0
```

## 2. Versioning policy (0.x pre-release)

- **MAJOR** stays `0` until first public release.
- **MINOR** bumps on a new Act or a major engine system (Act-2 world, GPU-driven culling, netcode-go-live).
- **PATCH** bumps on content/fixes that land on `main`.
- **Whoever merges to `main` bumps `VERSION`** (one bump per merge) and adds a `CHANGELOG.md` line. The primary rigs (13700K / 14900K / OG laptop) push `main`; workers push branches.
- Tag each bump: `git tag v0.4.0 && git push origin v0.4.0` — so versions ↔ commits are permanent.

## 3. How it's injected (CMake — clean-room, no new deps)

Add `cmake/GitVersion.cmake`. It runs `git` at build time and writes a generated
header the engine includes. The header is regenerated **every build** (so hash +
dirty + time stay fresh) but only rewritten when its contents change (so it doesn't
force a full rebuild each time).

```cmake
# cmake/GitVersion.cmake  (invoked via add_custom_target that runs pre-build)
find_package(Git QUIET)
file(READ "${CMAKE_SOURCE_DIR}/VERSION" X3_SEMVER)
string(STRIP "${X3_SEMVER}" X3_SEMVER)
execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE X3_GIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE X3_GIT_BRANCH
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE X3_GIT_DIRTY ERROR_QUIET)
if(X3_GIT_DIRTY STREQUAL "")
  set(X3_DIRTY "")
else()
  set(X3_DIRTY "-dirty")
endif()
string(TIMESTAMP X3_BUILD_TIME "%Y-%m-%d %H:%M" UTC)
cmake_host_system_information(RESULT X3_HOST QUERY HOSTNAME)
configure_file(${CMAKE_SOURCE_DIR}/cmake/x3_version.h.in
               ${CMAKE_BINARY_DIR}/generated/x3_version.h @ONLY)
```

`cmake/x3_version.h.in`:
```c
#pragma once
#define X3_VERSION_SEMVER  "@X3_SEMVER@"
#define X3_VERSION_HASH    "@X3_GIT_HASH@"
#define X3_VERSION_BRANCH  "@X3_GIT_BRANCH@"
#define X3_VERSION_DIRTY   "@X3_DIRTY@"
#define X3_VERSION_BUILT   "@X3_BUILD_TIME@"
#define X3_VERSION_MACHINE "@X3_HOST@"
#define X3_VERSION_STRING \
  "X3Native v" X3_VERSION_SEMVER "+" X3_VERSION_HASH X3_VERSION_DIRTY \
  " (built " X3_VERSION_BUILT " UTC, " X3_VERSION_MACHINE ", " X3_VERSION_BRANCH ")"
```

Wire-up: add `${CMAKE_BINARY_DIR}/generated` to the app include dirs; add an
`add_custom_target(x3_version ALL ...)` that re-runs `GitVersion.cmake` before the
`X3Engine` target builds, and `add_dependencies(X3Engine x3_version)`.

A tiny `engine/core/x3_version.h` wrapper exposes it to code:
```c
#pragma once
#include "x3_version.h"           // generated
namespace x3 { inline const char* versionString() { return X3_VERSION_STRING; } }
```

## 4. The feature manifest (answers "what features does THIS build have?")

A single in-code table is the human-readable inventory — the integrator adds a row
when a feature branch merges to `main`. `--features` prints it.

```c
// app/features.h  — keep in sync with main-merged features (integrator owns it)
struct Feature { const char* name; const char* note; };
inline constexpr Feature kFeatures[] = {
  {"render-device",     "Vulkan 1.3 dynamic-rendering, bindless, multidraw-indirect"},
  {"gpu-skinning",      "compute pre-pass skinning"},
  {"physics",           "Jolt world + character controller + vehicles"},
  {"act1-spire",        "7-floor spire B1->F7, bosses, drone hack, Nexus 4.5"},
  {"act2-world",        "alien-surface open world L8-20 (when merged)"},
  {"netcode",           "client prediction + server reconciliation (Phase 1)"},
  {"saveload",          "versioned checkpoints, F5/F9"},
  {"audio",             "miniaudio 3D + music; portable resolveAudio()"},
  // ... integrator appends on each merge ...
};
```

The existing `--test-*` / `--world` flag set is the *machine-checkable* inventory;
`--features` is the *human* inventory. Both should agree.

## 5. CLI surface (add to `app/main.cpp` arg parse)

| Flag | Output | Exit |
|---|---|---|
| `--version` | the one-line version string | 0 |
| `--version --json` | `{"semver":"0.4.0","hash":"c0d3c62","dirty":false,"branch":"main","machine":"I9DevPC","built":"..."}` | 0 |
| `--features` | version line + the `kFeatures` manifest, one per line | 0 |
| `--help` | usage + grouped flag list (tests / worlds / capture / dev) | 0 |

The `--json` form is what the fleet auto-posts to Slack on push/build.

## 6. Self-identifying logs + HUD (so screenshots & logs can't be confused)

- **First log line at every startup** (already have `logInfo`): print `versionString()`
  before the device line. Then `--smoketest`/`--bench` output is self-identifying.
- **HUD watermark**: draw `v0.4.0+c0d3c62 I9DevPC` small in a screen corner, gated by
  cvar `ui_showVersion` (default ON in dev). **This is why the Feb BabylonJS shot was
  mistaken for native — a watermark makes every screenshot say what it is.**

## 7. Rollout (one feature branch, gated like any other)

A worker machine (not the integrator) implements §3–§6 on `feat/versioning`:
1. add `VERSION`, `cmake/GitVersion.cmake`, `cmake/x3_version.h.in`, `engine/core/x3_version.h`, `app/features.h`;
2. wire the custom target + include dir; add `--version/--features/--help`; print at startup; add the HUD watermark + cvar;
3. run the full test-gate (all `--test-*` exit 0, Release+Debug `--smoketest` 0 VUID + allocationCount=0);
4. push `feat/versioning`; a **primary rig** merges to `main`, sets `VERSION`, tags `v0.4.0`.

After this lands, `X3Engine.exe --version` answers "which build is this?" forever.

---

## STATUS
<!-- branch HEAD, files changed, gate confirmation, READY FOR INTEGRATION / BLOCKED -->
- 2026-05-25 (I9DevPC): **design doc only** — specifies the standard; no engine code changed yet. READY FOR INTEGRATION (doc). Implementation (§7) is a separate `feat/versioning` task for a worker machine.
