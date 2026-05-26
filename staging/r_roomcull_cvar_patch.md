# `r_roomcull` cvar — toggle the per-room PVS cull (so noclip can see the whole level)

The per-room PVS cull (`Scene::roomVisible` gate in `Scene::render`, added by commit
5c55cc4) correctly draws nothing when the camera is in no room (e.g. noclipped outside
the level). Add a cvar to bypass it for debugging / level overview.

APPLY AFTER the door/hall agent's scene.cpp changes land (it's editing scene.cpp now).
Verify the API names against the agent's final `app/scene.h` (it added
`setVisibleRooms/clearVisibleRooms/roomVisible/drawnCount`).

## app/scene.h — add a cull-enable toggle
```cpp
// In class Scene (near setVisibleRooms):
void setRoomCull(bool on) { m_roomCull = on; }
bool roomCull() const { return m_roomCull; }
// member:
bool m_roomCull = true;   // per-room PVS cull on by default
```

## app/scene.cpp — bypass the gate when disabled (in render())
Find the cull gate (something like `if (!roomVisible(e.roomId)) continue;`) and make it
honor the toggle:
```cpp
if (m_roomCull && e.roomId != x3::game::kRoomAny && !roomVisible(e.roomId))
    continue;   // r_roomcull 0 -> draw everything (noclip overview / debug)
```
(`kRoomAny` = whatever sentinel marks always-visible entities, if the agent used one;
else just `m_roomCull && !roomVisible(e.roomId)`.)

## app/main.cpp — register the cvar + apply it each frame
Near the other `r_*` cvars (e.g. `r_maxfps`):
```cpp
console.registerCVar("r_roomcull", "1", "per-room PVS occlusion cull (0=draw whole level, e.g. for noclip)");
```
Where the per-frame PVS update happens (the `setVisibleRooms(...)` call in the canonlevel
path), gate it:
```cpp
const bool roomCull = console.cvarInt("r_roomcull", 1) != 0;   // match the console API
scene.setRoomCull(roomCull);
if (roomCull) scene.setVisibleRooms(/* ...current PVS... */);
// when off, setRoomCull(false) makes render() draw all regardless of the visible set
```
So `~` console → `r_roomcull 0` shows the whole level (great with `noclip`); `r_roomcull 1`
restores the cull. No rebuild needed to toggle once it's in. Gate: all `--test-*` green,
smoketest 0 VUID + allocationCount=0, perf unchanged when r_roomcull 1.
```
