#!/bin/sh
# W-CURVES round 3+: the valley road's ring landings (formerly stacked decks).
cd "$(dirname "$0")/.." || exit 1
EXE=build/bin/Release/X3Engine.exe
mkdir -p shots_wcurves
run() {
  name="$1"; cam="$2"
  "$EXE" --world tunnel --screenshot "shots_wcurves/$name.png" \
      --shot-cam "$cam" > "shots_wcurves/$name.log" 2>&1
  echo "$name: exit $?"
}
# west landing (108, -3847): approach from the valley side, driving height
run valley_west_landing "120,27,-3700,-1.66,-0.05"
# west landing from the RING driver's seat (the stacked-deck site, rebuilt)
run valley_west_from_ring "10,24,-3860,0.05,-0.04"
# east landing (3199, -1041) from the valley approach
run valley_east_landing "3077,28,-996,-0.36,-0.05"
ls -la shots_wcurves/valley_*.png
