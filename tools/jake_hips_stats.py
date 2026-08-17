"""jake_hips_stats.py — per-clip HIPS translation channel statistics + net yaw.

The root of truth for 'will this clip fling/slide/bury the mesh': the hips
node's raw local translation keys (min/max/net per axis, in the units baked
into the file) and the hips net YAW rotation over the clip (verifies the
turn clips' direction empirically — positive yaw = turn LEFT in rig space,
since R_y(+90) maps rig-forward +Z onto rig-left +X).

Usage: python tools/jake_hips_stats.py assets/rigged_glb/Jake_44_actions.glb
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


def read_accessor(data, js, bin_off, idx):
    acc = js["accessors"][idx]
    ncomp = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}[acc["type"]]
    bv = js["bufferViews"][acc["bufferView"]]
    boff = bin_off + bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride", 4 * ncomp)
    return [struct.unpack_from("<" + "f" * ncomp, data, boff + k * stride)
            for k in range(acc["count"])]


def quat_yaw_of_forward(q):
    """Yaw (rad) of the rig-forward +Z axis rotated by quat q."""
    x, y, z, w = q
    # R * (0,0,1)
    fx = 2 * (x * z + y * w)
    fz = 1 - 2 * (x * x + y * y)
    return math.atan2(fx, fz)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "assets/rigged_glb/Jake_44_actions.glb"
    data, js, bin_off = load_glb(path)
    nodes = js["nodes"]
    hips = next(i for i, n in enumerate(nodes) if n.get("name") == "mixamorigHips")
    rest = nodes[hips].get("translation", [0, 0, 0])
    print(f"hips node {hips}, rest T = "
          f"[{rest[0]:+.4f} {rest[1]:+.4f} {rest[2]:+.4f}]\n")
    print(f"{'idx':>3} {'clip':<34} {'X min..max (net)':>24} "
          f"{'Y min..max':>16} {'Z min..max (net)':>24} {'yawNet':>8}")
    for ai, anim in enumerate(js.get("animations", [])):
        tvals = rvals = None
        for ch in anim["channels"]:
            tgt = ch["target"]
            if tgt.get("node") != hips:
                continue
            smp = anim["samplers"][ch["sampler"]]
            if tgt["path"] == "translation":
                tvals = read_accessor(data, js, bin_off, smp["output"])
            elif tgt["path"] == "rotation":
                rvals = read_accessor(data, js, bin_off, smp["output"])
        name = anim.get("name", "")
        tcol = " (no hips T channel)"
        if tvals:
            xs = [v[0] for v in tvals]
            ys = [v[1] for v in tvals]
            zs = [v[2] for v in tvals]
            tcol = (f"{min(xs):+8.3f}..{max(xs):+8.3f} ({xs[-1]-xs[0]:+8.3f}) "
                    f"{min(ys):+7.3f}..{max(ys):+7.3f} "
                    f"{min(zs):+8.3f}..{max(zs):+8.3f} ({zs[-1]-zs[0]:+8.3f})")
        ycol = ""
        if rvals:
            y0 = quat_yaw_of_forward(rvals[0])
            y1 = quat_yaw_of_forward(rvals[-1])
            d = math.degrees(y1 - y0)
            while d > 180:
                d -= 360
            while d < -180:
                d += 360
            ycol = f"{d:+7.1f}"
        print(f"{ai:>3} {name:<34} {tcol} {ycol}")


if __name__ == "__main__":
    main()
