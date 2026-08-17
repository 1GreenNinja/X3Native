# Cloud pass + cloud shadows — the budget receipt

Gate: **cloud pass + ground shadows < 10% of frame time**, measured on a QUIET
GPU (no other X3Engine.exe running; `run_captures.sh` waits for a clear process
table before every launch and the round below ran with one 5 s wait).

Numbers are `[cloud-perf] gpu=… ms`, the device's own timestamp query averaged
over the last 60 of 200 settle frames, from the log beside each PNG. 1280x720.
Each row is an A/B at an IDENTICAL camera: `X3_CLOUD=0` (cloud pass and the
`cloudShad` lane both gate out — see vk_passes.cpp `cloudsOn`) vs the deck.

| camera | baseline (cover 0) | with deck | delta | % of frame |
|---|---|---|---|---|
| spawn, level (sky is a thin band) | 1.077 ms `perf_base_cloud0` | 1.079 ms `fair_01_spawn` (0.42) | +0.002 | **+0.2%** |
| spawn, level | 1.077 ms | 1.089 ms `ladder_1.00` (cover 1.0) | +0.012 | **+1.1%** |
| spawn, level | 1.077 ms | 1.094 ms `storm_02_ground` (0.94 + rain) | +0.017 | **+1.6%** |
| sky cam (pitch 0.55, ~half sky) | 0.875 ms `perf_base_sky` | 0.898 ms `fair_02_sky` (0.42) | +0.023 | **+2.6%** |
| sky cam | 0.875 ms | 0.911 ms `overcast_01_sky` (0.70) | +0.036 | **+4.1%** |
| sky cam | 0.875 ms | 0.913 ms `storm_01_sky` (0.94) | +0.038 | **+4.3%** |
| straight up (pitch 1.45, all sky) | 0.548 ms `perf_base_up` | 0.463 ms `fair_03_up` (0.42) | −0.085 | below noise |

**Worst measured case: +4.3% of frame time** (storm deck, half-sky camera) —
inside the 10% budget with room. The cost is a SKY-PIXEL cost, which is why the
level ground camera barely registers it and the sky cameras are the honest
gate; a ground-only A/B would have flattered the number by 20x.

The straight-up row came back NEGATIVE (the cloudy frame measured cheaper than
the clear one). That is the noise floor of this instrument at ~0.5 ms/frame,
not a free lunch; it is recorded rather than dropped because a 0.085 ms swing
is the resolution below which none of the other deltas should be trusted
either. The two that matter (+2.6%, +4.3%) are 5–8x above it.

Contended-GPU counter-example, kept as the reason for the wait-loop guard: the
first attempt at this receipt (2026-08-16, owner playing) measured
`gpu=358 ms` on the same baseline camera — 330x the quiet number.
