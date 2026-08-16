# W-ROADS connectivity audit: pure-XZ min distances between authored route polylines.
# Spawn corridor = the demo ridge tunnel route (straight, centre (-592,-352),
# heading 157.5 deg, halfLen 320) vs the inner tour circle, the outer tour table,
# and the valley/river road waypoints. No terrain needed: distances are planform.
import math

C = (-592.0, -352.0)
d = (-0.92388, 0.38268)
A = (C[0]-d[0]*320, C[1]-d[1]*320)   # node 0 (spawn side)
B = (C[0]+d[0]*320, C[1]+d[1]*320)   # far end (past exit portal)
spawn = [(A[0]+(B[0]-A[0])*t/64, A[1]+(B[1]-A[1])*t/64) for t in range(65)]

def seg_pt_dist(p,a,b):
    ax,az=a; bx,bz=b; px,pz=p
    dx,dz=bx-ax,bz-az; L2=dx*dx+dz*dz
    if L2<1e-9: return math.hypot(px-ax,pz-az)
    t=max(0.0,min(1.0,((px-ax)*dx+(pz-az)*dz)/L2))
    return math.hypot(px-(ax+dx*t), pz-(az+dz*t))

def poly_poly_dist(P,Q):
    best=(1e18,None,None)
    for i in range(len(P)-1):
        for q in Q:
            dd=seg_pt_dist(q,P[i],P[i+1])
            if dd<best[0]: best=(dd,P[i],q)
    for i in range(len(Q)-1):
        for p in P:
            dd=seg_pt_dist(p,Q[i],Q[i+1])
            if dd<best[0]: best=(dd,p,Q[i])
    return best

ring=[(C[0]+3842*math.cos(2*math.pi*i/396), C[1]+3842*math.sin(2*math.pi*i/396)) for i in range(397)]

tour_pts=[(0,7934),(20,7934),(40,8800),(55,7900),(58,7600),(72.3,7600),(85.75,7600),(87.7,7600),
(111.5,7600),(114,7600),(124,7400),(136,7050),(148,6800),(155.3,6800),(167.7,6800),(171.6,6800),
(177.9,6800),(179.8,6800),(203.4,6800),(208,6800),(220,8000),(232,8600),(243,7800),(248,7600),
(271.4,7600),(273.2,7934),(283,7934),(296.5,7934),(310,8100),(330,7934),(360,7934)]
outer=[]
for i in range(len(tour_pts)-1):
    a0,r0=tour_pts[i]; a1,r1=tour_pts[i+1]
    n=max(2,int(abs(a1-a0)*4))
    for k in range(n):
        t=k/n; e=t*t*(3-2*t)
        ang=math.radians(a0+(a1-a0)*t); r=r0+(r1-r0)*e
        outer.append((C[0]+r*math.cos(ang), C[1]+r*math.sin(ang)))
outer.append(outer[0])

west=[(75.2,-4135.6),(330.0,-1650.0),(330.0,-1150.0),(352.0,-905.0)]
east=[(1000.0,-560.0),(1600.0,-620.0),(2400.0,-760.0),(3185.6,-1052.1)]
river=west+east

for name,Q in [("inner ring",ring),("outer tour",outer),("river road",river)]:
    dd,p,q=poly_poly_dist(spawn,Q)
    print(f"spawn corridor -> {name}: min distance {dd:8.1f} m ({dd*3.28084:7.0f} ft) near spawn ({p[0]:.0f},{p[1]:.0f}) / route ({q[0]:.0f},{q[1]:.0f})")

for i in [172,173,174]:
    ang=2*math.pi*i/396
    P=(C[0]+3842*math.cos(ang), C[1]+3842*math.sin(ang))
    print(f"ring node {i}: angle {math.degrees(ang):7.3f} deg pos ({P[0]:.1f},{P[1]:.1f}) dist from far end B {math.hypot(P[0]-B[0],P[1]-B[1]):.1f} m")
print("far end B:", B, " node0 A:", A)
dd,_,_=poly_poly_dist(ring,river); print("inner ring <-> river road:", round(dd,1),"m")
dd,_,_=poly_poly_dist(ring,outer); print("inner ring <-> outer tour:", round(dd,1),"m")
