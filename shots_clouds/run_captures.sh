#!/bin/sh
# Cloud-lane eyes-on captures (W-CLOUDS v2). Aborts before ANY launch if an
# X3Engine.exe is already running (the owner may be playing — house law).
# Run from the worktree root:  sh shots_clouds/run_captures.sh
set -e
cd "$(dirname "$0")/.."
EXE=./build/bin/Release/X3Engine.exe

guard() {
    if tasklist //FI "IMAGENAME eq X3Engine.exe" | grep -q X3Engine; then
        echo "ABORT: foreign X3Engine.exe running — code+build only"; exit 2
    fi
}

shoot() { # $1 out-name  $2 log-name  $3.. env/extra args via prefix
    guard
    echo "=== $1 ==="
    timeout 300 "$EXE" --world tunnel --screenshot "shots_clouds/$1" $2 \
        > "shots_clouds/log_$3.txt" 2>&1 || echo "exit=$? (nonzero)"
    grep -c "\[ERROR\]" "shots_clouds/log_$3.txt" | sed 's/^/ERROR lines: /'
    grep -E "cloud-perf|shot cam=" "shots_clouds/log_$3.txt" || true
}

# Spawn cam (A/B pair for the perf gate; identical cam, cover 0 vs 0.42)
X3_CLOUD=0    ; export X3_CLOUD ; shoot perf_base_cloud0.png ""  perf_base
X3_CLOUD=0.42 ; export X3_CLOUD ; shoot fair_01_spawn.png    ""  fair_spawn
# Sky-heavy views, fair weather
shoot fair_02_sky.png "--shot-cam -301.9,17.6,-472.2,2.749,0.55" fair_sky
shoot fair_03_up.png  "--shot-cam -301.9,17.6,-472.2,2.749,1.45" fair_up
# Cloud shadows: elevated, looking down the open valley (away from the massif)
shoot shadows_01.png  "--shot-cam -301.9,280,-472.2,-0.393,-0.72" shadows_a
shoot shadows_02.png  "--shot-cam -301.9,280,-472.2,2.749,-0.72"  shadows_b
# Broken overcast
X3_CLOUD=0.70 ; export X3_CLOUD ; shoot overcast_01_sky.png "--shot-cam -301.9,17.6,-472.2,2.749,0.55" overcast_sky
unset X3_CLOUD
# Storm deck (rain 10): near-black, sun crushed
X3_WEATHER=storm ; export X3_WEATHER
shoot storm_01_sky.png    "--shot-cam -301.9,17.6,-472.2,2.749,0.55" storm_sky
shoot storm_02_ground.png ""                                          storm_ground
unset X3_WEATHER
echo "ALL CAPTURES DONE"
