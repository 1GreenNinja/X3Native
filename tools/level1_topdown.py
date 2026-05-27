"""Top-down PNG of Level 1 (B1 Detention) from the real level1.cpp kDetention[] data.
Overlays the CURRENT gameplay beats (red, z=0) vs a PROPOSED re-aimed route down the
Main Hallway through the real rooms (green). X=east, +Z=north. -> tools/level1_topdown.png"""
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt, matplotlib.patches as mp

# name, cx, cz, w(x), d(z), tag(m=monster,n=sarah,h=hall,a=medical/armory,.=normal)
ROOMS = [
 ("Jake's Cell",0,0,7,6,"."),("Cell 2",0,-8,6,5,"."),("Cell 3",0,-15,6,5,"m"),("Cell 4",0,-22,6,5,"."),
 ("MAIN HALLWAY",5.5,-12,3,26,"h"),("Guard Stn",11,-2,5,5,"."),("Storage",11,-9,5,5,"."),
 ("MEDICAL BAY",11,-16,5,5,"a"),("ARMORY",11,-23,5,5,"a"),("Elevator Lobby",5.5,-27,5,4,"."),
 ("Adjacent Cell",5.5,5,5,4,"."),("Old Armory",-1,7,7,5,"."),("Creepy Pass",16,-2,4,3,"."),
 ("Cell 5",-20,-4,6,5,"."),("Cell 6",-20,-11,6,5,"m"),("Cell 7",-20,-18,6,5,"."),
 ("Cell 8",-20,-25,6,5,"."),("Cell 9",-20,-32,6,5,"."),("Sarah (Cell10)",-7,-4,6,5,"n"),
 ("Cell 11",-7,-11,6,5,"m"),("Cell 12",-7,-18,6,5,"."),("Cell 13",-7,-25,6,5,"m"),
 ("Cell 14",-7,-32,6,5,"."),("Cell Blk B Hall",-13.5,-18,4,32,"h"),("CB S Connector",-13.5,-36,14,3,"h"),
 ("Desc Stairs",20,-2,4,3,"."),("Cave Tunnel",27,-2,10,3,"."),("Crystal Cavern",41,-2,18,16,"."),
 ("Side Grotto",55,1,8,8,"."),
]
DOORS=[(0,4),(1,4),(2,4),(3,4),(4,5),(4,6),(4,7),(4,8),(4,9),(4,10),(0,18),(1,18),(1,19),(2,19),
 (2,20),(3,20),(3,21),(5,12),(12,25),(25,26),(26,27),(27,28),(0,11),(10,11),(13,23),(14,23),
 (15,23),(16,23),(17,23),(18,23),(19,23),(20,23),(21,23),(22,23),(17,24),(22,24),(23,24)]
CURRENT=[("spawn",1.5,0),("cell",3,0),("corridor",7,0),("armory?",11,0),("checkpoint",13.7,0)]
PROPOSED=[("spawn",1.5,0),("enter hall",5.5,-1),("Guard",10,-2),("Storage",10,-9),
          ("MEDICAL",10,-16),("ARMORY",10,-23),("Elevator",5.5,-27)]
COL={"m":"#7a2222","n":"#1f6f3f","h":"#4a4f12","a":"#155e6e",".":"#2b3a55"}

fig,ax=plt.subplots(figsize=(17,12))
for i,(nm,cx,cz,w,d,t) in enumerate(ROOMS):
    ax.add_patch(mp.Rectangle((cx-w/2,cz-d/2),w,d,facecolor=COL[t],edgecolor="#cfcfcf",lw=1.1,alpha=.92))
    ax.text(cx,cz,nm,ha="center",va="center",fontsize=7.5,color="white",fontweight="bold" if t in"ha" else "normal")
for a,b in DOORS:
    ax.plot([ROOMS[a][1],ROOMS[b][1]],[ROOMS[a][2],ROOMS[b][2]],color="#999",lw=.7,alpha=.45,zorder=1)
cx_,cz_=[p[1] for p in CURRENT],[p[2] for p in CURRENT]
ax.plot(cx_,cz_,color="#ff2e2e",lw=3.5,marker="o",ms=11,zorder=6,label="CURRENT beats (cramped z=0 strip)")
px_,pz_=[p[1] for p in PROPOSED],[p[2] for p in PROPOSED]
ax.plot(px_,pz_,color="#23d04a",lw=3,ls="--",marker="s",ms=9,zorder=6,label="PROPOSED: down the hall thru real rooms")
for nm,x,z in CURRENT: ax.text(x,z+1.4,nm,ha="center",fontsize=8,color="#ff5a5a",fontweight="bold",zorder=7)
ax.set_aspect("equal"); ax.set_xlim(-26,60); ax.set_ylim(-40,12)  # +Z (north) up
ax.set_title("Level 1 B1 Detention — TOP DOWN (real geometry)\n"
  "RED = current gameplay (only touches Jake/hall-tip/Guard) | GREEN = proposed re-aim down the Main Hallway\n"
  "Real ARMORY & MEDICAL (teal) sit 16-23m south and are never visited. Hall=3m wide; rooms=5m deep.",fontsize=11)
ax.set_xlabel("X east (m)"); ax.set_ylabel("Z north-up (m)")
ax.legend(loc="lower right",fontsize=11); ax.grid(True,alpha=.12)
plt.tight_layout(); plt.savefig("tools/level1_topdown.png",dpi=120)
print("wrote tools/level1_topdown.png")
