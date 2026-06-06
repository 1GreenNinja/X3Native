// Level Architect — Phase 5 keybind TABLE (Doom-Builder Visual Mode nudge editing).
//
// The DATA-DRIVEN {action -> key} mapping. Both the host's input poll and the floating
// keybind cheat-sheet read this one table, so they can never drift, and the binds are
// rebindable at runtime (rebind()) — see editor.h / editor_host.cpp.
//
// CLASSIC default key map (Doom-Builder-flavored):
//   Mouse-wheel UP / DOWN .. raise / lower the looked-at surface HEIGHT (the brush TOP)
//   PgUp / PgDn ............. raise / lower the brush FLOOR (the brush bottom)
//   Up arrow / Down arrow ... move the brush OUT / IN along the faced axis (push walls)
//   ] / [ ................... stretch / shrink the brush extent on the faced axis
//   H ...................... toggle the floating keybind cheat-sheet overlay
//   (Shift = larger step, Ctrl = finer step — applied host-side, not a bind.)
//
// GLFW key codes live in <GLFW/glfw3.h>; the two synthetic mouse-wheel codes are in
// editor.h (kKeyMouseWheelUp/Down).
#include "editor.h"

#include <GLFW/glfw3.h>

namespace x3::editor {

void KeybindTable::resetDefaults() {
    m_binds[(int)NudgeAction::RaiseHeight]   = { NudgeAction::RaiseHeight,   kKeyMouseWheelUp };
    m_binds[(int)NudgeAction::LowerHeight]   = { NudgeAction::LowerHeight,   kKeyMouseWheelDown };
    m_binds[(int)NudgeAction::RaiseFloor]    = { NudgeAction::RaiseFloor,    GLFW_KEY_PAGE_UP };
    m_binds[(int)NudgeAction::LowerFloor]    = { NudgeAction::LowerFloor,    GLFW_KEY_PAGE_DOWN };
    m_binds[(int)NudgeAction::MoveOut]       = { NudgeAction::MoveOut,       GLFW_KEY_UP };
    m_binds[(int)NudgeAction::MoveIn]        = { NudgeAction::MoveIn,        GLFW_KEY_DOWN };
    m_binds[(int)NudgeAction::StretchGrow]   = { NudgeAction::StretchGrow,   GLFW_KEY_RIGHT_BRACKET };
    m_binds[(int)NudgeAction::StretchShrink] = { NudgeAction::StretchShrink, GLFW_KEY_LEFT_BRACKET };
    m_binds[(int)NudgeAction::ToggleTooltip] = { NudgeAction::ToggleTooltip, GLFW_KEY_H };
}

int KeybindTable::keyFor(NudgeAction action) const {
    if ((int)action < 0 || action >= NudgeAction::Count) return 0;
    return m_binds[(int)action].key;
}

NudgeAction KeybindTable::actionForKey(int key) const {
    if (key == 0) return NudgeAction::Count;
    for (int i = 0; i < (int)NudgeAction::Count; ++i)
        if (m_binds[i].key == key) return m_binds[i].action;
    return NudgeAction::Count;
}

bool KeybindTable::rebind(NudgeAction action, int key) {
    if ((int)action < 0 || action >= NudgeAction::Count || key == 0) return false;
    // No two actions may share a key: if `key` is already bound elsewhere, clear it.
    for (int i = 0; i < (int)NudgeAction::Count; ++i)
        if (m_binds[i].key == key && m_binds[i].action != action) m_binds[i].key = 0;
    m_binds[(int)action].key = key;
    return true;
}

const char* KeybindTable::actionLabel(NudgeAction a) {
    switch (a) {
        case NudgeAction::MoveOut:       return "Move Out";
        case NudgeAction::MoveIn:        return "Move In";
        case NudgeAction::StretchGrow:   return "Stretch +";
        case NudgeAction::StretchShrink: return "Shrink -";
        case NudgeAction::RaiseHeight:   return "Raise Ceiling";
        case NudgeAction::LowerHeight:   return "Lower Ceiling";
        case NudgeAction::RaiseFloor:    return "Raise Floor";
        case NudgeAction::LowerFloor:    return "Lower Floor";
        case NudgeAction::ToggleTooltip: return "Toggle Cheat-Sheet";
        default:                         return "?";
    }
}

const char* KeybindTable::keyName(int key) {
    switch (key) {
        case 0:                       return "(unbound)";
        case kKeyMouseWheelUp:        return "Wheel Up";
        case kKeyMouseWheelDown:      return "Wheel Dn";
        case GLFW_KEY_PAGE_UP:        return "PgUp";
        case GLFW_KEY_PAGE_DOWN:      return "PgDn";
        case GLFW_KEY_UP:             return "Up";
        case GLFW_KEY_DOWN:           return "Down";
        case GLFW_KEY_LEFT:           return "Left";
        case GLFW_KEY_RIGHT:          return "Right";
        case GLFW_KEY_LEFT_BRACKET:   return "[";
        case GLFW_KEY_RIGHT_BRACKET:  return "]";
        case GLFW_KEY_COMMA:          return ",";
        case GLFW_KEY_PERIOD:         return ".";
        case GLFW_KEY_MINUS:          return "-";
        case GLFW_KEY_EQUAL:          return "=";
        case GLFW_KEY_HOME:           return "Home";
        case GLFW_KEY_END:            return "End";
        case GLFW_KEY_INSERT:         return "Ins";
        case GLFW_KEY_DELETE:         return "Del";
        default: break;
    }
    // A-Z and 0-9 map straight to their ASCII glyph in GLFW.
    static char buf[2] = { 0, 0 };
    if ((key >= GLFW_KEY_A && key <= GLFW_KEY_Z) ||
        (key >= GLFW_KEY_0 && key <= GLFW_KEY_9)) {
        buf[0] = (char)key; return buf;
    }
    return "?";
}

} // namespace x3::editor
