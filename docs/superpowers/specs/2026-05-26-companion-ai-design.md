# Companion AI — Design: LLM-powered NPC co-op companions

> X3Native · brainstormed with Tim 2026-05-26 · lane `feat/companion-ai`.
> **Clean-room:** built only on X3Native's own systems (`player.*`, `monster.*`, `scene.*`,
> `terrain.*`, `engine/net/*`, the netcode spec) + public game-AI references (utility AI,
> behavior trees) + the Claude/Grok public APIs. No game-engine source consulted.

## 1. Vision

NPC companions that fight alongside the player as **autonomous squadmates** and double as
**"NPC co-op players"** — AI that drives *player* entities filling co-op slots. They are
**conversational, LLM-powered characters** with real personality, not scripted bots.

One system, two roles:
- **Story companions** — rescued characters (Aria, Sarah, the Lab Girls, Pixie, …) who join
  and fight with you; each has identity + voice.
- **Generic co-op-slot AI** — anonymous AI players that fill empty co-op slots and are swapped
  out the instant a human drops in.

## 2. The two-brain architecture (the crux)

A companion has **two cooperating brains** running at very different rates:

- **Reflex brain** — runs **every sim tick**, **deterministic + server-authoritative**
  (mandatory: the netcode spec `specs/NETCODE-architecture.spec.md` requires a deterministic
  server sim). A utility + behavior-tree system that reads the fight and emits the actual
  `move / aim / fire / cover / revive` command for its player entity. Fast, predictable,
  netcode-safe. **Not** `monster.cpp` — companion logic is a separate, far more capable system.
- **Cognitive brain** — runs **async, OFF the sim loop**, **LLM-powered**: personality, banter,
  high-level situational reasoning, talking to you and each other. **Grok powers the female
  companions; Claude powers the male companions.** It outputs (a) *intents* that bias the reflex
  brain's scores (via the "suggestion" seam below) and (b) *speech*.

**The iron rule:** the LLM is never in the per-tick sim — too slow (100 ms–s) and
non-deterministic, and the deterministic server tick forbids it. It rides alongside, feeding
intent + dialogue *down* into the reflex brain. The seam between them is the same **light
suggestion channel** the player's nudges use.

## 3. Decisions locked (brainstorm 2026-05-26)

| Question | Decision |
|---|---|
| Control model | **Autonomous + light player suggestions** (no micromanagement) |
| Identity | **Both** rescued story characters **and** generic co-op-slot fill |
| Reflex AI core | **Utility + behavior-tree hybrid**, "9X advanced" (see §6) |
| Cognitive core | **LLM, async/off-sim**: **Grok = girls, Claude = guys** (Claude side uses prompt caching) |
| Conversational | **Yes** — ambient banter + player-interactive |

## 4. Netcode / determinism fit

This design is *because of*, not in spite of, the multiplayer architecture:
- A companion **is a player entity** whose `NetCommand`s come from the reflex brain instead of
  GLFW/UDP. Per the netcode spec's #1 principle (everything is a command → server sim), this is
  just **a third command source** — no new sim path.
- Because the reflex brain runs **server-side**, NPC companions work in **single-player /
  loopback with zero networking** — they ship before human co-op (Phase 2 UDP).
- The cognitive (LLM) brain is non-deterministic + async, so it stays **outside** the
  deterministic tick, feeding intents/speech. Determinism + the snapshot model are preserved.

## 5. Subsystem decomposition + build order

Each is its own spec → plan → implementation cycle. Build in order; each is usable on its own.

1. **Reflex tactical AI** — utility+BT brain → player commands. SP, **no APIs**. ← *this spec details it first*
2. **Cognitive intent layer** — game-state → LLM → intents that bias the reflex scores.
3. **Conversation system** — dialogue/banter, player-interactive, context assembly.
4. **Dual-provider LLM integration** — Grok (girls) / Claude (guys); prompt caching; clean transport seam.
5. **Voice** — TTS out (spoken lines), optional STT in (talk to them).

## 6. Sub-project #1 (FIRST): Reflex Tactical AI

**Purpose:** smart, autonomous combat squadmates that work in single-player today, with a clean
seam for the cognitive brain + player suggestions to plug into later.

**Architecture:** one `CompanionBrain` per companion =
- a **utility scorer** that ranks candidate behaviors each tick and picks the best (the *what*), and
- **small behavior trees** that run the *how* for multi-step actions (e.g., the take-cover or
  revive sequence).
It emits a **`CompanionCommand`** per tick — the same shape as `PlayerInput`/`NetCommand`
(move axes, look angles, button edges: fire/use/reload) — consumed by the existing
`Player::update` + `IPhysicsWorld` character controller + the weapon fire path. **Server-side,
deterministic.** A `CompanionSquad` owns N companions.

**Scored behaviors:** follow/regroup, engage-threat, take-cover, suppress, reload, revive-downed-ally,
retreat-when-low, hold-position.
**Scoring context (the "reads the situation" part):** nearest / most-dangerous threat; distance +
line-of-sight to the player; *am I in the player's line of fire?*; is an ally or the player downed?;
my health + ammo; the current **suggestion bias**.

**Suggestion seam:** a light bias channel (focus-fire target / regroup / hold) that bumps behavior
scores. This is where (a) the player's light commands and (b) the cognitive brain's intents plug in.
For #1 it exists but is driven only by player nudges (no LLM yet).

**Navigation:** reuse the existing nav system (a `--test-nav` already exists — verify its API in the
plan). Companions path to cover/follow points through it.

**In scope for #1:** the reflex brain + behaviors + command output + a headless **`--test-companion`**
self-test driving a synthetic fight (companion picks correct behaviors vs scripted threats, deterministic).
**Out of scope for #1:** all LLM / dialogue / voice / networking (subsystems 2–5).

## 7. Interfaces

`app/companion.{h,cpp}` (game/slice; engine stays pure):
- `CompanionBrain` — the reflex utility+BT, `tick(ctx) -> CompanionCommand`.
- `CompanionSquad` — owns N companions, builds them (story or generic), ticks them.
- `runCompanionSelfTest()` — headless, mirrors `runMonsterSelfTest()` style.
- A `CompanionSuggestion` POD = the bias seam (later fed by the cognitive brain).

## 8. Resolved (2026-05-26)

- **Squad size:** up to **7** companions (you + 7 = an 8-strong fireteam). `CompanionSquad`
  supports N ≤ 7. *Perf note: 7 full `Player` controllers + 7 reflex brains/tick is the heaviest
  mover choice; reflex scoring runs **job-parallel** (one job per companion), and Jolt handles 7
  character controllers comfortably — but profile the showcase with a full squad.*
- **Downed/revive:** **co-op downed → revive** for companions AND the player. Adds a `Downed`
  state (incapacitated, finishable) + the `revive-downed-ally` behavior (already in the scored
  set) + a HUD cue + a revive window/timer.
- **Companion mover:** **reuse the existing `Player` character controller** — companions *are*
  player entities, so they move with the exact physics the netcode predicts (no second mover).
- **First test bed:** a dedicated **`--world companion`** showcase (spawn the player + a scripted
  squad + scripted threats), mirroring the `--world valley/openworld` pattern, plus the headless
  **`--test-companion`** self-test.
