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
#include "../host_context.h"
#include "../world_hosts.h"

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
    { "echotropolis",      hostEchotropolis    },
    { "gallery",           hostGallery         },
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
};
static_assert(kHostRouteCount == 27, "update kHostRouteFlags when adding a route");

} // namespace

int dispatchWorldHost(HostContext& hc) {
    // --screenshot-destruct forces the destruct host regardless of worldMode
    // (the old first line was `worldMode == "destruct" || hc.destructShot`).
    if (hc.destructShot) return hostDestruct(hc);
    for (unsigned i = 0; i < kHostRouteCount; ++i)
        if (hc.worldMode == kHostRoutes[i].flag) return kHostRoutes[i].host(hc);
    return -1;
}

const char* const* dispatchedWorldModes(unsigned& count) {
    count = kHostRouteCount;
    return kHostRouteFlags;
}

}} // namespace x3::apphost
