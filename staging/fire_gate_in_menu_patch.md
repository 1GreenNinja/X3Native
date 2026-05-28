# Suppress weapon fire while a menu is open (clicks were both clicking the UI AND firing)

`app/main.cpp` ~line 5889 gates the LMB fire only by `!consoleOpen`:
```cpp
bool fireHeld = !consoleOpen && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
```
But when the Settings/Pause/Main menu is open the sim is frozen yet LMB still fires — so
clicking a slider/checkbox also shoots. `simFrozen` (= `gameUi.shouldFreezeSim()`, true in
MainMenu/Paused/Settings) is already computed earlier in the loop (~line 5323). Gate fire
(and the RMB-autorun) by it:
```cpp
bool fireHeld = !consoleOpen && !simFrozen && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
```
And the RMB-autorun (~5793):
```cpp
if (!consoleOpen && !simFrozen && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
    in.moveFwd += 1.0f;
```
(Also consider gating melee / use / weapon-switch-scroll by `!simFrozen` if they fire in a
menu — but LMB fire is the reported one.) APPLY AFTER the audio-settings agent's main.cpp
changes land (it's editing main.cpp now). Gate: all `--test-*` green, smoketest 0-VUID/alloc=0.
