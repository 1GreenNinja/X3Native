#!/bin/sh
# W-FACTORY proof set — THE GLIMVALE WORKS (headless, --shot-cam overrides).
#
# Cameras are DERIVED, not eyeballed (gotcha 4.1: "derive cameras from room
# data, never eyeball coordinates"). The site is measured at boot and logged;
# these numbers come from that log:
#   site  (838, 4481)  padY 15.06   tour node 130 (744, 4165)
#   f = (0.2848, 0.9576)  (the outward radial, freeway -> works)
#   r = (-0.9576, 0.2848)
#   gate = site + f * -60 = (820.9, 4423.5)
#   stack #1 = site + f*64 + r*(-34) = (888.8, 4532.6), top at padY + 61.1
# --shot-cam is "x,y,z,yaw,pitch"; camera dir = (cos yaw, 0, sin yaw), so the
# yaw that looks along f is atan2(0.9576, 0.2848) = 1.2818.
cd "$(dirname "$0")/.." || exit 1
EXE=build/bin/Release/X3Engine.exe
OUT=shots_factory
mkdir -p "$OUT"
run() {
  name="$1"; cam="$2"
  "$EXE" --world tunnel --screenshot "$OUT/$name.png" --shot-cam "$cam" > "$OUT/$name.log" 2>&1
  echo "$name: exit $? | $(grep -o 'gpu [0-9.]* ms avg (settled 60f) = [0-9]* fps' "$OUT/$name.log" | tail -1)"
}

# 1) THE SKYLINE FROM THE FREEWAY — the whole reason the site was chosen.
#    ON THE PAVEMENT, not backed off into the country behind it: the first cut
#    stood 220 m back at (681, 3954), which is INSIDE the centre-north wood,
#    and photographed a tree trunk. The corridor footprint is a forest keep-out
#    (forest.cpp), so the road is exactly where the sightline is.
run 01_skyline_from_freeway "727,23,4108,1.2818,0.055"
# 2) THE GATE + THE SIGNAGE, from the drive, shut.
run 02_gate_shut           "810,17.3,4387,1.2818,0.09"
# 3) THE SMOKESTACKS, plume in frame (200 settle frames = 3.3 s of stack).
run 03_stack_smoke         "809,48,4264,1.2818,0.10"
# 4) A GOLDEN TICKET GLINTING — the works-gate card, close.
run 04_ticket_glint        "807,12.2,4418,0.6435,-0.09"
# 5) THE HUD COUNT after a pickup (X3_TICKETS seeds the count; see host wiring).
X3_TICKETS=3 "$EXE" --world tunnel --screenshot "$OUT/05_hud_count.png" \
  --shot-cam "810,17.3,4387,1.2818,0.09" > "$OUT/05_hud_count.log" 2>&1
echo "05_hud_count: exit $?"
# 6) ALL FIVE FOUND — the gate open (the slide finishes inside the settle).
X3_TICKETS=5 "$EXE" --world tunnel --screenshot "$OUT/06_gate_open.png" \
  --shot-cam "810,17.3,4387,1.2818,0.09" > "$OUT/06_gate_open.log" 2>&1
echo "06_gate_open: exit $?"
ls -la "$OUT"/*.png
