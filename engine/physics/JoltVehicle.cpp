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
constexpr float kShiftUpFrac   = 0.975f;
// 0.50 -> 0.58. At 0.50 the box would not drop a gear until 3750 rpm, so
// flooring it while cruising in a tall gear just LUGGED — Tim, 2026-08-15: "The
// engine seems bogged and lugging, overall". Jolt has no throttle kickdown, only
// this rpm threshold, so it has to sit high enough to catch a real overtake.
// Ceiling: an upshift at 0.975 lands the next gear at 0.975/1.47 = 0.66 of
// redline, so anything at or above 0.66 would immediately re-upshift and hunt.
// 0.58 (4350 rpm) is eager without touching that.
constexpr float kShiftDownFrac = 0.58f;
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
            // authored gripScale (see WheelDesc::gripScale).
            if (w.gripScale > 0.0f && w.gripScale != 1.0f) {
                for (auto& p : ws->mLongitudinalFriction.mPoints) p.mY *= w.gripScale;
                for (auto& p : ws->mLateralFriction.mPoints)      p.mY *= w.gripScale;
            }
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
            if (i == 0) {
                m_baseLongFriction = ws->mLongitudinalFriction;
                m_baseLatFriction  = ws->mLateralFriction;
            }
        }

        // ---- Controller (engine + transmission + 1 differential) ----
        JPH::WheeledVehicleControllerSettings* cs = new JPH::WheeledVehicleControllerSettings();
        m_baseMaxTorque = d.maxEngineTorque;            // tuning/boost baseline
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

        // Powered wheels feed the differential. Pick the first two powered wheels
        // as the differential's left/right (a standard single-axle drive). If only
        // one wheel is powered, drive it alone.
        int leftPowered = -1, rightPowered = -1;
        for (uint32_t i = 0; i < d.wheelCount; ++i) {
            if (!d.wheels[i].powered) continue;
            if (leftPowered < 0) leftPowered = (int)i;
            else if (rightPowered < 0) { rightPowered = (int)i; break; }
        }
        if (leftPowered < 0) leftPowered = 0;               // fall back: drive wheel 0
        JPH::VehicleDifferentialSettings diff;
        diff.mLeftWheel  = leftPowered;
        diff.mRightWheel = (rightPowered >= 0) ? rightPowered : -1;
        if (d.finalDrive > 0.0f) diff.mDifferentialRatio = d.finalDrive;
        cs->mDifferentials.push_back(diff);
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
                        "]  final " + std::to_string(diff.mDifferentialRatio) +
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

    void setInput(const VehicleInput& in) override { m_in = in; }

    void preStep(float) override {
        if (!m_ctrl) return;
        // Wake the body so the constraint solves (a parked car sleeps).
        if (m_system && (std::fabs(m_in.throttle) > 0.01f || m_in.brake > 0.01f ||
                         std::fabs(m_in.steer) > 0.01f || m_in.handBrake > 0.01f)) {
            m_system->GetBodyInterface().ActivateBody(m_chassis->GetID());
        }
        m_ctrl->SetDriverInput(m_in.throttle, m_in.steer, m_in.brake, m_in.handBrake);
    }

    void postStep(float) override { /* render state read on demand */ }
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
    int gear() const override {
        return m_ctrl ? m_ctrl->GetTransmission().GetCurrentGear() : 0;
    }

    float longitudinalSlip(uint32_t i) const override {
        if (!m_constraint || i >= m_wheelCount) return 0.0f;
        const JPH::Wheel* w = m_constraint->GetWheel(i);
        if (!w || !w->HasContact()) return 0.0f;
        const float surface = w->GetAngularVelocity() * m_wheelRadius[i];
        const float v = forwardSpeed();
        return (surface - v) / std::max(std::fabs(v), 1.0f);
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
        // Per-wheel: tires / suspension / ride height / brakes. All scale or offset
        // from the AUTHORED baseline captured in build(), so re-application after a
        // parts change is idempotent.
        for (size_t i = 0; i < m_wheelSettings.size(); ++i) {
            JPH::WheelSettingsWV* ws = m_wheelSettings[i];
            if (t.gripScale > 0.0f) {
                ws->mLongitudinalFriction = m_baseLongFriction;
                for (auto& p : ws->mLongitudinalFriction.mPoints) p.mY *= t.gripScale;
                ws->mLateralFriction = m_baseLatFriction;
                for (auto& p : ws->mLateralFriction.mPoints) p.mY *= t.gripScale;
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
    JPH::LinearCurve m_baseLongFriction, m_baseLatFriction; // default tire curves
    float m_baseMaxTorque = 600.0f;                     // tuned baseline (boost multiplies)
    float m_boost = 1.0f;                               // nitrous multiplier (1 = none)
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
