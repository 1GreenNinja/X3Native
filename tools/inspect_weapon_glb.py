# Inspect the embedded images + material factors of each weapon viewmodel GLB.
# Prints an sha256 of every embedded image so a SHARED texture atlas across
# different weapons is immediately visible (identical hashes = shared skin bug).
# ASCII-only on purpose.
import struct, json, os, sys, hashlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GLB_DIR = os.path.join(REPO, "assets", "rigged_glb")
JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942

WEAPONS = [
    ("pistol   ", "WeaponEnergyPistol2.glb"),
    ("shotgun  ", "WeaponShotgun2.glb"),
    ("chaingun ", "WeaponRocketLauncher.glb"),
    ("plasma   ", "WeaponBFG.glb"),
    ("lightning", "WeaponRailgun.glb"),
]


def load_glb(path):
    d = open(path, "rb").read()
    magic, ver, length = struct.unpack_from("<III", d, 0)
    assert magic == 0x46546C67, "not a GLB: " + path
    off = 12
    g = None
    blob = b""
    while off < length:
        clen, ctype = struct.unpack_from("<II", d, off)
        off += 8
        ch = d[off:off + clen]
        off += clen
        if ctype == JSON_CHUNK:
            g = json.loads(ch.decode("utf-8"))
        elif ctype == BIN_CHUNK:
            blob = ch
    return g, blob


def main():
    seen = {}
    for label, fn in WEAPONS:
        p = os.path.join(GLB_DIR, fn)
        if not os.path.exists(p):
            print("%s MISSING %s" % (label, fn))
            continue
        g, blob = load_glb(p)
        print("=== %s  %s  (%d KB)" % (label, fn, os.path.getsize(p) // 1024))
        for mi, m in enumerate(g.get("materials", [])):
            pbr = m.get("pbrMetallicRoughness", {})
            print("    mat[%d] %-22s base=%s metal=%s rough=%s emis=%s" % (
                mi, m.get("name", "?"),
                pbr.get("baseColorFactor"), pbr.get("metallicFactor"),
                pbr.get("roughnessFactor"), m.get("emissiveFactor")))
        for ii, im in enumerate(g.get("images", [])):
            if "bufferView" not in im:
                continue
            bv = g["bufferViews"][im["bufferView"]]
            o = bv.get("byteOffset", 0)
            data = blob[o:o + bv["byteLength"]]
            h = hashlib.sha256(data).hexdigest()[:16]
            tag = ""
            if h in seen and seen[h] != label:
                tag = "  <<< SHARED WITH %s" % seen[h]
            else:
                seen.setdefault(h, label)
            print("    img[%d] %-18s %7d B  sha=%s%s" % (
                ii, im.get("name", "?"), len(data), h, tag))
    print()
    print("Distinct image hashes across all 5 weapons: %d" % len(seen))


if __name__ == "__main__":
    main()
