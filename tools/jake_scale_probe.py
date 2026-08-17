"""jake_scale_probe.py — compare each clip's per-bone translation-channel
magnitudes against the bone's REST local translation, to find which clips baked
their translations in centimetres (magnitude ratio ~100x) vs metres (~1x).

Usage: python tools/jake_scale_probe.py assets/rigged_glb/Jake_44_actions.glb
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


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "assets/rigged_glb/Jake_44_actions.glb"
    data, js, bin_off = load_glb(path)
    nodes = js["nodes"]
    probe = {}
    for nm in ("mixamorigLeftFoot", "mixamorigLeftUpLeg", "mixamorigHead",
               "mixamorigSpine", "mixamorigHips"):
        i = next(k for k, n in enumerate(nodes) if n.get("name") == nm)
        rest = nodes[i].get("translation", [0, 0, 0])
        probe[i] = (nm, math.sqrt(sum(v * v for v in rest)))
    print("rest |T| per probe bone:",
          {nm: round(r, 4) for nm, r in probe.values()})
    print(f"\n{'idx':>3} {'clip':<34} " +
          " ".join(f"{nm.replace('mixamorig',''):>10}" for nm, _ in probe.values()))
    for ai, anim in enumerate(js.get("animations", [])):
        cols = {}
        for ch in anim["channels"]:
            tgt = ch["target"]
            nidx = tgt.get("node")
            if nidx in probe and tgt["path"] == "translation":
                vals = read_accessor(data, js, bin_off,
                                     anim["samplers"][ch["sampler"]]["output"])
                mags = [math.sqrt(v[0] ** 2 + v[1] ** 2 + v[2] ** 2) for v in vals]
                mean = sum(mags) / len(mags)
                _, rest_mag = probe[nidx]
                cols[nidx] = mean / rest_mag if rest_mag > 1e-6 else mean
        row = " ".join(f"{cols.get(i, float('nan')):>10.2f}" for i in probe)
        print(f"{ai:>3} {anim.get('name',''):<34} {row}")


if __name__ == "__main__":
    main()
