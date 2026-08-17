"""jake_clip_motion.py — measure each clip's ACTUAL travel direction, because
the labels lie (owner: "Left and right strafe play reversed"; the 44 list even
contains two clips both named 'Walking').

Method, per clip:
  1. NET HIPS DRIFT: sample the hips translation channel; net XZ displacement
     over the clip = authored root motion (zero for in-place clips).
  2. STANCE-FOOT DRIFT (works for in-place clips): sample the full leg pose,
     find which foot is planted (the lower one) each step, and average the
     planted foot's XZ velocity. A body travelling in direction d slides its
     planted foot at -d relative to the hips, so travel ≈ -mean(stance vel).

Directions are reported in RIG space (character faces +Z, character-LEFT = +X,
confirmed from the bind pose: LeftFoot x=+0.070, RightFoot x=-0.197) and as a
verdict: FWD / BACK / LEFT / RIGHT / IN-PLACE / TURN.

Usage: python tools/jake_clip_motion.py assets/rigged_glb/Jake_44_actions.glb
"""
import json
import math
import struct
import sys

GLB_JSON = 0x4E4F534A
GLB_BIN = 0x004E4942


def load_glb(path):
    data = open(path, "rb").read()
    length = struct.unpack_from("<I", data, 8)[0]
    off, js, bin_off = 12, None, None
    while off < length:
        clen, ctype = struct.unpack_from("<II", data, off)
        if ctype == GLB_JSON:
            js = json.loads(data[off + 8:off + 8 + clen])
        elif ctype == GLB_BIN:
            bin_off = off + 8
        off += 8 + clen
    return data, js, bin_off


class Reader:
    def __init__(self, data, js, bin_off):
        self.data, self.js, self.bin_off = data, js, bin_off

    def accessor(self, idx):
        acc = self.js["accessors"][idx]
        ncomp = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[acc["type"]]
        bv = self.js["bufferViews"][acc["bufferView"]]
        boff = self.bin_off + bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
        stride = bv.get("byteStride", 4 * ncomp)
        out = []
        for k in range(acc["count"]):
            out.append(struct.unpack_from("<" + "f" * ncomp, self.data,
                                          boff + k * stride))
        return out


def sample(times, values, t):
    """Linear interp (nlerp for quats — good enough for direction stats)."""
    if not times:
        return None
    if t <= times[0]:
        return values[0]
    if t >= times[-1]:
        return values[-1]
    lo, hi = 0, len(times) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if times[mid] <= t:
            lo = mid
        else:
            hi = mid
    f = (t - times[lo]) / max(times[hi] - times[lo], 1e-9)
    a, b = values[lo], values[hi]
    if len(a) == 4:  # quat: shortest-arc nlerp
        dot = sum(x * y for x, y in zip(a, b))
        if dot < 0:
            b = tuple(-x for x in b)
        v = tuple(x + (y - x) * f for x, y in zip(a, b))
        n = math.sqrt(sum(x * x for x in v)) or 1.0
        return tuple(x / n for x in v)
    return tuple(x + (y - x) * f for x, y in zip(a, b))


def trs_to_mat(t, q, s):
    x, y, z, w = q
    m = [
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), t[0]],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), t[1]],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), t[2]],
        [0, 0, 0, 1],
    ]
    for r in range(3):
        for c in range(3):
            m[r][c] *= s[c]
    return m


def mat_mul_pt(m, p):
    return tuple(m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3]
                 for r in range(3))


def mat_mul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)]
            for r in range(4)]


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "assets/rigged_glb/Jake_44_actions.glb"
    data, js, bin_off = load_glb(path)
    rd = Reader(data, js, bin_off)
    nodes = js["nodes"]
    parents = {}
    for i, n in enumerate(nodes):
        for c in n.get("children", []):
            parents[c] = i
    by_name = {n.get("name", ""): i for i, n in enumerate(nodes)}
    arm = by_name["Armature"]
    hips = by_name["mixamorigHips"]
    lfoot, rfoot = by_name["mixamorigLeftFoot"], by_name["mixamorigRightFoot"]

    # Node chains needed (armature-local: stop before the Armature node).
    def chain(idx):
        out = []
        n = idx
        while n is not None and n != arm:
            out.append(n)
            n = parents.get(n)
        return list(reversed(out))  # root-first

    need = set(chain(lfoot)) | set(chain(rfoot)) | set(chain(hips))

    print(f"{'idx':>3} {'clip':<34} {'dur':>5} {'hipsNet dX dZ':>16} "
          f"{'stanceVel dX dZ':>17}  verdict")
    for ai, anim in enumerate(js.get("animations", [])):
        # channel table: node -> {path: (times, values)}
        ch_tab = {}
        dur = 0.0
        for ch in anim["channels"]:
            tgt = ch["target"]
            nidx = tgt.get("node")
            if nidx not in need:
                continue
            smp = anim["samplers"][ch["sampler"]]
            times = [v[0] for v in rd.accessor(smp["input"])]
            values = rd.accessor(smp["output"])
            ch_tab.setdefault(nidx, {})[tgt["path"]] = (times, values)
            dur = max(dur, times[-1] if times else 0.0)
        if dur <= 0.0:
            print(f"{ai:>3} {anim.get('name',''):<34}  (no sampled channels)")
            continue

        def local_mat(nidx, t):
            node = nodes[nidx]
            tab = ch_tab.get(nidx, {})
            T = (sample(*tab["translation"], t) if "translation" in tab
                 else tuple(node.get("translation", [0, 0, 0])))
            R = (sample(*tab["rotation"], t) if "rotation" in tab
                 else tuple(node.get("rotation", [0, 0, 0, 1])))
            S = (sample(*tab["scale"], t) if "scale" in tab
                 else tuple(node.get("scale", [1, 1, 1])))
            return trs_to_mat(T, R, S)

        def world(nidx, t):
            m = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]
            for n in chain(nidx):
                m = mat_mul(m, local_mat(n, t))
            return mat_mul_pt(m, (0, 0, 0))

        N = 48
        ts = [dur * k / (N - 1) for k in range(N)]
        hp = [world(hips, t) for t in ts]
        lf = [world(lfoot, t) for t in ts]
        rf = [world(rfoot, t) for t in ts]
        net = (hp[-1][0] - hp[0][0], hp[-1][2] - hp[0][2])

        # stance-foot drift: average planted-foot velocity (lower foot wins).
        sx = sz = 0.0
        cnt = 0
        for k in range(1, N):
            dt = ts[k] - ts[k - 1]
            planted_now = lf if lf[k][1] < rf[k][1] else rf
            vx = (planted_now[k][0] - planted_now[k - 1][0]) / dt
            vz = (planted_now[k][2] - planted_now[k - 1][2]) / dt
            sx += vx
            sz += vz
            cnt += 1
        sx, sz = sx / cnt, sz / cnt
        # travel = -stance velocity
        tx, tz = -sx, -sz

        def verdict(dx, dz, thresh):
            mag = math.hypot(dx, dz)
            if mag < thresh:
                return "in-place", mag
            if abs(dz) >= abs(dx):
                return ("FWD(+Z)" if dz > 0 else "BACK(-Z)"), mag
            return ("LEFT(+X)" if dx > 0 else "RIGHT(-X)"), mag

        vNet, magNet = verdict(net[0], net[1], 0.25)
        vSt, magSt = verdict(tx, tz, 0.15)
        pick = vNet if magNet >= 0.25 else vSt
        print(f"{ai:>3} {anim.get('name',''):<34} {dur:>5.2f} "
              f"{net[0]:>+7.3f} {net[1]:>+7.3f}  "
              f"{tx:>+7.3f} {tz:>+7.3f}   net={vNet} stance={vSt} -> {pick}")


if __name__ == "__main__":
    main()
