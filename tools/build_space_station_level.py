#!/usr/bin/env python3
"""Generate assets/levels/space_station.leveldoc.json — a real, assembled,
connected space-station scene built from real GLB kit-of-parts pieces, seated
using their ACTUAL bounding boxes (see D:\\Assets\\_glb\\_host\\scifi_station_bbox.json,
produced by tools/glb_bbox_scan.py-equivalent scan over Vol2/Vol3/SpaceStationsCreator).

PIVOT CONVENTIONS (reverse-engineered from real bboxes, see spacestation_build.log
for the full survey):
  - Vol2 "3D Scifi Kit Vol 2" (interior kit, 6m grid, 4m story) and the Vol3
    "Ext_*"/"Roof_01..04"/"Stair_Ext*" family (exterior kit, 6m grid, 5m story):
    every piece is CORNER-PIVOTED with local origin (0,0,0) sitting exactly on
    one of the piece's own bbox corners. Critically, placing pos=(x,y,z) directly
    (yaw=0) puts that corner at world (x,y,z) and the rest of the mesh hangs off
    it in whatever direction its min/max says — for floor/wall/corridor pieces
    Y=0 is the BOTTOM (bottom-flush automatically), for platform deck tiles Y=0
    is the TOP surface, and for roof caps the local origin represents the SAME
    "floor datum" as the walls below it (roof geometry starts at Y=story height
    and rises from there) — so an entire room cell (floor+walls+roof) can share
    ONE (x,z) footprint anchor and ONE y = floor height, and every piece stacks
    correctly. This generator exploits that: `place()` below just sets pos
    directly; grid helpers compute the corner anchor per cell.
  - Vol3 "Platerform_Metal_*" and "Stargate_*" families are CENTER-PIVOTED
    (local origin = geometric center in X/Z at least). Stargate_Warp/_Part_A/B/C
    additionally preserve their ORIGINAL relative offsets from a shared gate
    origin (their min/max are NOT centered on 0 individually except Warp) —
    placing all four at the *same* pos/yaw/scale reassembles the ring correctly,
    because each part's local vertex data already encodes its true offset from
    that shared origin (they were exported as siblings of one gate assembly).
  - Space_Base_Module_01 has its local origin at the mass's top-front-most
    point (an attachment/anchor point) with the bulk of the mountain hanging
    away in -Y/-Z — placing pos at a pad's back edge at deck height lets the
    mass fall away below/behind exactly as a "built on a mountain outcrop"
    silhouette.

SCALE OUTLIERS: Space_Base_Module and the four Stargate_* pieces were exported
at a wildly different unit scale than the rest of the kit (hundreds of units
where a "big" ordinary piece is 10-25 units) — see the manifest. This generator
applies a corrective uniform `scale` per family (0.075 for the base module,
0.05 for the stargate) to bring them into proportion with the 6m modular grid.
Flagged in the handoff for Tim to eyeball.
"""
import json
import math
import os

MANIFEST_PATH = r"D:\Assets\_glb\_host\scifi_station_bbox.json"
# Write into THIS worktree (feat/space-station-land). Resolved relative to the
# script location so it works from any checkout, not just the dragdrop clone.
OUT_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "assets", "levels", "space_station.leveldoc.json")

VOL2 = "3D Scifi Kit Vol 2"
VOL3 = "3D Scifi Kit Vol 3"
# NOTE: the raw staged packs (ScifiKitVol2/ScifiKitVol3, hardlinked straight from the
# Creepy Cat source trees) are KHR_draco_mesh_compression-required GLBs — this engine's
# ModelLoader.cpp has NO Draco decoder (it unconditionally skips draco primitives), so
# every raw-staged piece silently rendered as a graybox marker. Fixed by decoding through
# `npx @gltf-transform/cli copy <src> <dst>` (see tools/decode_draco_glb.py) into sibling
# "*Decoded" pack dirs, junctioned in alongside the raw packs. Use the DECODED pack names
# for every model reference in this generator.
PACK2 = "ScifiKitVol2Decoded"   # assets/converted_glb/<PACK2>/<basename>
PACK3 = "ScifiKitVol3Decoded"

with open(MANIFEST_PATH) as f:
    _manifest = json.load(f)["files"]

# basename -> {pack: {min,max,size}}  (basenames can collide across packs, e.g.
# Radar_Part_01.glb exists in both Vol2 and Vol3 with different geometry)
_by_name = {}
for _path, info in _manifest.items():
    _by_name.setdefault(info["basename"], {})[info["pack"]] = info


def bbox(basename, pack):
    d = _by_name.get(basename)
    if not d or pack not in d:
        raise KeyError(f"no bbox for {basename} in pack '{pack}'")
    return d[pack]


doc_entities = []
doc_brushes = []
_name_counts = {}


def _uniq(name):
    n = _name_counts.get(name, 0)
    _name_counts[name] = n + 1
    return name if n == 0 else f"{name}_{n}"


def place(basename, pack_folder, pack_key, x, y, z, yaw=0.0, scale=1.0,
          tint=(1, 1, 1), name=None, emissive=None):
    """Place a model entity with pos=(x,y,z) directly (see module docstring —
    for every family used here, the local origin IS the meaningful anchor).

    `emissive` = optional (r, g, b, strength) SELF-LIT term (canon "emissive
    where it glows"). The loader (leveldoc_world.cpp) forwards it onto every
    drawable's Scene::Entity.emissive so the prop glows independent of light —
    used for the fusion core, the solar cell faces, and the stargate horizon.
    Model `tint` is ignored by the loader (models keep their GLB baseColor), so
    glow MUST come through this field, not tint."""
    _ = bbox(basename, pack_key)  # validates the piece exists / real dims known
    e = {
        "name": _uniq(name or basename.replace(".glb", "")),
        "type": "model",
        "pos": [x, y, z],
        "yaw": yaw,
        "scale": scale,
        "tint": list(tint),
        "size": [0, 0, 0],
        "model": f"{pack_folder}/{basename}",
        "script": "",
    }
    if emissive is not None:
        e["emissive"] = list(emissive)
    doc_entities.append(e)
    return e


def place_seated(basename, pack_folder, pack_key, cx, bottom_y, cz, yaw=0.0,
                  scale=1.0, tint=(1, 1, 1), name=None, emissive=None):
    """For CENTER-pivoted pieces (Platerform_Metal_*): seat so the piece's
    world CENTER is (cx, ?, cz) and its bottom face sits flush at bottom_y."""
    b = bbox(basename, pack_key)
    y = bottom_y - scale * b["min"][1]
    return place(basename, pack_folder, pack_key, cx, y, cz, yaw, scale, tint, name,
                 emissive=emissive)


def light(name, x, y, z, tint, brightness):
    doc_entities.append({
        "name": name, "type": "light", "pos": [x, y, z], "yaw": 0, "scale": brightness,
        "tint": list(tint), "size": [0, 0, 0], "model": "", "script": "",
    })


def brush(name, btype, x, y, z, sx, sy, sz, yaw=0.0, tint=(0.5, 0.5, 0.55),
          material="floor", collide=True):
    doc_brushes.append({
        "name": name, "type": btype, "pos": [x, y, z], "size": [sx, sy, sz],
        "yaw": yaw, "tint": list(tint), "material": material, "collide": 1 if collide else 0,
    })


TAU = math.pi * 2.0
HALF_PI = math.pi * 0.5

# ===========================================================================
# LAYOUT
# ===========================================================================
# World: X right, Y up, Z depth. The station runs along +Z: HANGAR (west pad,
# Z -30..-6) -> CORRIDOR (Z -6..18) -> STARGATE PAD (east pad, Z 18..42).
# Deck datum GY = the walkable surface height everywhere (flush, zero step at
# every junction per the height-transition law).
GY = 2.0

# ---------------------------------------------------------------------------
# ZONE A — HANGAR PAD: footprint X:[-12,12] Z:[-30,-6], a 4x4 grid of 6m
# Plateform_Center_01 deck tiles (corner-pivoted at local (0,0,0)=top surface,
# top-max corner -> place at the tile's WORLD max-X,max-Z corner).
# ---------------------------------------------------------------------------
HANGAR_X0, HANGAR_Z0 = -12.0, -30.0
for i in range(4):       # X cells
    for j in range(4):   # Z cells
        wx = HANGAR_X0 + (i + 1) * 6.0
        wz = HANGAR_Z0 + (j + 1) * 6.0
        place("Plateform_Center_01.glb", PACK2, VOL2, wx, GY, wz,
              name=f"hangar_deck_{i}_{j}")

# Edge trim: Plateform_Line along the west outer edge (i=0 face, x=-12), and
# Plateform_Angle at the two west corners — decorative edge, doesn't affect
# the connected walkable footprint (the Center tiles already cover it).
for j in range(4):
    wz = HANGAR_Z0 + (j + 1) * 6.0
    place("Plateform_Line_05.glb", PACK2, VOL2, HANGAR_X0, GY, wz, yaw=0,
          name=f"hangar_edge_w_{j}")
place("Plateform_Angle_05.glb", PACK2, VOL2, HANGAR_X0, GY, HANGAR_Z0, yaw=0,
      name="hangar_corner_sw")
place("Plateform_Angle_05.glb", PACK2, VOL2, HANGAR_X0, GY, HANGAR_Z0 + 24.0,
      yaw=HALF_PI, name="hangar_corner_nw")

# Safety-rail fence along the two OUTER long edges (west + north/back), using
# Room_Fence_01 (5.716m panels) chained — decorative, seats on the deck top.
for j in range(4):
    wz = HANGAR_Z0 + j * 6.0 + 0.15
    place("Room_Fence_01.glb", PACK2, VOL2, HANGAR_X0, GY, wz + 5.716, yaw=0,
          name=f"hangar_fence_w_{j}")

# ---- Hangar shell: Ext_Wall_Addon (6m x 5m story, native fit to the 6m grid
# and to Roof_01's 5m datum) forms the back wall + partial side walls; the
# front (facing +Z, the corridor) stays open as the hangar mouth. ----
BACK_Z = HANGAR_Z0            # z = -30, back wall plane
for i in range(4):
    wx = HANGAR_X0 + (i + 1) * 6.0
    place("Ext_Wall_Addon_01.glb", PACK3, VOL3, wx, GY, BACK_Z, yaw=0,
          name=f"hangar_wall_back_{i}")
# Side walls: enclose the back half only (z -30..-12), leave the front half
# (z -12..-6) open as the apron/mouth.
for j in range(2):
    wz = HANGAR_Z0 + (j + 1) * 6.0
    place("Ext_Wall_Addon_01.glb", PACK3, VOL3, HANGAR_X0, GY, wz, yaw=HALF_PI,
          name=f"hangar_wall_west_{j}")
    place("Ext_Wall_Addon_01.glb", PACK3, VOL3, HANGAR_X0 + 24.0, GY, wz - 6.0,
          yaw=-HALF_PI, name=f"hangar_wall_east_{j}")
# Roof caps over the whole 24x24 pad (Roof_01 = one 6x6 cap per cell, datum
# Y=5 matching the Ext_Wall_Addon story height).
for i in range(4):
    for j in range(4):
        wx = HANGAR_X0 + (i + 1) * 6.0
        wz = HANGAR_Z0 + (j + 1) * 6.0
        place("Roof_01.glb", PACK3, VOL3, wx, GY, wz, name=f"hangar_roof_{i}_{j}")

# Monumental entrance pylons flanking the hangar mouth (z=-6), using the BIG
# 10m-tall Wall_Big_Simple as freestanding entrance framing (not a roofed
# room -- these don't need a matching roof piece, they're an entry arch).
place("Wall_Big_Simple_02.glb", PACK3, VOL3, HANGAR_X0 - 4.0, GY, -6.0, yaw=HALF_PI,
      name="hangar_pylon_w")
place("Wall_Big_Angle_01.glb", PACK3, VOL3, HANGAR_X0, GY, -6.0, yaw=0,
      name="hangar_pylon_w_cap")
place("Wall_Big_Simple_02.glb", PACK3, VOL3, HANGAR_X0 + 24.0 + 4.0, GY, -6.0 - 12.0,
      yaw=-HALF_PI, name="hangar_pylon_e")
place("Wall_Big_Angle_01.glb", PACK3, VOL3, HANGAR_X0 + 24.0, GY, -6.0 - 4.0, yaw=HALF_PI,
      name="hangar_pylon_e_cap")

# Hangar dressing: landing gear, crates, radar on the deck.
place("Landing_Gear_01.glb", PACK3, VOL3, HANGAR_X0 + 9.0, GY, HANGAR_Z0 + 9.0,
      name="hangar_gear_a")
place("Landing_Gear_01.glb", PACK3, VOL3, HANGAR_X0 + 15.0, GY, HANGAR_Z0 + 9.0,
      yaw=math.pi, name="hangar_gear_b")
place("Radar_Base_01.glb", PACK3, VOL3, HANGAR_X0 + 3.0, GY, HANGAR_Z0 + 3.0,
      name="hangar_radar_base")
place("Radar_Part_02.glb", PACK3, VOL3, HANGAR_X0 + 3.0, GY + 4.4, HANGAR_Z0 + 3.0,
      name="hangar_radar_dish")

# ---------------------------------------------------------------------------
# ZONE B — CORRIDOR: chain of Corridor_Coin_Big segments (8m each) from the
# hangar mouth (z=-6) to the stargate pad (z=18): 3 segments, 24m span.
# Corridor_Coin_Big_01 local: X 0..8 (width), Z -8..0 (length, extends -Z from
# pivot), Y 0..4 (bottom flush). Chain along +Z: segment k's pivot Z is placed
# at the FAR (max) end of its span, so span_k = [z0+k*8, z0+(k+1)*8].
# ---------------------------------------------------------------------------
CORR_X0 = -4.0     # west edge of the 8m-wide corridor footprint
CORR_Z0 = -6.0
for k in range(3):
    wz = CORR_Z0 + (k + 1) * 8.0
    place("Corridor_Coin_Big_01.glb", PACK2, VOL2, CORR_X0, GY, wz,
          name=f"corridor_seg_{k}")
    place("Corridor_Coin_Big_Bottom_01.glb", PACK2, VOL2, CORR_X0, GY, wz,
          name=f"corridor_floor_{k}")

# ---------------------------------------------------------------------------
# ZONE C — STARGATE PAD: footprint X:[-12,12] Z:[18,42], centered on a big
# Platerform_Metal_02 deck plate (center-pivoted, ~22.5m across) ringed with
# Plateform_Line trim tiles to connect flush to the corridor mouth at z=18.
# ---------------------------------------------------------------------------
PAD2_CX, PAD2_CZ = 0.0, 30.0
place_seated("Platerform_Metal_02.glb", PACK3, VOL3, PAD2_CX, GY, PAD2_CZ,
             name="stargate_pad_core")

# Connector deck tiles bridging the corridor mouth (z=18) to the round pad
# core (Vol2 Plateform_Center_01, corner-pivoted, same 6m grid as the corridor
# width) -- 2x2 cells spanning X:[-6,6] Z:[18,30], flush with GY throughout.
for i in range(2):
    for j in range(2):
        wx = -6.0 + (i + 1) * 6.0
        wz = 18.0 + (j + 1) * 6.0
        place("Plateform_Center_01.glb", PACK2, VOL2, wx, GY, wz,
              name=f"pad2_connector_{i}_{j}")

# Edge trim around the pad's outer rim.
for i, (ex, ez, yaw) in enumerate([
    (-12.0, 24.0, 0.0), (-12.0, 30.0, 0.0), (-12.0, 36.0, 0.0),
    (12.0, 24.0 - 6.0, math.pi), (12.0, 30.0 - 6.0, math.pi), (12.0, 36.0 - 6.0, math.pi),
]):
    place("Plateform_Line_05.glb", PACK2, VOL2, ex, GY, ez, yaw=yaw,
          name=f"pad2_edge_{i}")

# ---- Stargate centerpiece: Stargate_Warp_01 (the ring/event-horizon disc)
# plus Part_A/B/C, all placed at the IDENTICAL pos/yaw/scale — their local
# vertex data already encodes each part's true offset from a shared gate
# origin (see module docstring), so this single shared placement reassembles
# the ring. scale=0.05 brings the 822-unit raw diameter down to ~41m; the
# ring stands in the local X-Y plane (already "upright" -- no extra rotation
# needed), facing -Z back down the corridor toward the player start. ----
GATE_SCALE = 0.05
GATE_X, GATE_Z = PAD2_CX, PAD2_CZ + 6.0     # far half of the pad, facing the corridor
gate_radius = GATE_SCALE * bbox("Stargate_Warp_01.glb", VOL3)["max"][0]   # ~20.55
GATE_Y = GY + gate_radius                    # bottom of the ring flush with the deck
for gbn in ("Stargate_Warp_01.glb", "Stargate_Part_A.glb", "Stargate_Part_B.glb", "Stargate_Part_C.glb"):
    place(gbn, PACK3, VOL3, GATE_X, GATE_Y, GATE_Z, yaw=math.pi, scale=GATE_SCALE,
          tint=(0.55, 0.85, 1.0) if "Warp" in gbn else (1, 1, 1),
          # The event-horizon disc SELF-LITS to an active cyan portal (tint alone
          # does nothing on models); the ring framework stays honest PBR.
          emissive=(0.30, 0.70, 1.0, 2.2) if "Warp" in gbn else None,
          name=gbn.replace(".glb", "").lower())

# ---- Space_Base_Module: the "mountain-top base" foundation mass the whole
# station is anchored to -- its local origin sits at the mass's top-front
# attachment point, so placing it at the pad's back edge (deck height) lets
# the ~30-45m mass fall away below/behind into the void, exactly the
# "built on a mountain outcrop" silhouette. scale=0.075. ----
place("Space_Base_Module_01.glb", PACK3, VOL3, PAD2_CX, GY, 42.0, yaw=math.pi,
      scale=0.075, name="mountain_base")

# Scatter detail on/around the stargate pad: exterior lights,
# antennae, air grids -- seated on the deck (GY) at its perimeter.
place("Light_Exterior_01.glb", PACK3, VOL3, -10.0, GY, 40.0, name="pad2_light_a")
place("Light_Exterior_01.glb", PACK3, VOL3, 9.0, GY, 40.0, name="pad2_light_b")
place("Antena_03.glb", PACK3, VOL3, -10.0, GY, 36.0, scale=0.3, name="antenna_a")
place("Air_Grid_01.glb", PACK3, VOL3, 4.0, GY, 22.0, name="airgrid_a")
place("Air_Grid_02.glb", PACK3, VOL3, -4.0, GY, 22.0, name="airgrid_b")

# Corridor greeble: exterior lights along the tube, air grids underfoot.
for k in range(3):
    wz = CORR_Z0 + k * 8.0 + 4.0
    light(f"corridor_light_{k}", 0.0, GY + 3.0, wz, (0.55, 0.8, 1.0), 1.6)

# ===========================================================================
# ZONE D — POWER: SOLAR ARRAYS + FUSION REACTOR
# Owner intent: "Solar and fusion reactor power tho space stations." Canon
# self-lit: emissive is where it glows (cell faces / the reactor heart / the
# containment torus); the structural steel stays honest PBR. Model `tint` is
# ignored by the loader, so ALL glow rides the `emissive` field.
# ===========================================================================

# ---- SOLAR ARRAYS: two big wings on booms off the station's flanks. The
# LevelDoc format is yaw-only (no pitch), so the panels are laid FLAT and lifted
# on masts — the classic ISS-style deployed array read against deep space. Each
# wing is a 4x2 grid of Solar_Panel_04 (2.23m x 6.02m cells) elevated ~11 m on a
# Pipe_Tube_02 mast rising from the deck edge. Cell faces SELF-LIT deep blue. ----
SOLAR_Y = GY + 11.0
PANEL_DX, PANEL_DZ = 2.30, 6.10
SOLAR_EMIS = (0.10, 0.30, 0.85, 1.15)   # deep photovoltaic blue, gentle self-glow


def solar_wing(cx, cz, yaw, tag):
    # Mast/boom up from the deck edge to the array.
    place("Pipe_Tube_02.glb", PACK3, VOL3, cx, GY, cz, name=f"{tag}_mast_a")
    place("Pipe_Tube_02.glb", PACK3, VOL3, cx, GY + 5.0, cz, name=f"{tag}_mast_b")
    # 4 (outboard) x 2 (along the hull) grid of flat cells.
    for i in range(4):
        for j in range(2):
            px = cx + (2.0 + i) * PANEL_DX * (1.0 if cx >= 0 else -1.0)
            pz = cz + (j - 0.5) * PANEL_DZ
            place("Solar_Panel_04.glb", PACK3, VOL3, px, SOLAR_Y, pz, yaw=yaw,
                  emissive=SOLAR_EMIS, name=f"{tag}_cell_{i}_{j}")


# East wing off the hangar's +X flank, west wing off the -X flank.
solar_wing(14.0, -15.0, 0.0, "solar_east")
solar_wing(-14.0, -15.0, 0.0, "solar_west")
# A softly-glowing fill light under each array so the blue reads even edge-on.
light("solar_east_glow", 22.0, SOLAR_Y - 1.0, -15.0, (0.25, 0.5, 1.0), 1.4)
light("solar_west_glow", -22.0, SOLAR_Y - 1.0, -15.0, (0.25, 0.5, 1.0), 1.4)

# ---- FUSION REACTOR ANNEX: the station's obvious power heart, on a raised
# platform branching WEST off the corridor and connected by a short walkway. The
# Cold-Fusion reactor core burns hot cyan-white, wrapped in a glowing containment
# torus (Light_Ring) and flanked by support struts. ----
REACTOR_CX, REACTOR_CZ = -28.0, 6.0
# Reactor pad (center-pivoted metal plate, seated flush at GY).
place_seated("Platerform_Metal_02.glb", PACK3, VOL3, REACTOR_CX, GY, REACTOR_CZ,
             scale=0.8, name="reactor_pad")
# Connector deck tiles bridging the corridor's west edge (x=-4) to the pad.
for i in range(2):
    place("Plateform_Center_01.glb", PACK2, VOL2, -10.0 - i * 6.0, GY, REACTOR_CZ + 3.0,
          name=f"reactor_bridge_{i}")

# The fusion core: Reactor_ColdFusion_01 (12m x 9.76m), bottom (local minY=-3)
# seated on the pad deck, facing the corridor. SELF-LIT hot cyan-white heart.
RC = bbox("Reactor_ColdFusion_01.glb", VOL3)
place("Reactor_ColdFusion_01.glb", PACK3, VOL3, REACTOR_CX, GY - RC["min"][1], REACTOR_CZ,
      yaw=HALF_PI, emissive=(0.55, 0.88, 1.0, 3.2), name="fusion_core")
# Glowing containment torus around the core mid-height (Light_Ring, scaled up).
CORE_MID = GY - RC["min"][1] + (RC["min"][1] + RC["max"][1]) * 0.5
place("Light_Ring_01.glb", PACK3, VOL3, REACTOR_CX, CORE_MID, REACTOR_CZ, yaw=HALF_PI,
      scale=1.6, emissive=(0.35, 0.80, 1.0, 2.6), name="fusion_torus")
place("Light_Ring_01.glb", PACK3, VOL3, REACTOR_CX, CORE_MID + 3.2, REACTOR_CZ, yaw=HALF_PI,
      scale=1.2, emissive=(0.35, 0.80, 1.0, 2.2), name="fusion_torus_up")
# Support struts flanking the core (honest PBR steel).
place("Reactor_Support_01.glb", PACK3, VOL3, REACTOR_CX, GY, REACTOR_CZ - 6.0, name="fusion_strut_a")
place("Reactor_Support_01.glb", PACK3, VOL3, REACTOR_CX, GY, REACTOR_CZ + 6.0, yaw=math.pi,
      name="fusion_strut_b")
# A secondary reactor face (Cruiser_Reactor) inset behind the core as a second
# glowing aperture (reads as the power bank).
place("Cruiser_Reactor_01.glb", PACK3, VOL3, REACTOR_CX - 5.0, GY + 3.0, REACTOR_CZ, yaw=HALF_PI,
      scale=0.7, emissive=(0.30, 0.75, 1.0, 2.4), name="fusion_bank")
# Hot cyan pooled light off the reactor onto the deck + corridor.
light("fusion_key", REACTOR_CX + 3.0, CORE_MID, REACTOR_CZ, (0.5, 0.85, 1.0), 3.2)
light("fusion_fill", REACTOR_CX, CORE_MID + 2.0, REACTOR_CZ, (0.45, 0.9, 1.0), 2.6)
light("fusion_corridor", -6.0, GY + 3.0, REACTOR_CZ, (0.4, 0.8, 1.0), 2.0)

# Collision so the reactor annex is WALKABLE (visual GLBs carry no body).
brush("floor_reactor_pad", 0, REACTOR_CX, GY - 0.15, REACTOR_CZ, 18.0, 0.3, 18.0,
      tint=(0.3, 0.3, 0.34), material="floor", collide=True)
brush("floor_reactor_bridge", 0, -14.0, GY - 0.15, REACTOR_CZ + 3.0, 20.0, 0.3, 6.0,
      tint=(0.3, 0.3, 0.34), material="floor", collide=True)

# ===========================================================================
# LIGHTS — key light over the hangar mouth, warm work-lights in the hangar,
# cool wash on the stargate for a "power spike" read.
# ===========================================================================
light("hangar_key", HANGAR_X0 + 12.0, GY + 8.5, HANGAR_Z0 + 12.0, (1.0, 0.93, 0.8), 2.4)
light("hangar_mouth", 0.0, GY + 6.0, -8.0, (1.0, 0.95, 0.85), 1.8)
light("gate_wash_a", GATE_X - 8.0, GATE_Y, GATE_Z, (0.4, 0.75, 1.0), 2.6)
light("gate_wash_b", GATE_X + 8.0, GATE_Y, GATE_Z, (0.4, 0.75, 1.0), 2.6)
light("gate_core", GATE_X, GATE_Y, GATE_Z + 2.0, (0.6, 0.9, 1.0), 3.0)

# ===========================================================================
# COLLISION FLOOR BRUSHES — invisible/graybox boxes matching the deck
# footprints so the player can actually walk the connected path (the GLB
# props above are visual-only; see leveldoc_world.cpp spawnEntity: type
# "model" gets no physics body). Thin slabs just under the visual deck.
# ===========================================================================
brush("floor_hangar", 0, 0.0, GY - 0.15, -18.0, 24.0, 0.3, 24.0,
      tint=(0.3, 0.3, 0.34), material="floor", collide=True)
brush("floor_corridor", 0, 0.0, GY - 0.15, 6.0, 8.0, 0.3, 24.0,
      tint=(0.3, 0.3, 0.34), material="floor", collide=True)
brush("floor_pad2", 0, 0.0, GY - 0.15, 30.0, 24.0, 0.3, 24.0,
      tint=(0.3, 0.3, 0.34), material="floor", collide=True)

# A faint starfield-adjacent ground-plane FAR below, purely so a stray look-
# down doesn't show infinite void immediately under the station.
brush("void_floor", 0, 0.0, GY - 40.0, 6.0, 400.0, 0.5, 400.0,
      tint=(0.05, 0.05, 0.08), material="solid", collide=False)

# ===========================================================================
# DOC
# ===========================================================================
doc = {
    "name": "space_station",
    "biome": "space",
    "playerStart": [0.0, GY + 1.7, -20.0],
    "entities": doc_entities,
    "brushes": doc_brushes,
}

os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
with open(OUT_PATH, "w") as f:
    json.dump(doc, f, indent=1)

print(f"wrote {OUT_PATH}: {len(doc_entities)} entities, {len(doc_brushes)} brushes")
print(f"  gate ring diameter ~{2*gate_radius:.1f}m, gate center Y={GATE_Y:.1f}")
print(f"  station extent: X [{HANGAR_X0:.0f},{HANGAR_X0+24:.0f}]  Z [{HANGAR_Z0:.0f},{PAD2_CZ+12:.0f}]")
