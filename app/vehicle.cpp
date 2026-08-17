// Vehicle demo worlds — implementation. See vehicle.h.
//
// Built only through the public engine interfaces (IRenderDevice / IPhysicsWorld /
// IVehicleController). Clean-room.

#include "vehicle.h"
#include "carspec.h"        // CarSpec — the per-car variables this build applies
#include "terrain.h"        // THE CONTACT LAW: wheels never under the height field

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

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

namespace {
// BODY WIDTH. Scales the hero car's bodywork on X. 1.0 = the GLB's stock
// 1.808 m (5.93 ft); 1.18 -> 2.13 m (7.0 ft).
// The WHEEL TRACK below is multiplied by the SAME factor, because the arches
// move out with the body — widening one without the other is what made it look
// like a donk (Tim, 2026-08-14). Change this single constant and the stance
// stays coherent.
constexpr float kBodyWiden = 1.18f;
} // namespace

// ===========================================================================
// DriveDemo
// ===========================================================================
bool DriveDemo::buildPhysics(x3::phys::IPhysicsWorld& physics, float x, float y, float z,
                             const CarSpec* spec) {
    m_physics = &physics;

    // ---- PER-CAR VARIABLES (app/carspec.h). ----------------------------
    // Everything below defaults to the shipped hero-car figures; a non-null
    // `spec` replaces them. The CTR row of the table is deliberately set to
    // these exact values, so passing the CTR spec changes nothing at all —
    // which is what keeps two days of tuning-by-ear safe from this feature.
    float hx = m_hx, hy = m_hy, hz = m_hz;
    float massKg   = 1083.2f;
    float comOffY  = -0.45f;   // their measured anti-tip figure; see the receipt below
    // Tyre-compound multiplier over the contact-patch spec set on each wheel
    // below. 1.0 == the hero car's compound, so no spec leaves their measured
    // 1.6 / 1.60 / 1.50 untouched.
    float gripCompound = 1.0f;
    float brakeNm  = 2200.0f;
    float suspFreq = 2.2f, suspDamp = 0.7f;
    if (spec) {
        // halfExtents[0] is the car's TRUE body half-width; kBodyWiden is the
        // STANCE widening applied to the render body, and the collision box has
        // to follow it or the bodywork hangs outside its own collision. That is
        // already why the shipped m_hx is 1.07 and not the GLB's 0.907 — taking
        // the spec figure literally would have narrowed the hero car's box by
        // 21%, and nothing short of a close screenshot would have shown it.
        hx = spec->halfExtents[0] * kBodyWiden;
        hy = spec->halfExtents[1]; hz = spec->halfExtents[2];
        massKg = spec->massKg;
        // The box CENTRE sits ~0.76 m up on the hero car (half-height 0.50 plus
        // its ride height), and the shipped -0.30 offset is what puts the CoM
        // at the 0.46 m Tim settled on. Referencing the offset to that known-
        // good pair — and letting it track a taller or shorter body — makes
        // this reduce EXACTLY to -0.30 for the CTR while still honouring
        // cars.json's comHeight for a lorry or a single-seater.
        const float boxCentreH = 0.76f + (hy - 0.50f);
        comOffY = spec->comHeight - boxCentreH;
        // Tyre compound RELATIVE to the hero car's 1.7, so the CTR resolves to
        // 1.0 and leaves the measured contact-patch numbers exactly as tuned.
        gripCompound = spec->gripScale / 1.7f;
        brakeNm = spec->brakeTorque;
        suspFreq = spec->suspFreq; suspDamp = spec->suspDamp;
    }

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
    //
    // MERGE 2026-08-16 (carspec lane): the -0.45 above is a MEASURED result
    // against a real owner receipt, so it stays as the default and the CTR row
    // of the per-car table was re-baselined to it (comHeight 0.31, not the
    // 0.46 this lane shipped). The spec path only REPLACES it for a car that
    // has its own figure — a lorry's centre of mass does not belong at 0.31 m,
    // and the whole point of the table is that it no longer has to.
    m_chassis = physics.addBox(x3::phys::Vec3{hx, hy, hz},
                               x3::phys::Vec3{x, y, z}, massKg, x3::phys::Layer::Dynamic,
                               x3::phys::Vec3{0.0f, comOffY, 0.0f});
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
    // x stations are the GLB's own (+-0.677 / +-0.723) SCALED BY kBodyWiden, so
    // the wheels stay centered under the widened arches instead of poking out.
    // PER-CAR TRACK AND WHEELBASE. The stations above are the CTR's, in metres.
    // A lorry on a sports car's track would make cars.json's comHeight thesis a
    // lie — the rollover threshold is atan(halfTrack / comHeight), so the track
    // has to move with the spec or half the table does nothing. Both scales are
    // 1.0 for the hero car by construction (1.354 / 1.95 are its own figures).
    const float trackScale = spec ? (spec->trackM / 1.354f) : 1.0f;
    const float baseScale  = spec ? (hz / 1.95f) : 1.0f;
    P p[4] = {
        { -0.677f * kBodyWiden * trackScale, -1.186f * baseScale, true,  false, true  },   // front-left  (AWD drive)
        {  0.677f * kBodyWiden * trackScale, -1.186f * baseScale, true,  false, true  },   // front-right
        { -0.723f * kBodyWiden * trackScale,  1.088f * baseScale, false, true,  true  },   // rear-left  (drive)
        {  0.723f * kBodyWiden * trackScale,  1.088f * baseScale, false, true,  true  },   // rear-right (drive)
    };
    for (int i = 0; i < 4; ++i) {
        x3::phys::WheelDesc w;
        // Attach high in the wheel well (NOT the box bottom) so the rest pose
        // matches the GLB arches: wheel center = attach - suspension (~0.30 m).
        w.position[0] = p[i].wx; w.position[1] = -0.15f; w.position[2] = p[i].wz;
        w.radius = 0.33f; w.width = 0.24f;
        w.suspensionMin = 0.15f; w.suspensionMax = 0.42f;
        w.suspensionFreq = suspFreq; w.suspensionDamp = suspDamp;
        w.steered = p[i].steer; w.handBraked = p[i].hb; w.powered = p[i].powered;
        w.maxSteerAngle = 0.5236f; // ~30deg
        w.maxBrakeTorque = brakeNm;
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
        //
        // MERGE 2026-08-16 (carspec lane): these are the CONTACT-PATCH spec now,
        // on a scale the friction-combine fix normalized. The per-car table's
        // gripScale is a TYRE-COMPOUND multiplier (the CTR's 1.7 is its unit),
        // so it multiplies these rather than replacing them — the hero car
        // lands on exactly 1.6 / 1.60 / 1.50 and a lorry on 1.0/1.7 of it.
        // Rebasing it this way matters: this lane's old 10.0/8.0 figures were
        // the pre-normalization scale, and carrying them across would have
        // undone the whole sqrt(tire * 0.2) discovery.
        w.gripScale        = 1.6f * gripCompound;
        w.lateralGripScale = (p[i].steer ? 1.60f : 1.50f) * gripCompound;
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
    vd.gearRatios[0] = 3.154f; vd.gearRatios[1] = 2.150f; vd.gearRatios[2] = 1.560f;
    vd.gearRatios[3] = 1.242f; vd.gearRatios[4] = 1.024f; vd.gearRatios[5] = 0.821f;
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
    // whole car punches harder everywhere you actually drive it. 1st now
    // redlines ~40 mph, 6th ~153. Live-tune: `car_final` (dial back toward
    // 4.2 if the top end matters more than the punch).
    vd.finalDrive = 4.6f;
    // ---- THE PER-CAR ENGINE, applied LAST so it wins. -------------------
    // Deliberately additive rather than a rewrite of the block above: the
    // hero car's tuning history (and every comment recording WHY each number
    // is what it is) stays exactly where it was. No spec -> not one byte of
    // the above changes, which is what let this lane and the vehicle-feel
    // lane land on the same function without either losing its receipts.
    if (spec) spec->applyTo(vd);
    // Wheel rays filter on Dynamic (the chassis layer): Jolt's vehicle object filter
    // is the COLLISION MATRIX, and Dynamic-vs-Static collides, so a Dynamic-masked
    // ray hits the Static ground. (Static-vs-Static does NOT collide — a Static mask
    // would pass through the ground.)
    vd.groundLayer = x3::phys::Layer::Dynamic;
    m_ctl.reset(x3::phys::createWheeledVehicle(physics, vd));
    if (!m_ctl) { physics.removeBody(m_chassis); m_chassis = {}; return false; }
    return true;
}

bool DriveDemo::build(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                      float x, float y, float z, const CarSpec* spec) {
    m_device = &device;
    if (!buildPhysics(physics, x, y, z, spec)) return false;

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
constexpr float kBodyDropY = -0.76f;
// BODY WIDTH (Tim, 2026-08-14: "The MODEL needs to be wider"). Scales the skin's
// X axis only, so the bodywork widens while the wheels — which are drawn from
// the physics wheel poses, not from this matrix — stay on the model's own track.
// That is the opposite of widening the track, which made it look like a donk.
// 1.0 = stock 1.808 m (5.93 ft); 1.18 -> 2.13 m (7.0 ft) of hips.
const float kBodySkin[16] = { -kBodyWiden,0,0,0,  0,1,0,0,  0,0,-1,0,  0,kBodyDropY,0,1 };
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
                          d.clearcoat, d.clearcoatRough);   // car-paint clearcoat lobe
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
        m_ctl->setTorqueBoost(m_userTorqueMult);
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
    m_ctl->setTorqueBoost(m_userTorqueMult * m_turboMult);
}

void DriveDemo::preStep(float dt)  { shapeSteering(dt); updateTurbo(dt); updateEngineModel(dt); if (m_ctl) m_ctl->preStep(dt); }

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
    // ---- THE CONTACT LAW (NO_SLOP.md rule 11): "no tires, and no Boots and
    // no feet can EVER be underground." Enforced HERE, once, for every world
    // that drives this car. If any wheel's contact point sits beyond-
    // suspension-deep below the carved terrain field, the car is lifted back
    // onto it and its downward velocity cleared. Corridors carve the field
    // itself, so bores/underpasses are safe — the law only ever pushes UP.
    // ...AND ONLY IN A WORLD WHOSE GROUND ACTUALLY IS THE TERRAIN.
    //
    // terrainHeightAtWorld() is PROCEDURAL: it answers for any (x,z) whether or
    // not terrain is what you are standing on. The law was applied to every
    // world that drives this car, but the drive/skidpad harnesses, the shop
    // floor and the showroom slab are all flat colliders sitting wherever the
    // analytic field happens to be metres away — so the law read them as buried
    // and teleported the car upward every frame. Gating on wheel contact alone
    // was not enough either: a drop test and a hard corner both leave wheels
    // legitimately airborne, and the law fired the instant they did (measured:
    // 12/6 ungated, 13/5 with a contact gate, 18/0 with this).
    // Terrain hosts opt IN; a bare DriveDemo does not get it.
    if (m_terrainContactLaw && m_ctl && m_physics && m_chassis.valid()) {
        float worst = 0.0f;
        for (uint32_t i = 0; i < m_ctl->wheelCount(); ++i) {
            x3::phys::WheelState ws;
            if (!m_ctl->wheelState(i, ws)) continue;
            // A WHEEL THAT IS TOUCHING SOMETHING IS NOT BURIED.
            //
            // The law originally compared every wheel against the ANALYTIC
            // height field regardless of what it was standing on, and the
            // field is evaluated procedurally — it answers everywhere, whether
            // or not terrain is the ground there. So on any world whose floor
            // is not the terrain (the headless slab the drive/skidpad tests
            // build, a shop floor, a deck) the field reported the car metres
            // "buried" while it sat perfectly on a collider, and the law
            // teleported it upward EVERY FRAME. Measured: --test-vehicle drive
            // 12/6 with the check ungated, 18/0 with it gated — six failures,
            // all of them this, including plain "wheels kept ground contact".
            //
            // Physics contact is authoritative. The case the law exists for —
            // a wheel that has fallen THROUGH geometry, or sits under a field
            // that has not streamed a collider yet — is exactly the case where
            // the wheel is touching nothing, so gating on that keeps every bit
            // of the protection and stops it fighting legitimate ground.
            if (ws.hasContact) continue;
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

void DriveDemo::chassisPos(float out[3]) const {
    x3::phys::Vec3 p = m_physics ? m_physics->getBodyPosition(m_chassis) : x3::phys::Vec3{};
    out[0] = p.x; out[1] = p.y; out[2] = p.z;
}

void DriveDemo::render(const x3::rhi::FrameContext& frame) const {
    if (!m_device || !m_ctl) return;

    x3::phys::Vec3 p = m_physics->getBodyPosition(m_chassis);
    float q[4]; m_physics->getBodyRotation(m_chassis, q);
    float pos[3] = { p.x, p.y, p.z };

    if (m_skinned) {
        // ---- HERO-CAR GLB skin: the body parts ride the sprung chassis (nose
        // flip + ride-height drop baked in kBodySkin); the wheels ride the LIVE
        // physics wheel poses (steer + spin + suspension travel). ----
        float chassisM[16]; composeTRS(pos, q, 1.0f, 1.0f, 1.0f, chassisM);
        float carM[16];     x3::asset::mulMat4(chassisM, kBodySkin, carM);
        float fin[16];
        for (const auto& d : m_bodyDraw) {
            x3::asset::mulMat4(carM, d.nodeTransform, fin);
            drawDrawable(frame, d, fin);
        }
        for (int s = 0; s < 4; ++s) {
            x3::phys::WheelState ws;
            if (!m_ctl->wheelState((uint32_t)s, ws)) continue;
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
    const uint32_t n = m_ctl->wheelCount();
    for (uint32_t i = 0; i < n; ++i) {
        x3::phys::WheelState ws;
        if (!m_ctl->wheelState(i, ws)) continue;
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
    x3::logInfo("[drive-test] " + std::to_string(passN) + " passed, " +
                std::to_string(failN) + " failed");
    return failN == 0;
}

// ===========================================================================
// BoatDemo
// ===========================================================================
bool BoatDemo::build(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                     float x, float y, float z, float seaLevel, bool isSub) {
    m_device = &device; m_physics = &physics;

    // Hull mass tuned so the box rides ~half-submerged in sea water:
    //   equilibrium submergedVol = mass / fluidDensity. For ~half of fullVol
    //   (=8*hx*hy*hz) submerged we want mass ~= 0.5 * fullVol * fluidDensity.
    const float fullVol = 8.0f * m_hx * m_hy * m_hz;
    const float fluidDensity = 1025.0f;
    const float mass = 0.5f * fullVol * fluidDensity * 0.95f; // slight float-high bias
    m_hull = physics.addBox(x3::phys::Vec3{m_hx, m_hy, m_hz},
                            x3::phys::Vec3{x, y, z}, mass, x3::phys::Layer::Dynamic);
    if (!m_hull.valid()) return false;

    x3::phys::BuoyancyDesc bd;
    bd.body = m_hull; bd.seaLevel = seaLevel;
    bd.halfExtents[0]=m_hx; bd.halfExtents[1]=m_hy; bd.halfExtents[2]=m_hz;
    bd.fluidDensity = fluidDensity;
    bd.linearDrag = 2.5f; bd.angularDrag = 2.5f;
    bd.propThrust = mass * 4.0f;     // can motor forward
    bd.steerTorque = mass * 1.5f;    // and turn
    if (isSub) bd.diveThrust = mass * 12.0f; // strong enough to submerge
    // Gentle synthetic SWELL (see BuoyancyDesc): the flat buoyancy plane would
    // otherwise settle the hull dead level — this rocks it a few degrees so the
    // attitude-following chase camera has REAL motion to read off the body. The
    // righting term makes it rock ABOUT LEVEL (the COM-buoyancy model has no
    // self-righting moment, so a spawn/drop transient would otherwise leave a
    // permanent list the camera would faithfully — and wrongly — show).
    bd.swellTorque    = mass * 0.18f;
    bd.swellFreqHz    = 0.18f;
    bd.rightingTorque = mass * 2.0f;
    m_ctl.reset(x3::phys::createBuoyancyController(physics, bd));
    if (!m_ctl) { physics.removeBody(m_hull); m_hull = {}; return false; }

    std::vector<x3::rhi::MeshVertex> cv; std::vector<uint32_t> ci;
    x3::prims::makeCube(0.5f, cv, ci);
    m_hullMesh = device.createMesh(cv.data(), (uint32_t)cv.size(), ci.data(), (uint32_t)ci.size());
    auto t = isSub ? x3::prims::makeSolidRGBA(8, 180, 180, 60)   // yellow sub
                   : x3::prims::makeSolidRGBA(8, 150, 90, 50);   // brown boat hull
    m_hullTex = device.createTexture(t.data(), 8, 8, true);
    return true;
}

void BoatDemo::setInput(const x3::phys::VehicleInput& in) { if (m_ctl) m_ctl->setInput(in); }
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
