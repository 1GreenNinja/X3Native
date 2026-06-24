# Asset Distribution — LFS Quota Escape Plan

> **STATUS (2026-06-24): D: is now PRIMARY.** The authoritative, read-first,
> publish-target content-addressed store is the **local NVMe**
> `D:\Assets\X3AssetStore` (all 46 objects mirrored there, 806 MiB). The
> off-box `\\p13700\G\X3AssetStore` share is retained as a **backup mirror**
> (kept intact, not deleted). The `store` block in `assets/manifest.json` is
> the single source of truth (mirrored in `tools/asset_store.py` and
> `app/asset_manifest_check.h` defaults). Builds no longer require the network.
>
> **STATUS (2026-06-12): Phase A is IMPLEMENTED** (branch `feat/asset-store`):
> the content-addressed store was first stood up at `\\p13700\G\X3AssetStore`, all 46
> files under `assets/rigged_glb/` + `assets/converted_glb/` (845,283,484
> bytes ≈ 806 MiB) are published + hash-verified in `assets/manifest.json`,
> `tools/asset_store.py` (publish/fetch/verify/status) is the one tool, the
> pre-commit guard now BLOCKS new git/LFS assets, and the engine auto-fetches
> /warns at boot (`app/asset_manifest_check.h`). See §4.5.
> **Phase B (§4.4 bulk move) and §6 (history rewrite) remain PROPOSED** — §6
> needs Tim's sign-off and every fleet machine coordinated. Do not action
> Phase B/§6 from a worktree lane.

## 1. The problem

- **856 MB of the 1 GB Git-LFS quota is used.** Headroom: ~168 MB.
- A **2.1 GB GGUF** (LLM NPC weights) nearly entered history **twice** in the
  week of 2026-06-08 — caught by review both times. The pre-commit guard
  (`tools/hooks/pre-commit`) + `.gitignore` `*.gguf` rules now block this
  class of accident, but they do nothing about the quota trajectory.
- LFS storage on GitHub is **append-only in practice**: every pushed version
  of every object counts against the quota forever (pruning requires support
  tickets or repo deletion). Rewriting/deleting branches does not refund it.

### Current numbers (audited 2026-06-12, worktree of `feat/doors-death-anim`)

| Scope | Objects | Size |
|---|---|---|
| This branch's checkout | 56 LFS files | **845 MB** |
| Unique LFS objects across **all local refs** | 143 | **1,292 MB** |
| Remote quota used | — | **856 MB / 1,024 MB** |

Per-directory (this branch):

| Directory | Files | Size |
|---|---|---|
| `assets/rigged_glb/` | 31 | 567.4 MB |
| `assets/converted_glb/` | 15 | 275.1 MB |
| `assets/audio/` | 8 | 2.1 MB |
| `assets/props/` | 2 | 0.3 MB |

Also note (plain git, not LFS, but the same disease): **~87 MB of PNG/GIF**
(mostly `captures/texrefine`, `docs/`) and **~27 MB of fonts** sit in regular
git history. New asset-dir textures/fonts now route to LFS via the 2026-06-12
`.gitattributes` patterns; the historical ones only leave with §6.

## 2. The LFS ledger — biggest objects and whether we even need them in git

"FETCHABLE" = the source of truth already lives on the fleet share
(`G:\GameModels\rigged_glb\` etc. — see `docs/ASSET_INVENTORY.md`).
"RE-DERIVABLE" = regenerated mechanically by a committed tool from a pack on
`G:\Assets` / `D:\Assets`.

| # | Object | Size | Verdict |
|---|---|---|---|
| 1 | `assets/converted_glb/ShowRoom_Vol30/Example_01.glb` | 84 MB | **RE-DERIVABLE** — `tools/convert_unity_pack.py repack-glb <kit> …` from the Unity ShowRoom_Vol30 kit (procedure already documented in `.gitignore` lines 88–94) |
| 2 | `assets/rigged_glb/blue_synth_seed1.glb` (other branches) | 74 MB | **FETCHABLE** — `G:\GameModels\rigged_glb\` |
| 3 | `assets/rigged_glb/chief_martinez_anim.glb` | 64 MB | **RE-DERIVABLE** — `tools/animate_creature.py` bakes Idle/Walk/Run onto the base GLB |
| 4 | `assets/rigged_glb/chief_martinez.glb` | 56 MB | **FETCHABLE** — `G:\GameModels\rigged_glb\` (per `docs/ASSET_INVENTORY.md`) |
| 5 | `assets/rigged_glb/marcus_webb_anim.glb` | 49 MB | **RE-DERIVABLE** — animate_creature.py |
| 6 | `assets/rigged_glb/marcus_webb.glb` | 43 MB | **FETCHABLE** — `G:\GameModels\rigged_glb\` |
| 7 | `assets/rigged_glb/BossBreederQueen.glb` | 41 MB | **FETCHABLE** — `G:\GameModels\rigged_glb\` |
| 8 | `assets/rigged_glb/Jake_22_actions.glb` | 26 MB | **FETCHABLE** — `G:\textures\JakeGLB\` + EscapeLab48 `models\jake\` |
| 9 | `assets/converted_glb/ModularSciFi_Interior/SM_Wall_A.glb` | 24 MB | **RE-DERIVABLE** — `tools/convert_unity_pack.py` from the ModularSciFi Unity pack (as are its 7 siblings, ~169 MB combined) |
| 10 | `assets/rigged_glb/BossTheSiren.glb` | 23 MB | **FETCHABLE** — `G:\GameModels\rigged_glb\` |

Pattern: **everything big is either a mirror of the G:\ share or machine
output of a committed converter.** Nothing in the top tier is
git-as-source-of-truth. What *is* source-of-truth in the repo is small: the
curated weapon SFX (2.1 MB), props (0.3 MB), and the converters/tools
themselves.

## 3. Quota timeline at current growth

- 2026-05: **136** LFS files added across branches (≈ the entire ~1.29 GB of
  unique objects landed in ~5 weeks, ≈ 200–260 MB/week at peak).
- 2026-06 (through 06-12): **14** files added — a slower but nonzero pace.
- Headroom is **168 MB**. One more character at chief_martinez scale
  (base + `_anim` bake ≈ **120 MB**) consumes it in a **single commit**.
- Projection: **quota exhausted within ~2–4 weeks** at the June pace, or
  **this week** if any Act-2 character/kit work lands. After that, pushes of
  new LFS objects fail fleet-wide.

## 4. Fleet asset store + content-hash manifest (Phase A: IMPLEMENTED)

### 4.1 Store

Two tiers (as of 2026-06-24, the local NVMe is the authoritative primary):

- **Primary (authoritative, read-first, publish target):** the local NVMe
  `D:\Assets\X3AssetStore\objects\<sha256[0:2]>\<sha256>` (content-addressed,
  immutable). All 46 objects live here; builds never need the network.
- **Backup mirror (off-box):** the `\\p13700\G\X3AssetStore` share, same
  layout, kept intact as a redundant copy. Re-mirror with
  `robocopy "D:\Assets\X3AssetStore" "\\p13700\G\X3AssetStore" /E` after
  publishing new objects. The human-browsable `G:\GameModels\` / `G:\Assets\`
  mirrors stay where they are (`docs/ASSET_INVENTORY.md`).

### 4.2 Manifest — `assets/manifest.json` (committed, tiny)

One entry per distributed asset: repo-relative path, SHA-256, byte size,
provenance (`fetch` vs `derive` + the tool/pack that regenerates it).

The repo already uses exactly this pattern: **`tools/ktx2bake/bake.ps1`
content-hashes every source texture (SHA-256), records it as `sourceSHA256`
in a `manifest.json`, and skips re-encoding when the hash matches** (see
`Get-FileSHA256` at line ~137, the skip-if-unchanged check at lines ~288–303,
and the manifest write at line ~360). The asset manifest is the same idea
promoted from the texture pipeline to all distributed assets, so loaders and
tools can verify integrity the same way.

### 4.3 Fetch tool — `tools/fetch_assets.py` (design)

- Read `assets/manifest.json`; for each entry, if the file is missing or its
  SHA-256 mismatches → copy from `D:\Assets\x3store` cache, else from
  `\\p13700\G\X3AssetStore`, verify hash, place it.
- Entries marked `derive` print the regeneration command
  (`tools/convert_unity_pack.py …` / `tools/animate_creature.py …`) instead
  of fetching, with a `--derive` flag to run them.
- Hook points: a CMake pre-build step and/or engine boot check (warn +
  continue modelless, same philosophy as the GGUF flow on `feat/llm-npc`).
- Failure mode: offline machine + cold cache → clear error listing what's
  missing; the engine already boots without optional content.
- Publishing: `tools/fetch_assets.py --publish <file>` hashes a new asset,
  copies it into the store, and appends the manifest entry — so adding an
  asset is one command and **zero LFS bytes**.

### 4.4 Migration path (what moves out, what stays)

Phase A — **stop the bleeding (no history change, safe now): ✅ IMPLEMENTED
2026-06-12** — new big assets go to the store + manifest, never to LFS. The
pre-commit guard blocks accordingly. See §4.5 for what shipped.

Phase B — **move the bulk (working tree only, still no history rewrite):**
`git rm --cached` + store-publish for `assets/rigged_glb/` (567 MB) and
`assets/converted_glb/` (275 MB) ⇒ **~842 MB of the 845 MB checkout leaves
git**. Clones stay reproducible: `git clone && tools/fetch_assets.py` yields
a byte-identical asset set (hash-verified). NOTE: this alone does **not**
refund quota (old objects remain in history) — it caps growth and makes §6
possible later.

**Stays in LFS (small/critical only, < ~5 MB total):** `assets/audio/`
weapon SFX (2.1 MB, curated/hand-picked = source of truth), `assets/props/`
(0.3 MB), and any future small one-off binaries. Fonts and committed PNGs are
unaffected until §6.

### 4.5 Phase A — AS IMPLEMENTED (2026-06-12, branch `feat/asset-store`)

- **Store (live):** `D:\Assets\X3AssetStore\objects\<sha256[0:2]>\<sha256>`
  is now the PRIMARY tier (immutable, no extensions; authoritative + read-first
  + publish target). The off-box `\\p13700\G\X3AssetStore` mirror (same layout)
  is retained as the backup. (Historically G: was primary and D: the cache;
  flipped 2026-06-24.)
- **Published:** all **46** files under `assets/rigged_glb/` (31) +
  `assets/converted_glb/` (15) — **845,283,484 bytes (806.1 MiB)** — each
  re-hashed from the store copy after upload. Round-trip proven (local file
  removed → fetched → SHA-256 identical), `verify` green 46/46.
- **Manifest:** `assets/manifest.json` (46 entries: repo_path / sha256 /
  size / source_note + the store tier paths; ~15 KB plain git).
- **Tool:** `tools/asset_store.py` — `publish` / `fetch [--all|paths]` /
  `verify` (CI-able, exit 1 on any failure) / `status`. Atomic temp+rename
  writes, retries on network blips, **never deletes** (a mismatched local
  file is set aside as `*.pre-fetch.bak`). If the share is unwritable,
  `publish` falls back to the D: cache tier and says so.
- **Guard:** `tools/hooks/pre-commit` now **BLOCKS** any NEW staged file
  under `assets/rigged_glb/`/`assets/converted_glb/` (any size) and any NEW
  `assets/**` file > 5 MB (LFS-pointer declared size counted), pointing at
  the publish command. `assets/manifest.json` commits normally. Re-run
  `tools/install_hooks.sh` on each machine to pick it up.
- **Boot check:** `app/asset_manifest_check.h` (wired in `app/main.cpp`,
  no python dependency) — missing manifest assets are auto-fetched from
  cache→share at startup (size-checked copy), else ONE log line says to run
  `python tools/asset_store.py fetch --all`. Proven live (deleted GLB
  restored bit-identically at boot).
- **NOT done in Phase A (by design):** the 46 files still sit in LFS too —
  removal from the index is Phase B; quota refund is §6.

## 5. What this buys

- LFS growth → ~0 MB/week; the 168 MB headroom stops mattering.
- Every fleet machine already mounts `\\p13700\G\`; the 940 Mbps link moves
  the full 845 MB set in ~10 s of transfer time, once, then it's cached.
- `git clone` drops from ~1 GB to ~150 MB.

## 6. The history-rewrite question — handled honestly

Quota is only *refunded* by purging LFS objects from history, which is a
**full history rewrite**. That is a fleet-stopping event:

- Every branch hash changes ⇒ **every machine and every in-flight worktree
  lane must stop, push, and re-clone.** Given the 2026-05-24 branch-divergence
  incident, an uncoordinated rewrite would be far worse.
- It needs **Tim's explicit go**, a chosen quiet day, and all PRs merged or
  parked.

**The exact steps for THAT day (do not run them now):**

1. Freeze: all machines push all branches; verify `git lfs ls-files --all`
   union is published to the store + manifest (Phase B complete).
2. Fresh mirror clone; run `git filter-repo --invert-paths
   --path assets/rigged_glb --path assets/converted_glb` (or BFG
   `--delete-folders`) to strip the moved objects from all history; the
   small/critical LFS set keeps its pointers untouched.
3. Verify: rebuilt history checks out, `tools/fetch_assets.py` restores
   assets bit-identically, the game boots, CI/screenshot suite passes.
4. Force-push all branches + tags to a **renamed backup remote first**, then
   to origin.
5. Every fleet machine re-clones (no `git pull` onto rewritten history —
   delete and re-clone; worktree lanes recreated from scratch).
6. Ask GitHub support to prune the orphaned LFS objects / or move the repo if
   quota doesn't release; confirm the quota meter drops.

Until that day: Phase A + B above keep us under quota without touching
history.
