# Detention Level — Locked Doors, Hidden Areas & Bio-Mesh System (design)

_Design captured 2026-05-30 with Tim. Floor 1 "Detention Level" (`--world canonlevel`). Status: DESIGN — door locks approved to build; hidden areas + bio-mesh still being designed._

## 1. Layout (top-down, Floor 1)

Public, walkable circulation (the **blue** halls on Tim's markup, all open to each other):
- North offices row (z≈50): Entrance, Admin, **IT Room**, Network Hub
- **Main Hall** (z≈44, 44 m wide) — the spine entry
- Central command spine (x≈22): **Security Station** (z38) · **Research Lab** (z30) · **Medical Bay** (z22, rescue girls) · **Armory** (z14)
- Detention cells both sides: WL/WR (x2/9 west), EL/ER (x35/42 east), z40→5
- Bottom Hall (z1) → Boss Approach → Boss Arena (Martinez, z-14) → Elevator Lobby/Shaft (z-25) → sub-levels (y≈-174)

## 2. Door locks (APPROVED — build first)

| Room | Lock | Player-facing |
|---|---|---|
| **Security Station** | keycard **OR** code | "Locked — acquire the ___ keycard, or enter the code." Either opens it. |
| **Medical Bay** | keypad **code only** | E → keypad → correct code. |
| **Armory** | keycard **AND** code | Must hold the keycard *and* enter the code. |
| Research Lab | open | normal E toggle |

All doors: E opens / E closes (no proximity auto-open — fixed in 8489d43).

## 3. Hidden system (the **yellow** under-route — invisible from the public halls)

A sub-level **under-hall** with **THREE stairways**, the secret backbone linking two concealed areas:
- **Stair 1**: DOWN from the **Armory** into the under-hall.
- **Stair 2**: UP to the **Security Monitoring Station** — at the **IT Room spot** (north, across the Main Hall). Glass holographic screens + weapon lockers. The **IT Room is the back room behind it**.
- **Stair 3**: to the **Bio-Integration Lab** — the disguised room with 3 survivors (below).

### Bio-Integration Lab (the "bookshelf" room)
- Reached via the hidden stair system (NOT the public halls). Highly detailed rendered textures.
- Occupants who barricaded themselves in, uninfected: a **researcher** (f), a **doctor**, and a **guard/mechanic**.
- **Save them → choose 2 of 7 bio-mesh upgrades** (more acquirable later).
- Inside is a **bookshelf** = a disguised **one-room elevator** DOWN to the **Hidden Supply Cache** (which sits directly under the Bio-Lab). The elevator only activates AFTER the player has the **substrate-scan** bio-mesh upgrade (it reveals the elevator's electronic signature).

## 4. Bio-Mesh system

- **Skill TREES** (multiple, like other RPGs), presented as a **blue / light-blue holographic overlay**.
- Reward gate: rescuing the Bio-Lab survivors grants **a choice of 2 of 7** starting bio-mesh upgrades; more found/earned later.
- Keystone upgrade: **Substrate Scan** — a **CP2077-grade scan/hack overlay** (à la Watch Dogs 2 but richer): highlights electronic signals, hackable objects, signatures through surfaces; gates the Bio-Lab→Supply-Cache elevator and opens hacking gameplay.

## 5. Phasing (build order)

1. **Door-lock system** (keycard / code / both / either) + the 3 locked doors — APPROVED, building first. (door.* + main.cpp + level_loader.cpp.)
2. **Hidden structure**: under-hall + 3 stairs + Security Monitoring Station (IT-room area) + Bio-Integration Lab shell + one-room elevator to the (existing) Hidden Supply Cache.
3. **Bio-Lab survivors** (researcher/doctor/guard) + the rescue/choose-reward interaction + bookshelf swing-door.
4. **Bio-Mesh system**: skill-tree data + the blue holo overlay UI + choose-2-of-7 + the Substrate-Scan / CP2077 scan-hack overlay (gates the elevator).

## 6. OPEN (to pin down before Phases 3-4)

- The **7 bio-mesh upgrades** + their tree grouping (draft to propose: Substrate-Scan + e.g. subdermal armor / reflex slow-mo / regen mesh / neural-hack / optical camo / adrenal-combat).
- The Security/Medical/Armory **codes** + the **keycard name(s)** and where each is found (likely tied to the survivors / Security Monitoring Station). Placeholder codes used in Phase 1, refined here.
- Exact tile placement of the under-hall, 3 stairs, and the bookshelf entrance (Tim to confirm/Paint).
