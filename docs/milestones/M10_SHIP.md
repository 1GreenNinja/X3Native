# Milestone M10 — Steamworks + Ship

**Branch:** `feat/m10-ship` · **Carved from** `X3_NATIVE_SLICES.md` on 2026-06-05
**Slices:** 95, 96 (Steam, from M9) + 97, 98, 99, 100 (ship)

## Context

The productization milestone — getting the game (EFLZ / the X3 slice) onto Steam
and running on a machine that has no dev tools. Already in tree: the build-time
version system (`feat/versioning`, `0.3.<commit-count>` ✅) and `.x3pak`
mount/priority (67 ✅, 69 ✅).

## Slices

### 95 📝 Steamworks SDK integration
Steam init, overlay, achievements, cloud-save hooks. **Gate:** overlay opens
in-game; a test achievement fires.

### 96 📝 Audio + Steam quality pass
Mix levels, achievement set, rich presence. **Gate:** shippable audio + Steam
presence.

### 97 📝 Release build + packaging
Optimized Release; bundle `X3Engine.exe` + `base.x3pak` + game pak into a
distributable. **Gate:** a packaged folder runs.

### 98 📝 Clean-PC test
Run the package on a machine without VS/SDKs (the 13700K or a VM); catch missing
redists. **Gate:** launches + plays on a clean machine.

### 99 📝 Steam page + depot upload
Store page assets; SteamPipe depot build; Steam-key smoke test. **Gate:**
installable via Steam (beta branch).

### 100 📝 Public build live
First shippable build published; day-1 patch branch ready. **Gate:** a stranger
can buy/download + play.

## Prerequisites & cross-dependencies

- **Steam appid** (Steamworks partner account) — *external, Tim's action.*
- **Game content green first** — the actual game must be shippable before this
  milestone means anything.
- **Pak builder (Slice 66, currently 📝)** — packaging (97) needs a tool to bake
  the game pak. Either land 66 first or fold a minimal baker into 97.

## Recommended build order

1. **Steamworks SDK init + overlay (95)** behind a `X3_STEAM` build flag
2. **Packaging (97) + clean-PC test (98)** — prove it runs off the dev machine
   (this is where missing-redist bugs surface; do it early)
3. **Achievements + rich presence (96)**
4. **Steam page + depot (99)** → **public (100)**

## Milestone gate

A stranger can buy/download and play on a clean machine via Steam (beta branch
first), engine v1 + first game shipped.
