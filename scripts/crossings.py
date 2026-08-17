import sys, collections
# find XZ crossings between routes from an X3_ROADNET_DUMP log
routes = collections.OrderedDict()
for line in open(sys.argv[1], encoding='utf-8', errors='ignore'):
    if not line.startswith('DUMP|'): continue
    p = line.strip().split('|')
    nm = p[1]
    routes.setdefault(nm, []).append((float(p[3]), float(p[5]), float(p[4])))  # x,z,y
def seg_int(a,b,c,d):
    r=(b[0]-a[0],b[1]-a[1]); s=(d[0]-c[0],d[1]-c[1])
    den=r[0]*s[1]-r[1]*s[0]
    if abs(den)<1e-9: return None
    t=((c[0]-a[0])*s[1]-(c[1]-a[1])*s[0])/den
    u=((c[0]-a[0])*r[1]-(c[1]-a[1])*r[0])/den
    if 0<=t<=1 and 0<=u<=1: return t,u
    return None
names=list(routes)
for i in range(len(names)):
    for j in range(i+1,len(names)):
        A,B=routes[names[i]],routes[names[j]]
        for k in range(len(A)-1):
            for m in range(len(B)-1):
                a,b=(A[k][0],A[k][1]),(A[k+1][0],A[k+1][1])
                c,d=(B[m][0],B[m][1]),(B[m+1][0],B[m+1][1])
                hit=seg_int(a,b,c,d)
                if hit:
                    t,u=hit
                    ya=A[k][2]+(A[k+1][2]-A[k][2])*t
                    yb=B[m][2]+(B[m+1][2]-B[m][2])*u
                    x=a[0]+(b[0]-a[0])*t; z=a[1]+(b[1]-a[1])*t
                    print("CROSS %s[%d] x %s[%d] at (%.0f, %.0f)  yA %.2f yB %.2f  dy %.2f" %
                          (names[i],k,names[j],m,x,z,ya,yb,ya-yb))
print("done", {n: len(v) for n,v in routes.items()})
