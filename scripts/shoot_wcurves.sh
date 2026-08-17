#!/bin/sh
# W-CURVES capture set: horizontal-flow A/B, jersey walls, hairpin survival,
# junction entry (headless, --shot-cam "x,y,z,yaw,pitch"; yaw = atan2(dz,dx)).
# Camera anchors from X3_ROADNET_DUMP (roadnet_test7.log, integration/complete
# + W-CURVES): ring worst-corner nodes 473-475, ring mixed-barrier node 9,
# ring jersey run nodes 46-49, circuit hairpin nodes 75-111, circuit landing.
cd "$(dirname "$0")/.." || exit 1
EXE=build/bin/Release/X3Engine.exe
mkdir -p shots_wcurves
run() {
  name="$1"; cam="$2"; env1="$3"
  if [ -n "$env1" ]; then
    env "$env1" "$EXE" --world tunnel --screenshot "shots_wcurves/$name.png" \
        --shot-cam "$cam" > "shots_wcurves/$name.log" 2>&1
  else
    "$EXE" --world tunnel --screenshot "shots_wcurves/$name.png" \
        --shot-cam "$cam" > "shots_wcurves/$name.log" 2>&1
  fi
  echo "$name: exit $?"
}
# 1) the formerly-kinked curve (ring fillet corner, grazing driver-ish view)
run curve_before "-1453,24.5,-4304,-0.30,-0.05" "X3_NO_HCURVE=1"
run curve_after  "-1453,24.5,-4304,-0.30,-0.05" ""
# 1b) same corner from above, where the polyline facets read as geometry
run curve_top_before "-1306,140,-4352,-0.30,-1.15" "X3_NO_HCURVE=1"
run curve_top_after  "-1306,140,-4352,-0.30,-1.15" ""
# 2) jersey wall run guarding the ditch (ring nodes 46-49)
run jersey_run   "3030,28.5,1290,2.40,-0.10" ""
# 2b) both barrier types in one frame (node 9: W-beam left, jersey right)
run barrier_mix  "3252,23.5,240,1.67,-0.08" ""
# 3) the circuit hairpin, elevated oblique — the radius floor kept it
run hairpin      "-1900,95,1700,2.29,-0.28" ""
# 4) entering the circuit junction from the access road, driving height
run junction_entry "-2133,11.5,543,0.925,-0.03" ""
ls -la shots_wcurves/*.png
