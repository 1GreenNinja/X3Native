"""BEFORE/AFTER Floor-1 plan proof for the canonical-level swap.

LEFT  = the OLD hand-coded level1.cpp kDetention table (29 rooms, 3 m-wide
        "Main Hallway", cells crammed in narrow columns) — the WRONG in-game layout.
RIGHT = the CANONICAL LevelArchitect Floor 1 (53 rooms, 44 m-wide Main Hall, cells
        on BOTH sides), parsed from the SAME JSON the engine now boots by default:
        assets/levels/EscapeLab48_AllFloors_v2.project.json.

Top-down; X east (right), +Z up. Deep underground rooms (y<-50) dropped (they'd
stamp on top of the surface facility in a flattened plan).
Usage: python tools/floorplan_before_after.py
Writes: tools/floorplan_before_after.png
"""
import json, os, re
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt, matplotlib.patches as mp

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# ---- RIGHT: canonical 53-room Floor 1 (the JSON the engine now boots) -------------
CANON = os.path.join(REPO, "assets", "levels", "EscapeLab48_AllFloors_v2.project.json")
cj = json.load(open(CANON, encoding="utf-8"))
f1 = cj["floors"]["1"]
canon_rooms = [r for r in f1["rooms"] if r.get("y", 0) >= -50]
canon_drop = len(f1["rooms"]) - len(canon_rooms)

# ---- LEFT: the OLD kDetention[] table, scraped straight out of level1.cpp ----------
src = open(os.path.join(REPO, "app", "level1.cpp"), encoding="utf-8").read()
blk = src[src.index("kDetention[] = {"):]
blk = blk[:blk.index("};")]
old_rooms = []
row = re.compile(r'\{\s*"([^"]+)",\s*([-\d.f]+),\s*([-\d.f]+),\s*([-\d.f]+),\s*([-\d.f]+),\s*([-\d.f]+),\s*([-\d.f]+)')
for m in row.finditer(blk):
    name = m.group(1)
    cx, cz, fy, w, h, d = (float(g.rstrip("f")) for g in m.groups()[1:])
    old_rooms.append({"n": name, "x": cx, "z": cz, "w": w, "d": d, "fy": fy})

def draw(ax, rooms, title, hallname_substr):
    pal = ["#4a6fa5","#7b6b55","#5a7a5a","#5a6a7a","#8a3a3a","#155e6e","#6a4a7a"]
    for i, r in enumerate(rooms):
        x, z, w, d = r["x"], r["z"], r["w"], r["d"]
        # highlight the hall in cyan so the WIDTH contrast is obvious
        is_hall = hallname_substr.lower() in r["n"].lower()
        fc = "#16b6d6" if is_hall else pal[i % len(pal)]
        ax.add_patch(mp.Rectangle((x-w/2, z-d/2), w, d, facecolor=fc,
                                  edgecolor="#eee", lw=0.8, alpha=0.9))
    ax.set_aspect("equal"); ax.grid(True, alpha=.12)
    ax.set_title(title, fontsize=12)
    ax.set_xlabel("X east (m)"); ax.set_ylabel("Z (m)")

fig, (a0, a1) = plt.subplots(1, 2, figsize=(20, 11))
draw(a0, old_rooms,
     f"BEFORE — hand-coded level1.cpp ({len(old_rooms)} rooms)\nnarrow 3 m 'Main Hallway', cells in thin columns",
     "Main Hallway")
draw(a1, canon_rooms,
     f"AFTER — CANONICAL LevelArchitect Floor 1 ({len(canon_rooms)} rooms)\nWIDE 44 m Main Hall (cyan), cells on BOTH sides",
     "Main Hall")
# shared axis scale so the size difference is HONEST
allx = [r["x"]+s*r["w"]/2 for rms in (old_rooms, canon_rooms) for r in rms for s in (-1,1)]
allz = [r["z"]+s*r["d"]/2 for rms in (old_rooms, canon_rooms) for r in rms for s in (-1,1)]
for ax in (a0, a1):
    ax.set_xlim(min(allx)-4, max(allx)+4); ax.set_ylim(min(allz)-4, max(allz)+4)
fig.suptitle("X3Native Floor 1 — wrong hand-coded layout (left) vs the REAL canonical level now booted by default (right)",
             fontsize=14)
out = os.path.join(HERE, "floorplan_before_after.png")
plt.tight_layout(rect=[0,0,1,0.97]); plt.savefig(out, dpi=110)
print("wrote", out)
print(f"BEFORE rooms={len(old_rooms)}  AFTER rooms={len(canon_rooms)} (dropped {canon_drop} deep)")
# report hall widths
oh = next((r for r in old_rooms if "main hall" in r["n"].lower()), None)
ch = next((r for r in canon_rooms if r["n"]=="Main Hall"), None)
if oh: print(f"BEFORE Main Hallway: w={oh['w']} d={oh['d']}")
if ch: print(f"AFTER  Main Hall:    w={ch['w']} d={ch['d']}")
