"""
forge_concrete_deck.py — SD3.5 photoreal CONCRETE tile for the freeway deck
(V7.2, Tim: "the bridge still looks like asphalt not concrete").

DO NOT run while Tim is in the game — one generation at a time on the 5090.

    python tools/forge_concrete_deck.py

Generates a tileable 1024 photoreal bridge-deck concrete albedo straight into
assets/roads/concrete_tile.png (V7 loader path; old tile backed up alongside as
concrete_tile.prev.png). Same pipeline conventions as forge_gate_textures.py
(local SD3.5 diffusers checkout, fp16 + cpu offload, offset-blend tiling,
linear-space albedo normalization). Clean-room prompt; no third-party content.
"""
import gc
import os

import numpy as np
from PIL import Image

MODEL_PATH = r"D:\GameDev\SD_Models\sd35"
OUT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", "assets", "roads", "concrete_tile.png")
SIZE = 1024
SEED = 20260730
STEPS = 40

# Weathered CA viaduct pour: pale portland grey, fine aggregate speckle, light
# broom finish (kept non-directional-ish so ribbon UV orientation can't fight
# it), hairline cracks + patch repairs + faint tire wear. Flat light: albedo.
PROMPT = ("orthographic top-down photo texture of smooth finished concrete "
          "highway pavement, one uniform continuous field of pale portland "
          "cement grey, lightly troweled surface, very fine sand speckle, "
          "faint hairline cracks, subtle grey wear patina, sun-bleached, "
          "flat even lighting, seamless tileable material, 4k pbr albedo")
NEG = ("blurry, soft focus, photo border, watermark, text, logo, seams, "
       "perspective, vanishing point, fisheye, people, hands, sky, "
       "depth of field, bokeh, frame, vignette, asphalt, black tar, "
       "expansion joint, lane markings, painted lines, wall, panels, "
       "stripes, bands, columns, bricks, blocks, tiles, grid")

# Real sun-bleached deck concrete reflects ~50%; the V7 bucket tint (0.95)
# multiplies on top.
ALBEDO_MEAN = 0.52


def make_tileable(img: Image.Image, blend_px: int = 160) -> Image.Image:
    """Offset-and-blend (forge_gate_textures.py recipe)."""
    a = np.asarray(img).astype(np.float32)
    h, w = a.shape[:2]
    r = np.roll(np.roll(a, h // 2, axis=0), w // 2, axis=1)
    yy = np.abs(np.arange(h) - h / 2.0)
    xx = np.abs(np.arange(w) - w / 2.0)
    wy = np.clip(yy / blend_px, 0, 1)[:, None]
    wx = np.clip(xx / blend_px, 0, 1)[None, :]
    k = (np.minimum(wy, wx))[..., None]
    out = r * k + a * (1 - k)
    return Image.fromarray(np.clip(out, 0, 255).astype(np.uint8))


def normalize_albedo(img: Image.Image, target: float) -> Image.Image:
    """Mean luminance -> target, scaled in linear space (forge recipe)."""
    a = np.asarray(img).astype(np.float32) / 255.0
    lin = np.power(a, 2.2)
    cur = float(np.mean(np.power(np.mean(a, axis=2), 2.2)))
    k = (target ** 2.2) / max(cur, 1e-5)
    out = np.power(np.clip(lin * k, 0.0, 1.0), 1.0 / 2.2)
    return Image.fromarray((out * 255.0).astype(np.uint8))


def main() -> None:
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--seeds", type=int, nargs="*", default=[SEED],
                    help="candidate seeds; with --cand-dir each writes there")
    ap.add_argument("--cand-dir", default=None,
                    help="write concrete_cand_<seed>.png here instead of the tile")
    ap.add_argument("--pick", default=None,
                    help="skip generation: install this png as the tile")
    args = ap.parse_args()

    out_path = os.path.abspath(OUT_PATH)
    prev = out_path.replace("concrete_tile.png", "concrete_tile.prev.png")
    if args.pick:
        if os.path.exists(out_path) and not os.path.exists(prev):
            os.replace(out_path, prev)
        Image.open(args.pick).save(out_path)
        print(f"[forge] installed {args.pick} -> {out_path}")
        return

    import torch
    from diffusers import StableDiffusion3Pipeline
    print(f"[forge] loading SD3.5 from {MODEL_PATH}")
    pipe = StableDiffusion3Pipeline.from_pretrained(
        MODEL_PATH,
        text_encoder_3=None, tokenizer_3=None,   # drop T5-XXL (~9 GB VRAM)
        torch_dtype=torch.float16)
    pipe.enable_model_cpu_offload()
    for seed in args.seeds:
        gen = torch.Generator("cuda").manual_seed(seed)
        print(f"[forge] txt2img {SIZE}x{SIZE} steps={STEPS} seed={seed}")
        img = pipe(prompt=PROMPT, negative_prompt=NEG,
                   width=SIZE, height=SIZE,
                   num_inference_steps=STEPS, guidance_scale=5.0,
                   generator=gen).images[0]
        out = normalize_albedo(make_tileable(img), ALBEDO_MEAN)
        if args.cand_dir:
            p = os.path.join(args.cand_dir, f"concrete_cand_{seed}.png")
            out.save(p)
            print(f"[forge] candidate -> {p}")
        else:
            if os.path.exists(out_path) and not os.path.exists(prev):
                os.replace(out_path, prev)
                print(f"[forge] previous tile -> {prev}")
            out.save(out_path)
            print(f"[forge] wrote {out_path}")
    del pipe
    gc.collect()
    torch.cuda.empty_cache()


if __name__ == "__main__":
    main()
