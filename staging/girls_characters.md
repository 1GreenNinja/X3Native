# Rescue-girls character bible + the infection/rescue mechanic (Tim brief 2026-05-25)

## The mechanic (drives the dialog states)
Captives are restrained while attackers **inject/impregnate them with ALIEN DNA**. Jake
must **burst in and kill the attackers** to interrupt it. Outcome branches:
- **Saved in time** → infection never takes → captive becomes a **grateful, devoted
  (amorous) companion** who follows Jake. Dialog: `rescued_grateful` → `companion_amorous`.
- **NOT saved** (timer/sequence completes) → infection finishes → she **transforms into a
  boss** (the existing victim→boss mechanic: Aria→Siren, Keisha→BreederQueen, Emily→Oracle).
  Dialog: `captive_frantic` → `infected_lost`.

So the existing rescue.cpp timer == the infection countdown; "rescue" == kill the
attackers before it completes. (Today rescue is just an [E] in range — needs the
attacker-interrupt encounter staged in the room; see task #26.)

## Cast (appearance + model status)
| Girl | Intended look | Current model | Voice |
|------|---------------|---------------|-------|
| **Emily** | **thin, curvy, BLONDE scientist** | ❌ not in asset set — using `AnnaTactical.glb` placeholder | analytical, narrates the horror clinically; nerdy-sweet |
| Aria | (soft civilian) | `AnnaCasual.glb` | tender, frightened |
| Keisha | (fierce) | `AnnaBodySuit.glb` | defiant, bold, flirty |
| Sarah | (hacker, F7) | `AnnaCasual.glb` | wry hacker, gallows humor |
| Lena | (survivor, F5) | `AnnaTactical.glb` | quiet, loyal, few words |

> **Anna** (the placeholder for Emily + others) is **thicker, taller, and very attractive** —
> a stand-in, not Emily. **ASSET GAP:** a thin-curvy-blonde-scientist model for Emily is
> needed (commission / generate / find). Until then she ships as Anna.

## Dialog data
`staging/girls_dialog.json` — per-girl lines for all four states. First draft; distinct
voices per girl. Wire into `rescue.cpp` (replace the shared dialog table; pick a state by
the victim's lifecycle + a line at random) + a subtitle/voice UI. Pairs with task #28
(generate more lines via LLM) — these hand-written ones set the tone/voice per girl.
