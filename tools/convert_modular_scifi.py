#!/usr/bin/env python3
"""ModularSciFi_Interior kit: rebuild the glTF materials from the pack's REAL maps.

THE BUG THIS FIXES
------------------
The pack ("Scifi Modular Interior Space Station") ships THREE maps per material:

    T_<X>_Dif.png    sRGB albedo
    T_<X>_Norm.png   normal
    T_<X>_MRAG.png   **M**etallic(R) **R**oughness(G) **A**mbient-occlusion(B) **G**low(A)

The kit is driven by a CUSTOM shader whose emission is `Dif.rgb * MRAG.a` — the
ALPHA channel of MRAG is a per-texel glow mask, and the DIFFUSE carries the
emitter's COLOUR (one atlas holds several: yellow ceiling panels, magenta accent
lenses, blue console screens, green stair strips, a white door readout).

Whoever first converted this kit to GLB kept only Dif + Norm and **threw MRAG
away**. So the engine got:
  * NO emissive map -> every light lens rendered as flat PAINT. Once the 0.42
    ambient crutch was removed, the facility read as slabs of pure magenta
    (255,0,255) and pure yellow (255,255,0) — the emitter colours, unlit.
  * NO metallic/roughness/occlusion map -> every surface fell back to
    metallic=0 / roughness=0.5 uniform plastic.

MEASURED KEY COLOURS (not guessed) — the diffuse under `MRAG.a > 200`:
    yellow  (255,255,0)   ceiling/floor/wall light panels + strips  (dominant)
    magenta (255,0,255)   small accent lenses / trim
    blue    (0,0,255)     console screens        (Console atlas only)
    green   (0,255,0)     stair strips           (Stairs atlas only)
    white   (~224,224,224) door readout          (DoorPanel atlas only)
100% of key texels lie inside the glow mask; the gold hazard stripes (194,160,65)
have alpha 0 and are LEGITIMATE PAINT — they must survive untouched.

WHAT THIS SCRIPT DOES
---------------------
For each GLB in the kit it replaces the material's images with:
  baseColorTexture : Dif with every glow texel repaired to a neutral diffuser
                     grey — an UNLIT panel must read as a dark panel, not a
                     magenta one. ZERO key texels survive.
  emissiveTexture  : linear(Dif) * glowmask, with the hot core desaturated toward
                     white (see CORE_WHITE — a real lamp's core reads white, and
                     this engine's per-channel ACES can never produce that from a
                     saturated primary). Per-texel, so it glows where the artist
                     painted a lens and stays black everywhere else, and it keeps
                     the mask's soft falloff. Radiance rides
                     KHR_materials_emissive_strength so bloom has something to eat.
  metallicRoughnessTexture / occlusionTexture : ORM repacked from MRAG
                     (R=AO=MRAG.B, G=rough=MRAG.G, B=metal=MRAG.R).
  normalTexture    : the pack's Norm map.

L4 (Scene::submit drops emissiveTex without an MR map) is satisfied for free:
every material now carries a real MR texture, so the emissive can never be
silently dropped on the non-PBR branch.
L5 (glTF metallicFactor defaults to 1.0) is satisfied: metallicFactor is 1.0
only because the ORM *texture* now supplies the real per-texel metalness (which
is 0 across the panels — a ceiling panel is not metal).

Geometry is untouched: we re-skin the existing GLBs (same meshes, same UVs).

Usage:
    python tools/convert_modular_scifi.py [--pack DIR] [--glb-dir DIR]
                                          [--out DIR] [--emissive-gain F]
                                          [--max-tex N] [--dry-run]
"""
import argparse, base64, io, os, sys
import numpy as np
from PIL import Image
import pygltflib
from pygltflib import (GLTF2, Texture, Sampler, TextureInfo, Image as GImage,
                       NormalMaterialTexture, OcclusionTextureInfo, PbrMetallicRoughness)

DEFAULT_PACK = r"D:\Assets\Scifi Modular Interior Space Station\ModularScifiInterior\Textures"
DEFAULT_GLB  = "assets/converted_glb/ModularSciFi_Interior"

# Albedo replacement for a keyed (emissive) texel. A light lens / diffuser is a
# neutral mid grey when it is OFF. sRGB 158 ~= 0.33 linear: dark enough that an
# unlit panel reads as a dark panel, bright enough to read as a plastic diffuser
# and not a hole. Well under unity (POLISH RECIPE: no over-unity albedo).
DIFFUSER_SRGB = (158, 158, 162)

# Emissive RADIANCE, shipped via KHR_materials_emissive_strength (ModelLoader reads
# it: cgltf has_emissive_strength -> m.emissive *= strength). Above 1.0 so the
# fixtures live in HDR and BLOOM can halo them (shaders/composite.frag blooms the
# HDR buffer, then tonemaps). Tuned by LOOKING, not by theory (VALUE NOT LUMENS):
# at 3.0 the panels clipped 2.3% of the frame to white — a blob, a failure. At 1.8
# they clip 0.02%, the same as the untouched baseline.
#
# NOTE what raising this CANNOT do: it cannot desaturate the panel. See CORE_WHITE.
DEFAULT_GAIN = 1.8

# How white the HOT CORE of a fixture goes (see build_maps). 0 = keep the raw key
# hue everywhere (a flat saturated swatch — the failure mode); 1 = a white blob with
# no colour left (the other failure mode). This is the fixture's own behaviour, not
# an exposure hack.
#
# Tuned on BOTH ends of the range, because they fail differently and a single camera
# will lie to you:
#   * the big ceiling PANELS have a soft mask, so their rim keeps the hue no matter
#     what — they tolerate a lot of whitening and want it (pure yellow reads neon).
#   * the thin door/wall STRIPS are a hard mask at alpha 1.0 all the way across, so
#     whiteness hits them everywhere at once. At 0.60 they went PURE WHITE and lost
#     the magenta entirely.
# 0.40 is the value where the panels read as warm lit diffusers AND the strips still
# read as magenta LEDs with a hot core. Judged on the render, both cameras.
CORE_WHITE = 0.40

GLOW_FLOOR = 3          # alpha <= this is "no glow" (all key texels measure >= 3)
DILATE     = 2          # px, to swallow the mask's antialiased fringe in the albedo


def log(*a): print("[msi]", *a, flush=True)


# ---------------------------------------------------------------- key probes
def key_masks(rgb):
    """The pack's emission KEY COLOURS. Used both to repair and (in the test) to
       prove none survived. `rgb` = uint8 HxWx3."""
    R, G, B = (rgb[..., i].astype(np.int32) for i in range(3))
    magenta = (np.abs(R - B) <= 25) & (G < 40) & (np.maximum(R, B) > 100)
    yellow  = (np.abs(R - G) <= 25) & (B < 40) & (np.maximum(R, G) > 200)
    blue    = (B > 150) & (R < 60) & (G < 60)
    green   = (G > 150) & (R < 60) & (B < 60)
    return magenta, yellow, blue, green


def magenta_fraction(rgb):
    """THE probe the regression test asserts on: the classic Unity emission key
       (R ~= B, G ~= 0). Must be exactly 0 in a converted base-colour map."""
    m, _, _, _ = key_masks(rgb)
    return float(m.mean())


# ---------------------------------------------------------------- colour math
def srgb_to_linear(c):
    c = c.astype(np.float32) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    s = np.where(c <= 0.0031308, c * 12.92, 1.055 * (c ** (1.0 / 2.4)) - 0.055)
    return np.clip(s * 255.0 + 0.5, 0, 255).astype(np.uint8)


def dilate(mask, r):
    """Cheap box dilation (no scipy dependency)."""
    out = mask.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            out |= np.roll(np.roll(mask, dy, 0), dx, 1)
    return out


def fit(im, max_tex):
    """Downscale to <= max_tex.

    TRAP: Pillow resamples an RGBA image with ALPHA-PREMULTIPLIED filtering. The
    MRAG map is NOT a colour+alpha image — its four channels are four unrelated
    DATA channels, and its alpha (the glow mask) is ~0 over 95% of the atlas. A
    plain `rgba.resize()` therefore multiplied metallic/roughness/AO by the glow
    mask and crushed them to ~0 (measured: roughness mean 0.22 -> 0.02, i.e. every
    wall became a MIRROR). Resize each channel independently as an L image.
    """
    w, h = im.size
    m = max(w, h)
    if m <= max_tex:
        return im
    s = max_tex / float(m)
    tgt = (max(1, int(w * s)), max(1, int(h * s)))
    if im.mode == "RGBA":
        return Image.merge("RGBA", [c.resize(tgt, Image.LANCZOS) for c in im.split()])
    return im.resize(tgt, Image.LANCZOS)


# ---------------------------------------------------------------- map builders
def build_maps(pack, tex_name, gain, max_tex):
    """-> (albedo_png_bytes, normal, orm, emissive, stats) for one texture set."""
    d = os.path.join(pack, tex_name)
    dif_p  = os.path.join(d, "T_%s_Dif.png"  % tex_name)
    nrm_p  = os.path.join(d, "T_%s_Norm.png" % tex_name)
    mrag_p = os.path.join(d, "T_%s_MRAG.png" % tex_name)
    for p in (dif_p, nrm_p, mrag_p):
        if not os.path.exists(p):
            raise FileNotFoundError(p)

    dif  = np.asarray(fit(Image.open(dif_p).convert("RGB"), max_tex))
    mrag = np.asarray(fit(Image.open(mrag_p).convert("RGBA"), max_tex))
    nrm  = fit(Image.open(nrm_p).convert("RGB"), max_tex)

    glow = mrag[..., 3].astype(np.float32) / 255.0        # per-texel emission mask
    before = magenta_fraction(dif)

    # ---- emissive: the emitter's own colour, in LINEAR light, gated per texel.
    #
    # THE CORE MUST GO WHITE, AND IT HAS TO BE BAKED HERE. The key colours are
    # FULLY saturated — yellow (255,255,0) has ZERO blue, magenta (255,0,255) zero
    # green. This engine tonemaps with the Narkowicz ACES approximation
    # (shaders/composite.frag), which is applied PER CHANNEL: a channel that is 0
    # stays 0 at ANY exposure. So a pure-yellow emitter can never roll off to white
    # no matter how much radiance you give it — it just clamps to flat yellow. That
    # is why raising the strength alone left the ceiling a glowing yellow SWATCH.
    #
    # A real lamp does not look like its filter's hue. Its bright core overwhelms
    # the eye/sensor and reads WHITE, and only the falloff and the spill carry the
    # colour. Since the tonemapper cannot produce that from a saturated primary, we
    # bake it into the map — which is the honest place for it, because it is a
    # property of the FIXTURE, not of the exposure. Whiteness rides the glow mask,
    # so the hot centre desaturates and the soft edge keeps the hue.
    hue = srgb_to_linear(dif)
    white_w = (CORE_WHITE * glow)[..., None]                    # 0 at the rim, CORE_WHITE at the core
    emis_lin = (hue * (1.0 - white_w) + white_w) * glow[..., None]
    emissive = linear_to_srgb(emis_lin)

    # ---- albedo: every glow texel becomes an OFF diffuser. Dilated so the
    #      mask's antialiased fringe cannot leave a rim of half-key colour.
    repair = dilate(mrag[..., 3] >= GLOW_FLOOR, DILATE)
    albedo = dif.copy()
    albedo[repair] = np.array(DIFFUSER_SRGB, np.uint8)
    after = magenta_fraction(albedo)

    # ---- ORM: Unity MRAG (M=R, R=G, AO=B, Glow=A) -> glTF (occl=R, rough=G, metal=B).
    #      NOTE the pack stores ROUGHNESS, not smoothness — G goes straight through
    #      (no 1-x). Verified: the perforated grilles read ~0.9 (rough) and the
    #      painted hull ~0.2 (satin); inverting would make the grilles mirrors.
    orm = np.zeros(dif.shape, np.uint8)
    orm[..., 0] = mrag[..., 2]      # occlusion  <- MRAG.B
    orm[..., 1] = mrag[..., 1]      # roughness  <- MRAG.G
    orm[..., 2] = mrag[..., 0]      # metallic   <- MRAG.R
    metal_mean = float(mrag[..., 0].mean()) / 255.0

    def png(arr_or_im, mode="RGB"):
        im = arr_or_im if isinstance(arr_or_im, Image.Image) else Image.fromarray(arr_or_im, mode)
        b = io.BytesIO(); im.save(b, "PNG", optimize=True); return b.getvalue()

    stats = dict(size=dif.shape[:2], glow_pct=100.0 * float((glow > 0).mean()),
                 magenta_before=100.0 * before, magenta_after=100.0 * after,
                 metal_mean=metal_mean, gain=gain)
    return png(albedo), png(nrm), png(orm), png(emissive), stats


# ---------------------------------------------------------------- GLB re-skin
def tex_for_material(mat_name, pack=DEFAULT_PACK):
    """MI_Ceiling_A -> Ceiling_A ; MI_Door_A_Movable -> Door_A ; MI_Wall_C_Static -> Wall_C.

    The kit's material names carry BEHAVIOUR suffixes (_Movable on the door leaf,
    _Static on the baked wall) that the texture folders do not. Rather than chase a
    suffix blocklist, strip trailing _<word> segments until a folder with a real
    T_<name>_Dif.png exists, and RAISE if none does — a silently unmatched material
    is exactly how the kit lost its maps the first time.
    """
    n = mat_name[3:] if mat_name.startswith("MI_") else mat_name
    cand = n
    while cand:
        if os.path.exists(os.path.join(pack, cand, "T_%s_Dif.png" % cand)):
            return cand
        if "_" not in cand:
            break
        cand = cand.rsplit("_", 1)[0]
    raise KeyError("no texture set in %s for material %r (tried down from %r)"
                   % (pack, mat_name, n))


def reskin(glb_in, glb_out, pack, gain, max_tex, cache):
    from pygltflib import BufferView

    g = GLTF2().load(glb_in)
    # Keep the geometry buffer, drop every old image/bufferView that only fed a
    # texture. Simplest correct move: rebuild the blob = [old geometry bytes that
    # the surviving bufferViews point at] + [new PNGs]. The old GLBs put their
    # images in bufferViews AFTER the mesh data, and pygltflib keeps byteOffsets,
    # so we just append and let the tail images be orphaned... which would still
    # ship their bytes. So: rewrite the blob from scratch, remapping bufferViews.
    old_blob = g.binary_blob()
    keep = set()
    for a in g.accessors:
        if a.bufferView is not None:
            keep.add(a.bufferView)
    new_blob = bytearray()
    remap = {}
    new_views = []
    for i, bv in enumerate(g.bufferViews):
        if i not in keep:
            continue
        off = bv.byteOffset or 0
        data = old_blob[off: off + bv.byteLength]
        while len(new_blob) % 4:
            new_blob.append(0)
        remap[i] = len(new_views)
        new_views.append(BufferView(buffer=0, byteOffset=len(new_blob),
                                    byteLength=bv.byteLength,
                                    byteStride=bv.byteStride, target=bv.target))
        new_blob += data
    for a in g.accessors:
        if a.bufferView is not None:
            a.bufferView = remap[a.bufferView]
    g.bufferViews = new_views

    g.images = []
    g.textures = []
    if not g.samplers:
        g.samplers.append(Sampler())

    def add(blob):
        while len(new_blob) % 4:
            new_blob.append(0)
        bvi = len(g.bufferViews)
        g.bufferViews.append(BufferView(buffer=0, byteOffset=len(new_blob), byteLength=len(blob)))
        new_blob.extend(blob)
        idx = len(g.images)
        g.images.append(GImage(bufferView=bvi, mimeType="image/png"))
        g.textures.append(Texture(source=idx, sampler=0))
        return len(g.textures) - 1

    allstats = []
    for m in g.materials:
        tname = tex_for_material(m.name, pack)
        if tname not in cache:
            cache[tname] = build_maps(pack, tname, gain, max_tex)
        alb, nrm, orm, emis, st = cache[tname]
        allstats.append((m.name, tname, st))

        if m.pbrMetallicRoughness is None:
            m.pbrMetallicRoughness = PbrMetallicRoughness()
        p = m.pbrMetallicRoughness
        p.baseColorTexture = TextureInfo(index=add(alb))
        p.baseColorFactor = [1.0, 1.0, 1.0, 1.0]
        m.normalTexture = NormalMaterialTexture(index=add(nrm))

        orm_t = add(orm)
        p.metallicRoughnessTexture = TextureInfo(index=orm_t)   # glTF reads G=rough, B=metal
        m.occlusionTexture = OcclusionTextureInfo(index=orm_t)  # ...and R=occlusion
        # L5: the factors MULTIPLY the texture. 1.0 here means "use the map as
        # authored" — the map's own metal channel is ~0 over the panels. Leaving
        # metallicFactor at glTF's 1.0 default WITHOUT a map is what turns props
        # into black full-metal; with the map, 1.0 is correct.
        p.metallicFactor = 1.0
        p.roughnessFactor = 1.0

        # The emissive TEXTURE carries the colour + the per-texel falloff; the
        # factor stays white and the STRENGTH extension carries the radiance, so
        # the fixture can exceed 1.0 and let ACES roll its core off to white.
        m.emissiveTexture = TextureInfo(index=add(emis))
        m.emissiveFactor = [1.0, 1.0, 1.0]
        ext = dict(m.extensions) if m.extensions else {}
        ext["KHR_materials_emissive_strength"] = {"emissiveStrength": float(gain)}
        m.extensions = ext

    for e in ("KHR_materials_emissive_strength",):
        if e not in (g.extensionsUsed or []):
            g.extensionsUsed = (g.extensionsUsed or []) + [e]

    g.set_binary_blob(bytes(new_blob))
    g.buffers[0].byteLength = len(new_blob)
    g.buffers[0].uri = None
    os.makedirs(os.path.dirname(os.path.abspath(glb_out)), exist_ok=True)
    g.save_binary(glb_out)
    return allstats, os.path.getsize(glb_out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pack", default=DEFAULT_PACK)
    ap.add_argument("--glb-dir", default=DEFAULT_GLB)
    ap.add_argument("--out", default=None, help="default: in place (--glb-dir)")
    ap.add_argument("--emissive-gain", type=float, default=DEFAULT_GAIN)
    ap.add_argument("--max-tex", type=int, default=2048)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    out_dir = a.out or a.glb_dir
    glbs = sorted(f for f in os.listdir(a.glb_dir) if f.lower().endswith(".glb"))
    if not glbs:
        log("no GLBs in", a.glb_dir); return 1
    cache = {}
    total = 0
    for f in glbs:
        src = os.path.join(a.glb_dir, f)
        dst = os.path.join(out_dir, f)
        if a.dry_run:
            g = GLTF2().load(src)
            for m in g.materials:
                log("DRY %-22s mat=%-22s -> %s" % (f, m.name, tex_for_material(m.name, a.pack)))
            continue
        stats, size = reskin(src, dst, a.pack, a.emissive_gain, a.max_tex, cache)
        total += size
        for mat, tname, st in stats:
            log("%-22s %-22s glow=%5.2f%%  magenta %.4f%% -> %.4f%%  metal(mean)=%.3f  %.1f MB"
                % (f, tname, st["glow_pct"], st["magenta_before"], st["magenta_after"],
                   st["metal_mean"], size / 1e6))
    if not a.dry_run:
        log("DONE %d GLBs, %.1f MB total, emissiveFactor=%.2f, maxTex=%d"
            % (len(glbs), total / 1e6, a.emissive_gain, a.max_tex))
    return 0


if __name__ == "__main__":
    sys.exit(main())
