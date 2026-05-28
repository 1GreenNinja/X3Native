# Fleet Messaging System — Design Spec

**Date:** 2026-05-27
**Author:** DJBOOTH (claude-opus-4-7, garage 4790K / 1080Ti)
**Status:** Draft — pending Tim's review
**Branch:** `feat/fleet-messaging-design`
**Project:** X3Native fleet (DJBOOTH, 13700K, 14900K, I5000, I4400, Snake, MOB BOSS, laptop OG, plus Tim + future fleet members)

---

## 1. Context

The X3Native fleet currently coordinates via:

1. **Git-mediated notes** — `NOTE_TO_<MACHINE>.md` files committed and merged via the 13700K integrator. Async, durable, but high-latency (hours to days between fetch cycles).
2. **Per-machine task files at repo root** — `DJBOOTH.md`, `Snake13700k.md`, `i5000.md`, `14900K.md`. Each carries a STATUS block the assigned machine appends to.
3. **Slack** (added 2026-05-26) — Tim's GameDev workspace with a per-machine bot per PC. DJBOOTH-Bot was the first; long-running PowerShell daemon polls every 5 min and queues incoming messages to a local inbox the next Claude session drains.

Slack works but introduces three concerns:

- **Vendor lock-in / subscription risk.** Slack Free is fine today but the workspace plan-tier already blocked the Anthropic-hosted MCP's broad scope set. Future feature paywalls are a real possibility.
- **Per-bot OAuth complexity.** Each new fleet PC requires a new Slack app, fresh scopes, fresh tokens, fresh install. Tim repeated the dance once; the dance is the problem.
- **Closed protocol.** No customization beyond what Slack's API allows. The cyberpunk-X3Native aesthetic Tim wants, the fleet-status sidebar widget, the build-bot integration — all are Slack-app-bound.

**This spec proposes a self-hosted Matrix-based fleet messaging fabric** that owns the protocol layer end-to-end, runs at $0/month forever, and provides a foundation for fleet-specific custom features (presence indicators tied to ping checks, build-bot CI integration, "wake the fleet" broadcast commands, automated `NOTE_TO_*.md` mirroring) that would be awkward or impossible on Slack.

The goal of this spec is to lock the architecture so the implementation plan (next step, `writing-plans` skill) can execute against a fixed target.

## 2. Decision summary

| Topic | Decision | Rationale | Alternatives noted |
|---|---|---|---|
| **Server host** | **13700K Command Center** | Confirmed by Tim 2026-05-27 21:13 PT. Always-on, integrator role aligns, fastest fleet CPU. | Dedicated Raspberry Pi 4 long-term (best separation of dev/infra); DJBOOTH as fallback. |
| **Server implementation** | **Conduit** (Rust, single 12 MB binary, SQLite) | Lightest mainstream Matrix homeserver. No Postgres, no Python, no Docker required. One systemd-style auto-restart service on the host. | Synapse (heavier, Python+Postgres); Dendrite (Go, also lighter than Synapse but more deps than Conduit). |
| **Federation** | **Disabled initially**, can be enabled later | Single-server fleet. No need to federate with public Matrix.org. Trivial to enable per-server later. | Federated from day one — more work, no immediate benefit. |
| **Domain** | `chat.tims-fleet.xyz` (placeholder, Tim picks the real domain) | Cloudflare Tunnel needs a public hostname to point at. Tim already owns or can register a fleet-specific domain (~$10/yr). | Use a Cloudflare-provided trycloudflare.com hostname — works but unstable; rerolls per-connection. |
| **Remote access** | **Cloudflare Tunnel** (`cloudflared` daemon on 13700K) | Free, no port-forwarding, no static IP, no DDNS. Works from any phone on cellular. | Tailscale (requires the human to be on the mesh too — same on phone — fine but extra app); router port-forward (needs DDNS, less secure). |
| **Client (human)** | **Element** — web, iOS, Android | Polished, mature, free. Tim installs Element on phone + uses Element Web on desktop. | Cinny, Schildi, FluffyChat, Hydrogen — alternatives, less polished. Custom client we build later if Element's UX falls short. |
| **Theme** | **Custom cyberpunk-X3Native** (Element theme JSON + custom CSS) | Element supports per-deployment themes. Dark background, neon-cyan accents on bold, monospace code spans, X3Native logo as default workspace icon. | Stock Element Dark — works fine but doesn't match the fleet aesthetic. |
| **Identity model** | **One Matrix account per machine** | Mirrors the per-machine Slack bot pattern. `@djbooth:tims-fleet.xyz`, `@13700k:...`, etc. Each Claude session on that machine logs in as its host. `@tim:...` is the human admin. | Single shared `@fleet-bot` — easier setup but loses identity granularity; can't `@14900k specifically`. |
| **Bot SDK** | **matrix-bot-sdk** on Node.js (the Node.js v24 already installed at `C:\Program Files\nodejs\`) | Mature, Tim already has the stack working for the local Slack MCP. Node.js is the lingua franca of MCP servers and we'll reuse the runtime. | matrix-nio (Python, also great — usable once Python 3.13 install finishes); custom Rust client (overkill). |
| **Bot architecture** | **One persistent daemon per machine** (long-running Node process, auto-start via Windows Scheduled Task) | Pattern proven by the just-built Slack daemon. Daemon receives Matrix events → writes to local inbox → Claude session drains on prompt or via `/loop`. Persistent connection = sub-second latency for incoming messages. | Polling — simpler but defeats the "live" reason to use Matrix. |
| **Channel taxonomy** | See §6 | Mirrors Slack's `#fleet-ops`, plus new fleet-specific channels not feasible on free Slack. | — |
| **Migration** | **30-day soak with Slack coexistence**, then cutover | Slack stays as Tim's phone notification channel until Matrix proves stable. Optional `mautrix-slack` bridge can mirror between if Tim wants. | Hard cutover (risky); Slack-only (defeats the spec). |

## 3. Architecture overview

```
                          ┌─────────────────────────────────┐
                          │  Cloudflare Tunnel              │
                          │  https://chat.tims-fleet.xyz    │
                          └─────────┬───────────────────────┘
                                    │ (HTTP/2 via cloudflared)
                                    ▼
┌───────────────────────────────────────────────────────────────┐
│  13700K Command Center (Win11)                                │
│  ┌─────────────────────────────────────────────────┐          │
│  │  Conduit (Rust binary, ~12 MB)                  │          │
│  │  - Federation disabled                          │          │
│  │  - SQLite at C:\opt\conduit\db\                 │          │
│  │  - Bind 127.0.0.1:6167 (cloudflared talks here) │          │
│  │  - Server name: tims-fleet.xyz                  │          │
│  │  - Auto-restart via Scheduled Task at boot      │          │
│  │  - Backup: nightly rclone copy to ~/Dropbox/    │          │
│  └─────────────────────────────────────────────────┘          │
└─────────┬─────────────────────────────────────────────────────┘
          │
          ▼ LAN (gigabit between fleet PCs, Cat6 upgrade pending)
          │
┌─────────┴───────────────────────────────────────────────────────┐
│  Per-fleet-PC Matrix bot daemon (Node.js, matrix-bot-sdk)       │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐           │
│  │ DJBOOTH  │ │  14900K  │ │  i5000   │ │ I4400    │  ...etc   │
│  │ daemon   │ │ daemon   │ │ daemon   │ │ daemon   │           │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘           │
│  Each:                                                          │
│   - Logs in as @<machine>:tims-fleet.xyz                        │
│   - Subscribes to all fleet rooms                               │
│   - On incoming event → append to ~/.claude/.matrix_inbox.jsonl │
│   - Exposes a local Unix socket (or named pipe on Windows) for  │
│     Claude session to POST outgoing messages                    │
│   - Auto-start via Windows Scheduled Task at logon              │
└─────────────────────────────────────────────────────────────────┘
          │
          ▼ Element clients (human)
┌─────────────────────────────────────────────────────────────────┐
│  Tim's iPhone      Tim's laptop      Future fleet members      │
│  Element iOS       Element Web/Desktop  Element on any platform │
│                                                                 │
│  Custom theme: cyberpunk-X3Native (CSS override + theme JSON)   │
│  Custom widgets: fleet-status sidebar (presence + branch state) │
└─────────────────────────────────────────────────────────────────┘
```

## 4. Component spec

### 4.1 Conduit server (on 13700K)

**Binary:** download from `https://gitlab.com/famedly/conduit/-/releases` (latest stable, currently 0.10.x as of 2026-05). Single static binary.

**Install location:** `C:\opt\conduit\conduit.exe` + config at `C:\opt\conduit\conduit.toml` + database at `C:\opt\conduit\db\`.

**Config (conduit.toml) essentials:**
```toml
[global]
server_name = "tims-fleet.xyz"
database_backend = "sqlite"
database_path = "C:/opt/conduit/db"
port = 6167
address = "127.0.0.1"  # cloudflared fronts it
allow_registration = false  # admin creates accounts manually
registration_token = "<generate-random-32-byte-hex>"  # for registering new fleet members
allow_federation = false  # off initially
allow_encryption = true
allow_check_for_updates = true
max_request_size = 20_000_000  # 20 MB for file uploads
trusted_servers = []  # no federation peers
```

**Service:** Windows Scheduled Task with `At startup` trigger, runs `C:\opt\conduit\conduit.exe`, `If task fails: restart every 1 min for 3 attempts`, `Run whether user is logged on or not` (so it survives logoff).

**Backup:** Nightly Scheduled Task at 03:00 runs `rclone sync C:\opt\conduit\db C:\Users\Tim Smith\Dropbox\fleet-backups\conduit\` (Tim already has Dropbox at `D:\Dropbox`, redirect path as needed).

**Initial admin account:** Tim creates `@tim:tims-fleet.xyz` via Conduit's admin command on first boot. From there, Tim creates the per-machine accounts.

### 4.2 Cloudflare Tunnel

**Install:** `cloudflared.exe` from Cloudflare on 13700K. Runs as another Scheduled Task at startup.

**Config (`config.yml`):**
```yaml
tunnel: <UUID-from-cloudflared-create>
credentials-file: C:\Users\Tim Smith\.cloudflared\<UUID>.json
ingress:
  - hostname: chat.tims-fleet.xyz
    service: http://127.0.0.1:6167
  - service: http_status:404
```

**DNS:** Cloudflare zone for `tims-fleet.xyz` (or whatever domain Tim picks). One CNAME `chat → <tunnel-UUID>.cfargotunnel.com`.

### 4.3 Per-machine matrix-bot-sdk daemon

**Path:** `~/.claude/matrix-daemon/` on each machine.

**Stack:**
- Node.js v24 (already installed on DJBOOTH at `C:\Program Files\nodejs\`)
- `matrix-bot-sdk` npm package
- A small (~200 lines) `daemon.js` per machine

**daemon.js responsibilities:**

1. **Login** as `@<machine>:tims-fleet.xyz` using an access token saved at `~/.claude/.matrix_token` (mode 700)
2. **Sync loop** — persistent connection to the homeserver, receives events as they arrive (sub-second latency)
3. **Inbox writer** — for every message in a room the machine is in OR direct message to the machine: append a JSON line to `~/.claude/.matrix_inbox.jsonl` (parallels the existing Slack inbox pattern)
4. **Outbox listener** — local IPC (Windows named pipe `\\.\pipe\matrix-<machine>` or HTTP localhost:47XXX) — Claude session POSTs an outgoing message + room + reply-to; daemon ships it to Matrix
5. **Presence** — daemon emits `m.presence` updates every 5 min indicating the machine is alive; Element clients show fleet machines as 🟢 / 🟡 / ⚫️ in the sidebar widget
6. **Reconnection** — on connection loss, exponential backoff retry; logs to `~/.claude/.matrix-daemon.log`

**Auto-start:** Windows Scheduled Task with `At logon` trigger, `Run whether user is logged on or not`. Runs `C:\Program Files\nodejs\node.exe %USERPROFILE%\.claude\matrix-daemon\daemon.js`.

### 4.4 Element clients (Tim's UX)

**Web:** Element Web hosted on the Conduit box at `https://chat.tims-fleet.xyz/` (Element Web is just static files behind the same Cloudflare Tunnel). Optional: serve Element Web from a separate subdomain to keep the homeserver and client separable.

**iOS / Android:** Tim installs Element from the App Store / Play Store. Logs in with homeserver `tims-fleet.xyz` + his account + password. Push notifications work via Element's standard push gateway (Matrix supports this out of the box).

**Desktop:** Element Desktop (Electron app) on Tim's laptop. Same login.

### 4.5 Custom theme (cyberpunk-X3Native)

Element supports themes via JSON. Spec:

```css
/* Base palette — matches X3Native engine aesthetic */
--bg: #0a0e1a         /* near-black with blue undertone */
--bg-panel: #121826   /* slightly lighter panel */
--bg-elevated: #1a2333  /* room cards, sidebar items */
--text: #d4dde6       /* off-white, slightly cool */
--text-dim: #8590a0   /* metadata, timestamps */
--accent: #00ffd5     /* portal-cyan from rifthub */
--accent-violet: #b94dff  /* Memory Hunter / Crystal Heart */
--accent-amber: #ff8c1a   /* alien sky */
--success: #00ff88
--warning: #ffaa00
--danger: #ff3366
--code-bg: #0f1420
--code-text: #00ffd5  /* code spans pop in portal-cyan */
--font-mono: "JetBrains Mono", "Fira Code", "Cascadia Code", monospace
--font-ui: "Inter", "Segoe UI", system-ui, sans-serif
--font-display: "Orbitron", "Inter", system-ui  /* for headers + machine names */
```

**Logo:** Use X3Native's existing branding (TBD asset from the repo). Default avatar fallback is a stylized machine-name token (e.g., "DJB", "13K", "I50") in matching cyan against the panel color.

**Density:** Compact mode by default. Tim is a power user who reads dense IDE-style UI. Element's compact-mode toggle stays ON.

### 4.6 Custom fleet-status sidebar widget

Matrix supports custom **widgets** — iframe-embedded mini-apps that show inside a room's right sidebar. We build a small HTML/JS widget served from the Conduit host that displays:

| Cell | Value |
|---|---|
| DJBOOTH | 🟢 — feat/portal-hub @ `3729d29` — last commit 6h ago |
| 13700K | 🟢 — main @ `0b6ab79` — integrator |
| 14900K | 🟡 — feat/multi-font-roles @ `8d74d74` — 49 behind main |
| I5000 | ⚫️ — last seen 4d ago — feat/act2-desert not pushed |
| I4400 | 🟢 — new install, no branches yet |
| Snake | 🟢 — feat/floors2-7-dims @ `0e59fe7` |
| MOB BOSS | ⚫️ — dormant |
| laptop OG | 🟡 — Tim's hands-on |

Data source: a small JSON file at `https://chat.tims-fleet.xyz/fleet-status.json` populated by a cron on 13700K that:
- `git fetch origin && git for-each-ref refs/remotes/origin` to get each branch's HEAD
- pings each machine's matrix-daemon health endpoint
- writes the JSON

Refreshes every 60s. Widget polls and re-renders.

## 5. Bot architecture & Claude-to-Claude RPC

### 5.1 The three communication patterns

1. **Human → Claude** (Tim DMs a machine): Tim opens Element → DMs `@djbooth` → DJBOOTH's daemon receives → writes to inbox → next Claude prompt on DJBOOTH reads + responds
2. **Claude → Human** (a machine pings Tim): Claude on the source machine writes to its outbox pipe → daemon posts a DM to `@tim` → Tim sees push notification on phone
3. **Claude → Claude** (fleet-internal RPC): Claude on DJBOOTH writes to its outbox pipe → daemon posts in `#fleet-ops` mentioning `@14900k` → 14900K's daemon receives → writes to 14900K's inbox → next 14900K Claude session reads it

Pattern 3 is the new unlock vs Slack. With per-machine Matrix accounts and persistent daemons, mention-based fleet RPC is reliable and sub-second.

### 5.2 What the daemon stores

```
~/.claude/.matrix_token           — bot's access token, mode 700
~/.claude/.matrix_inbox.jsonl     — incoming messages addressed to this machine
~/.claude/.matrix_outbox.jsonl    — outgoing log (audit trail)
~/.claude/.matrix-daemon.log      — daemon runtime log
~/.claude/.matrix_seen.json       — sync token (resume after restart)
```

### 5.3 Outbox API surface (named pipe on Windows)

Claude on a fleet PC POSTs JSON to `\\.\pipe\matrix-<machine>`:

```json
{
  "room": "#fleet-ops",
  "text": "Snake's act2-world is ready for review — branch feat/openworld",
  "mention": ["@snake", "@13700k"],
  "thread_root": "$evtid:tims-fleet.xyz"
}
```

The daemon dispatches it via `matrix-bot-sdk`'s `sendText` / `sendMessage`.

## 6. Channel taxonomy

| Channel | Purpose | Members |
|---|---|---|
| `#fleet-ops` | Primary fleet coordination — dispatches, status, broadcasts | All machines + Tim |
| `#x3native-game-engine` | X3Native engine work — same as Slack today | All machines + Tim |
| `#build-status` | Automated CI/build results — bot-only posts, humans read | Build-bot + Tim |
| `#integration-queue` | Integrator-facing — branches awaiting merge | 13700K + Tim |
| `#presence` | Heartbeat-style "I'm awake" pings — quiet by default | All machines |
| `#social` | Non-work — same as Slack today | Tim + selected humans |
| DMs `@<machine>` | Per-machine direct messages — Tim's primary input channel | Tim ↔ each machine |

## 7. Custom features (phased)

### Phase 1 (week 1)
1. Conduit deployment on 13700K
2. Cloudflare Tunnel + Element web at `chat.tims-fleet.xyz`
3. Element iOS + Element Web login for Tim
4. DJBOOTH and 13700K matrix-daemon (the two always-on boxes; bring others online incrementally)
5. `#fleet-ops` channel
6. Slack DJBOOTH-Bot bridges OR mirrors messages from `#fleet-ops` to Slack `#fleet-ops` (one-way during soak)

### Phase 2 (week 2–3)
1. Cyberpunk-X3Native theme deployed
2. Fleet-status sidebar widget (machine presence + branch state)
3. Remaining matrix-daemons on 14900K, I5000, I4400, Snake, MOB BOSS, laptop OG
4. `#build-status` + a build-bot that posts CI gauntlet results
5. Mention-based Claude-to-Claude RPC patterns documented in `docs/CLEANROOM_PROCESS.md`

### Phase 3 (week 4+)
1. `mautrix-slack` bridge — full two-way ferry between Matrix and Slack
2. Slack cutover after 30 days of Matrix stability
3. Automated `NOTE_TO_*.md` mirroring (when a NOTE_TO_X.md is committed, post the body to `@x`'s DM)
4. "Wake the fleet" command: `@13700k /wake-fleet` triggers a presence ping cycle and reports who's alive
5. Custom Element extensions (TBD based on usage patterns)

## 8. Migration from current Slack setup

| State | When | What |
|---|---|---|
| **Slack-only** | today | Status quo. DJBOOTH-Bot posts to `#fleet-ops` on Slack. |
| **Both, Slack primary** | day 1 of Matrix | Matrix `#fleet-ops` opens, daemon on DJBOOTH + 13700K. Tim's phone notifications still come from Slack. Cross-post happens via a tiny script (Slack daemon also writes to Matrix outbox). |
| **Both, Matrix primary** | day 7–14 | Tim switches phone notifications to Element. Slack becomes a fallback / archive. mautrix-slack bridge installed and configured. |
| **Matrix-only** | day 30+ | Slack DJBOOTH-Bot retired (or kept dormant). All fleet ops on Matrix. Slack workspace can be archived or kept for nostalgia. |

## 9. Open questions for Tim

1. **Domain name** — `tims-fleet.xyz` is a placeholder. What domain do you want? Need to register or use one you own.
2. **Cloudflare account** — do you already have a Cloudflare account? (Required for the Tunnel; free tier is fine.)
3. **Conduit registration token** — happy with a generated random one, OR want a memorable phrase (like `xkcd-correct-horse`)?
4. **Backup destination** — Dropbox at `D:\Dropbox\fleet-backups\conduit\` works? Or somewhere else?
5. **Element web hosting** — same subdomain as the homeserver (`chat.tims-fleet.xyz`) or separate (`elements.tims-fleet.xyz`)?
6. **Custom widget priorities** — fleet-status sidebar is in Phase 2 by default; want it Phase 1 instead?

## 10. Non-goals (explicit)

- **Mobile app development** — we use stock Element iOS/Android. No custom mobile app this phase.
- **Voice / video** — Matrix supports it but not relevant for fleet ops.
- **End-to-end encryption** — Available in Matrix, but adds complexity (key sharing across daemons). Defer to Phase 3 or later if Tim wants it.
- **Public-facing Matrix** — federation stays off. This is a private fleet server.
- **AI/LLM features inside the chat UI** — Claude lives in the CC terminal, talks via the daemon. We don't try to embed Claude into Element directly.

## 11. Success criteria

The system is "done" (Phase 1 complete) when:

- ✅ Tim can DM `@djbooth` from his iPhone over cellular and DJBOOTH's next Claude session receives + responds
- ✅ DJBOOTH can mention `@13700k` in `#fleet-ops` and 13700K's daemon writes it to its inbox in under 1 second
- ✅ Conduit's database survives a 13700K reboot without manual intervention
- ✅ Cloudflare Tunnel auto-reconnects after WAN flap
- ✅ Element Web at `chat.tims-fleet.xyz` shows the cyberpunk theme
- ✅ Backup script writes a fresh DB snapshot to Dropbox nightly
- ✅ The system has been up 168 consecutive hours (1 week) without manual intervention

---

*Spec authored autonomously during Tim's 8-hour overnight authorization, 2026-05-27. Awaiting Tim's review on §9 open questions before implementation.*
