# Volumetric God-Rays / Light Shafts — screenshots

Screen-space radial scatter (mode 1) god-rays. The shaft buffer is computed from
the HDR scene + depth (occluder-masked toward the projected sun position) and
ADDED into the HDR scene color BEFORE ACES tonemap.

All captures are deterministic (headless `--screenshot-*`). On/off pairs are
captured with the SAME exe; `--nogodrays` forces the effect fully off.

| file | what |
|---|---|
| `sky_godrays_on.png`  | `--screenshot-sky` with god-rays ON at the shipped default `r_godrays_intensity 0.55`. |
| `sky_godrays_off.png` | Same vantage, `--nogodrays`. |
| `sky_godrays_contribution_diff_x3.png` | (on − off) amplified 3× — isolates the warm radial scatter the effect adds around the sun. |
| `sky_godrays_pushed_1p6.png` | Intensity pushed to 1.6 (the intro can push the defaults). |
| `coldopen_engineglow_beat.png` | Cold-open space beat context (engine-glow). The sun is off-frame in this framing, so god-rays is a correct no-op here — shafts only appear when a bright source projects on/near screen with a far-plane occluder edge. |

## A/B (r_godrays 0 == base, byte-identical)

Under an identical post path (`--notaa`), a pristine `origin/main` build and this
branch with `--nogodrays` produce **byte-identical** output (md5 `d41a3bee…`),
confirming the god-rays code is fully inert when off.

## Cvars

`r_godrays` (1), `r_godrays_intensity` (0.55), `r_godrays_density` (0.6),
`r_godrays_decay` (0.96), `r_godrays_weight` (0.35). All live.
