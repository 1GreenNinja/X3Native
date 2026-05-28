# Fleet Messaging Phase 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build on Phase 1's bare Matrix homeserver + 2 daemons. Deploy the cyberpunk-X3Native theme to Element Web, stand up the fleet-status sidebar widget, and bring the remaining 6 fleet machines onto Matrix as bots (14900K, I5000, I4400, Snake, MOB BOSS, laptop OG).

**Architecture:** Phase 1 already shipped the design + bot daemon + theme + widget skeleton as source. Phase 2 is the **deploy + integrate** half. No new architecture; mostly configuration + replication.

**Tech Stack:** Conduit (already running on 13700K), Element Web (already deployed at chat.&lt;CHOSEN_DOMAIN&gt;), matrix-bot-sdk (already validated on DJBOOTH + 13700K), Node.js v24, PowerShell Scheduled Tasks, http-server static serving.

**Pre-conditions:**
- Phase 1 complete (Conduit live, Cloudflare Tunnel up, Element Web accessible, DJBOOTH + 13700K daemons running, #fleet-ops working, Slack→Matrix mirror live)
- Phase 1 has been running stable for at least 24 hours
- Tim has decided which fleet machines to onboard first (default: 14900K next — showcase rig + most-active)

**Branch:** `feat/fleet-messaging-phase2` (separate from Phase 1's branch).

---

## Phase 2.A — Deploy the cyberpunk theme

### Task 1: Deploy theme.css to Element Web webroot

**Files:**
- Source: `D:\GameDev\X3Native\tools\element-theme\theme.css`
- Destination on 13700K: `C:\opt\element-web\themes\cyberpunk-x3native.css`

- [ ] **Step 1: Copy theme.css to Element's webroot.**

```powershell
$src = 'D:\GameDev\X3Native\tools\element-theme\theme.css'
$dst = 'C:\opt\element-web\themes'
New-Item -ItemType Directory -Force -Path $dst
Copy-Item $src "$dst\cyberpunk-x3native.css"
```

- [ ] **Step 2: Update `C:\opt\element-web\index.html` to load the theme.**

Add to the `<head>` section, before any other `<link>` tags:

```html
<link rel="stylesheet" href="themes/cyberpunk-x3native.css" />
```

- [ ] **Step 3: Update `C:\opt\element-web\config.json` to mark the theme as default.**

Add or update:
```json
"default_theme": "cyberpunk-x3native"
```

- [ ] **Step 4: Restart the Element Web static-server scheduled task.**

```powershell
Stop-ScheduledTask -TaskName 'ElementWeb-Static'
Start-Sleep -Seconds 2
Start-ScheduledTask -TaskName 'ElementWeb-Static'
```

- [ ] **Step 5: Verify in browser.**

Open `https://chat.<CHOSEN_DOMAIN>/` — should load with dark blue-black background, cyan accents on the login form. Log in as `@tim` to confirm the full app applies the theme.

- [ ] **Step 6: Commit.**

```bash
cd D:\GameDev\X3Native
# No repo changes (deploy is to C:\opt\element-web on 13700K, not the repo)
# Optional: update tools/element-theme/README.md to note "deployed 2026-MM-DD"
git add tools/element-theme/README.md
git commit -m "element-theme: mark cyberpunk-x3native as deployed"
```

### Task 2: Per-user theme verification

- [ ] **Step 1: Element on Tim's iPhone — Element iOS doesn't load custom CSS,** only the JSON theme variables. Verify it applied the JSON palette (colors, fonts).
- [ ] **Step 2: Take screenshots of phone + web for the spec.**
- [ ] **Step 3: If anything reads wrong (Element class names shifted between versions), bump `tools/element-theme/theme.css` with the corrected selectors, redeploy.**

---

## Phase 2.B — Deploy the fleet-status widget

### Task 3: Copy widget bundle to Element Web's static serving

**Files:**
- Source: `D:\GameDev\X3Native\tools\fleet-status\widget\`
- Destination on 13700K: `C:\opt\element-web\fleet-widget\`

- [ ] **Step 1: Copy the widget files.**

```powershell
$src = 'D:\GameDev\X3Native\tools\fleet-status\widget'
$dst = 'C:\opt\element-web\fleet-widget'
New-Item -ItemType Directory -Force -Path $dst
Copy-Item -Recurse "$src\*" $dst
```

- [ ] **Step 2: Symlink fleet-status.json into the widget dir.**

The widget loads `fleet-status.json` relative to its own URL; the generator (Task 5 below) writes to `C:\opt\element-web\fleet-status.json`. A symlink keeps both convenient:

```powershell
New-Item -ItemType SymbolicLink `
  -Path C:\opt\element-web\fleet-widget\fleet-status.json `
  -Target C:\opt\element-web\fleet-status.json
```

(If symlinks aren't permitted on this system, copy on every generator run instead.)

- [ ] **Step 3: Verify widget loads.**

Open `https://chat.<CHOSEN_DOMAIN>/fleet-widget/` in a browser — should show the cyberpunk fleet grid. Initially the JSON file doesn't exist yet so the error fallback shows. That's expected until Task 5 runs.

### Task 4: Add the widget to #fleet-ops as a Matrix widget

- [ ] **Step 1: In Element, open `#fleet-ops` room.**
- [ ] **Step 2: Click info icon → Widgets → Add Custom.**
- [ ] **Step 3: URL: `https://chat.<CHOSEN_DOMAIN>/fleet-widget/`. Name: `Fleet Status`. Save.**
- [ ] **Step 4: Right sidebar now shows the widget iframe.** Currently displays the error state since fleet-status.json isn't generated yet.

### Task 5: Deploy + start the fleet-status generator

**Files:**
- Source: `D:\GameDev\X3Native\tools\fleet-status\generate.ps1`
- Destination on 13700K: `C:\opt\fleet-status\generate.ps1`

- [ ] **Step 1: Copy the generator script.**

```powershell
$src = 'D:\GameDev\X3Native\tools\fleet-status\generate.ps1'
$dst = 'C:\opt\fleet-status'
New-Item -ItemType Directory -Force -Path $dst
Copy-Item $src $dst
```

- [ ] **Step 2: Adjust `$RepoDir` at the top of the script** to the actual repo path on 13700K (probably `D:\GameDev\X3Native` if you cloned the same place).

- [ ] **Step 3: Manually run once to test.**

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File C:\opt\fleet-status\generate.ps1
# Then verify
Get-Content C:\opt\element-web\fleet-status.json | Out-String
```

- [ ] **Step 4: Register as scheduled task (every 60s).**

```powershell
$gen = 'C:\opt\fleet-status\generate.ps1'
$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
  -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$gen`""
$trigger = New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(1) `
  -RepetitionInterval (New-TimeSpan -Minutes 1)
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable `
  -ExecutionTimeLimit (New-TimeSpan -Minutes 1) -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive
Register-ScheduledTask -TaskName 'FleetStatus-Generator' `
  -Action $action -Trigger $trigger -Settings $settings -Principal $principal `
  -Description 'Regenerates fleet-status.json every minute.'
Start-ScheduledTask -TaskName 'FleetStatus-Generator'
```

- [ ] **Step 5: Verify widget now shows real data.** Refresh `https://chat.<CHOSEN_DOMAIN>/fleet-widget/` — should show DJBOOTH + 13700K + (any other machines with known LAN IPs) with their actual branch states.

---

## Phase 2.C — Onboard remaining fleet machines

### Task 6: 14900K matrix-bot onboarding

This task is a template — repeat for each remaining machine (i5000, i4400, snake, mob_boss, laptop_og) substituting the machine name.

**Files (on 14900K):**
- Create: `~/.claude/matrix-daemon/` (clone of the daemon source)
- Create: `~/.claude/.matrix_token` (bot's access token, mode 700)

- [ ] **Step 1: On 13700K, generate a registration token for the new bot.**

In Element, DM `@conduit`:
```
!admin create-token 14900k-bot-token
```

Record the token.

- [ ] **Step 2: On 14900K, install Node.js if missing.**

```powershell
winget install OpenJS.NodeJS.LTS --silent --accept-package-agreements
```

- [ ] **Step 3: On 14900K, clone the repo or pull the matrix-daemon subtree.**

```powershell
cd D:\GameDev  # adjust to wherever the repo lives on 14900K
git clone https://github.com/1GreenNinja/X3Native.git    # or git pull if already cloned
```

- [ ] **Step 4: Deploy matrix-daemon on 14900K.**

```powershell
$src = 'D:\GameDev\X3Native\tools\matrix-daemon'
$dst = "$env:USERPROFILE\.claude\matrix-daemon"
New-Item -ItemType Directory -Force -Path $dst
Copy-Item -Recurse -Force "$src\*" $dst
cd $dst
& 'C:\Program Files\nodejs\npm.cmd' install
```

- [ ] **Step 5: Register the bot account via Matrix API.**

```bash
TOKEN_NEW='<14900k-bot-token-from-step-1>'
BOT_PW=$(openssl rand -hex 24)
curl -s -X POST "https://chat.<CHOSEN_DOMAIN>/_matrix/client/v3/register" \
  -H "Content-Type: application/json" \
  -d "{
    \"username\": \"14900k\",
    \"password\": \"$BOT_PW\",
    \"auth\": { \"type\": \"m.login.registration_token\", \"token\": \"$TOKEN_NEW\" }
  }" > /tmp/14900k-register.json
cat /tmp/14900k-register.json
```

Save the password to Tim's password manager. Extract the `access_token` and save to `~/.claude/.matrix_token` on 14900K (mode 700).

- [ ] **Step 6: Set the `MATRIX_BOT_MACHINE` env var so the daemon identifies as `14900k`.**

```powershell
[System.Environment]::SetEnvironmentVariable('MATRIX_BOT_MACHINE','14900k','User')
```

- [ ] **Step 7: Smoketest the daemon.**

```powershell
$env:MATRIX_BOT_MACHINE = '14900k'
& 'C:\Program Files\nodejs\node.exe' "$env:USERPROFILE\.claude\matrix-daemon\daemon.js"
# Expect "identified" log line with user_id @14900k:<CHOSEN_DOMAIN>
# Ctrl+C to stop
```

- [ ] **Step 8: Register Scheduled Task at logon.**

```powershell
$daemonPath = "$env:USERPROFILE\.claude\matrix-daemon\daemon.js"
$action = New-ScheduledTaskAction -Execute 'C:\Program Files\nodejs\node.exe' -Argument $daemonPath -WorkingDirectory "$env:USERPROFILE\.claude\matrix-daemon"
$trigger = New-ScheduledTaskTrigger -AtLogOn
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -RestartCount 5 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit (New-TimeSpan -Days 365) -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive
Register-ScheduledTask -TaskName '14900K-MatrixDaemon' -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Description 'Matrix bot daemon for @14900k.'
Start-ScheduledTask -TaskName '14900K-MatrixDaemon'
```

- [ ] **Step 9: From Element on Tim's account, invite `@14900k:<CHOSEN_DOMAIN>` to `#fleet-ops`.**

The AutojoinRoomsMixin in `login.js` auto-accepts. Verify in the daemon log:

```bash
tail -5 ~/.claude/.matrix-daemon.log
# Expect: a join event line
```

- [ ] **Step 10: Verify the widget shows 14900K as 🟢 online.**

Refresh `https://chat.<CHOSEN_DOMAIN>/fleet-widget/`. 14900K should now appear with its LAN IP + current branch.

- [ ] **Step 11: Update `~/.claude/.fleet_hosts.json` on every fleet PC** to include 14900K's real LAN IP. The widget reads from 13700K's copy.

### Task 7: I5000 onboarding (repeat Task 6)

### Task 8: I4400 onboarding (repeat Task 6)

### Task 9: Snake onboarding (repeat Task 6)

### Task 10: MOB BOSS onboarding (repeat Task 6)

### Task 11: laptop OG onboarding (repeat Task 6 — but note: laptop's machine name may differ from the bot name)

---

## Phase 2.D — Production-ize

### Task 12: Disable allow_registration in conduit.toml

Once all 8 bots + Tim are registered, lock down further registrations.

- [ ] **Step 1: Edit `C:\opt\conduit\config\conduit.toml`:**

```toml
allow_registration = false
```

- [ ] **Step 2: Restart Conduit container.**

```powershell
docker restart conduit
```

- [ ] **Step 3: Verify by attempting to register a fake account — should fail.**

### Task 13: Rotate Conduit registration_token

Even with `allow_registration = false`, the leaked token shouldn't sit in plaintext. Generate a new one (same procedure as Plan-1 Task 6 step 1) and update `conduit.toml`, then restart.

### Task 14: First Phase 2 smoketest — full fleet round-trip

- [ ] **Step 1: Tim DMs `@14900k` from his phone — verify 14900K's daemon receives it.**
- [ ] **Step 2: From DJBOOTH's CC, send via the named-pipe outbox: a `#fleet-ops` post mentioning `@14900k`. Verify 14900K's inbox receives it within 1s.**
- [ ] **Step 3: Widget reflects new branch state when DJBOOTH pushes a commit.**

---

## Phase 2 success criteria

- [ ] Cyberpunk-X3Native theme renders correctly on Element Web + phone
- [ ] Fleet-status widget shows all 8 machines (online/dormant per actual state)
- [ ] At least 14900K + DJBOOTH + 13700K daemons all running 24/7
- [ ] Sub-second Claude-to-Claude RPC via Matrix mention works between any 3 of those
- [ ] No regressions in Phase 1: Conduit + Cloudflare Tunnel + Element Web still healthy
- [ ] Slack daemon still polling + mirroring (Tim's phone push still works on Slack as fallback)

---

## What this plan deliberately doesn't cover

- **Phase 3** (mautrix-slack bridge, cutover ceremony) — separate plan at `tools/mautrix-slack-bridge/README.md`
- **Custom widget interactivity** (clicking a branch to kick off the gauntlet, etc.) — Phase 4+
- **End-to-end encryption** — Matrix supports it; for a private fleet on a Cloudflare-tunneled homeserver we already control, the marginal security benefit is low and the operational complexity (key sharing across daemon restarts) is real. Deferred until Tim asks.
- **Federation** — server-to-server bridging to other Matrix homeservers; not needed for a private fleet

---

## Self-review notes

- **Scope:** Phase 2 is theme + widget + 6 daemons + production lockdown. Tight focus. No scope creep into Phase 3.
- **No placeholders:** all commands are concrete. `<CHOSEN_DOMAIN>` is the only template variable; Tim's §9 answer makes it concrete before execution.
- **Type consistency:** matrix-daemon's `daemon.js` / `config.js` / `login.js` interfaces are unchanged from Phase 1; this plan just deploys them more places.
- **Risk:** the most likely failure is the Conduit registration token leaking + someone outside the fleet creating an account. Task 13 (rotate token) mitigates. `allow_registration = false` (Task 12) is the harder gate.

---

## Execution handoff

**Plan complete and saved to `docs/superpowers/plans/2026-05-27-fleet-messaging-phase2-plan.md`. Two execution options when Phase 1 is stable:**

1. **Subagent-Driven (recommended)** — fresh subagent per machine onboarding (Tasks 6-11)
2. **Inline Execution** — batch through with checkpoints between machines

Phase 2 is mostly mechanical replication of Task 6 across 6 machines. Each machine should take ~20 minutes once the recipe is exercised on 14900K. Total Phase 2 wall time: **~3-4 hours of focused work**, mostly on the per-machine boxes.
