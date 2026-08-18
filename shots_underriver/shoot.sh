#!/bin/sh
# W-UNDERRIVER capture set. Every camera is DERIVED from the measured chain
# printed by --test-underriver (node xz + water Y), not eyeballed.
# NOTE the leading space inside each --shot-cam quote: a leading '-' is parsed
# as a flag and the shot silently becomes the default hero camera
# (ENGINE_GOTCHAS 4.1).
set -e
B=build/bin/Release/X3Engine.exe
O=shots_underriver

shot() { # name cam
  echo "=== $1"
  timeout 300 "$B" --world tunnel --screenshot "$O/$1.png" --shot-cam "$2" 2>&1 \
    | grep -iE "\[ERROR\]|under-river|frame .*ms|screenshot" | head -12
}

# A — JOB 1 regression: under the dry basin rim, looking UP. Must be clean sky,
#     no teal underside of a sea sheet hanging over dry land.
shot 10_underterrain_lookup_POSTMERGE "1650,-30,-1900,0.0,1.2"

# B — THE GREAT HALL from a rock beach. Node 6 (-990,40) w=-2.25, roof 38.9 m
#     up. Beach 15 m off the spine, eye 1.65 m over it, looking downstream.
shot 11_greathall_from_beach " -975.4,-0.15,36.4,-2.062,0.10"

# C — RUSHING WATER close up. Node 3 (-1135,540) w=-0.15, rush 1.00.
shot 12_rushing_water " -1121.0,1.5,541.0,-2.011,-0.20"

# D — THE CAVERN WIDE, high in the void over the Great Hall pool.
shot 13_cavern_wide " -985.0,12.0,45.0,-1.816,-0.10"

# E — THE GORGE. The last 120 m runs open to the sky; the plunge pool at
#     (-1090,-700) w=-5.32, seen from the rim. The rim camera is 60 m east of
#     the portal node — past kURWallOutW, so it stands on UNCARVED country at
#     ~21 m (scan) rather than inside the hillside, which is where the first
#     draft of this camera was.
shot 14_gorge_portal " -1030.0,22.7,-700.0,3.1416,-0.436"

# F — FROM ABOVE. The lid must read as country, not as a 1.8 km scar.
shot 15_vault_from_above " -1040.0,260.0,480.0,-1.586,-0.55"

echo "=== all shots done"
ls -la "$O"/*.png
