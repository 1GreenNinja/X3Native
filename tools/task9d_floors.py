"""Parse the Task9D AllFloors HTML addBox() calls and render Floors 2-7 top-down (one
panel each) so the whole tower can be eyeballed. addBox(g,x,z,w,d,y,h,c,'label','desc').
Top-down uses x,z (center) + w,d (extents); y/h ignored. -> tools/tower_floors_2_7.png"""
import re
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt, matplotlib.patches as mp

SRC=r"C:\_review_src_bln\Task9D_AllFloors_v10_3D_Models & Editor.html"
html=open(SRC,encoding="utf-8",errors="replace").read()
# addBox(g, x, z, w, d, y, h, c, 'label', 'desc'...)  — x,z,w,d are numeric literals
pat=re.compile(r"addBox\(\s*([A-Za-z0-9_]+)\s*,\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)\s*,[^,]+,[^,]+,\s*(0x[0-9a-fA-F]+)\s*,\s*'([^']*)'")
rooms={}
for m in pat.finditer(html):
    g,x,z,w,d,c,label=m.group(1),float(m.group(2)),float(m.group(3)),float(m.group(4)),float(m.group(5)),m.group(6),m.group(7)
    rooms.setdefault(g,[]).append((x,z,w,d,int(c,16),label))
TITLES={"gF2":"F2 Medical Bay","gF3":"F3 Genetics Lab","gF4":"F4 Cybernetics",
        "gF45":"F4.5 Nexus Chamber","gF5":"F5 Drone Station","gF6":"F6 Alien Tech","gF7":"F7 Executive"}
order=[g for g in ["gF2","gF3","gF4","gF45","gF5","gF6","gF7"] if g in rooms]
print("groups found:",{g:len(v) for g,v in rooms.items()})
fig,axes=plt.subplots(2,4,figsize=(24,12)); axes=axes.flatten()
for i,g in enumerate(order):
    ax=axes[i]; rs=rooms[g]
    for x,z,w,d,c,label in rs:
        col="#%06x"%c
        ax.add_patch(mp.Rectangle((x-w/2,z-d/2),w,d,facecolor=col,edgecolor="#ddd",lw=.8,alpha=.85))
        ax.text(x,z,label,ha="center",va="center",fontsize=5.5,color="white")
    xs=[r[0] for r in rs]; zs=[r[1] for r in rs]
    ax.set_xlim(min(x-r[2]/2 for x,r in zip(xs,rs))-3,max(x+r[2]/2 for x,r in zip(xs,rs))+3)
    ax.set_ylim(min(z-r[3]/2 for z,r in zip(zs,rs))-3,max(z+r[3]/2 for z,r in zip(zs,rs))+3)
    ax.set_aspect("equal"); ax.set_title(f"{TITLES.get(g,g)}  ({len(rs)} rooms)",fontsize=11)
    ax.grid(True,alpha=.12)
for j in range(len(order),len(axes)): axes[j].axis("off")
fig.suptitle("ESCAPE LAB 48 — Task9D facility, Floors 2-7 top-down (X east, +Z up)",fontsize=15)
plt.tight_layout(); plt.savefig("C:/GameDev/X3Native-engine/tools/tower_floors_2_7.png",dpi=110)
print("wrote tools/tower_floors_2_7.png")
