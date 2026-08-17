"""jake_scale_channels.py — scan every clip's SCALE channels for values that
would explode/shrink the skeleton (the walk-blend shatter investigation).

Usage: python tools/jake_scale_channels.py assets/rigged_glb/Jake_44_actions.glb
"""
import json
import struct
import sys


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "assets/rigged_glb/Jake_44_actions.glb"
    data = open(path, "rb").read()
    length = struct.unpack_from("<I", data, 8)[0]
    off, js, bo = 12, None, None
    while off < length:
        clen, ctype = struct.unpack_from("<II", data, off)
        if ctype == 0x4E4F534A:
            js = json.loads(data[off + 8:off + 8 + clen])
        elif ctype == 0x004E4942:
            bo = off + 8
        off += 8 + clen

    def acc(idx):
        a = js["accessors"][idx]
        n = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}[a["type"]]
        bv = js["bufferViews"][a["bufferView"]]
        b = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
        st = bv.get("byteStride", 4 * n)
        return [struct.unpack_from("<" + "f" * n, data, bo + b + k * st)
                for k in range(a["count"])]

    nodes = js["nodes"]
    for ai, a in enumerate(js.get("animations", [])):
        bad = []
        for ch in a["channels"]:
            if ch["target"]["path"] != "scale":
                continue
            n = ch["target"]["node"]
            vals = acc(a["samplers"][ch["sampler"]]["output"])
            mn = min(min(v) for v in vals)
            mx = max(max(v) for v in vals)
            if mx > 1.5 or mn < 0.6:
                bad.append((nodes[n].get("name"), round(mn, 4), round(mx, 4),
                            len(vals)))
        if bad:
            print(f"clip {ai} {a.get('name')}:")
            for b in bad:
                print("   ", b)
    print("scan done (only out-of-range scale channels listed)")


if __name__ == "__main__":
    main()
