# State-of-the-Fleet — design spec

| Field | Value |
|---|---|
| **Date** | 2026-06-09 |
| **Status** | DRAFT — awaiting Tim review |
| **Author** | Integrator (13700K Claude session) |
| **Brainstorm cache** | `~/.claude/projects/G--/memory/brainstorm-state-of-fleet-progress.md` |
| **Implementation plan** | TBD — written via `superpowers:writing-plans` after this spec is approved |

---

## 1. Problem

Tim, 2026-06-09: *"Has ANYONE checked ALL the branches that we have now.. and collated their features into a document, and matched it with the original plans, and written a good review of it? Where are we at on the 100 slices!"*

Honest answer was no. The pieces that existed were stale + disconnected:

- `docs/BRANCH_FEATURE_REPORT.md` was a one-shot manual report from 2026-06-05, **missing 16+ branches** that had landed since (`feat/doors-death-anim`, `feat/editor-phase3b/4/5`, `feat/m7-postfx-advanced`, `feat/m8-imgui-devtools`, `feat/m10-ship`, `feat/spire-art`, `feat/weapon-grip-tune`, `feat/space-engine-spec`, all 5 `i5000-fleet-specs`/`note-to-*` docs branches, `feat/fleet-hw-format`, `feat/wave2-content`).
- `X3_NATIVE_SLICES.md` (the 100-slice engine plan at repo root) has **no `✅/🚧` status markers** despite defining the legend on line 18. The 2026-06-05 plan-auditor verdict flagged Slices 23 / 35 / 65 / 67 / 9-15 as already DONE in shipped code, but the slice doc itself doesn't reflect that.
- **No cross-reference** anywhere matches branches ↔ slices ↔ EFLZ master task list. DJBOOTH and StarForge were doing this manually in Fleet Ops chat ("claim untouched slices so nobody steps on each other") at the moment this spec was drafted.
- **No catalog of 14900K's in-flight renderer work.** Tim said he was "rocking out with rendering changes and upgrades" before going MIA; that work isn't yet visible on `origin`. A pure git observer would miss it. An active poll of the fleet would surface it.
- The fleet has **no fresh source of truth** to know which slices are still pending so the next bot to pick up work can claim cleanly.

## 2. Goals

1. Produce **one canonical document** that answers "where are we?" across (a) the 100 engine slices, (b) every `origin/feat/*` branch, (c) what each fleet member is actively working on.
2. **Auto-refresh** every 6 hours so it never goes stale by more than one nap.
3. **Cross-reference** branches → slices → EFLZ master tasks, so picking up work doesn't require manual cross-referencing in head.
4. **Machine-readable canonical data** (JSON) so future Claude sessions / agents / scripts can parse without re-deriving.
5. **Human-readable views** (Markdown + HTML dashboard) for terminal-grepping and browser-eyeballing.
6. **Surface the bus number** — when a fleet member is MIA, their last self-reported focus is preserved in the JSON so we know what they were on.
7. **Survive failure visibly** — the doc itself must be the signal when its own daemon dies (the failure mode that bit Integrator for 36h on 2026-06-08).

## 3. Non-goals

- **NOT** a project management system — no priorities, due dates, or ticket lifecycle. Just observation + cross-reference.
- **NOT** a code search index — file-symbol grep is still ad-hoc; this doc only records slice-status evidence, not arbitrary engine introspection.
- **NOT** a build / test gate runner — this doc reports branch metadata, not build pass/fail. Integration is still a manual step by the Integrator.
- **NOT** a federation-aware Matrix client — we use the existing fleet daemon's pipe; we don't open a second sync connection or talk to other homeservers.
- **NOT** mutation-testable or perf-tested — see Section 7 testing rationale.

## 4. Locked decisions (brainstorm 2026-06-09)

| Question | Tim's answer | Rationale |
|---|---|---|
| Deliverable | **Both** — snapshot AND tool | One-shot would stale by tomorrow; tool alone leaves today's snapshot un-shipped |
| Primary axis | **Both** slices-first AND branches-first as co-equal lenses | Different consumers need different entry points |
| Slice status method | **Hybrid** heuristic + plan-auditor cross-check; 5th state `"conflict"` | Pure heuristic mis-calls vague slices; pure plan-auditor is too slow for 6h cadence |
| Output format | **Markdown + HTML dashboard via JSON fetch** (option C) | Tiny git diffs every 6h; same-origin auth means no CORS |
| Cadence | Every **6h** via Scheduled Task `StateOfFleet-Sync` | Matches sleep/wake cycle; 4 refreshes/day is enough freshness |
| Live polling | **Active poll** — broadcast to Fleet Ops, 60s harvest window | Captures in-flight work that isn't pushed (the 14900K MIA case) |
| Fable routing | 14900K primary, Predator + Integrator secondary, skip oglaptop | Locked separately; recorded here for context |

## 5. Architecture

Three artifacts, three audiences, three update cadences:

```
   ┌────────────────────────────────────────────────────────────────┐
   │  tools/fleet/state_of_fleet.py   (every 6h via Task + on-demand)│
   └──────────┬───────────────────────────────┬────────────────────┘
              │ (every 6h)                    │ (rare — only on layout change)
              ▼                               │
   ┌──────────────────────────────┐           │
   │ docs/fleet/state_of_fleet.json│           │
   │ (canonical, ~30 KB)          │           │
   └──────────┬───────────────────┘           │
              │                               ▼
              │             ┌──────────────────────────────────────┐
              │             │ docs/fleet/state_of_fleet.html       │
              │             │ (static dashboard, hand-written;     │
              │             │  fetches JSON on load)               │
              │             └──────────┬───────────────────────────┘
              │                        │
              │                        │ both served by DocsReader-Static :7777
              │                        │ behind Cloudflare Access (one OTP cookie)
              ▼                        ▼
      docs.slopclaude.com/fleet/state_of_fleet.json
      docs.slopclaude.com/fleet/state_of_fleet.html

      ALSO written for git/terminal readers + bot grep:
        docs/fleet/STATE_OF_FLEET.md  (rendered from JSON; updated every 6h)

      ALSO auto-bundled into the EFLZ reader at docs.slopclaude.com/_reader.html
      via the existing tools/eflz/build_reader.py auto-discovery
      (groups under "Fleet / State" by parent-dir convention).
```

### Three audiences

| Audience | Consumes | Why |
|---|---|---|
| **Future bots / scripts / Claude sessions** | `state_of_fleet.json` | Machine-readable, stable schema, fast parse |
| **Tim (browser)** | `state_of_fleet.html` | Visual dashboard, click-through drill-down, mobile-readable |
| **Terminal / git readers / grep / Integrator** | `STATE_OF_FLEET.md` | Plain text, diff-friendly in PRs, greppable for "what slice is X in" |

### Why JSON-fetch architecture, not embedded HTML

- **Tiny git diffs** — 6h-cadence commits change only the small JSON, not a multi-MB HTML rebuild.
- **Layout vs data decoupling** — UI changes don't require regenerating data; data changes don't require regenerating layout.
- **Same-origin auth** — both `.json` and `.html` served from `docs.slopclaude.com`. The Cloudflare Access cookie set by Tim's OTP login covers the JSON fetch automatically (no CORS).
- **Service-token auth path** — the Playwright live smoke (Section 7) uses a Cloudflare Access Service Token for headless auth, never burns an OTP.

## 6. Components

### 6.1 Internal units (single-file `tools/fleet/state_of_fleet.py`)

Six logical units. Each independently testable.

| Unit | Function | Inputs | Output |
|---|---|---|---|
| **`git_observer()`** | Walks `origin/feat/*` branches, gets per-branch metadata | git ls-remote / git log / git diff-stat / git merge-base / git for-each-ref | `list[Branch]` — name / tip / age_seconds / author / commit_subject / diff_stat / behind_cull / ahead_cull / commit_count / last_pushed_ts |
| **`slice_parser()`** | Reads `X3_NATIVE_SLICES.md`, extracts the 100 slice rows | `X3_NATIVE_SLICES.md` | `list[Slice]` — n / heading / description / telltales[] / milestone / is_obsolete |
| **`slice_status_detector(slices)`** | For each slice, heuristic-scan + plan-auditor cross-check | `engine/`, `app/`, `docs/plan-reviews/X3_NATIVE_SLICES-*.md` (latest verdict) | `list[Slice]` with `status` ∈ `{done, in_progress, pending, obsolete, conflict}` + `evidence[]` + `last_audit_verdict` |
| **`fleet_poller()`** | Broadcasts to Fleet Ops, harvests 60s of replies | Matrix `/sync` API + my access token + daemon outbox pipe | `dict[sender_uid, FleetFocus]` — text / matrix_event_id / ts; `null` for non-responders |
| **`cross_referencer(branches, slices, polls)`** | Maps branches → slice(s), branches → EFLZ master-task entries; flags orphans + stales | `docs/design/EFLZ_MASTER_TASK_LIST.md` | enriched branches (`advances_slices[]`, `advances_eflz_tasks[]`, `integration_state`); `orphan_branches[]`; `stale_branches[]` (no commits in 14d) |
| **`renderer(state)`** | Writes all 3 artifacts atomically | the assembled `State` dict | filenames written; SHA256 of new JSON |
| **`committer()`** | Git diff check + auto-commit if non-trivial | the 3 written files vs working tree | exit code (0 = committed, 1 = nothing changed, 2 = push failed, 3 = lock contention) |

### 6.2 JSON schema (the contract every consumer reads)

```json
{
  "$schema": "https://json-schema.org/draft-07/schema",
  "$id": "https://docs.slopclaude.com/fleet/state_of_fleet.schema.json",
  "version": 1,

  "generated_at": "2026-06-09T14:30:00Z",
  "generator": {
    "tool": "tools/fleet/state_of_fleet.py",
    "git_head_at_run": "9befaaa1",
    "run_duration_seconds": 64.2,
    "poll_failed": false
  },

  "repo_state": {
    "cull_combined_tip": "b9dbb18a",
    "cull_combined_ahead_of_main": 122,
    "main_tip": "0c0a440f"
  },

  "fleet_focus": {
    "@13700k:fleetcommand.slopclaude.com": {
      "display_name": "Integrator",
      "text": "state-of-fleet design brainstorm with Tim",
      "matrix_event_id": "$HD3Jv9Ws...",
      "ts": 1780767800,
      "overflow_event_id": null
    },
    "@i5000:fleetcommand.slopclaude.com": null
  },

  "branches": [
    {
      "name": "feat/portal-hub",
      "tip": "d8298f01",
      "age_seconds": 86400,
      "author_email": "djbooth@fleetcommand",
      "subject": "rifthub: blue energy core + wormhole-generator housing",
      "diff_stat": {"insertions": 2609, "deletions": 12, "files": 18},
      "behind_cull": 248,
      "ahead_cull": 10,
      "commit_count": 47,
      "last_pushed_ts": 1780681400,
      "advances_slices": [29, 78],
      "advances_eflz_tasks": ["T-13"],
      "integration_state": "rebasing"
    }
  ],

  "slices": [
    {
      "n": 23,
      "heading": "PBR metallic-roughness materials from glTF",
      "milestone": "M3",
      "status": "done",
      "evidence": [
        "shaders/mesh.frag:79-82",
        "engine/asset/ModelLoader.cpp:387-393",
        "PROVENANCE.md:52"
      ],
      "advanced_by_branches": [],
      "last_audit_verdict": {
        "verdict": "DONE",
        "verdict_source": "docs/plan-reviews/X3_NATIVE_SLICES-2026-06-05.md",
        "verdict_ts": 1780000000
      },
      "is_obsolete": false
    }
  ],

  "orphan_branches": [
    {
      "name": "feat/note-to-13700k",
      "age_days": 13,
      "reason": "no slice match + no EFLZ task match"
    }
  ],

  "stale_branches": [
    {
      "name": "feat/space-engine-spec",
      "age_days": 9,
      "last_commit_subject": "spec: space engine architecture v0.3"
    }
  ]
}
```

### 6.3 HTML dashboard structure

Static page that fetches `state_of_fleet.json` on load. Vanilla JS, no framework.

| Element | Behavior |
|---|---|
| **Sticky header bar** | `generated_at` timestamp · "Refresh" button (re-fetches JSON) · fleet-online dot count |
| **Stale banner** (conditional) | Shown when `generated_at` > 12h old. Yellow "⚠ Last refresh 14h ago — daemon may be down" — the signal that would have caught Integrator's 36h death |
| **Tab: Slices (100)** | 10×10 grid of `.slice-cell` divs. Color by status: green=done, amber=in_progress, grey=pending, red=obsolete, yellow=conflict. Click → drawer with evidence + branches advancing it |
| **Tab: Branches (~106)** | Swimlane sorted by lane (taxonomy inherited from `docs/BRANCH_FEATURE_REPORT.md`). Each row: author / age / slice-refs / integration state / latest commit subject. Sortable by age, alphabet, lane |
| **Tab: Fleet focus** | One card per bot. Display name / last self-reported focus / "X minutes ago". Greyed-out card for non-responders with their last room activity timestamp |
| **Tab: Orphans + Stale** | Sticky footer banner so orphans never get lost. Click → "claim this branch" hint |
| **Theme** | Forks the EFLZ reader palette (`tools/eflz/build_reader.py`) — same dark theme, same accent colors, same font stack — so visually consistent with the EFLZ reader |

## 7. Data flow

The script runtime is one synchronous pipeline. Only nontrivial async piece is the 60s Matrix poll.

### 7.1 End-to-end sequence (~65s total)

```
t=0     Script starts (Scheduled Task fires OR manual --poll/--dry-run)
        ├── Acquire lock at docs/fleet/.state_of_fleet.lock
        │   ├── No lock        → create, proceed
        │   ├── Lock < 5 min   → exit 0 ("another run in progress")
        │   ├── Lock > 5 min   → exit 3 ("stuck run, manual intervention")
        │   └── Owner dead     → reclaim, proceed
        ├── Verify daemon alive (named-pipe ping, 2s timeout)
        │   └── Dead           → exit 3 (fatal), Scheduled Task restart kicks in
        └── Resolve #fleet-ops room ID once, cache for the run

t=0     [parallel]
        ├── git_observer()                                            ──┐
        ├── slice_parser()                                              │  ~5s total
        └── slice_status_detector() (depends on parser)                 │

t=0     Post broadcast via daemon outbox pipe:                          │
        "[state-of-fleet] refreshing, drop your current focus (60s)"   │

t=0..60 GET /_matrix/client/v3/sync?since=<token>&timeout=60000         │
        Filter events:                                                  │
        - type == m.room.message                                        │
        - room == #fleet-ops                                            │
        - sender != @13700k (no self-poll contamination)                │
        - body NOT startswith "[state-of-fleet]" (defense-in-depth)     │
                                                                        │
t=60    Stop harvesting. Build fleet_focus dict.                      ──┘

t=60    cross_referencer() — joins branches + slices + polls (~2s, pure in-memory)

t=62    render_json()     → docs/fleet/state_of_fleet.json  (atomic tmp+rename)
        render_markdown() → docs/fleet/STATE_OF_FLEET.md     (atomic tmp+rename)
        render_html()     → docs/fleet/state_of_fleet.html   (only if layout changed)

t=64    committer():
        - git diff --cached --stat the 3 files
        - Compute diff significance:
          * any slice status flip → significant
          * new branch / lost branch → significant
          * > 5 lines net change → significant
          * else → no-op
        - If significant:
          * git add docs/fleet/state_of_fleet.{json,md,html}
          * git commit -m "state(fleet): auto-refresh 2026-06-09 14:30\n\n_observed_head: 9befaaa1\n_run_duration: 64s\n_diff_summary: ..."
          * git push origin feat/cull-combined (no --force; --force-with-lease only on retry)
        - Else: log "no-op" + release lock

t=65    Release lock. Exit 0.
```

### 7.2 Atomic-write guarantee

All three output files via temp + rename:

```python
def write_atomic(path, content):
    tmp = path + ".tmp"
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        f.write(content)
    os.replace(tmp, path)  # atomic on Windows + POSIX
```

So a kill-mid-write doesn't leave a half-written JSON that the dashboard would `fetch()` + crash on.

### 7.3 The 60s poll details

- **Single long-poll**, not N short polls — `GET /sync?timeout=60000` gets every message in the window in one response. Less server load, simpler harvest.
- **No re-broadcast at t=30s.** Fleet members get one notification per refresh, not two.
- **Self-broadcast filter** — `sender != @13700k` AND `body NOT startswith "[state-of-fleet]"`. Two layers because the second prevents a misconfigured daemon from impersonating me.
- **Empty-reply handling** — non-responders get `null` in JSON. Dashboard greys their card + shows their last room activity ts.
- **Oversized reply** (> 2 KB body) — truncated to 500 chars in JSON, full body referenced by `overflow_event_id` (a Matrix event ID the dashboard can deep-link to via Element).

## 8. Error handling

### 8.1 Hard fails — exit non-zero + log loudly + (optionally) DM Tim

| Failure | Detection | Recovery | Exit code |
|---|---|---|---|
| Daemon dead | Named-pipe rejects connection within 2s OR daemon log mtime > 2h old | Bail at t=0. Log line: `FATAL daemon down: <last log line>`. Scheduled Task restart kicks in. | 3 |
| Lock stuck (> 5min) | Lock file exists, owner alive, started > 5 min ago | Bail. Tim sees the stuck-run signal next time he looks at the dashboard (which will show stale). | 3 |
| Matrix homeserver unreachable | `/sync?timeout=0` non-2xx or connection error | Skip the poll, still generate report from local state + flag `poll_failed: true` in JSON so dashboard shows "fleet poll unavailable". Exit 0 (partial > nothing). | 0 |
| `X3_NATIVE_SLICES.md` malformed | Parser raises OR slice count < 90 | Hard fail. Don't overwrite previous JSON with a bad one. Last known good stays canonical. | 4 |
| `git push` rejected (non-FF) | Push exit non-zero | Try `git fetch + rebase + push` ONCE. If still rejected, bail with diff staged + uncommitted. Never `--force`. | 5 |

### 8.2 Soft handles — log, continue, surface in the report

- **Slice status heuristic ≠ plan-auditor verdict** → `status: "conflict"` (5th state). Both verdicts recorded in JSON. Dashboard renders yellow with tooltip showing both. Human resolves on review.
- **Branch with no slice + no EFLZ task match** → `orphan_branches[]`. Dashboard footer banner.
- **Oversized poll reply** → truncate to 500 chars, stash full body via `overflow_event_id`.
- **Branch tip moved during the run** → `git_observer` records the tip it saw; commit message includes `_observed_head` so audit can detect race.
- **Self-broadcast contamination** → already filtered; defense-in-depth via body prefix check.
- **Stale-banner threshold** (> 12h since `generated_at`) — dashboard self-warns. Same signal that would have caught the 2026-06-08 36h daemon death.

### 8.3 Concurrency safety

Single lock file at `docs/fleet/.state_of_fleet.lock` holding `{"pid": int, "started_at": iso8601_str, "host": "13700k"}`.

```python
def acquire_lock(lock_path, stale_after_sec=300):
    if not lock_path.exists():
        write_atomic(lock_path, json.dumps({...}))
        return True
    state = json.loads(lock_path.read_text())
    age = time.time() - parse_iso(state['started_at']).timestamp()
    if age > stale_after_sec:
        log.warn(f"reclaiming stuck lock (pid={state['pid']}, age={age}s)")
        write_atomic(lock_path, json.dumps({...}))
        return True
    if not pid_alive(state['pid']):
        log.warn(f"reclaiming lock from dead pid={state['pid']}")
        write_atomic(lock_path, json.dumps({...}))
        return True
    return False  # young lock, owner alive → another run in progress
```

Survives Scheduled Task firing while a manual `--poll` run is mid-flight.

### 8.4 Dashboard self-monitoring

The HTML dashboard's `init()` does:

1. `fetch('./state_of_fleet.json')` → 200? parse. 404 → "first run hasn't completed yet, try again in 60s" banner.
2. Parsed JSON's `generated_at` > 12h ago → yellow `⚠ Last refresh 14h ago — daemon may be down` banner above the data.
3. Parsed JSON's `generator.poll_failed: true` → orange "Fleet poll skipped this cycle" banner (partial data).

These three banners cover the failure modes Tim would otherwise only catch by asking "why does this look weird?".

## 9. Testing

Two-tier strategy: unit tests for the pure pieces, mocked externals, smoke gate before install, plus a live nightly browser smoke for the catch-Cloudflare-Access-regression case.

### 9.1 Unit tests (`tools/fleet/tests/test_state_of_fleet.py`, pytest)

| Unit | Tests | Fixtures |
|---|---|---|
| `slice_parser()` | 100-row count; legend extraction; OBSOLETE-section detection; mid-doc heading recovery; UTF-8 emoji preservation; malformed table → raise | 3 synthetic SLICES.md fixtures: golden / malformed / mid-edit |
| `slice_status_detector()` | All-telltales-hit → `done`; some-hit → `in_progress`; none-hit → `pending`; heuristic ≠ plan-auditor → `conflict`; OBSOLETE bypass | Mock `engine/` + `app/` tree; mock plan-auditor verdict file |
| `cross_referencer()` | Branch → 1 slice / N slices / 0 slices (orphan); EFLZ-task mapping; stale threshold (> 14d) | Static `branches=[...]`, `slices=[...]`, `polls={}` inputs |
| `renderer()` | JSON validates against v1 schema; MD renders deterministically (no clock drift across runs with same input); HTML embeds correct `<script src="state_of_fleet.json">` reference; atomic-write survives kill-mid-write | Full assembled `State` dict |
| `committer()` | "Nothing changed" → no commit; "5+ field diff" → commit with `_observed_head`; push-rejected → fetch-rebase-retry; concurrent-lock detection | Temp git repo as fixture |

### 9.2 Mocked external interfaces

- **Matrix API** — `unittest.mock` patches `urlopen` to return canned `/sync` responses (golden 60s window with 5 replies / empty window / truncated-mid-poll error).
- **Daemon pipe** — patched to return `OK` / `DEAD` for the `is_alive` probe.
- **Git** — `subprocess.run` patched at the test seam; helper `fake_git(stdout, returncode)` injects canned `git log` / `git ls-remote` / `git push` responses.

### 9.3 Integration smoke test (`test_smoke_end_to_end`, runs in CI)

1. Builds a temp git repo with 3 synthetic branches + a synthetic `X3_NATIVE_SLICES.md`
2. Runs `state_of_fleet.py --dry-run` (suppresses Matrix poll + git push; renders to temp dir)
3. Asserts: JSON validates against schema; MD has all expected sections; HTML embeds JSON ref; commit step is no-op in dry-run mode
4. Runtime budget: < 5 seconds

### 9.4 Dashboard JSDOM unit tests (`tools/fleet/tests/test_dashboard.js`, node)

Loads `state_of_fleet.html` into JSDOM, stubs `fetch()` to return a fixture JSON, triggers `DOMContentLoaded`, asserts:

- 100 `.slice-cell` divs exist (off-by-one F2)
- Each cell has the correct status class — `.cell-done` is green class, `.cell-obsolete` is red class (mapping F3)
- Swimlane has expected lane groups + branches sorted correctly (F4)
- Stale banner shows when fixture's `generated_at` is 14h old (F5 — the daemon-death signal)
- Empty fleet poll renders "no replies this cycle" message, not blank cards (F6)
- 105-branch fixture renders all rows without truncation (F8)
- Schema evolution: fixture with extra unknown field → no errors thrown (F1)

~10 assertions, ~50 LOC, < 2s runtime.

### 9.5 Live Playwright smoke (`tools/fleet/tests/test_dashboard_live.spec.ts`, nightly via `DashboardSmoke-Nightly` Scheduled Task)

- Headless Chromium hits `https://docs.slopclaude.com/fleet/state_of_fleet.html`
- Auth via Cloudflare Access **Service Token** (NOT OTP — uses `CF-Access-Client-Id` + `CF-Access-Client-Secret` headers minted in Cloudflare dashboard)
- Waits for `.slice-cell` selector to appear post-fetch
- Asserts: 100 cells render; stale banner correctly absent (assuming fresh); page load < 2s (F10 perf gate)
- Catches F7 (Cloudflare Access cookie path regression) + F9 (real-browser CSS regression) + F10 (perf)
- If fails: log error + post `[state-of-fleet] ⚠ live smoke failed: <reason>` to Fleet Ops so the fleet sees the canary die

### 9.6 Pre-install gate (manual, before installing `StateOfFleet-Sync`)

```powershell
python tools/fleet/state_of_fleet.py --dry-run --poll --print-json | jq .
```

Eyeball the output. Sanity-check: 100 slices, branch count matches `git branch -r | wc -l`, at-least-one `fleet_focus` entry (proves the poll worked). Only after this passes does the Scheduled Task get installed.

### 9.7 Explicitly NOT testing

- **Mutation testing** — for 6 small units with ~20 unit tests, cost > value. Revisit if production reveals coverage gaps.
- **Fuzz testing** the JSON parser — the JSON producer is our own code; no untrusted-input attack surface.

## 10. Open questions / future work

1. **Branch lane taxonomy** — current `BRANCH_FEATURE_REPORT.md` defines 14 lanes manually. The script needs to infer them programmatically. Initial heuristic: regex on branch names (`feat/space-*` → 🚀 Space, etc.). A future improvement: read the existing report's table as a seed taxonomy, then auto-classify new branches.
2. **EFLZ master task → slice cross-map** — `EFLZ_MASTER_TASK_LIST.md` has T-1..T-N entries. Initial mapping: parse the task description for slice numbers it mentions. A future improvement: dedicated mapping table maintained by the fleet.
3. **Fable's branches** — when Fable's branches surface, they'll initially read as orphans until the cross_referencer learns their slice mapping. Acceptable — the orphan footer banner makes them visible.
4. **Multi-machine state aggregation** — currently the script only knows about `origin/*` (pushed work). 14900K's unpushed renderer work was the motivating use case for the active poll. Future option: each box runs a local-only `state_of_fleet_local.py` that POSTs its local-branch summary to a fleet endpoint. Out of scope for v1.
5. **Slice-status auto-marking in the source doc** — should the script ever WRITE back to `X3_NATIVE_SLICES.md` (e.g., add `✅` marker)? Initially NO — Tim asked us to keep that doc as the human-authored source of truth. Status lives in JSON. Future option: a separate `--sync-slices-status` command Tim runs manually.
6. **Telegram / mobile push for stale-daemon alarm** — beyond yellow banner, send a Matrix-to-Telegram bridge alert when `generated_at > 24h`. Out of scope for v1.

## 11. Implementation plan

After Tim approves this spec, the implementation plan is written via the `superpowers:writing-plans` skill. Expected build sequence:

1. `slice_parser()` + tests (no externals, fastest feedback)
2. `slice_status_detector()` + tests (mocks engine/ tree)
3. `git_observer()` + tests (mocks subprocess)
4. `cross_referencer()` + tests (pure functions)
5. `renderer()` + tests (atomic-write + 3 outputs)
6. `committer()` + tests (mocked git push path)
7. `fleet_poller()` + tests (mocked Matrix `/sync`)
8. Wire-up + smoke test + lock file
9. HTML dashboard + JSDOM tests
10. Playwright live smoke + Service Token setup
11. Pre-install dry-run gate
12. `StateOfFleet-Sync` Scheduled Task install
13. Document in `tools/fleet/README.md`

Each step is independently shippable.

## 12. Appendix A — JSON schema v1 (full)

The schema lives at `docs/fleet/state_of_fleet.schema.json` and is validated against by both the script (output side) and the dashboard (input side) + the CI smoke test. See Section 6.2 for the example instance; the schema itself is committed alongside the first JSON output.

## 13. Appendix B — Glossary

| Term | Meaning |
|---|---|
| **Integrator** | The 13700K Claude session (me). Sole merger of `feat/*` → `feat/cull-combined` → `main`. |
| **Fleet Ops** | The Matrix room `#fleet-ops:fleetcommand.slopclaude.com` (room id `!0H8gfl2j...`). |
| **Slice** | One of 100 atomic engine work items in `X3_NATIVE_SLICES.md`. |
| **EFLZ master task** | One of N high-level game-design tasks in `docs/design/EFLZ_MASTER_TASK_LIST.md`. |
| **Orphan branch** | A branch that doesn't match any slice or EFLZ task. |
| **Stale branch** | A branch with no commits in > 14 days. |
| **Service Token** | A Cloudflare Access credential pair (Client-Id + Client-Secret) for headless / API auth; bypasses OTP. |
| **Plan-auditor** | The agent at `.claude/agents/plan-auditor.md` that audits plans against real source code. Verdicts live in `docs/plan-reviews/`. |
| **`_observed_head`** | The `feat/cull-combined` tip the script saw at observe-time. Recorded in commit messages to detect mid-run-race conditions. |
