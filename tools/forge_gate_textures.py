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

MODEL_PATH = r"C:\GameDev\SD_Models\sd35"
OUT_ROOT   = r"G:\Assets\X3Native\surface_library"
SIZE       = 1024

NEG = ("blurry, soft focus, photo border, watermark, text, logo, seams, "
       "perspective, vanishing point, fisheye,人物, people, hands, sky, "
       "depth of field, bokeh, frame, vignette")

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
        170, 235, 2.2),
    # Rust-streak patina variant for the over-plates / base shoulders.
    "gate_patina_plate": (
        "orthographic top-down photo texture of weathered steel plate with "
        "heavy rust streaks bleeding down from bolt heads, chipped teal-green "
        "oxide paint, grime buildup in panel recesses, industrial wear, flat "
        "even lighting, seamless tileable material, 4k pbr albedo",
        200, 210, 2.0),
    # Dark piston/hardware steel for clamps, rods, pipes, bolts.
    "gate_piston_steel": (
        "orthographic top-down photo texture of dark gunmetal machined steel, "
        "fine brushed grain, faint oil sheen, subtle scratches and tooling "
        "marks, small hex bolts, near-black industrial hardware metal, flat "
        "even lighting, seamless tileable material, 4k pbr albedo",
        110, 250, 1.2),
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


def height_from_albedo(albedo: Image.Image) -> np.ndarray:
    """Luminance -> blurred height in [0,1] (the proven surface-library recipe)."""
    g = np.asarray(albedo.convert("L")).astype(np.float32) / 255.0
    lo = np.asarray(albedo.convert("L").filter(
        ImageFilter.GaussianBlur(24))).astype(np.float32) / 255.0
    h = np.clip(0.5 + (g - lo) * 1.6, 0, 1)       # local contrast = surface relief
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


def pack_mr(h: np.ndarray, rough: int, metal: int) -> Image.Image:
    """glTF packing: G=roughness B=metallic (surface_library.h channel law).
    Roughness varies +/-18 with cavity (recesses rougher/dirtier)."""
    rvar = np.clip(rough + (0.5 - h) * 36.0, 0, 255).astype(np.uint8)
    mr = np.zeros((h.shape[0], h.shape[1], 3), np.uint8)
    mr[..., 1] = rvar
    mr[..., 2] = metal
    return Image.fromarray(mr)


def forge(name: str, steps: int, seed: int):
    import torch
    from diffusers import StableDiffusion3Pipeline
    prompt, rough, metal, nstr = SETS[name]
    print(f"[forge] {name}: loading SD3.5 from {MODEL_PATH}")
    pipe = StableDiffusion3Pipeline.from_pretrained(MODEL_PATH,
                                                    torch_dtype=torch.float16)
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
    args = ap.parse_args()
    names = sorted(SETS) if args.all else ([args.set_name] if args.set_name else [])
    if not names:
        ap.error("pass --all or --set <name>")
    for i, n in enumerate(names):
        forge(n, args.steps, args.seed + i * 101)
    print("[forge] done — see docs/FORGE_GATE_TEXTURES.md to land the sets")


if __name__ == "__main__":
    sys.exit(main())
