# CPU port of shaders/inc/sky_clouds.glsl (the sin-FREE field that replaced the
# shard hash) — receipts before the GPU eyes-on:
#   1. blob preview at the spawn camera (must be soft cumulus, no rectangles)
#   2. coverage fraction vs the `cover` parameter (0.42 fair / 0.66 rain / 0.94 storm)
#   3. magnitude safety: same statistics 50 km from the origin and at t=3600 s
import numpy as np
from PIL import Image

f32 = np.float32

def hash12(p):  # Hoskins hash12, exactly the GLSL (fract keeps fp32 in range)
    px, py = p[..., 0], p[..., 1]
    p3x = (px * f32(0.1031)); p3x = (p3x - np.floor(p3x)).astype(f32)
    p3y = (py * f32(0.1031)); p3y = (p3y - np.floor(p3y)).astype(f32)
    p3z = (px * f32(0.1031)); p3z = (p3z - np.floor(p3z)).astype(f32)
    d = (p3x * (p3y + f32(33.33)) + p3y * (p3z + f32(33.33)) + p3z * (p3x + f32(33.33))).astype(f32)
    p3x = (p3x + d).astype(f32); p3y = (p3y + d).astype(f32); p3z = (p3z + d).astype(f32)
    v = ((p3x + p3y) * p3z).astype(f32)
    return (v - np.floor(v)).astype(f32)

def noise(p):
    i = np.floor(p).astype(f32); f = (p - i).astype(f32)
    u = (f * f * (f32(3.0) - f32(2.0) * f)).astype(f32)
    def h(ox, oy): return hash12(i + np.array([ox, oy], dtype=f32))
    a, b, c, d = h(0, 0), h(1, 0), h(0, 1), h(1, 1)
    ab = a + (b - a) * u[..., 0]; cd = c + (d - c) * u[..., 0]
    return (ab + (cd - ab) * u[..., 1]).astype(f32)

ROT = np.array([[0.80, 0.60], [-0.60, 0.80]], dtype=f32) * f32(2.03)

def fbm(p, octaves=5.0):
    # NORMALIZED by the live weight sum (matches the GLSL): amplitude is
    # octave-count-invariant, so the 3-octave ground shadow and the 5-octave
    # sky read the SAME coverage through the same threshold.
    a = f32(0.5); s = np.zeros(p.shape[:-1], dtype=f32); q = p.copy(); n = f32(0.0)
    for k in range(5):
        w = f32(min(max(octaves - k, 0.0), 1.0))
        s = (s + a * w * noise(q)).astype(f32)
        n = f32(n + a * w)
        q = (q @ ROT.T + f32(11.7)).astype(f32)
        a = f32(a * 0.5)
    return (s / max(n, 1e-4)).astype(f32)

SCALE = f32(0.00055); DRIFT = np.array([0.011, 0.004], dtype=f32)

def cover_at(wxz, cover, t, octaves=5.0):
    p = (wxz * SCALE + DRIFT * f32(t)).astype(f32)
    w = np.stack([fbm((p * f32(0.5) + f32(17.3)).astype(f32), octaves),
                  fbm((p * f32(0.5) - f32(9.1)).astype(f32), octaves)], axis=-1)
    d = fbm((p + (w - f32(0.5)) * f32(1.6)).astype(f32), octaves)
    c = min(max(cover, 0.0), 1.0)
    lo = f32(0.66 - 0.20 * c - 0.29 * c * c)   # calibrated: see GLSL comment
    x = np.clip((d - lo) / f32(0.30), 0.0, 1.0).astype(f32)
    return (x * x * (f32(3.0) - f32(2.0) * x)).astype(f32)

# ---- 1. spawn-camera preview (same ray setup as repro_cloud.py) -------------
W, H = 640, 360
yaw, pitch, fov = 2.749, 0.55, np.radians(68.0)
xs = np.linspace(-1, 1, W, dtype=f32); ys = np.linspace(1, -1, H, dtype=f32)
gx, gy = np.meshgrid(xs, ys); th = np.tan(fov / 2); aspect = W / H
fwd = np.array([np.cos(yaw) * np.cos(pitch), np.sin(pitch), np.sin(yaw) * np.cos(pitch)], dtype=f32)
right = np.array([-np.sin(yaw), 0, np.cos(yaw)], dtype=f32)
upv = np.cross(right, fwd) * -1
d = fwd[None, None, :] + right[None, None, :] * (gx * th * aspect)[..., None] + upv[None, None, :] * (gy * th)[..., None]
d = (d / np.linalg.norm(d, axis=-1, keepdims=True)).astype(f32)
up = d[..., 1]
cam = np.array([-301.9, 17.6, -472.2], dtype=f32)
dist = f32(1400.0) / np.maximum(up, f32(0.02))
wp = np.stack([cam[0] + d[..., 0] * dist, cam[2] + d[..., 2] * dist], axis=-1).astype(f32)
for t in (12.0, 3600.0):
    dm = cover_at(wp, 0.42, t); dm[up <= 0] = 0
    Image.fromarray((np.clip(dm, 0, 1) * 255).astype(np.uint8)).save(
        f"shots_clouds/verify_newfield_t{int(t)}.png")

# ---- 2 + 3. coverage fraction near origin and 50 km out ---------------------
rng = np.random.default_rng(7)
for label, off in [("origin", (0.0, 0.0)), ("50km", (50000.0, 50000.0))]:
    pts = (rng.uniform(-4000, 4000, size=(200000, 2)) + np.array(off)).astype(f32)
    for cov in (0.0, 0.42, 0.66, 0.94, 1.0):
        d0 = cover_at(pts, cov, 3600.0)
        frac = float((d0 > 0.02).mean()); mean = float(d0.mean())
        print(f"{label:6s} cover={cov:.2f}  sky-fraction={frac:.3f}  mean-density={mean:.3f}")
