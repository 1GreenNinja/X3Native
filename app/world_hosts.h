#pragma once
// ============================================================================
// world_hosts — registry of the self-contained `--world` host bodies extracted
// out of main() (#28 deep monolith split).
//
// Each host was an inline block in main() of the form:
//     if (worldMode == "X") { ... return 0; }
// where the body builds its own scene/physics, runs either a headless capture or
// a windowed walk loop, tears down the device + window + glfw, and returns the
// program exit code. Lifted VERBATIM into app/world_hosts/<name>.cpp behind this
// registry; the only edits are reaching shared state via the HostContext.
//
// dispatchWorldHost() checks worldMode and runs the matching host; it returns:
//   >= 0 : a matched host ran — this is the program exit code, return it.
//   -1   : no discrete host matched — main() continues into the default host
//          (the shared interactive render loop / cell+terrain+canon+fromdoc).
// ============================================================================

namespace x3 { namespace apphost {

struct HostContext;

int hostDestruct (HostContext& hc);   // --world destruct  (+ --screenshot-destruct)
int hostPhysJoint(HostContext& hc);   // --world physjoint
int hostRagdoll  (HostContext& hc);   // --world ragdoll
int hostDrive    (HostContext& hc);   // --world drive | boat | fly (+ perfshop)
int hostClub     (HostContext& hc);   // --world club      (+ crowd proof)
int hostShowroom (HostContext& hc);   // --world showroom  (+ showroom-* proofs)
int hostValley   (HostContext& hc);   // --world valley    (+ ecology proof)
int hostCliffs   (HostContext& hc);   // --world cliffs
int hostStreamed (HostContext& hc);   // --world streamed
int hostSpace    (HostContext& hc);   // --world space     (Act-3 6DOF space pilot)
int hostSurfaceStart(HostContext& hc);
int hostStrata   (HostContext& hc);   // --world strata (R-3 fold: THE DESCENT)
int hostElevator (HostContext& hc);   // --world elevator-showcase (+ --screenshot-elevator)// --world surface   (Phase 7 ESCAPED-branch Act-1 landing)
int hostRifthub  (HostContext& hc);   // --world rifthub   (RIFTHUB Stargate portal hub)

int hostIntroCockpit(HostContext& hc); // --world introcockpit (intro cold-open cockpit)
int hostShipWindows(HostContext& hc);  // --world ship-windows (S5/S6 fold: walkable interior + true-portal space)
int hostDescentSlide(HostContext& hc); // --world descentslide (Wave 2C: the B1 -> -178 m coaster-grade ride)
int hostBodyContact(HostContext& hc);     // --world bodycontact (BODY CONTACT feature: rigid rest + soft mattress indent)
int hostWormhole(HostContext& hc);        // --world wormhole (feast fold: Salvari crystal-matrix tunnel VFX)
int hostWormholeTransit(HostContext& hc); // --world wormhole-transit (feast fold: S3 autopilot jump ride)
int hostTractor(HostContext& hc);         // --world tractor (feast fold: intro capital-ship capture beam)
int hostUndersea(HostContext& hc);        // --world undersea (Act-4 Abyssal Station art overlay + marine snow)

// Returns the exit code if a discrete host matched worldMode, else -1.
int dispatchWorldHost(HostContext& hc);

// --test-surfacestart (Phase 7): headless self-test of the ESCAPED-branch surface
// start — the cell-vs-surface branch selection (escaped -> surface, shot_down ->
// cell) AND the surface scene standing up headlessly (glass facility wall, player
// outside + armed, Sarah staged as a rescue target, the rescuer objective list).
// No window / Vulkan. Returns true iff all sub-checks pass.
bool runSurfaceStartSelfTest();

// --test-introcockpit: the intro cockpit GLB -> Scene-entity rig (PBR route +
// emissiveTex content screens + transparent canopy glass) standing up on the
// HeadlessRenderDevice. No window / Vulkan. True iff all sub-checks pass.
bool runIntroCockpitSelfTest();

}} // namespace x3::apphost
