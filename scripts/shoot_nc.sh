#!/bin/sh
cd "$(dirname "$0")/.." || exit 1
X3_CONNECTOR=0 build/bin/Release/X3Engine.exe --world tunnel --screenshot shots_roads/probe_mid_top_nc.png --shot-cam "-2500,400,445,2.749,-1.45" > shots_roads/probe_nc.log 2>&1
echo "nc probe exit $?"
