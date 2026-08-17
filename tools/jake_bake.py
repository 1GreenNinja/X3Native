"""jake_bake.py — one-time (reproducible) surgery on Jake_44_actions.glb.

THE ASSET FIX, NOT THE RUNTIME (owner: "Copy those from meshy and reset his
angle to 0 in the GLb"). Three defects, all measured by tools/jake_inspect.py,
tools/jake_hips_stats.py and tools/jake_scale_probe.py before this was written:

1. ROOT OFFSET: scene root node 'Armature' carries T=[-0.0004,-0.9488,-0.0007].
   That -0.9488 m Y is THE buried-body offset that has cost this project three
   separate runtime compensations. No animation channel targets the Armature
   (verified: channels target the mixamorig* joints only), so the translation
   is static and safe to ZERO.

2. FACING: the rig is authored facing +Z (bind-pose toes point +Z:
   LeftFoot->LeftToeBase dZ=+0.155, RightFoot->RightToeBase dZ=+0.127 — the
   Mixamo standard). Engine forward is -Z (CLAUDE.md AXES LAW). A Y-180
   rotation [0,1,0,0] is BAKED into the Armature so IDENTITY = engine-forward.

3. CLIP UNITS + ROOT MOTION: the 44 clips are two families merged:
     - clips 0-23 ("meshy/mixamo batch 1"): translations in METRES (limb
       translation channels == rest bone lengths, ratio 1.00);
     - clips 24-43 ("batch 2"): translations in CENTIMETRES from a different-
       proportioned source rig (limb-channel/rest ratios 74x-137x, hips Y mean
       ~90-100). Played raw, these put the hips metres off the capsule and
       stretch every bone ~100x.
   Repair: batch-2 limb translation channels are replaced with the node's REST
   translation (bone lengths must come from THIS skeleton, and real rigs never
   animate them); batch-2 hips translations are scaled cm->m (x0.01). Then any
   clip whose hips XZ nets more than 0.05 m of travel gets the LINEAR DRIFT
   removed (v -= net * (t-t0)/(t1-t0)) — the capsule owns world position, so
   every clip must be in-place; within-cycle sway is preserved.

The original file is preserved as Jake_44_actions.glb.bak (committed). The
script refuses to run if the .bak already exists (protects the true original).

Usage: python tools/jake_bake.py assets/rigged_glb/Jake_44_actions.glb
"""
import json
import math
import os
import struct
import sys

GLB_MAGIC = 0x46546C67
GLB_JSON = 0x4E4F534A
GLB_BIN = 0x004E4942

DRIFT_MIN_M = 0.05      # de-drift hips XZ when |net| exceeds this (metres)
CM_RATIO_MIN = 10.0     # channel-mean/rest-magnitude above this = cm-scale


def load_glb(path):
    data = bytearray(open(path, "rb").read())
    if struct.unpack_from("<I", data, 0)[0] != GLB_MAGIC:
        raise SystemExit(f"{path}: not a GLB")
    length = struct.unpack_from("<I", data, 8)[0]
    off, js, bin_off, bin_len = 12, None, None, 0
    while off < length:
        clen, ctype = struct.unpack_from("<II", data, off)
        if ctype == GLB_JSON:
            js = json.loads(bytes(data[off + 8:off + 8 + clen]))
        elif ctype == GLB_BIN:
            bin_off, bin_len = off + 8, clen
        off += 8 + clen
    if js is None or bin_off is None:
        raise SystemExit(f"{path}: missing JSON or BIN chunk")
    return data, js, bin_off, bin_len


def acc_span(js, idx):
    acc = js["accessors"][idx]
    if acc["componentType"] != 5126:
        raise SystemExit(f"accessor {idx}: not float32")
    ncomp = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}[acc["type"]]
    bv = js["bufferViews"][acc["bufferView"]]
    boff = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride", 4 * ncomp)
    return boff, acc["count"], stride, ncomp


def read_vecs(data, js, bin_off, idx):
    boff, count, stride, ncomp = acc_span(js, idx)
    return [list(struct.unpack_from("<" + "f" * ncomp, data,
                                    bin_off + boff + k * stride))
            for k in range(count)]


def write_vecs(data, js, bin_off, idx, vecs):
    boff, count, stride, ncomp = acc_span(js, idx)
    assert count == len(vecs)
    for k, v in enumerate(vecs):
        struct.pack_into("<" + "f" * ncomp, data, bin_off + boff + k * stride, *v)
    acc = js["accessors"][idx]
    if "min" in acc:
        acc["min"] = [min(v[c] for v in vecs) for c in range(ncomp)]
    if "max" in acc:
        acc["max"] = [max(v[c] for v in vecs) for c in range(ncomp)]


def save_glb(path, data, js, bin_off, bin_len):
    jbytes = json.dumps(js, separators=(",", ":")).encode("utf-8")
    while len(jbytes) % 4:
        jbytes += b" "
    bin_chunk = bytes(data[bin_off:bin_off + bin_len])
    while len(bin_chunk) % 4:
        bin_chunk += b"\x00"
    total = 12 + 8 + len(jbytes) + 8 + len(bin_chunk)
    out = bytearray()
    out += struct.pack("<III", GLB_MAGIC, 2, total)
    out += struct.pack("<II", len(jbytes), GLB_JSON) + jbytes
    out += struct.pack("<II", len(bin_chunk), GLB_BIN) + bin_chunk
    open(path, "wb").write(out)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "assets/rigged_glb/Jake_44_actions.glb"
    bak = path + ".bak"
    if os.path.exists(bak):
        raise SystemExit(f"{bak} already exists — refusing to overwrite the "
                         f"original backup. Restore from it first if re-baking.")
    data, js, bin_off, bin_len = load_glb(path)
    nodes = js["nodes"]
    by_name = {n.get("name", ""): i for i, n in enumerate(nodes)}
    arm = by_name["Armature"]
    hips = by_name["mixamorigHips"]

    # ---- 1+2. Root: zero translation, bake Y-180 ---------------------------
    old_t = nodes[arm].get("translation")
    print(f"[bake] Armature (node {arm}): T {old_t} -> removed (zero); "
          f"R -> [0,1,0,0] (Y-180: +Z-authored rig now faces engine -Z)")
    nodes[arm].pop("translation", None)
    nodes[arm]["rotation"] = [0.0, 1.0, 0.0, 0.0]

    # Safety: no channel may target the Armature (verified by jake_inspect.py,
    # re-checked here).
    for anim in js.get("animations", []):
        for ch in anim["channels"]:
            if ch["target"].get("node") == arm:
                raise SystemExit("a channel targets the Armature — the static "
                                 "zero is NOT safe; rebase needed. Aborting.")

    # ---- 3. Clip units + root motion ---------------------------------------
    # Accessor-sharing guard: one treatment per output accessor.
    treated = {}   # accessor idx -> description

    def claim(aidx, what):
        if aidx in treated and treated[aidx] != what:
            raise SystemExit(f"accessor {aidx} shared with conflicting "
                             f"treatment ({treated[aidx]} vs {what}) — abort")
        first = aidx not in treated
        treated[aidx] = what
        return first

    n_limb = n_cm = n_drift = 0
    for anim in js.get("animations", []):
        name = anim.get("name", "?")
        hips_out = hips_in = None
        for ch in anim["channels"]:
            tgt = ch["target"]
            if tgt["path"] != "translation":
                continue
            nidx = tgt.get("node")
            smp = anim["samplers"][ch["sampler"]]
            vals = read_vecs(data, js, bin_off, smp["output"])
            rest = nodes[nidx].get("translation", [0.0, 0.0, 0.0])
            rest_mag = math.sqrt(sum(v * v for v in rest))
            mean_mag = sum(math.sqrt(v[0] ** 2 + v[1] ** 2 + v[2] ** 2)
                           for v in vals) / len(vals)
            ratio = mean_mag / rest_mag if rest_mag > 1e-6 else mean_mag
            if nidx == hips:
                if mean_mag / max(rest_mag, 1e-6) > CM_RATIO_MIN:
                    if claim(smp["output"], f"hips-cm->m:{name}"):
                        vals = [[c * 0.01 for c in v] for v in vals]
                        write_vecs(data, js, bin_off, smp["output"], vals)
                        n_cm += 1
                hips_out = smp["output"]
                hips_in = smp["input"]
            elif ratio > CM_RATIO_MIN:
                if claim(smp["output"], f"limb-rest:{nodes[nidx].get('name')}:{name}"):
                    write_vecs(data, js, bin_off, smp["output"],
                               [list(rest)] * len(vals))
                    n_limb += 1

        # De-drift the hips XZ (post-scale).
        if hips_out is None:
            continue
        vals = read_vecs(data, js, bin_off, hips_out)
        times = [v[0] for v in read_vecs(data, js, bin_off, hips_in)]
        t0, t1 = times[0], times[-1]
        span = max(t1 - t0, 1e-6)
        changed = False
        for axis in (0, 2):
            net = vals[-1][axis] - vals[0][axis]
            if abs(net) > DRIFT_MIN_M:
                for k in range(len(vals)):
                    vals[k][axis] -= net * (times[k] - t0) / span
                print(f"[bake] '{name}': hips axis {'XZ'[axis // 2]} de-drifted "
                      f"({net:+.3f} m over {span:.2f} s)")
                changed = True
                n_drift += 1
        if changed:
            if claim(hips_out, f"hips-cm->m:{anim.get('name','?')}") or True:
                write_vecs(data, js, bin_off, hips_out, vals)

    print(f"[bake] {n_cm} hips channel(s) rescaled cm->m, {n_limb} limb "
          f"channel(s) reset to rest bone lengths, {n_drift} axis de-drift(s)")

    import shutil
    shutil.copyfile(path, bak)
    save_glb(path, data, js, bin_off, bin_len)
    print(f"[bake] wrote {path} (original preserved at {bak})")


if __name__ == "__main__":
    main()
