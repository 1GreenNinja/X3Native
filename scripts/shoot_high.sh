#!/bin/sh
cd "$(dirname "$0")/.." || exit 1
build/bin/Release/X3Engine.exe --world tunnel --screenshot shots_roads/probe_high.png --shot-cam "-2500,3400,445,2.749,-1.55" > shots_roads/probe_high.log 2>&1
echo "high probe exit $?"
build/bin/Release/X3Engine.exe --world tunnel --screenshot shots_roads/probe_high2.png --shot-cam "-900,3400,-250,2.749,-1.55" > shots_roads/probe_high2.log 2>&1
echo "high2 probe exit $?"
