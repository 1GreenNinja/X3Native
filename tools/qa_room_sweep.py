#!/usr/bin/env python3
"""QA room sweep — headless per-room screenshot driver for the canon facility.

Reads the canonical level JSON room registry and shoots every selected room from
player eye height (floor + 1.7 m) with `--world canonlevel --shot-cam ... --screenshot`.
One engine run per shot (the engine has no batch shot-cam); runs are serialized.

Usage:
  python tools/qa_room_sweep.py [--out docs/screenshots/qa_mainlevel/sweep] [--floors 1,2,...]
                                [--only substr] [--dry] [--settle 48]

Shot naming: F<floor>_<room-slug>_<a|b>.png  (a = looking down +long axis, b = back)
Cell blocks: only one representative cell per wing per floor is shot (they are
stamped from the same recipe); Jake's cell and named cells always shoot.
"""
import argparse, json, os, re, subprocess, sys, math

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE  = os.path.join(REPO, "build", "bin", "Release", "X3Engine.exe")
JSON_PATH = os.path.join(REPO, "assets", "levels", "EscapeLab48_AllFloors_v2.project.json")

def slug(s):
    return re.sub(r"[^A-Za-z0-9]+", "_", s).strip("_")

def select_rooms(floors_want):
    d = json.load(open(JSON_PATH))
    picked = []
    for fn, fl in sorted(d["floors"].items(), key=lambda kv: int(kv[0])):
        if floors_want and int(fn) not in floors_want:
            continue
        seen_generic_cell_wing = set()
        for rm in fl["rooms"]:
            name, typ = rm["n"], rm["t"]
            # generic cells: one representative per wing (WL/WR/EL/ER); named cells always
            m = re.match(r"^(WL|WR|EL|ER)-\d+$", name)
            if m:
                if m.group(1) in seen_generic_cell_wing:
                    continue
                seen_generic_cell_wing.add(m.group(1))
            picked.append((int(fn), rm))
    return picked

def shots_for(room):
    """Two eye-height shots: near one end looking down the long axis, and the reverse."""
    cx, cy, cz = room["x"], room["y"], room["z"]
    w, h, d    = room["w"], room["h"], room["d"]
    eye = cy - h / 2.0 + 1.7
    # slight downward pitch so the floor seam band is in frame
    pitch = -0.10
    out = []
    if w >= d:   # long axis = X, yaw 0 looks +X
        back = max(w / 2.0 - 1.0, 0.5)
        out.append((cx - back, eye, cz, 0.0, pitch))
        out.append((cx + back, eye, cz, math.pi, pitch))
    else:        # long axis = Z, yaw +pi/2 looks +Z
        back = max(d / 2.0 - 1.0, 0.5)
        out.append((cx, eye, cz - back, math.pi / 2.0, pitch))
        out.append((cx, eye, cz + back, -math.pi / 2.0, pitch))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="docs/screenshots/qa_mainlevel/sweep")
    ap.add_argument("--floors", default="", help="comma list, empty = all")
    ap.add_argument("--only", default="", help="substring filter on room name")
    ap.add_argument("--settle", type=int, default=48)
    ap.add_argument("--dry", action="store_true")
    ap.add_argument("--extra", default="", help="extra engine args, space-separated")
    args = ap.parse_args()

    floors = set(int(x) for x in args.floors.split(",") if x.strip()) if args.floors else None
    outdir = os.path.join(REPO, args.out) if not os.path.isabs(args.out) else args.out
    os.makedirs(outdir, exist_ok=True)

    rooms = select_rooms(floors)
    if args.only:
        rooms = [(f, r) for f, r in rooms if args.only.lower() in r["n"].lower()]
    total, fails = 0, []
    for fnum, rm in rooms:
        for tag, (x, y, z, yaw, pitch) in zip("ab", shots_for(rm)):
            png = os.path.join(outdir, f"F{fnum}_{slug(rm['n'])}_{tag}.png")
            cam = f"{x:.2f},{y:.2f},{z:.2f},{yaw:.3f},{pitch:.3f}"
            cmd = [EXE, "--world", "canonlevel",
                   "--shot-cam", cam, "--screenshot", png, str(args.settle)]
            if args.extra:
                cmd += args.extra.split()
            total += 1
            if args.dry:
                print(" ".join(cmd)); continue
            r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, timeout=300)
            ok = r.returncode == 0 and os.path.exists(png)
            print(("OK  " if ok else "FAIL") + f" F{fnum} {rm['n']} [{tag}] cam={cam}")
            if not ok:
                fails.append(png)
                tail = (r.stdout or "").splitlines()[-4:]
                for ln in tail: print("     " + ln)
    print(f"\n{total} shots, {len(fails)} failed")
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
