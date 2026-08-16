#!/bin/sh
# W-ROADS junction/summit capture set (headless, --shot-cam overrides).
cd "$(dirname "$0")/.." || exit 1
EXE=build/bin/Release/X3Engine.exe
mkdir -p shots_roads
run() {
  name="$1"; cam="$2"
  "$EXE" --world tunnel --screenshot "shots_roads/$name.png" --shot-cam "$cam" > "shots_roads/$name.log" 2>&1
  echo "$name: exit $?"
}
run jct_ring_wide     "-4015.6,64,1082.5,2.749,-0.32"
run jct_ring_low      "-4084.9,23,1111.2,2.749,-0.10"
run jct_ring_side     "-4170.5,37,1049.3,1.174,-0.18"
run jct_spur_mouth    "-4284.9,30,712.1,1.664,-0.12"
run summit            "-6497,88,1438,-0.283,-0.25"
run summit_approach   "-6324,48,1388,2.86,0.05"
run exit_to_connector "-809.5,45,-262.6,2.749,-0.22"
run connector_scurve  "-2265,160,349,2.749,-0.55"
ls -la shots_roads/*.png
