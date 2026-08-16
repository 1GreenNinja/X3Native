#!/bin/sh
cd "$(dirname "$0")/.." || exit 1
EXE=build/bin/Release/X3Engine.exe
mkdir -p shots_roads
run() {
  name="$1"; cam="$2"
  "$EXE" --world tunnel --screenshot "shots_roads/$name.png" --shot-cam "$cam" > "shots_roads/$name.log" 2>&1
  echo "$name: exit $?"
}
run probe_mid_side "-2404,60,676,-1.965,-0.15"
run probe_mid_top  "-2500,400,445,2.749,-1.45"
ls -la shots_roads/probe_*.png
