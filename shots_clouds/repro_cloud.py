# Repro of shaders/sky.frag cloudDensity in strict fp32 (numpy) to isolate the
# "angular shard" defect: if the rectangles appear here, the MATH is broken;
# if not, the GPU sin() precision in cloudHash is the culprit.
import numpy as np
from PIL import Image

f32 = np.float32

def hash_sin(p):  # fract(sin(dot(p,(127.1,311.7)))*43758.5453123) in fp32
    d = (p[..., 0] * f32(127.1) + p[..., 1] * f32(311.7)).astype(f32)
    s = np.sin(d, dtype=f32)                     # numpy sin: accurate; GPU may not be
    v = (s * f32(43758.5453123)).astype(f32)
    return (v - np.floor(v)).astype(f32)

def hash_sin_lowp(p):
    # emulate a GPU-ish sin: reduce the argument with fp32 2*pi first (this is
    # roughly what a fast-math range reduction costs you at large |x|)
    d = (p[..., 0] * f32(127.1) + p[..., 1] * f32(311.7)).astype(f32)
    twopi = f32(6.2831853)
    n = np.floor(d / twopi).astype(f32)
    r = (d - n * twopi).astype(f32)              # catastrophic cancellation at large d
    s = np.sin(r, dtype=f32)
    v = (s * f32(43758.5453123)).astype(f32)
    return (v - np.floor(v)).astype(f32)

def noise(p, hashf):
    i = np.floor(p).astype(f32)
    f = (p - i).astype(f32)
    u = (f * f * (f32(3.0) - f32(2.0) * f)).astype(f32)
    def h(ox, oy):
        return hashf((i + np.array([ox, oy], dtype=f32)))
    a = h(0, 0); b = h(1, 0); c = h(0, 1); d = h(1, 1)
    ab = a + (b - a) * u[..., 0]
    cd = c + (d - c) * u[..., 0]
    return (ab + (cd - ab) * u[..., 1]).astype(f32)

def fbm(p, hashf):
    a = f32(0.5); s = np.zeros(p.shape[:-1], dtype=f32); q = p.copy()
    for _ in range(5):
        s = (s + a * noise(q, hashf)).astype(f32)
        q = (q * f32(2.03)).astype(f32)
        a = f32(a * 0.5)
    return s

def density(wp, cover, t, hashf):
    drift = np.array([t * 0.006, t * 0.0022], dtype=f32)
    p = (wp * f32(0.00055) + drift).astype(f32)
    w = np.stack([fbm((p * f32(0.5) + f32(17.3)).astype(f32), hashf),
                  fbm((p * f32(0.5) - f32(9.1)).astype(f32), hashf)], axis=-1)
    d = fbm((p + (w - f32(0.5)) * f32(1.6)).astype(f32), hashf)
    lo = f32(0.62 + (0.24 - 0.62) * min(max(cover, 0.0), 1.0))
    x = np.clip((d - lo) / f32(0.22), 0.0, 1.0).astype(f32)
    return (x * x * (f32(3.0) - f32(2.0) * x)).astype(f32)

W, H = 640, 360
# camera at (-301.9, 17.6, -472.2) yaw 2.749 pitch 0.55, fov 68 deg like the shot
yaw, pitch, fov = 2.749, 0.55, np.radians(68.0)
aspect = W / H
xs = np.linspace(-1, 1, W, dtype=f32)
ys = np.linspace(1, -1, H, dtype=f32)
gx, gy = np.meshgrid(xs, ys)
th = np.tan(fov / 2)
fwd = np.array([np.cos(yaw) * np.cos(pitch), np.sin(pitch), np.sin(yaw) * np.cos(pitch)], dtype=f32)
right = np.array([-np.sin(yaw), 0, np.cos(yaw)], dtype=f32)
upv = np.cross(right, fwd) * -1  # y-up-ish; sign doesn't matter for the test
d = fwd[None, None, :] + right[None, None, :] * (gx * th * aspect)[..., None] + upv[None, None, :] * (gy * th)[..., None]
d = (d / np.linalg.norm(d, axis=-1, keepdims=True)).astype(f32)
up = d[..., 1]
cam = np.array([-301.9, 17.6, -472.2], dtype=f32)
dist = f32(1400.0) / np.maximum(up, f32(0.02))
wp = np.stack([cam[0] + d[..., 0] * dist, cam[2] + d[..., 2] * dist], axis=-1).astype(f32)

# m_skyTime after ~200 settle frames + boot: try the ~8600-frame value from the
# live log too (t = frames/60): shards may be TIME-dependent (drift grows p).
for name, t in [("t12", 12.0), ("t150", 150.0), ("t3600", 3600.0)]:
    for hn, hf in [("goodsin", hash_sin), ("lowpsin", hash_sin_lowp)]:
        dm = density(wp, 0.42, f32(t), hf)
        dm[up <= 0] = 0
        img = (np.clip(dm, 0, 1) * 255).astype(np.uint8)
        Image.fromarray(img).save(f"shots_clouds/repro_{name}_{hn}.png")
        print("wrote", name, hn, "max", dm.max())
