# unified4 fold — proof screenshots

integration/unified4 = integration/unified3 + `git merge --no-ff feat/cave-atmosphere`
(which brings feat/descent-fall stacked underneath). All captured in-engine, headless,
via `--world club --screenshot <path> [settle] --shot-cam x,y,z,yaw,pitch` on the RTX 5090.

Gamma is applied ONCE by the `VK_FORMAT_B8G8R8A8_SRGB` swapchain (no shader OETF);
`--test-gamma` verifies linear 0.5 -> byte 188. Per-area histogram (sampled): no shot
clips a large region to 255 (club max 0.19% — thin laser cores only), and the
deliberately-dark caves / dark room are NOT black-crushed (0% pure-black).

| shot | what | shot-cam | mean L / clip255 / crush0 |
|------|------|----------|---------------------------|
| `1_club_at_-800.png` | Club 1127 THE DEEP at **Y=-800** — "1127" sign, volumetric moving-head beams, laser-spirograph floor, disco ball, blacklit U-bar, Danny + NPCs | showcase (default) | 48.7 / 0.19% / 10.6% |
| `2_the_fall.png` | the vertical FALL SHAFT looking down the bore — concentric strata rings receding to the blue crystal glow far below | `38,-90,4,0,-1.25` | 70.3 / 0% / 0% |
| `3_dark_room.png` | the DARK LANDING ROOM — glowing HoloTerminal (dark-glass, colored status text, ceiling-pipe mount) + the lit keypad-door passage | `38,-786.4,2.5,3.14,-0.04` | 42.7 / 0% / 0% |
| `4_singing_cave.png` | a SINGING CAVE side-shoot — glowing blue Salvari crystal cluster, stalactites/stalagmites, dark basalt, fog haze | `45,-282.5,3.8,0,-0.03` | 71.7 / 0% / 0% |

Club floor Y=-800 confirmed by the build log (`built THE DEEP (Club 1127) at Y=-800`)
and `--test-descentfall` D6 (elevator rides to the CLUB floor Y=-800) + D7 (HUB rides
down to survival-complex L7 Y=-824 via the under-club hall, Route B).
