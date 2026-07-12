# FLEET — X3Native operations

Single source of truth for **who is who**, **how the fleet talks**, and **how code lands on `main`**. The per-machine task files (`DJBOOTH.md`, `Snake13700k.md`, `i5000.md`, …) are task-of-the-moment notes; **this file is the standing canon.**

---

## Project naming canon

- **X3Native** — the engine (this repo). Custom C++20 / Vulkan 1.3, clean-room, proprietary.
- **Escape From Lab Zero (EFLZ)** — the **first game** built on X3Native. The Spire / Jake's cell / Martinez / spire bosses content in this repo IS EFLZ.
- **RiftForged** — a **separate, concurrent** game-dev project. `#riftforged_development` belongs to that project; **do not cross-post EFLZ status there.**

---

## Fleet machines + Slack identities

| Hardware | Fleet identity | Role | Primary lane |
|---|---|---|---|
| Dell i9 laptop (RTX A2000) | **OG Dell_I9** | OG dev + verification | gating, doc, fleet ops, lightweight fixes |
| i7-13700K workstation (2× 1080 Ti, 128GB, 4TB) | **13700K** — **IntegratorCaptainCommanderInspector** *(a.k.a. FarmBoss)* | **central authority** — owns `engine/` | integrates feature branches → `main`, re-gates every merge, captains the crew, commands lane assignments, inspects every push; renderer + netcode |
| i9-14900K desktop (RTX 5090) | **14900K** | gameplay/content powerhouse — owns `app/` | Level/World Editor |
| i7-13700k second machine | **Snake13700k** | open-world engineer | mountains, city/metropolis, ocean + submarine combat |
| i7-4790K garage (1080 Ti, Z97) | **DJBOOTH** | mid-biomes engineer | L12–L15 caves + toxic swamplands + Memory Hunter boss |
| (TBD machine) | **i5000** | desert levels engineer | L10–L11 Crystalline Desert + Salvari Camp |
| Predator chassis (4790K, dual GTX 1080 Ti SLI, 32GB DDR3, fresh Win11 25h2, VS2026, Vulkan) | **Predator-I4400** *(a.k.a. Predator_FNG — Fresh New Guy)* | **new fleet member** — onboarding | TBD lane; reported ~**680 FPS** on a known-good X3Native build (the high-water bench used to anchor the perf-disparity investigation vs the 13700K's ~40-50 FPS on `integration/culling-glass`) |

Workers push **feature branches**; primaries (OG Dell_I9 / 14900K / 13700K) push **`main`** directly (fetch → rebase → push, small + often). See `docs/VERSIONING.md` for the build-identity standard every push should carry.

---

## Slack — coordination chat

**Workspace:** `GameDev` (`gamedev-otl8345.slack.com`, team `T0B6UU9U540`). One auth covers every channel — no per-channel re-login.

**Channels (live or proposed):**

| Channel | ID | Purpose |
|---|---|---|
| `#new-channel` (rename suggested) | `C0B648DJ43E` | generic / TBD |
| `#riftforged_development` | `C0B5V7W87LK` | the *other* project — NOT EFLZ |
| `#eflz-development` *(create)* | TBD | EFLZ / X3Native game work, status, integrations |
| `#x3native-engine` *(optional)* | TBD | engine-only work (renderer, RHI, physics, tooling) |
| `#fleet-ops` *(optional)* | TBD | machine status, online/offline, build identity announcements |

Every machine's Claude session should sign posts with its fleet identity (e.g. `— OG Dell_I9 :robot_face:`). Quote the build identity (`docs/VERSIONING.md`) when announcing pushes/builds: `OG Dell_I9 pushed vX.Y.Z+<hash>`.

---

## Slack onboarding — per machine (do this once, ~3 min)

To get a fleet machine's Claude session onto Slack:

1. In that machine's Claude Code session: `/plugin install slack` → `/reload-plugins`. Confirms with: `Installed slack. Run /reload-plugins to apply.`
2. Tell that Claude: *"Authenticate the Slack MCP."* It will call `mcp__plugin_slack_slack__authenticate` and return a `slack.com/oauth/v2_user/authorize?...` URL.
3. On that machine, **open the URL in a browser** → it'll show the Claude app + `GameDev` workspace → click **Allow**. The browser will redirect to `http://localhost:3118/callback?...` and show *"This site can't be reached" — that's EXPECTED.*
4. **Copy that callback URL as text** (Ctrl+A in the address bar, then Ctrl+C — NOT a screenshot; OAuth codes are single-use, one wrong character means redo).
5. Paste it back to that machine's Claude. It'll call `mcp__plugin_slack_slack__complete_authentication` with the URL. Done — Slack tools come online.
6. Tell that session its fleet name (e.g. *"You are 13700K"*) and have it post a one-line `online` message to a channel.

**Gotchas (lessons learned 2026-05-25/26):**
- **Don't pause between steps 2 and 5.** The OAuth flow times out across many turns; if you wait several minutes, the callback fails with *"No OAuth flow is in progress."* Just call `authenticate` again for a fresh URL and continue.
- The `localhost:3118/callback` "can't be reached" page is *normal*; the URL still has the code you need.
- Copy callback URLs as **text**, not screenshots — Claude can't reliably OCR a long single-use token off an image.
- One Slack auth = the entire workspace. No per-channel re-auth.

---

## Alternative path: dedicated Slack bot per machine

The onboarding playbook above uses the official `slack` Claude plugin, which authenticates **as Tim's Slack user via OAuth**. Every posting session shows up as `Claude APP` and is distinguished only by the **signature** in the message body. That works (OG Dell_I9 and i5000 are on this path), but it gets visually muddy once 4+ machines are posting.

**DJBOOTH** pioneered the alternative: a **dedicated Slack bot per machine**, each with its own app/identity/avatar (`DJBooth APP`, `I5000-Bot`, `13700K-Bot`, etc.). Posts then read as that specific machine in the channel — zero ambiguity, no signature-soup.

| | **MCP plugin (OAuth-as-user)** | **Custom Slack bot per machine** |
|---|---|---|
| Setup per machine | ~3 min (`/plugin install slack` + OAuth) | ~10–15 min (create Slack app + scopes + bot token + connect via custom MCP) |
| Identity in Slack | every session = `Claude APP` (distinguish by signature) | each machine = its own bot user with own name + avatar |
| Reads channels & DMs | yes (workspace-wide) | yes (channels/DMs the bot is invited to) |
| Posts on demand | yes | yes |
| Autonomy by default | **no** — session only reads when you prompt it in CC | **no** — bot only reads when its CC session is prompted (DJBOOTH note: *"I do not run as a daemon"*) |
| Path to always-on | add `/loop 5m` (or `/schedule` for cross-session) | same — add `/loop 5m` (or `/schedule`) in the bot's CC session |
| Channel readability at scale | gets muddy with 4+ machines | clean, scales well |

**Recommendation:** **MCP plugin for fast bootstrap, custom bot for the long-term cleaner channel.** A machine can start on the plugin and migrate to its own bot later without losing identity (sign-format stays the same). The integrator (13700K) and high-traffic content lanes (i5000 desert, DJBOOTH caves) are the highest-value bot migrations.

**Critical: autonomy is a separate concern from which path you pick.** Both plugins and bots are on-demand by default. To make a session *answer DMs without you having to prompt it at the terminal*, you still need `/loop` or `/schedule` — see next section. DJBOOTH today is a custom bot **without** an autonomy loop, so Tim's DMs sit unread until he next prompts `check Slack` in CC on the 4790K. Adding `/loop 5m` (or `/schedule`) closes that gap.

---

## Stay live — `/loop` the Slack check

Auth alone makes a session *able* to read/post Slack; it doesn't make it *responsive*. To get a fleet-feel ("brothers banter in the channel, the integrator pings, someone replies"), every onboarded session should run a recurring Slack check via `/loop`. Without this, a session only reads Slack when its human types — i.e., never autonomously.

**After step 6, in that machine's Claude Code session, run:**

```
/loop 5m Check Slack channels #x3native_escapelabzero_features (C0B6VTEU8Q0) and #new-channel (C0B648DJ43E) for new messages addressed to <YOUR_FLEET_IDENTITY> since the last loop iteration. Use slack_search_public_and_private with sort=timestamp to find recent posts. Respond via slack_send_message to anything that mentions your identity, your lane (e.g. caves for DJBOOTH, desert for i5000, engine/integration for 13700K, perf-bench on the A2000 for OG Dell_I9), or asks a direct question. Sign every reply "— <YOUR_FLEET_IDENTITY> <your_emoji>" (e.g. ":cactus:" for i5000, ":robot_face:" for OG). If nothing new is addressed to you, output ONE short line like "loop tick: nothing new for <ID>" and end the iteration — DO NOT post unless there's something genuinely worth saying. Never spam the channel. Stop the loop if Tim says "stop loop" or similar.
```

Replace `<YOUR_FLEET_IDENTITY>` and `<your_emoji>` per the roster table above. Pick an emoji per machine: OG Dell_I9 :robot_face: · 13700K :captain: · 14900K :rocket: · Snake13700k :snake: · DJBOOTH :musical_note: · i5000 :cactus: · Predator-I4400 :crossed_swords: *(or whatever you prefer — make them distinct so messages are scannable).*

**How it works** *(the `/loop` skill handles the scheduling internally):*
- 5 m → cron `2-59/5 * * * *` (fires at :02, :07, :12, …, :57 — off-aligned from :00/:30 so the fleet doesn't all hit the API at once).
- **Session-only by default** — the loop dies when that Claude Code session closes. To survive across sessions, use `/schedule` instead (cloud cron).
- Recurring tasks **auto-expire after 7 days**; restart the loop on day 7.
- `CronDelete <job_id>` to cancel sooner (the job ID is printed when `/loop` schedules it).
- The first iteration runs immediately on `/loop`; subsequent iterations follow the cron.

**Don't:**
- Don't `/loop` shorter than 3 m unless there's a specific reason (Slack API rate-limit headroom + token cost).
- Don't have the loop post on every iteration "checking in!" — Tim will mute you. Only post when there's actual content.
- Don't run multiple `/loop` instances for the same task in one session.

**Why pre-aligned offsets matter (fleet sync):** if every machine runs `/loop 5m` and uses the default `*/5`, they all fire at :00 :05 :10 simultaneously — five sessions hitting Slack at the same instant. Use `2-59/5`, `3-59/5`, `4-59/5`, etc. for different machines (the `/loop` skill picks one per session).

---

## Push protocol — short form

(Full version: `docs/VERSIONING.md` §Fleet push model.)

- **Primary rigs (OG Dell_I9 / 14900K / 13700K) → push `main` directly.** Always `git fetch && git rebase origin/main` first.
- **Worker machines (DJBOOTH / Snake / i5000) → push feature branches.** A primary merges + re-gates.
- **`VERSION` bumped by whoever merges to `main`**, with a `CHANGELOG.md` line.
- **Large promotions** (e.g. `feat/doors-death-anim` → `main`) **route through the 13700K integrator** unless it's offline.
- **Backup tags before risky merges** (the May-24 `backup/visual-pass-good`, `backup/pre-rt-merge-0524`, etc. saved us once — keep that habit).

---

## Recovering "lost" work — forensic playbook

If a session feels like work disappeared:

1. `git reflog --date=iso | head -80` — every state HEAD has been; pre-reset tips are here.
2. `git fsck --lost-found --no-reflogs` — dangling commit objects.
3. `git branch -a -vv` and `git tag -l 'backup/*'` — look for un-merged branches and pre-merge safety tags.
4. `git log --all --oneline -- <path>` — full history of a specific file across all branches.

Committed work is recoverable for ~90 days via reflog; nothing is truly gone unless `gc` was forced.

---

## Git LFS — self-hosted, on the 14900K (2026-07-11)

**GitHub's LFS budget is exhausted** (`batch response: This repository exceeded its LFS budget`). Asset fetches hard-failed fleet-wide, which blocked finished work from landing. GitHub's metered pricing ($0.0875/GB egress, $0.25/GB/mo storage) is a bad deal when we have a 2.5 Gbps LAN.

**We now host the LFS blobs ourselves. GitHub is still the git host** — `origin`, PRs, branches, and the whole fleet workflow are *unchanged*. **Only the blob transfer moved.** The switch is one committed file, `.lfsconfig`:

```ini
[lfs]
	url = http://192.168.7.23:3000/fleet/X3Native.git/info/lfs
```

### The endpoint

| | |
|---|---|
| **Server** | Gitea 1.24.3, `D:\LFS\bin\gitea.exe` |
| **Host** | `I9DevPC` (i9-14900K) — `192.168.7.23:3000` |
| **LFS endpoint** | `http://192.168.7.23:3000/fleet/X3Native.git/info/lfs` |
| **Web UI** | `http://192.168.7.23:3000/` |
| **Auth** | Gitea user `fleet` + access token (HTTP Basic). Not open to anonymous. |
| **Blob store** | `D:\LFS\blobs\x3native` — plain, unencrypted, sharded `<2hex>/<2hex>/<sha256>` |
| **Windows service** | `gitea` — **auto-start**, auto-restart on crash (5s / 15s / 60s) |
| **Firewall** | inbound TCP 3000, **LocalSubnet only** (Private + Domain profiles) |
| **Config** | `D:\LFS\custom\conf\app.ini` |

**Why Gitea and not `rudolfs` / `lfs-test-server` / `giftless`:** rudolfs is the leanest (Rust, local-disk) but ships **no authentication at all** — it expects a reverse proxy, which is a second moving part to own. `lfs-test-server` is self-described as not-for-production. `giftless` is Python and this box runs 3.14, where the dependency wheels are a coin-flip. Gitea is a **single Windows binary** with a real LFS server, real auth, native Windows-service support, a **plain-bytes local store** (so the backup below is directly usable without the server), and — the tiebreaker — it also keeps a **full git mirror** of the repo. Given the whole crisis is "our assets are stranded on a host we can't read from", a local copy of *both* the git history and the blobs is exactly the insurance we want.

**It also mirrors git.** All 240 refs are pushed to `http://192.168.7.23:3000/fleet/X3Native.git`. That is a read-only safety net, *not* a second place to push work — **keep pushing code to GitHub `origin`.**

### Per-machine one-time setup

Every fleet box (i4400 / DJBOOTH / Dell i9 / p13700 / Snake13700k) does this **once**. Nothing else changes — `git pull`, `git push`, `git lfs fetch` all behave normally afterward.

```bash
# 1. Cache the LFS credential for the endpoint (one line, once per machine).
git config --global credential.'http://192.168.7.23:3000'.helper manager
printf 'protocol=http\nhost=192.168.7.23:3000\nusername=fleet\npassword=<TOKEN>\n' | git credential approve

# 2. Verify.
cd <your X3Native clone>
git lfs env | grep Endpoint      # -> http://192.168.7.23:3000/fleet/X3Native.git/info/lfs
git lfs fetch --all && git lfs checkout
```

`<TOKEN>` is the Gitea access token for user `fleet` — it is **not** in the repo. Get it from Tim, or mint a fresh one per machine at `http://192.168.7.23:3000/user/settings/applications` (scope: `write:repository`). Per-machine tokens are preferred: they can be revoked individually.

`.lfsconfig` is committed, so the endpoint applies automatically to **every clone on every branch that contains it** — no per-repo config needed beyond the credential.

**Adding a new machine:** mint it a token in the Gitea UI, run the two commands above. That's the whole onboarding.

### Where the blobs live, and the backup

Blobs are at `D:\LFS\blobs\x3native` on the 14900K. **Every file's name IS its sha256, which IS its Git LFS oid** — so the store is self-describing and can be rebuilt or raided by hand with nothing but a hash.

That box has confirmed Raptor Lake degradation and an open Intel RMA, so the store is mirrored off it:

- **Task:** `X3Native-LFS-Backup` (Task Scheduler) → `D:\LFS\backup_lfs.ps1`
- **Runs:** daily 03:30 **and** at every startup
- **Destination:** `\\p13700\G\X3NativeLFS\blobs` (+ Gitea's `gitea.db` and `app.ini` under `…\meta`)
- **Additive only** — deliberately **not** `robocopy /MIR`. LFS objects are immutable; a local deletion must never propagate to the backup.
- Log: `D:\LFS\log\backup.log`

If the 14900K dies: the blobs are intact on D: (a crash is minutes of downtime, not data loss — the service auto-restarts), and a full copy is on G:. To rehost, install Gitea anywhere, point `[lfs] PATH` at a copy of the store, and change the one line in `.lfsconfig`.

### Rollback

Delete `.lfsconfig` (or `git config lfs.url https://github.com/1GreenNinja/X3Native.git/info/lfs`) and LFS points back at GitHub. Nothing else in the repo depends on this. **No history was rewritten and no LFS object was deleted** to set this up.

### Optional upgrade — offsite durability via Cloudflare R2

Gitea's LFS backend speaks S3, so this can gain offsite durability **without changing the fleet workflow at all** (the endpoint stays the same; only the server's storage config changes):

```ini
[lfs]
STORAGE_TYPE = minio
MINIO_ENDPOINT   = <account>.r2.cloudflarestorage.com
MINIO_BUCKET     = x3native-lfs
MINIO_USE_SSL    = true
```

R2 charges **$0.015/GB/mo and zero egress** → ~274 GB ≈ **$4/mo, no bandwidth bill ever**. Compare GitHub's metered rate on the same data. Migration is `rclone copy` of the blob store into the bucket, then a service restart.

---

## When this doc moves

Edits are welcome from any fleet machine. **Workers**: push as a branch (`docs/fleet-<topic>`) and ping in Slack for a primary to merge. **Primaries**: edit + push directly. Keep this doc tight — it's the canon, not a journal.
