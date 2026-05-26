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
| i7-13700K workstation (2× 1080 Ti, 128GB, 4TB) | **13700K** *(a.k.a. FarmBoss)* | **integrator on point** — owns `engine/` | merges feature branches → `main`, re-gates; renderer + netcode |
| i9-14900K desktop (RTX 5090) | **14900K** | gameplay/content powerhouse — owns `app/` | Level/World Editor |
| i7-13700k second machine | **Snake13700k** | open-world engineer | mountains, city/metropolis, ocean + submarine combat |
| i7-4790K garage (1080 Ti, Z97) | **DJBOOTH** | mid-biomes engineer | L12–L15 caves + toxic swamplands + Memory Hunter boss |
| (TBD machine) | **i5000** | desert levels engineer | L10–L11 Crystalline Desert + Salvari Camp |

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

## When this doc moves

Edits are welcome from any fleet machine. **Workers**: push as a branch (`docs/fleet-<topic>`) and ping in Slack for a primary to merge. **Primaries**: edit + push directly. Keep this doc tight — it's the canon, not a journal.
