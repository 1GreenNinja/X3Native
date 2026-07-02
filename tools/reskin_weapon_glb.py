# DEPRECATED (2026-07) -- DO NOT USE for weapon skins.
# This only TINTS a (shared) atlas via baseColorFactor + fake emissive; that is
# exactly what produced the "camo/pale slab" weapons. Weapons now carry their OWN
# per-weapon PBR maps -- use tools/rebind_weapon_textures.py, not this. Kept only
# as a generic factor-editor for non-weapon assets.
#
# Re-skin a weapon viewmodel GLB IN PLACE by editing its PBR material factors.
#
# ASSET-ONLY: changes only the Principled-BSDF-equivalent glTF material factors
# (baseColorFactor / metallicFactor / roughnessFactor / emissiveFactor +
# KHR_materials_emissive_strength). Geometry, skins, animations and the existing
# baseColor/normal/metalRough TEXTURES are left untouched -- baseColorFactor
# MULTIPLIES the diffuse texture, so each weapon gets a distinct colour cast
# while keeping its real PBR surface detail. Emissive accents pop under bloom.
#
# Same file path/name is kept so weapon.cpp still resolves the GLB unchanged.
#
# Usage:
#   python tools/reskin_weapon_glb.py <weapon_key> <glb_path>
#
# weapon_key in: pistol smg shotgun lightning plasma chaingun
#
# ASCII-only on purpose.
import sys, os
from pygltflib import GLTF2, Material, PbrMetallicRoughness

# Per-weapon distinct identities. (r,g,b,a) factors multiply the diffuse texture.
# emissive_strength via KHR_materials_emissive_strength so accents bloom.
RECIPES = {
    # Pistol: clean gunmetal + subtle cool blue accent.
    "pistol":   dict(base=[0.62, 0.68, 0.80, 1.0], metal=1.0, rough=0.34,
                     emissive=[0.05, 0.10, 0.30], estr=2.0),
    # SMG: matte tactical black + orange accents.
    "smg":      dict(base=[0.16, 0.15, 0.14, 1.0], metal=0.55, rough=0.78,
                     emissive=[0.55, 0.18, 0.02], estr=2.5),
    # Shotgun: heavy steel + brass/worn warmth.
    "shotgun":  dict(base=[0.78, 0.66, 0.42, 1.0], metal=1.0, rough=0.52,
                     emissive=[0.10, 0.06, 0.01], estr=1.2),
    # Lightning: dark body + bright ELECTRIC-BLUE / cyan emissive.
    "lightning":dict(base=[0.10, 0.13, 0.18, 1.0], metal=0.85, rough=0.30,
                     emissive=[0.05, 0.55, 0.95], estr=5.0),
    # Plasma / plasma rifle: sleek white + green/cyan plasma glow.
    "plasma":   dict(base=[0.90, 0.95, 0.92, 1.0], metal=0.45, rough=0.22,
                     emissive=[0.05, 0.80, 0.45], estr=4.5),
    # Chaingun: industrial dark steel + orange-hot accents.
    "chaingun": dict(base=[0.22, 0.20, 0.18, 1.0], metal=1.0, rough=0.62,
                     emissive=[0.70, 0.28, 0.03], estr=3.0),
}

def main():
    if len(sys.argv) < 3:
        print("usage: reskin_weapon_glb.py <weapon_key> <glb_path>")
        sys.exit(2)
    key, path = sys.argv[1], sys.argv[2]
    if key not in RECIPES:
        print("unknown weapon key:", key, "(have:", ", ".join(RECIPES), ")")
        sys.exit(2)
    if not os.path.exists(path):
        print("missing GLB:", path); sys.exit(2)
    r = RECIPES[key]

    gl = GLTF2().load(path)
    if not gl.materials:
        # No material at all -> create one so the tint/emissive still apply.
        gl.materials = [Material(name="WeaponPBR",
                                 pbrMetallicRoughness=PbrMetallicRoughness())]
    n = 0
    for m in gl.materials:
        if m.pbrMetallicRoughness is None:
            m.pbrMetallicRoughness = PbrMetallicRoughness()
        pbr = m.pbrMetallicRoughness
        pbr.baseColorFactor = list(r["base"])
        pbr.metallicFactor  = float(r["metal"])
        pbr.roughnessFactor = float(r["rough"])
        m.emissiveFactor = list(r["emissive"])
        # emissive strength extension for bloom-popping accents
        ext = m.extensions or {}
        ext["KHR_materials_emissive_strength"] = {"emissiveStrength": float(r["estr"])}
        m.extensions = ext
        m.name = "Weapon_" + key
        n += 1

    used = {"KHR_materials_emissive_strength"}
    gl.extensionsUsed = sorted(set(gl.extensionsUsed or []) | used)

    gl.save(path)
    print("[reskin] %-9s -> %s  (%d material(s))  base=%s metal=%.2f rough=%.2f emiss=%s x%.1f"
          % (key, os.path.basename(path), n, r["base"], r["metal"], r["rough"],
             r["emissive"], r["estr"]))

if __name__ == "__main__":
    main()
