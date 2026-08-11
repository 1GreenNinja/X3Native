# THE AGENT PLAYBOOK — how this studio shipped Waves 1–5 in three days
*Written by Fable on its final day. This is the METHOD. The companion trap-ledger is
docs/ENGINE_GOTCHAS.md; the visual law is docs/design/ART_BIBLE.md; the geometry law is
the x3-level-authoring skill. A future Opus/Sonnet session that follows this playbook
runs this studio at 90% of Fable's throughput. The 10% you can't inherit is judgment —
compensate with smaller waves and more eye-rounds.*

## 0. The shape of the operation
One DIRECTOR session (you) + waves of 3-5 background agents in isolated git worktrees.
The director never writes feature code during a wave — the director slices scope, writes
briefs, integrates serially, gates everything, reads every screenshot, and talks to Tim.
Agents own disjoint files, verify their own work against explicit gates, and return
structured reports. Everything merges through `integration/honor-fable-final`.

## 1. The brief (charter pattern) — every agent prompt has exactly these parts
1. **Identity + workspace**: "You are W#-#, <mission name>. Workspace: worktree
   D:\GameDev\wt-<name>, branch <branch> at <sha>. Never touch other checkouts."
2. **MISSION**: what player-facing outcome must exist when you're done. Include the WHY
   (agents make better micro-decisions when they know the intent).
3. **OWNED FILES — STRICT**: an explicit list, plus an explicit NOT-list naming the files
   other live agents own. File ownership is the whole concurrency model.
4. **BUILD block**: fetch --all → fresh-dir configure with the vcpkg toolchain → Release
   build → THE FABLE GATE (exe mtime advanced; see gotchas 1.1-1.4) → the test suite
   relevant to the mission, named explicitly, "0 new fails".
5. **EYE GATE**: which screenshots to take (camera coords included when known), "READ
   them yourself", honest /10, ≤3 rounds, and the specific failure modes that mean
   self-bounce ("if it reads like X, bounce yourself").
6. **Commit + co-author trailer; NO push, NO merge.**
7. **RETURN format**: enumerate what the report must contain (sha, per-item status,
   screenshot paths, honest residuals, gate results). Structured returns make integration
   mechanical.

## 2. Slicing scope — file ownership is everything
- One agent per problem domain; domains chosen so their FILE SETS are disjoint. When two
  missions want the same file, either merge them into one agent, serialize them, or split
  by REGION with explicit region names ("app_run.cpp: the cue-sink factory region is
  yours; the canon-spawn block is W4-1's — stay out"). Region splits merged cleanly every
  time we used them (git handles disjoint hunks).
- Shared-file collisions that DID happen: manifest.json (auto-merged), room_dressing.cpp
  in Wave 5 (different functions — "merge whichever lands first, the other rebases
  trivially" worked verbatim).
- New systems go in NEW FILES (canon_45.cpp, room_dressing.cpp, level_lint.cpp,
  surface_library.cpp) — ownership by construction, and app_run only gains a build/tick
  call site.

## 3. The paste-block pattern (cross-ownership wiring)
When an agent's feature needs a line in a file someone else owns, it does NOT edit — it
returns a PASTE-BLOCK: the exact code + the exact anchor, in its report. The director
applies paste-blocks after the owning agent lands. This resolved every cross-cut in five
waves (W2-B's cue-sink subscription, W2-C's viewmodel-draw + reload hooks, W2-E's door
PVS gate, AD-1's cvars, W5-2's tableau wiring) with zero merge conflicts.

## 4. Merge order — providers before consumers
Integrate in dependency order: API/asset providers first (audio files before the code
that resolves them; rescue-system changes before the endgame that reads them; recipe
system before the floors that use it). Within a wave the director merges each landing
serially: merge → re-configure → build → mtime check → FULL gate suite → eye-verify →
push. Never batch-merge unbuilt branches.

## 5. The gates (verbatim, non-negotiable)
- **Fable gate**: Release build relinked (mtime verified) · relevant --test-* suites 0
  new fails · `--world canonlevel --smoketest` 0 VUID + allocationCount=0 ·
  `--test-levellint` PASS whenever geometry moved.
  > ⚠️ **A "0 VUID" claim is only valid if the layers were ON.** Release used to
  > compile validation out entirely, so every historical Release "0 VUID" proved
  > nothing. Run `--validate` (or use Debug) and QUOTE the
  > `smoketest: VALIDATION layers=… sync-validation=…` line with the result.
  > **Touched a barrier / ResourceUse / loadOp / storeOp / layout? Add `--vksync`**
  > — standard validation does NOT check synchronization. See `docs/VALIDATION.md`.
- **Eye gate**: the agent reads its own renders and scores honestly; the DIRECTOR
  re-reads every screenshot personally before accepting a merge (memory rule: never
  relay a visual claim you haven't seen). Overselling a blockout is a firing offense.
- **Gate C (playthrough truth)**: `--test-goldenpath` (state-level spine: cell → hatch →
  elevator → F7 gate → Sarah → helipad win) must stay green. Live-feel items headless
  can't verify go on Tim's playtest list EXPLICITLY.

## 6. Failure modes seen (and the countermeasures now standard)
1. **Stale-exe false-green** — agents tested an old binary and reported success. Counter:
   mtime verification is IN every brief; distrust any report without it.
2. **Tests-pass-but-looks-wrong** — Wave 0's cell "passed" and looked like garbage.
   Counter: the eye gate + the director's own eyes + the ART_BIBLE as the rubric.
3. **Overselling** — art agents call blockouts AAA. Counter: demand honest /10 with
   named flaws; treat a report with zero residuals as suspicious.
4. **Agent dies silently** (W2-A: hours of silence, transcript gone). Counter: if an
   agent is silent long past its cohort, ping once; if unreachable, spawn a SUCCESSOR
   with the full original scope + every paste-block queued since (W2-A2 pattern) —
   never wait indefinitely, never absorb complex scope into the director's own context.
5. **Camera-in-wall verdicts** — review shots taken from inside geometry produce false
   alarms (floating doors, void). Counter: data-derived cameras only (gotchas 4.1).
6. **Wrong-target exploration** — an early agent explored a different engine entirely.
   Counter: pin absolute paths + "NOT X" in the brief's first lines.
7. **Convention violations by competent agents** (W3-2 committed store-served GLBs; W2-E
   edited outside its file list but flagged it). Counter: read the "deviations" section
   of every report FIRST; undo what violates, accept what's flagged and sound.
8. **Two sessions, one branch** — an outside "Empire Fold" session committed directly to
   integration mid-campaign. It merged fine, but watch `git log` for foreign authors
   before pushing; never force-push this branch.

## 7. Honest-residuals discipline (the cultural core)
Every report ends with what was NOT verified and why ("playback quality unhearable
headless", "motion can't be proven by a still", "4/10 on the capture — interactive path
test-verified only"). This is REQUIRED, not tolerated. The director's job is to route
those residuals: to Tim's playtest list, to a follow-up task, or to a fix round. An
agent that reports everything green with no residuals gets its work spot-checked twice.

## 8. Scale doctrine — calibrate, then recipe, then sweep
Never hand-polish N rooms. Hand-polish ONE space against the bible until the human's eye
says "bar" (the cell, R1-R5). Extract the recipe (palette/lights/composition per room
TYPE). Build the recipe SYSTEM (room_dressing). Apply per type across the world, with
per-type eye-round sampling — 124 rooms cost ~0.05 ms and one wave. The same doctrine
scaled textures (curate 20 sets from 2,626 cataloged, not all), audio (species buckets,
not per-enemy takes), and floors (classify() + recipes, not per-floor passes).

## 9. Director duties per integration round (checklist)
1. Read the report: deviations first, residuals second, claims last.
2. Merge; re-configure; build; MTIME CHECK; fetch --all if the branch store-published.
3. Full gate suite (levellint, canonlevel, secretroom, rescue, goldenpath, smoketest +
   suites the branch touched).
4. Read the agent's screenshots YOURSELF; render your own frame if claims are
   load-bearing; honest score in your own words.
5. Apply queued paste-blocks that this merge unblocked; rebuild + regate.
6. Push. Update the task list. Update memory files at milestones.
7. Tell Tim what actually happened — outcomes first, honest flaws named, decisions he
   owns surfaced explicitly (never bury a human decision inside progress prose).
8. Refresh the Michigan portable package when player-facing (exe + shaders zip; assets
   delta zip when store/manifest changed — CRD caps single files at 500 MB).

## 10. What to spend a strong model on (when you have one)
Judgment-dense engineering (render-graph fog, door-derivation forensics), creative
direction (bible, level 4.5's reveal), root-cause hunts, and WRITING DOCTRINE. Mechanical
sweeps (asset scouting, audio curation, catalog scans, design docs from clear briefs) run
fine on Sonnet — brief them tighter and verify harder.
