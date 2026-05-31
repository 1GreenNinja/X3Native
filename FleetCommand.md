# FleetCommand — Matrix Messaging Bring-Up (Phase 1)

**From:** DJBOOTH (4790K, garage) · **To:** 13700K (integrator)
**Date:** 2026-05-31
**Branch:** `feat/fleet-messaging-design`
**Full plan (verbatim commands):** `docs/superpowers/plans/2026-05-27-fleet-messaging-phase1-plan.md`
**Spec:** `docs/superpowers/specs/2026-05-27-fleet-messaging-design.md`

---

## TL;DR

We're standing up a self-hosted **Matrix** fabric to replace Slack for fleet
coordination. Conduit homeserver + Cloudflare Tunnel + Element Web, with a
per-machine Node bot daemon. **The homeserver lives on YOU (13700K)** — it needs
an always-on box, and that's the integrator. DJBOOTH's half (the bot daemon) is
already built and tested.

Production domain: **`fleetcommand.slopclaude.com`**
Homeserver bind: **`127.0.0.1:6167`** (never exposed directly — tunnel only)

---

## Current state (as of 2026-05-31)

- ✅ **Spec + Phase 1 & 2 plans written** and on this branch.
- ✅ **DJBOOTH matrix-daemon built** (`~/.claude/matrix-daemon/`): `config.js`,
  `login.js`, `inbox.js`, `outbox.js`, `daemon.js` — **22/22 jest tests pass.**
  This is the "app." It just needs a real bot token + the homeserver live.
- 🔄 **DNS migration in progress.** `slopclaude.com` nameservers moved from
  GoDaddy → Cloudflare (`demi.ns.cloudflare.com` / `tim.ns.cloudflare.com`).
  Propagating now; DJBOOTH is auto-watching for the flip.
  - DNSSEC: OFF (verified) — leave it off during bring-up.
  - Existing apps preserved as **DNS-only** in Cloudflare: `realbiblestudy`,
    `warroom` (both → 76.76.21.21 Vercel), `www`, apex, `pay` (GoDaddy payments).
  - `fleetcommand` subdomain is reserved for the tunnel — do NOT point it at
    Vercel; `cloudflared` will create its own CNAME.

---

## ⛔ Gate: do not start until DNS is Active

Confirm the cutover landed before touching Conduit:

```powershell
(Resolve-DnsName slopclaude.com -Type NS -Server 1.1.1.1).NameHost
# Expected: demi.ns.cloudflare.com / tim.ns.cloudflare.com (NOT pdns0X.domaincontrol.com)
```

DJBOOTH will post in #fleet-ops (or Slack during the soak) when it sees the flip.

---

## What YOU (13700K) must do

Each item maps to a Task block in the Phase 1 plan — go there for exact,
copy-paste commands. This is the outline + the gotchas.

### 1. Conduit homeserver — Plan Tasks 4–9
- `C:\opt\conduit\{db,logs}\`
- Download `conduit.exe` (gitlab.com/famedly/conduit releases, latest stable).
- Write `conduit.toml`: `server_name = "fleetcommand.slopclaude.com"`, sqlite,
  `port = 6167`, `address = "127.0.0.1"`, `allow_registration = true`,
  a **32-byte hex `registration_token`** (generate + SAVE it — Tim needs it to
  make the first account), `allow_federation = false`.
- First-boot foreground, confirm `GET /_matrix/client/versions` returns JSON.
- Install as Scheduled Task **`Conduit-Homeserver`** (`-AtStartup`, auto-restart).
- **Reboot 13700K and confirm it auto-starts.** ← this is the whole point of
  putting it on you; verify resilience.

### 2. Cloudflare Tunnel — Plan Tasks 10–16
- `winget install Cloudflare.cloudflared`
- `cloudflared tunnel login` (browser → pick slopclaude.com / Tim's CF account).
- `cloudflared tunnel create djbooth-fleet-chat` → **note the UUID.**
- `cloudflared tunnel route dns djbooth-fleet-chat fleetcommand.slopclaude.com`
  (this creates the proxied CNAME in Cloudflare automatically).
- `config.yml` ingress → `http://127.0.0.1:6167`.
- Smoketest: from any external box / phone on cellular,
  `https://fleetcommand.slopclaude.com/_matrix/client/versions` → 200 + JSON.
- Install as Scheduled Task **`CloudflareTunnel-Fleet`**, reboot-test again.

### 3. Element Web client — Plan Tasks 17–20
- Download Element Web tarball → `C:\opt\element-web\`.
- `config.json` → `base_url: https://fleetcommand.slopclaude.com`.
- Serve via `http-server` on `:8080` (Scheduled Task `ElementWeb-Static`).
- Update tunnel `config.yml` to path-route: `/_matrix/*` + `/.well-known/*` →
  `:6167`, everything else → `:8080`.
- Tim opens `https://fleetcommand.slopclaude.com/`, creates account **`tim`**
  with the registration token (first user → admin).

### 4. Bot accounts — Plan Tasks 21–22
- From `@tim`, mint per-bot registration tokens via the `@conduit` admin DM.
- Register **`@13700k`** on this machine; save its access token to
  `~/.claude/.matrix_token` (mode 700). **Use 13700K's own token — not DJBOOTH's.**
- DJBOOTH will register **`@djbooth`** on its end with its own token.

### 5. Replicate the daemon to 13700K — Plan Task 30
The daemon is machine-agnostic; it keys off `MATRIX_BOT_MACHINE`.
```powershell
robocopy "\\DJBOOTH\C$\Users\Tim Smith\.claude\matrix-daemon" "$env:USERPROFILE\.claude\matrix-daemon" /E /XD node_modules
cd "$env:USERPROFILE\.claude\matrix-daemon"; & 'C:\Program Files\nodejs\npm.cmd' install
$env:MATRIX_BOT_MACHINE = '13700k'   # → pipe name \\.\pipe\matrix-13700k
& 'C:\Program Files\nodejs\node.exe' daemon.js   # expect "starting-sync" in the log
```
- Confirm `~/.claude/.matrix_token` holds **13700K's** token (Task 4 above).
- Install as Scheduled Task **`Integrator-MatrixDaemon`**.

### 6. #fleet-ops + first cross-message — Plan Tasks 31–32
- Tim creates room **Fleet Ops**, invites `@djbooth` + `@13700k`
  (AutojoinRoomsMixin auto-joins them).
- DJBOOTH pings the room via its pipe → 13700K's inbox gets it < 1s → 13700K
  replies. Round-trip proves the fabric.

### 7. (Lower priority) Backup — Plan Task 35
- Nightly `rclone sync` of `C:\opt\conduit\db` → Dropbox `fleet-backups/conduit/`,
  03:00 Scheduled Task. Can wait until after the round-trip works.

---

## Note directly to the 13700K Claude session

You're the integrator and the always-on node, so the homeserver is yours. A few
things to keep straight:

- **Don't expose 6167 publicly.** Conduit binds to loopback; the tunnel is the
  ONLY ingress. No port-forwarding, no firewall holes. If you're tempted to open
  a port, stop — that's not the design.
- **`allow_federation = false`** stays false for Phase 1. This is a private fleet
  server, not part of the public Matrix network.
- **The registration token is a secret** — it lets anyone make an account. Save
  it where Tim can retrieve it, don't paste it into the repo.
- **Each machine uses its OWN bot token.** Copying DJBOOTH's token to 13700K
  would make both bots the same user — don't.
- Reboot-test Conduit AND the tunnel. The success criterion is "survives a
  reboot with zero hands," because that's what an integrator box is for.
- Full verbatim commands + expected outputs are in the Phase 1 plan — follow it
  task-by-task rather than improvising. Ping back in #fleet-ops if Conduit's
  first-boot JSON check fails; that's the usual first snag.

---

## What DJBOOTH does once you're up

1. Register `@djbooth` (own token) against the live homeserver.
2. Point its already-built daemon at `fleetcommand.slopclaude.com`, install its
   Scheduled Task.
3. Slack daemon gets a one-way mirror → Matrix during the 30-day soak (Task 33),
   so nothing's lost while we cut over.

Success = Tim DMs `@djbooth` from his phone over cellular and DJBOOTH's next
Claude session sees + answers it. That's the finish line for Phase 1.
