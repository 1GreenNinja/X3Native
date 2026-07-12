"""
forge_gate_textures.py — SD3.5 PBR forge for the RIFTHUB gate (ROUND 3 lane 1).

DO NOT run while Tim is in the game — one generation at a time on the 5090.
Owner fires it when the GPU is free:

    python tools/forge_gate_textures.py --all
    python tools/forge_gate_textures.py --set gate_ring_plate

Generates gate-SPECIFIC tileable PBR sets (albedo via SD3.5-Large, height from
luminance, normal from height via Sobel, mr glTF-packed G=roughness B=metallic)
into  G:\\Assets\\X3Native\\surface_library\\<set>\\{albedo,normal,mr,height}.png
— the same set convention app/surface_library.h loads (height.png is extra, for
future parallax). See docs/FORGE_GATE_TEXTURES.md for landing instructions.

Model: local SD3.5 diffusers checkout (C:\\GameDev\\SD_Models\\sd35 — the proven
path from the surface-library forge; NOT the broken :7860 WebUI). Deps:
    pip install torch diffusers transformers accelerate pillow numpy
Clean-room: our own prompts + numpy post; no third-party texture content.
"""
import argparse, gc, os, sys

import numpy as np
from PIL import Image, ImageFilter

MODEL_PATH = r"D:\GameDev\SD_Models\sd35"
OUT_ROOT   = r"G:\Assets\X3Native\surface_library"
# Land locally too — the engine loads assetRoot()/surface_library/<set>, and G: is
# the fleet share. Both get the same bytes.
LOCAL_ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "..", "assets", "surface_library")
SIZE       = 1024

NEG = ("blurry, soft focus, photo border, watermark, text, logo, seams, "
       "perspective, vanishing point, fisheye,人物, people, hands, sky, "
       "depth of field, bokeh, frame, vignette")

# ---------------------------------------------------------------------------
# ROUND 5 — IMG2IMG FROM THE OWNER'S OWN REFERENCE ("textures from SD3.5 BASED
# ON Grok's image"). Each set names a CROP BOX in docs/reference/PortalAnimated.mp4's
# highest-res frame (1168x768; the stills are only 800x526) — the material region
# of the real gate — which we upscale, img2img at a low-ish strength (keeps the
# source's character: rivet rows, panel seams, rust bleed, teal oxide), de-light,
# tile, and decompose into the engine's PBR set. --from-image <png> overrides the
# crop with any concept image the owner drops.
#
# MATERIAL LAW (round 5 fix, the "no light response / ghost grey slabs" bug): the
# round-4 sets were forged at metallic 0.82-0.98. A near-pure metal has NO diffuse
# term — in a dark hall with a handful of point lights it can only show what it
# REFLECTS, and with roughness pushed past the SSR mirror cutoff (the ghost-glass
# fix) it reflects nothing but the dim IBL probe => flat grey cardboard, zero
# highlight/shadow separation. The reference's ring is PAINTED, weathered steel:
# mostly dielectric with metal showing through the wear. Metallic comes DOWN hard
# (0.27-0.78), roughness stays >= 0.62 (keeps the SSR X-ray cured), and the normal
# strength goes UP so the rivets/seams actually carve light.
REF_VIDEO  = "docs/reference/PortalAnimated.mp4"
REF_TIME   = 1.0        # seconds — the gate is fully lit and unoccluded here
# set -> crop box (x0, y0, x1, y1) in the 1168x768 reference frame
# Target ALBEDO mean (sRGB 0..1) per set. An albedo is a REFLECTANCE map, not a
# photo: the reference crop is a LIT render, and de-lighting it to its own dark
# average produced albedos at sRGB 0.15-0.28 == LINEAR 0.02-0.06, i.e. coal. Lit
# by a dim interior rig the gate then rendered pure black once its (bogus) self
# -emissive was removed. Real weathered painted steel reflects ~35-45%; machined
# gunmetal ~20%. Normalize to these and the light rig finally has something to
# bounce off.
ALBEDO_MEAN = {
    "gate_ring_plate":   0.33,
    "gate_patina_plate": 0.30,
    "gate_piston_steel": 0.24,
}

REF_CROPS = {
    "gate_ring_plate":   (300, 140, 480, 320),   # riveted plate band + rust bleed
    "gate_patina_plate": (330,  25, 510, 205),   # teal-oxide weathered plates
    "gate_piston_steel": (386, 588, 566, 766),   # dark machined hardware + bolts
}

# name -> (prompt, roughness 0..255, metallic 0..255, normal_strength)
SETS = {
    # The hero set: the big weathered ring plates — greebles BAKED into the
    # texture so the normal map carries rivet/panel detail the mesh doesn't.
    "gate_ring_plate": (
        "orthographic top-down photo texture of heavy riveted industrial steel "
        "armor plating, weathered grey-white paint over dark metal, teal-oxide "
        "patina streaks, panel seams with recessed bolt lines, small vents and "
        "machined greebles, scratches and edge wear, flat even lighting, "
        "seamless tileable material, 4k pbr albedo",
        170, 90, 14.0),  # rough .67 / metal .35 (painted armor) / deep relief
    # Rust-streak patina variant for the over-plates / base shoulders.
    "gate_patina_plate": (
        "orthographic top-down photo texture of weathered steel plate with "
        "heavy rust streaks bleeding down from bolt heads, chipped teal-green "
        "oxide paint, grime buildup in panel recesses, industrial wear, flat "
        "even lighting, seamless tileable material, 4k pbr albedo",
        195, 70, 13.0),  # rough .76 / metal .27 (chipped oxide paint)
    # Dark piston/hardware steel for clamps, rods, pipes, bolts.
    "gate_piston_steel": (
        "orthographic top-down photo texture of dark gunmetal machined steel, "
        "fine brushed grain, faint oil sheen, subtle scratches and tooling "
        "marks, small hex bolts, near-black industrial hardware metal, flat "
        "even lighting, seamless tileable material, 4k pbr albedo",
        165, 200, 10.0), # rough .65 / metal .78 (machined steel, still past the SSR cutoff)
}


def make_tileable(img: Image.Image, blend_px: int = 96) -> Image.Image:
    """Offset-and-blend: wrap the image by half, cross-fade the seam cross."""
    a = np.asarray(img).astype(np.float32)
    h, w = a.shape[:2]
    r = np.roll(np.roll(a, h // 2, axis=0), w // 2, axis=1)
    # Blend a cross-shaped band (the rolled seams sit at the center lines).
    yy = np.abs(np.arange(h) - h / 2.0)
    xx = np.abs(np.arange(w) - w / 2.0)
    wy = np.clip(yy / blend_px, 0, 1)[:, None]
    wx = np.clip(xx / blend_px, 0, 1)[None, :]
    k = (np.minimum(wy, wx))[..., None]          # 0 at seam cross -> 1 away
    # r is the original with its seams rolled to the center lines; the original
    # a is seamless THERE, so cross-fade toward a near the seam cross. The
    # result wraps cleanly at the image border (where r == the original seam
    # neighborhood, now blended) — cheap but effective for grungy metal.
    out = r * k + a * (1 - k)
    return Image.fromarray(np.clip(out, 0, 255).astype(np.uint8))


def normalize_albedo(img: Image.Image, target: float) -> Image.Image:
    """Scale the albedo so its mean luminance lands on `target` (sRGB 0..1), keeping
    hue and local contrast. Gamma-correct: the scale is applied in LINEAR space."""
    a = np.asarray(img).astype(np.float32) / 255.0
    lin = np.power(a, 2.2)
    cur = float(np.mean(np.power(np.mean(a, axis=2), 2.2)))
    tgt_lin = target ** 2.2
    k = tgt_lin / max(cur, 1e-5)
    out = np.power(np.clip(lin * k, 0.0, 1.0), 1.0 / 2.2)
    return Image.fromarray((out * 255.0).astype(np.uint8))


def height_from_albedo(albedo: Image.Image, gain: float = 2.6,
                       blur: int = 10) -> np.ndarray:
    """Luminance -> height in [0,1]. ROUND 5: the 24 px / 1.6-gain recipe was tuned
    for busy text-forged noise; on the smoother img2img plates it yielded a normal
    map with ~1.4% xy deviation — i.e. FLAT, which is exactly why the gate showed
    no light response no matter how good the albedo was. Tighter blur + more gain =
    rivets, seams and bolt heads that actually carve the light."""
    g = np.asarray(albedo.convert("L")).astype(np.float32) / 255.0
    lo = np.asarray(albedo.convert("L").filter(
        ImageFilter.GaussianBlur(blur))).astype(np.float32) / 255.0
    h = np.clip(0.5 + (g - lo) * gain, 0, 1)      # local contrast = surface relief
    return h


def normal_from_height(h: np.ndarray, strength: float) -> Image.Image:
    """Sobel gradients -> tangent-space normal (OpenGL +Y convention)."""
    gx = np.zeros_like(h); gy = np.zeros_like(h)
    gx[:, 1:-1] = (h[:, 2:] - h[:, :-2]) * 0.5
    gy[1:-1, :] = (h[2:, :] - h[:-2, :]) * 0.5
    nx, ny = -gx * strength, gy * strength
    nz = np.ones_like(h)
    ln = np.sqrt(nx * nx + ny * ny + nz * nz)
    n = np.stack([nx / ln, ny / ln, nz / ln], axis=-1)
    return Image.fromarray(((n * 0.5 + 0.5) * 255).astype(np.uint8))


def pack_mr(h: np.ndarray, rough: int, metal: int, rough_floor: int = 160) -> Image.Image:
    """glTF packing: G=roughness B=metallic (surface_library.h channel law).
    Roughness varies +/-18 with cavity (recesses rougher/dirtier), but never
    below rough_floor (=0.627): mesh.frag's mirror-reflection gate opens under
    roughness 0.6 and the half-res SSR march tunnels through the gate's thin
    plates -> the ROUND-4 ghost-glass X-ray. The floor is the cure, baked in."""
    rvar = np.clip(rough + (0.5 - h) * 36.0, rough_floor, 255).astype(np.uint8)
    mr = np.zeros((h.shape[0], h.shape[1], 3), np.uint8)
    mr[..., 1] = rvar
    mr[..., 2] = metal
    return Image.fromarray(mr)


def ref_frame() -> Image.Image:
    """Grab the highest-res frame of the owner's reference video (the stills are
    only 800x526; the video is 1168x768)."""
    import subprocess, tempfile
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    vid = os.path.join(root, REF_VIDEO.replace("/", os.sep))
    tmp = os.path.join(tempfile.gettempdir(), "rifthub_ref_frame.png")
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-ss", str(REF_TIME),
                    "-i", vid, "-frames:v", "1", tmp], check=True)
    return Image.open(tmp).convert("RGB")


def delight(img: Image.Image, amount: float = 0.85) -> Image.Image:
    """Flatten the crop's BAKED lighting: divide out the low-frequency luminance
    (the concept render is lit from above-left; an albedo must not carry that) and
    re-center to mid grey, keeping all the high-frequency material detail."""
    a = np.asarray(img).astype(np.float32)
    lum = a.mean(axis=2, keepdims=True)
    lo = np.asarray(Image.fromarray(np.clip(lum[..., 0], 0, 255).astype(np.uint8))
                    .filter(ImageFilter.GaussianBlur(64))).astype(np.float32)[..., None]
    lo = np.maximum(lo, 8.0)
    flat = a / lo * float(np.mean(lo))                 # remove the lighting gradient
    out = a * (1.0 - amount) + flat * amount
    return Image.fromarray(np.clip(out, 0, 255).astype(np.uint8))


def forge_img2img(name: str, steps: int, seed: int, strength: float,
                  src_path: str | None):
    """ROUND 5 — SD3.5 IMG2IMG *from the owner's reference image*. The source is a
    crop of the real gate's material (or any --from-image the owner drops), upscaled
    to SIZE; img2img at `strength` keeps the source's character (rivet rows, panel
    seams, rust bleed, oxide colour) while regenerating it as a flat, tileable
    material. Then: de-light -> tile -> height -> normal -> MR."""
    import torch
    from diffusers import StableDiffusion3Img2ImgPipeline
    prompt, rough, metal, nstr = SETS[name]

    if src_path:
        src = Image.open(src_path).convert("RGB")
    else:
        box = REF_CROPS[name]
        src = ref_frame().crop(box)
        print(f"[forge] {name}: source = reference frame crop {box}")
    src = src.resize((SIZE, SIZE), Image.LANCZOS)

    print(f"[forge] {name}: loading SD3.5 img2img from {MODEL_PATH}")
    pipe = StableDiffusion3Img2ImgPipeline.from_pretrained(
        MODEL_PATH,
        text_encoder_3=None, tokenizer_3=None,   # drop T5-XXL (~9 GB VRAM)
        torch_dtype=torch.float16)
    pipe.enable_model_cpu_offload()              # never near the 32 GB ceiling
    gen = torch.Generator("cuda").manual_seed(seed)
    print(f"[forge] {name}: img2img {SIZE}x{SIZE} strength={strength} steps={steps}")
    img = pipe(prompt=prompt, negative_prompt=NEG, image=src, strength=strength,
               num_inference_steps=steps, guidance_scale=5.0,
               generator=gen).images[0]
    del pipe
    gc.collect()
    torch.cuda.empty_cache()

    albedo = normalize_albedo(make_tileable(delight(img)), ALBEDO_MEAN[name])
    h = height_from_albedo(albedo)
    normal = normal_from_height(h, nstr)
    mr = pack_mr(h, rough, metal)
    write_set(name, albedo, normal, mr, h)
    # Keep the exact source crop next to the set: provenance + re-runnable.
    for root in _roots():
        try:
            src.save(os.path.join(root, name, "_source_crop.png"))
        except OSError:
            pass


def remap(name: str):
    """Re-derive height/normal/mr from the ALREADY-LANDED albedo (no GPU). The map
    recipe is the tunable half of the forge — this lets us iterate relief/roughness
    against the engine without burning another diffusion pass."""
    _, rough, metal, nstr = SETS[name]
    src = os.path.join(LOCAL_ROOT, name, "albedo.png")
    albedo = normalize_albedo(Image.open(src).convert("RGB"), ALBEDO_MEAN[name])
    h = height_from_albedo(albedo)
    write_set(name, albedo, normal_from_height(h, nstr), pack_mr(h, rough, metal), h)


def _roots():
    out = []
    for r in (OUT_ROOT, LOCAL_ROOT):
        try:
            os.makedirs(r, exist_ok=True)
            out.append(r)
        except OSError as e:
            print(f"[forge] WARN: root unavailable ({r}): {e}")
    return out


def write_set(name, albedo, normal, mr, h):
    for root in _roots():
        d = os.path.join(root, name)
        os.makedirs(d, exist_ok=True)
        albedo.save(os.path.join(d, "albedo.png"))
        normal.save(os.path.join(d, "normal.png"))
        mr.save(os.path.join(d, "mr.png"))
        Image.fromarray((h * 255).astype(np.uint8)).save(os.path.join(d, "height.png"))
        print(f"[forge] {name}: wrote {d}")


def forge(name: str, steps: int, seed: int):
    import torch
    from diffusers import StableDiffusion3Pipeline
    prompt, rough, metal, nstr = SETS[name]
    print(f"[forge] {name}: loading SD3.5 from {MODEL_PATH}")
    pipe = StableDiffusion3Pipeline.from_pretrained(MODEL_PATH,
                                                    text_encoder_3=None, tokenizer_3=None,  # drop T5-XXL: ~9GB VRAM, not needed for texture prompts
                                                    torch_dtype=torch.float16)
    pipe.enable_model_cpu_offload()  # stage modules: never near the 32GB ceiling (prevents sysmem-fallback thrash)
    pipe.to("cuda")
    gen = torch.Generator("cuda").manual_seed(seed)
    print(f"[forge] {name}: generating {SIZE}x{SIZE} ({steps} steps)")
    img = pipe(prompt=prompt, negative_prompt=NEG, width=SIZE, height=SIZE,
               num_inference_steps=steps, guidance_scale=5.0,
               generator=gen).images[0]
    # Free the pipe BEFORE post (one gen at a time on the 5090 — VRAM guard).
    del pipe
    gc.collect()
    torch.cuda.empty_cache()

    albedo = make_tileable(img)
    h = height_from_albedo(albedo)
    normal = normal_from_height(h, nstr)
    mr = pack_mr(h, rough, metal)

    out_dir = os.path.join(OUT_ROOT, name)
    os.makedirs(out_dir, exist_ok=True)
    albedo.save(os.path.join(out_dir, "albedo.png"))
    normal.save(os.path.join(out_dir, "normal.png"))
    mr.save(os.path.join(out_dir, "mr.png"))
    Image.fromarray((h * 255).astype(np.uint8)).save(
        os.path.join(out_dir, "height.png"))
    print(f"[forge] {name}: wrote {out_dir}\\{{albedo,normal,mr,height}}.png")


def main():
    ap = argparse.ArgumentParser(description="SD3.5 gate PBR forge (one at a time)")
    ap.add_argument("--set", dest="set_name", choices=sorted(SETS),
                    help="forge one set")
    ap.add_argument("--all", action="store_true", help="forge all gate sets")
    ap.add_argument("--steps", type=int, default=32)
    ap.add_argument("--seed", type=int, default=1147)
    # ROUND 5: img2img FROM the owner's reference (default for the gate sets now).
    ap.add_argument("--img2img", action="store_true",
                    help="derive the set FROM the reference-frame crop (REF_CROPS)")
    ap.add_argument("--from-image", dest="from_image", default=None,
                    help="img2img source PNG (overrides the reference crop)")
    ap.add_argument("--strength", type=float, default=0.45,
                    help="img2img denoise strength: low keeps the source's character")
    ap.add_argument("--maps-only", dest="maps_only", action="store_true",
                    help="re-derive height/normal/mr from the landed albedo.png (no GPU)")
    args = ap.parse_args()
    names = sorted(SETS) if args.all else ([args.set_name] if args.set_name else [])
    if not names:
        ap.error("pass --all or --set <name>")
    for i, n in enumerate(names):
        if args.maps_only:
            remap(n)
            continue
        if args.img2img or args.from_image:
            forge_img2img(n, args.steps, args.seed + i * 101, args.strength,
                          args.from_image)
        else:
            forge(n, args.steps, args.seed + i * 101)
    print("[forge] done — see docs/FORGE_GATE_TEXTURES.md to land the sets")


if __name__ == "__main__":
    sys.exit(main())
