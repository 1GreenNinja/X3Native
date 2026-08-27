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

    albedo_gate()

    print("\n%s — %d failure(s)" % ("GREEN" if not fails else "RED", len(fails)))
    return 0 if not fails else 1


# ===========================================================================
# B5 — THE OVER-UNITY ALBEDO GATE
#
# A base colour is a PHYSICAL quantity: the fraction of light a surface reflects.
# The kit packs were authored for a bright showroom preview, and the L5 metal crutch
# (a full metal has no diffuse lobe) hid it for months. When L5 was clamped and the
# diffuse lobe came back, the kit started blowing out at close range. THE CLAMP
# EXPOSED B5; IT DID NOT CAUSE IT. The fix is the VALUE — never the lamp, never the
# roughness, and never putting the metal back.
#
#   K6  the LAW is in force in the engine: ModelLoader carries the ceiling, the target,
#       and every pack this gate measures — and door.cpp does NOT still carry its old
#       hand-picked scale (which would DOUBLE-APPLY on top of the loader's).
#   K7  no kit material's base colour, AS THE ENGINE WILL USE IT, exceeds the ceiling.
#   NEG2  the probe can FAIL: a synthetic near-white (0.768 — the measured SM_Door_A
#       slab) must come back HOT, and an honest 0.30 institutional grey must NOT.
# ===========================================================================
CEILING = 0.45   # nothing man-made in a detention facility reflects more than this
TARGET  = 0.32   # the value the door was hand-fixed to and shipped at
FLOOR   = 0.15   # a map needing a HARDER scale than this is a white sheet, not a tuning
                 # problem — a factor would be papering over art that must be re-authored.
PACKS   = ["SciFi_Warehouse_Kit", "ModularSciFi_Interior", "Detention", "SciFiKit3"]
LUMA    = np.array([0.2126, 0.7152, 0.0722])


def srgb_to_linear(c):
    c = np.asarray(c, dtype=np.float64) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def albedo_p95(rgb):
    """The BRIGHT PLATEAU of a base-colour map, in LINEAR light.

    p95, not the mean, and that distinction IS the bug: an atlas is many regions, and
    T_Door_A_Dif's mean is a respectable 0.397 precisely because a dark trim region
    averages away a white door slab that is 44% of the texels. The p95 recovers 0.768
    — the exact value the door was diagnosed at by hand. A mean-based gate would have
    called the blown-out door GREEN.
    """
    return float(np.percentile(srgb_to_linear(rgb) @ LUMA, 95))


def albedo_scale(p95):
    """The engine's law (ModelLoader::normalizeKitAlbedo), reimplemented independently."""
    return min(1.0, TARGET / p95) if p95 > CEILING else 1.0


def albedo_gate():
    # ---- K6: the law is actually IN FORCE in the engine.
    src = open(os.path.join("engine", "asset", "ModelLoader.cpp"), encoding="utf-8").read()
    check("kAlbedoCeiling = %.2ff" % CEILING in src,
          "K6 ModelLoader carries the %.2f albedo ceiling" % CEILING)
    check("kAlbedoTarget  = %.2ff" % TARGET in src or "kAlbedoTarget = %.2ff" % TARGET in src,
          "K6 ModelLoader carries the %.2f albedo target" % TARGET)
    check("normalizeKitAlbedo" in src, "K6 ModelLoader still CALLS normalizeKitAlbedo")
    for p in PACKS:
        check('"%s"' % p in src, "K6 ModelLoader's kit-pack list covers %s" % p)
    # The double-apply trap: the door's per-call-site scale MUST be gone now that the
    # loader does this by law, or SM_Door_A lands at 0.175 linear — asphalt.
    door = open(os.path.join("app", "door.cpp"), encoding="utf-8").read()
    check("kDoorAlbedoScale" not in door,
          "K6 door.cpp does NOT re-scale the albedo itself (the loader does it by law; "
          "a second scale here would DOUBLE-APPLY)")

    # ---- K7: every kit material lands under the ceiling.
    print("\nB5 albedo gate — kit base colours, LINEAR (ceiling %.2f, target %.2f)\n" % (
        CEILING, TARGET))
    print("  %-21s %-26s %-20s %6s %6s %6s" % (
        "pack", "glb", "material", "raw", "scale", "final"))
    n_hot = 0
    for pack in PACKS:
        d = os.path.join("assets", "converted_glb", pack)
        if not os.path.isdir(d):
            check(False, "K7 pack %s not found (run tools/asset_store.py fetch --all)" % pack)
            continue
        for f in sorted(x for x in os.listdir(d) if x.lower().endswith(".glb")):
            g = GLTF2().load(os.path.join(d, f))
            blob = g.binary_blob()
            if g.skins:
                continue   # A CHARACTER IS NOT A WALL — the loader exempts skinned models.
            for mi, m in enumerate(g.materials or []):
                p = m.pbrMetallicRoughness
                name = m.name or "mat%d" % mi
                fac = list(p.baseColorFactor) if (p and p.baseColorFactor) else [1, 1, 1, 1]
                fl = float(np.dot(np.array(fac[:3]), LUMA))
                if p is not None and p.baseColorTexture is not None:
                    raw = albedo_p95(decode(g, blob, p.baseColorTexture.index)) * fl
                else:
                    raw = fl   # no map: the factor IS the albedo
                s = albedo_scale(raw)
                final = raw * s
                if s < 1.0:
                    n_hot += 1
                    print("  %-21s %-26s %-20s %6.3f %6.3f %6.3f  <== renormalized" % (
                        pack, f[:26], name[:20], raw, s, final))
                tag = "%s/%s/%s" % (pack, f, name)
                check(final <= CEILING + 1e-3,
                      "K7 %s base colour %.3f linear is under the %.2f ceiling "
                      "(institutional ~0.30; snow is 0.85)" % (tag, final, CEILING))
                check(s >= FLOOR,
                      "K7 %s needs only a sane factor (x%.3f >= %.2f) — a map needing more "
                      "is a WHITE SHEET and must be re-authored, not scaled" % (tag, s, FLOOR))
    print("\n  %d material(s) renormalized by the loader's law" % n_hot)
    check(n_hot > 0, "K7 the law is actually DOING something (%d materials renormalized — "
                     "if this is 0, the packs changed and the gate is measuring nothing)"
                     % n_hot)

    # ---- NEG2: THE NEGATIVE CONTROL. The probe must be able to go RED.
    print("\nnegative control (the albedo probe MUST fire on a near-white surface)")
    # The measured SM_Door_A slab: sRGB 227 -> 0.768 linear. A door is not snow.
    white = np.full((64, 64, 3), 227, np.uint8)
    wp95 = albedo_p95(white)
    check(abs(wp95 - 0.768) < 0.01,
          "NEG2 the probe MEASURES the door slab correctly (sRGB 227 -> %.3f linear, "
          "expected 0.768)" % wp95)
    check(wp95 > CEILING,
          "NEG2 the probe FIRES on the 0.768 near-white door slab (%.3f > %.2f) — if this "
          "did not go RED, K7 above would prove nothing" % (wp95, CEILING))
    check(albedo_scale(wp95) < 1.0 and abs(wp95 * albedo_scale(wp95) - TARGET) < 1e-3,
          "NEG2 the law CORRECTS it to the institutional band (%.3f -> %.3f)" % (
              wp95, wp95 * albedo_scale(wp95)))
    # ...and does NOT fire on an honest institutional grey. A gate that condemns every
    # surface is as useless as one that condemns none.
    grey = np.full((64, 64, 3), 149, np.uint8)     # sRGB 149 -> ~0.30 linear
    gp95 = albedo_p95(grey)
    check(0.25 <= gp95 <= 0.35, "NEG2 the honest institutional grey measures %.3f linear "
                                "(the 0.25-0.35 painted-wall band)" % gp95)
    check(albedo_scale(gp95) == 1.0,
          "NEG2 the probe does NOT fire on an honest 0.30 surface (a gate that condemns "
          "everything is a gate that measures nothing)")


if __name__ == "__main__":
    sys.exit(main())
