"""
make_membrane_flipbook.py — bake the RIFTHUB membrane FLIPBOOK atlas from the
owner's reference video (ROUND 4 J2, docs/RIFTHUB_ART_TARGET.md: "steal Grok's
pixels").

    python tools/make_membrane_flipbook.py [--video PATH] [--t0 S] [--t1 S]
                                           [--frames N] [--tile PX] [--out PATH]

WHAT IT DOES
  1. Extracts N (default 48) evenly-spaced frames from docs/reference/
     PortalAnimated.mp4 via ffmpeg. The 10 s video plays the full activation
     arc (IDLE nebula ~0-3 s, SURGE vortex ~3-7 s, OPEN throat ~7-10 s); the
     engine wires this atlas as the IDLE state's plasma layer (SURGE/OPEN keep
     their approved engine behaviours compositing on top), so the DEFAULT span
     samples only the idle segment (t 0.0..3.2 s). Override --t0/--t1 to bake
     other spans (e.g. a throat flipbook later).
  2. Detects the circular membrane disc (it stays centered in the video):
     threshold the blue-dominant bright pixels over the mid frame, take the
     centroid + the 99th-percentile radius, and center-crops a square around
     it (side = 2 * r * 1.03).
  3. Applies a RADIAL mask — opaque center -> transparent rim (smoothstep from
     86% to 100% of the tile half-width) — written to BOTH the alpha channel
     (library reuse) and multiplied into RGB (the engine's PBR emissive path
     samples RGB only; a black rim melts into the membrane disk's dark edge
     the same way the procedural nebula's dark falloff does).
  4. Downscales each frame to --tile (256) and packs a row-major 8x6 atlas
     (2048x1536 at the defaults) -> assets/textures/rifthub/
     membrane_flipbook.png, plus the fleet-convention copy at
     G:/Assets/X3Native/surface_library/membrane_flipbook/.
  5. LOOP-BLEND: the last kLoopBlend frames are crossfaded toward the first
     frames INSIDE the atlas, so the engine can run a plain modulo loop with
     no wrap pop (no runtime crossfade needed for the seam).

Deps: ffmpeg on PATH + Pillow + numpy (the SD-forge python env has both).
Clean-room: our own reference video; no third-party art consulted.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

kCols, kRows = 8, 6
kLoopBlend = 8          # tail frames crossfaded toward the head (seamless loop)
kMaskStart = 0.80       # radial mask: fully opaque inside this fraction of half-width
kFleetDir = r"G:\Assets\X3Native\surface_library\membrane_flipbook"


def log(*a):
    print("[flipbook]", *a, flush=True)


def extract_frames(video, t0, t1, n, tmpdir):
    """Extract n evenly-spaced frames in [t0, t1] as PNGs; returns paths."""
    paths = []
    for i in range(n):
        t = t0 + (t1 - t0) * i / max(1, n - 1)
        out = os.path.join(tmpdir, "f%03d.png" % i)
        cmd = ["ffmpeg", "-v", "error", "-ss", "%.4f" % t, "-i", video,
               "-frames:v", "1", "-y", out]
        subprocess.run(cmd, check=True)
        if not os.path.isfile(out):
            raise RuntimeError("ffmpeg produced no frame at t=%.3f" % t)
        paths.append(out)
    return paths


def detect_disc(img_rgb):
    """Centroid + radius of the bright blue membrane disc (numpy RGB array).

    Detection runs on the OPEN-state frame (t ~9.3 s) where the disc is the
    single dominant bright-blue blob; the whole scene is blue-graded, so the
    thresholds are strict and an outlier-rejection pass drops the stray lit
    pixels (neon coils / holo screens) before the radius is measured."""
    f = img_rgb.astype(np.float32) / 255.0
    r, g, b = f[..., 0], f[..., 1], f[..., 2]
    lum = 0.299 * r + 0.587 * g + 0.114 * b
    mask = (lum > 0.50) & (b > r * 1.25) & (b > 0.55)
    ys, xs = np.nonzero(mask)
    if len(xs) < 500:
        raise RuntimeError("disc detection failed (%d px)" % len(xs))
    cx, cy = xs.mean(), ys.mean()
    for _ in range(3):                       # reject stray bright-blue dressing
        d = np.hypot(xs - cx, ys - cy)
        keep = d < 1.6 * np.median(d)
        xs, ys = xs[keep], ys[keep]
        cx, cy = xs.mean(), ys.mean()
    rad = np.percentile(np.hypot(xs - cx, ys - cy), 97.0) * 1.06
    return float(cx), float(cy), float(rad)


def radial_mask(tile):
    """(tile,tile) float mask: 1 inside kMaskStart, smoothstep to 0 at the rim."""
    ax = (np.arange(tile) + 0.5) / tile * 2.0 - 1.0
    xx, yy = np.meshgrid(ax, ax)
    rr = np.sqrt(xx * xx + yy * yy)
    t = np.clip((rr - kMaskStart) / (1.0 - kMaskStart), 0.0, 1.0)
    return 1.0 - (t * t * (3.0 - 2.0 * t))          # smoothstep fade-out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", default=os.path.join(REPO, "docs", "reference",
                                                    "PortalAnimated.mp4"))
    ap.add_argument("--t0", type=float, default=0.0)
    ap.add_argument("--t1", type=float, default=3.2,
                    help="end of the sampled span (default = end of the IDLE segment)")
    ap.add_argument("--frames", type=int, default=kCols * kRows)
    ap.add_argument("--tile", type=int, default=256)
    ap.add_argument("--out", default=os.path.join(REPO, "assets", "textures",
                                                  "rifthub", "membrane_flipbook.png"))
    # Disc crop (video-space px). Defaults are CALIBRATED to PortalAnimated.mp4
    # by overlay eyeball (docs/reference; the gate is static and slightly 3/4 —
    # the auto-detector kept grabbing the blue-graded hall, so measured wins;
    # pass --auto to re-detect for a different video).
    ap.add_argument("--cx", type=float, default=629.0)
    ap.add_argument("--cy", type=float, default=420.0)
    ap.add_argument("--cr", type=float, default=214.0)
    ap.add_argument("--auto", action="store_true",
                    help="auto-detect the disc instead of the calibrated crop")
    args = ap.parse_args()

    if args.frames != kCols * kRows:
        raise SystemExit("--frames must equal %d (8x6 atlas)" % (kCols * kRows))
    if not os.path.isfile(args.video):
        raise SystemExit("video not found: " + args.video)

    with tempfile.TemporaryDirectory() as tmp:
        log("extracting %d frames from %s  (t %.2f..%.2f s)"
            % (args.frames, os.path.basename(args.video), args.t0, args.t1))
        paths = extract_frames(args.video, args.t0, args.t1, args.frames, tmp)

        # Disc: calibrated constants by default (this video); --auto re-detects
        # on the OPEN-state frame (t ~9.3 s, the disc at its brightest).
        if args.auto:
            detdir = os.path.join(tmp, "det")
            os.makedirs(detdir, exist_ok=True)
            det = extract_frames(args.video, 9.3, 9.3, 1, detdir)[0]
            mid = np.array(Image.open(det).convert("RGB"))
            cx, cy, rad = detect_disc(mid)
        else:
            mid = np.array(Image.open(paths[0]).convert("RGB"))
            cx, cy, rad = args.cx, args.cy, args.cr
        side = rad
        log("disc: center (%.0f, %.0f) radius %.0f px -> crop side %.0f"
            % (cx, cy, rad, side * 2))

        mask = radial_mask(args.tile)
        tiles = []
        h, w = mid.shape[0], mid.shape[1]
        x0, x1 = cx - side, cx + side
        y0, y1 = cy - side, cy + side
        for p in paths:
            im = Image.open(p).convert("RGB")
            im = im.crop((int(round(x0)), int(round(y0)),
                          int(round(x1)), int(round(y1))))
            im = im.resize((args.tile, args.tile), Image.LANCZOS)
            tiles.append(np.array(im).astype(np.float32) / 255.0)
        log("cropped + masked %d tiles (%dx%d), source %dx%d"
            % (len(tiles), args.tile, args.tile, w, h))

        # LOOP-BLEND the tail toward the head so a plain modulo loop is seamless.
        n = len(tiles)
        for k in range(kLoopBlend):
            i = n - kLoopBlend + k
            t = (k + 1.0) / (kLoopBlend + 1.0)     # 0 -> 1 across the tail
            tiles[i] = tiles[i] * (1.0 - t) + tiles[(k + 1) % n] * t

        # Radial mask into RGB (engine samples RGB) + the alpha channel (library).
        atlas = np.zeros((kRows * args.tile, kCols * args.tile, 4), np.float32)
        for i, tl in enumerate(tiles):
            r0 = (i // kCols) * args.tile
            c0 = (i % kCols) * args.tile
            atlas[r0:r0 + args.tile, c0:c0 + args.tile, :3] = tl * mask[..., None]
            atlas[r0:r0 + args.tile, c0:c0 + args.tile, 3] = mask

    out8 = (np.clip(atlas, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    Image.fromarray(out8, "RGBA").save(args.out)
    log("WROTE", args.out, "(%dx%d)" % (out8.shape[1], out8.shape[0]))

    # Fleet-convention library copy (best-effort; G: may be absent on a laptop).
    try:
        os.makedirs(kFleetDir, exist_ok=True)
        dst = os.path.join(kFleetDir, os.path.basename(args.out))
        shutil.copyfile(args.out, dst)
        log("copied ->", dst)
    except OSError as e:
        log("fleet copy skipped:", e)


if __name__ == "__main__":
    sys.exit(main())
