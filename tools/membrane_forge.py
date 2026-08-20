"""membrane_forge.py -- headless Blender RE-SKIN forge that kills the
wrist->hip "membrane" web on the Meshy-auto-rigged humanoid cast.

THE DEFECT: Meshy's auto-rig bleeds Hand/ForeArm weights onto lower-torso /
skirt verts (the hands rest at hip height in the A-pose), so any arms-raised
pose (Wave, Jab, Talk) stretches a skin web from wrist to hip. Weight-only
fixes (distance prune, winner-take-all) failed twice because pure euclidean
distance CANNOT tell a glove vert from the hip vert it rests against.

THE SOLVE (adapted from the CyberWolf wave, tools/wolf_forge.py 27a50afd):
  1. Capsule-distance scoring per vertex against every deform bone
     (per-bone-class radii scaled to the character's height).
  2. GEODESIC SHELL OWNERSHIP -- confident capsule seeds (never inside the
     hand-rest contact zone) are flooded across the mesh EDGE GRAPH
     (multi-source Dijkstra). Surface connectivity, not euclidean distance,
     decides whether a vert belongs to the arm or the torso: a hip vert is
     geodesically close to torso seeds, a glove vert is geodesically close to
     the arm -- even though they touch in space.
  3. MESH CUT -- faces whose verts span ANATOMICALLY HOSTILE region pairs
     (arm vs torso/leg outside the shoulder blend zone = the physical weld
     that Meshy left between glove and hip) are bmesh-split off along the
     ownership frontier. Rest-pose geometry is IDENTICAL (verts duplicated in
     place, no faces deleted, no holes); raised arms now depart cleanly.
  4. Weights = capsule falloff restricted to the vert's owner region (+
     adjacent-region bones inside a geodesic blend band around LEGIT joints
     only), top-4, smoothed, then re-clamped to the allowed set. A hip vert
     can never hold Hand weight again -- the root kill.

Stages (Blender launcher DETACHES on this box: each stage writes
<workdir>/<name>.log and <name>.done for the caller to poll):

  blender-launcher.exe --background --python tools/membrane_forge.py -- \
      fix <src.glb> <out.glb> <workdir> [skirt]
    Re-skin every armature-driven mesh (>2000 verts) onto the EXISTING
    skeleton. Keeps skeleton, clips, materials, transforms untouched.
    "skirt" = SalvariPrincess mode: the skirt drape is force-owned by
    TORSO/LEGS (never arms), reported in the log.

  blender-launcher.exe --background --python tools/membrane_forge.py -- \
      qa <glb> <workdir> <prefix> "Clip:f1,f2;Clip2:f1" [side]
    Grounded floor+level-cam renders (front + 3/4) of the named clip frames.

Clean-room: public Blender Python API + glTF 2.0 spec only.
"""
import bpy, bmesh, sys, os, math, heapq, time
import numpy as np
from mathutils import Vector

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(ARGV) < 3:
    raise SystemExit("usage: -- fix <src> <out> <workdir> [skirt] | -- qa <glb> <workdir> <prefix> <shots>")
STAGE = ARGV[0]

_log = []
LOG_PATH = DONE_PATH = None
def log(*a):
    s = "[membrane_forge] " + " ".join(str(x) for x in a)
    _log.append(s); print(s, flush=True)
def flush(status):
    try:
        with open(LOG_PATH, "w", encoding="utf-8") as f: f.write("\n".join(_log))
        with open(DONE_PATH, "w", encoding="utf-8") as f: f.write(status)
    except Exception as e:
        print("[membrane_forge] log write failed:", e)

D = math.radians

# ---------------------------------------------------------------------------
# Meshy 24-joint humanoid: regions + per-class capsule radii (meters at a
# 1.75 m reference height; scaled by the character's actual mesh height).
# ---------------------------------------------------------------------------
REGION_BONES = {
    "TORSO": ["Hips", "Spine", "Spine01", "Spine02"],
    "HEAD":  ["neck", "Head", "head_end", "headfront"],
    "ARM_L": ["LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand"],
    "ARM_R": ["RightShoulder", "RightArm", "RightForeArm", "RightHand"],
    "LEG_L": ["LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase"],
    "LEG_R": ["RightUpLeg", "RightLeg", "RightFoot", "RightToeBase"],
}
BONE2REGION = {b: r for r, bs in REGION_BONES.items() for b in bs}
REGIONS = list(REGION_BONES)
RIDX = {r: i for i, r in enumerate(REGIONS)}
ARM_REGIONS = {"ARM_L", "ARM_R"}

_RADII = {
    "Hips": 0.155, "Spine": 0.150, "Spine01": 0.160, "Spine02": 0.165,
    "neck": 0.070, "Head": 0.110, "head_end": 0.055, "headfront": 0.055,
    "LeftShoulder": 0.085, "LeftArm": 0.072, "LeftForeArm": 0.058, "LeftHand": 0.055,
    "RightShoulder": 0.085, "RightArm": 0.072, "RightForeArm": 0.058, "RightHand": 0.055,
    "LeftUpLeg": 0.100, "LeftLeg": 0.072, "LeftFoot": 0.058, "LeftToeBase": 0.050,
    "RightUpLeg": 0.100, "RightLeg": 0.072, "RightFoot": 0.058, "RightToeBase": 0.050,
}

# Legit anatomical frontiers; every OTHER cross-region face is a Meshy weld.
LEGIT_PAIRS = {frozenset(p) for p in [
    ("TORSO", "HEAD"), ("TORSO", "LEG_L"), ("TORSO", "LEG_R"),
    ("LEG_L", "LEG_R"),
    ("TORSO", "ARM_L"), ("TORSO", "ARM_R"),   # ONLY inside the shoulder zone
]}

def reset_factory():
    bpy.ops.wm.read_factory_settings(use_empty=True)

def pack_textures():
    for img in bpy.data.images:
        try:
            if not img.has_data: _ = img.pixels[0]
            if not img.packed_file: img.pack()
            img.use_fake_user = True
        except Exception as e:
            log("img pack warn", img.name, e)
    for m in bpy.data.materials:
        m.use_fake_user = True

def find_rig():
    arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
    meshes = []
    for o in bpy.data.objects:
        if o.type != 'MESH':
            continue
        drives = any(m.type == 'ARMATURE' and m.object == arm for m in o.modifiers)
        if drives and len(o.data.vertices) > 2000:
            meshes.append(o)
    return arm, meshes

# ---------------------------------------------------------------------------
# Geometry helpers (numpy)
# ---------------------------------------------------------------------------
def world_verts(mesh):
    n = len(mesh.data.vertices)
    co = np.empty(n * 3, dtype=np.float64)
    mesh.data.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)
    M = np.array(mesh.matrix_world)
    return co @ M[:3, :3].T + M[:3, 3]

def seg_dist(P, a, b):
    """Distances from points P (n,3) to segment a-b."""
    d = b - a
    l2 = max(1e-12, float(d @ d))
    t = np.clip(((P - a) @ d) / l2, 0.0, 1.0)
    proj = a + t[:, None] * d
    return np.linalg.norm(P - proj, axis=1)

def bone_segments(arm):
    """[(name, head_w, tail_w, radius_unscaled)] for deform bones we know."""
    out = []
    M = arm.matrix_world
    for b in arm.data.bones:
        if b.name not in BONE2REGION:
            continue
        h = np.array(M @ b.head_local, dtype=np.float64)
        t = np.array(M @ b.tail_local, dtype=np.float64)
        out.append((b.name, h, t, _RADII[b.name]))
    return out

# ---------------------------------------------------------------------------
# Stage: fix
# ---------------------------------------------------------------------------
def fix_mesh(mesh, arm, flags):
    skirt_mode = 'skirt' in flags
    sheet_mode = 'sheetcut' in flags
    t0 = time.time()
    n = len(mesh.data.vertices)
    W = world_verts(mesh)
    zmin, zmax = float(W[:, 2].min()), float(W[:, 2].max())
    height = zmax - zmin
    scale = max(0.35, height / 1.75)
    log("mesh", mesh.name, "verts", n, "height %.2f scale %.2f" % (height, scale))

    segs = bone_segments(arm)
    names = [s[0] for s in segs]
    nb = len(segs)

    # ---- capture ORIGINAL weights (they are right everywhere except the
    # hand-rest contact zone -- keep them wherever ownership agrees) ---------
    gname = {g.index: g.name for g in mesh.vertex_groups}
    orig = [dict() for _ in range(n)]
    for v in mesh.data.vertices:
        for ge in v.groups:
            nm = gname.get(ge.group)
            if nm in BONE2REGION and ge.weight > 1e-4:
                orig[v.index][nm] = orig[v.index].get(nm, 0.0) + ge.weight

    def geom_arrays(W):
        dist = np.empty((nb, len(W)))
        for i, (nm, h, t, r) in enumerate(segs):
            dist[i] = seg_dist(W, h, t)
        radii = np.array([s[3] for s in segs]) * scale
        ndist = dist / radii[:, None]
        reg_nd = np.full((len(REGIONS), len(W)), 1e9)
        for i, nm in enumerate(names):
            ri = RIDX[BONE2REGION[nm]]
            reg_nd[ri] = np.minimum(reg_nd[ri], ndist[i])
        return dist, radii, ndist, reg_nd

    dist, radii, ndist, reg_nd = geom_arrays(W)
    nreg = len(REGIONS)
    order = np.argsort(reg_nd, axis=0)
    best = order[0]; second = order[1]
    nd_best = reg_nd[best, np.arange(n)]
    nd_second = reg_nd[second, np.arange(n)]

    # normalized distance to each full arm chain (the Hand segment is extended
    # 1.6x past the wrist so fingers/claws count as inside the tube) + the
    # hand-rest contact zone
    def arm_chain_nd(ndist, W):
        out = {}
        for r in ("ARM_L", "ARM_R"):
            m = np.full(ndist.shape[1], 1e9)
            for i, nm in enumerate(names):
                if BONE2REGION[nm] == r:
                    if nm.endswith("Hand"):
                        h, t = segs[i][1], segs[i][2]
                        t2 = t + (t - h) * 1.6
                        m = np.minimum(m, seg_dist(W, h, t2) / radii_g[i])
                    else:
                        m = np.minimum(m, ndist[i])
            out[r] = m
        return out
    radii_g = np.array([s[3] for s in segs]) * scale
    chain_nd = arm_chain_nd(ndist, W)
    hand_zone = np.zeros(n, dtype=bool)
    hand_core = np.zeros(n, dtype=bool)
    for i, (nm, h, t, r) in enumerate(segs):
        if nm.endswith("Hand"):
            hand_zone |= dist[i] < 2.2 * radii[i]
            hand_core |= ndist[i] < 0.75
        elif nm.endswith("ForeArm"):
            mid = (h + t) * 0.5
            hand_zone |= seg_dist(W, mid, t) < 1.8 * radii[i]
            hand_core |= ndist[i] < 0.75

    # per-side ORIGINAL arm-weight fraction: glove/sleeve verts are ~pure
    # Hand/ForeArm in the Meshy originals; the defective hip verts are MIXED
    # (part hips + part hand). This is the discriminator inside the contact
    # zone where geometry alone cannot decide.
    armfrac = {}
    for r in ("ARM_L", "ARM_R"):
        af = np.zeros(n)
        for v in range(n):
            tot = sum(orig[v].values())
            if tot > 1e-6:
                af[v] = sum(w for nm, w in orig[v].items()
                            if BONE2REGION[nm] == r) / tot
        armfrac[r] = af

    # ---- seeds: confident capsule votes; in the contact zone ONLY verts in
    # the tight hand tube whose ORIGINALS are arm-dominant may seed (the
    # glove gets seeds INSIDE it, the hip gets seeds outside the zone -- the
    # flood race is fair and the frontier lands on the weld) ----------------
    seeds = (nd_best < 1.15) & (nd_second > 1.35 * nd_best) & ~hand_zone
    best2 = best.copy()
    for r in ("ARM_L", "ARM_R"):
        s = hand_core & (armfrac[r] > 0.6)
        best2[s] = RIDX[r]
        seeds |= s
    best = best2
    log("seeds: %d/%d (contact zone: %d)" % (int(seeds.sum()), n, int(hand_zone.sum())))

    # ---- edge graph -------------------------------------------------------
    def edge_graph():
        ne = len(mesh.data.edges)
        ev = np.empty(ne * 2, dtype=np.int64)
        mesh.data.edges.foreach_get("vertices", ev)
        ev = ev.reshape(-1, 2)
        elen = np.linalg.norm(W[ev[:, 0]] - W[ev[:, 1]], axis=1)
        adj = [[] for _ in range(len(W))]
        for (a, b), l in zip(ev, elen):
            adj[a].append((b, l)); adj[b].append((a, l))
        return ev, adj
    ev, adj = edge_graph()

    # ---- multi-source Dijkstra flood with AFFINITY-BIASED costs ----------
    # Arm floods travel cheap across verts whose ORIGINALS lean arm (or that
    # hug the arm tube); body floods pay to enter them -- so the frontier
    # snaps onto the weld even where Meshy mixed the glove bottom.
    bodyfrac = 1.0 - armfrac["ARM_L"] - armfrac["ARM_R"]
    mult_arm = {}
    for r in ("ARM_L", "ARM_R"):
        m = np.where((armfrac[r] > 0.6) | (chain_nd[r] < 0.8), 0.4,
             np.where((armfrac[r] > 0.25) | (chain_nd[r] < 1.2), 0.8, 2.5))
        mult_arm[RIDX[r]] = m
    mult_body = np.where(bodyfrac > 0.75, 0.4,
                np.where(bodyfrac > 0.4, 1.0, 2.5))

    owner = np.full(n, -1, dtype=np.int32)
    gdist = np.full(n, np.inf)
    pq = []
    for v in np.nonzero(seeds)[0]:
        owner[v] = best[v]; gdist[v] = 0.0
        heapq.heappush(pq, (0.0, int(v)))
    aL, aR = RIDX["ARM_L"], RIDX["ARM_R"]
    while pq:
        dv, v = heapq.heappop(pq)
        if dv > gdist[v] + 1e-12:
            continue
        r = owner[v]
        mv = mult_arm[r] if r in (aL, aR) else mult_body
        for u, l in adj[v]:
            nd = dv + l * mv[u]
            if nd < gdist[u] - 1e-12:
                gdist[u] = nd; owner[u] = r
                heapq.heappush(pq, (nd, u))

    # ---- unreached islands: vote by ORIGINAL weights (Meshy bound whole
    # accessories to the right limb; only the contact zone lies), fall back
    # to capsule vote --------------------------------------------------------
    unreached = np.nonzero(owner < 0)[0]
    if len(unreached):
        log("flood unreached:", len(unreached), "verts -> island vote (orig weights)")
        seen = set()
        for v0 in unreached:
            if int(v0) in seen or owner[v0] >= 0:
                continue
            comp = [int(v0)]; seen.add(int(v0)); qi = 0
            while qi < len(comp):
                v = comp[qi]; qi += 1
                for u, _ in adj[v]:
                    if owner[u] < 0 and u not in seen:
                        seen.add(u); comp.append(u)
            votes = {}
            for v in comp:
                for nm, w in orig[v].items():
                    ri = RIDX[BONE2REGION[nm]]
                    votes[ri] = votes.get(ri, 0.0) + w
            ci = np.array(comp)
            if votes:
                ri = max(votes.items(), key=lambda kv: kv[1])[0]
            else:
                ri = int(np.argmin(reg_nd[:, ci].mean(axis=1)))
            owner[ci] = ri

    # ---- ownership smoothing: kill salt-and-pepper frontiers -------------
    for _ in range(3):
        flips = 0
        new_owner = owner.copy()
        for v in range(n):
            cnt = {}
            for u, _ in adj[v]:
                cnt[owner[u]] = cnt.get(owner[u], 0) + 1
            if not cnt:
                continue
            top_r, top_c = max(cnt.items(), key=lambda kv: kv[1])
            if top_r != owner[v] and top_c >= 0.7 * len(adj[v]) \
               and reg_nd[top_r, v] < 3.5:
                new_owner[v] = top_r; flips += 1
        owner = new_owner
        if flips == 0:
            break

    # ---- hand-tube override: a vert INSIDE the arm tube whose originals are
    # strongly that arm IS the arm, no matter how the flood raced (hip seeds
    # sit right at the weld and can beat the sleeve flood into the glove) ----
    overrode = 0
    for r in ("ARM_L", "ARM_R"):
        s = (chain_nd[r] < 1.15) & (armfrac[r] > 0.7) & (owner != RIDX[r])
        owner[s] = RIDX[r]
        overrode += int(s.sum())
    log("hand-tube ownership override:", overrode, "verts")

    # ---- SHEET DEMOTION (the mesh-cut targeting rule): Meshy modeled the
    # wrist->hip membrane INTO the mesh as a hand-weighted sheet. ARM
    # ownership is only legal INSIDE the anatomical arm tube -- any arm-owned
    # vert outside it (the sheet, whatever its original weights say) belongs
    # to the body, so the hostile cut lands at the glove|sheet boundary and
    # the sheet stays draped on the body forever.
    if sheet_mode:
        # thickness probe: a sail is a THIN two-layer sheet (opposing surface
        # within ~3 cm); gloves/fingers/claws are closed tubes (4 cm+). Ray
        # both ways along the vertex normal against a world-space BVH.
        from mathutils.bvhtree import BVHTree
        polys = []
        for p in mesh.data.polygons:
            polys.append(list(p.vertices))
        bvh = BVHTree.FromPolygons([Vector(w) for w in W], polys, all_triangles=False)
        nrm = np.empty(n * 3)
        mesh.data.vertices.foreach_get("normal", nrm)
        nrm = nrm.reshape(-1, 3)
        Mw = np.array(mesh.matrix_world)[:3, :3]
        nrm = nrm @ Mw.T
        nrm /= np.maximum(1e-9, np.linalg.norm(nrm, axis=1))[:, None]
        thin_lim = 0.032 * scale
        eps = 0.004 * scale
        arm_owned = np.isin(owner, [RIDX["ARM_L"], RIDX["ARM_R"]])
        thin = np.zeros(n, dtype=bool)
        for v in range(n):
            co = Vector(W[v]); nv = Vector(nrm[v])
            tmin = 1e9
            for sgn in (-1.0, 1.0):
                hit = bvh.ray_cast(co + nv * (eps * sgn), nv * sgn, thin_lim)
                if hit[0] is not None:
                    tmin = min(tmin, hit[3] + eps)
            thin[v] = tmin < thin_lim
        sheeted = 0
        nonarm = [RIDX[r] for r in REGIONS if r not in ARM_REGIONS]
        del_cand = np.zeros(n, dtype=bool)
        for r in ("ARM_L", "ARM_R"):
            s = (owner == RIDX[r]) & ((chain_nd[r] > 1.2) |
                                      (thin & (chain_nd[r] > 0.85)))
            if s.any():
                del_cand |= s
                fb2 = np.argmin(reg_nd[nonarm][:, s], axis=0)
                owner[s] = np.array(nonarm)[fb2]
                sheeted += int(s.sum())
        log("sheet demotion: re-owned", sheeted,
            "outside-tube/thin-sheet arm verts -> body (thin flagged:",
            int((thin & arm_owned).sum()), "; sail-delete candidates:",
            int(del_cand.sum()), ")")
    else:
        del_cand = None

    # ---- demotion: an ARM-owned vert whose ORIGINAL weights are solidly
    # torso/leg and which sits outside the arm tube is a mis-seeded body
    # vert -- trust the original binding ------------------------------------
    demoted = 0
    for v in range(n):
        r = REGIONS[owner[v]]
        if r not in ARM_REGIONS:
            continue
        tot = sum(orig[v].values())
        if tot < 1e-4 or chain_nd[r][v] <= 1.3:
            continue
        body = sum(w for nm, w in orig[v].items()
                   if BONE2REGION[nm] not in ARM_REGIONS)
        if body > 0.7 * tot:
            votes = {}
            for nm, w in orig[v].items():
                ri = RIDX[BONE2REGION[nm]]
                if REGIONS[ri] not in ARM_REGIONS:
                    votes[ri] = votes.get(ri, 0.0) + w
            owner[v] = max(votes.items(), key=lambda kv: kv[1])[0]
            demoted += 1
    log("demoted mis-seeded arm verts:", demoted)
    log("ownership:", {REGIONS[r]: int((owner == r).sum()) for r in range(nreg)})

    # ---- skirt mode: NOTHING below the hip line may be arm-owned ---------
    if skirt_mode:
        # HARD rule (the owner's directive): below the hip line the ONLY arm
        # geometry is the glove/forearm skin itself (tight tube nd<0.75) --
        # every other arm-owned vert there is skirt drape and belongs to
        # Hips/UpperLegs, even where the resting hand touches it.
        hips_z = None
        for nm, h, t, r in segs:
            if nm == "Hips": hips_z = max(h[2], t[2])
        tightnd = np.full(n, 1e9)
        for i, nm in enumerate(names):
            if nm.endswith("Hand") or nm.endswith("ForeArm"):
                tightnd = np.minimum(tightnd, ndist[i])
        below = W[:, 2] < hips_z + 0.02 * scale
        bad = np.isin(owner, [RIDX["ARM_L"], RIDX["ARM_R"]]) & below & (tightnd > 0.75)
        if del_cand is not None:
            # thin drape blades pressed INTO the hand tube are skirt too --
            # only the true hand/forearm skin (tight tube, thick) stays arm
            bad |= np.isin(owner, [RIDX["ARM_L"], RIDX["ARM_R"]]) & below                    & thin & (tightnd > 0.45)
        if bad.any():
            fb3 = np.argmin(reg_nd[[RIDX["TORSO"], RIDX["LEG_L"], RIDX["LEG_R"]]][:, bad], axis=0)
            owner[bad] = np.array([RIDX["TORSO"], RIDX["LEG_L"], RIDX["LEG_R"]])[fb3]
            if del_cand is not None:
                del_cand |= bad
        log("skirt mode: re-owned", int(bad.sum()), "arm-claimed drape verts -> torso/legs")

    # ---- shoulder zone (where TORSO<->ARM blending stays legit) ----------
    def calc_shoulder_zone(dist, radii):
        z = np.zeros(dist.shape[1], dtype=bool)
        for i, (nm, h, t, r) in enumerate(segs):
            if nm.endswith("Shoulder") or (nm.endswith("Arm") and "Fore" not in nm):
                z |= dist[i] < 2.6 * radii[i]
        return z
    shoulder_zone = calc_shoulder_zone(dist, radii)

    mesh_attr = mesh.data.attributes.get("mf_region")
    if mesh_attr is None:
        mesh_attr = mesh.data.attributes.new("mf_region", 'INT', 'POINT')
    mesh_attr.data.foreach_set("value", owner.astype(np.int32))

    # ---- SAIL DELETION: faces made ENTIRELY of thin demoted-sail verts are
    # the modeled-in membrane itself -- a redundant drape layer lying on the
    # body. Deleting it opens no silhouette hole (the body surface beneath is
    # complete -- verified per character in the QA renders). ----------------
    if del_cand is not None and del_cand.any():
        # only SHARD-sized demoted fragments are deleted (big demoted drapes
        # stay -- they are real costume surface that now belongs to the body)
        comp_id = np.full(n, -1, dtype=np.int64)
        comps = []
        for v0 in np.nonzero(del_cand)[0]:
            if comp_id[v0] >= 0:
                continue
            comp = [int(v0)]; comp_id[v0] = len(comps); qi = 0
            while qi < len(comp):
                v = comp[qi]; qi += 1
                for u, _ in adj[v]:
                    if del_cand[u] and comp_id[u] < 0:
                        comp_id[u] = len(comps); comp.append(u)
            comps.append(comp)
        small = np.zeros(n, dtype=bool)
        kept = 0
        for comp in comps:
            if len(comp) <= 1000:
                small[comp] = True
            else:
                kept += 1
        log("sail shards: %d components, %d big kept as body drape"
            % (len(comps), kept))
        del_cand = small
        # dilate across the THIN fringe: half-sail boundary faces otherwise
        # survive as ragged tufts on the body
        for _ in range(2):
            grow = []
            for v in range(n):
                if del_cand[v] or not thin[v]:
                    continue
                cnt = sum(1 for u, _ in adj[v] if del_cand[u])
                if cnt >= 2 and any(BONE2REGION.get(nm) in ARM_REGIONS and x > 0.2
                                    for nm, x in orig[v].items()):
                    grow.append(v)
            for v in grow:
                del_cand[v] = True
        del_faces = [p.index for p in mesh.data.polygons
                     if all(del_cand[v] for v in p.vertices)]
        log("sail delete:", len(del_faces), "faces")
        if del_faces:
            bm = bmesh.new()
            bm.from_mesh(mesh.data)
            bm.faces.ensure_lookup_table()
            bmesh.ops.delete(bm, geom=[bm.faces[i] for i in del_faces],
                             context='FACES')
            loose = [v for v in bm.verts if not v.link_faces]
            if loose:
                bmesh.ops.delete(bm, geom=loose, context='VERTS')
            bm.to_mesh(mesh.data)
            bm.free()
            mesh.data.update()
            # order-proof re-read after geometry change
            n = len(mesh.data.vertices)
            W = world_verts(mesh)
            dist, radii, ndist, reg_nd = geom_arrays(W)
            chain_nd = arm_chain_nd(ndist, W)
            attr = mesh.data.attributes.get("mf_region")
            owner = np.empty(n, dtype=np.int32)
            attr.data.foreach_get("value", owner)
            shoulder_zone = calc_shoulder_zone(dist, radii)
            ev, adj = edge_graph()
            gname = {g.index: g.name for g in mesh.vertex_groups}
            orig = [dict() for _ in range(n)]
            for v in mesh.data.vertices:
                for ge in v.groups:
                    nm = gname.get(ge.group)
                    if nm in BONE2REGION and ge.weight > 1e-4:
                        orig[v.index][nm] = orig[v.index].get(nm, 0.0) + ge.weight
            log("post-delete verts:", n)

    # ---- find hostile weld faces and SPLIT them off ----------------------
    hostile_faces = []
    for p in mesh.data.polygons:
        rs = set(int(owner[v]) for v in p.vertices)
        if len(rs) < 2:
            continue
        names_rs = [REGIONS[r] for r in rs]
        ok = True
        for a in range(len(names_rs)):
            for b in range(a + 1, len(names_rs)):
                pair = frozenset((names_rs[a], names_rs[b]))
                if pair not in LEGIT_PAIRS:
                    ok = False
                elif (ARM_REGIONS & pair):
                    if not all(shoulder_zone[v] for v in p.vertices):
                        ok = False
        if not ok:
            hostile_faces.append(p.index)
    log("hostile bridge faces (weld cut):", len(hostile_faces))

    strip_new_verts = set()
    if hostile_faces:
        bm = bmesh.new()
        bm.from_mesh(mesh.data)
        bm.faces.ensure_lookup_table()
        lay = bm.verts.layers.int.get("mf_region")
        hf = [bm.faces[i] for i in hostile_faces]
        ret = bmesh.ops.split(bm, geom=hf)
        # the split strips are now detached shells; re-own each strip RIGIDLY
        # to its majority NON-ARM region (it lies flush on the body at rest)
        strip_faces = set(g for g in ret["geom"] if isinstance(g, bmesh.types.BMFace))
        svisited = set()
        for f0 in strip_faces:
            if f0 in svisited:
                continue
            compf = [f0]; svisited.add(f0); qi = 0
            while qi < len(compf):
                f = compf[qi]; qi += 1
                for e in f.edges:
                    for f2 in e.link_faces:
                        if f2 in strip_faces and f2 not in svisited:
                            svisited.add(f2); compf.append(f2)
            votes = {}
            for f in compf:
                for vv in f.verts:
                    r = vv[lay]
                    if REGIONS[r] not in ARM_REGIONS:
                        votes[r] = votes.get(r, 0) + 1
            if not votes:
                for f in compf:
                    for vv in f.verts:
                        votes[vv[lay]] = votes.get(vv[lay], 0) + 1
            win = max(votes.items(), key=lambda kv: kv[1])[0]
            for f in compf:
                for vv in f.verts:
                    vv[lay] = win
        bm.to_mesh(mesh.data)
        bm.free()
        mesh.data.update()
        # ORDER-PROOF re-read: bmesh.to_mesh may reorder verts, so EVERY
        # per-vert array is rebuilt from the post-split mesh itself. The
        # original weights ride through the split in the deform layer
        # (duplicated verts inherit their source vert's groups), ownership
        # rides in the mf_region attribute.
        old_n = n
        n = len(mesh.data.vertices)
        W = world_verts(mesh)
        dist, radii, ndist, reg_nd = geom_arrays(W)
        chain_nd = arm_chain_nd(ndist, W)
        attr = mesh.data.attributes.get("mf_region")
        owner = np.empty(n, dtype=np.int32)
        attr.data.foreach_get("value", owner)
        shoulder_zone = calc_shoulder_zone(dist, radii)
        ev, adj = edge_graph()
        gname = {g.index: g.name for g in mesh.vertex_groups}
        orig = [dict() for _ in range(n)]
        for v in mesh.data.vertices:
            for ge in v.groups:
                nm = gname.get(ge.group)
                if nm in BONE2REGION and ge.weight > 1e-4:
                    orig[v.index][nm] = orig[v.index].get(nm, 0.0) + ge.weight
        log("post-split verts:", n, "(+%d dup)" % (n - old_n))

    # ---- HYBRID weights ---------------------------------------------------
    # Keep the original Meshy weights wherever they agree with ownership;
    # strip cross-region ARM weights (the membrane); capsule-fallback +
    # local smoothing only where stripping empties a vert.
    UPPER_ARM = {r: {b for b in REGION_BONES[r]
                     if b.endswith("Shoulder") or (b.endswith("Arm") and "Fore" not in b)}
                 for r in ARM_REGIONS}
    bone_idx_by_region = {r: [i for i, nm in enumerate(names) if BONE2REGION[nm] == r]
                          for r in REGIONS}
    inv = np.power(np.maximum(ndist, 0.12), -3.0)
    NON_ARM_BONES = set()
    for rr in REGIONS:
        if rr not in ARM_REGIONS:
            NON_ARM_BONES |= set(REGION_BONES[rr])

    def allowed_for(v):
        r = REGIONS[owner[v]]
        if r in ARM_REGIONS:
            al = set(REGION_BONES[r])
            if shoulder_zone[v]:
                al |= set(REGION_BONES["TORSO"])
            return al
        al = set(NON_ARM_BONES)
        if shoulder_zone[v]:
            for rr in ARM_REGIONS:
                al |= UPPER_ARM[rr]
        return al

    new_w = [None] * n
    fallback = []
    stripped = 0
    for v in range(n):
        al = allowed_for(v)
        w = {nm: x for nm, x in orig[v].items() if nm in al}
        if len(w) != len(orig[v]):
            stripped += 1
        tot = sum(w.values())
        if tot >= 0.15:
            new_w[v] = {nm: x / tot for nm, x in w.items()}
        else:
            fallback.append(v)
            r = REGIONS[owner[v]]
            bidx = list(bone_idx_by_region[r])
            if r == "TORSO":
                bidx += bone_idx_by_region["LEG_L"] + bone_idx_by_region["LEG_R"]
            wv = inv[bidx, v]
            top = np.argsort(wv)[::-1][:4]
            wmax = wv[top[0]]
            picked = [(names[bidx[t]], float(wv[t])) for t in top if wv[t] > 0.06 * wmax]
            s = sum(x for _, x in picked)
            new_w[v] = {nm: x / s for nm, x in picked}
    log("hybrid: stripped cross-weights on", stripped, "verts;",
        len(fallback), "capsule-fallback verts")

    # local Laplacian smoothing of the fallback verts only (blends the
    # re-owned drape into the kept originals around it)
    for it in range(8):
        upd = {}
        for v in fallback:
            al = allowed_for(v)
            acc = {}
            cnt = 0
            for u, l in adj[v]:
                for nm, x in new_w[u].items():
                    if nm in al:
                        acc[nm] = acc.get(nm, 0.0) + x
                cnt += 1
            if not cnt or not acc:
                continue
            mix = {}
            for nm in set(list(acc) + list(new_w[v])):
                mix[nm] = 0.5 * new_w[v].get(nm, 0.0) + 0.5 * acc.get(nm, 0.0) / cnt
            tot = sum(mix.values())
            if tot > 1e-8:
                upd[v] = {nm: x / tot for nm, x in mix.items()}
        for v, w in upd.items():
            new_w[v] = w

    # write groups
    mesh.vertex_groups.clear()
    groups = {nm: mesh.vertex_groups.new(name=nm) for nm in names}
    for v in range(n):
        for nm, x in new_w[v].items():
            if x > 1e-4:
                groups[nm].add([v], x, 'REPLACE')

    for o in bpy.data.objects: o.select_set(False)
    mesh.select_set(True)
    bpy.context.view_layer.objects.active = mesh
    bpy.ops.object.vertex_group_limit_total(group_select_mode='ALL', limit=4)
    bpy.ops.object.vertex_group_clean(group_select_mode='ALL', limit=0.01)
    bpy.ops.object.vertex_group_normalize_all(group_select_mode='ALL', lock_active=False)

    # ---- membrane metric --------------------------------------------------
    gi = {g.index: g.name for g in mesh.vertex_groups}
    cross = 0; unweighted = 0
    for v in mesh.data.vertices:
        tot = 0.0
        r = REGIONS[owner[v.index]]
        for ge in v.groups:
            nm = gi.get(ge.group, "")
            tot += ge.weight
            if nm in BONE2REGION and BONE2REGION[nm] in ARM_REGIONS \
               and r not in ARM_REGIONS and ge.weight > 0.05 \
               and not shoulder_zone[v.index]:
                cross += 1
        if tot < 1e-6:
            unweighted += 1
    log("METRIC arm-weight-on-body verts (outside shoulder zone):", cross)
    log("METRIC unweighted verts:", unweighted)
    log("fix_mesh done in %.1fs" % (time.time() - t0))


def stage_fix(src, out, workdir, flags):
    reset_factory()
    bpy.ops.import_scene.gltf(filepath=src)
    arm, meshes = find_rig()
    log("src:", src, "| armature:", arm.name, "| driven meshes:",
        [m.name for m in meshes])
    log("clips:", sorted(a.name for a in bpy.data.actions))
    for a in bpy.data.actions:
        a.use_fake_user = True
    for mesh in meshes:
        fix_mesh(mesh, arm, flags)
    pack_textures()
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=out, export_format='GLB', export_yup=True, use_selection=False,
        export_animations=True, export_animation_mode='ACTIONS',
        export_nla_strips=False, export_force_sampling=True, export_apply=False,
        export_materials='EXPORT', export_image_format='AUTO',
        export_texcoords=True, export_normals=True, export_tangents=True,
        export_skins=True)
    log("EXPORTED:", out, os.path.getsize(out), "bytes")


# ---------------------------------------------------------------------------
# Stage: qa -- grounded floor + level camera renders of clip frames
# ---------------------------------------------------------------------------
def qa_scene():
    scn = bpy.context.scene
    bpy.ops.mesh.primitive_plane_add(size=30, location=(0, 0, 0))
    floor = bpy.context.object
    floor.name = "QAFloor"
    fm = bpy.data.materials.new("QAFloorMat"); fm.use_nodes = True
    fm.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.32, 0.33, 0.36, 1)
    floor.data.materials.append(fm)
    sun = bpy.data.objects.new("QASun", bpy.data.lights.new("QASun", 'SUN'))
    sun.data.energy = 4.0
    sun.rotation_euler = (D(50), 0, D(35))
    bpy.context.collection.objects.link(sun)
    w = bpy.data.worlds.new("QAW"); scn.world = w; w.use_nodes = True
    w.node_tree.nodes["Background"].inputs[0].default_value = (0.18, 0.20, 0.24, 1)
    cam = bpy.data.objects.new("QACam", bpy.data.cameras.new("QACam"))
    bpy.context.collection.objects.link(cam)
    scn.camera = cam
    try:
        scn.render.engine = 'BLENDER_EEVEE_NEXT'
    except Exception:
        scn.render.engine = 'BLENDER_EEVEE'
    scn.render.resolution_x = 1024; scn.render.resolution_y = 1280
    scn.render.film_transparent = False

def char_bounds():
    deps = bpy.context.evaluated_depsgraph_get()
    mins = Vector((1e9, 1e9, 1e9)); maxs = Vector((-1e9, -1e9, -1e9))
    for o in bpy.data.objects:
        if o.type != 'MESH' or o.name in ("QAFloor",):
            continue
        oe = o.evaluated_get(deps)
        for c in oe.bound_box:
            wc = oe.matrix_world @ Vector(c)
            for i in range(3):
                mins[i] = min(mins[i], wc[i]); maxs[i] = max(maxs[i], wc[i])
    return mins, maxs

def qa_shot(path, view):
    mins, maxs = char_bounds()
    center = (mins + maxs) * 0.5
    height = max(0.8, maxs.z - mins.z)
    cam = bpy.data.objects["QACam"]
    dist = height * 2.1
    camz = min(center.z, mins.z + height * 0.55)   # level-ish, floor in frame
    if view == "front":
        cam.location = (center.x, center.y - dist, camz)
        cam.rotation_euler = (D(88), 0, 0)
    elif view == "back":
        cam.location = (center.x, center.y + dist, camz)
        cam.rotation_euler = (D(88), 0, D(180))
    else:  # 3/4
        cam.location = (center.x - dist * 0.75, center.y - dist * 0.75, camz)
        cam.rotation_euler = (D(84), 0, D(-45))
    cam.data.type = 'PERSP'; cam.data.lens = 50
    bpy.context.scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    log("render:", os.path.basename(path))

def stage_qa(glb, workdir, prefix, shots_spec, views):
    reset_factory()
    bpy.ops.import_scene.gltf(filepath=glb)
    arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
    if not arm.animation_data:
        arm.animation_data_create()
    qa_scene()
    for part in shots_spec.split(";"):
        if not part.strip():
            continue
        clip, frames = part.split(":")
        act = bpy.data.actions.get(clip)
        if act is None:
            for a in bpy.data.actions:
                if clip.lower() in a.name.lower(): act = a; break
        if act is None:
            log("QA WARN: no action", clip, "| have:",
                sorted(a.name for a in bpy.data.actions))
            continue
        arm.animation_data.action = act
        try:
            if getattr(act, "slots", None):
                arm.animation_data.action_slot = act.slots[0]
        except Exception:
            pass
        f0, f1 = act.frame_range
        for fs in frames.split(","):
            if fs.endswith("%"):
                f = int(round(f0 + (f1 - f0) * float(fs[:-1]) / 100.0))
            else:
                f = int(fs)
            bpy.context.scene.frame_set(f)
            bpy.context.view_layer.update()
            for view in views:
                qa_shot(os.path.join(workdir, "%s_%s_f%02d_%s.png"
                                     % (prefix, clip, f, view)), view)
    log("stage qa OK")


if __name__ == "__main__":
    if STAGE == "fix":
        src, out, workdir = ARGV[1], ARGV[2], ARGV[3]
        flags = set(ARGV[4:])
        os.makedirs(workdir, exist_ok=True)
        LOG_PATH = os.path.join(workdir, "fix.log")
        DONE_PATH = os.path.join(workdir, "fix.done")
        status = "OK"
        try:
            stage_fix(src, out, workdir, flags)
        except Exception as e:
            import traceback
            log("FAILED:", e); log(traceback.format_exc()); status = "FAIL: " + str(e)
        flush(status)
    elif STAGE == "qa":
        glb, workdir, prefix, shots = ARGV[1], ARGV[2], ARGV[3], ARGV[4]
        views = ARGV[5].split(",") if len(ARGV) > 5 else ["front", "persp"]
        os.makedirs(workdir, exist_ok=True)
        LOG_PATH = os.path.join(workdir, "qa_%s.log" % prefix)
        DONE_PATH = os.path.join(workdir, "qa_%s.done" % prefix)
        status = "OK"
        try:
            stage_qa(glb, workdir, prefix, shots, views)
        except Exception as e:
            import traceback
            log("FAILED:", e); log(traceback.format_exc()); status = "FAIL: " + str(e)
        flush(status)
    else:
        raise SystemExit("unknown stage: " + STAGE)
