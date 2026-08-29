# Wormhole TRANSIT — the ride (feat/wormhole-transit-ride)

Evidence for the interior of the jump. The exterior lanes built the throat you
fly *toward*; this is the corridor you fly *through*, and the star you come out
under.

All frames captured headless from `--world space` on the real world (not a
showcase world), 1280x720, `X3_MUTE=1`.

## The transit series

`X3_WORMHOLE_ENTER=<i>` parks the ship in wormhole *i*'s mouth so the capture
path actually takes the transit; `X3_WORMHOLE_T=<sec>` pre-rolls the whole world
at a fixed 1/165 s step before the capture frame, so each frame below is the
frame the 165 Hz game shows at that instant. **A single still cannot judge a
ride** — read these in order.

```
X3_WORMHOLE_ENTER=0 X3_WORMHOLE_T=<t> X3Space.exe --world space --smoketest --screenshot <out>
```

| file | t | what it shows |
|---|---|---|
| `01_entry_t0.95.png` | 0.95 s | ENTRY. The throat has just closed around the ship; corridor still narrow, DIST 8.40 ly, VEL 6664 c. |
| `02_tunnel_t1.80.png` | 1.80 s | The corridor opens out; filaments start streaming. |
| `03_tunnel_t4.60_hud_offscale.png` | 4.60 s | Mid-TUNNEL. **The HUD readouts at their off-scale values**: POS X/Y/Z and VEL are `#########` + `OVR`, DIST 3.16 ly and falling, ETA 3.6 s. |
| `04_tunnel_t6.20.png` | 6.20 s | Late tunnel; the throat has rolled and banked well off its entry attitude. |
| `05_exit_t8.10.png` | 8.10 s | EXIT. Membrane wash blooming, DIST 0.0004 ly, VEL decelerating through 50.9 c. |
| `06_unstable_corridor_t4.60.png` | 4.60 s | The UNSTABLE corridor (`X3_WORMHOLE_ENTER=1`) — violet-pushed spectrum, and AEGIS is saying something different. |

## Arrival — "like a different colour sun"

Departure and both arrivals, **same fixed capture camera** aimed down the
sun ray (`--shot-cam 4814,4011,4974,0.8018,0.5254`), so the only difference
between the three frames is which star you are under.

| file | system | star |
|---|---|---|
| `07_DEPART_kethzar_amber_sun.png` | Kethzar Prime | K0 hypergiant, amber |
| `08_ARRIVE_sirius_bluewhite_sun.png` | Sirius | A1V blue-white (via THE GAMMA CORRIDOR) |
| `09_ARRIVE_wolf359_reddwarf_sun.png` | Wolf 359 | M6.5V red dwarf (via THE DERELICT APERTURE, which was *plotted* for Tau Ceti and did not get there) |

## Arrival — the star's light on the hull

Same three skies, same fixed capture camera, same fixed decor-fleet world
positions. Measured over a 310,460-pixel hull mask:

| frame | system | hull R/B | hull luminance |
|---|---|---|---|
| `10_DEPART_hull_kethzar.png` | Kethzar Prime (K0 amber) | 0.7321 | 43.03 |
| `11_ARRIVE_hull_sirius.png` | Sirius (A1V blue-white) | 0.7870 | 46.55 (+8.2%) |
| `12_ARRIVE_hull_wolf359.png` | Wolf 359 (M6.5V red dwarf) | 0.8663 (+18.3% warmer) | 39.94 (−7.2%) |

The star drives the key/fill/rim rig, the analytic sky's directional key and the
deep-space ambient — so it changes what the hull is lit *by*, not only what is
painted behind it.

## Clipping

`pureWhite` = pixels at exactly 255/255/255. Across the whole transit series:
**0.0000%**. The convergence core is deliberately tuned so its 14-shell additive
stack *sums* to just under the tonemapper knee (it measured 0.0215% before that
bound went in), so the brightest region of the frame still has gradient inside it.

Note when reading absolute brightness numbers off these frames: the engine runs
**auto-exposure** (`r_autoexposure`, aeMin 0.70 / aeMax 2.20), so mean frame
luminance is pinned near mid regardless of how the effect is driven. Contrast and
structure are the meaningful measures here, not mean level.
