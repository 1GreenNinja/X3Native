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
        if [ "$i" -gt 480 ]; then
            echo "ABORT: an X3Engine.exe has held the GPU for 40 min — code+build only"; exit 2
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
    # PAIRED with host_tunnel.cpp settleAndGrab: the budget gate reads the
    # [tunnel-perf] gpuFrameMs average (W-FOREST's receipt — the cloud lane's
    # duplicate [cloud-perf] line was deleted in the ce48e2b3 merge rather than
    # shipping the same number twice). A change to that log format is a change
    # to this grep.
    grep -E "tunnel-perf\] shots|shot cam=" "shots_clouds/log_$3.txt" || true
}

# ===========================================================================
# CAM CHOICE IS PART OF THE PROOF (learned the hard way, 2026-08-17).
#
# THE HORIZON RING HAS A 470 m HOLE IN IT. host_tunnel.cpp lays the far country
# as a ring with rInner = 470 m about ROUTE MID, and the terrain streamer only
# ever gets ~90 tiles (~170 m) resident inside it during a settle loop — so
# between about 170 m and 470 m from route mid there is NO GROUND, and any
# elevated camera aimed across that annulus photographs the SKY BACKGROUND and
# calls it a white plane. Two of this script's own "cloud shadow" cams did
# exactly that (old shadows_01/02 at y=280 over the corridor) and proved
# nothing. Receipts: shots_clouds/diag_E_topdown_void.png (the hole, from
# straight up, one flat colour) and the verdict in commit e698ddce.
# Not this lane's bug — but it IS this lane's job not to shoot into it.
# RULE: elevated cams stand WELL CLEAR of route mid (>= ~1 km) and look AWAY,
# or they stay low enough that the annulus is below the horizon.
#
# The other pale intruder is the river's Gerstner water patch: a finite 480 m
# square centred on the CAMERA that fades to the analytic sky at its rim
# (kWaterPatchHalf, IRenderDevice.h:1213). It follows every cam, so the wide
# shots keep a small white wedge low in frame. Also not this lane's.
# ===========================================================================
# Sky, pointed AWAY from the massif (yaw 2.749 aims straight at it, and the
# ce48e2b3 merge raised that ridge to a 289 m summit — the old SKYCAM is now
# two thirds mountain, which is no way to judge a sky).
SKYCAM="--shot-cam -301.9,17.6,-472.2,-0.393,0.30"
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
# Cloud shadows on the ground. All four stand clear of the horizon-ring hole
# (see the cam-choice block above); shadows_01 is the LOW one, because the
# owner drives at eye height and that is where a moving shade has to read.
shoot shadows_01.png  "--shot-cam 690,26,-690,-0.385,-0.10"       shadows_a
shoot shadows_02.png  "--shot-cam 1150,220,-1150,-0.385,-0.42"    shadows_b
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
#
# ONE RUN, ONE BUILD — the whole ladder or none of it. The 2026-08-17 round had
# ladder 0.00..0.75 shot at 01:19 and 1.00 re-shot at 01:44 with x3app.dll
# rebuilt at 01:33 in between. Re-run the loop; never patch one rung.
#
# AND READ THE RESULT HONESTLY. That ladder came back flat-flat-flat-flat-then
# -34%, and the first reading of it here ("two builds in a trenchcoat") was
# WRONG — verify_new_field.py had already written down the real reason and it
# is not a defect: A GROUND CAMERA STANDS UNDER ONE DECK CELL. 100 m of visible
# ground projects to 100 m on a deck whose features are ~1.8 km across, so this
# cam samples a single cumulus-sized patch of sky. That patch is a HOLE at
# cover 0.00..0.75 (identical luma to the last decimal — the ground really is
# unshaded in all four) and closes at 1.00. A step is the CORRECT output of a
# one-cell sample; a smooth curve here would mean the deck had no structure.
# The monotonic curve lives in two other places, and those are the receipts:
#   * verify_new_field.py's landscape average of cloudShadowFactor over 8 km
#     (1.000 / 0.934 / 0.763 / 0.470 / 0.219 direct sun kept) — the shader's
#     own function, averaged over many cells instead of standing in one;
#   * the wide/elevated shadow shots, where dozens of cells are in frame at
#     once and the dapple is visible as dapple.
# So: this ladder proves the deck HAS cells and that cover 1.0 closes them. It
# is not, and cannot be, the dimming curve.
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
