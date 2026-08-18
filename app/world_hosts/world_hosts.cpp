// world_hosts dispatch (#28 deep split). Matches worldMode (and the destruct
// OR-flag) in the SAME order the inline main() blocks ran; returns the program
// exit code, or -1 to fall through to main() (its remaining inline hosts / the
// default render loop). Hosts are wired in here AS they are extracted so the
// dispatch + main() stay link-clean and gate-green at every step.
//
// [P0-2] The dispatch is now TABLE-DRIVEN: kHostRoutes is the one list of
// worldMode -> host mappings, dispatchWorldHost() walks it, and
// dispatchedWorldModes() exposes the flags to the destination-registry
// self-test. Before this, the registry's kDispatchedWorlds was a hand copy of
// these `if` lines — and it silently drifted 8 worlds behind in one week
// (introcockpit / ship-windows / ship-interior / descentslide / bodycontact /
// wormhole / wormhole-transit / tractor were dispatchable but invisible to the
// menu, the consoles and the hub). One table, exported, gate-checked.
//
// [--set] The dispatch is ALSO where the CLI `--set` overrides are applied. The
// per-frame cvar->device sync hub lives in runDefaultHost, which these hosts run
// INSTEAD of — so `--world tunnel --set r_bloom 0` used to render the DEFAULT
// state, silently, and every A/B taken that way was void. Wiring that per host
// would be the same forget-me trap one layer up, so it happens HERE, once, for
// every route in kHostRoutes: a new --world is covered the moment it is
// dispatchable. See app/world_hosts/world_host_common.h.
#include "../host_context.h"
#include "../world_hosts.h"
#include "world_host_common.h"   // applyHostRenderCVars / reportUnappliedHostCVars

namespace x3 { namespace apphost {

namespace {

struct HostRoute {
    const char* flag;                // the `--world` string
    int (*host)(HostContext&);       // the host body it runs
};

// Order preserved EXACTLY from the old if-chain (which itself preserved the
// order the inline main() blocks ran in).
const HostRoute kHostRoutes[] = {
    { "destruct",          hostDestruct        },
    { "physjoint",         hostPhysJoint       },
    { "ragdoll",           hostRagdoll         },
    { "drive",             hostDrive           },
    { "boat",              hostDrive           },
    { "fly",               hostDrive           },
    { "club",              hostClub            },
    { "complex",           hostComplex         },   // gamma-fold: 7-level survival complex
    { "showroom",          hostShowroom        },
    { "valley",            hostValley          },
    { "cliffs",            hostCliffs          },
    { "streamed",          hostStreamed        },
    { "space",             hostSpace           },
    { "surface",           hostSurfaceStart    },
    { "introcockpit",      hostIntroCockpit    },
    { "ship-windows",      hostShipWindows     },
    { "descentslide",      hostDescentSlide    },
    { "bodycontact",       hostBodyContact     },
    { "wormhole",          hostWormhole        },
    { "wormhole-transit",  hostWormholeTransit },
    { "tractor",           hostTractor         },
    { "ship-interior",     hostShipWindows     },   // alias flag, same host
    { "strata",            hostStrata          },
    { "elevator-showcase", hostElevator        },
    { "rifthub",           hostRifthub         },
    // ECHO HARBOR. `echoharbor` is CANONICAL (owner 2026-08-18: "Echo Harbor
    // should Not be called echotropolis"); `echotropolis` is the PERMANENT
    // legacy alias — ~350 references across 74 files type it. Same host, same
    // deal as ship-interior above; destinations.cpp carries the reasoned
    // exclusion and D12 asserts the alias never quietly dies.
    { "echoharbor",        hostEchoHarbor      },
    { "echotropolis",      hostEchoHarbor      },   // alias flag, same host
    { "gallery",           hostGallery         },
    { "tunnel",            hostTunnel          },   // terrain-corridor bore demo
    { "mines",             hostMines           },   // mine-entrance showcase (inspx/mines)
    { "cutaway",           hostCutaway         },   // LEVEL ARCHITECT cutaway view (W-CUTAWAY)
};
constexpr unsigned kHostRouteCount =
    (unsigned)(sizeof(kHostRoutes) / sizeof(kHostRoutes[0]));

// The flags alone, for dispatchedWorldModes() (stable storage, built once).
const char* const kHostRouteFlags[kHostRouteCount] = {
    kHostRoutes[0].flag,  kHostRoutes[1].flag,  kHostRoutes[2].flag,
    kHostRoutes[3].flag,  kHostRoutes[4].flag,  kHostRoutes[5].flag,
    kHostRoutes[6].flag,  kHostRoutes[7].flag,  kHostRoutes[8].flag,
    kHostRoutes[9].flag,  kHostRoutes[10].flag, kHostRoutes[11].flag,
    kHostRoutes[12].flag, kHostRoutes[13].flag, kHostRoutes[14].flag,
    kHostRoutes[15].flag, kHostRoutes[16].flag, kHostRoutes[17].flag,
    kHostRoutes[18].flag, kHostRoutes[19].flag, kHostRoutes[20].flag,
    kHostRoutes[21].flag, kHostRoutes[22].flag, kHostRoutes[23].flag,
    kHostRoutes[24].flag, kHostRoutes[25].flag, kHostRoutes[26].flag,
    kHostRoutes[27].flag, kHostRoutes[28].flag, kHostRoutes[29].flag,
    kHostRoutes[30].flag,
};
static_assert(kHostRouteCount == 31, "update kHostRouteFlags when adding a route");

} // namespace

namespace {
// Run ONE route with the `--set` wiring around it. Every dispatch path goes
// through here, so no host can be added without it:
//   before — push the CLI cvar overrides onto the device AND arm the run-long
//            latch that keeps them there (opt-in: a run with no --set does
//            nothing at all, see world_host_common.h);
//   after  — name, at ERROR level, every --set nothing applied, so a run that
//            ignored a flag says so in its own output instead of quietly
//            producing a plausible-looking lie.
int runRoute(int (*host)(HostContext&), const char* flag, HostContext& hc) {
    const std::string who = std::string("--world ") + flag;
    if (hc.device) applyHostRenderCVars(hc, *hc.device, who);
    const int rc = host(hc);
    reportUnappliedHostCVars(hc, who);
    return rc;
}
} // namespace

int dispatchWorldHost(HostContext& hc) {
    // --screenshot-destruct forces the destruct host regardless of worldMode
    // (the old first line was `worldMode == "destruct" || hc.destructShot`).
    if (hc.destructShot) return runRoute(hostDestruct, "destruct", hc);
    for (unsigned i = 0; i < kHostRouteCount; ++i)
        if (hc.worldMode == kHostRoutes[i].flag)
            return runRoute(kHostRoutes[i].host, kHostRoutes[i].flag, hc);
    return -1;
}

const char* const* dispatchedWorldModes(unsigned& count) {
    count = kHostRouteCount;
    return kHostRouteFlags;
}

}} // namespace x3::apphost
