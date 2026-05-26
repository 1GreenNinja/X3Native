# Task #23 — crouch (C) + crawl (Ctrl); Space already jumps

`app/player.cpp`: `kEyeHeight=1.6` (const) is used in `camera()`, `damageTargetPos()`
(line 102), and `setFeetPosition()` (line 252). Make eye height + a move-speed scalar
stance-driven; poll C / Left-Ctrl in main.cpp.

## app/player.h — add stance API
```cpp
enum class Stance : uint32_t { Stand = 0, Crouch = 1, Prone = 2 };
void   setStance(Stance s);
Stance stance() const { return m_stance; }
// members:
Stance m_stance    = Stance::Stand;
float  m_eyeHeight = 1.6f;   // == kEyeHeight default (stand)
```

## app/player.cpp
```cpp
constexpr float kCrouchEye = 0.95f;   // ducked
constexpr float kProneEye  = 0.45f;   // crawling/prone

void Player::setStance(Stance s) {
    m_stance    = s;
    m_eyeHeight = (s == Stance::Prone)  ? kProneEye :
                  (s == Stance::Crouch) ? kCrouchEye : 1.6f;   // 1.6 = kEyeHeight
}
// move-speed multiplier used in update()'s planar move:
//   float sm = (m_stance==Stance::Prone)?0.28f : (m_stance==Stance::Crouch)?0.5f : 1.0f;
//   moveSpeed *= sm;
```
Replace the `kEyeHeight` reads in `camera()`, `damageTargetPos()` (line 102), and
`setFeetPosition()` (line 252) with `m_eyeHeight`. (Leave the `kEyeHeight` constant for
the Stand default.) In `update()`, multiply the planar movement speed by the stance `sm`.

## app/main.cpp — input (near the WASD/jump polling; HOLD to crouch/crawl)
```cpp
if (!consoleOpen && !termMode && player.isAlive()) {
    const bool kCtrl = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;
    const bool kC    = glfwGetKey(window, GLFW_KEY_C)            == GLFW_PRESS;
    player.setStance(kCtrl ? x3::game::Player::Stance::Prone
                   : kC    ? x3::game::Player::Stance::Crouch
                           : x3::game::Player::Stance::Stand);
}
```
Update the controls log string (`"WASD walk, ... Space jump"`) to add "C crouch, Ctrl crawl".

## STRETCH (do later): shrink the physics capsule when crouched so you fit under low
gaps / vents. Needs a capsule-resize on the Jolt character (recreate or set half-height);
v1 above only lowers the camera + slows movement (visually ducks, no clearance change).
Gate: keep all `--test-*` green; `--smoketest` 0 VUID + allocationCount=0.
```
