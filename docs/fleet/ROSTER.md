# Fleet Roster

> Source of truth for **who's on the fleet, what hardware, what role.**
> Mirrored from the Integrator's personal memory at
> `~/.claude/projects/G--/memory/fleet-roster.md` so every fleet bot can
> read it from the X3Native repo without needing access to the 13700K.
> Live updates: edit here, push, sync.
>
> **Last updated:** 2026-06-05 by Integrator (13700K)

---

## Matrix homeserver

- **URL:** `https://fleetcommand.slopclaude.com/`
- **Element Web:** branded "FleetCommand", dark theme default, federation OFF
- **Conduit homeserver** lives on the **13700K** (the always-on integrator node)
- **EFLZ design docs reader:** `https://docs.slopclaude.com/` (Cloudflare Access email-OTP)

See `docs/superpowers/specs/2026-05-27-fleet-messaging-design.md` for the full architecture.

---

## The 7 machines (+ Tim)

| Matrix ID | Display | Box / role | Hardware |
|---|---|---|---|
| `@tim` | tim ⚡️ | Tim (human) — owner, designer, captain | — |
| `@13700k` | **Integrator** | this box — clean-room engine + sole integrator of `feat/*` → `feat/cull-combined` → `main` | i7-13700K · 2× 1080 Ti · 192.168.1.206 (`P13700`) |
| `@snake` | snake ⚡️ | right-screen on the 13700K box; ship-art + Blender MCP + fleet_image.py | shares 13700K · right 1080 Ti |
| `@14900k` | 14900k ⚡️ | gameplay/content rig | i9-14900K · RTX 5090 |
| `@djbooth` | djbooth ⚡️ | garage box; portal-hub + rifthub + matrix-daemon author | i7-4790K · GTX 1080 Ti · garage |
| `@i5000` | i5000 ⚡️ | Act-2 desert + canon-aliens + fire-DamageType chain author | i5-class worker |
| `@predator` | predator ⚡️ | the i4400 worker box ("Predator" is the display alias) | i5-4400-class |
| `@oglaptop` | oglaptop ⚡️ | the **Dell** — "laptop by name, powerhouse at heart" | i9 · 64 GB RAM · **RTX A2000** (6 GB ECC Quadro) |

---

## Lane assignments (fleet protocol)

Per the dispatch protocol established 2026-05-23:

1. Workers branch from current `origin/main` (or `origin/feat/cull-combined` when explicitly told)
2. Each takes ONE non-overlapping lane
3. Gate locally: Release build + all `--test-*` + Debug `--smoketest` 0 VUID + leak-clean
4. Push a `feat/` branch (NOT main)
5. Signal in `#fleet-ops` when ready for integration

**The 13700K (Integrator) is the SOLE merger** — fetch → merge each branch → re-gate → push.

**Clean-room rule binds ALL machines:** never read RBDOOM/idTech/Doom/Quake/Unreal engine source. Tim's own IP (Q3Engine, LevelArchitect, Babylon X3Engine, EFLZ design corpus) is fair game.

---

## Important coordination protocols

- **Identity:** `snake1847@gmail.com` is the shared git author email across most fleet identities. **The Matrix display name + commit-message self-tag** are the source of truth for "who did this," NOT the git email.
- **The Integrator (this 13700K session) is the only fleet member that doesn't tag itself in commits** because it just merges others' work — every merge commit's author IS the truth.
- **Bug 2 (Task #18):** multi-process Vulkan-queue saturation when N concurrent `X3Engine.exe` smoketests run on the same GPU. Operational rule: ≤2 concurrent on a single GPU. Release-isolated is rock-solid.
- **Detailed commit messages:** Tim wants multi-paragraph commit bodies (context + why + gates + next), not one-liners. See `tools/fleet/README.md` for the daemon side; same standard applies fleet-wide.

---

## Tooling each box should have

To participate in fleet messaging:
- `node` v22+ (matrix-bot-sdk requirement)
- `git` (obviously)
- Python 3 with `-X utf8` support (for `tools/fleet/fleet_send.py` + `fleet_inbox.py`)
- The matrix-daemon Scheduled Task installed (see `tools/matrix-daemon/`)
- `~/.claude/.matrix_token` populated with the bot's access token
- `tools/fleet/` hooks wired into `~/.claude/settings.json` (see `tools/fleet/README.md`)

---

## Updating this doc

This is the **shareable team copy**. When the roster changes (new box online, hardware swap, role shift):

1. Tell the Integrator (`@13700k` / @ Integrator) over Matrix or in a Tim conversation
2. Integrator updates `~/.claude/projects/G--/memory/fleet-roster.md` (personal memory)
3. Mirrors the relevant changes here, in `docs/fleet/ROSTER.md`
4. Commits + pushes — other fleet bots pull on their next sync

The personal-memory copy is the *internal* source of truth (and contains slightly more candid hardware notes). This doc is the *public* (intra-fleet) source of truth.
