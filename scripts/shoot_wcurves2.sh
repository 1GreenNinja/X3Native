#!/bin/sh
# W-CURVES round 2: dashes restored (re-shoot barrier_mix), jersey close-up
# at the wall, hairpin from higher, junction entry re-confirm with dashes.
cd "$(dirname "$0")/.." || exit 1
EXE=build/bin/Release/X3Engine.exe
mkdir -p shots_wcurves
run() {
  name="$1"; cam="$2"
  "$EXE" --world tunnel --screenshot "shots_wcurves/$name.png" \
      --shot-cam "$cam" > "shots_wcurves/$name.log" 2>&1
  echo "$name: exit $?"
}
run barrier_mix2  "3252,23.5,240,1.67,-0.08"
run jersey_close  "3244,23.6,452,1.60,-0.14"
run hairpin_top   "-2080,190,1930,2.29,-1.25"
ls -la shots_wcurves/*.png
