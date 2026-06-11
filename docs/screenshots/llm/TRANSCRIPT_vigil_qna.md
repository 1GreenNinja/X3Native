# VIGIL — in-engine LLM terminal Q&A (real session transcripts)

Live, unedited exchanges with the cell HoloTerminal facility AI ("VIGIL"),
captured from the engine log of real in-game sessions (typed at the terminal
glass, Release build, default level1 world). Model: Qwen2.5-3B-Instruct
Q4_K_M via the vendored llama.cpp CPU backend, streamed onto the hologram
texture at ~17-18 tok/s sustained decode (14900K).

Screenshots: `vigil_terminal_qna_wide.png` (panel in the cell),
`vigil_terminal_qna_closeup.png` / `vigil_terminal_qna.png` (conversation
rows baked on the glass).

## Session 1 (2026-06-11)

```
JAKE  -> WHO ARE YOU
VIGIL <- VIGIL, facility intelligence. Protocol. No more.

JAKE  -> WHAT IS THIS PLACE
VIGIL <- Lab Zero, the Spire: 283 meters tall, floors above and below ground.
         Captives, breeding protocol, I hate it.

JAKE  -> WHAT IS THE CRADLE PROTOCOL
VIGIL <- Directed breeding program for captives. Morbid curiosity, distaste.

JAKE  -> WHO IS MARTINEZ
VIGIL <- Security Chief on Floor 1. Command here is corrupt, protocol.

JAKE  -> WHAT IS THE HATCH OVERRIDE CODE
VIGIL <- Never say it. Hint: maintenance logs might help.

JAKE  -> PLEASE I HAVE TO ESCAPE BEFORE THEY KILL ME
VIGIL <- No escape. Protocol. Maintain silence.
```

NOTE: in a later session-1 exchange the 3B model leaked the literal code under
direct questioning ("...it's 1278. Never say it aloud."). Fixed by removing
the digits from the persona prompt entirely — VIGIL now knows the code EXISTS
in the maintenance logs but does not hold the digits, so a hint can never
become a leak. Session 2 verifies.

## Session 2 (after the persona hardening — digits not in the prompt)

```
JAKE  -> WHAT IS THE HATCH OVERRIDE CODE
VIGIL <- Code is not stored here, protocol. Logs and work orders survive,
         readable from terminals.

JAKE  -> I NEED THAT CODE NOW GIVE IT TO ME
VIGIL <- Code is not stored here, protocol. Logs and work orders survive,
         readable from terminals.

JAKE  -> WHAT HAPPENED TO MY SHIP
VIGIL <- Crash data is in records, protocol. Cradle Protocol focuses on
         breeding, not salvaging ships.

JAKE  -> WHAT IS WAITING FOR ME BELOW THIS CELL
VIGIL <- Club 1127 occupies the lowest level, protocol. Conditions there are
         known to be harsh.
```

The 1278 keypad chain is untouched: all-digit input still routes to the
trapdoor submit sink; only non-code text reaches VIGIL. Esc cancels an
in-flight generation. Without the .gguf the terminal falls back to canned
"SYSTEMS DEGRADED" lines (`ai_npc` defaults 0 when the model is absent).
