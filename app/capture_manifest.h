#pragma once
// ============================================================================
// capture_manifest — WHAT THIS CAPTURE DID NOT RENDER.
//
// WHY THIS EXISTS. This project verifies art almost entirely through
// `--screenshot*` frames, and a frame is judged as if it were the game. It is
// not. The headless capture paths run a SETTLE LOOP, not the live loop, and a
// settle loop only ticks what somebody remembered to tick in it. The
// WorldStreamer is the proven case: it is updated ONLY in the live loop
// (host_echotropolis.cpp, live `regionStreamer.update(...)`), and
// EchoRegionSet::drawAll/updateAll gate every region on the streamer's
// residency view (echo_regions.cpp, M-B DRAW GATE). In a capture the streamer
// has never run, so every streamed region reports Unloaded and submits
// NOTHING: the harbour fleet, the traffic, the subway, the drones and the
// district dressing are simply absent. Weeks of "vessels never cross land"
// were photographs of an empty bay while a ship sat beached on Tim's monitor.
//
// The bug was never the gate. The bug was that the frame said nothing about
// it. So this file's whole job is to make a capture ANNOUNCE what it left out,
// in the same shape as the two precedents that already work in this codebase:
//   [rhi] VALIDATION: layers=OFF ... a '0 VUID' result from this run is MEANINGLESS
//   [ERROR] [cvar] !!! THIS RUN DID NOT TEST THOSE.
//
// HOW IT AVOIDS GOING STALE (the same failure mode it exists to fix). There is
// no hardcoded list of subsystems anywhere in here. A subsystem appears in the
// manifest because IT declared itself, from its own construction site, in THIS
// binary. Consequences:
//   * a subsystem this build does not construct never declares, and is
//     correctly absent from the report — so an engine-EXE capture and an
//     editor-EXE capture over a shared host DLL each print THEIR OWN truth
//     without either one being taught about the other;
//   * "declared but never ticked" is detected by construction, which is
//     exactly the streamer's shape, and is what any future live-loop-only
//     subsystem will look like the day it is added;
//   * gate() reports COUNTS from inside the gate itself, so a partially-gated
//     subsystem reports PARTIAL rather than lying in either direction.
// The one thing the manifest cannot know is a subsystem that never calls
// declare() at all, so report() says so out loud instead of implying coverage.
//
// COST WHEN NOT CAPTURING: arm() is only called for a run whose command line
// names a capture flag. Un-armed, every entry point below is an early-out on
// one relaxed atomic load, and nothing is printed.
// ============================================================================
#include <string>

namespace x3 { namespace capture {

// ---- arming (called once from parseCli; see cli.cpp) ----------------------
// `runFlag` is the capture flag verbatim ("--screenshot", "--screenshot-car",
// "--capture-walk", ...). Arming is what turns every call below from a no-op
// into a recorded observation.
void arm(const std::string& runFlag);
bool armed();
// Optional context for the banner. Safe to call before or after arm().
void setWorld(const std::string& world);
void setOutput(const std::string& outPath);

// ---- declaration (called from the CONSTRUCTION site of a subsystem) -------
// `name`        short, greppable id ("worldstream", "regions.streamed.draw").
// `consequence` what is MISSING FROM THE FRAME when this never runs. Written
//               for the person reading the still, not for the person reading
//               the code: name the content, not the class.
// Re-declaring the same name is harmless (last consequence wins).
void declare(const char* name, const char* consequence);

// ---- observation (called from the site that actually runs / gates) --------
// tick(): this subsystem ran. gate(): a pass/block decision, by count.
void tick(const char* name);
void gate(const char* name, unsigned passed, unsigned blocked);

// ---- the loud part --------------------------------------------------------
// Prints the manifest exactly once per process. Called explicitly by the
// capture dispatch so it lands next to the "wrote PNG" line, and again from a
// static destructor for the capture paths that exit somewhere else — so a
// capture rig added later is covered with NO edit here.
void report();

}} // namespace x3::capture
