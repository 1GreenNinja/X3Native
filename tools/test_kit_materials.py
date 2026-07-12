#!/usr/bin/env python3
"""GATE: the ModularSciFi kit's converted GLBs carry HONEST materials.

This is a REGRESSION GATE for the emission-key bug (docs/KNOWN_BUGS.md). The kit's
Unity source bakes its emitters into the DIFFUSE as key colours — pure magenta
(255,0,255) and pure yellow (255,255,0) — with the real per-texel glow mask in the
ALPHA of T_<X>_MRAG.png. The original conversion kept only Dif + Norm, so every
light lens shipped as flat PAINT and the facility's ceilings/door trim/wall insets
rendered as magenta and yellow slabs the moment the ambient crutch was removed.

REGRESSION DISCIPLINE (docs/DECISIONS.md): "a material exists" is worthless. These
assertions measure the thing that actually broke:

  K1  every kit material has a baseColorTexture, a metallicRoughnessTexture and an
      emissiveTexture, and a non-zero emissiveFactor.
        - the MR map is not optional: Scene::submit() only forwards Entity::emissiveTex
          on the mrTex.valid() PBR branch (LANDMINE L4), so an emissive map with no MR
          map is SILENTLY DROPPED at runtime and the fix vanishes.
  K2  ZERO emission-key texels survive in the base colour. Probes for the classic
      Unity magenta key (R ~= B, G ~= 0) AND the pack's yellow key (R ~= G, B ~= 0).
      Must be exactly 0.0%.
  K3  the emissive map is REAL and PER-TEXEL: it glows over a small, plausible
      fraction of the atlas (>0.05%, <60%) and reaches a bright core (max >= 200).
      A wholly-black map means the glow was lost; a wholly-bright map is a flat
      self-emissive slab, which is the crutch this whole sweep exists to kill.
  K4  the kit is not accidentally full-metal (LANDMINE L5). A ceiling panel is not
      metal: the mean metalness over the panel atlases must stay < 0.5, and any
      material WITHOUT an MR texture must not sit at metallicFactor 1.0.
  K5  the gold hazard stripes (194,160,65) — LEGITIMATE PAINT, alpha 0 in the glow
      mask — survived the repair. A repair that eats real paint is not a fix.

  NEG the probe can FAIL. A synthetic keyed albedo is fed to the same K2 probe and
      must come back non-zero. Without this, K2 passing proves nothing (that is how
      the holo terminal shipped broken ten times).

Usage:  python tools/test_kit_materials.py          # exit 0 = green
"""
import io, os, sys
import numpy as np
from PIL import Image
from pygltflib import GLTF2

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convert_modular_scifi import key_masks, magenta_fraction  # the SAME probe the converter uses

KIT = os.path.join("assets", "converted_glb", "ModularSciFi_Interior")

fails = []
def check(ok, msg):
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        fails.append(msg)


def decode(g, blob, tex_index):
    src = g.textures[tex_index].source
    img = g.images[src]
    if img.bufferView is not None:
        bv = g.bufferViews[img.bufferView]
        off = bv.byteOffset or 0
        data = blob[off: off + bv.byteLength]
    else:
        import base64
        data = base64.b64decode(img.uri.split(",", 1)[1])
    return np.asarray(Image.open(io.BytesIO(bytes(data))).convert("RGB"))


def yellow_fraction(rgb):
    _, y, _, _ = key_masks(rgb)
    return float(y.mean())


def main():
    if not os.path.isdir(KIT):
        print("FAIL: %s not found (run from the repo root — LANDMINE L2)" % KIT)
        return 1

    glbs = sorted(f for f in os.listdir(KIT) if f.lower().endswith(".glb"))
    print("kit materials gate — %d GLBs in %s\n" % (len(glbs), KIT))

    for f in glbs:
        g = GLTF2().load(os.path.join(KIT, f))
        blob = g.binary_blob()
        print(f)
        for m in g.materials:
            p = m.pbrMetallicRoughness
            tag = "%s/%s" % (f, m.name)

            # ---- K1
            has_bc = p is not None and p.baseColorTexture is not None
            has_mr = p is not None and p.metallicRoughnessTexture is not None
            has_em = m.emissiveTexture is not None
            ef = max(m.emissiveFactor or [0, 0, 0])
            check(has_bc, "K1 %s has a baseColorTexture" % tag)
            check(has_mr, "K1 %s has a metallicRoughnessTexture (L4: no MR map -> "
                          "Scene::submit DROPS the emissive)" % tag)
            check(has_em, "K1 %s has an emissiveTexture" % tag)
            check(ef > 0.0, "K1 %s emissiveFactor > 0 (got %.2f)" % (tag, ef))
            if not (has_bc and has_mr and has_em):
                continue

            bc = decode(g, blob, p.baseColorTexture.index)
            em = decode(g, blob, m.emissiveTexture.index)
            orm = decode(g, blob, p.metallicRoughnessTexture.index)

            # ---- K2: NO emission key may survive in base colour.
            mag = magenta_fraction(bc)
            yel = yellow_fraction(bc)
            check(mag == 0.0, "K2 %s base colour carries ZERO magenta key texels "
                              "(R~=B, G~=0) — got %.4f%%" % (tag, 100 * mag))
            check(yel == 0.0, "K2 %s base colour carries ZERO yellow key texels "
                              "(R~=G, B~=0) — got %.4f%%" % (tag, 100 * yel))

            # ---- K3: the emissive map is real, localized, and has a bright core.
            lit = float((em.max(2) > 0).mean())
            peak = int(em.max())
            check(0.0005 < lit < 0.60,
                  "K3 %s emissive is PER-TEXEL (%.2f%% of the atlas glows; a black map "
                  "= glow lost, a full map = flat self-emissive slab)" % (tag, 100 * lit))
            check(peak >= 200, "K3 %s emissive reaches a bright core (max=%d)" % (tag, peak))

            # ---- K4: L5 — a ceiling panel is not metal.
            metal = float(orm[..., 2].mean()) / 255.0
            check(metal < 0.5, "K4 %s is not full-metal (mean metalness %.3f) — L5" % (tag, metal))
            check(not (p.metallicFactor == 1.0 and not has_mr),
                  "K4 %s does not sit at glTF's metallicFactor=1.0 default with no MR map" % tag)

            # ---- K5: the gold hazard stripes are PAINT and must survive.
            R, G, B = (bc[..., i].astype(np.int32) for i in range(3))
            gold = ((np.abs(R - 194) < 30) & (np.abs(G - 160) < 30) & (np.abs(B - 65) < 30))
            if f.startswith(("SM_Ceiling", "SM_Floor", "SM_Wall", "SM_Door")):
                check(gold.mean() > 0.0005,
                      "K5 %s kept its gold hazard stripes (%.3f%% — real paint, not a key)"
                      % (tag, 100 * gold.mean()))
        print("")

    # ---- NEGATIVE CONTROL: prove the K2 probe can fail.
    print("negative control (the probe MUST fire on a keyed texture)")
    keyed = np.full((64, 64, 3), 40, np.uint8)
    keyed[:32, :, :] = (255, 0, 255)        # the magenta emission key
    keyed[32:, :, :] = (255, 255, 0)        # the yellow emission key
    nm, ny = magenta_fraction(keyed), yellow_fraction(keyed)
    check(nm > 0.4, "NEG the magenta probe FIRES on a keyed texture (%.1f%% — if this is 0, "
                    "K2 above proves nothing)" % (100 * nm))
    check(ny > 0.4, "NEG the yellow probe FIRES on a keyed texture (%.1f%%)" % (100 * ny))
    # ...and does NOT fire on the gold hazard paint (a probe that eats real art is useless).
    paint = np.full((64, 64, 3), 0, np.uint8); paint[:, :] = (194, 160, 65)
    check(magenta_fraction(paint) == 0.0 and yellow_fraction(paint) == 0.0,
          "NEG the probes do NOT fire on the gold hazard paint (194,160,65)")

    print("\n%s — %d failure(s)" % ("GREEN" if not fails else "RED", len(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
