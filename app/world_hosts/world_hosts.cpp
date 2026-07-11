// world_hosts dispatch (#28 deep split). Matches worldMode (and the destruct
// OR-flag) in the SAME order the inline main() blocks ran; returns the program
// exit code, or -1 to fall through to main() (its remaining inline hosts / the
// default render loop). Hosts are wired in here AS they are extracted so the
// dispatch + main() stay link-clean and gate-green at every step.
#include "../host_context.h"
#include "../world_hosts.h"

namespace x3 { namespace apphost {

int dispatchWorldHost(HostContext& hc) {
    if (hc.worldMode == "destruct" || hc.destructShot)  return hostDestruct(hc);
    if (hc.worldMode == "physjoint")                    return hostPhysJoint(hc);
    if (hc.worldMode == "ragdoll")                      return hostRagdoll(hc);
    if (hc.worldMode == "drive" || hc.worldMode == "boat" || hc.worldMode == "fly")
                                                        return hostDrive(hc);
    if (hc.worldMode == "club")                         return hostClub(hc);
    if (hc.worldMode == "showroom")                     return hostShowroom(hc);
    if (hc.worldMode == "valley")                       return hostValley(hc);
    if (hc.worldMode == "cliffs")                       return hostCliffs(hc);
    if (hc.worldMode == "streamed")                     return hostStreamed(hc);
    if (hc.worldMode == "space")                        return hostSpace(hc);
    if (hc.worldMode == "surface")                      return hostSurfaceStart(hc);
    if (hc.worldMode == "introcockpit")               return hostIntroCockpit(hc);
    if (hc.worldMode == "ship-windows")               return hostShipWindows(hc);
    if (hc.worldMode == "descentslide")               return hostDescentSlide(hc);
    if (hc.worldMode == "bodycontact")                return hostBodyContact(hc);
    if (hc.worldMode == "wormhole")                   return hostWormhole(hc);
    if (hc.worldMode == "wormhole-transit")           return hostWormholeTransit(hc);
    if (hc.worldMode == "tractor")                    return hostTractor(hc);
    if (hc.worldMode == "strata")                       return hostStrata(hc);
    if (hc.worldMode == "elevator-showcase")            return hostElevator(hc);
    if (hc.worldMode == "rifthub")                      return hostRifthub(hc);
    return -1;
}

}} // namespace x3::apphost
