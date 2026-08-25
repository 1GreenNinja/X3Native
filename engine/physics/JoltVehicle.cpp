// Vehicle / flight / buoyancy controllers — Jolt implementation (clean-room).
// Public API: IVehicle.h. Spec/principle: "every vehicle = rigid body +
// force-controller on Jolt."
//
// All JPH:: types are confined to this translation unit. The controllers reach
// the world's raw JPH::PhysicsSystem / JPH::Body via IPhysicsWorld::nativeSystem()
// / nativeBody() (documented escape hatch), then:
//
//   WHEELED   : builds a JPH::VehicleConstraint + WheeledVehicleController on the
//               chassis body (engine/transmission/differential + raycast wheels +
//               suspension springs) and registers it as a step listener. This is
//               the canonical Jolt vehicle (its model is "Car Physics for Games"
//               by Marco Monster). Driver input is pushed each pre-step.
//   BUOYANCY  : Archimedes buoyancy (rho * g * submergedVolume, up) + quadratic
//               drag, applied as forces each fixed step against a flat water plane
//               at seaLevel. `dive` adds vertical thrust (a sub submerges/surfaces).
//   FLIGHT    : forward thrust + a lift force (grows with forward speed) + quadratic
//               drag + pitch/yaw/roll control torques on the rigid body.
//
// CLEAN-ROOM: built from Jolt's public headers/samples docs + the standard physics
// (Archimedes, quadratic drag, rigid-body force/torque). NO id Tech / RBDOOM src.

#include "IVehicle.h"
#include "IPhysicsWorld.h"
#include "../core/x3_log.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

JPH_SUPPRESS_WARNINGS

namespace x3::phys {

namespace {

// The Layer enum -> Jolt ObjectLayer mapping MUST match JoltPhysicsWorld.cpp's
// ObjLayers (Static=0, Dynamic=1, Player=2, Enemy=3, Projectile=4, Trigger=5).
// We can't include that file's anonymous namespace, so we re-state the mapping
// here. (It is part of the engine's own physics contract, not Jolt's.)
JPH::ObjectLayer objLayerOf(Layer l) {
    switch (l) {
        case Layer::Static:     return 0;
        case Layer::Dynamic:    return 1;
        case Layer::Player:     return 2;
        case Layer::Enemy:      return 3;
        case Layer::Projectile: return 4;
        case Layer::Trigger:    return 5;
    }
    return 0;
}

inline JPH::Vec3 V3(const float v[3]) { return JPH::Vec3(v[0], v[1], v[2]); }
inline JPH::Vec3 norm(JPH::Vec3Arg v) {
    float l = v.Length();
    return l > 1e-6f ? v / l : JPH::Vec3(0, 0, -1);
}

constexpr float kGravity = 9.81f;

// ---- Automatic-transmission shift points, as fractions of the REDLINE -------
// Jolt ships absolute defaults (4000 up / 2000 down) that are unrelated to the
// engine we author, so every car with a redline above ~4500 upshifts early and
// bogs. These fractions are chosen against the stock torque curve
// ([0,0.78] [0.3,0.97] [0.55,1.0] [0.8,0.95] [1,0.82]) and Jolt's default gear
// ratios (2.66/1.78/1.3/1.0/0.74, a 1.49 step from 1st to 2nd):
//
//   up at 0.92 * redline lands the next gear at 0.92/1.49 = 0.62 of redline,
//   i.e. straight onto the 0.55 torque peak. Shifting later would clip the
//   power peak; shifting earlier drops below the peak and bogs.
//   down at 0.50 * redline leaves a wide band between the two so the box does
//   not hunt (a downshift must not immediately re-trigger an upshift).
// 0.92 -> 0.975. At 0.92 the box upshifted ~600 rpm short of the limiter, so the
// engine never actually screamed — Tim: "Right now, it seems like the engine
// doesnt ever rev to redline!" / "Scream to 7200. 7500 since it has titanium
// retainers". A high-revving flat-six is SUPPOSED to be taken to the stop; the
// original 0.92 was tuned to land the next gear on the 0.55 torque peak, and at
// 0.975 the next gear lands at 0.975/1.49 = 0.65 — still on the fat part of the
// curve, just the far side of the peak. Worth the trade for the noise.
//
// 0.975 -> 0.94 (2026-08-16). 0.975 requires the engine to PULL to 97.5% of
// redline before the box will hand over the next gear — fine in 1st-4th where
// torque is abundant, but in 5th at speed the resistive load flattens the pull
// and the crank plateaus a few hundred rpm short of 7312. The box then simply
// never offers 6th: Tim, "i cannot get to 140mph or 6th gear". 0.94 (7050) is
// still a scream — the tach needle is visibly inside the red band before the
// shift — but it is a bar the engine can actually clear under load in the
// tall gears.
constexpr float kShiftUpFrac   = 0.94f;
// Downshift point. 0.50 (3750) still hunted on grades: an upshift lands the next
// gear at ~0.68 redline, and on a climb the car decelerates into the 3750
// threshold, downshifts, revs, upshifts, repeats — "7x or more on 4th". 0.33
// (~2500 rpm, like a real auto's kickdown) leaves a wide band so the box holds
// the gear it just picked unless the car genuinely bogs.
constexpr float kShiftDownFrac = 0.33f;
// ---- THROTTLE-ADAPTIVE SHIFT BAND (2026-08-16, "it shouldnt peg redline the
// whole time you drive... most cars dont do that"). Jolt's auto box shifts on
// RPM thresholds ALONE — it knows nothing about throttle — so with the WOT
// points above (0.94/0.33) a car CRUISING at 70 mph sat in 5th at ~4800 rpm
// forever: the 0.50 overdrive 6th was unreachable below ~7050 rpm = ~106 mph.
// A real automatic shifts EARLY at light throttle and holds to redline at WOT.
// So the fractions above become the WOT end of a band, and preStep slides the
// live thresholds between these light-throttle values and the WOT ones by the
// (smoothed) throttle. Numbers, derived not vibed:
//   up 0.55 (4125): light cruise upshifts land the next gear on the fat part
//     of the curve; at 70 mph in 5th (4822 rpm with the 0.50x5.2 gearset,
//     vehicle.cpp — PAIRED) this is what actually engages 6th at cruise.
//   down 0.22 (1650): after the 5th->6th step (ratio 2.05, the widest), a
//     light upshift at 4125 lands at 4125/2.05 = 2013 rpm; the downshift
//     threshold must sit BELOW that or the box hunts 5-6-5-6 near 63 mph.
//     0.22*7500 = 1650 < 2013 with margin (plus the 0.55 s switch latency).
// Throttle smoothing is asymmetric (rise ~0.12 s, fall ~0.7 s): a WOT stab
// raises the band almost immediately (kickdown feels instant), a momentary
// lift mid-corner does NOT slam an upshift.
constexpr float kShiftUpLightFrac   = 0.55f;
constexpr float kShiftDownLightFrac = 0.22f;
// Fallback redline for vehicles authored without one, matching VehicleEngine's
// own default so behavior is unchanged for them.
constexpr float kDefaultRedlineRPM = 6000.0f;

inline void applyShiftPoints(JPH::VehicleTransmissionSettings& tr, float redlineRPM) {
    const float redline = redlineRPM > 0.0f ? redlineRPM : kDefaultRedlineRPM;
    tr.mShiftUpRPM   = redline * kShiftUpFrac;
    tr.mShiftDownRPM = redline * kShiftDownFrac;
}

// =====================================================================
// WHEELED — Jolt VehicleConstraint + WheeledVehicleController
// =====================================================================
class WheeledController final : public IVehicleController {
public:
    bool build(IPhysicsWorld& world, const WheeledVehicleDesc& d) {
        m_system = static_cast<JPH::PhysicsSystem*>(world.nativeSystem());
        m_chassis = static_cast<JPH::Body*>(world.nativeBody(d.chassis));
        if (!m_system || !m_chassis) {
            x3::logError("[vehicle] wheeled: invalid chassis body / system");
            return false;
        }
        if (!d.wheels || d.wheelCount == 0) {
            x3::logError("[vehicle] wheeled: needs >= 1 wheel");
            return false;
        }
        m_bodyId = d.chassis;
        m_localForward = norm(V3(d.forward));
        m_localUp      = norm(V3(d.up));
        m_wheelCount   = d.wheelCount;
        m_wheelRadius.resize(d.wheelCount);
        m_wheelWidth.resize(d.wheelCount);

        // ---- Vehicle constraint settings ----
        JPH::VehicleConstraintSettings vs;
        vs.mUp      = m_localUp;
        vs.mForward = m_localForward;
        // Keep the car from flipping fully upside down (still lets it lean).
        vs.mMaxPitchRollAngle = JPH::DegreesToRadians(60.0f);

        m_wheelSettings.clear();
        m_baseSuspMin.clear();
        m_baseSuspMax.clear();
        for (uint32_t i = 0; i < d.wheelCount; ++i) {
            const WheelDesc& w = d.wheels[i];
            JPH::WheelSettingsWV* ws = new JPH::WheelSettingsWV();
            ws->mPosition            = V3(w.position);
            ws->mSuspensionDirection = -m_localUp;          // suspension points down
            ws->mSteeringAxis        = m_localUp;
            ws->mWheelUp             = m_localUp;
            ws->mWheelForward        = m_localForward;
            ws->mSuspensionMinLength = w.suspensionMin;
            ws->mSuspensionMaxLength = w.suspensionMax;
            ws->mSuspensionSpring    = JPH::SpringSettings(
                JPH::ESpringMode::FrequencyAndDamping, w.suspensionFreq, w.suspensionDamp);
            ws->mRadius              = w.radius;
            ws->mWidth               = w.width;
            ws->mMaxSteerAngle       = w.steered ? w.maxSteerAngle : 0.0f;
            ws->mMaxBrakeTorque      = w.maxBrakeTorque;
            ws->mMaxHandBrakeTorque  = w.handBraked ? w.maxBrakeTorque * 2.5f : 0.0f;
            // BASELINE tire compound: scale Jolt's default friction curves by the
            // authored gripScale (see WheelDesc::gripScale). Lateral takes its
            // OWN scale when authored (WheelDesc::lateralGripScale) — huge
            // longitudinal for the launch, rollover-bounded lateral for flat
            // cornering. 0 = lateral follows gripScale (old behavior).
            if (w.gripScale > 0.0f && w.gripScale != 1.0f)
                for (auto& p : ws->mLongitudinalFriction.mPoints) p.mY *= w.gripScale;
            {
                const float lat = w.lateralGripScale > 0.0f ? w.lateralGripScale : w.gripScale;
                if (lat > 0.0f && lat != 1.0f)
                    for (auto& p : ws->mLateralFriction.mPoints) p.mY *= lat;
            }
            // Wheel inertia: Jolt's default 0.9 (a bare 20 kg rim) spins to ~137%
            // slip the instant the engine hits it, and the friction can only pull
            // that back at the wheel's effective-mass rate — so the car crawls and
            // the RPM "cycles". A real wheel+tire+driveline is heavier; raise it.
            ws->mInertia = 2.5f;
            if (i == 0)
                x3::logInfo("[vehicle] grip " + std::to_string(w.gripScale) +
                            " -> long peak " + std::to_string(ws->mLongitudinalFriction.GetValue(0.06f)) +
                            ", lat peak " + std::to_string(ws->mLateralFriction.GetValue(3.0f)) +
                            " / plateau " + std::to_string(ws->mLateralFriction.GetValue(20.0f)));
            vs.mWheels.push_back(ws);
            m_wheelRadius[i] = w.radius;
            m_wheelWidth[i]  = w.width;
            // Tuning bookkeeping: keep the raw settings pointer (the constraint's
            // Ref owns it; it stays alive for our lifetime) + the authored base
            // suspension lengths and the BASELINE tire friction curves (post-
            // gripScale), so a live tuning scales/offsets from the authored
            // baseline (idempotent — re-applying a tuning never compounds).
            m_wheelSettings.push_back(ws);
            m_baseSuspMin.push_back(w.suspensionMin);
            m_baseSuspMax.push_back(w.suspensionMax);
            // PER-WHEEL friction baselines (post-authored-gripScale), so a live
            // tuning multiplies from THIS WHEEL's stock compound. The old code
            // kept only wheel 0's baseline and rebuilt every wheel from it —
            // harmless while all four wheels shipped identical grip, but it
            // silently FLATTENED any authored front/rear split the moment a
            // global car_grip was applied (NFS turn-in balance, 2026-08-16).
            // Also which axle this is (steered = front, for per-axle tuning)
            // and the wheel's current relative grip multiplier (stock = 1), so
            // grip and latTail re-derive idempotently without losing each other.
            m_baseLongFriction.push_back(ws->mLongitudinalFriction);
            m_baseLatFriction.push_back(ws->mLateralFriction);
            m_wheelSteered.push_back(w.steered);
            m_gripNow.push_back(1.0f);
        }

        // ---- ANTI-ROLL BARS (see WheeledVehicleDesc::antiRollFront). One bar
        // per axle: pair the first two steered wheels (front) and the first two
        // non-steered (rear). Jolt applies force = stiffness * travel-difference
        // between the paired wheels, so the body stays FLAT in a corner while
        // straight-line suspension is untouched. Owner receipt 2026-08-16: "The
        // car wants to tip up on two wheels even trying to make ANY curve at
        // speed" — bars + the lateral grip cap are that fix. ----
        {
            auto addBar = [&](bool steered, float stiffness) {
                if (stiffness <= 0.0f) return;
                int lw = -1, rw = -1;
                for (uint32_t i = 0; i < d.wheelCount; ++i) {
                    if (d.wheels[i].steered != steered) continue;
                    if (lw < 0) lw = (int)i;
                    else if (rw < 0) { rw = (int)i; break; }
                }
                if (lw < 0 || rw < 0) return;   // axle needs a left AND a right
                // Label order (which index is "left") is immaterial: the bar's
                // corrective impulse is antisymmetric in the pair, so swapping
                // the labels produces byte-identical simulation (verified —
                // both orders ran the skidpad to identical telemetry).
                JPH::VehicleAntiRollBar bar;
                bar.mLeftWheel = lw; bar.mRightWheel = rw;
                bar.mStiffness = stiffness;
                vs.mAntiRollBars.push_back(bar);
            };
            addBar(true,  d.antiRollFront);
            addBar(false, d.antiRollRear);
        }

        // ---- Controller (engine + transmission + 1 differential) ----
        JPH::WheeledVehicleControllerSettings* cs = new JPH::WheeledVehicleControllerSettings();
        m_baseMaxTorque = d.maxEngineTorque;            // tuning/boost baseline
        m_finalDrive = d.finalDrive;
        m_redlineRPM = d.maxEngineRPM > 0.0f ? d.maxEngineRPM : kDefaultRedlineRPM;
        cs->mEngine.mMaxTorque = d.maxEngineTorque;
        cs->mEngine.mMaxRPM    = d.maxEngineRPM;
        // Flywheel inertia — see WheeledVehicleDesc::engineInertia. Lower spins
        // up faster, which is what "peppy" actually is.
        if (d.engineInertia > 0.0f) cs->mEngine.mInertia = d.engineInertia;
        // SHIFT DEBOUNCE. Jolt will re-evaluate a gear change every step, and with
        // a light flywheel + big torque the rpm crosses the threshold repeatedly
        // — Tim, 2026-08-15: "It literally shifts five times before it actually
        // changes a gear!" mSwitchTime is how long a change TAKES; mSwitchLatency
        // is the minimum quiet period before another is allowed. Both must be
        // non-zero or the box machine-guns.
        cs->mTransmission.mSwitchTime    = 0.28f;   // a quick but real shift
        cs->mTransmission.mSwitchLatency = 0.55f;   // no re-shift inside this window
        cs->mTransmission.mClutchReleaseTime = 0.20f;

        // Torque curve (see WheeledVehicleDesc::curveRpm/curveTq).
        if (d.curveCount > 1) {
            cs->mEngine.mNormalizedTorque.Clear();
            for (uint32_t ci = 0; ci < d.curveCount && ci < 8; ++ci)
                cs->mEngine.mNormalizedTorque.AddPoint(d.curveRpm[ci], d.curveTq[ci]);
            cs->mEngine.mNormalizedTorque.Sort();
        }

        // Gearbox (see WheeledVehicleDesc::gearRatios).
        if (d.gearCount > 0) {
            cs->mTransmission.mGearRatios.clear();
            for (uint32_t gi = 0; gi < d.gearCount && gi < 8; ++gi)
                if (d.gearRatios[gi] > 0.0f)
                    cs->mTransmission.mGearRatios.push_back(d.gearRatios[gi]);
        }
        // Reverse is ONE fixed ratio — a car never shifts in reverse. Jolt's
        // default is already single {-2.90}; make it explicit and match 1st
        // gear's magnitude so reverse speed feels right.
        cs->mTransmission.mReverseGearRatios.clear();
        cs->mTransmission.mReverseGearRatios.push_back(-3.15f);
        cs->mTransmission.mClutchStrength = d.clutchStrength;
        // SHIFT POINTS SCALE WITH THE REDLINE. Jolt's transmission defaults are
        // ABSOLUTE (mShiftUpRPM 4000 / mShiftDownRPM 2000) and know nothing about
        // the engine we just authored. Against a 6500 rpm redline that upshifts
        // ~2500 rpm early, dropping the engine into the dead part of its curve.
        //
        // Measured before this fix (--test-vehparts, Release): the tier-1 STREET
        // build upshifted at ~4030 rpm and fell to 3082 rpm at 14 m/s, while the
        // bone-stock car pulled first gear to 6500 rpm / 22.7 m/s and BEAT it.
        // Stock only escaped the early shift because its wheels slipped enough to
        // block it -- so the shop sold a power upgrade that made the car slower.
        applyShiftPoints(cs->mTransmission, d.maxEngineRPM);

        // Powered wheels feed the differential(s). Pair consecutive powered
        // wheels as (left, right): two powered = one axle (RWD/FWD); four = two
        // axles (AWD) so the torque splits across all four wheels — which is also
        // what lets a high-torque car hook up instead of spinning one axle.
        {
            std::vector<int> pw;
            for (uint32_t i = 0; i < d.wheelCount; ++i)
                if (d.wheels[i].powered) pw.push_back((int)i);
            if (pw.empty()) pw.push_back(0);                 // fall back: drive wheel 0
            for (size_t i = 0; i < pw.size(); i += 2) {
                JPH::VehicleDifferentialSettings diff;
                diff.mLeftWheel  = pw[i];
                diff.mRightWheel = (i + 1 < pw.size()) ? pw[i + 1] : -1;
                if (d.finalDrive > 0.0f) diff.mDifferentialRatio = d.finalDrive;
                cs->mDifferentials.push_back(diff);
            }
        }
        // Report the ACTUAL gearbox Jolt ends up with. Guessing at this from the
        // outside cost real time — "it still shifts when in 6th, it KEEPS going"
        // is unanswerable without knowing how many ratios the constraint holds
        // and where the shift points landed.
        {
            std::string gl;
            for (size_t gi = 0; gi < cs->mTransmission.mGearRatios.size(); ++gi) {
                if (gi) gl += ", ";
                gl += std::to_string(cs->mTransmission.mGearRatios[gi]);
            }
            x3::logInfo("[vehicle] gearbox: " +
                        std::to_string(cs->mTransmission.mGearRatios.size()) + " gears [" + gl +
                        "]  final " + std::to_string(d.finalDrive) +
                        "  shift up " + std::to_string(cs->mTransmission.mShiftUpRPM) +
                        " / down " + std::to_string(cs->mTransmission.mShiftDownRPM) +
                        "  switchTime " + std::to_string(cs->mTransmission.mSwitchTime) +
                        " latency " + std::to_string(cs->mTransmission.mSwitchLatency) +
                        "  redline " + std::to_string(cs->mEngine.mMaxRPM) +
                        "  inertia " + std::to_string(cs->mEngine.mInertia));
        }
        vs.mController = cs;

        // ---- Create the constraint, register it as a step listener ----
        // We HOLD a JPH::Ref to the constraint + tester so they outlive our
        // controller; the PhysicsSystem's ConstraintManager keeps its OWN Ref while
        // the constraint is added (Array<Ref<Constraint>>), so we must NOT manually
        // Release after RemoveConstraint — that would double-free. Letting the Ref
        // members drop in our destructor is the correct, leak-free teardown.
        m_constraint = new JPH::VehicleConstraint(*m_chassis, vs);  // refcount -> our Ref
        // ---------------------------------------------------------------------
        // SURFACE FRICTION COMBINE — the 2026-08-16 "doesn't like to turn" root
        // cause, found by MEASURING (X3_VEHDBG): the per-wheel lateral impulse
        // capped at exactly 0.583 x normal load on a tire authored 1.7.
        // Jolt's default combine is sqrt(tireFriction * bodyFriction), and NO
        // engine body ever calls SetFriction, so every static mesh (the ground,
        // every road) carries Jolt's default 0.2 — sqrt(1.7 * 0.2) = 0.583.
        // EXACT match. Every gripScale in this repo's history (1.7 -> 3.4 -> 10)
        // was unknowingly compensating for that hidden sqrt-crush: authored 10
        // was effectively sqrt(12 * 0.2) = 1.55 mu at the road.
        // The fix: normalize the surface term so the engine's universal 0.2
        // reads as "plain road" (x1.0) and the AUTHORED TIRE CURVES BECOME THE
        // SPEC (NO_SLOP rule 8 — the numbers in WheelDesc are now true at the
        // contact patch). A future icy/wet surface can still call SetFriction
        // (< 0.2 = slick, e.g. 0.05 -> quarter grip) and it scales linearly.
        // ---------------------------------------------------------------------
        m_constraint->SetCombineFriction(
            [](JPH::uint, float& ioLongitudinalFriction, float& ioLateralFriction,
               const JPH::Body& inBody2, const JPH::SubShapeID&) {
                constexpr float kDefaultBodyFriction = 0.2f;   // Jolt's untouched default
                const float surface = inBody2.GetFriction() / kDefaultBodyFriction;
                ioLongitudinalFriction *= surface;
                ioLateralFriction      *= surface;
            });
        // Raycast wheels against the ground object layer (the static terrain).
        m_tester = new JPH::VehicleCollisionTesterRay(objLayerOf(d.groundLayer), m_localUp);
        m_constraint->SetVehicleCollisionTester(m_tester);          // constraint holds a RefConst
        m_system->AddConstraint(m_constraint);                      // manager takes its own Ref
        m_system->AddStepListener(m_constraint);
        m_ctrl = static_cast<JPH::WheeledVehicleController*>(m_constraint->GetController());
        return true;
    }

    ~WheeledController() override {
        if (m_system && m_constraint) {
            m_system->RemoveStepListener(m_constraint);
            m_system->RemoveConstraint(m_constraint);   // drops the manager's Ref
        }
        m_ctrl = nullptr;
        // m_constraint / m_tester are JPH::Ref<> members: dropping them here releases
        // OUR refs. The constraint's refcount reaches 0 -> destroyed (its destructor
        // drops the tester's RefConst). No manual Release() needed.
        m_constraint = nullptr;
        m_tester = nullptr;
    }

    void setAeroSuspended(bool on) override { m_aeroSuspended = on; }
    void setConstraintSuspended(bool on) override {
        if (m_constraint) m_constraint->SetEnabled(!on);
    }
    void setInput(const VehicleInput& in) override { m_in = in; }

    void preStep(float dt) override {
        if (!m_ctrl) return;
        // THROTTLE-ADAPTIVE SHIFT BAND (see kShiftUpLightFrac above for the
        // full story + the derived numbers). The live transmission settings are
        // read by Jolt every step, so sliding the thresholds here is exactly
        // how a load-aware automatic behaves: early relaxed shifts at cruise,
        // pull-to-redline at WOT, instant kickdown on a stab of throttle.
        if (dt > 0.0f) {
            const float thr = std::clamp(std::fabs(m_in.throttle), 0.0f, 1.0f);
            const float tau = (thr > m_thrSm) ? 0.12f : 0.70f;   // rise fast, fall slow
            m_thrSm += (thr - m_thrSm) * (1.0f - std::exp(-dt / tau));
            // THE KICKDOWN DETENT (W-HANDLING3, 2026-08-17). A real automatic
            // does not read pedal travel as a continuous "sportiness" dial: it
            // has a DETENT near the floor, and anything short of it is cruise.
            // Model that literally — load is 0 below kDetentFrac of pedal and
            // smoothsteps to 1 at the floor.
            //
            // WHY, measured (rule 9): the previous shaping was load = thr^2,
            // which is continuous and therefore has no safe margin anywhere.
            // Holding 70 mph took 0.46 pedal -> load 0.21 -> the 5->6 upshift
            // threshold landed at 4750 rpm while 5th at 70 mph sits at 4822 —
            // SEVENTY-TWO rpm of margin. It "worked" only until the spoiler's
            // induced drag (see the wing-drag block below) raised cruise
            // throttle a few points; then the box hunted 5-6-5-6 with rpm
            // spiking to 5890 (H1, 2026-08-17) — the owner's "pegs redline"
            // complaint growing back in miniature. Worse, the hunt is
            // SELF-DRIVEN: upshifting into the 0.50 overdrive needs more pedal
            // to hold speed, which raises the load, which demands the downshift
            // that started it.
            // With the detent, cruise at 0.46 pedal is load 0 -> upshift at
            // 4125 rpm -> 700 rpm of margin, and holding 6th at 0.55 pedal is
            // STILL load 0, so the loop cannot close. WOT is untouched
            // (load 1 -> the 0.94/0.33 points), and a genuine kickdown past
            // the detent still slides the whole band up.
            constexpr float kDetentFrac = 0.55f;   // pedal fraction where "go" begins
            float load = (m_thrSm - kDetentFrac) / (1.0f - kDetentFrac);
            load = std::clamp(load, 0.0f, 1.0f);
            load = load * load * (3.0f - 2.0f * load);          // smoothstep
            JPH::VehicleTransmission& tr = m_ctrl->GetTransmission();
            tr.mShiftUpRPM   = m_redlineRPM *
                (kShiftUpLightFrac   + (kShiftUpFrac   - kShiftUpLightFrac)   * load);
            tr.mShiftDownRPM = m_redlineRPM *
                (kShiftDownLightFrac + (kShiftDownFrac - kShiftDownLightFrac) * load);
        }
        // ANTI-SPIN (BEFORE the step). Clamp each wheel to ~10% slip so the
        // engine's semi-implicit solve sees the clamped wheel, not the free-rev.
        // Running it in postStep left the engine RPM one step stale — the wheel
        // still spun 28% at speed, the power went to smoke, and the car never
        // reached 6th. Here it bites before the solve.
        {
            const float av = std::fabs(forwardSpeed());
            if (av > 0.5f && m_constraint) {
                for (uint32_t i = 0; i < m_wheelCount; ++i) {
                    JPH::Wheel* w = m_constraint->GetWheel(i);
                    if (!w) continue;
                    const float maxOmega = av * 1.10f / m_wheelRadius[i];
                    const float ww = w->GetAngularVelocity();
                    if (std::fabs(ww) > maxOmega)
                        w->SetAngularVelocity(ww > 0.0f ? maxOmega : -maxOmega);
                }
            }
        }
        // Wake the body so the constraint solves (a parked car sleeps).
        if (m_system && (std::fabs(m_in.throttle) > 0.01f || m_in.brake > 0.01f ||
                         std::fabs(m_in.steer) > 0.01f || m_in.handBrake > 0.01f)) {
            m_system->GetBodyInterface().ActivateBody(m_chassis->GetID());
        }
        m_ctrl->SetDriverInput(m_in.throttle, m_in.steer, m_in.brake, m_in.handBrake);

        // AERO DRAG. The wheeled car had NONE, so top speed was rev-limiter
        // limited: in 6th the engine pinned 7500 and bounced — the "constant
        // shifting past 6th" Tim hears. Quadratic drag makes top speed drag-
        // limited below redline, like a real car (a real engine never hits the
        // limiter while accelerating). F = -c |v| v; c ~1.4 => ~160 mph in 6th.
        if (!m_aeroSuspended) {
            const float kAeroDrag = 1.4f;
            JPH::Vec3 vel = m_chassis->GetLinearVelocity();
            const float spd = vel.Length();
            if (spd > 0.5f)
                m_chassis->AddForce(-vel * (kAeroDrag * spd));
        }

        // AERO DOWNFORCE — the spoiler (owner, 2026-08-16: "Can we
        // substantially increase the 'stick on the road' idea?... and spoilers
        // for downforce"). F = k*v^2 pressing the body onto the road along the
        // chassis's own -up (so a banked road is pressed INTO, not just world-
        // down), applied at a point kDownforceRearOffset BEHIND the center of
        // mass — that is what a rear wing does: it loads the rear axle. NOTE
        // no new IPhysicsWorld API was needed (grep receipt, NO_SLOP rule 1):
        // JPH::Body::AddForce(force, position) is Jolt's own apply-at-point,
        // and this controller already holds the raw chassis body.
        //
        // Sizing (MASS-RELATIVE so car_mass keeps the character; MEASURED in
        // the --test-vehicle handling section):
        //   F = scale * m*g * min(kDownforceFrac70 * (v/31.3)^2, kDownforceCap)
        //   -> 0.35x weight at 70 mph, 0.71x at 100, capped 1.10x from 124 mph.
        // The cap doubles the wheel load at speed WITHOUT doubling the lateral
        // force needed to tip: rollover threshold rises by the same (1 + F/mg)
        // factor as the grip does, so the margin engineered at the CoM comment
        // in vehicle.cpp is preserved, not consumed. `car_downforce` scales it
        // live (0 = spoiler off); WheeledTuning::downforce is the plumbing.
        {
            const float v = forwardSpeed();      // the wing sees axial airflow
            if (std::fabs(v) > 3.0f && m_downforceScale > 0.0f && !m_aeroSuspended) {
                constexpr float kDownforceFrac70    = 0.35f;   // x weight at 70 mph
                constexpr float kDownforceCap       = 1.10f;   // x weight, max
                constexpr float kV70                = 31.29f;  // 70 mph in m/s
                constexpr float kDownforceRearOffset = 0.35f;  // m behind CoM
                const float invM = m_chassis->GetMotionProperties()->GetInverseMass();
                const float mass = invM > 1e-9f ? 1.0f / invM : 0.0f;
                const float frac = std::min(kDownforceFrac70 * (v / kV70) * (v / kV70),
                                            kDownforceCap);
                const float mag  = m_downforceScale * mass * kGravity * frac;
                const JPH::Quat rot = m_chassis->GetRotation();
                const JPH::Vec3 down = -(rot * m_localUp);
                const JPH::RVec3 at  = m_chassis->GetCenterOfMassPosition() +
                                       rot * (-m_localForward * kDownforceRearOffset);
                m_chassis->AddForce(down * mag, at);

                // THE WING'S DRAG (W-HANDLING3, 2026-08-17). A wing that makes
                // downforce and costs NOTHING is free lunch, and the receipt
                // for it was sitting in the top-speed number: with the 0.50
                // overdrive un-capping the gearing, the drag-limited terminal
                // came out at 198 mph — 45 mph ABOVE the car's own stated ~155,
                // because the only drag in the sim was the body's kAeroDrag and
                // the spoiler was pure lift. Real aero is a TRADE: downforce
                // buys grip and it costs top end, which is exactly the choice
                // `car_downforce` should expose.
                //   D_wing = L / (L/D)
                // L/D 3.0 is a road-car aero package (an add-on rear wing plus
                // splitter runs ~2.5-3.5; an F1 wing ~4). NOT tuned to hit a
                // target speed — the speed is whatever this falls out as, and
                // --test-vehicle H2/H3 report it. PAIRED with kDownforceFrac70
                // above and with kAeroDrag: all three set the terminal velocity
                // (NO_SLOP rule 4).
                constexpr float kWingLoverD = 3.0f;
                const JPH::Vec3 vel = m_chassis->GetLinearVelocity();
                const float spd = vel.Length();
                if (spd > 0.5f)
                    m_chassis->AddForce(vel * (-(mag / kWingLoverD) / spd));
            }
        }

        // ROLL-RATE DAMPING (flip resistance, part 2). A violent slalom or a
        // curb strike at speed pumps roll faster than springs+ARBs can absorb;
        // this bleeds ROLL RATE (torque = -c * w_roll about the chassis forward
        // axis) so the transient never accumulates into a rollover. Gated on
        // >= 3 wheels touching: an airborne or two-wheeling car is left to real
        // physics (and the recovery), so jumps still fly and a genuine tip
        // still tips. This is THE safe tool at 60 Hz — stiffer anti-roll bars
        // are NOT (>= 15 kN/m the solver pumps the roll mode and flips the car;
        // measured, see WheeledVehicleDesc::antiRollFront). `car_rolldamp`.
        if (m_rollDamp > 0.0f && m_constraint) {
            int grounded = 0;
            for (uint32_t i = 0; i < m_wheelCount; ++i) {
                const JPH::Wheel* w = m_constraint->GetWheel(i);
                if (w && w->HasContact()) ++grounded;
            }
            if (grounded >= 3) {
                const JPH::Vec3 fwdW = m_chassis->GetRotation() * m_localForward;
                const float rollRate = m_chassis->GetAngularVelocity().Dot(fwdW);
                m_chassis->AddTorque(fwdW * (-m_rollDamp * rollRate));
            }
        }
    }

    void postStep(float) override {
        // TIRE-FORCE TELEMETRY (X3_VEHDBG=1) — the measuring instrument that
        // found the 2026-08-16 "doesn't like to turn" root cause. Per wheel:
        // the ACTUAL steer angle Jolt applied and the solver's suspension /
        // lateral impulses (lambda, N*s per step; force = lambda * 60). Reads
        // post-solve state, logs at 2 Hz, costs nothing when the env is unset.
        static const bool dbg = [] { const char* e = std::getenv("X3_VEHDBG");
                                     return e && e[0] == '1'; }();
        if (dbg && m_constraint) {
            if (++m_dbgTick % 30 == 0) {
                std::string s = "[vehdbg]";
                for (uint32_t i = 0; i < m_wheelCount; ++i) {
                    const JPH::Wheel* w = m_constraint->GetWheel(i);
                    if (!w) continue;
                    s += " w" + std::to_string(i) +
                         "(steer " + std::to_string(w->GetSteerAngle()) +
                         " susp " + std::to_string(w->GetSuspensionLambda()) +
                         " lat "  + std::to_string(w->GetLateralLambda()) +
                         (w->HasContact() ? ")" : " AIR)");
                }
                x3::logInfo(s);
            }
        }
    }
    void update(float dt) override { preStep(dt); postStep(dt); }

    BodyId body() const override { return m_bodyId; }
    VehicleKind kind() const override { return VehicleKind::Wheeled; }

    float forwardSpeed() const override {
        if (!m_chassis) return 0.0f;
        JPH::Vec3 v = m_chassis->GetLinearVelocity();
        JPH::Vec3 fwdWorld = m_chassis->GetRotation() * m_localForward;
        return v.Dot(fwdWorld);
    }

    uint32_t wheelCount() const override { return m_wheelCount; }

    bool wheelState(uint32_t i, WheelState& out) const override {
        if (!m_constraint || i >= m_wheelCount) return false;
        // World transform of the wheel cylinder (aligned with +Y, radius/width 1
        // after we bake the scale). GetWheelWorldTransform maps a unit Y-cylinder
        // to the wheel's pose; we add the radius/width scale so the renderer can
        // draw a unit cylinder/box. wheelRight in wheel model space is +X, up +Y.
        JPH::RMat44 m = m_constraint->GetWheelWorldTransform(
            i, JPH::Vec3::sAxisX(), JPH::Vec3::sAxisY());
        // Bake scale: x,z = radius (diameter handled by mesh), y = half width.
        const float r = m_wheelRadius[i];
        const float hw = m_wheelWidth[i] * 0.5f;
        JPH::Vec3 c0 = m.GetColumn3(0) * r;
        JPH::Vec3 c1 = m.GetColumn3(1) * hw;
        JPH::Vec3 c2 = m.GetColumn3(2) * r;
        JPH::RVec3 t = m.GetTranslation();
        float* o = out.worldTransform;
        o[0]=c0.GetX(); o[1]=c0.GetY(); o[2]=c0.GetZ(); o[3]=0.0f;
        o[4]=c1.GetX(); o[5]=c1.GetY(); o[6]=c1.GetZ(); o[7]=0.0f;
        o[8]=c2.GetX(); o[9]=c2.GetY(); o[10]=c2.GetZ(); o[11]=0.0f;
        o[12]=(float)t.GetX(); o[13]=(float)t.GetY(); o[14]=(float)t.GetZ(); o[15]=1.0f;
        out.radius = r; out.width = m_wheelWidth[i];
        const JPH::Wheel* w = m_constraint->GetWheel(i);
        out.hasContact = w->HasContact();
        out.suspensionLength = w->GetSuspensionLength();
        return true;
    }

    float engineRPM() const override {
        return m_ctrl ? m_ctrl->GetEngine().GetCurrentRPM() : 0.0f;
    }
    float lockedRPM() const override {
        if (!m_ctrl || !m_chassis) return 0.0f;
        const float ratio = m_ctrl->GetTransmission().GetCurrentRatio();
        if (ratio == 0.0f) return 0.0f;
        const float r = m_wheelRadius.empty() ? 0.33f : m_wheelRadius[0];
        return std::fabs(forwardSpeed()) / r * std::fabs(ratio) * m_finalDrive * 9.5493f;
    }
    int gear() const override {
        return m_ctrl ? m_ctrl->GetTransmission().GetCurrentGear() : 0;
    }

    float driveForce() const override {
        if (!m_constraint) return 0.0f;
        // Solver longitudinal impulses (N*s) over the fixed 60 Hz step -> N.
        // Positive = pushing the car along its forward axis.
        float lambda = 0.0f;
        for (uint32_t i = 0; i < m_wheelCount; ++i) {
            const JPH::Wheel* w = m_constraint->GetWheel(i);
            if (w && w->HasContact()) lambda += w->GetLongitudinalLambda();
        }
        return lambda * 60.0f;
    }

    float longitudinalSlip(uint32_t i) const override {
        if (!m_constraint || i >= m_wheelCount) return 0.0f;
        const JPH::Wheel* w = m_constraint->GetWheel(i);
        if (!w || !w->HasContact()) return 0.0f;
        const float surface = w->GetAngularVelocity() * m_wheelRadius[i];
        const float v = forwardSpeed();
        // Proper slip RATIO. The old max(|v|, 1.0) returned m/s (not a ratio)
        // below 1 m/s, so a wheel just starting to rotate read as huge slip and
        // tripped TC on every launch. Slip is meaningless at rest — report 0.
        if (std::fabs(v) < 0.1f) return 0.0f;
        return (surface - v) / std::fabs(v);
    }

    // ---- LIVE TUNING (performance shop). Mutates the running Jolt settings in
    // place — the simulation reads the settings objects every step, so the next
    // physics tick already drives with the new engine/tires/suspension/brakes. ----
    bool applyWheeledTuning(const WheeledTuning& t) override {
        if (!m_ctrl || !m_chassis) return false;
        JPH::VehicleEngine& eng = m_ctrl->GetEngine();
        // Engine peak torque + redline. m_baseMaxTorque is the TUNED baseline the
        // nitrous boost multiplies (so boost never compounds with itself).
        if (t.maxEngineTorque > 0.0f) {
            m_baseMaxTorque = t.maxEngineTorque;
            eng.mMaxTorque  = m_baseMaxTorque * m_boost;
        }
        if (t.maxEngineRPM > 0.0f) {
            eng.mMaxRPM = t.maxEngineRPM;
            m_redlineRPM = t.maxEngineRPM;   // the adaptive shift band re-derives from this
            // Keep the shift points tied to the NEW redline — a race cam that
            // raises the limiter has to move the shift band with it, or the box
            // upshifts mid-powerband and gives the cam away.
            // VehicleTransmission derives from VehicleTransmissionSettings, so the
            // live transmission takes the same helper as the build-time settings.
            applyShiftPoints(m_ctrl->GetTransmission(), t.maxEngineRPM);
        }
        // Normalized torque CURVE (camshaft / forced-induction profile).
        if (t.curvePoints > 0) {
            eng.mNormalizedTorque.Clear();
            const uint32_t nPts = std::min<uint32_t>(t.curvePoints, 8u);
            for (uint32_t i = 0; i < nPts; ++i)
                eng.mNormalizedTorque.AddPoint(t.curve[i].rpmFrac, t.curve[i].torqueFrac);
            eng.mNormalizedTorque.Sort();
        }
        // Chassis mass (inertia rescaled with it).
        if (t.massKg > 0.0f && m_chassis->GetMotionProperties())
            m_chassis->GetMotionProperties()->ScaleToMass(t.massKg);
        // FINAL DRIVE, live. Jolt reads the differential settings every step, so
        // mutating the ratio in place re-gears the running car — shorter (bigger)
        // = more wheel torque in every gear, lower top speed. m_finalDrive must
        // follow or lockedRPM() (the audio engine model) lies about wheel rpm.
        if (t.finalDrive > 0.0f) {
            for (JPH::VehicleDifferentialSettings& diff : m_ctrl->GetDifferentials())
                diff.mDifferentialRatio = t.finalDrive;
            m_finalDrive = t.finalDrive;
        }
        // AERO DOWNFORCE scale + ROLL-RATE DAMPING (see WheeledTuning and the
        // preStep blocks that consume them). Sentinel < 0 leaves; 0 disables.
        if (t.downforce >= 0.0f) m_downforceScale = t.downforce;
        if (t.rollDamp  >= 0.0f) m_rollDamp       = t.rollDamp;
        // LATERAL BREAKAWAY SHAPE + LATERAL-ONLY GRIP (see WheeledTuning):
        // remembered so a later grip change re-applies the same shape/cap.
        // Everything tire is re-derived from the authored baselines below
        // (idempotent — re-applying a tuning never compounds).
        if (t.latTail > 0.0f)      m_latTail = t.latTail;
        if (t.latGripScale > 0.0f) m_latNow  = t.latGripScale;
        // ANTI-ROLL BARS, live. The constraint reads the bar array every step;
        // match each bar to its axle by its left wheel's steered flag.
        if ((t.antiRollFront > 0.0f || t.antiRollRear > 0.0f) && m_constraint) {
            for (JPH::VehicleAntiRollBar& bar : m_constraint->GetAntiRollBars()) {
                const bool front = bar.mLeftWheel >= 0 &&
                                   bar.mLeftWheel < (int)m_wheelSteered.size() &&
                                   m_wheelSteered[bar.mLeftWheel];
                if (front  && t.antiRollFront > 0.0f) bar.mStiffness = t.antiRollFront;
                if (!front && t.antiRollRear  > 0.0f) bar.mStiffness = t.antiRollRear;
            }
        }
        // Per-wheel: tires / suspension / ride height / brakes. All scale or offset
        // from the AUTHORED baseline captured in build() (grip multipliers are
        // RELATIVE to each wheel's stock compound — see WheeledTuning::gripScale),
        // so re-application after a parts change is idempotent.
        const bool reshapeTires = t.gripScale > 0.0f || t.gripScaleFront > 0.0f ||
                                  t.gripScaleRear > 0.0f || t.latTail > 0.0f ||
                                  t.latGripScale > 0.0f;
        for (size_t i = 0; i < m_wheelSettings.size(); ++i) {
            JPH::WheelSettingsWV* ws = m_wheelSettings[i];
            if (reshapeTires && i < m_baseLongFriction.size()) {
                // Resolve this wheel's new relative grip: all-wheel gripScale
                // first, then the per-axle override (steered = front axle).
                float g = m_gripNow[i];
                if (t.gripScale > 0.0f) g = t.gripScale;
                const bool front = i < m_wheelSteered.size() && m_wheelSteered[i];
                if (front  && t.gripScaleFront > 0.0f) g = t.gripScaleFront;
                if (!front && t.gripScaleRear  > 0.0f) g = t.gripScaleRear;
                m_gripNow[i] = g;
                ws->mLongitudinalFriction = m_baseLongFriction[i];
                for (auto& p : ws->mLongitudinalFriction.mPoints) p.mY *= g;
                // Lateral = authored lateral baseline x grip x the lateral-only
                // dial (the baseline already carries the rollover-bounded cap
                // and the front/rear turn-in split — see vehicle.cpp).
                ws->mLateralFriction = m_baseLatFriction[i];
                for (auto& p : ws->mLateralFriction.mPoints) p.mY *= g * m_latNow;
                // Reshape the lateral tail: the LAST point is Jolt's high-slip
                // plateau (20 deg, 0.83x peak in the stock shape). >1 = flatter
                // falloff past the peak (a slide keeps its grip — easy to catch
                // with countersteer); <1 = sharper breakaway (drift-happy).
                if (m_latTail != 1.0f && !ws->mLateralFriction.mPoints.empty())
                    ws->mLateralFriction.mPoints.back().mY *= m_latTail;
            }
            if (t.suspensionFreq > 0.0f) ws->mSuspensionSpring.mFrequency = t.suspensionFreq;
            if (t.suspensionDamp > 0.0f) ws->mSuspensionSpring.mDamping   = t.suspensionDamp;
            if (t.rideHeightDelta != WheeledTuning::kRideHeightLeave) {
                ws->mSuspensionMinLength = std::max(0.03f, m_baseSuspMin[i] + t.rideHeightDelta);
                ws->mSuspensionMaxLength = std::max(ws->mSuspensionMinLength + 0.05f,
                                                    m_baseSuspMax[i] + t.rideHeightDelta);
            }
            if (t.brakeTorque > 0.0f) {
                ws->mMaxBrakeTorque = t.brakeTorque;
                if (ws->mMaxHandBrakeTorque > 0.0f)         // keep the hand-brake lock ratio
                    ws->mMaxHandBrakeTorque = t.brakeTorque * 2.5f;
            }
        }
        // Wake the body so the re-tuned constraint solves immediately.
        if (m_system) m_system->GetBodyInterface().ActivateBody(m_chassis->GetID());
        return true;
    }

    void setTorqueBoost(float mult) override {
        m_boost = std::clamp(mult, 0.1f, 4.0f);
        if (m_ctrl) m_ctrl->GetEngine().mMaxTorque = m_baseMaxTorque * m_boost;
    }
    float torqueBoost() const override { return m_boost; }

private:
    JPH::PhysicsSystem* m_system = nullptr;
    JPH::Body*          m_chassis = nullptr;
    JPH::Ref<JPH::VehicleConstraint>          m_constraint;   // we hold a Ref
    JPH::WheeledVehicleController*             m_ctrl = nullptr; // owned by constraint
    JPH::Ref<JPH::VehicleCollisionTesterRay>  m_tester;        // we hold a Ref
    BodyId   m_bodyId;
    JPH::Vec3 m_localForward = JPH::Vec3(0, 0, -1);
    JPH::Vec3 m_localUp      = JPH::Vec3(0, 1, 0);
    uint32_t m_wheelCount = 0;
    std::vector<float> m_wheelRadius, m_wheelWidth;
    VehicleInput m_in;
    // ---- Live-tuning bookkeeping (applyWheeledTuning / setTorqueBoost) ----
    std::vector<JPH::WheelSettingsWV*> m_wheelSettings; // owned by the constraint's Refs
    std::vector<float> m_baseSuspMin, m_baseSuspMax;    // authored suspension lengths
    std::vector<JPH::LinearCurve> m_baseLongFriction, m_baseLatFriction; // per-wheel AUTHORED curves
    std::vector<bool>  m_wheelSteered;                  // steered = front axle (per-axle grip/ARB)
    std::vector<float> m_gripNow;                       // current grip multiplier per wheel (stock 1)
    float m_latNow  = 1.0f;                             // lateral-only grip multiplier (stock 1)
    float m_latTail = 1.0f;                             // lateral high-slip plateau multiplier
    float m_baseMaxTorque = 600.0f;                     // tuned baseline (boost multiplies)
    float m_finalDrive = 1.0f;                          // final-drive ratio (for the locked-RPM clamp)
    float m_boost = 1.0f;                               // nitrous multiplier (1 = none)
    float m_redlineRPM = kDefaultRedlineRPM;            // adaptive shift band re-derives from this
    float m_thrSm = 0.0f;                               // smoothed throttle (shift-band slider)
    bool  m_aeroSuspended  = false;   // set while the car is FLYING (see setAeroSuspended)
    float m_downforceScale = 1.0f;                      // spoiler scale (see preStep DOWNFORCE)
    // Roll-rate damping default (N*m*s/rad). 2000 on the ~1083 kg hero car
    // bleeds a slalom's roll transient in ~0.25 s without deadening body
    // motion; the --test-vehicle handling section A/Bs 0 vs this and gates on
    // the shipped value. `car_rolldamp` tunes it live.
    float m_rollDamp = 2000.0f;
    uint32_t m_dbgTick = 0;                             // X3_VEHDBG log cadence
};

// =====================================================================
// BUOYANCY — Archimedes + quadratic drag vs a flat water plane
// =====================================================================
class BuoyancyController final : public IVehicleController {
public:
    bool build(IPhysicsWorld& world, const BuoyancyDesc& d) {
        m_system = static_cast<JPH::PhysicsSystem*>(world.nativeSystem());
        m_body   = static_cast<JPH::Body*>(world.nativeBody(d.body));
        if (!m_system || !m_body) {
            x3::logError("[vehicle] buoyancy: invalid body / system");
            return false;
        }
        m_d = d;
        m_bodyId = d.body;
        return true;
    }

    void setInput(const VehicleInput& in) override { m_in = in; }

    // The water surface can MOVE under the hull (a river descends downstream
    // and swells in rain) — see IVehicleController::setSeaLevel. Everything
    // below reads m_d.seaLevel fresh each preStep, so this is the whole fix.
    bool setSeaLevel(float y) override { m_d.seaLevel = y; return true; }

    // Forces are applied each fixed step. We add them in preStep so the upcoming
    // world step integrates them.
    void preStep(float dt) override {
        if (!m_body || dt <= 0.0f) return;
        // Keep it awake — a settled floater would otherwise sleep and stop bobbing.
        m_system->GetBodyInterface().ActivateBody(m_body->GetID());

        const float hx = m_d.halfExtents[0], hy = m_d.halfExtents[1], hz = m_d.halfExtents[2];
        const float fullVol = 8.0f * hx * hy * hz;
        JPH::RVec3 pos = m_body->GetCenterOfMassPosition();
        const float cy = (float)pos.GetY();

        // Submerged depth of the (axis-aligned-approximated) box. Bottom at cy-hy,
        // top at cy+hy. depth = how far the surface is above the bottom, clamped.
        const float bottom = cy - hy, top = cy + hy;
        float submergedH = 0.0f;
        if (m_d.seaLevel >= top)        submergedH = 2.0f * hy;       // fully under
        else if (m_d.seaLevel <= bottom) submergedH = 0.0f;          // fully above
        else                             submergedH = m_d.seaLevel - bottom;
        m_submergedFrac = (hy > 1e-5f) ? (submergedH / (2.0f * hy)) : 0.0f;
        m_submergedFrac = std::clamp(m_submergedFrac, 0.0f, 1.0f);

        if (submergedH > 0.0f) {
            const float submergedVol = (submergedH / (2.0f * hy)) * fullVol;
            // Archimedes: F = rho * g * V, upward.
            float fB = m_d.fluidDensity * kGravity * submergedVol;
            // Apply at the submerged-region centroid (below COM) so it self-rights
            // a little (a restoring moment that keeps the boat level).
            JPH::Vec3 buoy(0.0f, fB, 0.0f);
            m_body->AddForce(buoy);

            // Linear drag in water: F = -c * rho/1000 * V_frac * m * v  (scaled by
            // submerged fraction so a barely-wet hull is barely damped).
            JPH::Vec3 v = m_body->GetLinearVelocity();
            float invM = m_body->GetMotionProperties()->GetInverseMass();
            float mass = invM > 1e-9f ? 1.0f / invM : 1.0f;
            float frac = submergedVol / fullVol;
            m_body->AddForce(-v * (m_d.linearDrag * frac * mass));

            // Angular drag (water resists rotation): torque = -c * frac * w * I~.
            JPH::Vec3 w = m_body->GetAngularVelocity();
            m_body->AddTorque(-w * (m_d.angularDrag * frac * mass));

            // Dive / surface thrust (a sub): vertical force scaled by input.
            if (m_d.diveThrust != 0.0f && std::fabs(m_in.dive) > 1e-3f) {
                m_body->AddForce(JPH::Vec3(0.0f, m_d.diveThrust * m_in.dive * frac, 0.0f));
            }
            // Forward propulsion + steering for a powered boat.
            if (m_d.propThrust != 0.0f && std::fabs(m_in.throttle) > 1e-3f) {
                JPH::Vec3 fwd = m_body->GetRotation() * JPH::Vec3(0, 0, -1);
                fwd.SetY(0.0f); fwd = norm(fwd);
                m_body->AddForce(fwd * (m_d.propThrust * m_in.throttle * frac));
            }
            if (m_d.steerTorque != 0.0f && std::fabs(m_in.steer) > 1e-3f) {
                m_body->AddTorque(JPH::Vec3(0.0f, -m_d.steerTorque * m_in.steer * frac, 0.0f));
            }
            // SWELL + METACENTRIC RIGHTING (see BuoyancyDesc): the swell is a
            // gentle periodic roll about the hull's horizontal forward axis +
            // a smaller off-phase pitch about its horizontal right axis, so
            // the hull genuinely rocks on the water. The righting term is the
            // linearized self-righting moment (-k * attitude) the COM-applied
            // buoyancy force lacks — without it any transient leaves a
            // PERMANENT list (zero-mean torque bounds angular velocity, not
            // angle) and the swell would rock about the list, not about level.
            if (m_d.swellTorque != 0.0f || m_d.rightingTorque != 0.0f) {
                JPH::Vec3 fwdH = m_body->GetRotation() * JPH::Vec3(0, 0, -1);
                fwdH.SetY(0.0f);
                if (fwdH.LengthSq() > 1e-6f) {
                    fwdH = norm(fwdH);
                    JPH::Vec3 rightH(-fwdH.GetZ(), 0.0f, fwdH.GetX());
                    JPH::Vec3 tq = JPH::Vec3::sZero();
                    if (m_d.swellTorque != 0.0f) {
                        m_swellT += dt;
                        const float w1 = 2.0f * 3.14159265f * m_d.swellFreqHz;
                        const float roll  = std::sin(w1 * m_swellT);
                        const float pitch = 0.4f * std::sin(0.63f * w1 * m_swellT + 1.3f);
                        tq += (fwdH * roll + rightH * pitch) * m_d.swellTorque;
                    }
                    if (m_d.rightingTorque != 0.0f) {
                        JPH::Vec3 up  = m_body->GetRotation() * JPH::Vec3(0, 1, 0);
                        JPH::Vec3 fwd = m_body->GetRotation() * JPH::Vec3(0, 0, -1);
                        const float sinRoll  = up.Dot(rightH);   // + = starboard lean
                        const float sinPitch = fwd.GetY();       // + = nose up
                        tq += fwdH   * (-m_d.rightingTorque * sinRoll);
                        tq += rightH * (-m_d.rightingTorque * sinPitch);
                    }
                    m_body->AddTorque(tq * frac);
                }
            }
        }
    }

    void postStep(float) override {}
    void update(float dt) override { preStep(dt); postStep(dt); }

    BodyId body() const override { return m_bodyId; }
    VehicleKind kind() const override { return VehicleKind::Buoyancy; }
    float forwardSpeed() const override {
        if (!m_body) return 0.0f;
        JPH::Vec3 v = m_body->GetLinearVelocity();
        JPH::Vec3 fwd = m_body->GetRotation() * JPH::Vec3(0, 0, -1);
        return v.Dot(fwd);
    }
    float submergedFraction() const override { return m_submergedFrac; }

private:
    JPH::PhysicsSystem* m_system = nullptr;
    JPH::Body*          m_body = nullptr;
    BuoyancyDesc        m_d;
    BodyId              m_bodyId;
    VehicleInput        m_in;
    float               m_submergedFrac = 0.0f;
    float               m_swellT = 0.0f;   // swell phase clock (s)
};

// =====================================================================
// FLIGHT — thrust + lift + drag + attitude control torques
// =====================================================================
class FlightController final : public IVehicleController {
public:
    bool build(IPhysicsWorld& world, const FlightDesc& d) {
        m_system = static_cast<JPH::PhysicsSystem*>(world.nativeSystem());
        m_body   = static_cast<JPH::Body*>(world.nativeBody(d.body));
        if (!m_system || !m_body) {
            x3::logError("[vehicle] flight: invalid body / system");
            return false;
        }
        m_d = d;
        m_bodyId = d.body;
        m_localForward = norm(V3(d.forward));
        m_localUp      = norm(V3(d.up));
        // A space-craft turns off gravity; an atmospheric plane keeps it on.
        if (!d.gravity)
            m_body->GetMotionProperties()->SetGravityFactor(0.0f);
        return true;
    }

    void setInput(const VehicleInput& in) override { m_in = in; }

    void preStep(float dt) override {
        if (!m_body || dt <= 0.0f) return;
        m_system->GetBodyInterface().ActivateBody(m_body->GetID());

        JPH::Quat rot = m_body->GetRotation();
        JPH::Vec3 fwd = rot * m_localForward;
        JPH::Vec3 up  = rot * m_localUp;
        JPH::Vec3 v   = m_body->GetLinearVelocity();
        float invM = m_body->GetMotionProperties()->GetInverseMass();
        float mass = invM > 1e-9f ? 1.0f / invM : 1.0f;

        // Thrust along the nose.
        float thr = std::clamp(m_in.throttle, 0.0f, 1.0f);
        m_body->AddForce(fwd * (m_d.maxThrust * thr));

        // Forward airspeed drives lift. Lift acts along the airframe up vector,
        // proportional to forward speed (a simple, stable analogue of v^2 lift
        // clamped so it can't explode): L = liftCoefficient * |fwdSpeed| * mass,
        // capped near 1.2*g*mass so it holds the plane up but doesn't rocket it.
        float fwdSpeed = v.Dot(fwd);
        float lift = m_d.liftCoefficient * std::fabs(fwdSpeed) * mass;
        float maxLift = 1.25f * kGravity * mass;
        if (lift > maxLift) lift = maxLift;
        if (m_d.gravity) m_body->AddForce(up * lift);

        // Quadratic drag opposing velocity.
        float speed = v.Length();
        if (speed > 1e-4f)
            m_body->AddForce(v * (-m_d.linearDrag * speed));

        // Attitude control torques in the airframe's local axes:
        //   pitch about local right (fwd x up), yaw about local up, roll about fwd.
        JPH::Vec3 right = norm(fwd.Cross(up));
        JPH::Vec3 torque = JPH::Vec3::sZero();
        torque += right * (m_d.pitchTorque * std::clamp(m_in.pitch, -1.0f, 1.0f));
        torque += up    * (m_d.yawTorque   * std::clamp(m_in.steer, -1.0f, 1.0f));
        torque += fwd   * (m_d.rollTorque   * std::clamp(m_in.roll,  -1.0f, 1.0f));
        m_body->AddTorque(torque);

        // Angular damping so the craft settles instead of tumbling forever.
        JPH::Vec3 w = m_body->GetAngularVelocity();
        m_body->AddTorque(-w * (m_d.angularDamping * mass));
    }

    void postStep(float) override {}
    void update(float dt) override { preStep(dt); postStep(dt); }

    BodyId body() const override { return m_bodyId; }
    VehicleKind kind() const override { return VehicleKind::Flight; }
    float forwardSpeed() const override {
        if (!m_body) return 0.0f;
        JPH::Vec3 v = m_body->GetLinearVelocity();
        JPH::Vec3 fwd = m_body->GetRotation() * m_localForward;
        return v.Dot(fwd);
    }

private:
    JPH::PhysicsSystem* m_system = nullptr;
    JPH::Body*          m_body = nullptr;
    FlightDesc          m_d;
    BodyId              m_bodyId;
    JPH::Vec3           m_localForward = JPH::Vec3(0, 0, -1);
    JPH::Vec3           m_localUp      = JPH::Vec3(0, 1, 0);
    VehicleInput        m_in;
};

} // namespace

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------
IVehicleController* createWheeledVehicle(IPhysicsWorld& world, const WheeledVehicleDesc& desc) {
    auto* c = new WheeledController();
    if (!c->build(world, desc)) { delete c; return nullptr; }
    return c;
}

IVehicleController* createBuoyancyController(IPhysicsWorld& world, const BuoyancyDesc& desc) {
    auto* c = new BuoyancyController();
    if (!c->build(world, desc)) { delete c; return nullptr; }
    return c;
}

IVehicleController* createFlightController(IPhysicsWorld& world, const FlightDesc& desc) {
    auto* c = new FlightController();
    if (!c->build(world, desc)) { delete c; return nullptr; }
    return c;
}

// ===========================================================================
// Self-test (--test-vehicle): V1 wheeled accelerates, V2 wheeled steers,
// V3 buoyant body settles near the waterline, V4 flight thrust -> forward speed.
// ===========================================================================
namespace {
int vg_pass = 0, vg_fail = 0;
void vcheck(bool cond, const char* name) {
    if (cond) { ++vg_pass; x3::logInfo(std::string("[vehicle-test] PASS ") + name); }
    else      { ++vg_fail; x3::logError(std::string("[vehicle-test] FAIL ") + name); }
}
constexpr float kDt = 1.0f / 60.0f;

BodyId makeFlatGround(IPhysicsWorld* w, float half = 200.0f) {
    float v[] = {
        -half, 0.0f, -half,  half, 0.0f, -half,
         half, 0.0f,  half, -half, 0.0f,  half,
    };
    uint32_t idx[] = { 0,2,1, 0,3,2 };   // CCW -> +Y normal
    return w->addStaticMesh(v, 4, idx, 6);
}

// Build a standard 4-wheel car desc on a chassis box.
void fillCarWheels(std::vector<WheelDesc>& wheels, float hx, float hy, float hz) {
    wheels.clear();
    // Front-left, front-right, rear-left, rear-right (fronts steer; rears powered +
    // handbrake = rear-wheel drive). Suspension is attached at the CHASSIS BOTTOM
    // (local y = -hy) and hangs down, so at rest the springs hold the body's belly
    // clearly above the ground (no belly-scrape -> the wheels keep traction).
    struct P { float x, z; bool steer, hb; };
    P p[4] = {
        { -hx, -hz, true,  false }, {  hx, -hz, true,  false },  // front (-Z)
        { -hx,  hz, false, true  }, {  hx,  hz, false, true  },  // rear  (+Z)
    };
    for (int i = 0; i < 4; ++i) {
        WheelDesc w;
        w.position[0] = p[i].x; w.position[1] = -hy; w.position[2] = p[i].z;
        w.radius = 0.35f; w.width = 0.25f;
        w.suspensionMin = 0.10f; w.suspensionMax = 0.35f;
        w.suspensionFreq = 2.0f; w.suspensionDamp = 0.6f;
        w.steered = p[i].steer; w.handBraked = p[i].hb;
        w.powered = !p[i].steer; // rear-wheel drive
        wheels.push_back(w);
    }
}
} // namespace

bool runVehicleSelfTest() {
    vg_pass = vg_fail = 0;

    // ---- V1: wheeled vehicle accelerates forward under throttle ----
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        makeFlatGround(w.get());
        const float hx = 0.9f, hy = 0.4f, hz = 1.8f;
        // Chassis spawned above the ground so the suspension settles the wheels onto
        // it (wheel reach = hy + suspensionMax + radius ~= 1.1 m below the center).
        BodyId chassis = w->addBox(Vec3{hx, hy, hz}, Vec3{0, 1.3f, 0}, 1500.0f, Layer::Dynamic);
        std::vector<WheelDesc> wheels; fillCarWheels(wheels, hx, hy, hz);
        WheeledVehicleDesc vd;
        vd.chassis = chassis; vd.wheels = wheels.data(); vd.wheelCount = (uint32_t)wheels.size();
        vd.maxEngineTorque = 800.0f;
        std::unique_ptr<IVehicleController> car(createWheeledVehicle(*w, vd));
        bool created = (car != nullptr);

        // Settle on suspension first (no input).
        for (int i = 0; i < 30; ++i) { car->setInput({}); car->preStep(kDt); w->step(kDt); }
        Vec3 startPos = w->getBodyPosition(chassis);

        // Full throttle forward for ~3s.
        VehicleInput in; in.throttle = 1.0f;
        for (int i = 0; i < 180; ++i) { car->setInput(in); car->preStep(kDt); w->step(kDt); }
        Vec3 endPos = w->getBodyPosition(chassis);
        float fwdSpeed = car->forwardSpeed();
        // Forward is local -Z, so the car should move toward -Z (endPos.z < startPos.z)
        // and report a positive forward speed.
        float dz = endPos.z - startPos.z;
        bool moved = dz < -2.0f;           // traveled at least 2m forward (-Z)
        bool hasSpeed = fwdSpeed > 1.5f;   // accelerating forward
        bool upright = endPos.y > 0.3f && endPos.y < 3.0f; // didn't sink or launch
        vcheck(created && moved && hasSpeed && upright,
               "V1 wheeled accelerates forward under throttle");
        x3::logInfo("[vehicle-test]   V1 dz=" + std::to_string(dz) +
                    " fwdSpeed=" + std::to_string(fwdSpeed) +
                    " y=" + std::to_string(endPos.y) + " rpm=" + std::to_string(car->engineRPM()));
        car.reset();
        w->shutdown();
    }

    // ---- V2: wheeled vehicle steers (heading changes under steer+throttle) ----
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        makeFlatGround(w.get());
        const float hx = 0.9f, hy = 0.4f, hz = 1.8f;
        BodyId chassis = w->addBox(Vec3{hx, hy, hz}, Vec3{0, 1.3f, 0}, 1500.0f, Layer::Dynamic);
        std::vector<WheelDesc> wheels; fillCarWheels(wheels, hx, hy, hz);
        WheeledVehicleDesc vd;
        vd.chassis = chassis; vd.wheels = wheels.data(); vd.wheelCount = (uint32_t)wheels.size();
        vd.maxEngineTorque = 800.0f;
        std::unique_ptr<IVehicleController> car(createWheeledVehicle(*w, vd));
        for (int i = 0; i < 30; ++i) { car->setInput({}); car->preStep(kDt); w->step(kDt); }

        float yaw0[4]; w->getBodyRotation(chassis, yaw0);
        // Throttle + full right steer for ~3.5s.
        VehicleInput in; in.throttle = 1.0f; in.steer = 1.0f;
        for (int i = 0; i < 210; ++i) { car->setInput(in); car->preStep(kDt); w->step(kDt); }
        float yaw1[4]; w->getBodyRotation(chassis, yaw1);
        // The Y component of the orientation quaternion should have changed (turned
        // about the up axis). Compare |qy| growth past noise.
        bool turned = std::fabs(yaw1[1] - yaw0[1]) > 0.03f;
        vcheck(turned, "V2 wheeled steers (heading changes)");
        x3::logInfo("[vehicle-test]   V2 qy0=" + std::to_string(yaw0[1]) +
                    " qy1=" + std::to_string(yaw1[1]));
        car.reset();
        w->shutdown();
    }

    // ---- V3: buoyant body settles near the waterline (no sink, no launch) ----
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        // A floor far below so a sinking body would obviously fail (not settle at sea).
        makeFlatGround(w.get(), 200.0f); // ground at y=0
        const float seaLevel = 20.0f;
        const float hx = 1.5f, hy = 0.6f, hz = 3.0f;
        const float fullVol = 8.0f * hx * hy * hz;        // 21.6 m^3
        const float fluidDensity = 1025.0f;               // sea water
        // Mass for a ~half-submerged equilibrium: at rest the buoyant force
        // (rho*g*submergedVol) balances weight (mass*g) => submergedVol = mass/rho.
        // For ~half of the hull submerged, mass = 0.5 * fullVol * rho.
        const float mass = 0.5f * fullVol * fluidDensity;  // ~11070 kg
        // Drop the boat from above the water so it splashes down + settles.
        BodyId boat = w->addBox(Vec3{hx, hy, hz}, Vec3{0, 24.0f, 0}, mass, Layer::Dynamic);
        BuoyancyDesc bd;
        bd.body = boat; bd.seaLevel = seaLevel;
        bd.halfExtents[0]=hx; bd.halfExtents[1]=hy; bd.halfExtents[2]=hz;
        bd.fluidDensity = fluidDensity;
        bd.linearDrag = 6.0f; bd.angularDrag = 4.0f;       // settle within a few seconds
        std::unique_ptr<IVehicleController> floatCtl(createBuoyancyController(*w, bd));
        bool created = (floatCtl != nullptr);

        // Simulate ~10s to settle (the heavy hull + drag converge to the waterline).
        for (int i = 0; i < 600; ++i) { floatCtl->setInput({}); floatCtl->preStep(kDt); w->step(kDt); }
        Vec3 p = w->getBodyPosition(boat);
        float frac = floatCtl->submergedFraction();
        // The body's center should sit NEAR the waterline (within ~hy of sea level
        // for a roughly half-submerged hull). NOT at the floor (y~0.6 = sank) and
        // NOT launched far above the surface.
        bool nearLine = std::fabs(p.y - seaLevel) < (hy + 0.6f);
        bool notSunk  = p.y > 2.0f;                 // didn't fall to the ground
        bool notLaunched = p.y < seaLevel + hy + 1.0f;
        bool someSubmerged = frac > 0.05f && frac <= 1.0f; // genuinely in the water
        vcheck(created && nearLine && notSunk && notLaunched && someSubmerged,
               "V3 buoyant body settles near the waterline");
        x3::logInfo("[vehicle-test]   V3 y=" + std::to_string(p.y) +
                    " seaLevel=" + std::to_string(seaLevel) +
                    " submergedFrac=" + std::to_string(frac));
        floatCtl.reset();
        w->shutdown();
    }

    // ---- V4: flight thrust produces forward speed; lift opposes gravity ----
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        // Aircraft starts high in the air with a forward push so lift can develop.
        BodyId plane = w->addBox(Vec3{2.0f, 0.5f, 3.0f}, Vec3{0, 100.0f, 0}, 1000.0f, Layer::Dynamic);
        // Give it an initial forward velocity (local -Z) so lift kicks in immediately.
        const float v0[3] = { 0.0f, 0.0f, -30.0f };
        w->setBodyLinearVelocity(plane, v0);
        FlightDesc fd;
        fd.body = plane;
        fd.maxThrust = 15000.0f; fd.liftCoefficient = 0.5f; fd.linearDrag = 0.4f;
        std::unique_ptr<IVehicleController> air(createFlightController(*w, fd));
        bool created = (air != nullptr);

        VehicleInput in; in.throttle = 1.0f;
        float yStart = w->getBodyPosition(plane).y;
        for (int i = 0; i < 120; ++i) { air->setInput(in); air->preStep(kDt); w->step(kDt); }
        float fwdSpeed = air->forwardSpeed();
        float yEnd = w->getBodyPosition(plane).y;
        // Thrust along -Z keeps/increases forward speed.
        bool flying = fwdSpeed > 25.0f;
        // With lift, the plane doesn't plummet: it loses less than free-fall would
        // (free fall over 2s ~ -19.6 m; lift should keep the drop far smaller).
        float drop = yStart - yEnd;
        bool liftHelps = drop < 15.0f;
        vcheck(created && flying && liftHelps,
               "V4 flight thrust -> forward speed + lift opposes gravity");
        x3::logInfo("[vehicle-test]   V4 fwdSpeed=" + std::to_string(fwdSpeed) +
                    " drop=" + std::to_string(drop));
        air.reset();
        w->shutdown();
    }

    x3::logInfo(std::string("[vehicle-test] ") + std::to_string(vg_pass) + " passed, " +
                std::to_string(vg_fail) + " failed");
    return vg_fail == 0;
}

} // namespace x3::phys
