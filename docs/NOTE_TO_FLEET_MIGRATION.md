# 📡 NOTE TO FLEET ✉️ — Matrix migration is happening

**To:** the fleet (13700K, 14900K, Snake13700k, i5000, I4400, MOB BOSS, laptop OG, and DJBOOTH itself)
**From:** DJBOOTH (garage 4790K / 1080Ti / Z97)
**Re:** we're moving fleet-internal messaging off Slack onto self-hosted Matrix
**Date:** 2026-05-28
**Branch:** `feat/fleet-messaging-design`

---

Hi everyone,

Tim wanted the fleet's messaging fabric out from under Slack's pricing/feature risk and onto something we own end-to-end. After an overnight design session (8-hour authorization, ~6 wakeup cycles deep when I wrote this), we have a complete spec, a 35-task implementation plan, and the deployment tooling sketched. Reading time end-to-end: ~15 min.

## TL;DR

| | |
|---|---|
| **Protocol** | Matrix (open standard, no vendor lock-in) |
| **Server** | Conduit (Rust, single 12 MB binary, SQLite) — runs on the 13700K Command Center |
| **Public URL** | `https://fleetcommand.slopclaude.com/` via Cloudflare Tunnel (free, no port-forwarding) |
| **Clients** | Element (web / iOS / Android), polished apps free of charge |
| **Identity** | Per-machine Matrix accounts — `@djbooth`, `@13700k`, `@14900k`, `@i5000`, `@i4400`, `@snake`, `@mob_boss`, `@laptop_og` |
| **Bot tech** | matrix-bot-sdk on Node.js — same daemon pattern as our Slack daemon today |
| **Theme** | Cyberpunk-X3Native (dark blue-black, portal-cyan accents, Orbitron headers) |
| **Cost** | $0/month forever (domain ~$1-3/yr is the only spend) |
| **Migration** | 30-day Slack coexistence then cutover |

## Why now

Three forces:

1. **Slack workspace plan-tier blocked the Anthropic-hosted Matrix MCP** during our setup last week. Tim hit the "invalid permissions requested" wall on newer scopes (Search 2.0, canvases). Free workspaces are good enough today but feature gates can shift any time.
2. **Per-bot OAuth dance is repetitive** — every new fleet PC needs its own Slack app + scopes + install + token. Matrix lets us issue registration tokens from our own Conduit and skip the OAuth dance entirely.
3. **Custom features were friction** on Slack. The fleet-status sidebar widget (showing each machine's presence + current branch + integration queue) is a Phase 2 deliverable in this spec — straightforward via Matrix widgets, awkward via Slack apps.

## What's already shipped on `feat/fleet-messaging-design`

| Commit | What |
|---|---|
| `9b264ce` | `docs/superpowers/specs/2026-05-27-fleet-messaging-design.md` — 332-line spec, 11 sections, decision table |
| `e334e61` | `docs/superpowers/plans/2026-05-27-fleet-messaging-phase1-plan.md` — 1262-line implementation plan, 35 tasks, 10 sub-phases, TDD where applicable |
| `b914af7` | `tools/lan-bus/` — fleet-internal RPC over LAN HTTP (receiver + sender + bootstrap). Sub-second latency between fleet PCs, complement to Slack/Matrix. |
| `0fec57a` | `tools/matrix-daemon/` — per-machine Node.js daemon, **18 jest tests passing**. Plus `tools/conduit-prep/conduit-v0.10.12.tar.gz` source archive + 3-option deployment guide. |
| `0374109` | `tools/element-theme/` — cyberpunk-X3Native theme.css + theme.json (per-bot identity colors: DJBOOTH violet, 13700K cyan, 14900K amber, etc.) |
| `54bb912` | `tools/slack-daemon/` (canonical source) + `tools/conduit-prep/docker-compose.yml` + `tools/element-web/setup.md` + `tools/fleet-status/` schema + generator stub + sidebar widget skeleton |

## What needs Tim's wake-up review

These are the 6 open questions from spec §9:

1. ✅ **Domain name** — `fleetcommand.slopclaude.com` (locked 2026-05-28; subdomain of Tim's existing slopclaude.com)
2. **Cloudflare account** — needs one for the Tunnel; free tier fine
3. **Conduit deployment option** — Docker (recommended) / WSL2 / build-from-source — see `tools/conduit-prep/CONDUIT-DEPLOY.md`
4. **Backup destination** — Dropbox proposed at `D:\Dropbox\fleet-backups\conduit\`
5. **Element web placement** — same subdomain as homeserver (recommended)
6. **Fleet-status widget priority** — Phase 1 or Phase 2 (default Phase 2)

## Per-machine ask (when Tim greenlights Phase 1)

Each fleet PC needs (in this order):

1. **Pull the branch** (or just the `tools/` subtree) for the deployment scripts
2. **LAN bus setup** (parallel to Matrix, gives sub-second Claude-to-Claude RPC): `pwsh tools/lan-bus/bootstrap.ps1` + admin firewall rule + admin urlacl reservation + edit `~/.claude/.fleet_hosts.json` with real LAN IPs + copy `~/.claude/.fleet_secret` from DJBOOTH
3. **matrix-daemon setup**: `cd ~/.claude/matrix-daemon && npm install`, then `~/.claude/.matrix_token` populated from the bot account Tim creates for this machine on Conduit, then register the daemon as a Scheduled Task at logon (see `tools/matrix-daemon/README.md`)
4. **Element on the human-facing side** (laptop, phone): install Element Web/Desktop/Mobile, log in with `tim` account, join `#fleet-ops`
5. **Theme** (optional, can wait): paste `tools/element-theme/theme.json` into Element's custom-theme setting, OR have 13700K deploy `theme.css` to Element Web's webroot

## What stays on Slack

For the first 30 days post-Matrix-deploy: Slack is the fallback. Tim's iPhone push notifications stay on Slack until Element's push reliability is proven. The Slack daemon on each machine continues to run + cross-mirrors incoming Slack DMs to `.matrix_inbox.jsonl` so the unified /loop drain handles both surfaces. After 30 days of stable Matrix uptime, Slack daemon can be disabled and the Slack workspace archived (or kept for nostalgia).

## What's NOT changing

- The git workflow stays: feature branches per machine, integrator (13700K) merges to main
- The `NOTE_TO_<MACHINE>.md` async note pattern stays as durable record alongside synchronous Matrix chat
- The per-machine task files (`DJBOOTH.md`, `Snake13700k.md`, etc.) stay — Matrix is the *chat* layer, the markdown files are the *contract* layer
- Slack's `#fleet-ops` channel doesn't go anywhere during the soak — but the canonical `#fleet-ops` becomes Matrix-side

## Slack → Matrix latency expectations

After Matrix is live:

| Path | Latency |
|---|---|
| Claude on DJBOOTH ↔ Claude on 13700K, via Matrix mention | < 1 second (persistent sync connection) |
| Tim's iPhone DM `@djbooth` → DJBOOTH's Claude responds | DM lands instantly via Element; response depends on whether CC is open on DJBOOTH (manual prompt latency) or `/loop` is running (~10-25 min worst case) |
| Tim's iPhone DM via Slack (during soak) | ≤ 5 min via daemon poll, then same CC-open latency |
| Slack DM mirrored into Matrix inbox | ≤ 5 min (daemon does the mirror on poll cycle) |

## Implementation order

Phase 1 plan tasks 1–35. Tim-gated tasks are §1.A (domain pick, Cloudflare confirm). After that, subagent-driven-development can execute the plan. Estimated wall time end-to-end Phase 1: **3-4 hours of focused work on 13700K** once Tim makes the §9 calls.

Phase 2 is the cyberpunk theme deploy + sidebar widget + remaining 6 fleet daemons. Phase 3 is the Slack bridge (mautrix-slack) + cutover ceremony.

## How to push back

If anyone (a future fleet member, a critical-thinking Tim on wake, etc.) sees this and thinks Matrix is the wrong call: the spec is in git history. Revert the branch, write a counter-spec in the same `docs/superpowers/specs/` directory, and we deliberate. The infrastructure here is all branch-isolated; nothing's on `main`.

Sincerely,

**DJBOOTH**
*garage 4790K · 1080Ti · Z97 · X3Native gameplay worker · overnight fleet-messaging-design author*
