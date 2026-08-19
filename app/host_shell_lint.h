#pragma once
// ============================================================================
// HOST-SHELL LINT — the gate that stops a world from growing its own console.
//
// WHY THIS EXISTS. app/world_hosts/host_shell.h explains that the engine had a
// console, a pause menu and an FPS overlay for a long time and that 28 of ~31
// world hosts wired none of them. That got fixed host by host. Echo Harbor did
// not get fixed: it kept a FULL bespoke implementation — its own IConsole, its
// own Hud, its own ` toggle, its own hand-drawn ESC panel — and because it
// never called registerEngineConsole(), it shipped for MONTHS with none of the
// ~118 shared commands. `noclip` said "unknown" in the second product.
//
// Nothing caught that. It surfaced in a manual audit, 2026-08-18. A convention
// that only a human sweep can enforce is not a convention, it is a hope — and
// this repo has shipped three "guards that don't guard" in one week, so the
// bar here is a probe with a PROVEN failure mode, not a checklist.
//
// WHAT IT CHECKS. Over the real app/world_hosts/host_*.cpp sources:
//   R1  every file that defines a `int hostX(HostContext&)` world entry point
//       references HostShell — i.e. it wires the shared console/menu/FPS —
//       unless it is on the reasoned exemption list.
//   R2  no world host builds its own console FRONT-END. drawConsole() and
//       toggleConsole() are Hud entry points that HostShell owns exclusively;
//       a host calling either on a Hud OF ITS OWN has, by definition, its own
//       console surface. Reaching them through HostShell::hudForCallbacks() is
//       the one sanctioned route — whatever that returns IS the shared console,
//       so a pause-menu "console" row driving it is fine (and HS8 keeps that
//       carve-out from widening). Checked on comment-stripped source, so "the
//       old hud.drawConsole call is gone" in a comment is not a violation.
//
// WHAT IT CANNOT CHECK, honestly stated: a host could still hand-draw a pause
// panel out of raw drawHudQuad calls without touching the console API, and no
// text probe distinguishes that from any other HUD. R1 is the load-bearing
// rule — a host on the shared shell HAS the shared menu — and R2 catches the
// specific surface that carries the ~118 commands, which is the thing that
// actually drifted.
//
// THE NEGATIVE CONTROL. inspectWorldHostSource() is a pure function of source
// TEXT, so the gate can be run against the pre-migration Echo Harbor without
// needing its file back: kPreMigrationEchoHarbor below is a verbatim excerpt
// of what this host looked like on 2026-08-18, and the suite FAILS if that
// excerpt does not trip both rules. There is a positive control too (the
// migrated shape must come back clean) so the probe cannot pass by flagging
// everything. Both controls run even when the source tree is absent.
// ============================================================================
#include <string>
#include <vector>

namespace x3::game {

// The verdict for ONE world-host translation unit, from its source text alone.
struct HostShellVerdict {
    bool isWorldHost = false;   // defines `int hostX(HostContext&)`
    bool wiresShell  = false;   // references HostShell
    // Console-FRONT-END construction found in a world host (R2). Each string is
    // the marker, so a failure names the exact call that has to go.
    std::vector<std::string> bespokeConsole;
};

// Pure: no filesystem, no globals. `src` is a whole .cpp file.
HostShellVerdict inspectWorldHostSource(const std::string& src);

// The gate. Sweeps app/world_hosts/host_*.cpp when the source tree is reachable
// (skip-as-pass with a loud warn when it is not — the repo convention, see
// --test-sealevel S7), and ALWAYS runs the negative + positive controls.
// Logs [hostshell] PASS/FAIL lines; returns true when every check passed.
bool runHostShellLint();

} // namespace x3::game
