#!/usr/bin/env bash
# UNIFY 0820 sandbox gate. Usage: unify_gate.sh <exe> <outdir>
# Every run is bounded. Never uses --test-destinations / --test-enemy
# (not real flags: they fall through to a WINDOWED boot and hang).
EXE="$1"; OUT="$2"; mkdir -p "$OUT"
run() {  # run <name> <timeout> <args...>
  local name="$1"; shift; local t="$1"; shift
  timeout -k 5 "$t" "$EXE" "$@" > "$OUT/$name.log" 2>&1
  local rc=$?
  echo "$name rc=$rc"
}
echo "=== EXE: $EXE"
# --- suites -----------------------------------------------------------------
for t in ui weapons grounding canonplay level1 canonlevel rifthub engineconsole \
         space csm doors levellint propclip gibs cutscene audio jukebox \
         ai phase2b factory canonmission; do
  run "test-$t" 240 "--test-$t"
done
# jukebox + audio muted AND unmuted
X3_AUDIO_MUTE=1 timeout -k 5 240 "$EXE" --test-audio   > "$OUT/test-audio-muted.log"   2>&1; echo "test-audio-muted rc=$?"
X3_AUDIO_MUTE=1 timeout -k 5 240 "$EXE" --test-jukebox > "$OUT/test-jukebox-muted.log" 2>&1; echo "test-jukebox-muted rc=$?"
# --- smoketests -------------------------------------------------------------
run smoke-default    300 --smoketest
run smoke-canonlevel 300 --smoketest --world canonlevel
run smoke-space      300 --smoketest --world space
run smoke-surface    300 --smoketest --world surface
run smoke-echoharbor 300 --smoketest --world echoharbor
run smoke-echotropolis 300 --smoketest --world echotropolis
run smoke-factory    300 --smoketest --world factory
# --vksync needs --smoketest or it opens a window
run vksync-space     300 --smoketest --vksync --world space
