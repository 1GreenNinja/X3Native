#!/bin/sh
# Cloud-lane eyes-on captures (W-CLOUDS). Aborts before ANY launch if an
# X3Engine.exe is already running (the owner may be playing — house law).
# Run from the worktree root:  sh shots_clouds/run_captures.sh
set -e
cd "$(dirname "$0")/.."
EXE=./build/bin/Release/X3Engine.exe

# WAIT, don't abort. Many lanes shoot on this GPU tonight, and the previous
# shot of THIS script is itself still in the process table for a second or two
# after it writes its PNG — the first version of this guard aborted the whole
# round on its own exiting child. Poll for a clear table (up to 10 min) and
# only give up if something really is camped on the GPU.
guard() {
    i=0
    while tasklist //FI "IMAGENAME eq X3Engine.exe" | grep -q X3Engine; do
        i=$((i + 1))
        if [ "$i" -gt 120 ]; then
            echo "ABORT: an X3Engine.exe has held the GPU for 10 min — code+build only"; exit 2
        fi
        [ "$i" = 3 ] && echo "  (waiting for a clear GPU...)"
        sleep 5
    done
}

shoot() { # $1 out-name  $2 extra args  $3 log-name
    guard
    echo "=== $1 ==="
    timeout 300 "$EXE" --world tunnel --screenshot "shots_clouds/$1" $2 \
        > "shots_clouds/log_$3.txt" 2>&1 || echo "exit=$? (nonzero)"
    grep -c "\[ERROR\]" "shots_clouds/log_$3.txt" | sed 's/^/ERROR lines: /'
    grep -E "cloud-perf|shot cam=" "shots_clouds/log_$3.txt" || true
}

SKYCAM="--shot-cam -301.9,17.6,-472.2,2.749,0.55"
UPCAM="--shot-cam -301.9,17.6,-472.2,2.749,1.45"
WIDECAM="--shot-cam 400,800,-800,-0.130,-0.767"    # the whole valley: many cloud cells

# Spawn cam (A/B pair for the perf gate; identical cam, cover 0 vs 0.42)
X3_CLOUD=0    ; export X3_CLOUD ; shoot perf_base_cloud0.png ""  perf_base
X3_CLOUD=0.42 ; export X3_CLOUD ; shoot fair_01_spawn.png    ""  fair_spawn
# Sky-heavy views, fair weather - each paired with its OWN cover-0 baseline.
# The cloud pass is a SKY-PIXEL cost, so a ground-cam A/B (where the sky is a
# thin band over the ridge) measures almost none of it; the budget gate has to
# be read off a frame that is mostly sky.
X3_CLOUD=0    ; export X3_CLOUD
shoot perf_base_sky.png "$SKYCAM" perf_base_sky
shoot perf_base_up.png  "$UPCAM"  perf_base_up
X3_CLOUD=0.42 ; export X3_CLOUD
shoot fair_02_sky.png "$SKYCAM" fair_sky
shoot fair_03_up.png  "$UPCAM"  fair_up
# Cloud shadows: elevated, looking down the open valley (away from the massif)
shoot shadows_01.png  "--shot-cam -301.9,280,-472.2,-0.393,-0.72" shadows_a
shoot shadows_02.png  "--shot-cam -301.9,280,-472.2,2.749,-0.72"  shadows_b
shoot shadows_03_blob.png "--shot-cam 700,350,-700,-0.385,-0.550" shadows_blob
shoot shadows_04_wide.png "$WIDECAM"                              shadows_wide
# Broken overcast (sky + the ground under it)
X3_CLOUD=0.70 ; export X3_CLOUD
shoot overcast_01_sky.png    "$SKYCAM" overcast_sky
shoot overcast_02_ground.png ""        overcast_ground
unset X3_CLOUD

# THE COVER LADDER — the "sun dims as coverage rises" receipt. Cover 0 -> 1 at
# a fixed cam and clock: the ground must darken as the deck thickens, or the
# sky and the lighting are telling different stories.
#
# SHOT FROM THE SPAWN CAM, NOT THE WIDE ONE, and this is the whole measurement:
# at 800 m the frame is 1-3 km of DISTANT terrain, which aerial perspective has
# already blended most of the way to the haze colour — the wide ladder read
# 140.1 / 140.1 / 139.6 / 138.7 / 139.0 (a 0.8% spread) while the same build
# dimmed the near ground 31% under the storm. The fog was eating the signal,
# not the shading. Near ground, no fog, honest number.
for c in 0.00 0.25 0.50 0.75 1.00; do
    X3_CLOUD=$c ; export X3_CLOUD
    shoot "ladder_${c}.png" "" "ladder_${c}"
done
unset X3_CLOUD

# Storm deck (rain 10): near-black, sun crushed, dark ground
X3_WEATHER=storm ; export X3_WEATHER
shoot storm_01_sky.png    "$SKYCAM"  storm_sky
shoot storm_02_ground.png ""         storm_ground
shoot storm_03_wide.png   "$WIDECAM" storm_wide
unset X3_WEATHER
echo "ALL CAPTURES DONE"
