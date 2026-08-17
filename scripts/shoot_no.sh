#!/bin/sh
cd "$(dirname "$0")/.." || exit 1
X3_OUTER_RING=0 build/bin/Release/X3Engine.exe --world tunnel --screenshot shots_roads/probe_mid_top_noouter.png --shot-cam "-2500,400,445,2.749,-1.45" > shots_roads/probe_noouter.log 2>&1
echo "no-outer probe exit $?"
X3_RIVER_ROAD=0 build/bin/Release/X3Engine.exe --world tunnel --screenshot shots_roads/probe_mid_top_noriver.png --shot-cam "-2500,400,445,2.749,-1.45" > shots_roads/probe_noriver.log 2>&1
echo "no-river probe exit $?"
