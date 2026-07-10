// EFLZ FLOORS 2-7 WEST-WING ROOM ART PASS — see wing_dressing.h. Synthesizes a
// CanonFloor from the shared kWingRooms table (level1WingRooms) so the calibrated
// RoomDressing recipe engine dresses the Spire's per-floor identity interiors.
#include "wing_dressing.h"

#include "level1.h"           // level1Rooms() / level1WingRooms() / L1WingRoom / L1RoomDef
#include "asset_root.h"       // assetRoot() / convertedGlbRoot() (self-test)
#include "headless_device.h"  // HeadlessRenderDevice (self-test)

#include "engine/core/x3_log.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace x3::game {

bool WingDressing::build(x3::rhi::IRenderDevice& device,
                         std::string_view surfaceLibDir, std::string_view convertedGlbDir) {
    const L1RoomDef*  floors = level1Rooms();
    uint32_t wc = 0;
    const L1WingRoom* wr = level1WingRooms(wc);

    m_floor.floorNum = 0;
    m_floor.name = "Spire West Wings (F2-F7)";
    m_floor.rooms.clear();   m_floor.rooms.reserve(wc);
    m_floor.doorways.clear();
    m_bounds.clear();        m_bounds.reserve(wc);

    for (uint32_t i = 0; i < wc; ++i) {
        const L1WingRoom& w = wr[i];
        const L1RoomDef&  f = floors[(uint32_t)w.floor];
        CanonRoom r;
        r.name = w.name;
        r.type = "";
        r.cx = w.cx;  r.cz = w.cz;
        r.w  = w.hw * 2.0f;  r.d = w.hd * 2.0f;  r.h = f.ceil;
        r.cy = f.y0 + f.ceil * 0.5f;   // center Y so y0()=floor, y1()=ceiling
        m_floor.rooms.push_back(r);
        m_bounds.push_back(RoomBounds{ r.x0(), r.x1(), r.z0(), r.z1(), r.y0(), r.y1() });

        // One doorway on the room's door side. The recipe engine reads floor.doorways to
        // (1) cut wall panels around the opening (Law 1: no panel covers a door) and (2)
        // hang the accent light at the threshold. a==b==i is fine (collectCuts only tests
        // membership); axis 0 = wall plane X=const (W/E), axis 1 = Z=const (S/N).
        if (w.door == 'W' || w.door == 'E' || w.door == 'S' || w.door == 'N') {
            CanonDoorway d;
            d.a = i;  d.b = i;
            d.cy = f.y0 + 1.0f;
            d.cutHalf = 0.6f;              // == level1.cpp kDoorHalf (1.2 m opening)
            d.junction = false;
            switch (w.door) {
                case 'W': d.cx = r.x0(); d.cz = r.cz; d.axis = 0; d.kind = DoorwayKind::AdjacentX; break;
                case 'E': d.cx = r.x1(); d.cz = r.cz; d.axis = 0; d.kind = DoorwayKind::AdjacentX; break;
                case 'S': d.cx = r.cx;  d.cz = r.z0(); d.axis = 1; d.kind = DoorwayKind::AdjacentZ; break;
                case 'N': d.cx = r.cx;  d.cz = r.z1(); d.axis = 1; d.kind = DoorwayKind::AdjacentZ; break;
            }
            m_floor.doorways.push_back(d);
        }
    }

    // No CanonBeats (all kNoRoom) — the wings have no Jake's-cell / boss beats, so
    // classify() routes purely on room name + elevation band.
    CanonBeats beats;
    // BLACK-PROP FIX: the tower props are dark-metal kit furniture that reads black in
    // the windowless wing floors — matte-tint them (see RoomDressing::setPropMaterialLift).
    m_dress.setPropMaterialLift(true);
    m_built = m_dress.build(device, surfaceLibDir, convertedGlbDir, m_floor, beats);
    x3::logInfo("[wing-dress] " + std::to_string(m_dress.roomsDressed()) + "/" +
                std::to_string(wc) + " F2-F7 wing rooms dressed");
    return m_built;
}

uint32_t WingDressing::eyeRoom(const x3::phys::Vec3& eye) const {
    for (uint32_t i = 0; i < m_bounds.size(); ++i) {
        const RoomBounds& b = m_bounds[i];
        if (eye.x >= b.x0 && eye.x <= b.x1 && eye.z >= b.z0 && eye.z <= b.z1 &&
            eye.y >= b.y0 - 1.5f && eye.y <= b.y1 + 1.5f)
            return i;
    }
    return kNoRoom;
}

void WingDressing::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                        const x3::phys::Vec3& eye) const {
    if (!m_built) return;
    // Floor-band gate: draw only the rooms on the plate the eye is standing on (the
    // floors are >= ~10 m apart, so the band never spans two plates). Cheap — <= 5
    // rooms — and it keeps the dressing off the other 6 dark plates.
    static thread_local std::vector<uint32_t> vis;
    vis.clear();
    for (uint32_t i = 0; i < m_bounds.size(); ++i) {
        const RoomBounds& b = m_bounds[i];
        if (eye.y >= b.y0 - 2.5f && eye.y <= b.y1 + 2.5f) vis.push_back(i);
    }
    if (vis.empty()) return;
    m_dress.draw(device, frame, vis);
}

uint32_t WingDressing::collectFloorLights(const x3::phys::Vec3& eye,
                                          std::vector<x3::rhi::PointLight>& out) const {
    if (!m_built) return 0;
    uint32_t n = 0;
    for (const CanonLight& cl : m_dress.lights()) {
        if (cl.room >= m_bounds.size()) continue;
        const RoomBounds& b = m_bounds[cl.room];
        if (eye.y >= b.y0 - 2.5f && eye.y <= b.y1 + 2.5f) { out.push_back(cl.light); ++n; }
    }
    return n;
}

void WingDressing::applyEyeFog(x3::rhi::IRenderDevice& device, const x3::phys::Vec3& eye) {
    if (!m_built) return;
    const uint32_t rm = eyeRoom(eye);
    if (rm != kNoRoom)   // only re-tint while standing INSIDE a wing room
        m_dress.applyZoneAtmosphere(device, rm);
}

// ---- Headless self-test (--test-wingdressing) -----------------------------------------
bool runWingDressingSelfTest() {
    int pass = 0, total = 0;
    auto check = [&](bool c, const char* name) {
        ++total;
        std::printf(c ? "  PASS %s\n" : "  FAIL %s\n", name);
        if (c) ++pass;
    };

    uint32_t wc = 0;
    const L1WingRoom* wr = level1WingRooms(wc);
    check(wr != nullptr && wc >= 20, "W1 shared wing table populated (>=20 rooms)");

    HeadlessRenderDevice device;
    device.init({});

    WingDressing wd;
    const bool ok = wd.build(device, assetRoot() + "/surface_library", convertedGlbRoot());
    check(ok && wd.built(), "W2 synthetic wing floor builds");

    // Every wing room must classify to a real recipe zone (no ZNone holes) AND get a wall
    // recipe -> roomsDressed == the wing count. A future rename to a name that classifies
    // ZNone (or a Hall/Cave keyword collision) trips this.
    check(wd.roomsDressed() == wc,
          "W3 every wing room dressed (no ZNone / fog-only holes)");

    std::printf("--test-wingdressing: %d/%d checks passed\n", pass, total);
    return pass == total;
}

} // namespace x3::game
