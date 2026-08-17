#!/usr/bin/env python3
"""W-TOWN pedestrians: real CIVILIANS for the Small Mountain Town.

WHY (the receipt, NO_SLOP rule 10)
----------------------------------
`app/town.cpp` spawned its six pedestrians from the crowd_skin roster —
`AnnaCasual_anim`, `marcus_webb_anim`, `chief_martinez_anim`. Rendering that
roster (`tools/glb_contact_sheet.py assets/rigged_glb/...`) shows what it
actually is:

    AnnaCasual_anim      a woman in a crop top and shorts     — a civilian
    chief_martinez_anim  a black-clad SWAT/tactical operator
    marcus_webb_anim     A CLAWED GREEN-VEINED MUTANT

Two of the three townspeople walking Main Street were a special-forces officer
and a monster, because `CrowdSkin::defaultRigs()` was cast for the CLUB scene
and picked purely on "has Idle/Walk/Run", never on whether the character
belongs in the world. The eye gate caught it the moment the pedestrian camera
finally framed a walker properly (`shots_town/03_pedestrians.png`).

The shared roster is NOT changed here — other worlds legitimately want those
rigs. The town keeps its own list, and this tool builds it.

THE SOURCE
----------
`City People FREE Samples` (Denys Almaral, licensed; found with
`python tools/unitypackage_index.py --search "City People"` — it was one of the
~700 packages that had never been extracted, which is exactly what that index
exists for). It ships:
  * civilian character meshes, one skin each, on a shared 33-node skeleton —
    casual male/female, an elder, a child, a doctor, a police officer;
  * locomotion and idle clips as SEPARATE single-clip FBXs on that same
    skeleton (`locom_m_basicWalk_30f`, `idle_m_1_200f`, `locom_m_jogging_30f`);
  * one palette atlas, `people_pal.png`, for every character.

THE PIPELINE (each step reuses something that already existed)
--------------------------------------------------------------
1. Read the `.unitypackage` directly — it is a gzipped tar of
   `<guid>/asset` + `<guid>/pathname` — and pull only what is needed. No Unity,
   no full extraction.
2. `FBX2glTF` each mesh and each clip.
3. Inject `people_pal.png` as the baseColor of the `peopleColors` material
   (the FBX has no texture path, so without this every pedestrian is a 1x1
   white placeholder — the same trap `tools/town_assets.py` documents for the
   buildings; NO_SLOP rule 3).
4. **`node tools/glb-merge-anims.mjs`** fuses mesh + clips into one GLB, and
   NAMES the clips `Idle` / `Walk` / `Run`. That tool already existed for
   exactly this shape of pack (see tools/README-glb-merge.md) — NO_SLOP rule 1,
   the wheel was in the tree.
   The names are not cosmetic: `AnimatedCharacter` resolves clips by EXACT
   name, and `townPedClipTable()` in app/town.cpp asks for Idle/Walk/Run. A
   mismatch yields a bind-pose statue that slides, which is the T-pose defect
   NO_SLOP rule 1 catalogues.

USAGE
-----
    python tools/town_people.py build     # -> assets/rigged_glb/CityPerson_*.glb
    python tools/town_people.py verify    # assert clips + textures are right

AFTER RUNNING: `python tools/asset_store.py publish assets/rigged_glb` —
store-served, never git-committed (docs/ENGINE_GOTCHAS.md gotcha 2.5).
"""
from __future__ import annotations

import json
import os
import shutil
import struct
import subprocess
import sys
import tarfile

PKG = (r"C:\Users\Tim\AppData\Roaming\Unity\Asset Store-5.x\Denys Almaral"
       r"\3D ModelsCharacters\City People FREE Samples.unitypackage")
FBX2GLTF = r"D:\GameDev\tools\FBX2glTF.exe"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "rigged_glb")
STAGE = os.path.join(os.environ.get("TEMP", "/tmp"), "x3_citypeople")

# The town's cast. Deliberately ordinary: this is a mountain town on a highway,
# so it gets people who live there, not a tactical team. Each entry is
# (source mesh stem, output name, "m"|"f" clip set).
CAST = [
    ("casual_Male_G",   "CityPerson_ManCasual",   "m"),
    ("casual_Female_G", "CityPerson_WomanCasual", "f"),
    ("casual_Male_K",   "CityPerson_ManJacket",   "m"),
    ("casual_Female_K", "CityPerson_WomanCoat",   "f"),
    ("elder_Female_A",  "CityPerson_Elder",       "f"),
    ("little_boy_B",    "CityPerson_Boy",         "m"),
]

# Clip FBX per sex -> the EXACT names townPedClipTable() asks for.
# PAIRED with townPedClipTable() in app/town.cpp (NO_SLOP rule 4): these three
# strings and that table are one value. Change either and the walkers freeze.
CLIPS = {
    "m": [("idle_m_1_200f", "Idle"), ("locom_m_basicWalk_30f", "Walk"),
          ("locom_m_jogging_30f", "Run"), ("idle_selfcheck_1_300f", "LookAround")],
    "f": [("idle_f_1_150f", "Idle"), ("locom_f_basicWalk_30f", "Walk"),
          ("locom_f_jogging_30f", "Run"), ("idle_phoneTalking_180f", "LookAround")],
}
ATLAS = "people_pal.png"
SKIN_MAT = "peopleColors"


# --------------------------------------------------------------------------
def extract():
    """Pull the meshes, clips and atlas straight out of the .unitypackage."""
    os.makedirs(STAGE, exist_ok=True)
    need = {s for s, _, _ in CAST}
    for sex in CLIPS:
        need |= {c for c, _ in CLIPS[sex]}
    names = {}
    with tarfile.open(PKG, "r:gz") as tf:
        for m in tf:
            if m.name.endswith("/pathname"):
                names[m.name.split("/")[0]] = \
                    tf.extractfile(m).read().decode("utf-8").split("\n")[0].strip()
    want = {}
    for guid, path in names.items():
        base = os.path.basename(path)
        stem = os.path.splitext(base)[0]
        if stem in need or base == ATLAS:
            want[guid] = base
    got = 0
    with tarfile.open(PKG, "r:gz") as tf:
        for m in tf:
            if not m.name.endswith("/asset"):
                continue
            guid = m.name.split("/")[0]
            if guid not in want:
                continue
            with open(os.path.join(STAGE, want[guid]), "wb") as f:
                f.write(tf.extractfile(m).read())
            got += 1
    missing = need - {os.path.splitext(v)[0] for v in want.values()}
    if missing:
        raise SystemExit(f"[people] NOT IN PACKAGE: {sorted(missing)}")
    print(f"  extracted {got} members from the .unitypackage")


def to_glb(stem):
    src = os.path.join(STAGE, stem + ".fbx")
    dst = os.path.join(STAGE, stem + ".glb")
    if os.path.exists(dst) and os.path.getmtime(dst) > os.path.getmtime(src):
        return dst
    r = subprocess.run([FBX2GLTF, "-b", "-i", src, "-o", dst],
                       capture_output=True, text=True)
    if not os.path.exists(dst):
        raise SystemExit(f"[people] FBX2glTF failed on {stem}:\n{r.stdout}\n{r.stderr}")
    return dst


# --------------------------------------------------------------------------
def load_glb(path):
    with open(path, "rb") as f:
        magic, _v, _l = struct.unpack("<III", f.read(12))
        assert magic == 0x46546C67, path
        clen, _ct = struct.unpack("<II", f.read(8))
        gltf = json.loads(f.read(clen))
        blen, _bt = struct.unpack("<II", f.read(8))
        return gltf, bytearray(f.read(blen))


def save_glb(path, gltf, bin_):
    while len(bin_) % 4:
        bin_.append(0)
    gltf["buffers"] = [{"byteLength": len(bin_)}]
    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    while len(js) % 4:
        js += b" "
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(bin_)))
        f.write(struct.pack("<II", len(js), 0x4E4F534A)); f.write(js)
        f.write(struct.pack("<II", len(bin_), 0x004E4942)); f.write(bytes(bin_))


def paint(path):
    """Bind people_pal.png to the skin material. Without this every pedestrian
    ships on FBX2glTF's 1x1 white placeholder — NO_SLOP rule 3."""
    gltf, bin_ = load_glb(path)
    with open(os.path.join(STAGE, ATLAS), "rb") as f:
        raw = f.read()
    while len(bin_) % 4:
        bin_.append(0)
    off = len(bin_); bin_ += raw
    gltf.setdefault("bufferViews", []).append(
        {"buffer": 0, "byteOffset": off, "byteLength": len(raw)})
    gltf.setdefault("images", []).append(
        {"bufferView": len(gltf["bufferViews"]) - 1, "mimeType": "image/png"})
    gltf.setdefault("samplers", []).append(
        {"wrapS": 33071, "wrapT": 33071, "magFilter": 9729, "minFilter": 9987})
    gltf.setdefault("textures", []).append(
        {"source": len(gltf["images"]) - 1, "sampler": len(gltf["samplers"]) - 1})
    ti = len(gltf["textures"]) - 1
    for mat in gltf.get("materials", []):
        pbr = mat.setdefault("pbrMetallicRoughness", {})
        pbr["baseColorTexture"] = {"index": ti, "texCoord": 0}
        pbr["baseColorFactor"] = [1.0, 1.0, 1.0, 1.0]
        # A person is not metal (rule 5) and skin/cloth is not a mirror.
        pbr["metallicFactor"] = 0.0
        pbr["roughnessFactor"] = 0.78
    save_glb(path, gltf, bin_)
    return path


def cmd_build():
    extract()
    os.makedirs(OUT, exist_ok=True)
    clip_glb = {}
    for sex, entries in CLIPS.items():
        for stem, _name in entries:
            clip_glb[stem] = to_glb(stem)
    for mesh_stem, out_name, sex in CAST:
        mesh = to_glb(mesh_stem)
        painted = os.path.join(STAGE, out_name + ".mesh.glb")
        shutil.copyfile(mesh, painted)
        paint(painted)
        clips = [clip_glb[s] for s, _ in CLIPS[sex]]
        names = [n for _, n in CLIPS[sex]]
        dst = os.path.join(OUT, out_name + ".glb")
        r = subprocess.run(
            ["node", os.path.join(ROOT, "tools", "glb-merge-anims.mjs"),
             "--mesh", painted, "--clips", ",".join(clips),
             "--names", ",".join(names), "--out", dst],
            capture_output=True, text=True)
        if not os.path.exists(dst):
            raise SystemExit(f"[people] merge failed for {out_name}:\n{r.stdout}\n{r.stderr}")
        gltf, _ = load_glb(dst)
        print(f"  {out_name:26s} {os.path.getsize(dst)//1024:5d} KB  "
              f"clips={[a.get('name') for a in gltf.get('animations', [])]}")
    print(f"\n[people] wrote {OUT}")


def cmd_verify():
    """Every rig must carry Idle/Walk/Run under those EXACT names and a real
    bound texture. Exactly the two things that, missing, produce a sliding
    T-pose or a white ghost — and neither shows up as an error at runtime."""
    ok = True
    for _stem, name, _sex in CAST:
        p = os.path.join(OUT, name + ".glb")
        if not os.path.exists(p):
            print(f"  FAIL {name}: not built"); ok = False; continue
        gltf, bin_ = load_glb(p)
        clips = [a.get("name") for a in gltf.get("animations", [])]
        for need in ("Idle", "Walk", "Run"):
            if need not in clips:
                print(f"  FAIL {name}: no {need!r} clip (has {clips}) — "
                      f"AnimatedCharacter resolves by EXACT name"); ok = False
        if not gltf.get("skins"):
            print(f"  FAIL {name}: no skin — it cannot animate"); ok = False
        bound = {t.get("source") for t in gltf.get("textures", [])}
        if not bound:
            print(f"  FAIL {name}: no bound texture (NO_SLOP rule 3)"); ok = False
        for mat in gltf.get("materials", []):
            pbr = mat.get("pbrMetallicRoughness", {})
            if "baseColorTexture" not in pbr:
                print(f"  FAIL {name}: material {mat.get('name')} untextured"); ok = False
        print(f"  {'ok  ' if ok else '    '}{name}: clips={clips} skins={len(gltf.get('skins', []))}")
    print("\n[people] verify", "GREEN" if ok else "RED")
    return 0 if ok else 1


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "build"
    if cmd == "build":
        cmd_build()
    elif cmd == "verify":
        sys.exit(cmd_verify())
    else:
        raise SystemExit(__doc__)
