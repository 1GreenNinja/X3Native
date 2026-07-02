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

#include "engine/core/IConsole.h"       // vis-unify test hooks: IConsole
#include "engine/rhi/IRenderDevice.h"   // vis-unify test hooks: IRenderDevice
#include "engine/rhi/Visibility.h"      // vis-unify test hooks: VisPolicy

namespace x3 { namespace apphost {

struct HostContext;

int runDefaultHost(HostContext& hc);

// ---- vis-unify self-test hooks -------------------------------------------
// Defined in app_run.cpp; they bridge --test-visunify (app/self_tests.cpp) to
// the file-local default-host cvar-sync state (g_visPolicy / g_visSync) and the
// per-frame applyRtaoCVars() resolver.
void registerViewmodelCVarsForTest(x3::con::IConsole& console);
void applyRtaoCVarsForTest(x3::con::IConsole& console, x3::rhi::IRenderDevice& device);
const x3::rhi::VisPolicy& visPolicyForTest();
void resetVisSyncForTest();

}} // namespace x3::apphost
