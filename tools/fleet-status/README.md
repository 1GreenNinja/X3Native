# fleet-status — periodic JSON snapshot for the Element sidebar widget

13700K runs `generate.ps1` every 60 seconds. It writes `fleet-status.json` to `C:\opt\element-web\` (Element Web's static webroot, behind the same Cloudflare Tunnel). The Phase 2 Element widget polls that URL and renders the machine grid.

## Files

| File | Purpose |
|---|---|
| `schema.json` | JSON Schema describing the fleet-status document shape |
| `generate.ps1` | The generator script that runs on 13700K every minute |
| `README.md` | This file |

## Phase

This is **Phase 2** work per `docs/superpowers/specs/2026-05-27-fleet-messaging-design.md` §4.6. The data pipeline can be stood up before the widget; Tim can verify the JSON looks right before any UI work.

## What the widget shows

Per-machine grid in Element's right-side panel:

```
┌──────────────────────────────────────────────────────────────────┐
│  Fleet status — updated 14s ago                                  │
├──────────────────────────────────────────────────────────────────┤
│  🟢 DJBOOTH    feat/portal-hub @ 3729d29 — 6h ago                │
│      worker · 192.168.7.110 · 24/7 garage 4790K                  │
├──────────────────────────────────────────────────────────────────┤
│  🟢 13700K     main @ 0b6ab79 — integrator                       │
│      integrator · 192.168.7.x                                    │
├──────────────────────────────────────────────────────────────────┤
│  🟡 14900K     feat/multi-font-roles @ 8d74d74 — 49 behind main  │
│      showcase · 192.168.7.x · ⚠ rebase needed                    │
├──────────────────────────────────────────────────────────────────┤
│  ⚫ I5000      (no recent branch — dormant)                      │
│      worker · 192.168.7.x · last seen 4d ago                     │
├──────────────────────────────────────────────────────────────────┤
│  🟢 I4400      new install — no branches yet                     │
│      worker · 192.168.7.x · dual 1080Ti glass desk               │
└──────────────────────────────────────────────────────────────────┘

  Integration queue (1):
  ▸ feat/portal-hub (DJBOOTH) — Ready, 24/24 self-test pass
```

## How presence is determined (Phase 1 — ping only)

```powershell
Test-Connection -ComputerName $lanIp -Count 1 -Quiet -TimeoutSeconds 1
```

Boolean → online / dormant. Phase 2 layers in `m.presence` events from Conduit so we can detect "matrix-daemon alive even though the machine is busy" (CPU-bound work that's still pingable) and "matrix-daemon dead even though pingable" (e.g. daemon crashed but OS up).

## How branches are matched to machines (Phase 1 heuristic)

Walks every `refs/remotes/origin/feat/*` and tags branches that touch the machine's `<MACHINE>.md` file in any of their commits. Catches the convention where each machine appends to its own per-machine markdown file.

Falls back to: explicit naming patterns (`feat/<machine>-*` or `feat/*-<machine>`).

Phase 2 could use git notes or a curated map if the heuristic produces too many false positives.

## Schema overview

Single JSON document; top-level keys:

| Key | Type | Notes |
|---|---|---|
| `generated_at` | ISO-8601 | When this snapshot was made |
| `generator_machine` | string | "13700k" usually |
| `machines[]` | array | One entry per fleet PC |
| `integration_queue[]` | array | Branches awaiting merge |
| `warnings[]` | array | Stale-branch / dormant-machine alerts for the widget header |

Per-machine sub-schema:

| Key | Type | Notes |
|---|---|---|
| `name` | string | lowercase slug ("djbooth", "13700k") |
| `display_name` | string | pretty form ("DJBOOTH") |
| `lan_ip` | string | from `~/.claude/.fleet_hosts.json` |
| `presence` | enum | "online" / "away" / "dormant" / "unknown" |
| `presence_source` | enum | "ping+matrix" / "matrix-only" / "ping-only" / "stale" |
| `last_seen_at` | ISO-8601? | most recent matrix presence (Phase 2) |
| `branches[]` | array | active branches for this machine |
| `role` | enum | "worker" / "integrator" / "primary" / "showcase" / "fallback" |
| `notes` | string? | integrator can pin free-text |

Per-branch sub-schema:

| Key | Type | Notes |
|---|---|---|
| `name` | string | "feat/portal-hub" etc. |
| `head_sha` | string | short or full SHA |
| `head_subject` | string | commit message first line |
| `head_committed_at` | ISO-8601 | when HEAD was created |
| `ahead_of_main` | int | commits not on main yet |
| `behind_main` | int | commits the branch is missing from main |
| `ready_for_integration` | bool | text-match "READY FOR INTEGRATION" in the STATUS block |

## Schedule

```powershell
$gen = "$env:USERPROFILE\.claude\fleet-status\generate.ps1"  # adjust path on 13700K
$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
  -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$gen`""
$trigger = New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(1) `
  -RepetitionInterval (New-TimeSpan -Minutes 1)
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable `
  -ExecutionTimeLimit (New-TimeSpan -Minutes 1) -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive
Register-ScheduledTask -TaskName 'FleetStatus-Generator' `
  -Action $action -Trigger $trigger -Settings $settings -Principal $principal `
  -Description 'Regenerates fleet-status.json every minute for the Element sidebar widget.'
Start-ScheduledTask -TaskName 'FleetStatus-Generator'
```

## What's NOT in Phase 1

- The actual Element widget (UI): comes in Phase 2 after the JSON shape is validated
- WebSocket push instead of polling: nice-to-have if 60s feels laggy; can use Server-Sent Events from a thin Node proxy
- Per-user view filtering: future. The current widget shows the same data to everyone
- Build CI status (gating green/red badges): wait until Phase 2 hooks the X3Native test gauntlet into the JSON
