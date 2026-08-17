#pragma once
// ===========================================================================
// THE GARAGE SCREEN — choose a car, in the bay, not in a menu.
//
// Tim: "with a garage screen to choose, and park them all inside the tunnel
// off in a garage with lifts and toolboxes" ... "we need that to look better
// than NFS EVER DID".
//
// WHAT MAKES IT BETTER THAN A CAR-SELECT MENU. Not more chrome — three things
// that are true here and are not true of a 2D chooser:
//
//   1. IT HAPPENS IN THE ROOM. The camera does not cut to a black void with a
//      floating car. It moves to the turntable in LATE NIGHT SPEED, under the
//      shop's own lights, with the Rotary two-posts and the Hunter rack and the
//      rest of Tim's actual kit still standing around it. The car you are
//      choosing is standing where you would really stand to look at it.
//      docs/design/VEHICLE_UPGRADES.md 3.6 asked for exactly this: "preview on
//      a turntable under controlled lighting ... light it like a car
//      configurator".
//
//   2. THE NUMBERS ARE THE REAL ONES. Every figure on the card is the value
//      the car will actually be built with (app/carspec.h) — torque, redline,
//      mass, centre of mass, drivetrain — in Tim's units. NFS shows you three
//      meaningless bars; this shows the spec the physics reads, and the bars
//      are scaled ACROSS YOUR OWN FLEET, so they tell you where this car sits
//      among the cars you have rather than against an invented maximum.
//
//   3. IT TELLS YOU WHAT THE CAR IS LIKE. Each spec carries an authored note —
//      the one sentence explaining why it feels the way it does. "Least torque,
//      least grip, tallest of the small cars" says more than any bar can.
//
// SELECTING REBUILDS THE CAR. Not a re-skin: the chassis box, centre of mass,
// track, wheelbase, engine, gearing and tyres all come from the chosen spec, so
// the car you drive out is a different machine. That also closes the loop the
// tuning panel leaves open — it says "CoM / TRACK apply on the next car build",
// and this IS the next car build.
//
// Logic here is headless-testable and deterministic (--test-garage); the 3D
// turntable draw belongs to the host, which owns the bay's geometry.
// ===========================================================================

#include "carspec.h"

#include <cstdint>
#include <vector>

namespace x3::rhi { class IRenderDevice; struct FrameContext; }

namespace x3::game {

class GarageScreen {
public:
    // Selectable list = the catalog entries that HAVE art. cars.json also
    // carries handling targets with no GLB (the Plaid/NSX/Cobra rows); those
    // are specs to aim at, not cars you can walk up to, so they are excluded
    // rather than offered and then failing to load.
    void build(const CarCatalog& cat);

    bool open() const { return m_open; }
    void setOpen(bool o);
    void toggle() { setOpen(!m_open); }

    // Cursor wraps in both directions — a chooser that dead-ends at the last
    // car makes you reverse down the list to reach the first one.
    void moveCursor(int delta);
    int  cursor() const { return m_cursor; }
    size_t count() const { return m_cars.size(); }
    const CarSpec* highlighted() const;
    const CarSpec* at(size_t i) const;

    // Point the cursor at whatever is being driven, so opening the screen
    // starts where you are instead of at the top of the list.
    void selectByGlb(const std::string& glbRelPath);

    // dt-driven, deterministic: no clock reads, no rand.
    void tick(float dt);
    float spinRad() const { return m_spin; }     // turntable angle
    float reveal()  const { return m_reveal; }   // 0..1 open/close ease

    // The 2D card (name, spec, fleet-relative bars, the note). The turntable
    // car itself is drawn by the host in the bay.
    void drawCard(x3::rhi::IRenderDevice& device,
                  const x3::rhi::FrameContext& frame) const;

    // Fleet min/max of a field, for the comparison bars. Computed over the
    // SELECTABLE cars so a bar is "where this sits among yours".
    void fleetRange(float (*get)(const CarSpec&), float& lo, float& hi) const;

private:
    std::vector<const CarSpec*> m_cars;
    int   m_cursor = 0;
    bool  m_open   = false;
    float m_spin   = 0.0f;
    float m_reveal = 0.0f;
};

// --test-garage
bool runGarageSelfTest();

} // namespace x3::game
