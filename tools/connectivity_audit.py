"""Audit a v2.project.json floor's door connectivity for the NATIVE engine: a door can
only become a real doorway if the two rooms physically share a wall. Babylon treated
doors as logical center-to-center links, so some pairs may not touch. Reports adjacent
(doorway computable) vs gap/overlap/cross-level, and isolated rooms."""
import json, sys
SRC=r"C:\GameDev\OneDrive\GameDev\DellGameDev\Escape48BLN\LevelArchitect\EscapeLab48_AllFloors_v2.project.json"
fn=sys.argv[1] if len(sys.argv)>1 else "1"
f=json.load(open(SRC,encoding="utf-8"))["floors"][fn]
R=f["rooms"]; D=f["doors"]
def box(r): return (r["x"]-r["w"]/2, r["x"]+r["w"]/2, r["z"]-r["d"]/2, r["z"]+r["d"]/2, r["y"])
TOL=0.8; MINSPAN=1.0
def classify(a,b):
    ax0,ax1,az0,az1,ay=box(a); bx0,bx1,bz0,bz1,by=box(b)
    if abs(ay-by)>3: return "cross-level (stairs/tube)"
    ox=min(ax1,bx1)-max(ax0,bx0); oz=min(az1,bz1)-max(az0,bz0)
    if ox>TOL and oz>TOL: return "overlap (rooms interpenetrate)"
    # share a wall: edge-to-edge on one axis, real span on the other
    if oz>MINSPAN and (abs(ax1-bx0)<=TOL or abs(bx1-ax0)<=TOL): return "adjacent-X (doorway OK)"
    if ox>MINSPAN and (abs(az1-bz0)<=TOL or abs(bz1-az0)<=TOL): return "adjacent-Z (doorway OK)"
    gap=max(max(ax0,bx0)-min(ax1,bx1), max(az0,bz0)-min(az1,bz1))
    return f"GAP ~{gap:.1f}m (no shared wall)"
from collections import Counter
cnt=Counter(); bad=[]; deg=[0]*len(R)
for a,b in D:
    if a>=len(R) or b>=len(R): cnt["bad-index"]+=1; continue
    deg[a]+=1; deg[b]+=1
    c=classify(R[a],R[b]); cnt[c]+=1
    if c.startswith("GAP"): bad.append((R[a]["n"],R[b]["n"],c))
print(f"FLOOR {fn}: {len(R)} rooms, {len(D)} doors")
for k,v in cnt.most_common(): print(f"  {v:3} {k}")
iso=[R[i]["n"] for i in range(len(R)) if deg[i]==0]
print(f"isolated rooms (0 doors): {len(iso)}", iso[:12])
print("--- sample GAP doors (rooms that DON'T touch -> need adapt) ---")
for n in bad[:12]: print("   ",n[0],"<->",n[1],":",n[2])
