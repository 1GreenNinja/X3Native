#pragma once
// ============================================================================
// app_run — the DEFAULT HOST: the shared interactive render loop + the cell/
// terrain/ocean/canon/fromdoc world build, audio/physics/loading bring-up, the
// intro cold-open, the headless smoketest/screenshot/bench paths, and the
// boot-time report. This is the deeply-coupled core of the old main() body
// (hundreds of locals in one scope). Lifted VERBATIM out of main() (#28 deep
// split, Phase C) into app/app_run.cpp behind runDefaultHost(HostContext&).
//
// main() reaches here ONLY when neither dispatchScreenshotHosts nor
// dispatchWorldHost matched (both returned -1). runDefaultHost returns the
// program exit code (bootTestExit).
// ============================================================================

namespace x3 { namespace apphost {

struct HostContext;

int runDefaultHost(HostContext& hc);

}} // namespace x3::apphost
