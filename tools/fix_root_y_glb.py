"""
fix_root_y_glb.py - repair a retargeted GLB whose Hips (root) translation-Y
channel was baked with a corrupted vertical scale.

THE BUG (measured 2026-07, the "buried half-way + bouncing" captive/citizen):
tools/retarget_from_jake.py scaled the source hips vertical bob by
    loc_scale = tgt_hips_world_h / src_hips_world_h
but src_hips_world_h was measured through the Jake source armature OBJECT's
baked world offset (Armature node T.y = -0.94875 in Jake_22_actions.glb), so
    src_h_bad  = 1.14234 - 0.94875 = 0.19359   (instead of 1.14234)
    loc_scale  = 0.864 / 0.19359   = 4.4630    (instead of 0.75634)
i.e. every hips vertical delta was amplified 5.90x. Clips with a real hips
deviation (Walk, Run) drove the pelvis THROUGH the floor:
    AnnaCasual_anim Walk hips Y baked [0.179 .. 0.571]  (rest 0.864)
    AnnaCasual_anim Run  hips Y baked [-0.374 .. 0.396]
Clips with ~zero hips deviation (Idle, Talk, the gesture set) were untouched.

THE FIX: rescale the baked delta back:  y' = rest + (y - rest) * r
with  r = src_h_bad / src_h_true  (both measured from the Jake source GLB:
hips rest Y + the armature node's baked Y offset). Verified exact: the
corrected Run range [0.654 .. 0.785] matches Jake's Riflerun delta * the
correct 0.75634 ratio to 4 decimals.

The patch is applied IN-PLACE in the GLB BIN chunk (float32 Y lanes of the
Hips translation sampler output accessor) - no re-serialization, no Blender.
A .bak copy is written next to the input first.

Usage:
    python tools/fix_root_y_glb.py <target_anim.glb> <jake_source.glb> \
        --clips Walk,Run [--root Hips] [--dry-run]

Clean-room: glTF 2.0 spec parsing only.
"""
import argparse
import json
import shutil
import struct
import sys

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942


def load_glb(path):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    if struct.unpack_from("<I", data, 0)[0] != GLB_MAGIC:
        raise SystemExit(f"{path}: not a GLB")
    length = struct.unpack_from("<I", data, 8)[0]
    off = 12
    js = None
    bin_off = None
    while off < length:
        clen, ctype = struct.unpack_from("<II", data, off)
        if ctype == CHUNK_JSON:
            js = json.loads(bytes(data[off + 8:off + 8 + clen]))
        elif ctype == CHUNK_BIN:
            bin_off = off + 8
        off += 8 + clen
    if js is None or bin_off is None:
        raise SystemExit(f"{path}: missing JSON or BIN chunk")
    return data, js, bin_off


def accessor_span(js, idx):
    """(byteOffset-within-BIN, count, stride, ncomp) for a float accessor."""
    acc = js["accessors"][idx]
    if acc["componentType"] != 5126:
        raise SystemExit(f"accessor {idx}: not float32")
    ncomp = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[acc["type"]]
    bv = js["bufferViews"][acc["bufferView"]]
    boff = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride", 4 * ncomp)
    return boff, acc["count"], stride, ncomp


def node_index_by_name(js, name):
    for i, n in enumerate(js["nodes"]):
        if n.get("name", "") == name:
            return i
    return -1


def measure_ratio(jake_path, root_name="mixamorigHips", arm_name="Armature"):
    """r = (hips_rest_y + armature_offset_y) / hips_rest_y from the Jake GLB."""
    _, js, _ = load_glb(jake_path)
    hips = node_index_by_name(js, root_name)
    arm = node_index_by_name(js, arm_name)
    if hips < 0 or arm < 0:
        raise SystemExit(f"{jake_path}: cannot find {root_name} / {arm_name}")
    hips_y = js["nodes"][hips].get("translation", [0, 0, 0])[1]
    arm_y = js["nodes"][arm].get("translation", [0, 0, 0])[1]
    if abs(hips_y) < 1e-6:
        raise SystemExit(f"{jake_path}: hips rest Y is zero")
    r = (hips_y + arm_y) / hips_y
    print(f"[fix-root-y] source: hips rest Y={hips_y:+.5f} armature Y={arm_y:+.5f}"
          f" -> correction ratio r={r:.6f} (bake amplified 1/r={1.0/r:.3f}x)")
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("target")
    ap.add_argument("jake")
    ap.add_argument("--clips", default="Walk,Run",
                    help="comma list of clip names to repair (default Walk,Run)")
    ap.add_argument("--root", default="Hips", help="root bone node name in the target")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    r = measure_ratio(args.jake)
    data, js, bin_off = load_glb(args.target)
    root = node_index_by_name(js, args.root)
    if root < 0:
        raise SystemExit(f"{args.target}: no node named {args.root}")
    rest_y = js["nodes"][root].get("translation", [0, 0, 0])[1]
    print(f"[fix-root-y] target: node[{root}] '{args.root}' rest Y={rest_y:+.5f}")

    want = {c.strip().lower() for c in args.clips.split(",") if c.strip()}
    patched_clips = 0
    for anim in js.get("animations", []):
        name = anim.get("name", "")
        if name.lower() not in want:
            continue
        for ch in anim["channels"]:
            tgt = ch["target"]
            if tgt.get("node") != root or tgt.get("path") != "translation":
                continue
            smp = anim["samplers"][ch["sampler"]]
            boff, count, stride, ncomp = accessor_span(js, smp["output"])
            if ncomp != 3:
                raise SystemExit(f"clip {name}: translation output is not VEC3")
            ys = []
            for k in range(count):
                o = bin_off + boff + k * stride + 4  # +4 = the Y float lane
                (y,) = struct.unpack_from("<f", data, o)
                ys.append(y)
                y_fixed = rest_y + (y - rest_y) * r
                if not args.dry_run:
                    struct.pack_into("<f", data, o, y_fixed)
            lo, hi = min(ys), max(ys)
            flo = rest_y + (lo - rest_y) * r
            fhi = rest_y + (hi - rest_y) * r
            print(f"[fix-root-y] clip '{name}': {count} keys, hips Y"
                  f" [{lo:+.4f} .. {hi:+.4f}] -> [{flo:+.4f} .. {fhi:+.4f}]")
            patched_clips += 1

    if patched_clips == 0:
        raise SystemExit("no matching clip/channel found - nothing patched")
    if args.dry_run:
        print("[fix-root-y] dry run - file untouched")
        return
    shutil.copyfile(args.target, args.target + ".bak")
    with open(args.target, "wb") as f:
        f.write(data)
    print(f"[fix-root-y] wrote {args.target} ({patched_clips} clip(s) repaired;"
          f" backup at {args.target}.bak)")


if __name__ == "__main__":
    main()
