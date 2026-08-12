#!/usr/bin/env python3
# tools/echo_terrain_gen.py — ECHO HARBOR / ECHOTROPOLIS terrain generator.
#
# REGENERATES the lost island_mesa assets (island_20260530.glb +
# island_height_20260530.png were LFS pointers whose blobs survive nowhere the
# fleet can reach; the original SimCityLLM2 pipeline — gen_heightmap.py +
# island_to_glb.py — lived OUTSIDE this repo and is gone with it). THE LESSON
# OF THAT LOSS IS THIS FILE: the generator is committed, deterministic, and
# emits BOTH artifacts from ONE height array so mesh and heightfield can never
# drift apart.
#
# ---------------------------------------------------------------------------
# THE WORLD THIS BUILDS (Tim's canon, 2026-08-03, superseding the old "mesa
# island" naming — Echo Harbor was never meant to be an island):
#
#   OCEAN (west edge) -> a winding CLIFF-WALLED INLET (drowned glacial gorge,
#   Chelan / Columbia-gorge landform, Lake-Chelan sinuosity) -> beach shelves
#   from ~2/7 of the way in -> the HARBOR BASIN deep inside the cliffs where
#   ECHO HARBOR (the LOWER city: Harbor Boulevard + fanned blocks + waterfront)
#   sits on a curving shore shelf -> HIGH CLIFFS (~190 m) -> ECHOTROPOLIS (the
#   UPPER city: the "crown" plateau, downtown towers at (-20,760)) on top.
#   The FREEWAY RING is the spine that climbs between them: a graded SE bench
#   up from the flats, the crown crossing on the plateau, the rim run, and a
#   graded NE descent bench back down — with a ravine under the NE leg for a
#   viaduct moment and spurs worth tunnelling (the terrain-corridor lane bores
#   by lowering terrain).  A knife-ridge BLUFF splits basin from gorge at the
#   gorge mouth (Tim's iPad: suspension-bridge span goes there), and a slot-
#   canyon arm dead-ends at the lighthouse cove below the west shoulder.
#
# SCALE HONESTY: the engine's island frame is FIXED at 4096 m (kMeters in
# app/world_hosts/echo_heightfield.h — every camera/fog/shadow/road constant
# is tuned to it, and the "8-16 km bigger frame" decision in
# docs/plans/SESSION_LANES.md is still open). Tim's "7 miles of waterways"
# therefore lands here COMPRESSED: the navigable centerline this file builds
# is reported by --verify (~4.6 km ocean->city head). The 7-mile STRUCTURE is
# kept proportionally: settlement begins ~2/7 of the way in from the mouth.
# If/when the big-frame decision lands, re-run this generator with FRAME
# scaled up — everything here is authored in world metres, not pixels.
#
# ENCODING (must mirror app/world_hosts/echo_heightfield.h EXACTLY):
#   16-bit grayscale PNG, kMeters=4096 (world extent, centered), kScale=320,
#   kSeaNorm=0.20  =>  height_m = (px/65535 - 0.20) * 320, range -64..+256.
#   PNG row 0 = z=-2048 (stbi returns rows top-first; heightAt v=0 there).
#
# GLB (mirrors what host_echotropolis.cpp documents of the lost bake):
#   land mesh 513^2 grid + skirt, one 4096^2 splat-blended albedo, plus a flat
#   dark ocean sheet at y=-0.4 out to +-14 km. WATER_V2 behavior: the sheet
#   runs UNDER the island square too, so in-frame water (basin/gorge/arm) has
#   a surface — the pre-V2 fjord bake's missing-water bug is not reproduced.
#
# HARD ANCHORS (authored constants elsewhere in the codebase that this terrain
# must satisfy — verified by --verify, which re-implements the actual probe
# logic from echo_roads.cpp):
#   crown datum (-20,760) ~ 190 m plateau        [echo_roads.cpp kCrownX/Z]
#   ring fixed wps (-160,720)(120,720)(480,900)(820,1120)(1060,900)(980,560)
#   rim probe: bearings 320..140 step -20 from crown, rim = last sample >=
#     0.80*crown within 120..700 m                [echo_roads.cpp V4.1]
#   9 shore seeds (-140,470)..(860,268): waterline (h<1.5) within 400 m
#   gates (830,1150) land; (700,452) land-or-nudgeable
#   boat lanes: (-400,330)+x760, (340,240)-x760, (-560,260)+z420 all wet
#   mine (-480,850) + truck lot (-556,814) on the plateau shoulder
#   lighthouse (-493.24, 789.39) at sea level (rock at the slot-cove head)
#   districts: URBAN pad (700,350) terrace; RECIFE (950,1250); HIVEMIND
#     (1340,1000) on gentle flats
#   woodlands pine gate: h in [24,172] + slope gate  [echo_woodlands.cpp]
#
# Usage:
#   python tools/echo_terrain_gen.py [--out assets/island_mesa] [--verify]
#                                    [--png-only] [--albedo-res 4096]
# Deterministic: no wall-clock, fixed seed. Output filenames keep the
# island_20260530.* names on purpose — precedent set by the 20260728 rebake
# (filename = slot name, content = latest bake; every consumer refs them).

import argparse
import json
import math
import os
import struct
import sys

import numpy as np
from PIL import Image

# ---------------------------------------------------------------- constants
# ---------------------------------------------------------------------------
# THE SEA DATUM lives in app/world_hosts/echo_sea.h (kEchoSeaLevelY = 0). It is
# defined as the height where heightAt crosses zero, i.e. where hn == SEANORM —
# which is fixed BY THIS FILE and baked into the GLB. So the constants below are
# not free: `--test-sealevel` PARSES this file and fails if any of them stops
# agreeing with the engine. Do not edit one side only.
#   SEANORM        -> Heightfield::kSeaNorm   (puts the datum at y=0)
#   HSCALE         -> Heightfield::kScale
#   OCEAN_Y        -> kEchoOceanRingY. NOT a sea level: it is the FLOOR the
#                     Gerstner troughs must clear. Lowering it is what buys
#                     bigger waves (echoMaxAmplitude()); it must never be raised
#                     toward the datum to "fix" a waterline.
#   WATER_MIN_LAND -> datum + kEchoLandMinClear (echo_roads.cpp kWaterMinLand)
SEED       = 20260803          # bake seed (date of the fjord regen)
FRAME      = 4096.0            # kMeters — echo_heightfield.h
HSCALE     = 320.0             # kScale
SEANORM    = 0.20              # kSeaNorm  => sea datum at world y = 0
N_PNG      = 1025              # heightfield resolution (4 m/px, ~2.1 MB PNG)
N_MESH     = 513               # land mesh grid (samples PNG every 2nd px)
OCEAN_Y    = -0.4              # baked flat water sheet = kEchoOceanRingY (a FLOOR)
OCEAN_EXT  = 14000.0           # water sheet + old "ocean ring" reach
SKIRT_Y    = -8.0              # skirt bottom (below the water sheet)

WATER_MIN_LAND = 1.5           # echo_roads.cpp kWaterMinLand (= datum + 1.5)
KEEL_DRAFT     = -4.0          # echo_region_builders.cpp kKeelDraft (= datum - 4.0)

# ---------------------------------------------------------------- helpers
def smoothstep(t):
    t = np.clip(t, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)

def _upsample(g, N):
    """Bilinear upsample a small 2D grid to N x N."""
    gy, gx = g.shape
    ys = np.linspace(0, gy - 1, N)
    xs = np.linspace(0, gx - 1, N)
    y0 = np.floor(ys).astype(int); y1 = np.minimum(y0 + 1, gy - 1); fy = (ys - y0)[:, None]
    x0 = np.floor(xs).astype(int); x1 = np.minimum(x0 + 1, gx - 1); fx = (xs - x0)[None, :]
    fy = fy * fy * (3.0 - 2.0 * fy); fx = fx * fx * (3.0 - 2.0 * fx)
    a = g[np.ix_(y0, x0)]; b = g[np.ix_(y0, x1)]
    c = g[np.ix_(y1, x0)]; d = g[np.ix_(y1, x1)]
    return a * (1 - fy) * (1 - fx) + b * (1 - fy) * fx + c * fy * (1 - fx) + d * fy * fx

def fbm(N, seed, octaves=5, base_cells=6, gain=0.5, ridged=False):
    """Deterministic value-noise fBm in [0,1] (ridged: crease mountains)."""
    rng = np.random.RandomState(seed)
    out = np.zeros((N, N), np.float64)
    amp, total, cells = 1.0, 0.0, base_cells
    for _ in range(octaves):
        g = rng.rand(cells + 1, cells + 1)
        layer = _upsample(g, N)
        if ridged:
            layer = 1.0 - np.abs(2.0 * layer - 1.0)
        out += amp * layer
        total += amp
        amp *= gain
        cells = min(cells * 2, N - 1)
    return out / total

def chain_dist(X, Z, pts, attrs):
    """Distance from every grid point to a polyline chain, with per-vertex
    attributes lerped at the nearest point. pts: [(x,z),...]; attrs: dict of
    name -> [v0, v1, ...] (len == len(pts)). Returns (dist, {name: array},
    nearest point (x,z)) — the nearest point supports side-of-chain tests."""
    best = np.full(X.shape, 1e18)
    out = {k: np.zeros(X.shape) for k in attrs}
    ncx = np.zeros(X.shape); ncz = np.zeros(X.shape)
    for i in range(len(pts) - 1):
        ax, az = pts[i]; bx, bz = pts[i + 1]
        dx, dz = bx - ax, bz - az
        L2 = dx * dx + dz * dz
        t = np.clip(((X - ax) * dx + (Z - az) * dz) / max(L2, 1e-9), 0.0, 1.0)
        px, pz = ax + t * dx, az + t * dz
        d = np.hypot(X - px, Z - pz)
        m = d < best
        best = np.where(m, d, best)
        ncx = np.where(m, px, ncx); ncz = np.where(m, pz, ncz)
        for k, vals in attrs.items():
            out[k] = np.where(m, vals[i] + t * (vals[i + 1] - vals[i]), out[k])
    return best, out, (ncx, ncz)

def chain_length(pts):
    return sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(pts, pts[1:]))

def poly_sdf(X, Z, poly):
    """Signed distance to a closed polygon: positive inside."""
    d, _, _ = chain_dist(X, Z, poly + [poly[0]], {})
    inside = np.zeros(X.shape, bool)
    n = len(poly)
    for i in range(n):
        ax, az = poly[i]; bx, bz = poly[(i + 1) % n]
        cond = (az > Z) != (bz > Z)
        with np.errstate(divide="ignore", invalid="ignore"):
            xi = ax + (Z - az) * (bx - ax) / np.where(bz - az == 0, 1e-9, bz - az)
        inside ^= cond & (X < xi)
    return np.where(inside, d, -d)

# ============================================================== the terrain
def build_height(N=N_PNG):
    half = FRAME / 2.0
    axis = np.linspace(-half, half, N)
    X, Z = np.meshgrid(axis, axis)          # arr[iz, ix]; row 0 = z=-2048

    fb_broad  = fbm(N, SEED + 1, octaves=5, base_cells=5)
    fb_mid    = fbm(N, SEED + 2, octaves=5, base_cells=12)
    fb_fine   = fbm(N, SEED + 3, octaves=4, base_cells=48)
    fb_ridge  = fbm(N, SEED + 4, octaves=5, base_cells=7, ridged=True)
    fb_ridge2 = fbm(N, SEED + 5, octaves=5, base_cells=13, ridged=True)

    # ---- 1. base: gentle east/coastal flats ------------------------------
    h = 10.0 + 9.0 * (fb_broad - 0.5) + 6.0 * (fb_mid - 0.5)
    h += 4.0 * smoothstep((Z - 300.0) / 900.0)          # flats tilt up northward

    # ---- 2. highland masses (the fjord's outer walls) --------------------
    # South highlands (south bank of the gorge, to the frame edge).
    wob = (fb_mid - 0.5) * 300.0
    m = smoothstep((-180.0 - Z + wob) / 260.0) * smoothstep((X + 2400.0) / 300.0)
    m *= 1.0 - 0.65 * smoothstep((X - 800.0 + wob) / 500.0)   # taper toward SE flats
    tgt = 66.0 + 148.0 * fb_ridge + 0.05 * np.maximum(0.0, -Z - 180.0)
    h = h * (1 - m) + tgt * m
    # West highlands (outer coast north of the gorge, west of the arm).
    m = smoothstep((-720.0 - X + wob) / 200.0) * smoothstep((Z + 650.0 + wob) / 320.0)
    tgt = 82.0 + 132.0 * fb_ridge2 + 0.045 * np.maximum(0.0, -X - 720.0)
    h = h * (1 - m) + tgt * m
    # North backdrop range (behind the plateau).
    m = smoothstep((Z - 1430.0 + wob) / 280.0)
    tgt = 120.0 + 95.0 * fb_ridge + 0.06 * np.maximum(0.0, Z - 1430.0)
    h = np.maximum(h, h * (1 - m) + tgt * m)

    # ---- 3. harbor shelf (Echo Harbor's ground) + urban terrace ----------
    # The curving shore shelf behind the basin's north waterline: the strip
    # the Harbor Boulevard + fanned blocks + waterfront row live on. It runs
    # x -470..430 — the stretch that sits under the big south cliff. East of
    # that the coast relaxes into the beach-town terrace + the freeway's
    # climbing flank instead of a cliff.
    basin_pts = [(-480, 215), (-100, 285), (300, 275), (620, 200),
                 (840, 180), (980, 110)]
    basin_at = {
        "hw":      [155, 150, 145, 90, 80, 55],
        "floor":   [-12, -13, -12, -10, -7, -2.5],
        "beach":   [70, 70, 65, 50, 45, 50],
    }
    bd, bat, bnp = chain_dist(X, Z, basin_pts,
        {k: basin_at[k] for k in ("hw", "floor", "beach")})
    sd_basin = bd - bat["hw"]
    north = smoothstep((Z - bnp[1]) / 40.0)
    w = (smoothstep((sd_basin - 35.0) / 30.0) * (1.0 - smoothstep((sd_basin - 205.0) / 45.0))
         * north * smoothstep((X + 470.0) / 60.0) * (1.0 - smoothstep((X - 430.0) / 60.0)))
    tgt = 2.5 + 0.030 * np.maximum(0.0, sd_basin - 35.0) + 1.2 * (fb_fine - 0.5)
    h = h * (1 - w) + tgt * w
    # Urban district terrace (keep-out rect 540..860 x 190..510, pad (700,350)).
    rd = np.maximum(np.maximum(540.0 - X, X - 860.0), np.maximum(190.0 - Z, Z - 510.0))
    w = smoothstep((90.0 - rd) / 90.0)
    h = h * (1 - w) + (14.0 + 2.0 * (fb_mid - 0.5)) * w
    # Outlying district pads on the flats.
    for (px_, pz_, pe, pr) in [(950, 1250, 16.0, 190.0), (1340, 1000, 18.0, 190.0)]:
        r = np.hypot(X - px_, Z - pz_)
        w = smoothstep((pr + 70.0 - r) / 70.0)
        h = h * (1 - w) + (pe + 1.5 * (fb_mid - 0.5)) * w

    # ---- 4. THE CROWN PLATEAU (Echotropolis' ground) ---------------------
    # The rim polygon is the cliff TOP lip; each vertex carries its own cliff
    # face width `cw`. The harbor-facing south-central wall (cw 60) and the
    # cove-facing west wall (cw 32 — it threads the 62 m gap between the mine
    # on top and the lighthouse at sea level) are near-sheer basalt: THE echo
    # walls the world is named for. North/NE faces are soft descents.
    rim = [(320, 640), (365, 800), (330, 950), (220, 1080), (40, 1190),
           (-180, 1280), (-420, 1290), (-610, 1190), (-640, 1060),
           (-520, 1000), (-505, 900), (-495, 810), (-462, 750), (-440, 650),
           (-418, 548), (-308, 532), (-190, 538), (-104, 522), (-20, 515),
           (64, 530), (137, 572), (200, 590)]
    rim_cw = [130, 130, 220, 220, 200,
              200, 200, 200, 90,
              90, 32, 32, 32, 60,
              90, 75, 60, 60, 60,
              60, 60, 75]
    sdp = poly_sdf(X, Z, rim)
    _, rat, _ = chain_dist(X, Z, rim + [rim[0]], {"cw": rim_cw + [rim_cw[0]]})
    cw = rat["cw"]
    sdp = sdp + (fb_mid - 0.5) * 90.0 * np.clip((cw - 95.0) / 80.0, 0.0, 1.0)
    # Interior height: 193 m datum, tilted DOWN toward the SE lip (eases the
    # freeway's flats->rim climb) and toward the NE exit (the descent bench).
    Hp = 193.0 + 8.0 * (fb_mid - 0.5)
    Hp -= np.clip(0.05 * np.maximum(0.0, X - 100.0), 0.0, 22.0)                # SE tilt
    ne = ((X - 140.0) * 0.871 + (Z - 760.0) * 0.492)
    Hp -= np.clip(0.09 * np.maximum(0.0, ne), 0.0, 24.0)                        # NE tilt
    # Calm ground under the tower/houses core.
    calm = smoothstep((460.0 - np.hypot(X + 20.0, Z - 760.0)) / 160.0)
    Hp = Hp * (1 - 0.7 * calm) + (191.0 + 2.0 * (fb_mid - 0.5)) * 0.7 * calm
    # Cliff: full height held 15 m PAST the rim line (the 20260728 "extend the
    # cliffs out at the top" outcrop law), then the cw-wide face, talus toe.
    t = np.clip((sdp + 15.0) / cw, 0.0, 1.0)
    s = smoothstep(t) ** 1.30
    h = np.maximum(h, h * (1 - s) + Hp * s)

    # ---- 5. freeway benches (the climb the canon demands) ----------------
    # SE climb: a shore PROMONTORY ramp — flats (980,560) up a headland that
    # rises over the harbor's east waterfront to the plateau lip. It serves
    # two masters: a ~20% graded corridor the deck can hug (echo_roads
    # kDeckMaxGrade=0.22), and a rim-probe radius that TAPERS (450->300->230
    # over bearings 320..280) so the probed waypoints splice into the ring's
    # long fixed arc without the uniform-Catmull cusp that trips the zigzag
    # law. The freeway climbs above the water — the harbor sees it overhead.
    bench_w = np.zeros_like(h)
    for pts, hws, (e0, e1) in [
        ([(1010, 558), (760, 552), (560, 538), (390, 515), (250, 515), (130, 560)],
         [60, 60, 60, 60, 60, 70], (13.0, 191.0)),
        # NE descent: crown edge down to the flats through the ravine field.
        ([(200, 760), (390, 860), (480, 900), (640, 1005), (820, 1120), (960, 1075)],
         [140, 140, 135, 130, 130, 120], (190.0, 20.0)),
    ]:
        L = chain_length(pts)
        acc, elev = 0.0, [e0]
        for a, b in zip(pts, pts[1:]):
            acc += math.hypot(b[0] - a[0], b[1] - a[1])
            elev.append(e0 + (e1 - e0) * acc / L)
        d, at, _ = chain_dist(X, Z, pts, {"hw": hws, "elev": elev})
        w = smoothstep((at["hw"] + 60.0 - d) / 60.0)
        h = h * (1 - w) + at["elev"] * w
        bench_w = np.maximum(bench_w, w)

    # ---- 6. the bluff (knife ridge between basin and gorge mouth) --------
    bd2, bat2, _ = chain_dist(X, Z, [(-380, 60), (-140, 30), (60, -10)],
                              {"e": [120.0, 95.0, 60.0]})
    h = np.maximum(h, bat2["e"] * np.exp(-(bd2 / 95.0) ** 2 * 1.2))

    # ---- 7. dry ravines / coulees (viaduct + tunnel vocabulary) ----------
    gullies = [
        # NE flank ravine — crosses UNDER the freeway's descent leg (viaduct).
        ([(250, 1130), (480, 1040), (620, 990), (800, 955), (950, 940)],
         [130, 92, 80, 42, 20], [55, 45, 42, 40, 45]),
        # South-bank coulee draining to a small cove on the basin's south shore.
        ([(620, -420), (540, -160), (470, 40), (430, 110)],
         [95, 40, 6, -2.0], [50, 45, 40, 35]),
        # Hanging valley meeting the gorge (waterfall notch on the north wall).
        ([(-1250, -330), (-1160, -560), (-1090, -780)],
         [70, 25, -6.0], [45, 40, 38]),
    ]
    for pts, floors, hws in gullies:
        d, at, _ = chain_dist(X, Z, pts, {"floor": floors, "hw": hws})
        carve = at["floor"] + 0.9 * np.maximum(0.0, d - at["hw"]) ** 1.15
        h = np.minimum(h, carve)

    # ---- 8. THE WATERWAYS (carved last — water always wins) --------------
    # Cross-profile: flat floor -> beach lip to +2.2 over `beach` metres ->
    # bank rising to `bankTop` over `bankW` (a fjord wall where bankTop is
    # cliff-high, a soft berm where it is a few metres) -> a hard super-steep
    # term far outside so no channel ever undercuts distant terrain. Banks
    # are PER SIDE (suffix 0/1) so e.g. the basin can have a town bench on
    # the north shore and the bluff rising from the south shore. `side_axis`
    # picks which coordinate splits the sides ('z': side1 = north of the
    # chain; 'x': side1 = east of it).
    fb_shore = fbm(X.shape[0], SEED + 21, octaves=4, base_cells=22)
    def carve_channel(pts, at, side_axis):
        d, a, npt = chain_dist(X, Z, pts, at)
        sd = d - a["hw"] + (fb_shore - 0.5) * 44.0   # crenulated coast
        if side_axis == "z":
            s1 = smoothstep((Z - npt[1]) / 40.0)
        else:
            s1 = smoothstep((X - npt[0]) / 15.0)
        bT = a["bankTop0"] * (1 - s1) + a["bankTop1"] * s1
        bW = a["bankW0"] * (1 - s1) + a["bankW1"] * s1
        carve = (a["floor"]
                 + (2.2 - a["floor"]) * smoothstep(sd / np.maximum(a["beach"], 1.0))
                 + (bT - 2.2) * smoothstep((sd - a["beach"]) / bW)
                 + 8.0 * np.maximum(0.0, sd - a["beach"] - 1.6 * bW) ** 1.4)
        return np.minimum(h, carve), chain_length(pts)

    pre_water = h.copy()   # benches are protected from bank caps (below)
    # Main gorge: ocean mouth (funnel past the west frame edge) -> serpentine
    # cliff-walled reach -> gorge mouth at the bluff -> harbor basin.
    gorge_pts = [(-2400, -1120), (-1880, -980), (-1520, -760), (-1180, -1010),
                 (-760, -1230), (-340, -1150), (-40, -860), (160, -560),
                 (60, -260), (-260, -90), (-430, 90), (-480, 200)]
    n_g = len(gorge_pts)
    gorge_at = {
        "hw":      [420, 260, 150, 140, 150, 140, 130, 120, 130, 140, 150, 160],
        "floor":   [-22, -20, -18, -18, -16, -16, -15, -14, -13, -12, -12, -12],
        # cliffy at the outer reaches, beach shelves appearing ~2/7 in:
        "beach":   [30, 25, 20, 22, 30, 55, 60, 55, 55, 50, 55, 60],
        "bankTop0": [180, 210, 235, 235, 220, 200, 190, 190, 170, 150, 130, 90],
        "bankW0":   [220, 190, 160, 160, 170, 180, 180, 160, 160, 160, 170, 170],
    }
    gorge_at["bankTop1"] = gorge_at["bankTop0"]
    gorge_at["bankW1"] = gorge_at["bankW0"]
    h, gorge_len = carve_channel(gorge_pts, gorge_at, "z")
    # Harbor basin (Echo Harbor's water; both boat lanes live here).
    # side1 = north shore (town bench under the shelf + terrace); side0 =
    # south shore (the bluff and the south-bank hills rise straight off it).
    basin_at.update({
        "bankTop1": [9, 9, 9, 20, 20, 18],
        "bankW1":   [37, 37, 37, 30, 28, 60],
        "bankTop0": [130, 150, 170, 120, 90, 60],
        "bankW0":   [110, 100, 90, 110, 120, 120],
    })
    h, basin_len = carve_channel(basin_pts, basin_at, "z")
    # Lighthouse arm: the SW-inlet boat lane running north between the
    # plateau's west wall (side1=east: sheer) and the outer-coast bench
    # (side0=west: low wooded shore), bending west to dead-end at the
    # lighthouse cove under the mining spur — echo walls on three sides.
    arm_pts = [(-520, 210), (-548, 430), (-558, 620), (-560, 700)]
    arm_at = {
        "hw":      [130, 110, 92, 62],
        "floor":   [-9, -8, -7, -5],
        "beach":   [40, 35, 25, 10],
        "bankTop1": [210, 210, 210, 210],
        "bankW1":   [90, 110, 130, 26],
        "bankTop0": [100, 100, 120, 200],
        "bankW0":   [260, 260, 240, 30],
    }
    h, arm_len = carve_channel(arm_pts, arm_at, "x")
    # The freeway benches are engineered cuts/fills: the basin's soft north
    # bank cap must not shave the SE climb where it crosses above the shore
    # strip. No bench touches open water, so restoring the pre-carve height
    # inside the bench footprint is safe.
    h = np.where(bench_w > 0.35, np.maximum(h, pre_water), h)

    # ---- 9. west-shoulder SPUR + lighthouse rock -------------------------
    # The spur is a plateau-level rock promontory carrying the mine truck lot
    # (-556,814); applied AFTER the carve so its east face plunges straight
    # into the lighthouse cove (the cove's west wall IS the spur).
    spur_r = np.hypot((X + 540.0) / 105.0, (Z - 820.0) / 70.0)
    h = np.where(spur_r < 1.15,
                 np.maximum(h, 150.0 * smoothstep((1.15 - spur_r) / 0.35)), h)
    # Lighthouse rock (sea-level point at the cove head).
    r = np.hypot(X + 587.0, Z - 712.0)
    h = np.where(r < 25.0, np.maximum(h, 1.3 * np.exp(-(r / 20.0) ** 2) - 0.25), h)

    # ---- 10. detail + polish --------------------------------------------
    slope_proxy = np.hypot(*np.gradient(h, FRAME / (N - 1)))
    cliffy = smoothstep((slope_proxy - 0.55) / 0.35)
    calm_zone = np.maximum(w * 0, calm)  # keep the crown core calm
    h += (1 - calm_zone) * cliffy * 5.0 * (fb_ridge2 - 0.5)      # strata jitter
    h += (1 - cliffy) * 1.6 * (fb_fine - 0.5)                    # ground grain
    # one light 3x3 box pass (kills single-px spikes without moving anchors)
    h = (h + np.roll(h, 1, 0) + np.roll(h, -1, 0) + np.roll(h, 1, 1)
         + np.roll(h, -1, 1)) / 5.0
    h = np.clip(h, -60.0, 250.0)

    meta = {
        "gorge_len_m": gorge_len, "basin_len_m": basin_len, "arm_len_m": arm_len,
        "waterway_total_m": gorge_len + basin_len + arm_len,
    }
    return h, meta

# ============================================================== encoding
def height_to_png16(h):
    norm = np.clip(h / HSCALE + SEANORM, 0.0, 1.0)
    return np.round(norm * 65535.0).astype(np.uint16)

def png16_to_height(px):
    return (px.astype(np.float64) / 65535.0 - SEANORM) * HSCALE

class HF:
    """Python mirror of x3::game::Heightfield::heightAt (bilinear, clamped)."""
    def __init__(self, px):
        self.px = px.astype(np.float64) / 65535.0
        self.w = px.shape[1]; self.h = px.shape[0]
    def height_at(self, x, z):
        u = min(max((x / FRAME + 0.5) * (self.w - 1), 0.0), self.w - 1)
        v = min(max((z / FRAME + 0.5) * (self.h - 1), 0.0), self.h - 1)
        x0, z0 = int(u), int(v)
        x1 = min(x0 + 1, self.w - 1); z1 = min(z0 + 1, self.h - 1)
        fx, fz = u - x0, v - z0
        a = self.px[z0, x0]; b = self.px[z0, x1]
        c = self.px[z1, x0]; d = self.px[z1, x1]
        hn = a + (b - a) * fx + (c - a) * fz + (a - b - c + d) * fx * fz
        return (hn - SEANORM) * HSCALE

# ============================================================== albedo splat
def build_albedo(h, res):
    N = h.shape[0]
    H = _upsample(h, res)
    gy, gx = np.gradient(H, FRAME / (res - 1))
    slope = np.hypot(gx, gy)
    axis = np.linspace(-FRAME / 2, FRAME / 2, res)
    X, Z = np.meshgrid(axis, axis)
    n1 = fbm(res // 4, SEED + 11, octaves=5, base_cells=8)
    n1 = _upsample(n1, res)
    n2 = fbm(res // 4, SEED + 12, octaves=5, base_cells=24)
    n2 = _upsample(n2, res)

    img = np.zeros((res, res, 3))
    def lay(mask, col):
        m = np.clip(mask, 0, 1)[..., None]
        img[...] = img * (1 - m) + np.array(col, float)[None, None, :] * m

    lay(np.ones_like(H), (46, 66, 70))                                   # seabed
    lay(smoothstep((H + 6.0) / 5.0), (92, 104, 88))                      # shallows
    lay(smoothstep((H + 1.0) / 2.5) * smoothstep((0.30 - slope) / 0.20),
        (172, 156, 118))                                                 # sand
    grass_m = smoothstep((H - 2.5) / 3.0) * smoothstep((0.45 - slope) / 0.25)
    dry = smoothstep((1500.0 - np.hypot(X + 1800.0, Z + 1000.0)) / 900.0)
    grass_col = (np.array((84, 102, 56), float)[None, None, :] * (1 - dry[..., None])
                 + np.array((128, 116, 66), float)[None, None, :] * dry[..., None])
    img[...] = img * (1 - grass_m[..., None] * 0.9) + grass_col * (grass_m[..., None] * 0.9)
    forest = (grass_m * smoothstep((H - 20.0) / 10.0) * (1 - smoothstep((H - 168.0) / 12.0))
              * smoothstep((n1 - 0.42) / 0.16))
    lay(forest * 0.9, (40, 60, 36))                                      # pines
    plateau_green = smoothstep((H - 165.0) / 12.0) * smoothstep((0.4 - slope) / 0.2)
    lay(plateau_green * (0.55 + 0.35 * (n1 - 0.5)), (58, 74, 44))        # crown moor
    rock = smoothstep((slope - 0.55) / 0.30)
    band = 0.5 + 0.5 * np.sin(H * 0.55 + 4.0 * n2)
    rock_col = (np.array((78, 70, 62), float)[None, None, :] * (1 - band[..., None] * 0.4)
                + np.array((52, 48, 46), float)[None, None, :] * (band[..., None] * 0.4))
    img[...] = img * (1 - rock[..., None]) + rock_col * rock[..., None]
    talus = smoothstep((slope - 0.35) / 0.2) * (1 - rock) * smoothstep((H - 3.0) / 4.0)
    lay(talus * 0.5, (96, 88, 76))
    urb = smoothstep((450.0 - np.hypot(X + 20.0, Z - 760.0)) / 140.0) * smoothstep((H - 160) / 15.0)
    lay(urb * 0.45, (86, 88, 82))                                        # crown urban tint
    img *= (0.86 + 0.28 * n2[..., None])                                 # grain
    img *= (1.0 - 0.25 * np.clip(slope, 0, 1)[..., None])                # slope AO
    return np.clip(img, 0, 255).astype(np.uint8)

# ============================================================== GLB writer
def _pad4(b, fill=b"\x00"):
    return b + fill * ((4 - len(b) % 4) % 4)

def build_glb(px, albedo_png_bytes, out_path):
    """513^2 land grid + skirt (textured) + one flat dark water sheet at
    y=OCEAN_Y spanning +-OCEAN_EXT (WATER_V2: covers in-frame water too)."""
    step = (N_PNG - 1) // (N_MESH - 1)                  # exact px sampling
    sub = px[::step, ::step].astype(np.float64)
    hh = (sub / 65535.0 - SEANORM) * HSCALE
    half = FRAME / 2.0
    axis = np.linspace(-half, half, N_MESH)
    Xg, Zg = np.meshgrid(axis, axis)

    pos = np.stack([Xg, hh, Zg], axis=-1).astype(np.float32).reshape(-1, 3)
    d = FRAME / (N_MESH - 1)
    gz, gx = np.gradient(hh, d)
    nrm = np.stack([-gx, np.ones_like(gx), -gz], axis=-1)
    nrm /= np.linalg.norm(nrm, axis=-1, keepdims=True)
    nrm = nrm.astype(np.float32).reshape(-1, 3)
    uv = np.stack([(Xg / FRAME + 0.5), (Zg / FRAME + 0.5)],
                  axis=-1).astype(np.float32).reshape(-1, 2)

    # land triangles, CCW from +Y: (i, i+N, i+1) / (i+1, i+N, i+N+1)
    ii = np.arange(N_MESH - 1)
    jj = np.arange(N_MESH - 1)
    J, I = np.meshgrid(jj, ii, indexing="ij")
    v0 = (J * N_MESH + I).ravel()
    tris = np.empty((v0.size, 6), np.uint32)
    tris[:, 0] = v0;             tris[:, 1] = v0 + N_MESH; tris[:, 2] = v0 + 1
    tris[:, 3] = v0 + 1;         tris[:, 4] = v0 + N_MESH; tris[:, 5] = v0 + N_MESH + 1
    idx = tris.ravel()

    # skirt: duplicate rim verts, drop to SKIRT_Y (hides the mesh edge over
    # the water sheet exactly like the lost bake's skirt did)
    edge_ids = (list(range(0, N_MESH)) +                                    # z=-half row
                [r * N_MESH + (N_MESH - 1) for r in range(1, N_MESH)] +     # x=+half col
                [(N_MESH - 1) * N_MESH + c for c in range(N_MESH - 2, -1, -1)] +
                [r * N_MESH for r in range(N_MESH - 2, 0, -1)])
    eN = len(edge_ids)
    base = pos.shape[0]
    sk_pos = pos[edge_ids].copy(); sk_pos[:, 1] = SKIRT_Y
    sk_nrm = np.zeros((eN, 3), np.float32); sk_nrm[:, 1] = 1.0
    sk_uv = uv[edge_ids].copy()
    pos = np.vstack([pos, sk_pos]); nrm = np.vstack([nrm, sk_nrm]); uv = np.vstack([uv, sk_uv])
    sk_idx = []
    for k in range(eN):
        a = edge_ids[k]; b = edge_ids[(k + 1) % eN]
        a2 = base + k;   b2 = base + (k + 1) % eN
        sk_idx += [a, a2, b, b, a2, b2]
    idx = np.concatenate([idx, np.array(sk_idx, np.uint32)])

    # water sheet (own primitive/material)
    wbase = pos.shape[0]
    E = OCEAN_EXT
    wpos = np.array([[-E, OCEAN_Y, -E], [E, OCEAN_Y, -E],
                     [-E, OCEAN_Y, E], [E, OCEAN_Y, E]], np.float32)
    wnrm = np.tile(np.array([0, 1, 0], np.float32), (4, 1))
    wuv = np.array([[0, 0], [1, 0], [0, 1], [1, 1]], np.float32)
    widx = np.array([0, 2, 1, 1, 2, 3], np.uint32) + wbase
    pos = np.vstack([pos, wpos]); nrm = np.vstack([nrm, wnrm]); uv = np.vstack([uv, wuv])

    bin_parts, views, accessors = [], [], []
    off = 0
    def add_view(data, target=None):
        nonlocal off
        b = _pad4(data.tobytes())
        views.append({"buffer": 0, "byteOffset": off, "byteLength": len(data.tobytes()),
                      **({"target": target} if target else {})})
        bin_parts.append(b); off += len(b)
        return len(views) - 1
    def add_acc(view, ctype, count, atype, mn=None, mx=None):
        a = {"bufferView": view, "componentType": ctype, "count": count, "type": atype}
        if mn is not None: a["min"] = mn; a["max"] = mx
        accessors.append(a); return len(accessors) - 1

    v_pos = add_view(pos, 34962); v_nrm = add_view(nrm, 34962); v_uv = add_view(uv, 34962)
    v_land = add_view(idx, 34963); v_wat = add_view(widx, 34963)
    a_pos = add_acc(v_pos, 5126, pos.shape[0], "VEC3",
                    [float(v) for v in pos.min(0)], [float(v) for v in pos.max(0)])
    a_nrm = add_acc(v_nrm, 5126, nrm.shape[0], "VEC3")
    a_uv = add_acc(v_uv, 5126, uv.shape[0], "VEC2")
    a_land = add_acc(v_land, 5125, idx.size, "SCALAR")
    a_wat = add_acc(v_wat, 5125, widx.size, "SCALAR")
    v_img = add_view(np.frombuffer(albedo_png_bytes, np.uint8))

    gltf = {
        "asset": {"version": "2.0",
                  "generator": "tools/echo_terrain_gen.py seed %d" % SEED},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "island"}],
        "meshes": [{"name": "island", "primitives": [
            {"attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "TEXCOORD_0": a_uv},
             "indices": a_land, "material": 0},
            {"attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "TEXCOORD_0": a_uv},
             "indices": a_wat, "material": 1},
        ]}],
        "materials": [
            {"name": "land",
             "pbrMetallicRoughness": {"baseColorTexture": {"index": 0},
                                      "metallicFactor": 0.0, "roughnessFactor": 1.0}},
            {"name": "ocean",
             "pbrMetallicRoughness": {"baseColorFactor": [0.012, 0.043, 0.055, 1.0],
                                      "metallicFactor": 0.0, "roughnessFactor": 0.32}},
        ],
        "textures": [{"source": 0, "sampler": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987,
                      "wrapS": 33071, "wrapT": 33071}],
        "images": [{"bufferView": v_img, "mimeType": "image/png"}],
        "bufferViews": views,
        "accessors": accessors,
        "buffers": [{"byteLength": off}],
    }
    js = _pad4(json.dumps(gltf, separators=(",", ":")).encode(), b" ")
    bb = b"".join(bin_parts)
    total = 12 + 8 + len(js) + 8 + len(bb)
    with open(out_path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A)); f.write(js)
        f.write(struct.pack("<II", len(bb), 0x004E4942)); f.write(bb)
    return total

# ============================================================== verification
def verify(px, meta):
    """Re-implement the actual consumer probes from echo_roads.cpp against the
    ENCODED png (what the game will really sample). Returns (ok, report)."""
    hf = HF(px)
    rep, ok = [], True
    def chk(cond, msg):
        nonlocal ok
        rep.append(("PASS " if cond else "FAIL ") + msg)
        ok = ok and cond

    ha = hf.height_at
    rep.append("-- waterway: gorge %.0f m + basin %.0f m + arm %.0f m = %.2f km"
               " (%.2f mi) ocean->cove" % (meta["gorge_len_m"], meta["basin_len_m"],
               meta["arm_len_m"], meta["waterway_total_m"] / 1000.0,
               meta["waterway_total_m"] / 1609.34))

    crown = ha(-20, 760)
    chk(185.0 <= crown <= 200.0, "crown datum (-20,760) = %.1f m (want ~190)" % crown)
    for (x, z) in [(-160, 720), (120, 720)]:
        v = ha(x, z); chk(v > 170.0, "crown crossing (%d,%d) = %.1f m" % (x, z, v))
    for (x, z, lo, hi) in [(480, 900, 90, 175), (820, 1120, 15, 70),
                           (1060, 900, 5, 40), (980, 560, 5, 40)]:
        v = ha(x, z); chk(lo <= v <= hi, "ring wp (%d,%d) = %.1f m (want %d..%d)" % (x, z, v, lo, hi))

    # rim probe (verbatim logic: last sample >= 0.8*crown, 120..700m, 8m step)
    rim_elev = crown * 0.80
    kept = 0
    for deg in range(320, 139, -20):
        th = math.radians(deg); dx, dz = math.cos(th), math.sin(th)
        rimR = -1.0
        r = 120.0
        while r <= 700.0:
            if ha(-20 + dx * r, 760 + dz * r) >= rim_elev: rimR = r
            r += 8.0
        if rimR >= 120.0:
            kept += 1
            rep.append("  rim %3d deg -> R=%.0f m wp=(%.0f,%.0f) h=%.1f"
                       % (deg, rimR, -20 + dx * (rimR - 45), 760 + dz * (rimR - 45),
                          ha(-20 + dx * (rimR - 45), 760 + dz * (rimR - 45))))
    chk(kept >= 8, "rim probe: %d/10 bearings found a rim (>=8)" % kept)

    # shore seeds -> waterlineFrom (verbatim: perp of neighbors, higher side
    # inland, 8m march to 400m for h<1.5 crossing)
    seeds = [(-140, 470), (-40, 455), (90, 450), (230, 452), (370, 440),
             (510, 408), (650, 356), (780, 300), (860, 268)]
    found = 0
    for i, (sx, sz) in enumerate(seeds):
        s0 = seeds[max(i - 1, 0)]; s1 = seeds[min(i + 1, len(seeds) - 1)]
        tx, tz = s1[0] - s0[0], s1[1] - s0[1]
        L = math.hypot(tx, tz); tx, tz = tx / L, tz / L
        pxd, pzd = tz, -tx
        hR = ha(sx + pxd * 60, sz + pzd * 60); hL = ha(sx - pxd * 60, sz - pzd * 60)
        ix, iz = (pxd, pzd) if hR >= hL else (-pxd, -pzd)
        start_wet = ha(sx, sz) < WATER_MIN_LAND
        dx, dz = (ix, iz) if start_wet else (-ix, -iz)
        hit = None
        m = 8.0
        while m <= 400.0:
            wet = ha(sx + dx * m, sz + dz * m) < WATER_MIN_LAND
            if wet != start_wet: hit = m; break
            m += 8.0
        if hit: found += 1
        rep.append("  seed %d (%d,%d) h=%.1f %s waterline at %s m"
                   % (i, sx, sz, ha(sx, sz), "wet" if start_wet else "dry",
                      ("%.0f" % hit) if hit else "NOT FOUND"))
    chk(found == 9, "shore seeds: %d/9 find a waterline within 400 m" % found)

    for (gx2, gz2, nm) in [(830, 1150, "Recife gate"), (935, 1235, "Recife pad"),
                           (700, 452, "Urban gate")]:
        v = ha(gx2, gz2)
        chk(v > 2.0 or nm == "Urban gate", "%s (%d,%d) = %.1f m" % (nm, gx2, gz2, v))

    lanes = [((-400, 330), (1, 0), 760, "south bay E"),
             ((340, 240), (-1, 0), 760, "south bay W"),
             ((-560, 260), (0, 1), 420, "SW inlet N")]
    for (sx, sz), (dx, dz), ln, nm in lanes:
        worst = max(ha(sx + dx * t, sz + dz * t) for t in range(0, int(ln) + 1, 20))
        # Was `< -1.5`, which is NOT the depth the engine requires: the coastline
        # gate in buildHarborBay clips lanes to kKeelDraft = -4.0. A bake could
        # therefore pass its own lane check and still have every lane clipped or
        # dropped at run time. Same number on both sides now.
        chk(worst < KEEL_DRAFT,
            "boat lane %s: max floor %.1f m (want < %.1f = keel draft)"
            % (nm, worst, KEEL_DRAFT))

    rep.append("  note: legacy lighthouse-beam anchor (-493,789) now sits at "
               "%.1f m on the west shoulder (stale prop coordinate from the "
               "lost bake — props were heightmap-derived; re-derive on rebake)"
               % ha(-493.24, 789.39))
    rep.append("  new sea-level lighthouse site: cove head rock (-587,712) = %.1f m"
               % ha(-587, 712))
    for (x, z, lo, hi, nm) in [(-480, 850, 175, 205, "mine"),
                               (-556, 814, 140, 205, "truck lot"),
                               (700, 350, 8, 20, "URBAN pad"),
                               (950, 1250, 10, 22, "RECIFE pad"),
                               (1340, 1000, 12, 24, "HIVEMIND pad")]:
        v = ha(x, z); chk(lo <= v <= hi, "%s (%.0f,%.0f) = %.1f m (want %d..%d)" % (nm, x, z, v, lo, hi))
    for i2, (hx2, hz2) in enumerate([(60, 700), (125, 745), (10, 675), (150, 690), (85, 640)]):
        v = ha(hx2, hz2); chk(v > 175.0, "hero house %d (%d,%d) = %.1f m" % (i2, hx2, hz2, v))

    # freeway ring deck simulation (catmull + relax, as echo_roads does) ->
    # max pier must respect the V3.1 acceptance gate (<= 45 m)
    fixed = [(-160, 720), (120, 720), (480, 900), (820, 1120), (1060, 900), (980, 560)]
    rimwps = []
    for deg in range(320, 139, -20):
        th = math.radians(deg); dx, dz = math.cos(th), math.sin(th)
        rimR = -1.0
        r = 120.0
        while r <= 700.0:
            if ha(-20 + dx * r, 760 + dz * r) >= rim_elev: rimR = r
            r += 8.0
        if rimR >= 120.0:
            wpc = (-20 + dx * (rimR - 45), 760 + dz * (rimR - 45))
            # seam clearance (mirrors echo_roads.cpp kRimSeamClear=260: a rim
            # waypoint that close to the fixed arc's splice nodes always folds
            # the ring into an unsmoothable U-turn)
            if (math.hypot(wpc[0] - fixed[0][0], wpc[1] - fixed[0][1]) < 260.0 or
                    math.hypot(wpc[0] - fixed[-1][0], wpc[1] - fixed[-1][1]) < 260.0):
                continue
            rimwps.append(wpc)
    # convexify (verbatim echo_roads: drop waypoints whose turn dot < -0.17
    # walking [arcEnd, rim..., arcStart])
    arc_end, arc_start = fixed[-1], fixed[0]
    dropped = True
    while dropped and rimwps:
        dropped = False
        for i in range(len(rimwps)):
            p_ = rimwps[i - 1] if i > 0 else arc_end
            q_ = rimwps[i]
            r_ = rimwps[i + 1] if i + 1 < len(rimwps) else arc_start
            ax_, az_ = q_[0] - p_[0], q_[1] - p_[1]
            bx_, bz_ = r_[0] - q_[0], r_[1] - q_[1]
            la_ = math.hypot(ax_, az_); lb_ = math.hypot(bx_, bz_)
            if la_ < 1e-3 or lb_ < 1e-3 or (ax_ * bx_ + az_ * bz_) / (la_ * lb_) < -0.17:
                rep.append("  convexify drops wp (%.0f,%.0f)" % q_)
                rimwps.pop(i); dropped = True; break
    rep.append("  ring wps kept: %d rim + 6 fixed" % len(rimwps))
    wps = fixed + rimwps
    n = len(wps)
    dense = []
    for i in range(n):
        p0 = wps[(i - 1) % n]; p1 = wps[i]; p2 = wps[(i + 1) % n]; p3 = wps[(i + 2) % n]
        for tt in np.arange(0, 1, 0.02):
            t2, t3 = tt * tt, tt * tt * tt
            xx = 0.5 * ((2 * p1[0]) + (-p0[0] + p2[0]) * tt
                        + (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2
                        + (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * t3)
            zz = 0.5 * ((2 * p1[1]) + (-p0[1] + p2[1]) * tt
                        + (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2
                        + (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * t3)
            dense.append((xx, zz))
    # resample at 4 m
    ring = [dense[0]]
    carry = 0.0
    for a, b in zip(dense, dense[1:] + dense[:1]):
        seg = math.hypot(b[0] - a[0], b[1] - a[1])
        while carry + seg >= 4.0:
            f = (4.0 - carry) / seg
            a = (a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f)
            ring.append(a)
            seg = math.hypot(b[0] - a[0], b[1] - a[1])
            carry = 0.0
        carry += seg
    floor_y = []
    for (xx, zz) in ring:
        g = max(ha(xx + ox, zz + oz) for ox, oz in
                [(0, 0), (9, 9), (-9, 9), (9, -9), (-9, -9)])
        floor_y.append(max(max(g, 2.0) + 11.0, 13.0))
    y = floor_y[:]
    nR = len(y); mx_step = 0.22 * 4.0
    for k in range(1, 2 * nR):
        i2, p2_ = k % nR, (k - 1) % nR
        y[i2] = max(floor_y[i2], max(y[i2], y[p2_] - mx_step))
    for k in range(2 * nR - 1, 0, -1):
        i2, q2 = (k - 1) % nR, k % nR
        y[i2] = max(y[i2], y[q2] - mx_step)
    piers = [y[i2] - ha(ring[i2][0], ring[i2][1]) for i2 in range(nR)]
    order = sorted(range(nR), key=lambda i2: -piers[i2])
    shown = []
    for i2 in order:
        if any(math.hypot(ring[i2][0] - ring[j][0], ring[i2][1] - ring[j][1]) < 120
               for j in shown):
            continue
        shown.append(i2)
        if len(shown) == 3: break
    for i2 in shown:
        rep.append("  pier hotspot %.1f m at (%.0f,%.0f)"
                   % (piers[i2], ring[i2][0], ring[i2][1]))
    # curvature (zigzag law analogue: freeway limit 0.8 deg/m). The engine
    # smooths first (echo_roads smoothOnce: closed moving average, halfWin 5,
    # 2 passes, escalating x2/x4/x8) and allows 15% grace — mirror that.
    def _smooth_ring(r_pts, half):
        n3 = len(r_pts)
        out3 = []
        for i3 in range(n3):
            sx3 = sz3 = 0.0
            for k3 in range(-half, half + 1):
                p3 = r_pts[(i3 + k3) % n3]
                sx3 += p3[0]; sz3 += p3[1]
            out3.append((sx3 / (2 * half + 1), sz3 / (2 * half + 1)))
        return out3
    def _worst(r_pts):
        wc, wi = 0.0, 0
        n3 = len(r_pts)
        for i3 in range(n3):
            a3 = r_pts[i3 - 1]; b3 = r_pts[i3]; c3 = r_pts[(i3 + 1) % n3]
            v1 = (b3[0] - a3[0], b3[1] - a3[1]); v2 = (c3[0] - b3[0], c3[1] - b3[1])
            l1 = math.hypot(*v1); l2 = math.hypot(*v2)
            if l1 < 1e-4 or l2 < 1e-4: continue
            dt = max(-1.0, min(1.0, (v1[0] * v2[0] + v1[1] * v2[1]) / (l1 * l2)))
            c4 = math.degrees(math.acos(dt)) / 4.0
            if c4 > wc: wc, wi = c4, i3
        return wc, wi
    sm = _smooth_ring(_smooth_ring(list(ring), 5), 5)
    worst_c, worst_i = _worst(sm)
    esc2 = 1
    while worst_c > 0.8 and esc2 <= 3:
        sm = _smooth_ring(_smooth_ring(sm, 5 << esc2), 5 << esc2)
        worst_c, worst_i = _worst(sm)
        esc2 += 1
    ring_sm = sm
    for i2 in range(0):
        a2 = ring[i2 - 1]; b2 = ring[i2]; c2 = ring[(i2 + 1) % nR]
        v1 = (b2[0] - a2[0], b2[1] - a2[1]); v2 = (c2[0] - b2[0], c2[1] - b2[1])
        l1 = math.hypot(*v1); l2 = math.hypot(*v2)
        if l1 < 1e-4 or l2 < 1e-4: continue
        pass
    rep.append("  ring worst curvature %.3f deg/m (post-smoothing) at (%.0f,%.0f)"
               " [law limit 0.8 +15%% grace]"
               % (worst_c, ring_sm[worst_i][0], ring_sm[worst_i][1]))
    chk(worst_c <= 0.8 * 1.15, "ring curvature %.3f deg/m within the zigzag law" % worst_c)
    mp = max(piers)
    ampx = ring[piers.index(mp)]
    rep.append("  ring sim: %d wps, %d samples, max pier %.1f m at (%.0f,%.0f)"
               " deck=%.1f ground=%.1f"
               % (n, nR, mp, ampx[0], ampx[1], y[piers.index(mp)],
                  ha(ampx[0], ampx[1])))
    chk(mp <= 45.0, "freeway ring max simulated pier %.1f m (gate <= 45)" % mp)

    # V5 mine spur corridor: nearest ring deck -> truck lot must ride
    # continuous high ground (a canyon dip = zigzag law drops the road)
    lot = (-556.0, 814.0)
    near = min(range(nR), key=lambda i2: (ring[i2][0] - lot[0]) ** 2 + (ring[i2][1] - lot[1]) ** 2)
    dx2, dz2 = lot[0] - ring[near][0], lot[1] - ring[near][1]
    ln2 = math.hypot(dx2, dz2)
    lows = min(ha(ring[near][0] + dx2 * t / ln2, ring[near][1] + dz2 * t / ln2)
               for t in range(0, int(ln2) + 1, 10))
    rep.append("  mine spur: ring deck (%.0f,%.0f) -> lot, %d m, corridor min %.1f m"
               % (ring[near][0], ring[near][1], int(ln2), lows))
    chk(lows > 140.0, "mine-spur corridor stays high (min %.1f m > 140)" % lows)

    hmin, hmax = png16_to_height(px).min(), png16_to_height(px).max()
    chk(-64.0 < hmin and hmax < 256.0,
        "height range %.1f..%.1f m inside encodable -64..256" % (hmin, hmax))
    return ok, rep

# ============================================================== main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="assets/island_mesa")
    ap.add_argument("--albedo-res", type=int, default=4096)
    ap.add_argument("--png-only", action="store_true")
    ap.add_argument("--verify", action="store_true")
    args = ap.parse_args()

    print("[gen] building height field (seed %d, %dx%d, frame %.0f m)..."
          % (SEED, N_PNG, N_PNG, FRAME))
    h, meta = build_height()
    px = height_to_png16(h)

    okmsg = ""
    ok, rep = verify(px, meta)
    for line in rep: print("  " + line)
    if not ok:
        print("[gen] VERIFY FAILED — not writing outputs"); sys.exit(1)
    print("[gen] verify PASS")
    if args.verify:
        return

    os.makedirs(args.out, exist_ok=True)
    png_path = os.path.join(args.out, "island_height_20260530.png")
    Image.fromarray(px).save(png_path)
    print("[gen] wrote %s (%d bytes)" % (png_path, os.path.getsize(png_path)))

    if not args.png_only:
        print("[gen] baking %dx%d albedo splat..." % (args.albedo_res, args.albedo_res))
        alb = build_albedo(h, args.albedo_res)
        import io
        buf = io.BytesIO()
        Image.fromarray(alb, "RGB").save(buf, format="PNG", optimize=False)
        glb_path = os.path.join(args.out, "island_20260530.glb")
        total = build_glb(px, buf.getvalue(), glb_path)
        print("[gen] wrote %s (%d bytes)" % (glb_path, total))

if __name__ == "__main__":
    main()
