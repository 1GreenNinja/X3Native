"""Top-down PNG of a floor from EscapeLab48_AllFloors_v2.project.json (the 53-room
canonical layout). Rooms: {n,t,x,y,z,w,h,d,f}; x/z=center, w(x)/d(z)=full extents.
Usage: python v2_floor_topdown.py [floorNum]  -> tools/v2_floor<N>_topdown.png"""
import json, sys, itertools
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt, matplotlib.patches as mp

SRC=r"C:\GameDev\OneDrive\GameDev\DellGameDev\Escape48BLN\LevelArchitect\EscapeLab48_AllFloors_v2.project.json"
fn=sys.argv[1] if len(sys.argv)>1 else "1"
d=json.load(open(SRC,encoding="utf-8"))
fl=d["floors"][fn]
rooms=fl["rooms"]; doors=fl.get("doors",[])
# Drop the deep underground levels (caves / hidden sub-level at y<-50): a top-down map
# flattens height, so those -174/-178 m rooms would stamp ON TOP of the surface facility
# and become unreadable. Keep only the facility floor (y >= -50).
deep=[r for r in rooms if r.get("y",0)<-50]
rooms=[r for r in rooms if r.get("y",0)>=-50]
if deep: print("dropped %d deep underground rooms (y<-50): %s"%(len(deep),", ".join(r['n'] for r in deep)))
# color per room type
types=sorted({r.get("t","?") for r in rooms})
palette=["#4a6fa5","#7b6b55","#5a7a5a","#5a6a7a","#8a3a3a","#155e6e","#6a4a7a","#7a6a3a",
         "#3a7a6a","#7a3a6a","#4a4f12","#555","#2b7a4a","#7a5a2a"]
cmap={t:palette[i%len(palette)] for i,t in enumerate(types)}

xs=[r["x"] for r in rooms]; zs=[r["z"] for r in rooms]
fig,ax=plt.subplots(figsize=(18,13))
for r in rooms:
    x,z,w,dd=r["x"],r["z"],r["w"],r["d"]; t=r.get("t","?"); n=r.get("n","?")
    ax.add_patch(mp.Rectangle((x-w/2,z-dd/2),w,dd,facecolor=cmap[t],edgecolor="#ddd",lw=1,alpha=.88))
    ax.text(x,z,n,ha="center",va="center",fontsize=6.5,color="white")
# doors: list of [i,j] index pairs OR dicts — draw if index pairs
for dr in doors:
    try:
        a,b=(dr[0],dr[1]) if isinstance(dr,(list,tuple)) else (dr.get("a"),dr.get("b"))
        if a is not None and b is not None and a<len(rooms) and b<len(rooms):
            ax.plot([rooms[a]["x"],rooms[b]["x"]],[rooms[a]["z"],rooms[b]["z"]],color="#fc0",lw=.6,alpha=.4,zorder=1)
    except Exception: pass
ax.set_aspect("equal")
ax.set_xlim(min(x-r["w"]/2 for x,r in zip(xs,rooms))-4, max(x+r["w"]/2 for x,r in zip(xs,rooms))+4)
ax.set_ylim(min(z-r["d"]/2 for z,r in zip(zs,rooms))-4, max(z+r["d"]/2 for z,r in zip(zs,rooms))+4)
handles=[mp.Patch(color=cmap[t],label=t) for t in types]
ax.legend(handles=handles,loc="upper right",fontsize=8,ncol=2)
nm=d.get("floors",{}).get(fn,{}).get("name","")
ax.set_title(f"EscapeLab48 v2 project — FLOOR {fn} ({len(rooms)} rooms)  top-down  X east, +Z up",fontsize=13)
ax.set_xlabel("X (m)"); ax.set_ylabel("Z (m)"); ax.grid(True,alpha=.12)
out=f"C:/GameDev/X3Native-engine/tools/v2_floor{fn}_topdown.png"
plt.tight_layout(); plt.savefig(out,dpi=120); print("wrote",out)
