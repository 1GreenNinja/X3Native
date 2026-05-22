// Button -> door interaction (S4). See app/door.h.
//
// Clean-room: built from the IPhysicsWorld + Scene interfaces only.
#include "door.h"
#include "level.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

// ---------------------------------------------------------------------------
// Door geometry constants (must match the S2 doorway gap in level.cpp).
//   Doorway: +Z wall at z = kRoomHalf (8.0), gap centered at X=0, half-width
//   kDoorHalf (0.6 -> 1.2 m wide), wall thickness kWallT (0.2 -> half 0.1),
//   passage clear up to the lintel bottom at y = 2.1.
// ---------------------------------------------------------------------------
namespace {
constexpr float kDoorEdgeZ      = 8.0f;   // +Z wall plane (== level.cpp edge)
constexpr float kDoorHalfX      = 0.6f;   // == kDoorHalf (fills the 1.2 m opening)
constexpr float kDoorHalfZ      = 0.1f;   // == kWallT*0.5 (matches wall thickness)
constexpr float kPassageTop     = 2.1f;   // == lintel bottom (clear opening height)
constexpr float kDoorHalfY      = kPassageTop * 0.5f;          // 1.05
constexpr float kDoorCenterY    = kDoorHalfY;                  // bottom sits at y=0
// Slide UP by the full door height so the bottom rises to the lintel: the
// passage (y in [0, 2.1]) is then clear.
constexpr float kSlideUp        = kPassageTop;                 // 2.1 m
constexpr float kOpenDuration   = 1.0f;                        // seconds
} // namespace

uint32_t DoorSystem::add(const Door& d) {
    uint32_t i = (uint32_t)m_doors.size();
    m_doors.push_back(d);
    return i;
}

Door* DoorSystem::findByEntity(uint32_t entityId) {
    for (Door& d : m_doors)
        if (d.entity == entityId) return &d;
    return nullptr;
}

const Door* DoorSystem::findByEntity(uint32_t entityId) const {
    for (const Door& d : m_doors)
        if (d.entity == entityId) return &d;
    return nullptr;
}

bool DoorSystem::startOpening(Door& d) const {
    if (d.locked) return false;                  // §6.4: locked doors stay shut
    if (d.state != DoorState::Closed) return false;
    d.state = DoorState::Opening;
    d.t = 0.0f;
    return true;
}

void DoorSystem::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics) {
    if (dt <= 0.0f) return;
    for (Door& d : m_doors) {
        if (d.state != DoorState::Opening) continue;
        d.t += dt;
        float u = d.duration > 0.0f ? (d.t / d.duration) : 1.0f;
        if (u >= 1.0f) { u = 1.0f; d.state = DoorState::Open; }
        // Lerp closed -> open and drive the body.
        x3::phys::Vec3 p{
            d.closedPos.x + (d.openPos.x - d.closedPos.x) * u,
            d.closedPos.y + (d.openPos.y - d.closedPos.y) * u,
            d.closedPos.z + (d.openPos.z - d.closedPos.z) * u,
        };
        physics.setBodyPosition(d.body, p);
        // Refresh the Entity transform translation so the render mesh follows.
        // (Scene::update would also do this, but we update here so the door
        // tracks even if the caller skips a scene sync this frame.)
        if (d.entity != kNoLink && d.entity < scene.size()) {
            Entity& e = scene.get(d.entity);
            e.transform[12] = p.x;
            e.transform[13] = p.y;
            e.transform[14] = p.z;
            e.transform[15] = 1.0f;
        }
    }
}

bool tryUse(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir, float maxDist,
            Scene& scene, DoorSystem& doors, x3::phys::IPhysicsWorld& physics) {
    // Ray against static geometry (walls, button, door are all Static-layer).
    x3::phys::RayHit hit = physics.rayCast(eye, dir, maxDist, x3::phys::Layer::Static);
    if (!hit.hit || !hit.body.valid()) return false;

    uint32_t ent = scene.entityForBody(hit.body);
    if (ent == kNoLink || ent >= scene.size()) return false;

    const Entity& e = scene.get(ent);
    if (e.tag != (uint32_t)Tag::Button) return false;     // aimed at not-a-button
    if (e.link == kNoLink || e.link >= scene.size()) return false;

    Door* door = doors.findByEntity(e.link);
    if (!door) return false;
    return doors.startOpening(*door);
}

uint32_t buildDoorAndButton(Scene& scene, DoorSystem& doors,
                            x3::rhi::IRenderDevice& device,
                            x3::phys::IPhysicsWorld& physics) {
    // ---- Door (fills the doorway gap when closed) ----
    const x3::phys::Vec3 doorClosed{ 0.0f, kDoorCenterY, kDoorEdgeZ };
    const x3::phys::Vec3 doorOpen{ doorClosed.x, doorClosed.y + kSlideUp, doorClosed.z };

    uint32_t doorEntId;
    {
        // Render mesh authored centered at the body origin (NOT world-baked) so
        // the Entity transform translation drives its position as the body moves.
        x3::prims::PrimMesh geo = x3::prims::makeBox(kDoorHalfX, kDoorHalfY, kDoorHalfZ,
                                                     0.0f, 0.0f, 0.0f, 1.0f);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        // Distinct door look: a solid orange-red tint (no texture -> flat color).
        e.baseColor[0] = 0.85f; e.baseColor[1] = 0.30f; e.baseColor[2] = 0.18f; e.baseColor[3] = 1.0f;
        e.tag = (uint32_t)Tag::Door;
        // Static body (mass 0): blocks the character while closed, repositioned
        // via setBodyPosition while opening. Box half-extents == render extents.
        e.body = physics.addBox(x3::phys::Vec3{kDoorHalfX, kDoorHalfY, kDoorHalfZ},
                                doorClosed, 0.0f, x3::phys::Layer::Static);
        // Authored transform translation = closed position (Scene::update / the
        // door system overwrite this each frame from the body).
        e.transform[12] = doorClosed.x;
        e.transform[13] = doorClosed.y;
        e.transform[14] = doorClosed.z;
        doorEntId = scene.add(e);
    }

    Door d;
    d.entity    = doorEntId;
    d.body      = scene.get(doorEntId).body;
    d.closedPos = doorClosed;
    d.openPos   = doorOpen;
    d.duration  = kOpenDuration;
    d.state     = DoorState::Closed;
    doors.add(d);

    // ---- Button (small box on the right wall segment, beside the doorway) ----
    // Right +Z wall segment spans x in [+0.6, +8.0] at z=8.0; mount the button
    // on its inner face (z just inside the room) at a comfy 1.3 m height.
    const float kBtnHalf = 0.12f;
    const x3::phys::Vec3 btnPos{ 1.1f, 1.3f, kDoorEdgeZ - kDoorHalfZ - kBtnHalf };
    uint32_t btnEntId;
    {
        x3::prims::PrimMesh geo = x3::prims::makeBox(kBtnHalf, kBtnHalf, kBtnHalf,
                                                     0.0f, 0.0f, 0.0f, 1.0f);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        // Distinct button look: bright cyan-green.
        e.baseColor[0] = 0.20f; e.baseColor[1] = 0.85f; e.baseColor[2] = 0.55f; e.baseColor[3] = 1.0f;
        e.tag  = (uint32_t)Tag::Button;
        e.link = doorEntId;      // link the button to its door
        // Small static collision box so the use-ray can hit it.
        e.body = physics.addBox(x3::phys::Vec3{kBtnHalf, kBtnHalf, kBtnHalf},
                                btnPos, 0.0f, x3::phys::Layer::Static);
        e.transform[12] = btnPos.x;
        e.transform[13] = btnPos.y;
        e.transform[14] = btnPos.z;
        btnEntId = scene.add(e);
    }

    x3::logInfo("buildDoorAndButton: door entity " + std::to_string(doorEntId) +
                " + button entity " + std::to_string(btnEntId) +
                " (button links to door)");
    return btnEntId;
}

// ---------------------------------------------------------------------------
// Generalized door (+ optional button) at an arbitrary doorway (Level 1).
// ---------------------------------------------------------------------------
uint32_t buildLevelDoor(Scene& scene, DoorSystem& doors,
                        x3::rhi::IRenderDevice& device,
                        x3::phys::IPhysicsWorld& physics,
                        const DoorSpec& spec) {
    const float hw = spec.halfWidth;
    const float hh = spec.height * 0.5f;
    const float ht = spec.thickness * 0.5f;
    // Half-extents in (x,y,z) depend on which axis the host wall runs along: the
    // door spans the doorway width along the wall run and is thin across it.
    x3::phys::Vec3 half;
    if (spec.axis == DoorAxis::AlongX) {
        // Wall plane is Z = const: door is wide in X (the run), thin in Z.
        half = x3::phys::Vec3{ hw, hh, ht };
    } else {
        // Wall plane is X = const: door is thin in X, wide in Z (the run).
        half = x3::phys::Vec3{ ht, hh, hw };
    }

    // Closed body center: the doorway center, raised so the slab bottom sits at
    // the floor (doorwayCenter.y). Open: slid straight UP by the door height.
    const x3::phys::Vec3 closedPos{ spec.doorwayCenter.x,
                                    spec.doorwayCenter.y + hh,
                                    spec.doorwayCenter.z };
    const x3::phys::Vec3 openPos{ closedPos.x, closedPos.y + spec.height, closedPos.z };

    uint32_t doorEntId;
    {
        // Render mesh authored centered at the body origin (NOT world-baked) so
        // the Entity transform translation drives its position as the body moves.
        x3::prims::PrimMesh geo = x3::prims::makeBox(half.x, half.y, half.z, 0, 0, 0, 1.0f);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.baseColor[0] = spec.tint[0]; e.baseColor[1] = spec.tint[1];
        e.baseColor[2] = spec.tint[2]; e.baseColor[3] = spec.tint[3];
        e.tag  = (uint32_t)Tag::Door;
        e.body = physics.addBox(half, closedPos, 0.0f, x3::phys::Layer::Static);
        e.transform[12] = closedPos.x;
        e.transform[13] = closedPos.y;
        e.transform[14] = closedPos.z;
        doorEntId = scene.add(e);
    }

    Door d;
    d.entity    = doorEntId;
    d.body      = scene.get(doorEntId).body;
    d.closedPos = closedPos;
    d.openPos   = openPos;
    d.duration  = spec.duration;
    d.state     = DoorState::Closed;
    d.locked    = spec.locked;
    uint32_t doorIdx = doors.add(d);

    // Optional linked button, mounted on the wall beside the doorway, on the
    // approach (−) side along the axis of travel so the player can press it from
    // the room they arrive in. Cyan-green like the original button.
    if (spec.withButton) {
        const float kBtnHalf = 0.12f;
        x3::phys::Vec3 btnPos;
        if (spec.axis == DoorAxis::AlongX) {
            // Doorway in a Z=const wall: button to +X of the opening, on the −Z face.
            btnPos = x3::phys::Vec3{ spec.doorwayCenter.x + hw + 0.5f, 1.3f,
                                     spec.doorwayCenter.z - ht - kBtnHalf };
        } else {
            // Doorway in an X=const wall: button to +Z of the opening, on the −X face.
            btnPos = x3::phys::Vec3{ spec.doorwayCenter.x - ht - kBtnHalf, 1.3f,
                                     spec.doorwayCenter.z + hw + 0.5f };
        }
        x3::prims::PrimMesh geo = x3::prims::makeBox(kBtnHalf, kBtnHalf, kBtnHalf, 0, 0, 0, 1.0f);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.baseColor[0] = 0.20f; e.baseColor[1] = 0.85f; e.baseColor[2] = 0.55f; e.baseColor[3] = 1.0f;
        e.tag  = (uint32_t)Tag::Button;
        e.link = doorEntId;
        e.body = physics.addBox(x3::phys::Vec3{ kBtnHalf, kBtnHalf, kBtnHalf },
                                btnPos, 0.0f, x3::phys::Layer::Static);
        e.transform[12] = btnPos.x;
        e.transform[13] = btnPos.y;
        e.transform[14] = btnPos.z;
        scene.add(e);
    }

    x3::logInfo("buildLevelDoor: door idx " + std::to_string(doorIdx) +
                " entity " + std::to_string(doorEntId) +
                (spec.locked ? " [LOCKED]" : "") +
                (spec.withButton ? " + button" : ""));
    return doorIdx;
}

// ===========================================================================
// Headless self-test (--test-interact). T1-T4.
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[interact-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[interact-test] FAIL ") + name); }
}

constexpr float kFixedDt = 1.0f / 60.0f;

// Minimal headless IRenderDevice: hands out monotonically-increasing valid
// handles so buildTestLevel()/buildDoorAndButton() run unchanged with no Vulkan.
// All draw/frame/camera calls are no-ops.
class HeadlessDevice final : public x3::rhi::IRenderDevice {
public:
    bool init(const x3::rhi::DeviceDesc&) override { return true; }
    void shutdown() override {}
    void onResize(uint32_t, uint32_t) override {}
    void setCamera(float, float, float, float, float, float) override {}
    x3::rhi::FrameContext beginFrame() override { return {}; }
    void endFrame(const x3::rhi::FrameContext&) override {}
    x3::rhi::MeshHandle createMesh(const x3::rhi::MeshVertex*, uint32_t,
                                   const uint32_t*, uint32_t) override {
        return x3::rhi::MeshHandle{ m_next++ };
    }
    void destroyMesh(x3::rhi::MeshHandle) override {}
    void updateMesh(x3::rhi::MeshHandle, const x3::rhi::MeshVertex*, uint32_t) override {}
    x3::rhi::TextureHandle createTexture(const void*, uint32_t, uint32_t, bool) override {
        return x3::rhi::TextureHandle{ m_next++ };
    }
    void destroyTexture(x3::rhi::TextureHandle) override {}
    void drawMesh(const x3::rhi::FrameContext&, x3::rhi::MeshHandle,
                  x3::rhi::TextureHandle, const float[4], const float[16]) override {}
    void setPointLights(const x3::rhi::PointLight*, uint32_t) override {}
    void drawHudQuad(const x3::rhi::FrameContext&, float, float, float, float, const float[4]) override {}
    void drawHudText(const x3::rhi::FrameContext&, const char*, float, float, float, const float[4]) override {}
    void hudSize(uint32_t& w, uint32_t& h) const override { w = 0; h = 0; }
    x3::rhi::RenderStats stats() const override { return {}; }
    void armCapture(const char*) override {}                    // headless: no swapchain
    bool captureFrame(const char*) override { return false; }  // headless: no swapchain
    bool supportsDescriptorIndexing() const override { return false; }
    bool supportsMeshShaders() const override { return false; }
private:
    uint32_t m_next = 1;
};

// Build "eye -> button" aim from a known eye position toward the button entity's
// world center (so the test does not depend on player look math).
x3::phys::Vec3 sub(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
    return x3::phys::Vec3{ a.x - b.x, a.y - b.y, a.z - b.z };
}

} // namespace

bool runInteractSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();

    HeadlessDevice device;
    Scene scene;
    DoorSystem doors;

    // Full S2 graybox room (floor + walls + doorway gap + step), then the door
    // + linked button filling that gap. Mirrors the real app's construction.
    buildTestLevel(scene, device, *physics);
    uint32_t btnEnt = buildDoorAndButton(scene, doors, device, *physics);

    // Locate the door (the single registered door).
    Door& door = doors.at(0);
    const x3::phys::Vec3 btnCenter = physics->getBodyPosition(scene.get(btnEnt).body);

    // ---- T1: aim AT the button within range -> button hit, Closed->Opening ---
    {
        // Eye 2.5 m in front of (toward -Z from) the button, at button height.
        x3::phys::Vec3 eye{ btnCenter.x, btnCenter.y, btnCenter.z - 2.5f };
        x3::phys::Vec3 dir = sub(btnCenter, eye);   // points at the button
        bool started = tryUse(eye, dir, 3.0f, scene, doors, *physics);
        bool opening = door.state == DoorState::Opening;
        check(started && opening, "T1 use-ray at button starts door Opening");
    }

    // Record the closed-door body center for the T2/T4 doorway checks.
    const x3::phys::Vec3 doorClosedCenter = door.closedPos;

    // ---- T2: after ~1 s of stepping, door reaches Open and has moved ---------
    {
        // Step the door system + physics for ~1.1 s (66 frames @ 1/60).
        for (int i = 0; i < 66; ++i) {
            doors.update(kFixedDt, scene, *physics);
            physics->step(kFixedDt);
        }
        bool open = door.state == DoorState::Open;
        x3::phys::Vec3 now = physics->getBodyPosition(door.body);
        float moved = std::sqrt((now.x - doorClosedCenter.x) * (now.x - doorClosedCenter.x) +
                                (now.y - doorClosedCenter.y) * (now.y - doorClosedCenter.y) +
                                (now.z - doorClosedCenter.z) * (now.z - doorClosedCenter.z));
        // Slide offset is the full door height (2.1 m up).
        bool slid = moved > 2.0f;
        // The doorway volume (y in [0, 2.1] at x=0, z=8) is no longer occupied:
        // a ray straight DOWN through the opening from above should now pass the
        // door (the closed door's top was at y=2.1; open door bottom is at 2.1).
        // Check directly: door body center y is now well above the passage.
        bool clear = now.y > kPassageTop;       // center above the clear opening
        check(open && slid && clear, "T2 door reaches Open + body slid clear of doorway");
    }

    // ---- T3: aiming AWAY (or beyond range) does NOT open a (fresh) door ------
    {
        // Fresh world so we start from a Closed door.
        std::unique_ptr<x3::phys::IPhysicsWorld> p2(x3::phys::createPhysicsWorld());
        p2->init();
        HeadlessDevice dev2;
        Scene s2;
        DoorSystem d2;
        buildTestLevel(s2, dev2, *p2);
        uint32_t b2 = buildDoorAndButton(s2, d2, dev2, *p2);
        const x3::phys::Vec3 bc = p2->getBodyPosition(s2.get(b2).body);

        // (a) Aim AWAY from the button (opposite direction).
        x3::phys::Vec3 eye{ bc.x, bc.y, bc.z - 2.5f };
        x3::phys::Vec3 away{ 0.0f, 0.0f, -1.0f };   // points further -Z, away from button
        bool startedAway = tryUse(eye, away, 3.0f, s2, d2, *p2);

        // (b) Aim at the button but BEYOND range (button is 2.5 m away; maxDist 1.0).
        x3::phys::Vec3 toBtn = sub(bc, eye);
        bool startedFar = tryUse(eye, toBtn, 1.0f, s2, d2, *p2);

        bool stillClosed = d2.at(0).state == DoorState::Closed;
        check(!startedAway && !startedFar && stillClosed,
              "T3 aiming away / out of range does not open the door");
        p2->shutdown();
    }

    // ---- T4: closed door blocks the doorway; open door lets a character pass --
    {
        // Closed door: a character walking toward +Z at the doorway is stopped
        // before passing z=8. Open door: it passes through.
        auto runDoorway = [](bool openIt) -> float {
            std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
            w->init();
            HeadlessDevice dev;
            Scene s;
            DoorSystem dd;
            buildTestLevel(s, dev, *w);
            buildDoorAndButton(s, dd, dev, *w);
            Door& dr = dd.at(0);
            if (openIt) {
                // Drive the door fully open before the character approaches.
                dd.startOpening(dr);   // Closed -> Opening
                for (int i = 0; i < 70; ++i) { dd.update(kFixedDt, s, *w); w->step(kFixedDt); }
            }
            // Character starts inside the room, on the doorway centerline, walks +Z.
            x3::phys::BodyId chr = w->createCharacter(0.3f, 1.8f, x3::phys::Vec3{0.0f, 0.05f, 6.0f});
            for (int i = 0; i < 30; ++i) { w->moveCharacter(chr, x3::phys::Vec3{0,0,0}, kFixedDt); w->step(kFixedDt); }
            // Push toward +Z (through the doorway at z=8) for ~4 s.
            for (int i = 0; i < 240; ++i) {
                w->moveCharacter(chr, x3::phys::Vec3{0,0,4.0f}, kFixedDt);
                w->step(kFixedDt);
            }
            float z = w->getBodyPosition(chr).z;
            w->shutdown();
            return z;
        };

        float zClosed = runDoorway(false);  // should be stopped before/at the door
        float zOpen   = runDoorway(true);   // should pass beyond z=8
        bool blocked = zClosed < 8.0f;      // never reached the door plane
        bool passed  = zOpen   > 8.2f;      // walked out through the doorway
        check(blocked && passed, "T4 closed door blocks doorway, open door passes");
    }

    physics->shutdown();

    x3::logInfo(std::string("[interact-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
