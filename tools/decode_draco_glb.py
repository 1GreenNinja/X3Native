#!/usr/bin/env python3
"""Decode KHR_draco_mesh_compression out of staged editor-palette GLBs.

WHY: the Creepy Cat "3D Scifi Kit Vol 2/Vol 3" (and, it turns out, the
already-staged "Sci-Fi Space Stations Creator") source GLBs all export with
`extensionsRequired: ["KHR_draco_mesh_compression"]`. This engine's runtime
loader (engine/asset/ModelLoader.cpp, buildPrimitives()) has NO Draco decoder
-- it unconditionally does `if (prim.has_draco_mesh_compression) { skip; }` --
so every raw-staged piece from these packs silently falls back to the small
graybox marker (no crash, no error, just wrong content). This was invisible
until an actual screenshot was taken and read; `--test-editor` /
`--test-loader` don't load real GLB content so they stayed green throughout.

FIX: `npx @gltf-transform/cli copy <src> <dst>` decodes Draco on read and
(since `copy` applies no compression on write) emits a plain GLB with a
normal POSITION/NORMAL/TEXCOORD_0 bufferView per primitive and no
extensionsUsed/Required — verified byte-for-byte on a sample file. This
script batch-runs that over a staged flat pack dir (produced by
tools/stage_editor_glb.ps1) into a sibling "<PackName>Decoded" dir, which you
then junction into assets/converted_glb the same way (see
tools/stage_editor_glb.ps1's own junction step, or run it manually -- this
script does NOT create the junction, only the decoded files, since it may be
run against a partial file LIST for speed rather than a whole pack).

USAGE
  # Decode an explicit file list (fast -- used to unblock the space-station
  # level's ~24 referenced pieces without waiting on the whole pack):
  python tools/decode_draco_glb.py --editor-root D:\\Assets\\_glb\\_editor \
      --pack ScifiKitVol2 --files Plateform_Center_01.glb Room_Fence_01.glb ...

  # Decode an ENTIRE staged pack (slower -- ~1-1.5s/file via npx CLI restart
  # per file; a Vol3-sized pack of ~884 files takes ~20-25 minutes):
  python tools/decode_draco_glb.py --editor-root D:\\Assets\\_glb\\_editor --pack ScifiKitVol3 --all

Resumable: files already present at the destination are skipped (re-run any
time to pick up newly staged modules, same convention as stage_editor_glb.ps1).
"""
import argparse
import os
import subprocess
import sys
import time


def decode_one(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    r = subprocess.run(
        ["npx", "--yes", "@gltf-transform/cli", "copy", src, dst],
        capture_output=True, text=True, shell=(os.name == "nt"),
    )
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--editor-root", default=r"D:\Assets\_glb\_editor")
    ap.add_argument("--pack", required=True, help="source pack folder name, e.g. ScifiKitVol3")
    ap.add_argument("--files", nargs="*", help="explicit basenames to decode (fast path)")
    ap.add_argument("--all", action="store_true", help="decode every *.glb in the pack (slow)")
    args = ap.parse_args()

    src_dir = os.path.join(args.editor_root, args.pack)
    dst_dir = os.path.join(args.editor_root, args.pack + "Decoded")
    if not os.path.isdir(src_dir):
        print(f"pack not found: {src_dir}", file=sys.stderr)
        sys.exit(1)

    if args.all:
        names = sorted(f for f in os.listdir(src_dir) if f.lower().endswith(".glb"))
    elif args.files:
        names = args.files
    else:
        print("pass --files <names...> or --all", file=sys.stderr)
        sys.exit(1)

    ok, fail, skipped = 0, 0, 0
    t0 = time.time()
    for i, name in enumerate(names):
        src = os.path.join(src_dir, name)
        dst = os.path.join(dst_dir, name)
        if os.path.exists(dst):
            skipped += 1
            continue
        if not os.path.exists(src):
            print(f"  MISSING source: {src}")
            fail += 1
            continue
        if decode_one(src, dst):
            ok += 1
        else:
            fail += 1
            print(f"  FAILED: {name}")
        if (i + 1) % 25 == 0:
            print(f"  ...{i+1}/{len(names)} ({time.time()-t0:.0f}s elapsed)")

    print(f"decoded {ok}, failed {fail}, already-present {skipped} -> {dst_dir}")
    print("Remember to junction the *Decoded dir into assets/converted_glb and add it "
          "to .git/info/exclude (same pattern as tools/stage_editor_glb.ps1) if it isn't already.")


if __name__ == "__main__":
    main()
