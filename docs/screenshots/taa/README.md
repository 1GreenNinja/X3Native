# TAA before/after proof (STRIKE 2 — temporal anti-aliasing)

All captures from the SAME `feat/taa` Release build. "off" = `--notaa`
(jitter fully disabled + resolve skipped), "on" = default (`r_taa 1`,
`r_taasharpen 0.25`). Headless captures are bit-reproducible (jitter phase
is a pure frame counter; settle counts exceed the 8-frame Halton cycle so
the captured frame is converged).

| file | what it shows |
|---|---|
| `checker_horizon_on/off.png` | NEAREST-zoomed distant checkerboard (the worst-case high-frequency surface). OFF: hard pixel crawl. ON: resolves to a clean filtered gradient — the headline TAA win. |
| `monster_edge_on/off.png` | NEAREST zoom on moving monsters + pillar edge mid-fight (`--capture-ai` frame 15). Edges anti-aliased; the fast drone shows slight softening under camera-only reprojection (clamp contains it; no trails). |
| `motion_frame15_on/off.png` | the full motion frame the zooms come from |
| `level1_taa_on/off.png` | Level-1 spawn still (1x, no SSAA) |
| `showroom_taa_on/off.png` | Showroom hero still (4x SSAA path, TAA stacked on top) |
| `level1_legacypost_taabuild.png` | `--legacypost` from this build — md5-IDENTICAL to the `--legacypost` capture from the base `feat/post-stack` build (bit-exact legacy guarantee preserved) |

A/B verdicts (md5, Level-1 still):
- base(feat/post-stack) default == feat/taa `--notaa` -> **identical** (`9e0b3976...`)
- base `--legacypost` == feat/taa `--legacypost` -> **identical** (`3d5c9dc3...`)
- feat/taa default run twice -> **identical** (`fd0cce05...`, deterministic)

Honest ghosting note: reprojection is CAMERA-ONLY; skinned/fast movers rely
on the 3x3 YCoCg neighborhood clamp (slight softening/shimmer instead of
trails). A per-object velocity buffer is the documented next tier.
