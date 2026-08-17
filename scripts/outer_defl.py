import math
# reproduce makeOuterTour polyline (pre-smoothing) and find worst deflections
CX, CZ = -592.0, -352.0
SP = 61.0
tour = [
    (0.0,7934,0),(20.0,7934,0),(40.0,8800,0),(55.0,7900,0),(58.0,7600,0),
    (72.3,7600,1),(85.75,7600,0),(87.7,7600,1),(111.5,7600,0),(114.0,7600,0),
    (124.0,7400,0),(136.0,7050,0),(148.0,6800,0),(155.3,6800,1),(167.7,6800,0),
    (171.6,6800,1),(177.9,6800,0),(179.8,6800,1),(203.4,6800,0),(208.0,6800,0),
    (220.0,8000,0),(232.0,8600,0),(243.0,7800,0),(248.0,7600,0),(271.4,7600,0),
    (273.2,7934,0),(283.0,7934,0),(296.5,7934,0),(310.0,8100,0),(330.0,7934,0),
    (360.0,7934,0),
]
X,Z,tag = [],[],[]
def pt(a,r):
    a=math.radians(a); return CX+math.cos(a)*r, CZ+math.sin(a)*r
for w in range(len(tour)-1):
    A,B = tour[w], tour[w+1]
    ax,az = pt(A[0],A[1]); bx,bz = pt(B[0],B[1])
    if A[2]:
        L = math.hypot(bx-ax,bz-az); n = max(1,math.ceil(L/SP))
        for k in range(n):
            t=k/n; X.append(ax+(bx-ax)*t); Z.append(az+(bz-az)*t); tag.append('chord' if k>0 else 'portal')
    else:
        a0,a1 = math.radians(A[0]), math.radians(B[0])
        arc=(a1-a0)*0.5*(A[1]+B[1]); n=max(1,math.ceil(arc/SP))
        for k in range(n):
            t=k/n; e=t*t*(3-2*t); ang=a0+(a1-a0)*t; rad=A[1]+(B[1]-A[1])*e
            X.append(CX+math.cos(ang)*rad); Z.append(CZ+math.sin(ang)*rad); tag.append('arc')
X.append(X[0]); Z.append(Z[0]); tag.append('dup')
worst=[]
for i in range(1,len(X)-1):
    ax,az=X[i]-X[i-1],Z[i]-Z[i-1]; bx,bz=X[i+1]-X[i],Z[i+1]-Z[i]
    la=math.hypot(ax,az); lb=math.hypot(bx,bz)
    if la<1e-4 or lb<1e-4: continue
    d=max(-1,min(1,(ax*bx+az*bz)/(la*lb)))
    deg=math.degrees(math.acos(d))
    ang=(math.degrees(math.atan2(Z[i]-CZ,X[i]-CX))+360)%360
    worst.append((deg,i,ang,tag[i],la,lb))
worst.sort(reverse=True)
for w in worst[:15]:
    print("defl %6.2f deg at node %4d  theta %7.2f  tag %-6s  segs %.0f/%.0f" % w)
