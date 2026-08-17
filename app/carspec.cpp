// CarSpec — the per-car character table. See carspec.h for why this exists.

#include "carspec.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "asset_root.h"     // assetRoot() -> assets/vehicles/cars.json
#include "json_mini.h"      // the shared tiny JSON DOM (9 loaders already use it)

#include "engine/core/x3_log.h"

namespace x3::game {

namespace {

// Curve authoring helper: the SHAPE is the character, so it is written out
// point by point per car rather than generated from a peak number.
void setCurve(CarSpec& c, std::initializer_list<std::pair<float, float>> pts) {
    c.curvePoints = 0;
    for (const auto& p : pts) {
        if (c.curvePoints >= 8) break;
        c.curve[c.curvePoints].rpmFrac    = p.first;
        c.curve[c.curvePoints].torqueFrac = p.second;
        ++c.curvePoints;
    }
}

void setGears(CarSpec& c, std::initializer_list<float> ratios, float finalDrive) {
    c.gearCount = 0;
    for (float r : ratios) {
        if (c.gearCount >= 8) break;
        c.gearRatios[c.gearCount++] = r;
    }
    c.finalDrive = finalDrive;
}

// =========================================================================
// THE ROSTER. Eleven cars, eleven sets of numbers.
//
// Each is grounded in a real machine — its own torque peak, redline, mass,
// flywheel, gearing, grip, centre-of-mass height and idle. Read down any
// single column and the cars should disagree with each other; if a column is
// constant, that parameter is not doing any work and should be questioned.
//
// comHeight is the one Tim called out as mattering most (cars.json), so it
// spans a deliberate range: 0.28 (F1, on the floor) to 1.05 (the lorry, sat
// up high) — a 3.75x spread, which is far wider than the torque spread.
// =========================================================================
std::vector<CarSpec> makeBuiltin() {
    std::vector<CarSpec> v;
    v.reserve(11);

    // ---- 1. E46 SPORT — S54 3.2 straight six, NA, the one being driven ----
    // The reference "why does my BMW sound like a Porsche" car. An S54 makes
    // its power at the TOP: modest torque, an 8000 rpm redline, and a light
    // valvetrain that gets there fast. No turbo step anywhere in the curve.
    {
        CarSpec c;
        c.id = "e46"; c.name = "E46 SPORT"; c.glb = "Vehicles/E46_New.glb";
        c.drive = Drivetrain::RWD;
        c.massKg = 1570.0f; c.comHeight = 0.51f; c.trackM = 1.52f;
        c.halfExtents[0] = 0.89f; c.halfExtents[1] = 0.71f; c.halfExtents[2] = 2.29f;
        c.torqueNm = 365.0f; c.maxRpm = 8000.0f; c.engineInertia = 0.20f;
        setCurve(c, { {0.00f,0.62f}, {0.25f,0.82f}, {0.45f,0.95f},
                      {0.61f,1.00f}, {0.80f,0.97f}, {1.00f,0.88f} });
        setGears(c, { 4.23f, 2.51f, 1.67f, 1.23f, 1.00f, 0.83f }, 3.62f);
        c.gripScale = 1.70f; c.brakeTorque = 2400.0f;
        c.suspFreq = 2.35f; c.suspDamp = 0.72f;
        c.idleRpm = 780.0f; c.pitchAtRedline = 7.2f; c.idlePitch = 0.62f;
        c.note = "Naturally aspirated straight six. Nothing happens below 3000 "
                 "and everything happens above 6000 — the opposite shape to the "
                 "turbo car it has been impersonating.";
        v.push_back(c);
    }

    // ---- 2. CTR — the hero car, AS SHIPPED AND TUNED ----
    // Chassis figures are Tim's from cars.json (1300 kg, comHeight 0.46 lowered
    // from 0.76 to stop it rolling, track 1.354, grip 1.7).
    //
    // The ENGINE figures are deliberately NOT cars.json's 700 Nm / 6500 rpm.
    // That file was written 2026-08-14; on 2026-08-15 Tim tuned this engine by
    // ear across a long session — "I need it to rev faster, and pull HARD",
    // "titanium valve retainers" — and app/vehicle.cpp has shipped 800 Nm /
    // 7500 rpm ever since. Those are the numbers he actually drives.
    //
    // This matters because the JSON layer OVERRIDES this table: had the stale
    // row been left in place, the first successful cars.json load would have
    // quietly rolled a day of ear-tuning back to 6500 rpm, and nothing would
    // have reported it. cars.json's `ctr` row has been reconciled to match
    // (see the revision note in that file), so both sources now agree and
    // --test-carspec C11 keeps them that way.
    {
        CarSpec c;
        c.id = "ctr"; c.name = "CTR"; c.glb = "Vehicles/CTR.glb";
        c.drive = Drivetrain::RWD;
        // 1083.2 kg, not cars.json's round 1300: that is the mass the chassis
        // body is ACTUALLY created with in buildPhysics, and it is the third
        // place this hero car's paper spec and its driven spec had drifted
        // apart (after torque and redline). Taking the driven figure keeps the
        // C12 no-op gate honest — the alternative was a car that silently
        // gained 217 kg, 20% of itself, the first time this table was wired up.
        // comHeight 0.46 -> 0.31 on the 2026-08-16 merge with the vehicle-feel
        // lane. Not a preference: 0.31 m is their MEASURED anti-tip figure
        // (box centre 0.76 minus the -0.45 CoM offset), paired with the lateral
        // grip cap, against Tim's receipt "the car wants to tip up on two
        // wheels even trying to make ANY curve at speed". The C12 gate failed
        // on exactly this when the branches met, which is what it is for.
        c.massKg = 1083.2f; c.comHeight = 0.31f; c.trackM = 1.354f;
        // 0.9068 is the GLB's TRUE body half-width; buildPhysics multiplies it
        // by kBodyWiden (1.18) to reach the shipped collision half-width of
        // 1.07. cars.json's 0.84 was the fourth figure on this car that had
        // drifted from what is actually built.
        c.halfExtents[0] = 0.9068f; c.halfExtents[1] = 0.50f; c.halfExtents[2] = 1.95f;
        c.torqueNm = 800.0f; c.maxRpm = 7500.0f; c.engineInertia = 0.35f;
        // The 993 turbo step this whole lane was tuned around.
        setCurve(c, { {0.00f,0.42f}, {0.22f,0.55f}, {0.32f,0.82f}, {0.45f,1.00f},
                      {0.70f,1.00f}, {0.85f,0.94f}, {1.00f,0.78f} });
        // GEARING TRACKS THE VEHICLE-FEEL LANE, and has moved twice in two days:
        // finalDrive 4.2 -> 4.6 ("MORE acceleration", trading a paper top end
        // the aero already capped) -> 5.2, PAIRED with a deep 0.50 sixth. The
        // pair is the point: 5.2 gives gears 1-5 ~13% more wheel torque while
        // 0.50 x 5.2 still geartops 6th at ~223 mph, which is what keeps the
        // 220-with-NOS spec reachable IN GEAR. Copy both or neither — and C12
        // is what catches you copying neither.
        setGears(c, { 3.154f, 2.150f, 1.560f, 1.242f, 1.024f, 0.50f }, 5.2f);
        c.gripScale = 1.70f; c.brakeTorque = 2200.0f;
        c.suspFreq = 2.20f; c.suspDamp = 0.70f;
        c.turbo = true; c.turboMaxPsi = 16.0f; c.turboSpoolS = 0.45f;
        // pitchAtRedline 7.0 on a 7500 redline == the shipped rpm/1071 divisor
        // exactly, so the hero car's voice is preserved to the sample.
        c.idleRpm = 800.0f; c.pitchAtRedline = 7.0f; c.idlePitch = 0.75f;
        c.note = "The 993-shaped baseline. comHeight lowered from 0.76 on "
                 "2026-08-14 to stop it rolling; engine re-tuned by ear on "
                 "2026-08-15 to 800 Nm / 7500 rpm.";
        v.push_back(c);
    }

    // ---- 3. M3 E36 — S50 3.0, the older, softer, narrower six ----
    {
        CarSpec c;
        c.id = "m3_e36"; c.name = "M3 E36"; c.glb = "Vehicles/M3_E36.glb";
        c.drive = Drivetrain::RWD;
        c.massKg = 1460.0f; c.comHeight = 0.52f; c.trackM = 1.42f;
        c.halfExtents[0] = 0.85f; c.halfExtents[1] = 0.69f; c.halfExtents[2] = 2.24f;
        c.torqueNm = 320.0f; c.maxRpm = 7280.0f; c.engineInertia = 0.22f;
        setCurve(c, { {0.00f,0.66f}, {0.30f,0.88f}, {0.50f,1.00f},
                      {0.75f,0.96f}, {1.00f,0.84f} });
        setGears(c, { 4.20f, 2.49f, 1.66f, 1.24f, 1.00f }, 3.15f);
        c.gripScale = 1.55f; c.brakeTorque = 2150.0f;
        c.suspFreq = 2.15f; c.suspDamp = 0.68f;
        c.idleRpm = 760.0f; c.pitchAtRedline = 6.8f; c.idlePitch = 0.60f;
        c.note = "Narrower track and less grip than the E46 on a similar shape: "
                 "the one that moves around underneath you.";
        v.push_back(c);
    }

    // ---- 4. E30 — 325i M20 2.5. Light, tall, skinny-tyred, tail-happy ----
    {
        CarSpec c;
        c.id = "e30"; c.name = "E30"; c.glb = "Vehicles/E30.glb";
        c.drive = Drivetrain::RWD;
        c.massKg = 1200.0f; c.comHeight = 0.54f; c.trackM = 1.40f;
        c.halfExtents[0] = 0.83f; c.halfExtents[1] = 0.70f; c.halfExtents[2] = 2.21f;
        c.torqueNm = 222.0f; c.maxRpm = 6500.0f; c.engineInertia = 0.26f;
        setCurve(c, { {0.00f,0.70f}, {0.35f,0.94f}, {0.58f,1.00f},
                      {0.82f,0.92f}, {1.00f,0.78f} });
        setGears(c, { 3.83f, 2.20f, 1.40f, 1.00f, 0.81f }, 3.64f);
        // The lowest grip of the road cars. This is the point of it.
        c.gripScale = 1.35f; c.brakeTorque = 1750.0f;
        c.suspFreq = 1.95f; c.suspDamp = 0.62f;
        c.idleRpm = 800.0f; c.pitchAtRedline = 6.2f; c.idlePitch = 0.66f;
        c.note = "Least torque, least grip, tallest of the small cars. Slow in a "
                 "straight line and the most fun in a corner — do not sanitise it.";
        v.push_back(c);
    }

    // ---- 5. COUPE — a modern 2.0 turbo four. Flat, effortless, characterless ----
    {
        CarSpec c;
        c.id = "coupe"; c.name = "COUPE"; c.glb = "Vehicles/Coupe.glb";
        c.drive = Drivetrain::RWD;
        c.massKg = 1450.0f; c.comHeight = 0.50f; c.trackM = 1.56f;
        c.halfExtents[0] = 0.90f; c.halfExtents[1] = 0.68f; c.halfExtents[2] = 2.26f;
        c.torqueNm = 350.0f; c.maxRpm = 6500.0f; c.engineInertia = 0.30f;
        // Modern small-turbo shape: everything arrives at 1800 and simply stays.
        setCurve(c, { {0.00f,0.55f}, {0.18f,0.94f}, {0.28f,1.00f},
                      {0.72f,1.00f}, {0.88f,0.90f}, {1.00f,0.74f} });
        setGears(c, { 4.71f, 3.14f, 2.11f, 1.67f, 1.29f, 1.00f, 0.84f, 0.67f }, 3.08f);
        c.gripScale = 1.80f; c.brakeTorque = 2500.0f;
        c.suspFreq = 2.30f; c.suspDamp = 0.75f;
        c.turbo = true; c.turboMaxPsi = 14.0f; c.turboSpoolS = 0.30f;
        c.idleRpm = 700.0f; c.pitchAtRedline = 5.8f; c.idlePitch = 0.58f;
        c.note = "Eight speeds and a table-flat torque plateau: quick everywhere "
                 "and dramatic nowhere. The deliberate contrast to the E30.";
        v.push_back(c);
    }

    // ---- 6. MUSCLE — big-block V8. Fat, lazy, heavy crank, low redline ----
    {
        CarSpec c;
        c.id = "muscle"; c.name = "MUSCLE"; c.glb = "Vehicles/Muscle.glb";
        c.drive = Drivetrain::RWD;
        c.massKg = 1750.0f; c.comHeight = 0.56f; c.trackM = 1.54f;
        c.halfExtents[0] = 0.97f; c.halfExtents[1] = 0.68f; c.halfExtents[2] = 2.52f;
        c.torqueNm = 610.0f; c.maxRpm = 5800.0f;
        // A heavy iron crank. It does not rev, it HEAVES — and you feel that
        // long before you read the torque figure.
        c.engineInertia = 0.55f;
        setCurve(c, { {0.00f,0.82f}, {0.20f,0.96f}, {0.40f,1.00f},
                      {0.62f,0.97f}, {0.82f,0.86f}, {1.00f,0.70f} });
        setGears(c, { 2.88f, 1.75f, 1.30f, 1.00f }, 3.55f);
        c.gripScale = 1.30f; c.brakeTorque = 2000.0f;
        c.suspFreq = 1.80f; c.suspDamp = 0.58f;
        c.idleRpm = 650.0f; c.pitchAtRedline = 4.6f; c.idlePitch = 0.42f;
        c.note = "Four gears, torque from idle, and a deep lopey idle a long way "
                 "under everything else. Grip 1.30 so it still lights them up.";
        v.push_back(c);
    }

    // ---- 7. SKYLINE — RB26 twin-turbo, AWD. The one that just goes ----
    {
        CarSpec c;
        c.id = "skyline"; c.name = "SKYLINE";
        c.glb = "Vehicles/Skyline_by_BUMSTRUM.glb";
        c.drive = Drivetrain::AWD;
        c.massKg = 1560.0f; c.comHeight = 0.48f; c.trackM = 1.48f;
        c.halfExtents[0] = 0.89f; c.halfExtents[1] = 0.68f; c.halfExtents[2] = 2.33f;
        c.torqueNm = 470.0f; c.maxRpm = 8000.0f; c.engineInertia = 0.24f;
        // Sequential twins: a real step, but higher up than the 993's.
        setCurve(c, { {0.00f,0.38f}, {0.30f,0.58f}, {0.42f,0.88f}, {0.52f,1.00f},
                      {0.78f,1.00f}, {0.90f,0.93f}, {1.00f,0.82f} });
        setGears(c, { 3.83f, 2.36f, 1.69f, 1.31f, 1.00f, 0.79f }, 3.55f);
        c.gripScale = 2.00f; c.brakeTorque = 2600.0f;
        c.suspFreq = 2.45f; c.suspDamp = 0.78f;
        c.turbo = true; c.turboMaxPsi = 18.0f; c.turboSpoolS = 0.55f;
        c.idleRpm = 850.0f; c.pitchAtRedline = 7.4f; c.idlePitch = 0.68f;
        c.note = "AWD, so it hooks up where the muscle car smokes. Longest spool "
                 "in the roster — the wait IS the character.";
        v.push_back(c);
    }

    // ---- 8. PICKUP — 5.3 V8 half-ton. Tall, soft, and it leans ----
    {
        CarSpec c;
        c.id = "pickup"; c.name = "PICKUP"; c.glb = "Vehicles/Pickup.glb";
        c.drive = Drivetrain::RWD;
        c.massKg = 2300.0f;
        // High. Tim's thesis parameter doing its job: halfTrack/comHeight puts
        // the rollover threshold here far below any of the cars.
        c.comHeight = 0.78f; c.trackM = 1.72f;
        c.halfExtents[0] = 1.02f; c.halfExtents[1] = 0.92f; c.halfExtents[2] = 2.85f;
        c.torqueNm = 515.0f; c.maxRpm = 5600.0f; c.engineInertia = 0.60f;
        setCurve(c, { {0.00f,0.78f}, {0.25f,0.95f}, {0.48f,1.00f},
                      {0.70f,0.95f}, {1.00f,0.76f} });
        setGears(c, { 3.06f, 1.63f, 1.00f, 0.70f }, 3.42f);
        c.gripScale = 1.15f; c.brakeTorque = 2600.0f;
        c.suspFreq = 1.55f; c.suspDamp = 0.52f;
        c.idleRpm = 620.0f; c.pitchAtRedline = 4.2f; c.idlePitch = 0.40f;
        c.note = "Soft springs, high mass, high centre of gravity. Turn in hard "
                 "and it rolls onto the outside wheel — as it should.";
        v.push_back(c);
    }

    // ---- 9. JEEP — 3.6 V6 off-roader. The tallest thing on four small wheels --
    {
        CarSpec c;
        c.id = "jeep"; c.name = "JEEP"; c.glb = "Vehicles/Jeep.glb";
        c.drive = Drivetrain::AWD;
        c.massKg = 2000.0f;
        c.comHeight = 0.82f; c.trackM = 1.60f;   // the worst ratio in the roster
        c.halfExtents[0] = 0.94f; c.halfExtents[1] = 0.95f; c.halfExtents[2] = 2.18f;
        c.torqueNm = 353.0f; c.maxRpm = 6000.0f; c.engineInertia = 0.50f;
        setCurve(c, { {0.00f,0.70f}, {0.30f,0.90f}, {0.55f,1.00f},
                      {0.80f,0.94f}, {1.00f,0.80f} });
        setGears(c, { 4.71f, 3.14f, 2.11f, 1.67f, 1.29f, 1.00f }, 3.73f);
        c.gripScale = 1.10f; c.brakeTorque = 2300.0f;
        c.suspFreq = 1.45f; c.suspDamp = 0.50f;   // long soft travel
        c.idleRpm = 700.0f; c.pitchAtRedline = 4.8f; c.idlePitch = 0.48f;
        c.note = "Highest comHeight relative to track of anything here. Softest "
                 "springs. It should feel like it is walking on stilts.";
        v.push_back(c);
    }

    // ---- 10. TRUCK — medium diesel lorry. The anti-sports-car ----
    // The row that proves the table is not one car scaled: 7500 kg, a 3000 rpm
    // redline, 1400 Nm from just off idle, a flywheel with 6x the inertia of
    // the E46's, eight gears, and an idle note nearly two octaves down.
    {
        CarSpec c;
        c.id = "truck"; c.name = "TRUCK"; c.glb = "Vehicles/Truck.glb";
        c.drive = Drivetrain::RWD;
        c.massKg = 7500.0f; c.comHeight = 1.05f; c.trackM = 2.00f;
        c.halfExtents[0] = 1.22f; c.halfExtents[1] = 1.45f; c.halfExtents[2] = 4.10f;
        c.torqueNm = 1400.0f; c.maxRpm = 3000.0f; c.engineInertia = 2.20f;
        // A diesel's curve: everything low, and it falls off a cliff at the top.
        setCurve(c, { {0.00f,0.72f}, {0.30f,1.00f}, {0.55f,1.00f},
                      {0.75f,0.86f}, {1.00f,0.58f} });
        setGears(c, { 7.05f, 4.35f, 2.85f, 1.95f, 1.38f, 1.00f, 0.78f, 0.63f }, 4.30f);
        c.gripScale = 1.00f; c.brakeTorque = 5200.0f;
        c.suspFreq = 1.30f; c.suspDamp = 0.48f;
        c.turbo = true; c.turboMaxPsi = 22.0f; c.turboSpoolS = 0.80f;
        c.idleRpm = 550.0f; c.pitchAtRedline = 1.60f; c.idlePitch = 0.29f;
        c.note = "Redlines where the F1 idles. Eight gears because 1400 Nm at "
                 "3000 rpm has to be geared, not revved.";
        v.push_back(c);
    }

    // ---- 11. F1 — the other extreme. 740 kg, on the floor, screaming ----
    {
        CarSpec c;
        c.id = "f1"; c.name = "F1"; c.glb = "Vehicles/F1.glb";
        c.drive = Drivetrain::RWD;
        c.massKg = 740.0f;
        c.comHeight = 0.28f; c.trackM = 1.80f;   // lowest CoG, widest track
        c.halfExtents[0] = 0.90f; c.halfExtents[1] = 0.35f; c.halfExtents[2] = 2.60f;
        c.torqueNm = 290.0f; c.maxRpm = 15000.0f;
        c.engineInertia = 0.06f;                 // almost nothing to spin up
        setCurve(c, { {0.00f,0.42f}, {0.30f,0.68f}, {0.55f,0.88f},
                      {0.78f,0.98f}, {0.92f,1.00f}, {1.00f,0.99f} });
        setGears(c, { 2.90f, 2.20f, 1.78f, 1.48f, 1.26f, 1.09f, 0.96f }, 4.10f);
        c.gripScale = 3.20f; c.brakeTorque = 4800.0f;
        c.suspFreq = 3.60f; c.suspDamp = 0.90f;  // effectively no travel
        c.idleRpm = 3500.0f; c.pitchAtRedline = 7.5f; c.idlePitch = 1.75f;
        c.note = "Least torque of anything with a V8, and by far the fastest: "
                 "290 Nm at 15000 rpm on 740 kg. Idles above the E46's power peak.";
        v.push_back(c);
    }

    return v;
}

Drivetrain parseDrive(const std::string& s, Drivetrain def) {
    // cars.json writes prose ("AWD, tri-motor"), so match on a substring rather
    // than requiring an exact token.
    if (s.find("AWD") != std::string::npos) return Drivetrain::AWD;
    if (s.find("FWD") != std::string::npos) return Drivetrain::FWD;
    if (s.find("RWD") != std::string::npos) return Drivetrain::RWD;
    return def;
}

}  // namespace

// ---------------------------------------------------------------------------

void CarSpec::applyTo(x3::phys::WheeledVehicleDesc& vd) const {
    vd.maxEngineTorque = torqueNm;
    vd.maxEngineRPM    = maxRpm;
    vd.engineInertia   = engineInertia;
    vd.clutchStrength  = clutchStrength;
    vd.gearCount = singleSpeed ? 1u : gearCount;
    for (uint32_t i = 0; i < 8; ++i)
        vd.gearRatios[i] = (i < vd.gearCount) ? gearRatios[i] : 0.0f;
    if (singleSpeed && gearCount > 0) vd.gearRatios[0] = gearRatios[0];
    vd.finalDrive = finalDrive;
    // An EV makes full torque from zero rpm: a flat curve is CORRECT here, not
    // lazy — the shape that would be slop on a combustion engine is the truth
    // on this one.
    if (evPowerband) {
        vd.curveRpm[0] = 0.0f; vd.curveTq[0] = 1.0f;
        vd.curveRpm[1] = 0.75f; vd.curveTq[1] = 1.0f;
        vd.curveRpm[2] = 1.0f;  vd.curveTq[2] = 0.85f;
        vd.curveCount = 3;
    } else {
        vd.curveCount = std::min<uint32_t>(curvePoints, 8);
        for (uint32_t i = 0; i < vd.curveCount; ++i) {
            vd.curveRpm[i] = curve[i].rpmFrac;
            vd.curveTq [i] = curve[i].torqueFrac;
        }
    }
}

x3::phys::WheeledTuning CarSpec::asTuning() const {
    x3::phys::WheeledTuning t;
    t.maxEngineTorque = torqueNm;
    t.maxEngineRPM    = maxRpm;
    t.massKg          = massKg;
    t.gripScale       = gripScale;
    t.brakeTorque     = brakeTorque;
    t.suspensionFreq  = suspFreq;
    t.suspensionDamp  = suspDamp;
    t.curvePoints     = std::min<uint32_t>(curvePoints, 8);
    for (uint32_t i = 0; i < t.curvePoints; ++i) t.curve[i] = curve[i];
    // rideHeightDelta deliberately left at its "leave" sentinel: ride height is
    // the SHOP's business (vehparts writes it), not the stock spec's.
    return t;
}

// ---------------------------------------------------------------------------

const CarCatalog& CarCatalog::builtin() {
    static const CarCatalog c = [] {
        CarCatalog k;
        k.m_cars = makeBuiltin();
        return k;
    }();
    return c;
}

bool CarCatalog::loadJson(const std::string& json) {
    if (json.empty()) return false;
    jmini::JReader r(json);
    jmini::JVal root = r.parse();
    if (!r.ok || root.t != jmini::JVal::Obj) {
        x3::logError("[carspec] cars.json parse failed — keeping the built-in roster");
        return false;
    }
    const jmini::JVal* cars = root.get("cars");
    if (!cars || cars->t != jmini::JVal::Arr) {
        x3::logError("[carspec] cars.json has no `cars` array — keeping the built-in roster");
        return false;
    }

    uint32_t overrode = 0, added = 0;
    for (const jmini::JVal& j : cars->arr) {
        if (j.t != jmini::JVal::Obj) continue;
        const std::string id = j.sval("id");
        if (id.empty()) continue;

        // Override in place when the id is known, otherwise append. An
        // authored car with no `glb` is a HANDLING TARGET, not a playable
        // entry — cars.json carries three of those (plaid/nsx/cobra), and
        // they are kept so the targets stay legible and can be bound to art
        // later without re-deriving the numbers.
        auto it = std::find_if(m_cars.begin(), m_cars.end(),
                               [&](const CarSpec& c) { return c.id == id; });
        const bool isNew = (it == m_cars.end());
        if (isNew) { m_cars.push_back(CarSpec{}); it = m_cars.end() - 1; it->id = id; }

        CarSpec& c = *it;
        c.name    = j.sval("name", c.name);
        c.glb     = j.sval("glb",  c.glb);
        c.note    = j.sval("note", c.note);
        c.drive   = parseDrive(j.sval("layout"), c.drive);
        c.massKg  = j.fnum("massKg",          c.massKg);
        c.torqueNm= j.fnum("maxEngineTorque", c.torqueNm);
        c.maxRpm  = j.fnum("maxEngineRPM",    c.maxRpm);
        c.gripScale = j.fnum("gripScale",     c.gripScale);
        c.comHeight = j.fnum("comHeight",     c.comHeight);
        c.trackM    = j.fnum("trackM",        c.trackM);
        c.engineInertia = j.fnum("engineInertia", c.engineInertia);
        c.brakeTorque   = j.fnum("brakeTorque",   c.brakeTorque);
        c.suspFreq      = j.fnum("suspFreq",      c.suspFreq);
        c.suspDamp      = j.fnum("suspDamp",      c.suspDamp);
        c.idleRpm       = j.fnum("idleRpm",       c.idleRpm);
        c.pitchAtRedline= j.fnum("pitchAtRedline",c.pitchAtRedline);
        c.idlePitch     = j.fnum("idlePitch",     c.idlePitch);
        c.singleSpeed   = j.bval("singleSpeed",   c.singleSpeed);
        c.evPowerband   = j.bval("evPowerband",   c.evPowerband);
        c.turbo         = j.bval("turbo",         c.turbo);
        c.turboMaxPsi   = j.fnum("turboMaxPsi",   c.turboMaxPsi);
        c.turboSpoolS   = j.fnum("turboSpoolS",   c.turboSpoolS);
        if (const jmini::JVal* he = j.get("halfExtents");
            he && he->t == jmini::JVal::Arr && he->arr.size() == 3) {
            for (int i = 0; i < 3; ++i)
                if (he->arr[(size_t)i].t == jmini::JVal::Num)
                    c.halfExtents[i] = (float)he->arr[(size_t)i].num;
        }
        // An authored torque curve, if present: [[rpmFrac, torqueFrac], ...].
        if (const jmini::JVal* cv = j.get("curve"); cv && cv->t == jmini::JVal::Arr) {
            uint32_t n = 0;
            for (const jmini::JVal& p : cv->arr) {
                if (n >= 8 || p.t != jmini::JVal::Arr || p.arr.size() < 2) continue;
                c.curve[n].rpmFrac    = (float)p.arr[0].num;
                c.curve[n].torqueFrac = (float)p.arr[1].num;
                ++n;
            }
            if (n >= 2) c.curvePoints = n;
        }
        if (isNew) ++added; else ++overrode;
    }

    char b[192];
    std::snprintf(b, sizeof(b),
        "[carspec] cars.json applied: %u car(s) overridden, %u added, %u total",
        overrode, added, (uint32_t)m_cars.size());
    x3::logInfo(b);
    return (overrode + added) > 0;
}

bool CarCatalog::loadFile() {
    // Same resolution order vehparts uses for parts.json: the rooted asset
    // path first, then the repo-relative fallbacks that let a build directory
    // several levels down still find the source tree.
    const std::string rooted = assetRoot() + "/vehicles/cars.json";
    std::string js = jmini::readFile(rooted);
    if (js.empty()) {
        for (const char* rel : { "assets/vehicles/cars.json",
                                 "../assets/vehicles/cars.json",
                                 "../../assets/vehicles/cars.json" }) {
            js = jmini::readFile(rel);
            if (!js.empty()) break;
        }
    }
    if (js.empty()) {
        x3::logInfo("[carspec] no cars.json found — running the built-in roster");
        return false;
    }
    return loadJson(js);
}

const CarSpec* CarCatalog::find(const std::string& id) const {
    for (const CarSpec& c : m_cars) if (c.id == id) return &c;
    return nullptr;
}

const CarSpec* CarCatalog::forGlb(const std::string& glbRelPath) const {
    for (const CarSpec& c : m_cars) if (!c.glb.empty() && c.glb == glbRelPath) return &c;
    return nullptr;
}

const CarCatalog& CarCatalog::game() {
    static const CarCatalog c = [] {
        CarCatalog k = builtin();     // copy, then let the JSON win over it
        k.loadFile();
        return k;
    }();
    return c;
}

// ===========================================================================
// --test-carspec
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void ccheck(bool ok, const char* what) {
    if (ok) { ++g_pass; x3::logInfo(std::string("  [ok]   ") + what); }
    else    { ++g_fail; x3::logError(std::string("  [FAIL] ") + what); }
}
}  // namespace

bool runCarSpecSelfTest() {
    g_pass = g_fail = 0;
    x3::logInfo("--- per-car character table self-test ---");
    char b[320];

    const CarCatalog& cat = CarCatalog::builtin();

    // C0: the roster exists and is the size we think. A suite that passes on an
    // empty set is the trap this codebase already fell into once (lay-bys).
    std::snprintf(b, sizeof(b), "C0 built-in roster is non-empty: %u cars",
                  (uint32_t)cat.size());
    ccheck(cat.size() == 11, b);

    // C1: every car has an id, a name and a curve. A spec with no curve would
    // silently inherit Jolt's flat default — the exact "characterless lump"
    // this table exists to end.
    {
        bool ok = true; std::string bad;
        for (const CarSpec& c : cat.all()) {
            if (c.id.empty() || c.name.empty() || c.curvePoints < 2 ||
                c.torqueNm <= 0.0f || c.maxRpm <= 0.0f || c.massKg <= 0.0f ||
                c.gearCount == 0) { ok = false; bad = c.id.empty() ? "<no id>" : c.id; break; }
        }
        std::snprintf(b, sizeof(b),
            "C1 every car is fully specified (id/name/curve/torque/rpm/mass/gears)%s%s",
            ok ? "" : " — first offender: ", ok ? "" : bad.c_str());
        ccheck(ok, b);
    }

    // C2: curves ascend in rpmFrac and stay in [0,1]. A descending point set
    // silently breaks Jolt's interpolation.
    {
        bool ok = true; std::string bad;
        for (const CarSpec& c : cat.all()) {
            for (uint32_t i = 0; i < c.curvePoints; ++i) {
                const auto& p = c.curve[i];
                if (p.rpmFrac < 0.0f || p.rpmFrac > 1.0f ||
                    p.torqueFrac <= 0.0f || p.torqueFrac > 1.0f) { ok = false; bad = c.id; }
                if (i && c.curve[i].rpmFrac <= c.curve[i-1].rpmFrac) { ok = false; bad = c.id; }
            }
            if (!ok) break;
        }
        std::snprintf(b, sizeof(b),
            "C2 every torque curve ascends and stays normalised%s%s",
            ok ? "" : " — offender: ", ok ? "" : bad.c_str());
        ccheck(ok, b);
    }

    // C3: THE NEGATIVE CONTROL. The table must actually DISAGREE with itself.
    // If every car shares a value, that parameter is decoration. This is the
    // assertion that fails if someone "simplifies" the roster into one row
    // multiplied by a scalar — the anti-slop gate, expressed as a test.
    {
        auto spread = [&](float (*get)(const CarSpec&)) {
            float lo = 1e30f, hi = -1e30f;
            for (const CarSpec& c : cat.all()) { const float v = get(c); lo = std::min(lo, v); hi = std::max(hi, v); }
            return (lo > 1e-6f) ? (hi / lo) : 0.0f;
        };
        const float sTorque  = spread([](const CarSpec& c){ return c.torqueNm; });
        const float sRpm     = spread([](const CarSpec& c){ return c.maxRpm; });
        const float sMass    = spread([](const CarSpec& c){ return c.massKg; });
        const float sInertia = spread([](const CarSpec& c){ return c.engineInertia; });
        const float sCom     = spread([](const CarSpec& c){ return c.comHeight; });
        const float sIdle    = spread([](const CarSpec& c){ return c.idleRpm; });
        std::snprintf(b, sizeof(b),
            "C3 the cars genuinely differ — torque x%.1f, redline x%.1f, mass x%.1f, "
            "flywheel x%.1f, comHeight x%.2f, idle x%.1f",
            (double)sTorque, (double)sRpm, (double)sMass,
            (double)sInertia, (double)sCom, (double)sIdle);
        ccheck(sTorque > 3.0f && sRpm > 3.0f && sMass > 5.0f &&
               sInertia > 5.0f && sCom > 2.0f && sIdle > 3.0f, b);
    }

    // C4: Tim's thesis, enforced. comHeight must be doing real work, so the
    // rollover threshold ~ atan(halfTrack / comHeight) has to separate a
    // sports car from a 4x4 by a visible margin.
    {
        const CarSpec* f1   = cat.find("f1");
        const CarSpec* jeep = cat.find("jeep");
        const float tF1 = f1   ? std::atan((f1->trackM   * 0.5f) / f1->comHeight)   * 57.2958f : 0.0f;
        const float tJp = jeep ? std::atan((jeep->trackM * 0.5f) / jeep->comHeight) * 57.2958f : 0.0f;
        std::snprintf(b, sizeof(b),
            "C4 rollover threshold separates the roster: F1 %.1f deg vs Jeep %.1f deg",
            (double)tF1, (double)tJp);
        ccheck(f1 && jeep && (tF1 - tJp) > 15.0f, b);
    }

    // C5: the truck is not a car with small numbers. Checked explicitly
    // because "one row multiplied" is the failure mode, and the lorry is
    // where it would show first.
    {
        const CarSpec* t = cat.find("truck");
        const CarSpec* e = cat.find("e46");
        const bool ok = t && e &&
            t->torqueNm > e->torqueNm * 3.0f &&   // far more torque
            t->maxRpm   < e->maxRpm   * 0.5f &&   // at half the revs
            t->engineInertia > e->engineInertia * 5.0f &&
            t->gearCount > e->gearCount &&
            t->idleRpm  < e->idleRpm;
        std::snprintf(b, sizeof(b),
            "C5 the truck inverts the sports car: %.0f Nm vs %.0f, %.0f rpm vs %.0f, "
            "flywheel %.2f vs %.2f, %u gears vs %u",
            t ? (double)t->torqueNm : 0.0, e ? (double)e->torqueNm : 0.0,
            t ? (double)t->maxRpm : 0.0,   e ? (double)e->maxRpm : 0.0,
            t ? (double)t->engineInertia : 0.0, e ? (double)e->engineInertia : 0.0,
            t ? t->gearCount : 0u, e ? e->gearCount : 0u);
        ccheck(ok, b);
    }

    // C6: the VOICE differs. This is the literal complaint — "an E46 makes
    // flat-six noises".
    //
    // NOTE ON WHAT IS MEASURED. The first version of this assertion compared
    // pitchUnityRpm() across the roster and demanded a 2x spread; it failed at
    // 1.91x. That was the wrong quantity: unity rpm is a derived divisor, and
    // two cars sharing one can still sound nothing alike because their rpm
    // RANGES differ (the truck lives at 550-3000, the F1 at 3500-15000). What
    // a player actually hears is where the note SITS at idle and where it ENDS
    // at the redline, so those are what this checks — along with the mixer's
    // 8.0x playback clamp, which is a hard ceiling rather than a taste call.
    {
        bool inRange = true;
        float idleLo = 1e30f, idleHi = -1e30f, redLo = 1e30f, redHi = -1e30f;
        for (const CarSpec& c : cat.all()) {
            const float atRedline = c.maxRpm / c.pitchUnityRpm();
            if (atRedline > 8.0f || c.idlePitch <= 0.0f) inRange = false;
            idleLo = std::min(idleLo, c.idlePitch); idleHi = std::max(idleHi, c.idlePitch);
            redLo  = std::min(redLo, atRedline);    redHi  = std::max(redHi, atRedline);
        }
        std::snprintf(b, sizeof(b),
            "C6 engine voices differ audibly and stay under the 8.0x clamp — "
            "idle %.2fx..%.2fx (x%.1f), redline %.1fx..%.1fx (x%.1f)",
            (double)idleLo, (double)idleHi, (double)(idleHi / idleLo),
            (double)redLo,  (double)redHi,  (double)(redHi / redLo));
        ccheck(inRange && (idleHi / idleLo) > 3.0f && (redHi / redLo) > 3.0f, b);
    }

    // C7: applyTo() actually lowers the spec — the wiring, not the data. The
    // bug class here is a table that is authored, correct, and never read.
    {
        const CarSpec* t = cat.find("truck");
        x3::phys::WheeledVehicleDesc vd;      // engine defaults: 600 Nm / 6000
        t->applyTo(vd);
        const bool ok = std::fabs(vd.maxEngineTorque - t->torqueNm) < 0.01f &&
                        std::fabs(vd.maxEngineRPM - t->maxRpm) < 0.01f &&
                        std::fabs(vd.engineInertia - t->engineInertia) < 0.001f &&
                        vd.gearCount == t->gearCount &&
                        vd.curveCount == t->curvePoints &&
                        std::fabs(vd.finalDrive - t->finalDrive) < 0.001f;
        std::snprintf(b, sizeof(b),
            "C7 applyTo() lowers the spec onto the build desc (%.0f Nm / %.0f rpm / "
            "%u gears / %u curve points), not the 600/6000 default",
            (double)vd.maxEngineTorque, (double)vd.maxEngineRPM,
            vd.gearCount, vd.curveCount);
        ccheck(ok, b);
    }

    // C8: asTuning() is what a car SWAP applies to a running rig, and what the
    // shop composes on top of. rideHeightDelta must stay at its leave
    // sentinel or a car swap would silently undo a suspension purchase.
    {
        const CarSpec* c = cat.find("e30");
        const x3::phys::WheeledTuning t = c->asTuning();
        const bool ok = std::fabs(t.maxEngineTorque - c->torqueNm) < 0.01f &&
                        std::fabs(t.massKg - c->massKg) < 0.01f &&
                        std::fabs(t.gripScale - c->gripScale) < 0.001f &&
                        t.curvePoints == c->curvePoints &&
                        t.rideHeightDelta == x3::phys::WheeledTuning::kRideHeightLeave;
        ccheck(ok, "C8 asTuning() carries the stock figures and leaves ride height "
                   "to the shop (a car swap must not undo bought suspension)");
    }

    // C9: every fleet GLB in host_tunnel's kFleet resolves to a spec. A car
    // you can select but cannot look up would silently fall back to the hero
    // car's numbers — which is exactly today's bug, just moved.
    {
        static const char* kFleetGlbs[] = {
            "Vehicles/E46_New.glb", "Vehicles/CTR.glb", "Vehicles/M3_E36.glb",
            "Vehicles/E30.glb", "Vehicles/Coupe.glb", "Vehicles/Muscle.glb",
            "Vehicles/Skyline_by_BUMSTRUM.glb", "Vehicles/Pickup.glb",
            "Vehicles/Jeep.glb", "Vehicles/Truck.glb", "Vehicles/F1.glb",
        };
        bool ok = true; std::string missing;
        for (const char* g : kFleetGlbs)
            if (!cat.forGlb(g)) { ok = false; missing = g; break; }
        std::snprintf(b, sizeof(b),
            "C9 all 11 fleet GLBs resolve to a spec%s%s",
            ok ? "" : " — unmapped: ", ok ? "" : missing.c_str());
        ccheck(ok, b);
    }

    // C10: the JSON layer overrides rather than replaces, and a MALFORMED
    // document degrades to the built-in roster instead of emptying it.
    {
        CarCatalog k = CarCatalog::builtin();
        const size_t before = k.size();
        ccheck(!k.loadJson("{ this is not json"),
               "C10a a malformed cars.json is rejected");
        ccheck(k.size() == before && k.find("ctr") != nullptr,
               "C10b ... and the built-in roster survives it intact");

        CarCatalog k2 = CarCatalog::builtin();
        const float wasTorque = k2.find("e30")->torqueNm;
        k2.loadJson(R"({"cars":[{"id":"e30","maxEngineTorque":999},
                                {"id":"newcar","name":"NEW","massKg":1000}]})");
        const CarSpec* e30 = k2.find("e30");
        const bool overrode = e30 && std::fabs(e30->torqueNm - 999.0f) < 0.01f &&
                              // untouched fields must SURVIVE the override
                              std::fabs(e30->comHeight - 0.54f) < 0.001f &&
                              e30->curvePoints > 0;
        std::snprintf(b, sizeof(b),
            "C10c JSON overrides field-by-field (e30 torque %.0f -> %.0f, curve + "
            "comHeight preserved) and appends unknown ids (%u cars)",
            (double)wasTorque, e30 ? (double)e30->torqueNm : 0.0, (uint32_t)k2.size());
        ccheck(overrode && k2.size() == before + 1 && k2.find("newcar"), b);
    }

    // C11: the SHIPPED cars.json agrees with the built-in table where they
    // overlap. Two sources of truth that can drift are worse than one that is
    // wrong, because nothing ever tells you they parted company.
    {
        CarCatalog k = CarCatalog::builtin();
        const CarSpec beforeCtr = *k.find("ctr");
        if (!k.loadFile()) {
            ccheck(true, "C11 cars.json not present in this working dir — "
                         "built-in roster is authoritative (not a failure)");
        } else {
            const CarSpec* after = k.find("ctr");
            const bool agree = after &&
                std::fabs(after->torqueNm  - beforeCtr.torqueNm)  < 0.5f &&
                std::fabs(after->maxRpm    - beforeCtr.maxRpm)    < 0.5f &&
                std::fabs(after->massKg    - beforeCtr.massKg)    < 0.5f &&
                std::fabs(after->comHeight - beforeCtr.comHeight) < 0.005f &&
                std::fabs(after->gripScale - beforeCtr.gripScale) < 0.005f;
            std::snprintf(b, sizeof(b),
                "C11 shipped cars.json agrees with the built-in table on `ctr` "
                "(%.0f Nm / %.0f rpm / %.0f kg / com %.2f)",
                after ? (double)after->torqueNm : 0.0, after ? (double)after->maxRpm : 0.0,
                after ? (double)after->massKg : 0.0, after ? (double)after->comHeight : 0.0);
            ccheck(agree, b);
        }
    }

    // C12: THE NO-REGRESSION GATE. DriveDemo::buildPhysics(..., nullptr) keeps
    // the shipped hero-car constants; passing the CTR spec must therefore
    // change NOTHING. Asserted against the literal values in vehicle.cpp, so
    // if anyone edits either side the suite says so instead of the car quietly
    // driving differently — which is how two days of tuning-by-ear gets lost.
    {
        const CarSpec* c = cat.find("ctr");
        const float boxCentreH = 0.76f + (c->halfExtents[1] - 0.50f);
        const float comOffY    = c->comHeight - boxCentreH;
        const bool ok = c &&
            std::fabs(c->massKg   - 1083.2f) < 0.5f &&    // addBox mass
            std::fabs(comOffY     - (-0.45f)) < 0.005f && // addBox comOffset
            std::fabs(c->torqueNm - 800.0f) < 0.5f &&     // vd.maxEngineTorque
            std::fabs(c->maxRpm   - 7500.0f) < 0.5f &&    // vd.maxEngineRPM
            std::fabs(c->engineInertia - 0.35f) < 0.001f &&
            std::fabs(c->finalDrive - 5.2f) < 0.001f &&
            std::fabs(c->gearRatios[5] - 0.50f) < 0.001f &&   // the PAIRED deep 6th
            c->gearCount == 6 && c->curvePoints == 7 &&
            std::fabs(c->trackM - 1.354f) < 0.001f &&     // track scale -> 1.0
            // ALL THREE half-extents, not just the length. The first cut of
            // this gate checked only [2] and let a 21% narrower collision box
            // through — 1.07 shipped vs cars.json's 0.84. kBodyWiden (1.18) is
            // applied by buildPhysics, so the spec must hold 1.07/1.18.
            std::fabs(c->halfExtents[0] * 1.18f - 1.07f) < 0.005f &&
            std::fabs(c->halfExtents[1] - 0.50f) < 0.001f &&
            std::fabs(c->halfExtents[2] - 1.95f) < 0.001f &&
            std::fabs(c->gripScale - 1.7f) < 0.001f &&    // grip scale -> 1.0
            std::fabs(c->brakeTorque - 2200.0f) < 0.5f &&
            std::fabs(c->suspFreq - 2.2f) < 0.001f &&
            std::fabs(c->suspDamp - 0.7f) < 0.001f;
        std::snprintf(b, sizeof(b),
            "C12 applying the CTR spec is a NO-OP on the hero car "
            "(%.1f kg, CoM %+.2f m, %.0f Nm, %.0f rpm, flywheel %.2f, %u gears) — "
            "the tuned car cannot regress behind this feature",
            c ? (double)c->massKg : 0.0, (double)comOffY,
            c ? (double)c->torqueNm : 0.0, c ? (double)c->maxRpm : 0.0,
            c ? (double)c->engineInertia : 0.0, c ? c->gearCount : 0u);
        ccheck(ok, b);
    }

    std::snprintf(b, sizeof(b), "--- per-car table self-test: %d passed, %d failed ---",
                  g_pass, g_fail);
    if (g_fail) x3::logError(b); else x3::logInfo(b);
    return g_fail == 0;
}

}  // namespace x3::game
