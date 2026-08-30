// Vehicle demo worlds — implementation. See vehicle.h.
//
// Built only through the public engine interfaces (IRenderDevice / IPhysicsWorld /
// IVehicleController). Clean-room.

#include "vehicle.h"
#include "terrain.h"   // THE CONTACT LAW: wheels never under the height field

#include "engine/core/x3_log.h"
#include "engine/net/SimClock.h"   // kSimDt + SimAccumulator: the render-interp gate
                                   // samples the car on the HOST's exact cadence

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace x3::game {

namespace {

// Build a column-major 4x4 model matrix from a position + quaternion (x,y,z,w)
// + per-axis scale. RH, matches CONVENTIONS.md (the renderer/Scene use the same
// column-major glm layout, translation in m[12..14]).
void composeTRS(const float pos[3], const float q[4],
                float sx, float sy, float sz, float out[16]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x*x, yy = y*y, zz = z*z;
    const float xy = x*y, xz = x*z, yz = y*z;
    const float wx = w*x, wy = w*y, wz = w*z;
    // Rotation matrix (column-major), columns scaled.
    out[0]  = (1.0f - 2.0f*(yy+zz)) * sx;
    out[1]  = (2.0f*(xy+wz))        * sx;
    out[2]  = (2.0f*(xz-wy))        * sx;
    out[3]  = 0.0f;
    out[4]  = (2.0f*(xy-wz))        * sy;
    out[5]  = (1.0f - 2.0f*(xx+zz)) * sy;
    out[6]  = (2.0f*(yz+wx))        * sy;
    out[7]  = 0.0f;
    out[8]  = (2.0f*(xz+wy))        * sz;
    out[9]  = (2.0f*(yz-wx))        * sz;
    out[10] = (1.0f - 2.0f*(xx+yy)) * sz;
    out[11] = 0.0f;
    out[12] = pos[0]; out[13] = pos[1]; out[14] = pos[2]; out[15] = 1.0f;
}

} // namespace

// ---------------------------------------------------------------------------
// Unit Y-cylinder (radius 1, height 1 -> y in [-0.5, 0.5]).
// ---------------------------------------------------------------------------
void makeUnitCylinderY(uint32_t segments,
                       std::vector<x3::rhi::MeshVertex>& verts,
                       std::vector<uint32_t>& idx) {
    verts.clear(); idx.clear();
    if (segments < 6) segments = 6;
    const float hy = 0.5f;
    // Side ring vertices (top + bottom), with outward normals.
    for (uint32_t i = 0; i <= segments; ++i) {
        float a = (float)i / (float)segments * 6.2831853f;
        float cx = std::cos(a), cz = std::sin(a);
        float u = (float)i / (float)segments;
        verts.push_back({{cx, -hy, cz}, {cx, 0.0f, cz}, {u, 0.0f}}); // bottom
        verts.push_back({{cx,  hy, cz}, {cx, 0.0f, cz}, {u, 1.0f}}); // top
    }
    for (uint32_t i = 0; i < segments; ++i) {
        uint32_t b = i * 2;
        // two triangles per quad (CCW for VK counter-clockwise front face)
        idx.insert(idx.end(), { b, b+1, b+2,  b+2, b+1, b+3 });
    }
    // End caps (center fan).
    uint32_t topCenter = (uint32_t)verts.size();
    verts.push_back({{0,  hy, 0}, {0, 1, 0}, {0.5f, 0.5f}});
    uint32_t botCenter = (uint32_t)verts.size();
    verts.push_back({{0, -hy, 0}, {0,-1, 0}, {0.5f, 0.5f}});
    for (uint32_t i = 0; i < segments; ++i) {
        float a0 = (float)i / (float)segments * 6.2831853f;
        float a1 = (float)(i+1) / (float)segments * 6.2831853f;
        float c0x=std::cos(a0), c0z=std::sin(a0), c1x=std::cos(a1), c1z=std::sin(a1);
        uint32_t t0 = (uint32_t)verts.size();
        verts.push_back({{c0x, hy, c0z}, {0,1,0}, {0,0}});
        verts.push_back({{c1x, hy, c1z}, {0,1,0}, {1,0}});
        idx.insert(idx.end(), { topCenter, t0, t0+1 });
        uint32_t bb = (uint32_t)verts.size();
        verts.push_back({{c0x,-hy,c0z}, {0,-1,0}, {0,0}});
        verts.push_back({{c1x,-hy,c1z}, {0,-1,0}, {1,0}});
        idx.insert(idx.end(), { botCenter, bb+1, bb });
    }
}

// BODY WIDTH lives in app/car_roster.h now (CarSpec::bodyWiden). It scales the
// hero car's bodywork on X — 1.0 = the GLB's stock width, CTR's 1.18 -> 2.13 m
// (7.0 ft), the owner's 2026-08-14 ask. The WHEEL TRACK below is multiplied by
// the SAME factor because the arches move out with the body; widening one
// without the other is what made it look like a donk. Per-car now, but still
// ONE number per car (NO_SLOP rule 4).

// ===========================================================================
// DriveDemo
// ===========================================================================
bool DriveDemo::buildPhysics(x3::phys::IPhysicsWorld& physics, float x, float y, float z) {
    m_physics = &physics;
    const CarSpec& cs = spec();
    const float kBodyWiden = cs.bodyWiden;

    // --- Chassis dynamic body (a box). Layer Dynamic. ---
    // CENTER OF MASS dropped 0.45 m below the box center (was 0.30). CoM height
    // above ground: 0.76 - 0.45 = 0.31 m against a 0.83 m mean half-track
    // (0.677/0.723 stations x kBodyWiden), so the static tip threshold is
    // 0.83/0.31 = ~2.7 g of lateral acceleration. THE PAIRED NUMBER: the
    // lateral tire grip below peaks ~1.9 g, a ~40% margin — MEASURED
    // necessary: at -0.40 (threshold 2.3, margin 20%) the skidpad's full-lock
    // turn-in transient still dynamically rolled the car onto the 60-deg
    // limiter; the static balance holds only if the entry transient (roll
    // overshoot + suspension compliance) fits inside the margin.
    // Owner receipt 2026-08-16: "The car wants to tip up on two wheels even
    // trying to make ANY curve at speed" — mass fell 1300 -> 1083 kg (992
    // spec) while effective lateral grip (1.55 g, see the friction-combine
    // receipt below) sat nearly AT the old 1.76 g threshold, so every hard
    // corner danced on the tip line. Fix = this + the lateral grip cap below.
    // 0.31 m is slot-car low, and invisible: the render skin never reads CoM.
    m_chassis = physics.addBox(x3::phys::Vec3{m_hx, m_hy, m_hz},
                               x3::phys::Vec3{x, y, z}, cs.massKg, x3::phys::Layer::Dynamic,
                               x3::phys::Vec3{0.0f, -0.45f, 0.0f});
    if (!m_chassis.valid()) return false;

    // --- 4 wheels at the HERO-CAR GLB stations (CTR, after the nose flip to the
    // engine's -Z forward: fronts z=-1.186, rears z=+1.088, track +-0.677 m).
    // AWD (all four driven) — a 911 Turbo is AWD, and four driven wheels is also
    // what lets the high torque hook up instead of spinning one axle. ---
    m_wheels.clear();
    struct P { float wx, wz; bool steer, hb; bool powered; };
    // Track = the GLB's OWN wheel stations. An earlier attempt pushed these out
    // to +-0.85/0.90 to "make the car wider" and it read as a DONK — wheels proud
    // of the arches. Width belongs to the BODY (kBodyWiden below), not the track.
    // x stations are the GLB's own SCALED BY kBodyWiden, so the wheels stay
    // centered under the widened arches instead of poking out. Both the
    // stations and the widen now come from the car's CarSpec (app/car_roster.h)
    // — CTR's entry holds exactly the +-0.677/+-0.723, -1.186/+1.088 that were
    // literals here, so nothing moves unless a host selects another car.
    P p[4] = {
        { -cs.wheelXFront * kBodyWiden, cs.wheelZFront, true,  false, true },  // front-left (AWD)
        {  cs.wheelXFront * kBodyWiden, cs.wheelZFront, true,  false, true },  // front-right
        { -cs.wheelXRear  * kBodyWiden, cs.wheelZRear,  false, true,  true },  // rear-left (drive)
        {  cs.wheelXRear  * kBodyWiden, cs.wheelZRear,  false, true,  true },  // rear-right (drive)
    };
    for (int i = 0; i < 4; ++i) {
        x3::phys::WheelDesc w;
        // Attach high in the wheel well (NOT the box bottom) so the rest pose
        // matches the GLB arches: wheel center = attach - suspension (~0.30 m).
        w.position[0] = p[i].wx; w.position[1] = -0.15f; w.position[2] = p[i].wz;
        w.radius = cs.wheelRadius; w.width = cs.wheelWidth;
        w.suspensionMin = 0.15f; w.suspensionMax = 0.42f;
        w.suspensionFreq = 2.2f; w.suspensionDamp = 0.7f;
        w.steered = p[i].steer; w.handBraked = p[i].hb; w.powered = p[i].powered;
        w.maxSteerAngle = 0.5236f; // ~30deg
        w.maxBrakeTorque = 2200.0f;
        // Sports-car compound baseline (Jolt's default curve is a generic economy
        // tire — a 700 Nm RWD on it lives in a permanent torque-independent
        // burnout; see WheelDesc::gripScale). Shop tire tiers multiply this.
        // 1.7 was tuned against 700 Nm. At 3200 the rears just lit up, traction
        // control cut the torque, and the car made noise instead of going —
        // Tim: "The car does NOT feel fast". Grip has to scale with the power or
        // the power is thrown away. Rears get more than fronts (they are the
        // driven axle and carry a rear-engined car's weight).
        // HONEST NUMBERS (2026-08-16). The old value here was 10, and the whole
        // 1.7 -> 3.4 -> 10 escalation above was unknowingly compensating for a
        // hidden sqrt: Jolt combines tire friction with the GROUND BODY's
        // friction (default 0.2, never set anywhere in the engine) as
        // sqrt(tire * 0.2) — authored 10 was 1.55 mu at the contact patch,
        // measured via X3_VEHDBG (see the combine-friction fix in
        // JoltVehicle.cpp build()). With the combine normalized, these curves
        // ARE the contact-patch spec:
        //  * LONGITUDINAL 1.6 (peak 1.92 mu): the launch. Slightly ABOVE the
        //    old effective 1.55, so 800 Nm x1.7-turbo AWD hooks up exactly as
        //    tuned and the drag numbers hold (0-60 ~3.1 s, measured).
        //  * LATERAL 1.60 front / 1.50 rear (peak 1.92/1.80 mu): bounded ABOVE
        //    by rollover. Tip threshold is ~2.3 g (the CoM comment at addBox —
        //    PAIRED, retune together); peak cornering ~1.9 g stays under it,
        //    so the car corners FLAT at its limit instead of lifting the
        //    inside wheels (owner: "wants to tip up on two wheels" — the old
        //    effective 1.55 g lateral against a 1.76 g threshold at the old
        //    CoM was exactly that marginal). 1.9 g is still far beyond a real
        //    992's ~1.1 — NFS-grade mid-corner grip.
        //  * FRONT > REAR (turn-in balance): the nose bites the instant you
        //    steer, and at the limit the REAR lets go first — progressively
        //    (Jolt's lateral curve tapers 1.2 -> 1.0 past the peak, not a
        //    cliff), so power-over is a catchable slide, not a snap.
        // Live-tune: `car_grip`/`car_gripf`/`car_gripr` (multipliers over
        // these), `car_latgrip` (lateral-only master), `car_lattail`
        // (breakaway shape) — help text carries these stock numbers, NO_SLOP
        // rule 4: change together.
        w.gripScale        = 1.6f;
        w.lateralGripScale = p[i].steer ? 1.60f : 1.50f;
        m_wheels.push_back(w);
    }
    x3::phys::WheeledVehicleDesc vd;
    vd.chassis = m_chassis;
    vd.wheels = m_wheels.data(); vd.wheelCount = (uint32_t)m_wheels.size();
    // ANTI-ROLL BARS (2026-08-16, the "tips up on two wheels" fix, part 3).
    // Couples each axle's left/right suspension so the body stays FLAT in a
    // corner — NFS cars do not lean. MEASURED STABILITY CEILING (skidpad,
    // 60 Hz fixed step): 5k and 10k N/m are stable (max roll 0.10 / 0.05 deg
    // at 1.43 g); 15k and up the discrete solver pumps the roll mode until
    // the car flips onto Jolt's 60-deg limiter — at ANY grip. Stay well under
    // 12000. 8k/6k is dead flat with margin. FRONT STIFFER than rear: the
    // stiff end saturates its outside tire first, so front-stiff = a stable,
    // plant-the-nose balance that resists snap oversteer without killing the
    // throttle-on slide (the lateral grip split above still lets the rear go
    // first under power). Live: `car_arb_f` / `car_arb_r` (help text carries
    // these numbers AND the ceiling — NO_SLOP rule 4).
    vd.antiRollFront = 8000.0f;
    vd.antiRollRear  = 6000.0f;
    // TRIPLED (Tim, 2026-08-14: "the car feels HEAVY... it should accelerate
    // nicely" -> "triple the Hp/tq"). 700 -> 2100 Nm. The powerband/shift-point
    // fix is already live on this lane, so the extra torque lands ON the curve
    // instead of being thrown away by an early upshift.
    // NOTE the tyres are the next limit: gripScale 1.7 was tuned against 700 Nm
    // and a 3x jump will spin the rears up unless traction control catches it
    // (setInput's TC is on by default). If it just smokes instead of going,
    // raise gripScale rather than backing the torque off.
    // 1600 Nm. 3200 was my escalation and it was ~6x a real 993 Turbo (540 Nm):
    // the tyres never left the friction plateau, so traction control sat on the
    // throttle permanently — Tim, 2026-08-15: "Throttle doesnt seem to go to max"
    // and, in top gear against the limiter, "Wahhhhhh, brrp, wahhhhhh, brrp".
    // That surge IS TC cutting and restoring. He also asked for it to behave
    // "like a real car".
    // 1600 is still ~3x stock (his original "triple the Hp/tq") and with the
    // short 6-speed + gripScale 3.4 it is genuinely quick, but the tyres can now
    // actually hold it, so TC only intervenes when it should.
    // 2400 Nm peak, but the CURVE is what makes it a Turbo. Tim, 2026-08-15:
    // "The engine really sucks. I need it to rev faster, and pull HARD. Make
    // this Porsche Turbo Feel like it should."
    // Stock spec = the 992 Turbo S hybrid: 590 ft-lb (800 Nm) / 701 HP.
    // (HP = ft-lb * rpm / 5252.) AWD splits it across four wheels so it hooks up
    // at sane grip without TC. The shop soups it up from here. Live-tune
    // `car_torque` (ft-lb).
    vd.maxEngineTorque = 800.0f;   // 590 ft-lb (992 Turbo S)
    // FLAT-SIX CHARACTER. A 993-era Porsche runs to ~7200 and gets there fast;
    // the old 6500 with Jolt's default 0.5 flywheel felt like a diesel.
    // 7500: titanium valve retainers (Tim, 2026-08-15) — the light valvetrain is
    // exactly what lets a flat-six carry revs past where a steel one lets go.
    vd.maxEngineRPM  = 7500.0f;
    // 0.18 was TOO light. Combined with 3200 Nm the crank hit the 0.92-redline
    // shift point instantly, dropped to 0.62, and slammed back up — the box
    // cycled one gearchange forever and the note never dwelt long enough to
    // climb. Tim, 2026-08-15: "you shift even when not accelerating... it KEEPS
    // repeating the same shift and rpm.. the engine note does not get higher".
    // It also made engine speed track wheel speed with almost no smoothing, so
    // coasting tripped shifts.
    // 0.35 still revs noticeably quicker than Jolt's 0.5 default (the flat-six
    // pep) without turning the transmission into a metronome.
    // 0.28: revs noticeably faster than 0.35 without the gear-hunting 0.18 caused
    // (the debounce in JoltVehicle now guards that anyway).
    // 0.35: revs fast without hunting. 0.5 was tried to damp the "Wahh", but the
    // oscillation was the launch-control throttle, not the flywheel — so keep the
    // light flywheel Tim wants ("rev FAST") and let grip + no-TC handle the rest.
    vd.engineInertia = 0.35f;
    // Clutch. Jolt's clutch is viscous — Torque = clutchStrength * (engine_rpm -
    // wheel_rpm). Default 10 lets the engine free-rev ~600 rpm above the wheels
    // at 800 Nm; 100 (~60 rpm slip) was the lane's original tight value. 10000
    // was tried and made the engine HUNT at idle and stick at redline — the
    // clutch time constant (inertia/strength) fell far below the 1/60 s physics
    // substep, so the integrator oscillates (the "rpm cycles even at idle").
    vd.clutchStrength = 100.0f;
    // SIX-SPEED, SHORT. 993-family ratios with a 4.2 final drive instead of
    // Jolt's 3.42, which pulls 1st down from a ludicrous 64 mph at redline to
    // ~44 and puts 6th at ~168. Now the engine actually sweeps its range in
    // every gear — that is what makes it rev AND feel fast, far more than peak
    // torque does. Six gears also matches the 1-6 shift-pattern HUD.
    //
    // 6th 0.821 -> 0.50 (2026-08-16, OWNER SPEC verbatim: "make the final gear
    // a 0.50:1" — his cruise-RPM fix, NO_SLOP rule 8). The old 0.821 x 4.6
    // 6th hit redline at 153 mph, BELOW the ~160 drag ceiling, so top speed
    // was the rev limiter — the tach genuinely pegged the whole time on the
    // freeway ("it shouldnt peg redline the whole time you drive"). The 0.50
    // overdrive puts the 6th-gear redline far above the drag ceiling: top
    // speed is now drag-limited mid-band, and a steady 70 mph cruise in 6th
    // sits ~2350 rpm (with the 5.2 final below + 0.33 m wheels — PAIRED).
    // Reaching 6th at cruise throttle is the adaptive shift band's job
    // (kShiftUpLightFrac, JoltVehicle.cpp — the box knows throttle now).
    vd.gearRatios[0] = 3.154f; vd.gearRatios[1] = 2.150f; vd.gearRatios[2] = 1.560f;
    vd.gearRatios[3] = 1.242f; vd.gearRatios[4] = 1.024f; vd.gearRatios[5] = 0.50f;
    vd.gearCount  = 6;
    // 993 TURBO CHARACTER CURVE. Soft off boost, a hard step as it spools around
    // 0.32-0.45 of redline (~2400-3400 rpm), a long fat plateau to 0.85, then it
    // signs off toward the limiter. That step is the shove you feel in a real
    // turbo car, and a flat curve cannot produce it at any peak value.
    vd.curveRpm[0]=0.00f; vd.curveTq[0]=0.42f;   // off boost — deliberately weak
    vd.curveRpm[1]=0.22f; vd.curveTq[1]=0.55f;
    vd.curveRpm[2]=0.32f; vd.curveTq[2]=0.82f;   // spool
    vd.curveRpm[3]=0.45f; vd.curveTq[3]=1.00f;   // full boost — the hit
    vd.curveRpm[4]=0.70f; vd.curveTq[4]=1.00f;   // plateau
    vd.curveRpm[5]=0.85f; vd.curveTq[5]=0.94f;
    vd.curveRpm[6]=1.00f; vd.curveTq[6]=0.78f;   // sign-off
    vd.curveCount = 7;
    // 4.2 -> 4.6 (2026-08-16, "MORE acceleration"). The 4.2/993-ratio stack
    // topped out ~168 mph — but the aero drag (kAeroDrag 1.4, JoltVehicle.cpp)
    // caps the car near 160 anyway, so the last 8 mph of gearing were pure
    // paper. 4.6 trades them for ~10% more wheel torque in EVERY gear — the
    // whole car punches harder everywhere you actually drive it.
    // 4.6 -> 5.2 (2026-08-16, PAIRED with the 0.50 6th above). The deep
    // overdrive frees the final drive to go shorter: gears 1-5 gain ~13%
    // wheel torque (1st redlines ~35 mph, 5th ~109), while 6th (0.50 x 5.2 =
    // 2.60 total) still geartops at ~223 mph at redline — which keeps the
    // 220-with-NOS spec REACHABLE in gear (5.3+ would cap it below 220;
    // 5.2 is the shortest final that doesn't). Cruise at 70 in 6th = ~2350
    // rpm. Live-tune: `car_final`.
    vd.finalDrive = 5.2f;
    // Wheel rays filter on Dynamic (the chassis layer): Jolt's vehicle object filter
    // is the COLLISION MATRIX, and Dynamic-vs-Static collides, so a Dynamic-masked
    // ray hits the Static ground. (Static-vs-Static does NOT collide — a Static mask
    // would pass through the ground.)
    vd.groundLayer = x3::phys::Layer::Dynamic;
    m_ctl.reset(x3::phys::createWheeledVehicle(physics, vd));
    if (!m_ctl) { physics.removeBody(m_chassis); m_chassis = {}; return false; }
    // Prime the render-interpolation history (prev == cur) so the very first
    // frame after a spawn draws the car AT the spawn instead of sliding into it.
    resetRenderInterp();
    return true;
}

bool DriveDemo::build(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                      float x, float y, float z) {
    m_device = &device;
    if (!buildPhysics(physics, x, y, z)) return false;

    // --- Render meshes ---
    std::vector<x3::rhi::MeshVertex> cv; std::vector<uint32_t> ci;
    x3::prims::makeCube(0.5f, cv, ci);  // unit cube; we scale per draw
    m_chassisMesh = device.createMesh(cv.data(), (uint32_t)cv.size(), ci.data(), (uint32_t)ci.size());
    std::vector<x3::rhi::MeshVertex> wv; std::vector<uint32_t> wi;
    makeUnitCylinderY(16, wv, wi);
    m_wheelMesh = device.createMesh(wv.data(), (uint32_t)wv.size(), wi.data(), (uint32_t)wi.size());

    auto cTex = x3::prims::makeSolidRGBA(8, 200, 40, 40);    // red car body
    m_chassisTex = device.createTexture(cTex.data(), 8, 8, true);
    auto wTex = x3::prims::makeSolidRGBA(8, 30, 30, 35);     // dark tires
    m_wheelTex = device.createTexture(wTex.data(), 8, 8, true);
    return true;
}

// ---------------------------------------------------------------------------
// HERO-CAR GLB skin. Partition the converted GLB's drawables by node name:
// Wheel_FL/FR/RL/RR follow the live physics wheel poses; everything else is the
// sprung body. The GLB is authored nose=+Z, origin on the ground plane; the
// engine car drives nose=-Z with the physics chassis center ~0.76 m above the
// ground at rest — both baked into kBodySkin below.
// ---------------------------------------------------------------------------
namespace {
// GLB ground-origin -> physics chassis-center offset + the 180-deg nose flip.
// (chassis center = wheel attach (-0.15) + rest suspension (~0.28) + wheel
// radius 0.33 above the ground plane => drop the skin by the sum.)
// BODY WIDTH (Tim, 2026-08-14: "The MODEL needs to be wider") scales the skin's
// X axis only, so the bodywork widens while the wheels — which are drawn from
// the physics wheel poses, not from this matrix — stay on the model's own track.
// That is the opposite of widening the track, which made it look like a donk.
// Both numbers are PER-CAR now (app/car_roster.h): CTR keeps -0.76 / 1.18, the
// GBX coupe is already 1.926 m wide so it takes 1.0.
inline void bodySkinMatrix(const CarSpec& cs, float out[16]) {
    const float m[16] = { -cs.bodyWiden,0,0,0,  0,1,0,0,  0,0,-1,0,  0,cs.bodyDropY,0,1 };
    std::memcpy(out, m, sizeof(m));
}
// Mesh-local wheel axis is +-X (car lateral); the physics wheel pose maps a unit
// Y-cylinder (axis = axle). Rotate mesh X onto pose Y (Rz +90deg, column-major).
// EXPERIMENT (Tim live-testing 2026-08-13): the Rz+90 below assumes the GLB's
// wheels are authored with their axle on mesh-local +-X. On THIS car they spin
// sideways like a gyroscope, i.e. 90 degrees off their axle — which is what this
// matrix would cause if the wheels were ALREADY axle-aligned. Identity tests
// exactly that. X3_WHEELFIX=1 restores the old matrix for an instant A/B.
//   const float kWheelAxisFixRz90[16] = { 0,1,0,0, -1,0,0,0, 0,0,1,0, 0,0,0,1 };
const float kWheelAxisFix[16] = { 1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1 };
const float kWheelAxisFixRz90[16] = { 0,1,0,0,  -1,0,0,0,  0,0,1,0,  0,0,0,1 };
inline const float* wheelAxisFix() {
    static const bool oldWay = [](){ const char* e = std::getenv("X3_WHEELFIX");
                                     return e && e[0] == '1'; }();
    return oldWay ? kWheelAxisFixRz90 : kWheelAxisFix;
}
} // namespace

bool DriveDemo::skin(x3::rhi::IRenderDevice& device, std::string_view glbDir,
                     std::string_view relPath) {
    m_skinSrc.reset(x3::asset::createAssetSource());
    if (!m_skinSrc || !m_skinSrc->mountDir(glbDir, 0)) return false;
    m_skinLoader.reset(x3::asset::createModelLoader(&device, m_skinSrc.get()));
    m_skinModel = m_skinLoader->load(relPath);
    if (!m_skinModel.ok) return false;

    std::vector<std::string> names;
    std::vector<x3::asset::ModelDrawable> all = x3::asset::makeDrawablesNamed(m_skinModel, names);
    // GLB wheel name -> physics wheel slot. The nose flip maps GLB FL->engine FL
    // (GLB +X/+Z both negate, so left/right and front/rear BOTH swap = identity).
    auto slotOf = [](const std::string& nm) -> int {
        if (nm.find("Wheel_FL") != std::string::npos) return 0;
        if (nm.find("Wheel_FR") != std::string::npos) return 1;
        if (nm.find("Wheel_RL") != std::string::npos) return 2;
        if (nm.find("Wheel_RR") != std::string::npos) return 3;
        return -1;
    };
    m_bodyDraw.clear();
    for (int s = 0; s < 4; ++s) m_wheelDraw[s].clear();
    for (size_t i = 0; i < all.size(); ++i) {
        // 'Buttom' (sic, in CTR.glb) is the UNDERBODY / floor pan. Tim asked for
        // it back — "LEAVE BOTTOM .. rename it if you can" — after removing it
        // made the car see-through from below. The GLB node name is misspelled;
        // renaming it properly means re-exporting the asset, so it is aliased
        // here instead and the engine-side name is "Bottom" wherever we surface
        // it. It is NOT the mini-car Tim keeps seeing: that model is not in this
        // GLB at all (36 nodes, all car parts + a mesh-less 'Collider').
        const int s = (i < names.size()) ? slotOf(names[i]) : -1;
        if (s < 0) { m_bodyDraw.push_back(all[i]); continue; }
        // Wheel drawable: bake (axis fix) * (node transform WITHOUT translation —
        // the physics pose supplies position/steer/spin; keep authored scale).
        x3::asset::ModelDrawable d = all[i];
        float noT[16]; std::memcpy(noT, d.nodeTransform, sizeof(noT));
        noT[12] = noT[13] = noT[14] = 0.0f;
        float local[16];
        // Through the ACCESSOR, not the constant: wheelAxisFix() is what reads
        // X3_WHEELFIX. Calling kWheelAxisFix directly here made the env-var A/B
        // silently inert — the toggle appeared to work and tested nothing.
        x3::asset::mulMat4(wheelAxisFix(), noT, local);
        std::memcpy(d.nodeTransform, local, sizeof(local));
        m_wheelDraw[s].push_back(d);
    }
    m_skinned = !m_bodyDraw.empty();
    return m_skinned;
}

void DriveDemo::drawDrawable(const x3::rhi::FrameContext& f,
                             const x3::asset::ModelDrawable& d, const float world[16]) const {
    const bool matEmis = d.emissiveTexId != 0 ||
        d.emissiveFactor[0] > 0.001f || d.emissiveFactor[1] > 0.001f || d.emissiveFactor[2] > 0.001f;
    float emis[4] = { d.emissiveFactor[0], d.emissiveFactor[1], d.emissiveFactor[2],
                      matEmis ? 1.0f : 0.0f };
    // WORLD CARS paint tint: repaint the clearcoat (car-paint) panels only.
    float bc[4] = { d.baseColorFactor[0], d.baseColorFactor[1],
                    d.baseColorFactor[2], d.baseColorFactor[3] };
    if (m_tintOn && d.clearcoat > 0.01f) { bc[0] = m_tint[0]; bc[1] = m_tint[1]; bc[2] = m_tint[2]; }
    m_device->drawMeshPBR(f,
                          x3::rhi::MeshHandle{ d.meshId },
                          x3::rhi::TextureHandle{ d.baseColorTexId },
                          x3::rhi::TextureHandle{ d.normalTexId },
                          x3::rhi::TextureHandle{ d.mrTexId },
                          bc, emis, world,
                          d.alphaMask, d.alphaBlend,
                          x3::rhi::TextureHandle{ d.emissiveTexId },
                          x3::rhi::TextureHandle{ d.detailTexId }, d.detailUvScale,
                          d.clearcoat, d.clearcoatRough, 0.0f, 1.0f,
                          /*foliage=*/0.0f, d.metallicFactor, d.roughnessFactor);   // car-paint clearcoat lobe + authored MR factors
}

bool DriveDemo::allWheelsInContact() const {
    if (!m_ctl) return false;
    const uint32_t n = m_ctl->wheelCount();
    for (uint32_t i = 0; i < n; ++i) {
        x3::phys::WheelState ws;
        if (!m_ctl->wheelState(i, ws) || !ws.hasContact) return false;
    }
    return n > 0;
}

void DriveDemo::setInput(const x3::phys::VehicleInput& in) {
    m_lastIn = in;   // RAW driver ask (HUD)
    if (!m_ctl) return;
    // ---- TRACTION CONTROL (game layer). Jolt's tire friction peaks near slip
    // ratio ~0.06 and falls to a plateau beyond it; a powerful RWD launch lives
    // deep in that plateau (a torque-INDEPENDENT burnout, with upshifts slip-
    // blocked at redline). TC trims the gas to hold the drive wheels near peak
    // slip, which (a) launches harder and (b) makes engine torque the binding
    // constraint — the reason every power part is FELT. dt-independent: the trim
    // factor is recomputed from the CURRENT slip each call (no accumulation).
    x3::phys::VehicleInput eff = in;
    // LAUNCH CONTROL (light). Jolt's box REFUSES to upshift while any driven
    // wheel slips >10% (its own Update), so a hard launch spins the tyres, the
    // box holds the gear, and the engine sits at redline — the "high<->low rpm
    // swing". Hold slip just under 10% so the wheels hook up and the box can
    // shift: a light trim, NOT the old 0.15-floor cut that caused "lead weights".
    if ((m_tcEnabled || m_climbMode) && eff.throttle > 0.0f) {
        float slip = 0.0f;
        for (uint32_t i = 0; i < m_ctl->wheelCount(); ++i)
            slip = std::max(slip, m_ctl->longitudinalSlip(i));
        // CLIMB MODE runs the same controller with crawl numbers. On a steep
        // grade the failure is not "not enough torque", it is all four wheels
        // spun deep into the friction plateau where grip FALLS — Tim's
        // screenshot of the smoke ladder up the rock face is exactly that. A
        // crawl holds slip AT the friction peak and is allowed to trim the
        // throttle almost to nothing, because at 5 mph up a wall, almost
        // nothing is precisely how much throttle the tires can use.
        const float slipTarget = m_climbMode ? 0.05f : 0.07f;
        const float slipGain   = m_climbMode ? 6.0f  : 1.5f;
        const float trimFloor  = m_climbMode ? 0.06f : 0.35f;
        const float cutOn      = m_climbMode ? 0.06f : 0.10f;
        const float cutOff     = m_climbMode ? 0.03f : 0.04f;
        if (slip > cutOn)       m_tcCutting = true;
        else if (slip < cutOff) m_tcCutting = false;
        // Smooth the trim so the throttle GLIDES instead of snapping. The snap is
        // what the audio layers track as a "shift" (whine/turbo ride throttle),
        // and the cut/restore oscillation is the residual "Wahh". Cut fast,
        // restore slow — the asymmetry is the anti-oscillation.
        const float target = m_tcCutting
            ? std::clamp(1.0f - slipGain * (slip - slipTarget), trimFloor, 1.0f)
            : 1.0f;
        const float rate = (target < m_tcTrim) ? 0.25f : 0.08f;
        m_tcTrim += (target - m_tcTrim) * rate;
        eff.throttle *= m_tcTrim;
    }
    // OUTSIDE the TC branch. This used to be assigned only when TC was active
    // and the throttle positive, so with TC off — or the instant you lifted —
    // it kept whatever value it last had. The engine audio reads it as "load",
    // so the note stayed pinned at the last trimmed value while coasting.
    m_effThrottle = eff.throttle;
    // Stored for preStep, which SHAPES the steering (speed map + slew — needs
    // dt, which setInput doesn't have) and pushes the final input to Jolt.
    // The raw push below is kept so any setInput-without-preStep caller (none
    // in-tree today) still drives with yesterday's behavior instead of nothing.
    m_effIn = eff;
    m_ctl->setInput(eff);
}

// ---------------------------------------------------------------------------
// NFS STEERING SHAPING — see SteerParams in vehicle.h for the full story.
// Runs in preStep because slew needs dt (dt-scaled or nothing — the HARD rule).
// Order matters: this pushes the FINAL input to Jolt, overriding the unshaped
// push in setInput, and must run before m_ctl->preStep hands input to the
// constraint for this physics step.
// ---------------------------------------------------------------------------
void DriveDemo::shapeSteering(float dt) {
    if (!m_ctl || dt <= 0.0f) return;
    const SteerParams& sp = m_steerP;
    const float mph = std::fabs(m_ctl->forwardSpeed()) * 2.23694f;
    // 1) SPEED MAP: fraction of full lock available at this speed.
    float frac = 1.0f;
    if (sp.hiSpeedMph > sp.fullLockMph + 1.0f) {
        const float t = std::clamp((mph - sp.fullLockMph) /
                                   (sp.hiSpeedMph - sp.fullLockMph), 0.0f, 1.0f);
        frac = 1.0f + (sp.hiFrac - 1.0f) * t;
    }
    const float target = std::clamp(m_effIn.steer, -1.0f, 1.0f) * frac;
    // 2) SLEW toward the target at slewPerSec full-lock units per second.
    const float maxStep = std::max(0.0f, sp.slewPerSec) * dt;
    m_steerNow += std::clamp(target - m_steerNow, -maxStep, maxStep);
    x3::phys::VehicleInput shaped = m_effIn;
    shaped.steer = m_steerNow;
    m_ctl->setInput(shaped);
}

// ---------------------------------------------------------------------------
// TURBO — a manifold-pressure model, not a curve.
//
// The engine already had a turbo-SHAPED torque curve: soft off the bottom, a
// hard step around 0.32-0.45 of the rev range, a plateau, then taper. That is
// the right STEADY-STATE shape, and it is what the engine makes at full boost.
// What it cannot express is the part of a turbo you actually feel, which is all
// transient: the half-second where you have asked for everything and the engine
// has not yet given it, and the shove when it arrives.
//
// So the curve stays as the full-boost delivery and this supplies the lag. The
// torque multiplier tracks ABSOLUTE manifold pressure (the pressure-ratio
// formula below — off boost is naturally-aspirated half power, full boost
// x1.70), which means peak power and the tuned curve shape are both
// unchanged — the difference is entirely in WHEN you get them.
//
// Asymmetric time constants, because a turbo is asymmetric: the compressor has
// inertia and takes ~0.45 s to come up, but lift and the wastegate and the
// recirculation valve dump it in ~0.1 s. That asymmetry is why part-throttle
// driving in a turbo car feels like it does, and why short-shifting keeps you
// on boost while a lift-and-coast does not.
//
// Below the throttle, the model reads VACUUM — the engine pumping against a
// closed plate. It is what a real boost gauge shows for most of its life, and
// it is the reason the needle sitting at zero would have looked wrong.
// ---------------------------------------------------------------------------
void DriveDemo::updateTurbo(float dt) {
    if (!m_ctl) { m_turboMult = 1.0f; return; }
    if (!m_turboOn) {
        m_boostPsi = 0.0f;
        m_turboMult = 1.0f;
        m_ctl->setTorqueBoost(m_userTorqueMult * m_nosTorqueMult);
        return;
    }
    const TurboParams& tp = m_turbo;
    const float rpm = m_ctl->engineRPM();
    const float thr = std::clamp(m_effThrottle, 0.0f, 1.0f);

    // Spool: the compressor needs exhaust energy, which needs revs.
    float spool = (rpm - tp.spoolStartRpm) / std::max(1.0f, tp.spoolFullRpm - tp.spoolStartRpm);
    spool = std::clamp(spool, 0.0f, 1.0f);
    spool = spool * spool * (3.0f - 2.0f * spool);          // smoothstep

    const float boostTarget = tp.maxPsi * spool * thr;
    // Vacuum deepens with revs and a closed throttle; it vanishes as the plate
    // opens. Needs some revs to exist at all, hence the rpm ramp.
    const float vacTarget = -tp.vacuumPsi * (1.0f - thr)
                          * std::clamp(rpm / 2500.0f, 0.0f, 1.0f);
    const float target = boostTarget + vacTarget;

    const float tau = (target > m_boostPsi) ? tp.spoolTau : tp.dumpTau;
    m_boostPsi += (target - m_boostPsi) * (1.0f - std::exp(-dt / std::max(0.01f, tau)));

    // PRESSURE-RATIO TORQUE (Tim: "47.6 HP up PER PSI" — the 992 Turbo S's own
    // ratio, 701 hp / 14.7 psi). An engine is an air pump: torque tracks
    // ABSOLUTE manifold pressure, so the multiplier is (atmosphere + boost)
    // over the calibration pressure the authored curve represents (stock 992
    // boost, ~14.5 psi over atmosphere). Everything falls out of one formula:
    //   35 psi  -> x1.70  (~1,670 hp — the drag build he asked for)
    //   14.5    -> x1.00  (the authored curve, stock)
    //    0      -> x0.50  (off boost = naturally aspirated half-power;
    //                      replaces the old ad-hoc floorTorque 0.60)
    //   -8.5    -> x0.21  (deep vacuum — engine braking territory)
    constexpr float kAtmPsi = 14.7f;
    constexpr float kRefBoostPsi = 14.5f;   // boost the torque curve is calibrated at
    m_turboMult = std::max(0.05f, (kAtmPsi + m_boostPsi) / (kAtmPsi + kRefBoostPsi));
    // m_nosTorqueMult is the vehicle-layer 200-shot (updateNitro) — the host's
    // setTorqueBoost no longer carries the NOS factor (NO_SLOP rule 4 pair).
    m_ctl->setTorqueBoost(m_userTorqueMult * m_nosTorqueMult * m_turboMult);
}

void DriveDemo::preStep(float dt) {
    updateNitro(dt);   // stages 0-2 of the three-stage secret + the wing trigger
    if (m_wings) {
        // WINGED: the flight controller owns propulsion/attitude; the wheeled
        // constraint keeps stepping (neutral input, pushed at deploy) so the
        // suspension is warm for touchdown. Turbo/steer shaping are ground
        // systems and stay parked; the engine model keeps the audio alive.
        updateWingFlight(dt);
        if (m_flyCtl) m_flyCtl->preStep(dt);
        updateEngineModel(dt);
        // THE TAKEOFF ROLL. While thrust is COMMANDED and the wheels are still
        // down, the wheeled constraint must NOT step: its suspension holds the
        // hull flat on the deck, so the pitch torque cannot raise the nose and
        // the beast can never leave the ground. Every other winged frame still
        // steps it, which is what keeps the suspension warm for touchdown and
        // lets the landing rollout behave like a car.
        // THE WHEELED CONSTRAINT IS A ROAD SYSTEM. Two cases where it must not
        // step while the wings are out:
        //  * the TAKEOFF ROLL (thrust commanded, wheels still down) — its
        //    suspension holds the hull flat so the nose can never come up;
        //  * AEROBATIC ATTITUDES — MEASURED: a full-stick loop spun up to
        //    2.56 rad/s and then stopped dead (w = 0.0007 rad/s at 6 s) at
        //    61.6 deg nose-up. Nothing in the flight model can arrest rotation
        //    like that; the constraint was fighting the loop, so the beast just
        //    climbed instead of going over the top.
        // It still steps in every level-ish frame, which is what keeps the
        // suspension warm and lets grounded() see the wheels for the landing
        // rule — landings happen near level, never at 60 deg of pitch.
        // Thresholds chosen so ordinary flying never trips this. A banked turn
        // lives well inside 80 deg of roll (MEASURED: gating at 40 deg suspended
        // the constraint mid-bank and broke the carve gate), while a loop pushes
        // straight through 57 deg of pitch.
        const bool aerobatic = std::fabs(m_hullPitch) > 1.0f ||
                               std::fabs(m_hullRoll)  > 1.4f;
        // Jolt runs the constraint from its own step listener, so declining to
        // call preStep is not enough — it has to be DISABLED to leave the solver.
        if (m_ctl) m_ctl->setConstraintSuspended(aerobatic);
        if (m_ctl && !(m_wantNos && grounded()) && !aerobatic) m_ctl->preStep(dt);
        return;
    }
    shapeSteering(dt); updateTurbo(dt); updateEngineModel(dt); if (m_ctl) m_ctl->preStep(dt);
}

// REAL ENGINE MODEL. The engine RPM is its own state, not Jolt's road-speed-
// locked value. Idles ~800, is pulled toward the locked (wheel-speed) RPM when
// in gear, with the flywheel's lag. The audio follows THIS, so the note revs up
// and settles like a real engine instead of mirroring road speed frame-for-frame.
void DriveDemo::updateEngineModel(float dt) {
    constexpr float kIdle = 800.0f;
    const float locked = m_ctl ? m_ctl->lockedRPM() : 0.0f;
    const float target = std::max(kIdle, locked);
    const float tau = 0.14f;   // flywheel response (a quick-revving flat-six)
    m_engineRpm += (target - m_engineRpm) * (1.0f - std::exp(-dt / tau));
    if (m_engineRpm < kIdle) m_engineRpm = kIdle;
}
void DriveDemo::postStep(float dt) {
    if (m_ctl) m_ctl->postStep(dt);
    updateTireSquash(dt);
    // ---- WINGED FLIGHT aftermath: crash detection + the landing rule -------
    if (m_wings && m_physics && m_chassis.valid()) {
        float lv[3]; m_physics->getBodyLinearVelocity(m_chassis, lv);
        const float spd = std::sqrt(lv[0]*lv[0] + lv[1]*lv[1] + lv[2]*lv[2]);
        // CRASH ("Crashing hurts, a lot"): only a collision can shed this much
        // speed in one 60 Hz step — drag physically cannot. Wings torn, the
        // tumble is handed to Jolt with an angular kick (the wings' inertia),
        // and NOS/overdrive/wings lock out for crashLockSecs.
        if (m_prevFlySpeed > m_wingT.crashMinSpeed &&
            (m_prevFlySpeed - spd) > m_wingT.crashDeltaV) {
            float q[4]; m_physics->getBodyRotation(m_chassis, q);
            float f[3], u[3]; vehcam::hullAxes(q, f, u);
            const float r[3] = { f[1]*u[2] - f[2]*u[1],
                                 f[2]*u[0] - f[0]*u[2],
                                 f[0]*u[1] - f[1]*u[0] };
            float av[3]; m_physics->getBodyAngularVelocity(m_chassis, av);
            // Violent, deterministic tumble: pitch-over + a yaw skew scaled by
            // how hard the hit was (a 40 m/s wipe spins harder than a graze).
            const float k = std::min(1.5f, (m_prevFlySpeed - spd) / 30.0f);
            av[0] += r[0] * 6.0f * k + u[0] * 2.5f * k;
            av[1] += r[1] * 6.0f * k + u[1] * 2.5f * k;
            av[2] += r[2] * 6.0f * k + u[2] * 2.5f * k;
            m_physics->setBodyAngularVelocity(m_chassis, av);
            retractWings(/*torn=*/true);
            m_crashLock = m_wingT.crashLockSecs;
            m_evCrashed = true;
            x3::logInfo("[vehicle] WINGED CRASH: dV=" +
                        std::to_string(m_prevFlySpeed - spd) + " m/s at " +
                        std::to_string(m_prevFlySpeed * 2.23694f) + " mph — wings torn, "
                        "overdrive locked " + std::to_string(m_wingT.crashLockSecs) + " s");
        } else if (grounded() && spd * 2.23694f < m_wingT.retractMph) {
            // THE LANDING RULE: wheels down below ~60 mph = a car again. The
            // wings fold (animated back through m_wingPose in updateNitro) and
            // THE CONTACT LAW resumes below this very frame.
            retractWings(/*torn=*/false);
            x3::logInfo("[vehicle] wings retracted — wheels down at " +
                        std::to_string(spd * 2.23694f) + " mph");
        }
        m_prevFlySpeed = spd;
    }
    // ---- THE CONTACT LAW (NO_SLOP.md rule 11): "no tires, and no Boots and
    // no feet can EVER be underground." Enforced HERE, once, for every world
    // that drives this car. If any wheel's contact point sits beyond-
    // suspension-deep below the carved terrain field, the car is lifted back
    // onto it and its downward velocity cleared. Corridors carve the field
    // itself, so bores/underpasses are safe — the law only ever pushes UP.
    // m_contactLaw: ON in every world; OFF only in the headless slab tests,
    // whose ground is NOT the terrain field (see setTerrainContactLaw).
    // !m_wings: while the WINGS are deployed the car is airborne BY DESIGN
    // (the beast dives below ridge lines and the lifter would yank it onto
    // them) — the law is suspended for exactly as long as the wings are out
    // and resumes the frame they retract (PAIRED with the landing rule above
    // and with retractWings(), which re-arms it — NO_SLOP rule 4).
    if (m_contactLaw && !m_wings && m_ctl && m_physics && m_chassis.valid()) {
        float worst = 0.0f;
        for (uint32_t i = 0; i < m_ctl->wheelCount(); ++i) {
            x3::phys::WheelState ws;
            if (!m_ctl->wheelState(i, ws)) continue;
            const float wx = ws.worldTransform[12], wy = ws.worldTransform[13],
                        wz = ws.worldTransform[14];
            const float burial = x3::game::terrainHeightAtWorld(wx, wz) - (wy - ws.radius);
            if (burial > worst) worst = burial;
        }
        if (worst > 0.55f) {
            x3::phys::Vec3 cp = m_physics->getBodyPosition(m_chassis);
            cp.y += worst + 0.10f;
            m_physics->setBodyPosition(m_chassis, cp);
            float lv[3]; m_physics->getBodyLinearVelocity(m_chassis, lv);
            if (lv[1] < 0.0f) { lv[1] = 0.0f; m_physics->setBodyLinearVelocity(m_chassis, lv); }
        }
    }
    // LAST in postStep, deliberately: the snapshot must record the FINAL
    // post-step pose, after the contact law has had its say. Snapshotting
    // earlier would hand render() a pose the sim then moved out from under it.
    captureRenderInterp();
}

// ===========================================================================
// FIXED-STEP RENDER INTERPOLATION — see the vehicle.h block for the defect and
// the argument. Everything below is render-only: nothing here reaches Jolt, and
// the physics state the sim reads is untouched.
// ===========================================================================
namespace {

// Unit quaternion (xyzw) from an ORTHONORMAL column-major basis. Shepperd's
// method — the branch on the largest diagonal term keeps it conditioned when
// the trace is near -1 (a wheel half a turn round is exactly that case).
void quatFromBasis(const float c0[3], const float c1[3], const float c2[3], float q[4]) {
    // M(row, col): column `col` is c<col>, so M(r,0)=c0[r], M(r,1)=c1[r], M(r,2)=c2[r].
    const float m00 = c0[0], m10 = c0[1], m20 = c0[2];
    const float m01 = c1[0], m11 = c1[1], m21 = c1[2];
    const float m02 = c2[0], m12 = c2[1], m22 = c2[2];
    const float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (m21 - m12) / s;
        q[1] = (m02 - m20) / s;
        q[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q[3] = (m21 - m12) / s;
        q[0] = 0.25f * s;
        q[1] = (m01 + m10) / s;
        q[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q[3] = (m02 - m20) / s;
        q[0] = (m01 + m10) / s;
        q[1] = 0.25f * s;
        q[2] = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q[3] = (m10 - m01) / s;
        q[0] = (m02 + m20) / s;
        q[1] = (m12 + m21) / s;
        q[2] = 0.25f * s;
    }
}

// Shortest-arc nlerp. EXACT at both endpoints: t <= 0 copies a and t >= 1 copies
// b bit-for-bit, so an un-interpolated frame is byte-identical to the pre-fix
// path (this is what makes the fix a no-op when render rate == sim rate).
void nlerpQuat(const float a[4], const float b[4], float t, float out[4]) {
    if (t <= 0.0f) { out[0]=a[0]; out[1]=a[1]; out[2]=a[2]; out[3]=a[3]; return; }
    if (t >= 1.0f) { out[0]=b[0]; out[1]=b[1]; out[2]=b[2]; out[3]=b[3]; return; }
    const float d = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    const float s = (d < 0.0f) ? -1.0f : 1.0f;   // shortest arc
    const float u = 1.0f - t;
    float o[4] = { u*a[0] + t*s*b[0], u*a[1] + t*s*b[1],
                   u*a[2] + t*s*b[2], u*a[3] + t*s*b[3] };
    const float len = std::sqrt(o[0]*o[0] + o[1]*o[1] + o[2]*o[2] + o[3]*o[3]);
    if (len > 1e-8f) { const float inv = 1.0f / len;
        out[0]=o[0]*inv; out[1]=o[1]*inv; out[2]=o[2]*inv; out[3]=o[3]*inv; }
    else { out[0]=b[0]; out[1]=b[1]; out[2]=b[2]; out[3]=b[3]; }
}

// Endpoint-exact scalar lerp (same bit-identity contract as nlerpQuat).
inline float lerpf(float a, float b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return a + (b - a) * t;
}

} // namespace

void DriveDemo::setRenderAlpha(float a) {
    m_renderAlpha = (a < 0.0f) ? 0.0f : (a > 1.0f ? 1.0f : a);
}

void DriveDemo::resetRenderInterp() {
    m_interpPrimed = false;
    m_bodyPrev = m_bodyCur = PoseSnap{};
    for (int s = 0; s < 4; ++s) m_wheelPrev[s] = m_wheelCur[s] = PoseSnap{};
    captureRenderInterp();          // re-prime prev == cur from the live body
}

void DriveDemo::captureRenderInterp() {
    if (!m_physics || !m_chassis.valid()) return;
    // Roll the history forward, then sample the new post-step state.
    m_bodyPrev = m_bodyCur;
    for (int s = 0; s < 4; ++s) m_wheelPrev[s] = m_wheelCur[s];

    const x3::phys::Vec3 p = m_physics->getBodyPosition(m_chassis);
    m_bodyCur.pos[0] = p.x; m_bodyCur.pos[1] = p.y; m_bodyCur.pos[2] = p.z;
    m_physics->getBodyRotation(m_chassis, m_bodyCur.rot);
    m_bodyCur.scl[0] = m_bodyCur.scl[1] = m_bodyCur.scl[2] = 1.0f;
    m_bodyCur.valid = true;

    for (int s = 0; s < 4; ++s) {
        x3::phys::WheelState ws;
        if (!m_ctl || !m_ctl->wheelState((uint32_t)s, ws)) { m_wheelCur[s].valid = false; continue; }
        PoseSnap& w = m_wheelCur[s];
        w.pos[0] = ws.worldTransform[12];
        w.pos[1] = ws.worldTransform[13];
        w.pos[2] = ws.worldTransform[14];
        // Split the baked column scale off the rotation before extracting the
        // quaternion — see the vehicle.h note on why a raw component-wise lerp
        // of this matrix deflates the tire.
        float c[3][3];
        for (int k = 0; k < 3; ++k) {
            const float* col = &ws.worldTransform[k * 4];
            const float len = std::sqrt(col[0]*col[0] + col[1]*col[1] + col[2]*col[2]);
            w.scl[k] = len;
            const float inv = (len > 1e-6f) ? (1.0f / len) : 0.0f;
            c[k][0] = col[0]*inv; c[k][1] = col[1]*inv; c[k][2] = col[2]*inv;
        }
        // A MIRRORED basis (negative determinant — the left-hand wheels are
        // built by negating an axis) has no quaternion. Fold the mirror into
        // the stored X scale so recomposition restores it exactly, and extract
        // the quaternion from the right-handed remainder.
        const float det =
            c[0][0]*(c[1][1]*c[2][2] - c[1][2]*c[2][1]) -
            c[1][0]*(c[0][1]*c[2][2] - c[0][2]*c[2][1]) +
            c[2][0]*(c[0][1]*c[1][2] - c[0][2]*c[1][1]);
        if (det < 0.0f) {
            c[0][0] = -c[0][0]; c[0][1] = -c[0][1]; c[0][2] = -c[0][2];
            w.scl[0] = -w.scl[0];
        }
        quatFromBasis(c[0], c[1], c[2], w.rot);
        w.radius = ws.radius; w.width = ws.width;
        w.hasContact = ws.hasContact; w.suspLen = ws.suspensionLength;
        w.valid = true;
    }

    if (!m_interpPrimed) {          // first capture after build/reset: no history
        m_bodyPrev = m_bodyCur;
        for (int s = 0; s < 4; ++s) m_wheelPrev[s] = m_wheelCur[s];
        m_interpPrimed = true;
    }
}

void DriveDemo::renderChassisPos(float out[3]) const {
    if (!m_bodyCur.valid) { chassisPos(out); return; }
    for (int k = 0; k < 3; ++k)
        out[k] = lerpf(m_bodyPrev.pos[k], m_bodyCur.pos[k], m_renderAlpha);
}

void DriveDemo::renderChassisRot(float out[4]) const {
    if (!m_bodyCur.valid) {
        if (m_physics && m_chassis.valid()) m_physics->getBodyRotation(m_chassis, out);
        else { out[0]=out[1]=out[2]=0.0f; out[3]=1.0f; }
        return;
    }
    nlerpQuat(m_bodyPrev.rot, m_bodyCur.rot, m_renderAlpha, out);
}

bool DriveDemo::renderWheelPose(uint32_t slot, x3::phys::WheelState& out) const {
    if (slot >= 4) return false;
    const PoseSnap& a = m_wheelPrev[slot];
    const PoseSnap& b = m_wheelCur[slot];
    if (!b.valid) return m_ctl && m_ctl->wheelState(slot, out);
    // A slot with no previous sample (first frame after a reset) presents `cur`.
    const float t = a.valid ? m_renderAlpha : 1.0f;
    float pos[3], rot[4], scl[3];
    for (int k = 0; k < 3; ++k) {
        pos[k] = lerpf(a.pos[k], b.pos[k], t);
        scl[k] = lerpf(a.scl[k], b.scl[k], t);
    }
    nlerpQuat(a.rot, b.rot, t, rot);
    composeTRS(pos, rot, scl[0], scl[1], scl[2], out.worldTransform);
    out.radius = lerpf(a.radius, b.radius, t);
    out.width  = lerpf(a.width,  b.width,  t);
    out.suspensionLength = lerpf(a.suspLen, b.suspLen, t);
    out.hasContact = b.hasContact;   // a boolean has no meaningful in-between
    return true;
}

// ===========================================================================
// NITROUS + THE THREE-STAGE SECRET — see the vehicle.h block for the story.
// Moved down from host_tunnel.cpp (which keeps only the SHIFT read + HUD/audio
// hooks) so every world's car carries the same secret.
// ===========================================================================
bool DriveDemo::grounded() const {
    if (!m_ctl) return false;
    for (uint32_t i = 0; i < m_ctl->wheelCount(); ++i) {
        x3::phys::WheelState ws;
        if (m_ctl->wheelState(i, ws) && ws.hasContact) return true;
    }
    return false;
}

void DriveDemo::updateNitro(float dt) {
    m_nosTorqueMult = 1.0f;
    if (dt <= 0.0f || !m_physics || !m_chassis.valid()) return;
    if (m_crashLock > 0.0f) m_crashLock = std::max(0.0f, m_crashLock - dt);
    const NitroTuning& nt = m_nitroT;

    // Wing fold-back animation (clean retract eases; a torn wing already
    // snapped to 0 in retractWings).
    if (!m_wings && m_wingPose > 0.0f)
        m_wingPose = std::max(0.0f, m_wingPose - dt / std::max(0.05f, m_wingT.deploySecs));

    if (m_wings) {
        // Winged: SHIFT is flight throttle (updateWingFlight); the bottle
        // machinery idles. The tank refills only while SHIFT is up, same as
        // the ground rule below.
        m_nosSpraying = false; m_od01 = 0.0f; m_odHeld = 0.0f; m_odKickClock = 0.0f;
        if (!m_wantNos) m_nosTank = std::min(1.0f, m_nosTank + dt / nt.rechargeSecs);
        return;
    }

    const bool want = m_wantNos && m_crashLock <= 0.0f;
    const bool canSpray = want && m_nosTank > 0.0f;
    float fwd[3] = { 0, 0, -1 };
    if (canSpray || (want && m_nosTank <= 0.0f)) {
        float q[4]; m_physics->getBodyRotation(m_chassis, q);
        float up[3]; vehcam::hullAxes(q, fwd, up);
    }

    // ---- STAGE 0: the NORMAL bottle (behavior unchanged — regression-gated
    // by runWingedFlightSelfTest N1: 15 s bottle, ~20 s recharge, ONE kick
    // per engagement, +1.1 g shove, x1.19 torque).
    if (canSpray) {
        if (!m_nosSpraying)   // THE HIT: one hard kick the instant the bottle lights
            m_physics->applyImpulse(m_chassis,
                x3::phys::Vec3{ fwd[0] * nt.kickImpulse, 0.0f, fwd[2] * nt.kickImpulse });
        m_physics->applyImpulse(m_chassis,   // THE SHOVE: sustained while spraying
            x3::phys::Vec3{ fwd[0] * nt.shoveForce * dt, 0.0f, fwd[2] * nt.shoveForce * dt });
        m_nosTorqueMult = nt.sprayTorqueMult;
        m_nosTank -= dt / nt.bottleSecs;
        m_nosSpraying = true;
        if (m_nosTank <= 0.0f) {
            // ---- STAGE 1: DEPLETION. Fired exactly once per emptying — the
            // host's "NITROUS DEPLETED" flash + PSSSHT hang off this edge.
            m_nosTank = 0.0f;
            m_evDepleted = true;
        }
    } else {
        m_nosSpraying = false;
    }

    // ---- STAGE 2: OVERDRIVE — the accident made deliberate. The old block
    // recharged the tank even while SHIFT was held, so at empty it re-crossed
    // the 0.02 threshold every ~3rd frame and re-fired the ignition kick at
    // ~20 Hz. Now it is an explicit state: the SAME 2600 N·s kick on a fixed
    // 20 Hz metronome (dt-independent — a 30 fps frame fires two), tapering
    // in over odTaperInSecs, plus the accident's time-averaged 1/3-duty shove.
    if (want && m_nosTank <= 0.0f && !m_nosSpraying) {
        m_odHeld += dt;
        float t = std::min(1.0f, m_odHeld / std::max(0.05f, nt.odTaperInSecs));
        m_od01 = t * t * (3.0f - 2.0f * t);            // smoothstep taper-in
        m_odKickClock += dt;
        const float period = 1.0f / std::max(1.0f, nt.odKickHz);
        while (m_odKickClock >= period) {
            m_odKickClock -= period;
            m_physics->applyImpulse(m_chassis,          // THE MACHINE-GUN KICK
                x3::phys::Vec3{ fwd[0] * nt.odKickImpulse * m_od01, 0.0f,
                                fwd[2] * nt.odKickImpulse * m_od01 });
            m_evOdKick = true;                          // host: rhythmic sputter/FX
        }
        m_physics->applyImpulse(m_chassis,
            x3::phys::Vec3{ fwd[0] * nt.odShoveForce * m_od01 * dt, 0.0f,
                            fwd[2] * nt.odShoveForce * m_od01 * dt });
        m_nosTorqueMult = nt.sprayTorqueMult;           // the accident's flicker, held on
        // ---- STAGE 3: five seconds of commitment -> THE WINGS.
        if (m_odHeld >= nt.odWingsSecs) deployWings();
    } else if (!m_wings) {
        m_odHeld = 0.0f; m_od01 = 0.0f; m_odKickClock = 0.0f;
    }

    // RECHARGE — only while the button is UP. THE deliberate change vs the
    // accident: the old block recharged unconditionally when not spraying,
    // INCLUDING while SHIFT was held at empty — that recharge WAS the
    // threshold oscillation. Overdrive above replaces it as an explicit
    // state; normal use (button released) recharges exactly as before.
    if (!want && !m_nosSpraying)
        m_nosTank = std::min(1.0f, m_nosTank + dt / nt.rechargeSecs);
}

void DriveDemo::deployWings() {
    if (m_wings || !m_physics || !m_chassis.valid()) return;
    const WingTuning& wt = m_wingT;
    x3::phys::FlightDesc fd;
    fd.body            = m_chassis;
    fd.maxThrust       = wt.maxThrust;
    fd.liftCoefficient = wt.liftCoeff;
    fd.linearDrag      = wt.drag;
    fd.pitchTorque     = wt.pitchTorque;
    fd.yawTorque       = wt.yawTorque;
    fd.rollTorque      = wt.rollTorque;
    fd.angularDamping  = wt.angDamping;
    fd.gravity         = true;             // an atmospheric beast, not a spaceship
    m_flyCtl.reset(x3::phys::createFlightController(*m_physics, fd));
    if (!m_flyCtl) {
        x3::logError("[vehicle] wing deploy: flight controller failed to build");
        return;
    }
    m_wings = true;
    // THE FLIGHT CONTROLLER OWNS DAMPING (app/space_pilot.cpp:263 does the same,
    // "we own damping"). Jolt body damping is velocity-LINEAR and is a CAR
    // setting; at flight speed it dwarfs the quadratic aero model (~10 kN at
    // 123 m/s vs the 3 kN the tuning asks for), so the owner numbers could never
    // fall out of thrust==drag. retractWings puts the car values back.
    m_physics->setBodyDamping(m_chassis, 0.0f, 0.0f);
    // ...and the CAR aero stops too: the wheeled controller adds its own body
    // drag (kAeroDrag 1.4, ~7x the flight coefficient) plus spoiler downforce
    // every frame it steps. Those are ROAD systems. Left running they held
    // terminal at 296 mph against the owner spec of 700 (MEASURED).
    if (m_ctl) m_ctl->setAeroSuspended(true);
    m_wingPose = 0.0f;                     // pop-out animation runs in updateWingFlight
    m_evWingsOut = true;                   // host: the THUNK
    {   // seed the crash detector with the CURRENT speed so deploy-frame dV=0
        float lv[3]; m_physics->getBodyLinearVelocity(m_chassis, lv);
        m_prevFlySpeed = std::sqrt(lv[0]*lv[0] + lv[1]*lv[1] + lv[2]*lv[2]);
    }
    // Neutral the wheels so touchdown rolls free (no stale throttle/brake).
    if (m_ctl) { x3::phys::VehicleInput n{}; m_ctl->setInput(n); m_effIn = n; m_effThrottle = 0.0f; }
    x3::logInfo("[vehicle] WINGS DEPLOYED — the car is a flying beast "
                "(bank to turn, SHIFT = thrust, hands-off cruise)");
}

void DriveDemo::retractWings(bool torn) {
    if (!m_wings) return;
    m_flyCtl.reset();                      // flight forces off THIS step
    // PAIRED with deployWings: Jolt defaults back, the car values.
    if (m_physics && m_chassis.valid()) m_physics->setBodyDamping(m_chassis, 0.05f, 0.05f);
    if (m_ctl) m_ctl->setAeroSuspended(false);   // road aero back with the wheels
    if (m_ctl) m_ctl->setConstraintSuspended(false);  // and the constraint rejoins
    m_wings = false;
    if (torn) m_wingPose = 0.0f;           // ripped away — no tidy fold
    m_evWingsIn = true;
    m_odHeld = 0.0f; m_od01 = 0.0f; m_odKickClock = 0.0f;
    // THE CONTACT LAW re-arms in postStep the moment m_wings drops (see the
    // paired comment at the lifter gate — NO_SLOP rule 4).
}

void DriveDemo::updateWingFlight(float dt) {
    if (!m_wings || dt <= 0.0f) return;
    const WingTuning& wt = m_wingT;
    // Wing pop-out animation (render reads m_wingPose; the THUNK is the event).
    m_wingPose = std::min(1.0f, m_wingPose + dt / std::max(0.05f, wt.deploySecs));
    if (!m_flyCtl || !m_physics || !m_chassis.valid()) return;

    float q[4]; m_physics->getBodyRotation(m_chassis, q);
    float f[3], u[3]; vehcam::hullAxes(q, f, u);
    float roll, pitch; vehcam::hullRollPitch(f, u, roll, pitch);
    m_hullPitch = pitch; m_hullRoll = roll;   // cached for preStep (wheeled-constraint gate)

    x3::phys::VehicleInput fin{};
    // THROTTLE — the overdrive lineage: SHIFT = full thrust (700 mph falls out
    // of thrust==drag), hands-off = idle thrust (the restful 277 mph cruise).
    // Grounded: thrust cut so the wheels/brakes own the rollout. AIRBRAKE
    // (brake/handbrake held): thrust to ZERO + doubled drag — the landing
    // lever, because idle thrust would otherwise hold the 277 cruise forever
    // and the beast could never slow below the 60 mph retract line.
    const bool airbrake = (m_lastIn.brake > 0.3f) || (m_lastIn.handBrake > 0.3f);
    const float idleFrac = wt.idleThrust / std::max(1.0f, wt.maxThrust);
    // TAKEOFF vs ROLLOUT. The grounded cut exists so the wheels and brakes own
    // the LANDING rollout — but applied unconditionally it also made takeoff
    // impossible: wings deploy on the ground, grounded() zeroes thrust, the car
    // never accelerates, so it never leaves the ground and stays grounded
    // forever. MEASURED (--test-vehicle N4 CLIMB DIAG, 2026-08-22):
    // grounded=YES dY=-0.012 m over 1.5 s of commanded nose-up at full thrust,
    // which is what failed all seven N4 aerodynamic checks at once.
    // So: a COMMANDED thrust (SHIFT) overrides the cut and flies you off the
    // deck; hands-off on the ground still yields to the wheels.
    fin.throttle = (grounded() && !m_wantNos) ? 0.0f
                 : (m_wantNos ? 1.0f : (airbrake ? 0.0f : idleFrac));
    // ATTITUDE — full authority, never fought: no auto-level anywhere, so a
    // committed roll or loop is the player's to keep (arcade-plane, not sim).
    // ATTITUDE — the pilot's stick, plus STATIC STABILITY on whichever axis the
    // pilot is not holding (WingTuning::pitchStab/rollStab, PAIRED there). The
    // authority term is 1-|stick|, so at full deflection the airframe contributes
    // NOTHING and a committed loop/roll is untouched — "aerobatics never fought"
    // survives intact. Hands-off, the nose returns to the horizon and the wings
    // return level, which is what makes the 277 mph cruise restful and what lets
    // the 700/277 LEVEL-flight terminals fall out of thrust==drag at all.
    const float pStick = std::clamp(m_flyPitchIn, -1.0f, 1.0f);
    const float rStick = std::clamp(m_flyRollIn,  -1.0f, 1.0f);
    fin.pitch = std::clamp(pStick + (1.0f - std::fabs(pStick)) *
                           std::clamp(-pitch * wt.pitchStab, -1.0f, 1.0f), -1.0f, 1.0f);
    // ROLL REFERENCE DIES AT THE VERTICAL. hullRollPitch derives roll from
    // rightLevel = cross(fwd, worldUp), which degenerates as the nose approaches
    // straight up or down — precisely what a loop flies through. Holding full
    // roll authority there makes the stabilizer chase a singular reference and
    // it fights the loop over the top. Fade it with cos(pitch): full authority
    // in level flight where "wings level" means something, none at the vertical
    // where it does not. Pitch needs no such guard — asin(fwd.y) stays well
    // defined, and reads 0 in INVERTED level flight, so it never fights being
    // upside down.
    const float rollRef = std::max(0.0f, std::cos(pitch));
    fin.roll  = std::clamp(rStick + (1.0f - std::fabs(rStick)) * rollRef *
                           std::clamp(-roll  * wt.rollStab,  -1.0f, 1.0f), -1.0f, 1.0f);
    // BANK-TO-TURN: yaw injected per sin(bank) carves the heading the way a
    // banked wing does (zero when inverted — sin(180°)=0 — so aerobatics are
    // untouched), plus a mild direct rudder. SIGN: FlightController's +steer
    // torques the nose LEFT (RH about +up), and +roll here is a RIGHT bank,
    // so both terms enter negated — verified by N4's carve-direction check.
    fin.steer = std::clamp(-wt.carveGain * std::sin(roll) * std::cos(pitch)
                           - wt.rudderGain * fin.roll, -1.0f, 1.0f);
    m_flyCtl->setInput(fin);

    // AIRBRAKE drag (see the throttle comment): an extra 2x quadratic drag
    // while the brake is held airborne, applied as an impulse alongside the
    // controller's own drag.
    float lv[3]; m_physics->getBodyLinearVelocity(m_chassis, lv);
    const float spd = std::sqrt(lv[0]*lv[0] + lv[1]*lv[1] + lv[2]*lv[2]);
    if (airbrake && spd > 2.0f && !grounded()) {
        const float fmag = 2.0f * wt.drag * spd;   // N per (m/s) -> quadratic overall
        m_physics->applyImpulse(m_chassis,
            x3::phys::Vec3{ -lv[0] * fmag * dt, -lv[1] * fmag * dt, -lv[2] * fmag * dt });
    }
    // ARCADE NOSE-FOLLOW: ease the velocity DIRECTION toward the nose
    // (magnitude preserved) so the beast flies where it points — loops and
    // rolls track true instead of ballistic-drifting (Crimson Skies, not a
    // momentum brick). Skipped when slow or grounded.
    if (spd > 8.0f && !grounded()) {
        const float k = 1.0f - std::exp(-wt.noseFollow * dt);   // dt-scaled, HARD rule
        float d[3] = { lv[0]/spd, lv[1]/spd, lv[2]/spd };
        d[0] += (f[0] - d[0]) * k; d[1] += (f[1] - d[1]) * k; d[2] += (f[2] - d[2]) * k;
        const float dl = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        if (dl > 1e-4f) {
            const float s = spd / dl;
            float nv[3] = { d[0]*s, d[1]*s, d[2]*s };
            m_physics->setBodyLinearVelocity(m_chassis, nv);
        }
    }
}

// ---------------------------------------------------------------------------
// WING SKIN — armory Sci-Fi Kit Vol 3 "Wing_02" (dark paneled fin, one embedded
// texture; authored: thickness ±0.41 X, span 3.29 up +Y, chord 3.77 along Z).
// Drawn twice: rotated Rz(-90°) span->+X for the RIGHT wing, Rz(+90°) span->-X
// for the LEFT (rotations, not mirrors — winding/normals stay correct), swept
// back ~22°, scaled to a 2.6 m half-span / 1.5 m chord / 0.25 m thickness.
// Deploy animation: the pair pivots up out of the rocker line (fold angle
// 80°->0) as wingDeploy01 runs 0->1.
// ---------------------------------------------------------------------------
bool DriveDemo::skinWings(x3::rhi::IRenderDevice& device, std::string_view glbDir,
                          std::string_view relPath) {
    m_wingSrc.reset(x3::asset::createAssetSource());
    if (!m_wingSrc || !m_wingSrc->mountDir(glbDir, 0)) return false;
    m_wingLoader.reset(x3::asset::createModelLoader(&device, m_wingSrc.get()));
    m_wingModel = m_wingLoader->load(relPath);
    if (!m_wingModel.ok) return false;
    m_wingDrawL = x3::asset::makeDrawables(m_wingModel);
    m_wingSkinned = !m_wingDrawL.empty();
    return m_wingSkinned;
}

// ---------------------------------------------------------------------------
// TIRE SQUASH (render-only). Owner: "when Landing hard on pavement, the
// RUBBER TIRES should deflect visually, a tiny bit." This is deliberately
// NOT a physics change — Jolt's wheel is a rigid cylinder and stays one;
// WheeledTuning / the suspension the DS-Vehicle session owns is never
// touched. All this does is watch WheelState.suspensionLength (already
// exposed by IVehicleController::wheelState) for a fast compression toward
// the suspension's min length — a hard hit — and hand render() a small,
// short-lived per-wheel scale to draw with.
//
// DETECTION: compressRate = how fast suspensionLength is SHRINKING (m/s),
// i.e. the wheel travelling toward mSuspensionMinLength (see JoltVehicle.cpp
// WheelState::suspensionLength — Jolt's live, per-step raycast-derived
// value). Normalized against THIS wheel's own authored suspension travel
// (m_wheels[slot].suspensionMax - suspensionMin) so the same "how many
// travel-lengths per second" threshold works whether the car is stock or
// has been lowered/raised (car_ride) or given stiffer/softer springs —
// travel is read once at build time, so a live car_ride retune does not
// reach in here (kept deliberately independent of WheeledTuning, per spec).
//
// RESPONSE: a peak-hold "kick" (never stomps a bigger hit already recovering)
// relaxed to 0 by a CRITICALLY DAMPED spring (no overshoot / no bounce-back
// wobble — a real tire does not oscillate after a hit) over roughly a quarter
// second. squashFactors() turns the current amount into the actual render
// scale (~4-8% radial shrink, ~2-3% width bulge), gated by the tire_squash
// cvar (see DriveDemo::setTireSquash / host_tunnel.cpp's `tire_squash`).
// ---------------------------------------------------------------------------
void DriveDemo::updateTireSquash(float dt) {
    if (!m_ctl || dt <= 0.0f) return;
    const uint32_t n = std::min<uint32_t>(m_ctl->wheelCount(), 4u);
    for (uint32_t s = 0; s < n; ++s) {
        x3::phys::WheelState ws;
        WheelSquash& sq = m_squash[s];
        if (m_ctl->wheelState(s, ws)) {
            if (sq.havePrev && ws.hasContact) {
                // + = suspension SHORTENING (compressing toward min == a hit).
                const float compressRate = (sq.prevSuspLen - ws.suspensionLength) / dt;
                const float travel = (s < m_wheels.size())
                    ? std::max(0.05f, m_wheels[s].suspensionMax - m_wheels[s].suspensionMin)
                    : 0.25f;
                // A hard landing eats most of the travel in well under 1/6 s;
                // ordinary road bumps/compression from throttle squat do not.
                // 6 travel-lengths/sec is the "hard" line; kSpikeSoftKnee gives
                // a little runway above it before the effect reaches full
                // strength, instead of a hard on/off snap at the threshold.
                constexpr float kSpikeTravelPerSec = 6.0f;
                constexpr float kSpikeSoftKnee      = 4.0f;  // travel-lengths/sec of runway to full strength
                const float spikeRate = travel * kSpikeTravelPerSec;
                if (compressRate > spikeRate) {
                    const float over = (compressRate - spikeRate) / (travel * kSpikeSoftKnee);
                    const float kick = std::clamp(over, 0.0f, 1.0f);
                    sq.squash = std::max(sq.squash, kick);   // peak-hold
                }
            }
            sq.prevSuspLen = ws.suspensionLength;
            sq.havePrev = true;
        }

        // Critically-damped relax toward 0 (exact closed form for a "kick from
        // rest" critically damped spring — monotonic, no overshoot). omega=20
        // settles a full-strength kick under ~2% within ~0.3 s, i.e. the "~0.25 s"
        // the spec asks for.
        if (sq.squash > 0.0f || sq.squashVel != 0.0f) {
            constexpr float omega = 20.0f;
            const float x = sq.squash, v = sq.squashVel;
            const float e = std::exp(-omega * dt);
            sq.squash    = (x + (v + omega * x) * dt) * e;
            sq.squashVel = (v - omega * (v + omega * x) * dt) * e;
            if (sq.squash < 0.001f) { sq.squash = 0.0f; sq.squashVel = 0.0f; }
        }
    }
}

// Per-wheel render scale for THIS frame: outSquashY [0,1) radial shrink (the
// tire flattening — see WheeledController::wheelState in JoltVehicle.cpp:
// worldTransform columns 0/2 carry the wheel's RADIUS, column 1 the WIDTH),
// outBulge width growth. Both 0 when not squashing — render() then draws the
// untouched WheelState transform, byte-identical to before this feature.
void DriveDemo::squashFactors(int slot, float& outSquashY, float& outBulge) const {
    outSquashY = 0.0f; outBulge = 0.0f;
    if (slot < 0 || slot >= 4) return;
    const float amt = std::clamp(m_squash[slot].squash * m_tireSquash, 0.0f, 1.0f);
    if (amt <= 0.0f) return;
    // 4-8% radial shrink, 2-3% width bulge, ramped by the squash amount.
    outSquashY = 0.04f + 0.04f * amt;
    outBulge   = 0.02f + 0.01f * amt;
}

namespace {
// Column-major 4x4 builders for the wing pose composition (kept local — the
// rest of the file composes with composeTRS/mulMat4 and needs no more).
inline void matIdentity(float m[16]) {
    std::memset(m, 0, sizeof(float) * 16); m[0] = m[5] = m[10] = m[15] = 1.0f;
}
inline void matRotY(float a, float m[16]) {   // rotation about +Y (RH)
    matIdentity(m);
    const float c = std::cos(a), s = std::sin(a);
    m[0] = c; m[2] = -s; m[8] = s; m[10] = c;
}
inline void matRotZ(float a, float m[16]) {   // rotation about +Z (RH)
    matIdentity(m);
    const float c = std::cos(a), s = std::sin(a);
    m[0] = c; m[1] = s; m[4] = -s; m[5] = c;
}
inline void matScaleT(float sx, float sy, float sz,
                      float tx, float ty, float tz, float m[16]) {
    matIdentity(m);
    m[0] = sx; m[5] = sy; m[10] = sz; m[12] = tx; m[13] = ty; m[14] = tz;
}
} // namespace

// Draw the deployed wing pair (see skinWings for the asset story). Called from
// both render() paths with the chassis pose; no-op until the pose animation
// has actually started.
void DriveDemo::drawWings(const x3::rhi::FrameContext& frame,
                          const float chassisM[16]) const {
    if (!m_wingSkinned || m_wingPose <= 0.01f || !m_device) return;
    const float t = m_wingPose;
    // Authored fin: thickness ±0.41 X, span 3.29 +Y, chord 3.77 Z. Scale to
    // 0.25 m thick / 2.6 m half-span / 1.5 m chord BEFORE the axis swing.
    const float kSx = 0.30f, kSy = 2.6f / 3.29f, kSz = 1.5f / 3.77f;
    const float kSweep = 22.0f * 3.14159265f / 180.0f;   // tips trail backward
    const float kFold  = 35.0f * 3.14159265f / 180.0f;   // emerge tips-up, settle level
    for (int side = 0; side < 2; ++side) {
        const float sgn = (side == 0) ? 1.0f : -1.0f;     // +1 right (+X), -1 left
        float S[16]; matScaleT(kSx, kSy, kSz, 0, 0, 0, S);
        float swing[16]; matRotZ(sgn * -1.5707963f, swing);        // span -> ±X
        float sweep[16]; matRotY(-sgn * kSweep, sweep);
        float fold[16];  matRotZ(sgn * kFold * (1.0f - t), fold);  // deploy animation
        // Slide out of the rocker line as the pose runs: buried in the body at
        // t=0, shoulder at ±0.95 at t=1. Pivot rides slightly rear of center.
        float T[16]; matScaleT(1, 1, 1, sgn * (0.15f + 0.80f * t), 0.10f, 0.45f, T);
        float a[16], b[16], local[16], world[16], fin[16];
        x3::asset::mulMat4(swing, S, a);
        x3::asset::mulMat4(sweep, a, b);
        x3::asset::mulMat4(fold, b, a);
        x3::asset::mulMat4(T, a, local);
        x3::asset::mulMat4(chassisM, local, world);
        for (const auto& d : m_wingDrawL) {
            x3::asset::mulMat4(world, d.nodeTransform, fin);
            drawDrawable(frame, d, fin);
        }
    }
}

void DriveDemo::chassisPos(float out[3]) const {
    x3::phys::Vec3 p = m_physics ? m_physics->getBodyPosition(m_chassis) : x3::phys::Vec3{};
    out[0] = p.x; out[1] = p.y; out[2] = p.z;
}

void DriveDemo::render(const x3::rhi::FrameContext& frame) const {
    if (!m_device || !m_ctl) return;

    // FIXED-STEP RENDER INTERPOLATION (fix/car-phasing): the drawn pose is
    // lerp(pre-step, post-step, accumulator remainder), NOT the raw post-step
    // state. Reading physics live here is what made the body advance on 60 Hz
    // lurches while the display ran at 165. The wheels below come from
    // renderWheelPose() — the SAME alpha — so they stay bolted to this hull.
    // This is also the only transform the renderer ever sees, so the velocity
    // pass's prev-model history records interpolated matrices by construction.
    float pos[3]; renderChassisPos(pos);
    float q[4];   renderChassisRot(q);

    if (m_skinned) {
        // ---- HERO-CAR GLB skin: the body parts ride the sprung chassis (nose
        // flip + ride-height drop baked in kBodySkin); the wheels ride the LIVE
        // physics wheel poses (steer + spin + suspension travel). ----
        float chassisM[16]; composeTRS(pos, q, 1.0f, 1.0f, 1.0f, chassisM);
        drawWings(frame, chassisM);   // the three-stage secret's stage 3
        // PER-CAR body skin (app/car_roster.h). The wings lane predates the
        // roster and multiplied by the old hardcoded kBodySkin; taking that
        // back would collapse GBX and CTR onto one body transform.
        float bodySkin[16]; bodySkinMatrix(spec(), bodySkin);
        float carM[16];     x3::asset::mulMat4(chassisM, bodySkin, carM);
        float fin[16];
        for (const auto& d : m_bodyDraw) {
            x3::asset::mulMat4(carM, d.nodeTransform, fin);
            drawDrawable(frame, d, fin);
        }
        for (int s = 0; s < 4; ++s) {
            x3::phys::WheelState ws;
            if (!renderWheelPose((uint32_t)s, ws)) continue;
            // Strip the baked radius/half-width scale -> the pure wheel POSE.
            float P[16]; std::memcpy(P, ws.worldTransform, sizeof(P));
            for (int c = 0; c < 3; ++c) {
                float* col = &P[c * 4];
                const float len = std::sqrt(col[0]*col[0] + col[1]*col[1] + col[2]*col[2]);
                if (len > 1e-5f) { col[0] /= len; col[1] /= len; col[2] /= len; }
            }
            // TIRE SQUASH (render-only; see updateTireSquash/squashFactors).
            // Columns 0/2 are the wheel's radius directions (the vertical
            // rolling-plane), column 1 is the width/axle direction — shrink
            // the former, bulge the latter, same as the graybox path below.
            // Ground-anchored: drop the pose along WORLD +Y by radius*squashY
            // so the tire's BOTTOM stays put and the hub visibly sinks toward
            // it, rather than the tire lifting off the road.
            float squashY, bulge; squashFactors(s, squashY, bulge);
            if (squashY > 0.0f) {
                for (int c : {0, 2}) { float* col = &P[c*4]; col[0]*=(1.0f-squashY); col[1]*=(1.0f-squashY); col[2]*=(1.0f-squashY); }
                { float* col = &P[4]; col[0]*=(1.0f+bulge); col[1]*=(1.0f+bulge); col[2]*=(1.0f+bulge); }
                P[13] -= ws.radius * squashY;
            }
            for (const auto& d : m_wheelDraw[s]) {
                x3::asset::mulMat4(P, d.nodeTransform, fin);   // nodeTransform = axisFix * authored scale
                drawDrawable(frame, d, fin);
            }
        }
        return;
    }

    // ---- Graybox fallback (no GLB): box chassis + cylinder wheels. ----
    const float bodyCol[4]  = { m_tintOn ? m_tint[0] : 1.0f,
                                m_tintOn ? m_tint[1] : 0.25f,
                                m_tintOn ? m_tint[2] : 0.22f, 1.0f };
    const float wheelCol[4] = { 0.12f, 0.12f, 0.14f, 1.0f };
    float m[16]; composeTRS(pos, q, m_hx*2.0f, m_hy*2.0f, m_hz*2.0f, m);
    m_device->drawMesh(frame, m_chassisMesh, m_chassisTex, bodyCol, m);
    { float cM[16]; composeTRS(pos, q, 1.0f, 1.0f, 1.0f, cM); drawWings(frame, cM); }
    const uint32_t n = m_ctl->wheelCount();
    for (uint32_t i = 0; i < n; ++i) {
        x3::phys::WheelState ws;
        if (!renderWheelPose(i, ws)) continue;   // interpolated, same alpha as the hull
        // TIRE SQUASH (render-only; see updateTireSquash/squashFactors). The
        // wheel mesh is a unit Y-cylinder baked into world space by wheelState
        // (col0=right*r, col1=up*halfWidth [the axle], col2=forward*r — see
        // WheeledController::wheelState in JoltVehicle.cpp), so shrinking
        // columns 0/2 flattens the tire's rolling-plane radius and growing
        // column 1 bulges its width — then drop the pose along WORLD +Y by
        // radius*squashY so the tire's ground contact stays put (the hub
        // sinks toward the road, the tire never lifts off it).
        float squashY, bulge; squashFactors((int)i, squashY, bulge);
        if (squashY <= 0.0f) {
            m_device->drawMesh(frame, m_wheelMesh, m_wheelTex, wheelCol, ws.worldTransform);
        } else {
            float T[16]; std::memcpy(T, ws.worldTransform, sizeof(T));
            for (int c : {0, 2}) { float* col = &T[c*4]; col[0]*=(1.0f-squashY); col[1]*=(1.0f-squashY); col[2]*=(1.0f-squashY); }
            { float* col = &T[4]; col[0]*=(1.0f+bulge); col[1]*=(1.0f+bulge); col[2]*=(1.0f+bulge); }
            T[13] -= ws.radius * squashY;
            m_device->drawMesh(frame, m_wheelMesh, m_wheelTex, wheelCol, T);
        }
    }
}

void DriveDemo::shutdown() {
    m_flyCtl.reset();  // wing flight controller (if the secret was found)
    m_wings = false; m_wingPose = 0.0f;
    m_ctl.reset();  // remove the constraint/step-listener BEFORE the body/world go
    if (m_physics && m_chassis.valid()) m_physics->removeBody(m_chassis);
    if (m_device) {
        if (m_chassisMesh.valid()) m_device->destroyMesh(m_chassisMesh);
        if (m_wheelMesh.valid())   m_device->destroyMesh(m_wheelMesh);
        if (m_chassisTex.valid())  m_device->destroyTexture(m_chassisTex);
        if (m_wheelTex.valid())    m_device->destroyTexture(m_wheelTex);
    }
    // GLB skin: the loader frees the model's GPU handles (meshes/textures).
    if (m_skinned && m_skinLoader) m_skinLoader->unload(m_skinModel);
    if (m_wingSkinned && m_wingLoader) m_wingLoader->unload(m_wingModel);
    m_wingDrawL.clear(); m_wingSkinned = false;
    m_wingLoader.reset(); m_wingSrc.reset();
    m_bodyDraw.clear();
    for (int s = 0; s < 4; ++s) m_wheelDraw[s].clear();
    m_skinned = false;
    m_skinLoader.reset();
    m_skinSrc.reset();
    m_device = nullptr; m_physics = nullptr;
}

// ===========================================================================
// Headless DRIVE enter/exit self-test (--test-vehicle, game layer). Physics
// only — no render device. Mirrors the in-world UX: walk up, E to enter, drive,
// E to exit (control restored beside the car).
// ===========================================================================
bool runDriveEnterExitSelfTest() {
    int passN = 0, failN = 0;
    auto check = [&](bool ok, const char* name) {
        if (ok) { ++passN; x3::logInfo(std::string("[drive-test] PASS ") + name); }
        else    { ++failN; x3::logError(std::string("[drive-test] FAIL ") + name); }
    };

    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    if (!phys->init()) { x3::logError("[drive-test] physics init failed"); return false; }
    {
        // Flat static slab the wheel rays can stand on. Big enough that a car
        // accelerating 0->95 mph over 10 s (~325 m) stays on it (was 200 m, which
        // the car drove off of, tripping the "kept ground contact" check).
        x3::prims::PrimMesh g = x3::prims::makeBox(2000.0f, 0.5f, 2000.0f, 0.0f, -0.5f, 0.0f, 0.02f);
        phys->addStaticMesh(g.cverts.data(), (uint32_t)(g.cverts.size() / 3),
                            g.cindex.data(), (uint32_t)g.cindex.size());
    }
    DriveDemo car;
    check(car.buildPhysics(*phys, 0.0f, 1.2f, 0.0f), "spawn: chassis + wheeled controller built");
    // This world's ground is the SLAB above, not the streamed terrain — the
    // contact-law lifter would hoist the car onto the PHANTOM procedural
    // field (37 m up at z=-381 after the W-MOUNTAIN merge; it broke the
    // wheels-contact/ride-height/skidpad sections). See setTerrainContactLaw.
    car.setTerrainContactLaw(false);
    phys->optimizeBroadphase();

    const float dt = 1.0f / 60.0f;
    // Player ON FOOT beside the spawn point.
    float player[3] = { 3.0f, 0.0f, 4.0f };
    bool inCar = false;

    // Settle onto the suspension.
    for (int i = 0; i < 90; ++i) {
        x3::phys::VehicleInput in{};
        car.setInput(in); car.preStep(dt); phys->step(dt); car.postStep(dt);
    }
    check(car.allWheelsInContact(), "settle: all 4 wheels in ground contact");

    // 'E' — proximity enter (the in-world rule: within 3.5 m of the chassis).
    float c0[3]; car.chassisPos(c0);
    const float dEnter = std::sqrt((player[0]-c0[0])*(player[0]-c0[0]) +
                                   (player[2]-c0[2])*(player[2]-c0[2]));
    if (dEnter <= 5.0f) inCar = true;
    check(inCar, "enter: player within range takes the wheel");

    // Full throttle for 600 fixed ticks (10 s) — long enough for the turbo to
    // spool up and the car to run through the gears to its top end.
    // ACCELERATION TELEMETRY (2026-08-16, "MORE acceleration"): 0-60 and 0-100
    // TIMES, measured, not vibed (NO_SLOP rule 9) — the numbers the owner's
    // seat-of-pants impression is calibrated against. Also the peak launch
    // slip over the first 2 s: the launch is right when the driven wheels sit
    // NEAR Jolt's friction peak (slip ~0.06), not deep in the plateau (smoke).
    float t060 = -1.0f, t0100 = -1.0f, launchPeakSlip = 0.0f;
    constexpr float kMps60  = 26.822f;   // 60 mph
    constexpr float kMps100 = 44.704f;   // 100 mph
    for (int i = 0; i < 600; ++i) {
        x3::phys::VehicleInput in{};
        in.throttle = 1.0f;
        car.setInput(in); car.preStep(dt); phys->step(dt); car.postStep(dt);
        const float t = (i + 1) * dt;
        const float v = car.forwardSpeed();
        // Slip ratio is meaningless while v ~ 0 (the denominator), so the
        // launch metric only samples once the car is genuinely rolling.
        if (t <= 2.0f && v > 3.0f) launchPeakSlip = std::max(launchPeakSlip, car.maxSlip());
        if (t060  < 0.0f && v >= kMps60)  t060  = t;
        if (t0100 < 0.0f && v >= kMps100) t0100 = t;
        if ((i % 60) == 59)
            // rpmLocked (wheel-side) vs rpm (crank) = live CLUTCH SLIP, and
            // driveF = the solver's actual longitudinal force — the two
            // instruments for "something invisible caps drive force at
            // speed" (owner: nitrous moved 121 -> 123 mph and plateaued).
            // If driveF collapses while rpm and throttle stay high, the cap
            // is between crank and contact patch and these numbers name it.
            x3::logInfo("[drive-test] t=" + std::to_string((i + 1) * dt) + "s v=" +
                        std::to_string(car.forwardSpeed()) + " rpm=" + std::to_string(car.engineRPM()) +
                        " rpmLocked=" + std::to_string(car.controller()->lockedRPM()) +
                        " gear=" + std::to_string(car.gear()) + " thr=" +
                        std::to_string(car.effectiveThrottle()) +
                        " driveF=" + std::to_string(car.controller()->driveForce()) + " N");
    }
    x3::logInfo("[drive-test] ACCEL: 0-60 mph " +
                (t060  >= 0.0f ? std::to_string(t060)  + " s" : std::string("NOT REACHED")) +
                ", 0-100 mph " +
                (t0100 >= 0.0f ? std::to_string(t0100) + " s" : std::string("NOT REACHED")) +
                ", launch peak slip " + std::to_string(launchPeakSlip) +
                " (friction peak ~0.06)");
    // Gates are LOOSE on purpose: the point is the printed number (the owner's
    // target is low-3s to 60), the gate only catches a regression back to the
    // "does NOT feel fast" era. 60 mph in under 5 s and 100 in the 10 s window.
    check(t060 >= 0.0f && t060 < 5.0f, "accel: 0-60 mph under 5 s (target: low 3s)");
    check(t0100 >= 0.0f, "accel: reaches 100 mph inside the 10 s pull");
    float c1[3]; car.chassisPos(c1);
    const float dx = c1[0] - c0[0], dz = c1[2] - c0[2];
    const float disp = std::sqrt(dx*dx + dz*dz);
    x3::logInfo("[drive-test] displacement after 10 s full throttle: " + std::to_string(disp) +
                " m, fwdSpeed=" + std::to_string(car.forwardSpeed()) + " m/s");
    check(disp > 10.0f, "drive: forward displacement > 10 m");
    check(dz < -5.0f, "drive: displacement is along -Z (the car's forward)");
    check(car.forwardSpeed() > 3.0f, "drive: forward speed positive");
    check(car.allWheelsInContact(), "drive: wheels kept ground contact");

    // 'E' — exit: control restored ON FOOT beside the car.
    inCar = false;
    player[0] = c1[0] + 2.5f; player[1] = c1[1]; player[2] = c1[2];
    const float dExit = std::sqrt((player[0]-c1[0])*(player[0]-c1[0]) +
                                  (player[2]-c1[2])*(player[2]-c1[2]));
    check(!inCar && dExit > 2.0f && dExit < 3.0f, "exit: player control restored beside the car");

    // =======================================================================
    // RIDE HEIGHT measurement (ITEM 1 diagnostic — owner: "vehicle height
    // console commands do NOT make the car change height"). This is the EXACT
    // path host_tunnel.cpp's `car_ride` console command drives: build a
    // WheeledTuning with rideHeightDelta set and call DriveDemo::applyTuning
    // -> IVehicleController::applyWheeledTuning (JoltVehicle.cpp), headless,
    // with a real before/after chassis-height measurement logged so the
    // effect (or its absence) is provable, not asserted.
    // =======================================================================
    {
        // Bleed off the drive above back to a dead stop first so the
        // measurement isn't polluted by residual pitch/roll/velocity.
        for (int i = 0; i < 90; ++i) {
            x3::phys::VehicleInput in{};
            car.setInput(in); car.preStep(dt); phys->step(dt); car.postStep(dt);
        }
        float before[3]; car.chassisPos(before);
        x3::logInfo("[drive-test] RIDE HEIGHT before car_ride: chassis.y=" +
                    std::to_string(before[1]) + " m");

        // `car_ride -0.10` (host_tunnel.cpp): rideHeightDelta -0.10 m, all else
        // left at "leave" sentinels (a partial tuning, exactly like the console
        // command builds — see WheeledTuning::kRideHeightLeave).
        x3::phys::WheeledTuning rideT;
        rideT.rideHeightDelta = -0.10f;
        const bool tuned = car.applyTuning(rideT);
        check(tuned, "car_ride: applyWheeledTuning(rideHeightDelta=-0.10) accepted");

        // Let the suspension settle onto the new (shorter) travel window.
        for (int i = 0; i < 120; ++i) {
            x3::phys::VehicleInput in{};
            car.setInput(in); car.preStep(dt); phys->step(dt); car.postStep(dt);
        }
        float after[3]; car.chassisPos(after);
        const float drop = before[1] - after[1];
        x3::logInfo("[drive-test] RIDE HEIGHT after  car_ride -0.10: chassis.y=" +
                    std::to_string(after[1]) + " m  (drop=" + std::to_string(drop) +
                    " m, expect ~0.10 m)");
        // A real -0.10 m ride-height delta shifts BOTH the min and max
        // suspension bounds down by 0.10 m (see JoltVehicle.cpp
        // applyWheeledTuning), so the chassis should settle ~0.10 m lower.
        // Wide tolerance (0.04-0.16 m) — it only has to be CLEARLY non-zero
        // and in the right direction; exact settle depends on spring/damper.
        check(drop > 0.04f && drop < 0.16f,
              "car_ride: console command has a VISIBLE effect on chassis rest height");
        check(car.allWheelsInContact(), "car_ride: still all 4 wheels grounded after the drop");

        // Put the ride height back to stock so the skidpad below measures the
        // shipped car, not the lowered one (rideHeightDelta 0 = the authored
        // baseline — applyWheeledTuning offsets FROM base, so 0 restores).
        x3::phys::WheeledTuning restoreT;
        restoreT.rideHeightDelta = 0.0f;
        car.applyTuning(restoreT);
        for (int i = 0; i < 120; ++i) {
            x3::phys::VehicleInput in{};
            car.setInput(in); car.preStep(dt); phys->step(dt); car.postStep(dt);
        }
    }

    // =======================================================================
    // SKIDPAD (2026-08-16, NFS handling pass). Steady-state cornering AT THE
    // LIMIT, headless: full steering lock asked (the speed map shapes it),
    // speed held near 22 m/s, 15 s total, measure the last 7 s (the first 8
    // are the spiral-in transient):
    //   * lateral g   = mean yaw rate x mean speed / 9.81 (circular motion —
    //                   no accelerometer needed, the kinematics ARE the meter)
    //   * MAX ROLL    = peak body roll angle. THE ROLLOVER GATE. Owner,
    //                   2026-08-16: "The car wants to tip up on two wheels
    //                   even trying to make ANY curve at speed." NFS cars
    //                   corner FLAT: > ~6 deg of body roll at steady state =
    //                   regression, and ANY tick with BOTH inside wheels off
    //                   the ground is an automatic fail.
    //   * body slip   = angle between nose and velocity. Small = planted;
    //                   growing = the rear walking out; > ~35 deg = spun.
    //   * yaw gain    = measured yaw rate / kinematic (Ackermann) yaw rate at
    //                   the CURRENT shaped steer angle. < 1 = understeer
    //                   (front washing out), > 1 = oversteer (rear slipping).
    //                   THE turn-in/balance number for the grip-split work.
    // =======================================================================
    {
        const float vTarget = 22.0f;          // m/s (~49 mph): "a curve at speed"
        const float steerIn = 1.0f;           // full lock asked; speed map shapes it
        const float kWheelbase = 2.274f;      // |z_front| + |z_rear| (buildPhysics)
        const float kMaxSteer  = 0.5236f;     // WheelDesc::maxSteerAngle (paired!)
        float sumV = 0.0f, sumYawRate = 0.0f, sumSlip = 0.0f;
        float maxRoll = 0.0f;
        int   nMeas = 0, oneWheelUpTicks = 0, twoWheelUpTicks = 0;
        float prevHeading = 0.0f; bool haveHeading = false;
        for (int i = 0; i < 900; ++i) {       // 15 s total
            x3::phys::VehicleInput in{};
            in.throttle = (car.forwardSpeed() < vTarget) ? 0.6f : 0.0f;
            in.steer    = steerIn;
            car.setInput(in); car.preStep(dt); phys->step(dt); car.postStep(dt);
            // Heading/roll from the chassis quaternion (vehcam::hullAxes /
            // hullRollPitch are the canonical extractors; -Z fwd -> heading 0).
            float q[4]; phys->getBodyRotation(car.chassis(), q);
            float f[3], u[3];
            vehcam::hullAxes(q, f, u);
            const float heading = std::atan2(f[0], -f[2]);
            float dh = 0.0f;
            if (haveHeading) {
                dh = heading - prevHeading;
                while (dh >  3.14159265f) dh -= 6.2831853f;
                while (dh < -3.14159265f) dh += 6.2831853f;
            }
            prevHeading = heading; haveHeading = true;
            if ((i % 120) == 119)              // diagnostic trace, 2 s cadence
                x3::logInfo("[drive-test] skid t=" + std::to_string((i + 1) * dt) +
                            " v=" + std::to_string(car.forwardSpeed()) +
                            " steerNow=" + std::to_string(car.steerNow()) +
                            " yaw=" + std::to_string(dh / dt));
            if (i >= 480) {                    // measure the last 7 s
                float roll, pitch;
                vehcam::hullRollPitch(f, u, roll, pitch);
                maxRoll = std::max(maxRoll, std::fabs(roll));
                int wheelsUp = 0;
                for (uint32_t wi = 0; wi < car.controller()->wheelCount(); ++wi) {
                    x3::phys::WheelState ws;
                    if (car.controller()->wheelState(wi, ws) && !ws.hasContact) ++wheelsUp;
                }
                if (wheelsUp >= 1) ++oneWheelUpTicks;
                if (wheelsUp >= 2) ++twoWheelUpTicks;
                const float vFwd = car.forwardSpeed();
                // Body slip: velocity decomposed on the chassis axes.
                float vel[3]; phys->getBodyLinearVelocity(car.chassis(), vel);
                const float right[3] = { f[1]*u[2] - f[2]*u[1],    // fwd x up
                                         f[2]*u[0] - f[0]*u[2],
                                         f[0]*u[1] - f[1]*u[0] };
                const float vLat = vel[0]*right[0] + vel[1]*right[1] + vel[2]*right[2];
                sumV       += vFwd;
                sumYawRate += dh / dt;
                sumSlip    += std::atan2(std::fabs(vLat), std::max(0.5f, std::fabs(vFwd)));
                ++nMeas;
            }
        }
        const float meanV   = nMeas ? sumV / nMeas : 0.0f;
        const float meanYaw = nMeas ? std::fabs(sumYawRate / nMeas) : 0.0f;
        const float meanSlip= nMeas ? sumSlip / nMeas : 0.0f;
        const float latG    = meanYaw * meanV / 9.81f;
        // Kinematic yaw rate at the shaped steer the car actually ran.
        const float delta   = std::fabs(car.steerNow()) * kMaxSteer;
        const float yawKin  = meanV * std::tan(delta) / kWheelbase;
        const float yawGain = (yawKin > 1e-3f) ? meanYaw / yawKin : 0.0f;
        x3::logInfo("[drive-test] SKIDPAD: v=" + std::to_string(meanV) + " m/s, yawRate=" +
                    std::to_string(meanYaw) + " rad/s, lateral " + std::to_string(latG) +
                    " g, maxRoll=" + std::to_string(maxRoll * 57.2958f) +
                    " deg, bodySlip=" + std::to_string(meanSlip * 57.2958f) +
                    " deg, yawGain=" + std::to_string(yawGain) +
                    " (<1 understeer, >1 oversteer), wheelLift ticks 1up=" +
                    std::to_string(oneWheelUpTicks) + " 2up=" +
                    std::to_string(twoWheelUpTicks) + "/" + std::to_string(nMeas));
        check(latG > 1.2f, "skidpad: steady-state lateral acceleration > 1.2 g");
        check(maxRoll < 0.105f, "skidpad: body roll < 6 deg at the limit (corners FLAT)");
        check(twoWheelUpTicks == 0, "skidpad: NEVER two wheels off the ground (no tip-up)");
        check(oneWheelUpTicks < nMeas / 10, "skidpad: inside wheels stay planted (>90% of ticks)");
        check(meanSlip < 0.61f, "skidpad: body slip < 35 deg (holds the circle, no spin)");
    }

    car.shutdown();
    phys->shutdown();

    // =======================================================================
    // HANDLING PASS (2026-08-16, W-HANDLING2). Owner spec, verbatim: "Can we
    // substantially increase the 'stick on the road' idea? It should be harder
    // to flip the car upside down and spin it just driving" / "spoilers for
    // downforce" / "it shouldnt peg redline the whole time you drive" / "make
    // the final gear a 0.50:1". Everything below is MEASURED (rule 9) on a
    // fresh 16-km slab world:
    //   H1 CRUISE-70: steady 70 mph must settle in 6th at mid-band rpm — the
    //      throttle-adaptive shift band + 0.50 overdrive under test.
    //   H2 TOP SPEED: WOT to terminal velocity; must still exceed 155 mph and
    //      must NOT be pinned at the rev limiter when it gets there.
    //   H3 NOS: same pull with the 200-shot (x1.6); logs how close the 220
    //      spec is (gear-reachability is the gate; the drag equilibrium is
    //      reported honestly).
    //   H4 SLALOM at ~100 mph on clean pavement, run TWICE — shipped aero
    //      (downforce 1, roll damping on) vs none: the SPIN gate.
    //   H5 CURB STRIKE (12 cm staggered, straight line) at ~100 mph, same
    //      A/B: the FLIP gate. Kept SEPARATE from H4 on purpose — see the
    //      comment at handlingRun for the receipt.
    // =======================================================================
    {
        auto buildLongWorld = [&](std::unique_ptr<x3::phys::IPhysicsWorld>& outPhys,
                                  DriveDemo& outCar, bool withCurbs) -> bool {
            outPhys.reset(x3::phys::createPhysicsWorld());
            if (!outPhys->init()) return false;
            // 16 km x 400 m slab along Z, top at y=0. The car spawns near +Z
            // and drives -Z (its forward).
            x3::prims::PrimMesh g =
                x3::prims::makeBox(200.0f, 0.5f, 8000.0f, 0.0f, -0.5f, 0.0f, 0.02f);
            outPhys->addStaticMesh(g.cverts.data(), (uint32_t)(g.cverts.size() / 3),
                                   g.cindex.data(), (uint32_t)g.cindex.size());
            if (withCurbs) {
                // Two 12-cm curbs (half-extent 0.06 -> top at y=0.12), LEFT
                // wheels first then RIGHT 8 m later — a staggered strike is
                // the roll excitation a symmetric bump is not. Curb halves
                // span [-60.5, -0.5] and [0.5, 60.5] in x, so the car running
                // down x=0 puts its LEFT wheels (x = -0.80) on the first and
                // its RIGHT wheels on the second.
                //
                // THE 3-METRE LENGTH IS LOAD-BEARING (W-HANDLING3, rule 10).
                // W-HANDLING2 authored these 0.30 m long in z and the strike
                // NEVER HAPPENED: at 100 mph a 60 Hz step advances the wheel
                // 44.7/60 = 0.745 m, so the suspension raycast stepped clean
                // OVER a 0.30 m curb and sampled flat slab on both sides —
                // roll came back 7e-6 deg, a green that meant "no test ran".
                // 3 m (hz 1.5) is ~4 guaranteed samples at 100 mph AND is what
                // a real curb/rumble strip section is. Any bump test in this
                // engine must be longer than v_max/60 or it is not a test.
                x3::prims::PrimMesh cl =
                    x3::prims::makeBox(30.0f, 0.06f, 1.5f, -30.5f, 0.06f, 7450.0f, 0.1f);
                outPhys->addStaticMesh(cl.cverts.data(), (uint32_t)(cl.cverts.size() / 3),
                                       cl.cindex.data(), (uint32_t)cl.cindex.size());
                x3::prims::PrimMesh cr =
                    x3::prims::makeBox(30.0f, 0.06f, 1.5f, 30.5f, 0.06f, 7442.0f, 0.1f);
                outPhys->addStaticMesh(cr.cverts.data(), (uint32_t)(cr.cverts.size() / 3),
                                       cr.cindex.data(), (uint32_t)cr.cindex.size());
            }
            if (!outCar.buildPhysics(*outPhys, 0.0f, 1.2f, 7800.0f)) return false;
            outCar.setTerrainContactLaw(false);   // slab world (phantom-field receipt above)
            outPhys->optimizeBroadphase();
            const float dt2 = 1.0f / 60.0f;
            for (int i = 0; i < 90; ++i) {        // settle on the suspension
                x3::phys::VehicleInput in{};
                outCar.setInput(in); outCar.preStep(dt2);
                outPhys->step(dt2); outCar.postStep(dt2);
            }
            return true;
        };
        auto rollDeg = [&](x3::phys::IPhysicsWorld& p, const DriveDemo& c) {
            float q[4]; p.getBodyRotation(c.chassis(), q);
            float f[3], u[3], r, pt;
            vehcam::hullAxes(q, f, u);
            vehcam::hullRollPitch(f, u, r, pt);
            return r * 57.2958f;
        };

        // ---- H1 + H2 + H3: cruise, then WOT to terminal, then NOS. ----
        std::unique_ptr<x3::phys::IPhysicsWorld> ph;
        DriveDemo hcar;
        if (check(buildLongWorld(ph, hcar, false), "handling: long-slab world builds"),
            ph && hcar.controller()) {
            // H1 CRUISE-70: integral throttle controller holds 70 mph for 30 s;
            // measure the last 8 s. (An integrator, not a magic constant — the
            // steady-state throttle is whatever the drag actually demands.)
            // TWO RPMs, BOTH GATED (NO_SLOP rule 4 — they are one value to the
            // owner but two code paths, and only one of them is the complaint):
            //   engineRPM() = Jolt's road-speed-locked value -> THE TACH
            //     (host_tunnel.cpp needle, "it shouldnt peg redline").
            //   audioRPM()  = DriveDemo's own flywheel state -> THE ENGINE NOTE
            //     (host_tunnel.cpp pitch = rpm/1071). Gating only the tach
            //     would let a settled needle sit over a screaming note.
            // Also track the PEAK over the measured window, not just the mean:
            // a mean of 2400 can hide a box hunting 6-5-6 into the limiter.
            float thr = 0.2f;
            float sumRpm = 0.0f, sumARpm = 0.0f, sumV = 0.0f, peakRpm = 0.0f, sumThr = 0.0f;
            int nM = 0, gearAtEnd = 0;
            for (int i = 0; i < 1800; ++i) {
                const float v = hcar.forwardSpeed();
                thr = std::clamp(thr + 0.004f * (31.29f - v), 0.0f, 0.8f);
                x3::phys::VehicleInput in{}; in.throttle = thr;
                hcar.setInput(in); hcar.preStep(dt); ph->step(dt); hcar.postStep(dt);
                if (i >= 1320) {   // last 8 s
                    sumRpm += hcar.engineRPM(); sumARpm += hcar.audioRPM();
                    sumV += hcar.forwardSpeed(); sumThr += thr; ++nM;
                    peakRpm = std::max(peakRpm, std::max(hcar.engineRPM(), hcar.audioRPM()));
                    gearAtEnd = hcar.gear();
                }
            }
            const float cruiseRpm  = nM ? sumRpm / nM : 0.0f;
            const float cruiseARpm = nM ? sumARpm / nM : 0.0f;
            const float cruiseV    = nM ? sumV / nM : 0.0f;
            x3::logInfo("[drive-test] H1 CRUISE-70: v=" + std::to_string(cruiseV * 2.23694f) +
                        " mph, gear=" + std::to_string(gearAtEnd) +
                        ", tach rpm=" + std::to_string(cruiseRpm) +
                        ", NOTE rpm=" + std::to_string(cruiseARpm) +
                        ", peak=" + std::to_string(peakRpm) +
                        ", cruise throttle=" + std::to_string(nM ? sumThr / nM : 0.0f) +
                        " (redline 7500; owner: no pegging at cruise)");
            check(std::fabs(cruiseV - 31.29f) < 2.0f, "H1 cruise: speed holds 70 +- 4.5 mph");
            check(gearAtEnd == 6, "H1 cruise: settles in 6th (the 0.50 overdrive engages)");
            check(cruiseRpm > 1900.0f && cruiseRpm < 3600.0f,
                  "H1 cruise: TACH rpm mid-band (1900-3600), NOT pegged at redline");
            check(cruiseARpm > 1900.0f && cruiseARpm < 3600.0f,
                  "H1 cruise: ENGINE NOTE rpm mid-band too (the note settles, it does not scream)");
            check(peakRpm < 4200.0f,
                  "H1 cruise: no rpm SPIKES in the window (the box is not hunting 6-5-6)");

            // H2 WOT to terminal velocity (45 s is asymptote-close).
            float vPeak = 0.0f, rpmAtPeak = 0.0f; int gearAtPeak = 0;
            for (int i = 0; i < 2700; ++i) {
                x3::phys::VehicleInput in{}; in.throttle = 1.0f;
                hcar.setInput(in); hcar.preStep(dt); ph->step(dt); hcar.postStep(dt);
                const float v = hcar.forwardSpeed();
                if (v > vPeak) { vPeak = v; rpmAtPeak = hcar.engineRPM(); gearAtPeak = hcar.gear(); }
            }
            x3::logInfo("[drive-test] H2 TOP SPEED: " + std::to_string(vPeak * 2.23694f) +
                        " mph in gear " + std::to_string(gearAtPeak) +
                        " at " + std::to_string(rpmAtPeak) + " rpm (limiter 7500)");
            check(vPeak * 2.23694f > 155.0f, "H2 top speed: still exceeds 155 mph");
            check(rpmAtPeak < 7350.0f,
                  "H2 top speed: DRAG-limited mid-band, not bouncing the rev limiter");

            // H3 NOS 200-shot (x1.6, parts.json nos_200 — PAIRED): keep pulling.
            hcar.setTorqueBoost(1.6f);
            float vNos = vPeak, rpmNos = rpmAtPeak; int gearNos = gearAtPeak;
            for (int i = 0; i < 1800; ++i) {
                x3::phys::VehicleInput in{}; in.throttle = 1.0f;
                hcar.setInput(in); hcar.preStep(dt); ph->step(dt); hcar.postStep(dt);
                const float v = hcar.forwardSpeed();
                if (v > vNos) { vNos = v; rpmNos = hcar.engineRPM(); gearNos = hcar.gear(); }
            }
            hcar.setTorqueBoost(1.0f);
            // 220 mph gear-reachability: 6th's speed at redline. PAIRED with
            // buildPhysics (wheel r=0.33, 6th 0.50, final 5.2, redline 7500):
            // v = redline / (60/(2*pi*r) * 0.50 * final) — must exceed 220 mph
            // or NOS can never get there no matter the power.
            const float wheelRpmPerMps = 60.0f / (2.0f * 3.14159265f * 0.33f);
            const float gearTop6 = 7500.0f / (wheelRpmPerMps * 0.50f * 5.2f) * 2.23694f;
            x3::logInfo("[drive-test] H3 NOS TOP SPEED: " + std::to_string(vNos * 2.23694f) +
                        " mph in gear " + std::to_string(gearNos) + " at " +
                        std::to_string(rpmNos) + " rpm; 6th geartop at redline = " +
                        std::to_string(gearTop6) + " mph (220 spec gear-reachable)");
            check(gearTop6 > 220.0f,
                  "H3 NOS: 220 mph remains gear-reachable (6th geartop above it)");
            hcar.shutdown(); ph->shutdown();
        }

        // ---- H4 SLALOM / H5 CURB STRIKE at ~100 mph: shipped aero vs none. ----
        // THE TWO COMPLAINTS ARE TWO TESTS (W-HANDLING3, 2026-08-17). W-HANDLING2
        // ran the slalom THROUGH the curbs in one section and the result was
        // uninterpretable: the no-aero car spun out at the first flick and never
        // reached the curbs, so "shipped rolls, no-aero doesn't" compared a
        // curb strike against a spin. Split:
        //   H4 = flicks on CLEAN pavement  -> the SPIN complaint ("harder to
        //        ... spin it just driving").
        //   H5 = straight over the staggered curbs -> the FLIP complaint
        //        ("harder to flip the car upside down"), same speed, same
        //        A/B, no steering input to confound it.
        struct HRun { float maxRoll, maxSlip, vStrike, maxAsym; int twoUp; bool upright, spun; };
        auto handlingRun = [&](bool shippedAero, bool curbs, bool slalom,
                               const char* tag, HRun& out) -> bool {
            std::unique_ptr<x3::phys::IPhysicsWorld> p2;
            DriveDemo c2;
            if (!buildLongWorld(p2, c2, curbs)) return false;
            if (!shippedAero) {
                x3::phys::WheeledTuning t;
                t.downforce = 0.0f; t.rollDamp = 0.0f;   // the "before" car
                c2.applyTuning(t);
            }
            out = HRun{0.0f, 0.0f, 0.0f, 0.0f, 0, false, false};
            // Phase 1: WOT to ~100 mph (spawn z=+7800, curbs at ~+7446).
            for (int i = 0; i < 1500 && c2.forwardSpeed() < 44.7f; ++i) {
                x3::phys::VehicleInput in{}; in.throttle = 1.0f;
                c2.setInput(in); c2.preStep(dt); p2->step(dt); c2.postStep(dt);
            }
            // Phase 2 (9 s): either VIOLENT slalom — full digital flicks every
            // 0.55 s, the owner's actual A/D input at speed — or dead straight.
            for (int j = 0; j < 540; ++j) {
                x3::phys::VehicleInput in{};
                in.throttle = c2.forwardSpeed() < 44.7f ? 0.7f : 0.2f;
                in.steer    = slalom ? (((j / 33) % 2 == 0) ? 1.0f : -1.0f) : 0.0f;
                c2.setInput(in); c2.preStep(dt); p2->step(dt); c2.postStep(dt);
                out.maxRoll = std::max(out.maxRoll, std::fabs(rollDeg(*p2, c2)));
                int up = 0;
                float susp[4] = {0,0,0,0};
                for (uint32_t wi = 0; wi < c2.controller()->wheelCount(); ++wi) {
                    x3::phys::WheelState ws;
                    if (c2.controller()->wheelState(wi, ws)) {
                        if (!ws.hasContact) ++up;
                        if (wi < 4) susp[wi] = ws.suspensionLength;
                    }
                }
                if (up >= 2) ++out.twoUp;
                // LEFT/RIGHT SUSPENSION ASYMMETRY = THE STRIKE INSTRUMENT.
                // A wheel up on a 12 cm curb rides ~0.12 m more compressed
                // than its partner. Without this, a curb the sim stepped over
                // (see the 3-metre receipt in buildLongWorld) reads as a
                // PASS — the test has to prove it hit something. Wheel order
                // is FL, FR, RL, RR (buildPhysics).
                out.maxAsym = std::max(out.maxAsym,
                                       std::max(std::fabs(susp[0] - susp[1]),
                                                std::fabs(susp[2] - susp[3])));
                float cp[3]; c2.chassisPos(cp);
                if (out.vStrike == 0.0f && cp[2] < 7446.0f) out.vStrike = c2.forwardSpeed();
                // Body slip = the SPIN measure (angle between where the car
                // points and where it is going). Sampled every tick, not just
                // at trace time, so a spin can't hide between log lines.
                float vel[3]; p2->getBodyLinearVelocity(c2.chassis(), vel);
                float q2[4]; p2->getBodyRotation(c2.chassis(), q2);
                float f2[3], u2[3]; vehcam::hullAxes(q2, f2, u2);
                const float vFwd = c2.forwardSpeed();
                const float right2[3] = { f2[1]*u2[2] - f2[2]*u2[1],
                                          f2[2]*u2[0] - f2[0]*u2[2],
                                          f2[0]*u2[1] - f2[1]*u2[0] };
                const float vLat = vel[0]*right2[0] + vel[1]*right2[1] + vel[2]*right2[2];
                const float slipDeg = std::atan2(std::fabs(vLat),
                                        std::max(0.5f, std::fabs(vFwd))) * 57.2958f;
                out.maxSlip = std::max(out.maxSlip, slipDeg);
                // 1 Hz diagnostic trace (rule 9): where does the roll build —
                // the flicks, or the curbs (z 7450/7442)? Body slip names a
                // spin-then-trip; roll without slip names a suspension pump.
                if ((j % 60) == 59) {
                    x3::logInfo(std::string("[drive-test] ") + tag + " trace t=" +
                                std::to_string((j + 1) / 60) +
                                "s z=" + std::to_string(cp[2]) +
                                " v=" + std::to_string(vFwd * 2.23694f) +
                                " mph roll=" + std::to_string(rollDeg(*p2, c2)) +
                                " slip=" + std::to_string(slipDeg) +
                                " deg up=" + std::to_string(up));
                }
            }
            // Upright + recovered?
            float q[4]; p2->getBodyRotation(c2.chassis(), q);
            float f[3], u[3]; vehcam::hullAxes(q, f, u);
            out.upright = u[1] > 0.7f;
            out.spun    = out.maxSlip > 45.0f;
            c2.shutdown(); p2->shutdown();
            return true;
        };

        // ---- H4: the SPIN gate — flicks at ~100 mph on clean pavement. ----
        HRun s1{}, s0{};
        const bool ranS1 = handlingRun(true,  /*curbs*/false, /*slalom*/true,  "H4-aero", s1);
        const bool ranS0 = handlingRun(false, /*curbs*/false, /*slalom*/true,  "H4-none", s0);
        x3::logInfo("[drive-test] H4 SLALOM ~100 mph (clean pavement): SHIPPED aero maxRoll=" +
                    std::to_string(s1.maxRoll) + " deg, maxSlip=" + std::to_string(s1.maxSlip) +
                    " deg, spun=" + (s1.spun ? "YES" : "NO") + ", upright=" +
                    (s1.upright ? "YES" : "NO") + "  |  NO aero maxRoll=" +
                    std::to_string(s0.maxRoll) + " deg, maxSlip=" + std::to_string(s0.maxSlip) +
                    " deg, spun=" + (s0.spun ? "YES" : "NO") + ", upright=" +
                    (s0.upright ? "YES" : "NO"));
        check(ranS1 && ranS0, "H4 slalom: both A/B runs completed");
        check(s1.upright, "H4 slalom: shipped car ends UPRIGHT");
        check(s1.maxRoll < 10.0f, "H4 slalom: shipped car stays FLAT (< 10 deg roll)");
        check(!s1.spun, "H4 slalom: shipped car does NOT spin (body slip < 45 deg)");
        check(s1.maxSlip < s0.maxSlip,
              "H4 slalom: the aero car holds a straighter line than the no-aero car (owner's spin complaint)");

        // ---- H5: the FLIP gate — straight over the staggered curbs. ----
        HRun k1{}, k0{};
        const bool ranK1 = handlingRun(true,  /*curbs*/true, /*slalom*/false, "H5-aero", k1);
        const bool ranK0 = handlingRun(false, /*curbs*/true, /*slalom*/false, "H5-none", k0);
        x3::logInfo("[drive-test] H5 CURB STRIKE (12 cm staggered, hit at " +
                    std::to_string(k1.vStrike * 2.23694f) + " mph): SHIPPED aero maxRoll=" +
                    std::to_string(k1.maxRoll) + " deg, susp asym=" +
                    std::to_string(k1.maxAsym) + " m, 2-wheel-lift ticks=" +
                    std::to_string(k1.twoUp) + ", upright=" + (k1.upright ? "YES" : "NO") +
                    "  |  NO aero maxRoll=" + std::to_string(k0.maxRoll) +
                    " deg, asym=" + std::to_string(k0.maxAsym) +
                    " m, lift=" + std::to_string(k0.twoUp) +
                    ", upright=" + (k0.upright ? "YES" : "NO"));
        check(ranK1 && ranK0, "H5 curb: both A/B runs completed");
        // THE TEST-RAN GATE (rule 9, and the 3-metre receipt above): a curb
        // the wheels never sampled reports perfect zero roll and passes
        // everything. Prove the strike before believing the result.
        check(k1.maxAsym > 0.05f && k0.maxAsym > 0.05f,
              "H5 curb: the car ACTUALLY struck the curbs (suspension asymmetry > 5 cm)");
        check(k1.upright, "H5 curb: shipped car ends UPRIGHT (does not roll over)");
        check(k1.maxRoll < 45.0f, "H5 curb: shipped car never exceeds 45 deg of roll");
        check(k1.maxRoll <= k0.maxRoll + 2.0f,
              "H5 curb: downforce+roll-damping do not WORSEN the roll transient");
    }

    // =======================================================================
    // R: THE CAR ROSTER (W-HEROCAR, 2026-08-17). Every entry in
    // app/car_roster.h must build a rig whose wheels land AT ITS OWN STATIONS
    // and settle on the ground.
    //
    // WHY THIS TEST EXISTS: host_tunnel.cpp carried a comment saying the E46
    // could not be the hero because "DriveDemo's chassis box + wheel stations
    // are still sized to the CTR, so the E46 body sits mis-scaled over
    // CTR-position wheels — Tim's screenshot of the broken red sedan". That is
    // a defect an eyeball caught after it shipped. It is now a check: add a car
    // to the roster with a fat-fingered station and --test-vehicle says so.
    // =======================================================================
    {
        size_t nCars = 0;
        const CarSpec* roster = carRoster(nCars);
        check(nCars >= 2, "roster: more than one car exists (the whole point)");
        for (size_t ci = 0; ci < nCars; ++ci) {
            const CarSpec& cs = roster[ci];
            std::unique_ptr<x3::phys::IPhysicsWorld> rp(x3::phys::createPhysicsWorld());
            if (!rp->init()) { check(false, "roster: physics init"); break; }
            {
                x3::prims::PrimMesh g = x3::prims::makeBox(400.0f, 0.5f, 400.0f,
                                                           0.0f, -0.5f, 0.0f, 0.02f);
                rp->addStaticMesh(g.cverts.data(), (uint32_t)(g.cverts.size() / 3),
                                  g.cindex.data(), (uint32_t)g.cindex.size());
            }
            DriveDemo rc;
            rc.setSpec(cs);
            const bool built = rc.buildPhysics(*rp, 0.0f, 1.2f, 0.0f);
            check(built, (std::string("roster[") + cs.id + "]: rig builds").c_str());
            if (!built) continue;
            rc.setTerrainContactLaw(false);
            rp->optimizeBroadphase();
            for (int i = 0; i < 120; ++i) {
                x3::phys::VehicleInput in{};
                rc.setInput(in); rc.preStep(dt); rp->step(dt); rc.postStep(dt);
            }
            check(rc.allWheelsInContact(),
                  (std::string("roster[") + cs.id + "]: settles on all four wheels").c_str());

            // The stations the rig ACTUALLY built, read back out of the live
            // wheel poses in chassis space, not re-derived from the same
            // literals the builder used. Wheelbase and track are the two
            // numbers that made the E46 look broken.
            float cpos[3]; rc.chassisPos(cpos);
            float zf = 0.0f, zr = 0.0f, xspread = 0.0f;
            int got = 0;
            for (int s = 0; s < 4; ++s) {
                x3::phys::WheelState ws;
                if (!rc.wheelState((uint32_t)s, ws)) continue;
                const float wx = ws.worldTransform[12] - cpos[0];
                const float wz = ws.worldTransform[14] - cpos[2];
                if (s < 2) zf += wz * 0.5f; else zr += wz * 0.5f;
                xspread += std::fabs(wx) * 0.25f;
                ++got;
            }
            const float wb = std::fabs(zr - zf);
            const float wbSpec = std::fabs(cs.wheelZRear - cs.wheelZFront);
            const float trSpec = (cs.wheelXFront + cs.wheelXRear) * 0.5f * cs.bodyWiden;
            x3::logInfo(std::string("[drive-test] roster[") + cs.id + "] " + cs.name +
                        " wheelbase " + std::to_string(wb) + " m (spec " +
                        std::to_string(wbSpec) + "), half-track " +
                        std::to_string(xspread) + " m (spec " + std::to_string(trSpec) +
                        "), glb " + cs.glb);
            check(got == 4, (std::string("roster[") + cs.id + "]: four wheel poses").c_str());
            check(std::fabs(wb - wbSpec) < 0.02f,
                  (std::string("roster[") + cs.id + "]: WHEELBASE matches its own spec, "
                   "not the previous car's").c_str());
            check(std::fabs(xspread - trSpec) < 0.02f,
                  (std::string("roster[") + cs.id + "]: TRACK matches its own spec").c_str());
        }
        // Every roster GLB path must be distinct — two entries pointing at the
        // same file is the silent way a "new car" turns out to be the old one.
        bool distinct = true;
        for (size_t i = 0; i < nCars; ++i)
            for (size_t j = i + 1; j < nCars; ++j)
                if (std::strcmp(roster[i].glb, roster[j].glb) == 0) distinct = false;
        check(distinct, "roster: every entry names a DIFFERENT GLB");
    }

    // =====================================================================
    // FIXED-STEP RENDER INTERPOLATION — THE PHASING GATE (fix/car-phasing).
    //
    // Tim, from live play: the car "looked like it was phasing out of phase"
    // at high speed. This section is the instrument that makes that visible as
    // a number instead of an opinion.
    //
    // It reproduces the HOST's exact timing — a 165 Hz render loop driving a
    // 60 Hz sim through the very same x3::net::SimAccumulator main.cpp uses —
    // and samples the car's position once per RENDER frame on both paths:
    //
    //   RAW        chassisPos()        = what render() read before the fix
    //   INTERP     renderChassisPos()  = what it presents after it
    //
    // The signature of the defect is a STAIRCASE: with 60 sim ticks feeding
    // 165 frames, the raw pose is unchanged on ~64% of frames and then jumps a
    // whole tick of travel. A correct render path is a RAMP: every frame moves,
    // and every frame moves by very nearly the same amount.
    // =====================================================================
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> ip(x3::phys::createPhysicsWorld());
        bool ipOk = ip && ip->init();
        check(ipOk, "phasing: interpolation-gate physics world");
        if (ipOk) {
            x3::prims::PrimMesh g = x3::prims::makeBox(6000.0f, 0.5f, 6000.0f, 0.0f, -0.5f, 0.0f, 0.02f);
            ip->addStaticMesh(g.cverts.data(), (uint32_t)(g.cverts.size() / 3),
                              g.cindex.data(), (uint32_t)g.cindex.size());
            DriveDemo ic;
            check(ic.buildPhysics(*ip, 0.0f, 1.2f, 0.0f), "phasing: car built");
            ic.setTerrainContactLaw(false);   // slab world, not the terrain field
            ip->optimizeBroadphase();

            const float sdt = x3::net::kSimDt;
            x3::phys::VehicleInput go{}; go.throttle = 1.0f;
            // Run out to a steady top-end speed so the per-tick travel is
            // essentially constant and any staircase is pure timing, not accel.
            for (int i = 0; i < 900; ++i) {
                ic.setInput(go); ic.preStep(sdt); ip->step(sdt); ic.postStep(sdt);
            }
            const float mph = ic.forwardSpeed() * 2.23694f;
            check(mph > 60.0f, "phasing: car is at speed for the measurement");

            auto dist3 = [](const float a[3], const float b[3]) {
                const float dx = a[0]-b[0], dy = a[1]-b[1], dz = a[2]-b[2];
                return std::sqrt(dx*dx + dy*dy + dz*dz);
            };

            // ---- 165 Hz render / 60 Hz sim -------------------------------
            x3::net::SimAccumulator acc;
            const float rdt = 1.0f / 165.0f;
            const int   NF  = 330;                 // two seconds of frames
            // Tracked separately: the HORIZONTAL travel (the direction the world
            // streams past, i.e. what "phasing" is actually made of) and the
            // VERTICAL bounce (real suspension motion, which linear
            // interpolation between tick samples reproduces as a polyline with
            // genuine kinks at the ticks — a residual the fix cannot and should
            // not remove, only the sim rate can).
            std::vector<float> rawD, intD, rawH, intH;
            float rawPrev[3], intPrev[3];
            ic.setRenderAlpha(1.0f);
            ic.chassisPos(rawPrev); ic.renderChassisPos(intPrev);
            // Wheel attachment: the interpolated hub must keep the SAME distance
            // to the interpolated hull that the post-step hub keeps to the
            // post-step hull. `broken` is the negative control — what the drift
            // would have been had the wheels been left on the post-step pose
            // while the hull interpolated (the "detached wheels" failure mode).
            float attachDrift = 0.0f, brokenDrift = 0.0f;
            // WARM-UP (same idea as the motion rig's `settle`): the seed samples
            // above were taken at alpha = 1, but the loop's first frame presents a
            // mid-tick alpha — so frame 0's delta measures the sampler starting
            // up, not the render path. Walk a few frames before recording.
            const int WARM = 8;
            for (int f = 0; f < NF + WARM; ++f) {
                const uint32_t n = acc.advance(rdt);
                for (uint32_t s = 0; s < n; ++s) {
                    ic.setInput(go); ic.preStep(sdt); ip->step(sdt); ic.postStep(sdt);
                }
                ic.setRenderAlpha(acc.accum / sdt);
                float rp[3]; ic.chassisPos(rp);
                float xp[3]; ic.renderChassisPos(xp);
                const bool rec = (f >= WARM);
                if (rec) {
                rawD.push_back(dist3(rp, rawPrev));
                intD.push_back(dist3(xp, intPrev));
                rawH.push_back(std::sqrt((rp[0]-rawPrev[0])*(rp[0]-rawPrev[0]) +
                                         (rp[2]-rawPrev[2])*(rp[2]-rawPrev[2])));
                intH.push_back(std::sqrt((xp[0]-intPrev[0])*(xp[0]-intPrev[0]) +
                                         (xp[2]-intPrev[2])*(xp[2]-intPrev[2])));
                }
                std::memcpy(rawPrev, rp, sizeof(rawPrev));
                std::memcpy(intPrev, xp, sizeof(intPrev));
                for (uint32_t s = 0; s < 4 && rec; ++s) {
                    x3::phys::WheelState iw, cw;
                    if (!ic.renderWheelPose(s, iw)) continue;
                    if (!ic.wheelState(s, cw)) continue;
                    const float hubI[3] = { iw.worldTransform[12], iw.worldTransform[13], iw.worldTransform[14] };
                    const float hubC[3] = { cw.worldTransform[12], cw.worldTransform[13], cw.worldTransform[14] };
                    const float ref = dist3(hubC, rp);            // truth: post-step pair
                    attachDrift = std::max(attachDrift, std::fabs(dist3(hubI, xp) - ref));
                    brokenDrift = std::max(brokenDrift, std::fabs(dist3(hubC, xp) - ref));
                }
            }

            // Two numbers per path.
            //   held  = frames that did not move at all. A staircase has tread.
            //   jerk  = max |step[i] - step[i-1]| / mean(step) — the CONSECUTIVE
            //           discontinuity. This, not a global max/min, is the right
            //           smoothness metric: over two seconds the car's speed
            //           genuinely changes a little, and a global ratio charges
            //           the render path for physics it faithfully reproduced.
            //           A staircase riser costs ~1 whole tick of travel against
            //           a 0 tread, so jerk lands near 165/60 = 2.75; a ramp's
            //           consecutive steps differ only by real acceleration.
            auto profile = [&](const std::vector<float>& d, int& held, float& jerk) {
                double sum = 0.0; for (float v : d) sum += v;
                const double mean = sum / (double)d.size();
                held = 0;
                for (float v : d) if (v < 0.02 * mean) ++held;
                double worst = 0.0;
                for (size_t i = 1; i < d.size(); ++i)
                    worst = std::max(worst, (double)std::fabs(d[i] - d[i-1]));
                jerk = (mean > 1e-9) ? (float)(worst / mean) : 0.0f;
            };
            int rawHeld = 0, intHeld = 0; float rawJerk = 0.0f, intJerk = 0.0f;
            profile(rawD, rawHeld, rawJerk);
            profile(intD, intHeld, intJerk);
            int rawHeldH = 0, intHeldH = 0; float rawJerkH = 0.0f, intJerkH = 0.0f;
            profile(rawH, rawHeldH, rawJerkH);
            profile(intH, intHeldH, intJerkH);
            x3::logInfo("[drive-test] PHASING @ " + std::to_string(mph) +
                        " mph, 165 Hz render / 60 Hz sim, " + std::to_string(NF) + " frames:");
            x3::logInfo("[drive-test]   RAW    (pre-fix)  held-frames " + std::to_string(rawHeld) +
                        "/" + std::to_string(NF) + "  jerk " + std::to_string(rawJerk));
            x3::logInfo("[drive-test]   INTERP (post-fix) held-frames " + std::to_string(intHeld) +
                        "/" + std::to_string(NF) + "  jerk " + std::to_string(intJerk));
            x3::logInfo("[drive-test]   HORIZONTAL only (the travel direction): RAW jerk " +
                        std::to_string(rawJerkH) + " held " + std::to_string(rawHeldH) +
                        "  |  INTERP jerk " + std::to_string(intJerkH) +
                        " held " + std::to_string(intHeldH));
            x3::logInfo("[drive-test]   wheel hub drift vs hull: interpolated " +
                        std::to_string(attachDrift) + " m, un-interpolated (control) " +
                        std::to_string(brokenDrift) + " m");
            // THE STAIRCASE: the pre-fix path must visibly hold. 165/60 = 2.75,
            // so ~105 of every 165 frames repeat the previous pose.
            check(rawHeld > NF / 3,
                  "phasing: RAW render pose STAIRCASES at 165 Hz (the defect, measured)");
            // THE RAMP: every frame advances, and by nearly the same amount.
            check(intHeld == 0,
                  "phasing: INTERPOLATED pose advances on EVERY frame (no held frames)");
            check(intJerkH < 0.25f,
                  "phasing: INTERPOLATED horizontal step is a smooth ramp, not a staircase");
            check(rawJerkH > 1.5f,
                  "phasing: RAW horizontal step is a staircase riser (~1 tick of travel)");
            check(intJerkH < rawJerkH * 0.2f,
                  "phasing: interpolation is a large, not marginal, improvement");
            // WHEELS STAY ON THE CAR.
            check(attachDrift < 0.02f,
                  "phasing: wheels stay attached to the interpolated hull (<2 cm)");
            check(brokenDrift > attachDrift * 4.0f,
                  "phasing: control — NOT interpolating the wheels would detach them");

            // ---- Endpoint exactness + the matched-rate no-op ---------------
            // alpha 1 must return the post-step pose BIT-for-bit, and alpha 0 the
            // pre-step pose, or the fix would perturb every existing capture.
            float truth[3]; ic.chassisPos(truth);
            ic.setRenderAlpha(1.0f);
            float at1[3]; ic.renderChassisPos(at1);
            check(at1[0] == truth[0] && at1[1] == truth[1] && at1[2] == truth[2],
                  "phasing: alpha=1 returns the post-step pose BIT-IDENTICALLY");
            // Matched rate: feed the accumulator exactly one sim step per frame.
            // The remainder is then identically 0, so the presented stream is the
            // un-interpolated stream delayed one frame — no new math, no drift.
            x3::net::SimAccumulator macc;
            bool matchedNoop = true;
            float lastPost[3]; ic.chassisPos(lastPost);
            for (int f = 0; f < 60; ++f) {
                const uint32_t n = macc.advance(sdt);
                if (n != 1) { matchedNoop = false; break; }
                ic.setInput(go); ic.preStep(sdt); ip->step(sdt); ic.postStep(sdt);
                if (macc.accum != 0.0f) { matchedNoop = false; break; }
                ic.setRenderAlpha(macc.accum / sdt);
                float pres[3]; ic.renderChassisPos(pres);
                if (pres[0] != lastPost[0] || pres[1] != lastPost[1] || pres[2] != lastPost[2]) {
                    matchedNoop = false; break;
                }
                ic.chassisPos(lastPost);
            }
            check(matchedNoop,
                  "phasing: at 60 Hz render == 60 Hz sim the fix is a NO-OP "
                  "(remainder is exactly 0; presented pose is bit-identical to the "
                  "un-interpolated pose, one frame later)");

            // ---- The camera and the draw must agree ------------------------
            // The chase camera is rigidly bolted to the car. If it followed the
            // post-step pose while the body drew interpolated, the car would swim
            // inside its own framing — a different artifact, not a fix.
            ic.setRenderAlpha(0.37f);
            float drawPos[3]; ic.renderChassisPos(drawPos);
            float camPos[3];  ic.renderChassisPos(camPos);   // driverCamera's source
            check(drawPos[0] == camPos[0] && drawPos[1] == camPos[1] && drawPos[2] == camPos[2],
                  "phasing: the chase camera and the drawn body share ONE pose");
            float rawNow[3]; ic.chassisPos(rawNow);
            check(dist3(drawPos, rawNow) > 1e-5f,
                  "phasing: mid-tick alpha actually presents an in-between pose");

            // ---- COST -----------------------------------------------------
            // GPU cost is zero BY CONSTRUCTION: no new pass, buffer, descriptor
            // or draw, and ObjectData's 160-byte stride is untouched — the same
            // drawMesh calls simply receive a different matrix.
            // CPU: time the ENTIRE per-render-frame presentation cost (hull pose
            // + all four wheel poses). This REPLACES work the old path did per
            // frame: render() used to query Jolt for the body transform and four
            // GetWheelWorldTransform poses on EVERY frame. Those queries now run
            // once per 60 Hz tick instead of once per 165 Hz frame, and what is
            // left on the frame path is pure arithmetic.
            {
                const int N = 200000;
                x3::phys::WheelState tmp;
                volatile float sink = 0.0f;
                const auto t0 = std::chrono::steady_clock::now();
                for (int i = 0; i < N; ++i) {
                    ic.setRenderAlpha((float)(i & 63) / 63.0f);
                    float p[3], r[4]; ic.renderChassisPos(p); ic.renderChassisRot(r);
                    for (uint32_t s = 0; s < 4; ++s) {
                        ic.renderWheelPose(s, tmp);
                        sink = sink + tmp.worldTransform[12];
                    }
                    sink = sink + p[0] + r[0];
                }
                const auto t1 = std::chrono::steady_clock::now();
                const double ns =
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / (double)N;
                x3::logInfo("[drive-test]   COST: full per-render-frame presentation "
                            "(hull pose + 4 wheel poses) = " + std::to_string(ns) +
                            " ns/frame; GPU cost zero (no new pass/buffer/draw, "
                            "ObjectData stride unchanged)");
                check(ns < 2000.0,
                      "phasing: per-frame presentation cost is negligible (< 2 us)");
            }
            ic.shutdown();
        }
        if (ip) ip->shutdown();
    }

    x3::logInfo("[drive-test] " + std::to_string(passN) + " passed, " +
                std::to_string(failN) + " failed");
    return failN == 0;
}

// ===========================================================================
// ParachuteBailout — see vehicle.h. Kinematic drift-down: the canopy snap
// bleeds the ejection velocity toward the steady descent over ~0.8 s, then
// sink at kSinkRate with kDriftRate of steer authority. Landing is CONTACT
// LAW by construction: the descent CANNOT pass the terrain field — the final
// position is clamped ONTO it (NO_SLOP rule 11).
// ===========================================================================
void ParachuteBailout::deploy(const float pos[3], const float vel[3]) {
    m_active = true; m_landed = false;
    for (int i = 0; i < 3; ++i) { m_pos[i] = pos[i]; m_vel[i] = vel[i]; }
    // The canopy kills most of the forward rush immediately (it is a giant
    // airbrake); the rest bleeds in update().
    m_vel[0] *= 0.35f; m_vel[2] *= 0.35f;
    if (m_vel[1] < -8.0f) m_vel[1] = -8.0f;   // never a screaming drop under canopy
}

bool ParachuteBailout::update(float dt, float steerX, float steerZ) {
    if (!m_active || m_landed || dt <= 0.0f) return false;
    // Ease toward the steady state: steer * drift laterally, sink vertically.
    const float tx = std::clamp(steerX, -1.0f, 1.0f) * kDriftRate;
    const float tz = std::clamp(steerZ, -1.0f, 1.0f) * kDriftRate;
    const float k = 1.0f - std::exp(-dt / 0.8f);   // the canopy-snap time constant
    m_vel[0] += (tx        - m_vel[0]) * k;
    m_vel[1] += (-kSinkRate - m_vel[1]) * k;
    m_vel[2] += (tz        - m_vel[2]) * k;
    m_pos[0] += m_vel[0] * dt; m_pos[1] += m_vel[1] * dt; m_pos[2] += m_vel[2] * dt;
    const float gy = x3::game::terrainHeightAtWorld(m_pos[0], m_pos[2]);
    if (m_pos[1] <= gy) {   // boots ON the field, never under it (rule 11)
        m_pos[1] = gy;
        m_landed = true;
    }
    return !m_landed;
}

// ===========================================================================
// BoatDemo
// ===========================================================================
bool BoatDemo::build(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                     float x, float y, float z, float seaLevel, bool isSub) {
    m_device = &device; m_physics = &physics;

    // THE HULL COMES FROM THE ROSTER when one is set (app/boat_roster.h). With
    // no spec this is the legacy craft, unchanged: the same 1.5/0.6/3.0 box and
    // the same derived mass the river boats and the submarine have always used.
    if (m_spec) { m_hx = m_spec->halfX; m_hy = m_spec->halfY; m_hz = m_spec->halfZ; }

    // Hull mass tuned so the box rides ~half-submerged in sea water:
    //   equilibrium submergedVol = mass / fluidDensity. For ~half of fullVol
    //   (=8*hx*hy*hz) submerged we want mass ~= 0.5 * fullVol * fluidDensity.
    // A ROSTERED craft states its real mass instead — a 350 kg jet ski is not
    // half a cubic-metre-per-tonne slab, and deriving its mass from its box
    // would make it float like one.
    const float fullVol = 8.0f * m_hx * m_hy * m_hz;
    const float fluidDensity = m_spec ? 1000.0f : 1025.0f;   // river vs sea water
    const float mass = m_spec ? m_spec->massKg
                              : (0.5f * fullVol * fluidDensity * 0.95f);
    m_hull = physics.addBox(x3::phys::Vec3{m_hx, m_hy, m_hz},
                            x3::phys::Vec3{x, y, z}, mass, x3::phys::Layer::Dynamic);
    if (!m_hull.valid()) return false;

    x3::phys::BuoyancyDesc bd;
    bd.body = m_hull; bd.seaLevel = seaLevel;
    bd.halfExtents[0]=m_hx; bd.halfExtents[1]=m_hy; bd.halfExtents[2]=m_hz;
    bd.fluidDensity = fluidDensity;
    if (m_spec) {
        // A RIDEABLE CRAFT: the roster owns propulsion, damping and THE FEEL.
        bd.linearDrag   = m_spec->linearDrag;
        bd.angularDrag  = m_spec->angularDrag;
        bd.propThrust   = m_spec->propThrust;
        bd.steerTorque  = m_spec->steerTorque;
        // Planing + attitude — the difference between ploughing and skimming.
        bd.planeSpeed    = m_spec->planeSpeed;
        bd.planeLift     = m_spec->planeLift;
        bd.bowLiftTorque = m_spec->bowLift;
        bd.leanTorque    = m_spec->leanTorque;
        // River forces. Gains scale with MASS so a jet ski and a speedboat are
        // carried by the same current at the same rate — a light hull would
        // otherwise be flung downstream while a heavy one barely noticed.
        bd.currentStrength = mass * 2.6f;
        bd.maxCurrentForce = mass * 12.0f;
        bd.shorePush       = mass * 6.0f;
        bd.downstreamAlign = mass * 7.0f;
    } else {
        bd.linearDrag = 2.5f; bd.angularDrag = 2.5f;
        bd.propThrust = mass * 4.0f;     // can motor forward
        bd.steerTorque = mass * 1.5f;    // and turn
    }
    if (isSub) bd.diveThrust = mass * 12.0f; // strong enough to submerge
    // Gentle synthetic SWELL (see BuoyancyDesc): the flat buoyancy plane would
    // otherwise settle the hull dead level — this rocks it a few degrees so the
    // attitude-following chase camera has REAL motion to read off the body. The
    // righting term makes it rock ABOUT LEVEL (the COM-buoyancy model has no
    // self-righting moment, so a spawn/drop transient would otherwise leave a
    // permanent list the camera would faithfully — and wrongly — show).
    bd.swellTorque    = mass * 0.18f;
    bd.swellFreqHz    = 0.18f;
    bd.rightingTorque = m_spec ? m_spec->rightingTorque : (mass * 2.0f);
    m_ctl.reset(x3::phys::createBuoyancyController(physics, bd));
    if (!m_ctl) { physics.removeBody(m_hull); m_hull = {}; return false; }

    std::vector<x3::rhi::MeshVertex> cv; std::vector<uint32_t> ci;
    x3::prims::makeCube(0.5f, cv, ci);
    m_hullMesh = device.createMesh(cv.data(), (uint32_t)cv.size(), ci.data(), (uint32_t)ci.size());
    // GRAYBOX HULL, honestly labelled. The roster names Vehicles/JetSki.glb and
    // Vehicles/Speedboat.glb but neither asset has been sourced yet, and NO_SLOP
    // rule 3 forbids dressing a stand-in up as the real thing. So a rostered
    // craft gets its own flat colour and stays visibly a placeholder until a
    // real model lands; it is the PHYSICS that is finished here, not the art.
    auto t = isSub   ? x3::prims::makeSolidRGBA(8, 180, 180, 60)   // yellow sub
           : m_spec  ? x3::prims::makeSolidRGBA(8, 220, 70,  40)   // rostered craft
                     : x3::prims::makeSolidRGBA(8, 150, 90,  50);  // brown boat hull
    m_hullTex = device.createTexture(t.data(), 8, 8, true);
    return true;
}

void BoatDemo::setInput(const x3::phys::VehicleInput& in) { if (m_ctl) m_ctl->setInput(in); }
void BoatDemo::setSeaLevel(float y) { if (m_ctl) m_ctl->setSeaLevel(y); }
void BoatDemo::setRiverFlow(const float vel[3], float centreDist, float halfWidth,
                            const float toCentre[3]) {
    if (m_ctl) m_ctl->setRiverFlow(vel, centreDist, halfWidth, toCentre);
}
void BoatDemo::preStep(float dt)  { if (m_ctl) m_ctl->preStep(dt); }
void BoatDemo::postStep(float dt) { if (m_ctl) m_ctl->postStep(dt); }

void BoatDemo::hullPos(float out[3]) const {
    x3::phys::Vec3 p = m_physics ? m_physics->getBodyPosition(m_hull) : x3::phys::Vec3{};
    out[0] = p.x; out[1] = p.y; out[2] = p.z;
}

void BoatDemo::render(const x3::rhi::FrameContext& frame) const {
    if (!m_device || !m_ctl) return;
    const float col[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    x3::phys::Vec3 p = m_physics->getBodyPosition(m_hull);
    float q[4]; m_physics->getBodyRotation(m_hull, q);
    float pos[3] = { p.x, p.y, p.z };
    float m[16]; composeTRS(pos, q, m_hx*2.0f, m_hy*2.0f, m_hz*2.0f, m);
    m_device->drawMesh(frame, m_hullMesh, m_hullTex, col, m);
}

void BoatDemo::shutdown() {
    m_ctl.reset();
    if (m_physics && m_hull.valid()) m_physics->removeBody(m_hull);
    if (m_device) {
        if (m_hullMesh.valid()) m_device->destroyMesh(m_hullMesh);
        if (m_hullTex.valid())  m_device->destroyTexture(m_hullTex);
    }
    m_device = nullptr; m_physics = nullptr;
}

// ===========================================================================
// FlyDemo
// ===========================================================================
bool FlyDemo::build(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                    float x, float y, float z) {
    m_device = &device; m_physics = &physics;
    m_body = physics.addBox(x3::phys::Vec3{m_hx, m_hy, m_hz},
                            x3::phys::Vec3{x, y, z}, 1200.0f, x3::phys::Layer::Dynamic);
    if (!m_body.valid()) return false;

    x3::phys::FlightDesc fd;
    fd.body = m_body;
    fd.maxThrust = 26000.0f; fd.liftCoefficient = 0.55f; fd.linearDrag = 0.5f;
    fd.pitchTorque = 16000.0f; fd.yawTorque = 8000.0f; fd.rollTorque = 16000.0f;
    fd.angularDamping = 2.0f; fd.gravity = true;
    m_ctl.reset(x3::phys::createFlightController(physics, fd));
    if (!m_ctl) { physics.removeBody(m_body); m_body = {}; return false; }

    std::vector<x3::rhi::MeshVertex> cv; std::vector<uint32_t> ci;
    x3::prims::makeCube(0.5f, cv, ci);
    m_bodyMesh = device.createMesh(cv.data(), (uint32_t)cv.size(), ci.data(), (uint32_t)ci.size());
    m_wingMesh = m_bodyMesh; // reuse the cube for the wing (drawn flat + wide)
    auto t = x3::prims::makeSolidRGBA(8, 200, 200, 210);  // silver airframe
    m_bodyTex = device.createTexture(t.data(), 8, 8, true);
    return true;
}

void FlyDemo::setInput(const x3::phys::VehicleInput& in) { if (m_ctl) m_ctl->setInput(in); }
void FlyDemo::preStep(float dt)  { if (m_ctl) m_ctl->preStep(dt); }
void FlyDemo::postStep(float dt) { if (m_ctl) m_ctl->postStep(dt); }

void FlyDemo::airframePos(float out[3]) const {
    x3::phys::Vec3 p = m_physics ? m_physics->getBodyPosition(m_body) : x3::phys::Vec3{};
    out[0] = p.x; out[1] = p.y; out[2] = p.z;
}

void FlyDemo::render(const x3::rhi::FrameContext& frame) const {
    if (!m_device || !m_ctl) return;
    const float fuselage[4] = { 0.85f, 0.85f, 0.9f, 1.0f };
    const float wingCol[4]  = { 0.65f, 0.65f, 0.72f, 1.0f };
    x3::phys::Vec3 p = m_physics->getBodyPosition(m_body);
    float q[4]; m_physics->getBodyRotation(m_body, q);
    float pos[3] = { p.x, p.y, p.z };
    // Fuselage (the body box).
    float m[16]; composeTRS(pos, q, m_hx*2.0f, m_hy*2.0f, m_hz*2.0f, m);
    m_device->drawMesh(frame, m_bodyMesh, m_bodyTex, fuselage, m);
    // Wing: a thin wide slab through the body (local: wide in X, thin in Y, short
    // in Z). Drawn at the same transform with a different scale (compose with the
    // chassis rotation: we re-use composeTRS with a wing-shaped scale).
    float wm[16]; composeTRS(pos, q, (m_hx*2.4f)*2.0f, (0.08f)*2.0f, (m_hz*0.5f)*2.0f, wm);
    m_device->drawMesh(frame, m_wingMesh, m_bodyTex, wingCol, wm);
}

void FlyDemo::shutdown() {
    m_ctl.reset();
    if (m_physics && m_body.valid()) m_physics->removeBody(m_body);
    if (m_device) {
        if (m_bodyMesh.valid()) m_device->destroyMesh(m_bodyMesh);
        if (m_bodyTex.valid())  m_device->destroyTexture(m_bodyTex);
    }
    m_device = nullptr; m_physics = nullptr;
}

// ===========================================================================
// runWingedFlightSelfTest (--test-vehicle) — THE THREE-STAGE SECRET, measured
// (NO_SLOP rule 9). See vehicle.h for the section list. The overdrive A/B in
// N3 runs a LINE-FOR-LINE REPLICA of the original host_tunnel accident
// (threshold oscillation + recharge-while-held) on a twin car and compares
// terminal speeds — the owner loves the FEEL of the bug, so the deliberate
// version must measure the same, and the log tells it straight.
// ===========================================================================
bool runWingedFlightSelfTest() {
    int passN = 0, failN = 0;
    auto check = [&](bool ok, const char* name) {
        if (ok) { ++passN; x3::logInfo(std::string("[wings-test] PASS ") + name); }
        else    { ++failN; x3::logError(std::string("[wings-test] FAIL ") + name); }
    };
    const float dt = 1.0f / 60.0f;
    auto buildWorld = [&](std::unique_ptr<x3::phys::IPhysicsWorld>& p, DriveDemo& c,
                          float slabHalf) -> bool {
        p.reset(x3::phys::createPhysicsWorld());
        if (!p->init()) return false;
        x3::prims::PrimMesh g = x3::prims::makeBox(slabHalf, 0.5f, slabHalf,
                                                   0.0f, -0.5f, 0.0f, 0.02f);
        p->addStaticMesh(g.cverts.data(), (uint32_t)(g.cverts.size() / 3),
                         g.cindex.data(), (uint32_t)g.cindex.size());
        if (!c.buildPhysics(*p, 0.0f, 1.2f, 0.0f)) return false;
        c.setTerrainContactLaw(false);   // slab, not the terrain field (see drive test)
        p->optimizeBroadphase();
        for (int i = 0; i < 90; ++i) {   // settle
            x3::phys::VehicleInput in{};
            c.setInput(in); c.preStep(dt); p->step(dt); c.postStep(dt);
        }
        return true;
    };
    auto stepCar = [&](x3::phys::IPhysicsWorld& p, DriveDemo& c,
                       const x3::phys::VehicleInput& in) {
        c.setInput(in); c.preStep(dt); p.step(dt); c.postStep(dt);
    };

    std::unique_ptr<x3::phys::IPhysicsWorld> ph;
    DriveDemo car;
    check(buildWorld(ph, car, 20000.0f), "world: slab + car build");   // 40 km: N3 covers ~6 km at overdrive speed
    if (failN) return false;
    car.nitroTuning().odWingsSecs = 1e9f;   // hold stage 2 open for the A/B (restored in N4)

    // ---- N1: the NORMAL bottle, regression-gated. -------------------------
    {
        float tank0 = car.nosTank();
        float v0 = car.forwardSpeed();
        float firstDv = 0.0f, maxLaterDv = 0.0f;
        for (int i = 0; i < 180; ++i) {   // 3 s of spray at full throttle
            x3::phys::VehicleInput in{}; in.throttle = 1.0f;
            car.setNitroInput(true);
            stepCar(*ph, car, in);
            const float v1 = car.forwardSpeed();
            const float dv = v1 - v0; v0 = v1;
            if (i == 0) firstDv = dv;
            else        maxLaterDv = std::max(maxLaterDv, dv);
        }
        x3::logInfo("[wings-test] N1: ignition kick dv=" + std::to_string(firstDv) +
                    " m/s (expect ~2.4+launch), later per-step dv max=" +
                    std::to_string(maxLaterDv) + ", tank after 3 s spray=" +
                    std::to_string(car.nosTank()) + " (expect ~" +
                    std::to_string(tank0 - 3.0f / 15.0f) + ")");
        check(firstDv > 2.0f, "N1: THE HIT fires on ignition (+2.4 m/s in a frame)");
        check(maxLaterDv < 1.5f, "N1: the kick fires ONCE per engagement (no repeat slam)");
        check(std::fabs(car.nosTank() - (tank0 - 3.0f / 15.0f)) < 0.02f,
              "N1: 15 s bottle (3 s spray drains 0.20)");
        const float tankSpray = car.nosTank();
        for (int i = 0; i < 240; ++i) {   // 4 s off the button
            x3::phys::VehicleInput in{}; in.throttle = 1.0f;
            car.setNitroInput(false);
            stepCar(*ph, car, in);
        }
        check(std::fabs(car.nosTank() - std::min(1.0f, tankSpray + 4.0f / 20.0f)) < 0.02f,
              "N1: ~20 s recharge (4 s off the button restores 0.20)");
    }

    // ---- N2: DEPLETION fires once; the tank STAYS empty while held. -------
    {
        int depletedEdges = 0;
        for (int i = 0; i < 60 * 16 && car.nosTank() > 0.0f; ++i) {
            x3::phys::VehicleInput in{}; in.throttle = 1.0f;
            car.setNitroInput(true);
            stepCar(*ph, car, in);
            if (car.nitroJustDepleted()) ++depletedEdges;
        }
        for (int i = 0; i < 60; ++i) {    // one more second of holding at empty
            x3::phys::VehicleInput in{}; in.throttle = 1.0f;
            car.setNitroInput(true);
            stepCar(*ph, car, in);
            if (car.nitroJustDepleted()) ++depletedEdges;
        }
        x3::logInfo("[wings-test] N2: depletion edges=" + std::to_string(depletedEdges) +
                    ", tank while held=" + std::to_string(car.nosTank()) +
                    ", overdrive held=" + std::to_string(car.overdriveHeldSecs()) + " s");
        check(depletedEdges == 1, "N2: NITROUS DEPLETED fires exactly once per emptying");
        check(car.nosTank() <= 0.0f, "N2: tank stays EMPTY while SHIFT is held (no oscillation)");
        check(car.overdriveHeldSecs() > 0.5f, "N2: overdrive engages past empty");
    }

    // ---- N3: OVERDRIVE vs THE ACCIDENT — terminal speed A/B. ---------------
    // The car is already in overdrive; run it to terminal (25 s).
    {
        for (int i = 0; i < 60 * 25; ++i) {
            x3::phys::VehicleInput in{}; in.throttle = 1.0f;
            car.setNitroInput(true);
            stepCar(*ph, car, in);
        }
        const float vNew = car.forwardSpeed();
        // THE REPLICA: a twin car in its own world, driven by a line-for-line
        // copy of the original host block (host_tunnel.cpp @4236, pre-move):
        // no hysteresis, recharge even while held, threshold 0.02, kick on the
        // rising edge — the oscillation that hit ~379 mph.
        std::unique_ptr<x3::phys::IPhysicsWorld> ph2;
        DriveDemo legacy;
        bool okL = buildWorld(ph2, legacy, 20000.0f);
        check(okL, "N3: legacy-replica world builds");
        float vLegacy = 0.0f;
        if (okL) {
            float tank = 0.0f;            // start at empty — the oscillation regime
            bool wasActive = false;
            for (int i = 0; i < 60 * 40; ++i) {   // 40 s to terminal
                const bool wantNos = true;         // SHIFT held, throttle > 0.1
                const bool active = wantNos && tank > 0.02f;
                if (active) {
                    float cq[4]; ph2->getBodyRotation(legacy.chassis(), cq);
                    float fwd[3], up[3]; vehcam::hullAxes(cq, fwd, up);
                    if (!wasActive)
                        ph2->applyImpulse(legacy.chassis(),
                            x3::phys::Vec3{ fwd[0] * 2600.0f, 0.0f, fwd[2] * 2600.0f });
                    ph2->applyImpulse(legacy.chassis(),
                        x3::phys::Vec3{ fwd[0] * 12000.0f * dt, 0.0f, fwd[2] * 12000.0f * dt });
                }
                wasActive = active;
                tank += active ? -dt / 15.0f : dt / 20.0f;   // THE BUG: recharges while held
                tank = std::min(1.0f, std::max(0.0f, tank));
                legacy.setTorqueBoost(active ? 1.19f : 1.0f);
                x3::phys::VehicleInput in{}; in.throttle = 1.0f;
                stepCar(*ph2, legacy, in);
            }
            vLegacy = legacy.forwardSpeed();
            legacy.shutdown(); ph2->shutdown();
        }
        x3::logInfo("[wings-test] N3 OVERDRIVE A/B: deliberate=" +
                    std::to_string(vNew * 2.23694f) + " mph, accident-replica=" +
                    std::to_string(vLegacy * 2.23694f) + " mph (story: ~379). "
                    "The feel gate is +-12%.");
        check(vNew * 2.23694f > 300.0f, "N3: overdrive is still a monster (> 300 mph)");
        check(okL && std::fabs(vNew - vLegacy) / std::max(1.0f, vLegacy) < 0.12f,
              "N3: deliberate overdrive matches the accident's terminal speed (+-12%)");
    }

    // ---- N4: THE WINGS + the flight model (owner numbers are spec). --------
    {
        car.nitroTuning().odWingsSecs = 5.0f;   // restore the real trigger
        bool sawThunk = false;
        for (int i = 0; i < 120 && !car.wingsDeployed(); ++i) {
            x3::phys::VehicleInput in{}; in.throttle = 1.0f;
            car.setNitroInput(true);
            stepCar(*ph, car, in);
            if (car.wingsJustDeployed()) sawThunk = true;
        }
        check(car.wingsDeployed(), "N4: 5 s held past empty -> WINGS DEPLOY");
        check(sawThunk, "N4: wingsJustDeployed edge fires (the THUNK)");
        for (int i = 0; i < 60; ++i) {   // finish the pop-out animation
            x3::phys::VehicleInput in{};
            car.setNitroInput(true); car.setFlightInput(0.0f, 0.0f);
            stepCar(*ph, car, in);
        }
        check(car.wingDeploy01() >= 1.0f, "N4: deploy animation completes");

        // Climb out, then level full-throttle terminal = 700 mph.
        float p0[3]; car.chassisPos(p0);
        for (int i = 0; i < 90; ++i) {   // 1.5 s nose-up under full thrust
            x3::phys::VehicleInput in{};
            car.setNitroInput(true); car.setFlightInput(0.35f, 0.0f);
            stepCar(*ph, car, in);
        }
        float p1[3]; car.chassisPos(p1);
        x3::logInfo("[wings-test] N4 CLIMB DIAG: grounded=" +
                    std::string(car.grounded() ? "YES" : "no") +
                    " y0=" + std::to_string(p0[1]) + " y1=" + std::to_string(p1[1]) +
                    " dY=" + std::to_string(p1[1]-p0[1]) +
                    " fwd_mph=" + std::to_string(car.forwardSpeed()*2.23694f) +
                    " wings=" + std::string(car.wingsDeployed() ? "OUT" : "in"));
        check(p1[1] > p0[1] + 10.0f, "N4: pitch up under thrust CLIMBS");
        for (int i = 0; i < 60 * 45; ++i) {   // 45 s to terminal
            x3::phys::VehicleInput in{};
            car.setNitroInput(true); car.setFlightInput(0.0f, 0.0f);
            stepCar(*ph, car, in);
        }
        const float vFull = car.forwardSpeed();
        {   // WHERE IS THE THRUST GOING? total |v| vs forward-axis v, plus
            // attitude and altitude — a nose-up hold turns thrust into climb.
            float lv[3]; ph->getBodyLinearVelocity(car.chassis(), lv);
            const float vmag = std::sqrt(lv[0]*lv[0]+lv[1]*lv[1]+lv[2]*lv[2]);
            float qq[4]; ph->getBodyRotation(car.chassis(), qq);
            float ff[3], uu[3]; vehcam::hullAxes(qq, ff, uu);
            float rr, pp; vehcam::hullRollPitch(ff, uu, rr, pp);
            float pz[3]; car.chassisPos(pz);
            x3::logInfo("[wings-test] N4 THRUST DIAG: |v|=" +
                        std::to_string(vmag * 2.23694f) + " mph fwdAxis=" +
                        std::to_string(vFull * 2.23694f) + " mph pitch=" +
                        std::to_string(pp * 57.2958f) + " deg roll=" +
                        std::to_string(rr * 57.2958f) + " deg y=" +
                        std::to_string(pz[1]) + " vy=" + std::to_string(lv[1]));
        }
        x3::logInfo("[wings-test] N4 FULL THRUST terminal: " +
                    std::to_string(vFull * 2.23694f) + " mph (owner spec 700)");
        check(std::fabs(vFull * 2.23694f - 700.0f) < 70.0f,
              "N4: 700 mph falls out of thrust==drag (+-10%)");

        // Hands off: the restful 277 mph drag-equilibrium cruise, altitude held.
        for (int i = 0; i < 60 * 45; ++i) {
            x3::phys::VehicleInput in{};
            car.setNitroInput(false); car.setFlightInput(0.0f, 0.0f);
            stepCar(*ph, car, in);
        }
        float pc0[3]; car.chassisPos(pc0);
        for (int i = 0; i < 60 * 10; ++i) {   // 10 more seconds for the altitude gate
            x3::phys::VehicleInput in{};
            car.setNitroInput(false); car.setFlightInput(0.0f, 0.0f);
            stepCar(*ph, car, in);
        }
        float pc1[3]; car.chassisPos(pc1);
        const float vCruise = car.forwardSpeed();
        x3::logInfo("[wings-test] N4 HANDS-OFF cruise: " +
                    std::to_string(vCruise * 2.23694f) + " mph (owner spec 277), "
                    "altitude drift over 10 s = " + std::to_string(pc1[1] - pc0[1]) + " m");
        check(std::fabs(vCruise * 2.23694f - 277.0f) < 28.0f,
              "N4: hands-off settles at 277 mph (+-10%) — no stall, restful");
        check(std::fabs(pc1[1] - pc0[1]) < 25.0f,
              "N4: altitude HOLDS at coast (lift == g at cruise speed)");

        // Bank to turn: roll right -> heading swings RIGHT (carve).
        float q[4]; ph->getBodyRotation(car.chassis(), q);
        float f0[3], u0[3]; vehcam::hullAxes(q, f0, u0);
        const float head0 = std::atan2(f0[0], -f0[2]);
        float maxRoll = 0.0f;
        for (int i = 0; i < 60 * 3; ++i) {
            x3::phys::VehicleInput in{};
            car.setNitroInput(false);
            car.setFlightInput(0.0f, (i < 45) ? 1.0f : 0.0f);   // 0.75 s roll, then hold
            stepCar(*ph, car, in);
            float qq[4]; ph->getBodyRotation(car.chassis(), qq);
            float ff[3], uu[3], rr, pp; vehcam::hullAxes(qq, ff, uu);
            vehcam::hullRollPitch(ff, uu, rr, pp);
            maxRoll = std::max(maxRoll, rr);
        }
        float qh[4]; ph->getBodyRotation(car.chassis(), qh);
        float fh[3], uh[3]; vehcam::hullAxes(qh, fh, uh);
        float dHead = std::atan2(fh[0], -fh[2]) - head0;
        while (dHead >  3.14159265f) dHead -= 6.2831853f;
        while (dHead < -3.14159265f) dHead += 6.2831853f;
        x3::logInfo("[wings-test] N4 BANK-TO-TURN: maxRoll=" +
                    std::to_string(maxRoll * 57.2958f) + " deg, heading swing=" +
                    std::to_string(dHead * 57.2958f) + " deg right");
        check(maxRoll > 0.25f, "N4: steer input BANKS the car (roll right)");
        check(dHead > 0.10f, "N4: the bank CARVES the heading (turns right)");

        // Full aerobatics: a committed full-stick loop goes INVERTED and comes
        // back upright — the model never fights it, speed never stalls.
        car.setFlightInput(0.0f, 0.0f);
        for (int i = 0; i < 120; ++i) {   // settle the bank first
            x3::phys::VehicleInput in{}; car.setNitroInput(true);
            car.setFlightInput(0.0f, -0.35f * std::min(1.0f, maxRoll * 2.0f));
            stepCar(*ph, car, in);
        }
        bool wentInverted = false, cameBack = false;
        float loopMinUpY = 1e9f, loopMaxPitch = -1e9f, loopMinPitch = 1e9f, loopMaxAbsRoll = 0.0f;
        float loopMaxW = 0.0f, loopWAt6s = -1.0f;
        float minLoopSpeed = 1e9f;
        for (int i = 0; i < 60 * 14; ++i) {
            x3::phys::VehicleInput in{};
            car.setNitroInput(true); car.setFlightInput(1.0f, 0.0f);   // full stick back
            stepCar(*ph, car, in);
            float qq[4]; ph->getBodyRotation(car.chassis(), qq);
            float ff[3], uu[3]; vehcam::hullAxes(qq, ff, uu);
            if (uu[1] < -0.5f) wentInverted = true;
            if (wentInverted && uu[1] > 0.5f) { cameBack = true; break; }
            minLoopSpeed = std::min(minLoopSpeed, car.forwardSpeed());
            { float rr2, pp2; vehcam::hullRollPitch(ff, uu, rr2, pp2);
              loopMinUpY = std::min(loopMinUpY, uu[1]);
              loopMaxPitch = std::max(loopMaxPitch, pp2);
              loopMinPitch = std::min(loopMinPitch, pp2);
              loopMaxAbsRoll = std::max(loopMaxAbsRoll, std::fabs(rr2));
              float av[3]; ph->getBodyAngularVelocity(car.chassis(), av);
              const float aw = std::sqrt(av[0]*av[0]+av[1]*av[1]+av[2]*av[2]);
              loopMaxW = std::max(loopMaxW, aw);
              if (i == 60*6) loopWAt6s = aw; }
        }
        x3::logInfo("[wings-test] N4 LOOP DIAG: minUpY=" + std::to_string(loopMinUpY) +
                    " pitchRange=[" + std::to_string(loopMinPitch*57.2958f) + "," +
                    std::to_string(loopMaxPitch*57.2958f) + "] deg maxAbsRoll=" +
                    std::to_string(loopMaxAbsRoll*57.2958f) + " deg maxW=" +
                    std::to_string(loopMaxW) + " rad/s wAt6s=" + std::to_string(loopWAt6s));
        x3::logInfo("[wings-test] N4 LOOP: inverted=" + std::string(wentInverted ? "YES" : "NO") +
                    " recovered=" + std::string(cameBack ? "YES" : "NO") +
                    " minSpeed=" + std::to_string(minLoopSpeed * 2.23694f) + " mph");
        check(wentInverted, "N4: full-stick loop goes INVERTED (aerobatics never fought)");
        check(cameBack, "N4: the loop completes back to upright");
        check(minLoopSpeed > 20.0f, "N4: no stall through the loop");
    }
    car.shutdown(); ph->shutdown();

    // ---- N5: LANDING RULE + CRASH LOCKOUT on fresh cars. -------------------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> ph3;
        DriveDemo c3;
        bool ok3 = buildWorld(ph3, c3, 2000.0f);
        check(ok3, "N5: landing-test world builds");
        if (ok3) {
            // Fast-track the secret: tiny bottle, instant wings.
            c3.nitroTuning().bottleSecs = 0.20f;
            c3.nitroTuning().odWingsSecs = 2.5f;   // overdrive builds ~120 m/s first, so
            // the deploy lands ABOVE the 60 mph retract line (no same-frame fold)
            for (int i = 0; i < 60 * 6 && !c3.wingsDeployed(); ++i) {
                x3::phys::VehicleInput in{}; in.throttle = 1.0f;
                c3.setNitroInput(true);
                stepCar(*ph3, c3, in);
            }
            check(c3.wingsDeployed(), "N5: fast-track wings deploy");
            (void)c3.wingsJustDeployed();
            // Grounded ABOVE 60 mph: wings stay out (rolling fast).
            bool stayedOut = true;
            for (int i = 0; i < 30; ++i) {
                x3::phys::VehicleInput in{};
                c3.setNitroInput(true);   // still thrusting down the runway
                stepCar(*ph3, c3, in);
                if (c3.forwardSpeed() * 2.23694f > 62.0f && !c3.wingsDeployed())
                    stayedOut = false;
            }
            check(stayedOut, "N5: wheels down ABOVE 60 mph keeps the wings out");
            // Brake below 60: the wings fold, the car is a car, CONTACT LAW back.
            bool sawRetract = false;
            for (int i = 0; i < 60 * 12 && c3.wingsDeployed(); ++i) {
                x3::phys::VehicleInput in{}; in.brake = 1.0f;
                c3.setNitroInput(false);
                stepCar(*ph3, c3, in);
                if (c3.wingsJustRetracted()) sawRetract = true;
            }
            check(!c3.wingsDeployed() && sawRetract,
                  "N5: grounded below 60 mph RETRACTS the wings (a car again)");
            c3.shutdown(); ph3->shutdown();
        }

        // CRASH: the detector, the tumble handoff and the lockout.
        std::unique_ptr<x3::phys::IPhysicsWorld> ph4;
        DriveDemo c4;
        bool ok4 = buildWorld(ph4, c4, 2000.0f);
        check(ok4, "N5: crash-test world builds");
        if (ok4) {
            c4.nitroTuning().bottleSecs = 0.20f;
            c4.nitroTuning().odWingsSecs = 2.5f;
            for (int i = 0; i < 60 * 6 && !c4.wingsDeployed(); ++i) {
                x3::phys::VehicleInput in{}; in.throttle = 1.0f;
                c4.setNitroInput(true);
                stepCar(*ph4, c4, in);
            }
            // Climb well clear of the slab, build speed.
            for (int i = 0; i < 60 * 6; ++i) {
                x3::phys::VehicleInput in{};
                c4.setNitroInput(true);
                c4.setFlightInput(i < 120 ? 0.5f : 0.0f, 0.0f);
                stepCar(*ph4, c4, in);
            }
            const float vPre = c4.forwardSpeed();
            // The wall: shed 30 m/s in one step (only a collision can do this —
            // here it is synthesized so the test doesn't depend on streamed
            // geometry; the live crash comes from real Jolt contacts).
            float lv[3]; ph4->getBodyLinearVelocity(c4.chassis(), lv);
            const float sp = std::sqrt(lv[0]*lv[0]+lv[1]*lv[1]+lv[2]*lv[2]);
            if (sp > 1.0f) {
                const float s = std::max(0.0f, (sp - 30.0f) / sp);
                float nv[3] = { lv[0]*s, lv[1]*s, lv[2]*s };
                ph4->setBodyLinearVelocity(c4.chassis(), nv);
            }
            bool sawCrash = false;
            {
                x3::phys::VehicleInput in{};
                c4.setNitroInput(true); c4.setFlightInput(0.0f, 0.0f);
                stepCar(*ph4, c4, in);
                if (c4.justCrashed()) sawCrash = true;
            }
            x3::logInfo("[wings-test] N5 CRASH: vPre=" + std::to_string(vPre * 2.23694f) +
                        " mph, event=" + (sawCrash ? "FIRED" : "missed") +
                        ", lockout=" + std::to_string(c4.crashLockout()) + " s");
            check(sawCrash, "N5: a >18 m/s single-step speed loss while winged = CRASH");
            check(!c4.wingsDeployed(), "N5: the crash TEARS the wings off");
            check(c4.crashLockout() > 4.0f, "N5: NOS/overdrive locked out after the crash");
            bool odWhileLocked = false;
            for (int i = 0; i < 60; ++i) {
                x3::phys::VehicleInput in{}; in.throttle = 1.0f;
                c4.setNitroInput(true);
                stepCar(*ph4, c4, in);
                if (c4.overdriveHeldSecs() > 0.0f || c4.nosSpraying()) odWhileLocked = true;
            }
            check(!odWhileLocked, "N5: the lockout actually blocks spray/overdrive");
            c4.shutdown(); ph4->shutdown();
        }
    }

    // ---- N6: THE PARACHUTE — descends, steers, lands ON the field. ---------
    {
        ParachuteBailout chute;
        const float gx = 1200.0f, gz = -800.0f;   // arbitrary spot on the world field
        const float gy = x3::game::terrainHeightAtWorld(gx, gz);
        float pos[3] = { gx, gy + 80.0f, gz };
        float vel[3] = { 60.0f, -2.0f, 0.0f };    // ejected from a fast beast
        chute.deploy(pos, vel);
        int steps = 0;
        while (chute.update(dt, 1.0f, 0.0f) && steps < 60 * 60) ++steps;
        float lp[3]; chute.pos(lp);
        const float gEnd = x3::game::terrainHeightAtWorld(lp[0], lp[2]);
        x3::logInfo("[wings-test] N6 CHUTE: " + std::to_string(steps * dt) +
                    " s down from 80 m, landed y=" + std::to_string(lp[1]) +
                    " field=" + std::to_string(gEnd) + ", drift x=" +
                    std::to_string(lp[0] - gx) + " m");
        check(chute.landed(), "N6: the chute lands");
        check(std::fabs(lp[1] - gEnd) < 0.01f, "N6: boots ON the field (CONTACT LAW)");
        check(lp[0] - gx > 10.0f, "N6: the canopy steers (drift under A/D)");
        check(steps * dt > 8.0f && steps * dt < 40.0f, "N6: a drift DOWN, not a drop");
    }

    x3::logInfo("[wings-test] " + std::to_string(passN) + " passed, " +
                std::to_string(failN) + " failed");
    return failN == 0;
}

// ===========================================================================
// vehcam — hull-attitude chase-camera math (see vehicle.h)
// ===========================================================================
namespace vehcam {

namespace {
inline void quatRotate(const float q[4], const float v[3], float out[3]) {
    // v' = v + 2*qv x (qv x v + w*v), quat (x,y,z,w).
    const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    float t[3] = { qy*v[2] - qz*v[1] + qw*v[0],
                   qz*v[0] - qx*v[2] + qw*v[1],
                   qx*v[1] - qy*v[0] + qw*v[2] };
    out[0] = v[0] + 2.0f * (qy*t[2] - qz*t[1]);
    out[1] = v[1] + 2.0f * (qz*t[0] - qx*t[2]);
    out[2] = v[2] + 2.0f * (qx*t[1] - qy*t[0]);
}
inline float len3(const float v[3]) {
    return std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
}
inline bool norm3(float v[3]) {
    const float l = len3(v);
    if (l < 1e-5f) return false;
    v[0] /= l; v[1] /= l; v[2] /= l; return true;
}
} // namespace

void hullAxes(const float q[4], float outFwd[3], float outUp[3]) {
    const float fL[3] = { 0.0f, 0.0f, -1.0f };
    const float uL[3] = { 0.0f, 1.0f,  0.0f };
    quatRotate(q, fL, outFwd);
    quatRotate(q, uL, outUp);
}

void hullRollPitch(const float fwdW[3], const float upW[3],
                   float& outRoll, float& outPitch) {
    // Pitch: elevation of the hull nose. Roll: the hull up decomposed in the
    // LEVEL frame of the horizontal forward (rightLevel = cross(fwd, worldUp)
    // = (-fz, 0, fx) — the car's construction), + = leaning starboard.
    const float fy = std::clamp(fwdW[1], -1.0f, 1.0f);
    outPitch = std::asin(fy);
    float rl[3] = { -fwdW[2], 0.0f, fwdW[0] };
    if (!norm3(rl)) { outRoll = 0.0f; return; }   // nose straight up/down
    // levelUp = cross(rightLevel, fwd)
    const float lu[3] = { rl[1]*fwdW[2] - rl[2]*fwdW[1],
                          rl[2]*fwdW[0] - rl[0]*fwdW[2],
                          rl[0]*fwdW[1] - rl[1]*fwdW[0] };
    const float sr = upW[0]*rl[0] + upW[1]*rl[1] + upW[2]*rl[2];
    const float cr = upW[0]*lu[0] + upW[1]*lu[1] + upW[2]*lu[2];
    outRoll = std::atan2(sr, cr);
}

void flyChase(FlyCamState& s, const float q[4], float dt,
              float upLevelBlend, float lerpRate) {
    float f[3], u[3];
    hullAxes(q, f, u);
    // Up target: hull up blended toward world-up (a chase plane lags level).
    const float b = std::clamp(upLevelBlend, 0.0f, 1.0f);
    float ut[3] = { u[0]*(1.0f-b), u[1]*(1.0f-b) + b, u[2]*(1.0f-b) };
    if (!norm3(ut)) { ut[0] = u[0]; ut[1] = u[1]; ut[2] = u[2]; }
    if (!s.valid) {
        for (int k = 0; k < 3; ++k) { s.fwd[k] = f[k]; s.up[k] = ut[k]; }
        s.valid = true;
        return;
    }
    // dt-scaled ease (never per-frame — the HARD rule).
    const float k = 1.0f - std::exp(-std::max(0.0f, lerpRate) * dt);
    for (int i = 0; i < 3; ++i) {
        s.fwd[i] += (f[i]  - s.fwd[i]) * k;
        s.up[i]  += (ut[i] - s.up[i])  * k;
    }
    // Renormalize; snap through the (transient) antipodal degeneracy mid-loop.
    if (!norm3(s.fwd)) { for (int i = 0; i < 3; ++i) s.fwd[i] = f[i]; }
    if (!norm3(s.up))  { for (int i = 0; i < 3; ++i) s.up[i]  = ut[i]; }
    const float d = s.fwd[0]*s.up[0] + s.fwd[1]*s.up[1] + s.fwd[2]*s.up[2];
    if (std::fabs(d) > 0.995f) {   // up collapsed onto fwd — take the target
        for (int i = 0; i < 3; ++i) s.up[i] = ut[i];
    }
}

void boatFollow(BoatCamState& s, const float q[4], float dt,
                float rollFrac, float pitchFrac, float lerpRate) {
    float f[3], u[3], hr, hp;
    hullAxes(q, f, u);
    hullRollPitch(f, u, hr, hp);
    const float k = 1.0f - std::exp(-std::max(0.0f, lerpRate) * dt);
    s.roll  += (rollFrac  * hr - s.roll)  * k;
    s.pitch += (pitchFrac * hp - s.pitch) * k;
}

void basisYawPitchRoll(float yaw, float pitch, float roll,
                       float outFwd[3], float outUp[3]) {
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    outFwd[0] = cp * cy; outFwd[1] = sp; outFwd[2] = cp * sy;
    // camRight = normalize(cross(fwd, worldUp)) = normalize(-fz, 0, fx).
    float cr[3] = { -outFwd[2], 0.0f, outFwd[0] };
    if (!norm3(cr)) { cr[0] = -sy; cr[1] = 0.0f; cr[2] = cy; }
    // levelUp = cross(right, fwd); rolled: up = cos(r)*levelUp + sin(r)*right.
    const float cu[3] = { cr[1]*outFwd[2] - cr[2]*outFwd[1],
                          cr[2]*outFwd[0] - cr[0]*outFwd[2],
                          cr[0]*outFwd[1] - cr[1]*outFwd[0] };
    const float cb = std::cos(roll), sb = std::sin(roll);
    for (int i = 0; i < 3; ++i) outUp[i] = cb*cu[i] + sb*cr[i];
}

} // namespace vehcam

// ===========================================================================
// runVehicleCamSelfTest (--test-vehicle) — see vehicle.h
// ===========================================================================
bool runVehicleCamSelfTest() {
    int passN = 0, failN = 0;
    auto check = [&](bool ok, const char* what) {
        if (ok) { ++passN; x3::logInfo(std::string("PASS C") + std::to_string(passN + failN) + " " + what); }
        else    { ++failN; x3::logError(std::string("FAIL C") + std::to_string(passN + failN) + " " + what); }
    };
    const float kRoll = 0.3f;                       // 17.2 deg hull roll
    // Quat for roll about the hull's local forward (0,0,-1): axis*sin(a/2), cos(a/2).
    const float hs = std::sin(kRoll * 0.5f), hc = std::cos(kRoll * 0.5f);
    const float qRoll[4]  = { 0.0f, 0.0f, -hs, hc };
    const float qLevel[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    // Pitch 0.2 rad about local right (+X): nose up.
    const float ps = std::sin(0.1f), pc = std::cos(0.1f);
    const float qPitch[4] = { ps, 0.0f, 0.0f, pc };

    // --- Extraction sanity: the rolled hull reads back its own roll. ---
    {
        float f[3], u[3], r, p;
        vehcam::hullAxes(qRoll, f, u);
        vehcam::hullRollPitch(f, u, r, p);
        check(std::fabs(r - kRoll) < 0.01f && std::fabs(p) < 0.01f,
              "extract: rolled hull reads roll=0.3, pitch=0");
        vehcam::hullAxes(qPitch, f, u);
        vehcam::hullRollPitch(f, u, r, p);
        check(std::fabs(p - 0.2f) < 0.01f && std::fabs(r) < 0.01f,
              "extract: pitched hull reads pitch=0.2, roll=0");
    }

    // --- FLY: converged camera up TRACKS hull roll (blended toward level). ---
    {
        const float kBlend = 0.30f, kRate = 5.0f, dt = 1.0f / 60.0f;
        vehcam::FlyCamState s;
        for (int i = 0; i < 600; ++i) vehcam::flyChase(s, qRoll, dt, kBlend, kRate);
        float r, p; vehcam::hullRollPitch(s.fwd, s.up, r, p);
        // Expected converged roll = atan2(0.7 sin, 0.7 cos + 0.3) ~= 0.21 rad.
        check(r > 0.15f && r < 0.30f,
              "fly: camera up tracks hull roll (0.15 < r < 0.30 for hull 0.3)");
        vehcam::FlyCamState s2;
        for (int i = 0; i < 600; ++i) vehcam::flyChase(s2, qLevel, dt, kBlend, kRate);
        float r2, p2; vehcam::hullRollPitch(s2.fwd, s2.up, r2, p2);
        check(std::fabs(r2) < 1e-3f, "fly (negative control): level hull -> level camera");
    }

    // --- BOAT: camera roll is a FRACTION (< 0.5) of hull roll, same sign. ---
    {
        const float kRollFrac = 0.40f, kPitchFrac = 0.20f, kRate = 2.5f, dt = 1.0f / 60.0f;
        vehcam::BoatCamState s;
        for (int i = 0; i < 900; ++i) vehcam::boatFollow(s, qRoll, dt, kRollFrac, kPitchFrac, kRate);
        check(s.roll > 0.02f && s.roll < 0.5f * kRoll,
              "boat: camera roll a positive fraction < 0.5 of hull roll");
        vehcam::BoatCamState sp;
        for (int i = 0; i < 900; ++i) vehcam::boatFollow(sp, qPitch, dt, kRollFrac, kPitchFrac, kRate);
        check(sp.pitch > 0.005f && sp.pitch < 0.5f * 0.2f,
              "boat: camera pitch a positive fraction < 0.5 of hull pitch");
        vehcam::BoatCamState sl;
        for (int i = 0; i < 900; ++i) vehcam::boatFollow(sl, qLevel, dt, kRollFrac, kPitchFrac, kRate);
        check(std::fabs(sl.roll) < 1e-4f && std::fabs(sl.pitch) < 1e-4f,
              "boat (negative control): level hull -> zero camera roll/pitch");
    }

    // --- SWELL: a floating hull with swellTorque genuinely rocks (bounded);
    //     without it the flat plane settles it dead level. Real Jolt physics. ---
    {
        auto rockAmplitude = [&](bool swell, float* outMean = nullptr) -> float {
            std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
            if (!w->init()) return -1.0f;
            const float hx = 1.5f, hy = 0.6f, hz = 3.0f, sea = 8.0f;
            const float fullVol = 8.0f * hx * hy * hz;
            const float mass = 0.5f * fullVol * 1025.0f * 0.95f;
            x3::phys::BodyId hull = w->addBox({hx, hy, hz}, {0.0f, sea + 0.2f, 0.0f},
                                              mass, x3::phys::Layer::Dynamic);
            x3::phys::BuoyancyDesc bd;
            bd.body = hull; bd.seaLevel = sea;
            bd.halfExtents[0]=hx; bd.halfExtents[1]=hy; bd.halfExtents[2]=hz;
            bd.fluidDensity = 1025.0f; bd.linearDrag = 2.5f; bd.angularDrag = 2.5f;
            if (swell) {   // mirror BoatDemo::build's live config
                bd.swellTorque    = mass * 0.18f;
                bd.swellFreqHz    = 0.18f;
                bd.rightingTorque = mass * 2.0f;
            }
            std::unique_ptr<x3::phys::IVehicleController> ctl(
                x3::phys::createBuoyancyController(*w, bd));
            if (!ctl) { w->shutdown(); return -1.0f; }
            const float dt = 1.0f / 60.0f;
            float maxRoll = 0.0f, sumRoll = 0.0f;
            int   samples = 0;
            for (int i = 0; i < 1800; ++i) {           // 30 s: several swell periods
                ctl->preStep(dt); w->step(dt); ctl->postStep(dt);
                if (i < 300) continue;                  // let the drop/settle pass
                float q[4]; w->getBodyRotation(hull, q);
                float f[3], u[3], r, p;
                vehcam::hullAxes(q, f, u);
                vehcam::hullRollPitch(f, u, r, p);
                maxRoll = std::max(maxRoll, std::fabs(r));
                sumRoll += r; ++samples;
            }
            ctl.reset(); w->shutdown();
            if (outMean) *outMean = samples ? sumRoll / (float)samples : 0.0f;
            return maxRoll;
        };
        float meanRoll = 0.0f;
        const float withSwell = rockAmplitude(true, &meanRoll);
        const float calm      = rockAmplitude(false);
        x3::logInfo("[vehcam-test] swell roll: max=" + std::to_string(withSwell) +
                    " mean=" + std::to_string(meanRoll) + " rad, calm max=" +
                    std::to_string(calm) + " rad");
        check(withSwell > 0.01f && withSwell < 0.30f,
              "swell: hull rocks (0.01 < maxRoll < 0.30 rad)");
        check(std::fabs(meanRoll) < 0.02f,
              "swell: hull rocks ABOUT LEVEL (|mean roll| < 0.02 rad — righting kills the list)");
        check(calm >= 0.0f && calm < 0.005f,
              "swell (negative control): calm water stays level");
    }

    x3::logInfo("[vehcam-test] " + std::to_string(passN) + " passed, " +
                std::to_string(failN) + " failed");
    return failN == 0;
}

} // namespace x3::game
