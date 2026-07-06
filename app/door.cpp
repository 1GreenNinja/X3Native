// Button -> door interaction (S4). See app/door.h.
//
// Clean-room: built from the IPhysicsWorld + Scene interfaces only.
#include "door.h"
#include "headless_device.h"
#include "level.h"
#include "mesh_prims.h"
#include "asset_root.h"

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

// ---- Shared SM_Door_A GLB visual ------------------------------------------
// Loose-GLB root + relative path of the real sci-fi door mesh (same kit as
// env_art.cpp). The door mesh is drawn OVER the (now invisible) collision box.
// Root resolved via assetRoot() (assets-LFS pass): repo-relative assets/ first,
// G:/GameModels fallback. Lazy-resolved once (the exe path is stable).
inline const std::string& kDoorGlbDir() {
    static const std::string d = convertedGlbRoot();
    return d;
}
const char* kDoorGlbRel = "ModularSciFi_Interior/SM_Door_A.glb";

// Probed WORLD-space AABB of SM_Door_A AFTER the GLB node TRS is applied (the
// space makeDrawables() bakes into nodeTransform — measured with python parsing
// the glb POSITION accessor min/max through the node hierarchy):
//   min (-4.875, 0.054, -0.112)  max (-2.525, 3.554, 0.112)
//   size 2.350 (X wide) x 3.500 (Y tall) x 0.224 (Z thick), centered at
//   X=-3.700, Y=1.804, Z=0.0. The slab faces along Z (thin in Z, wide in X).
struct GlbAabb { float minx,miny,minz, maxx,maxy,maxz; };
constexpr GlbAabb kDoorAabb { -4.875f, 0.054f, -0.112f, -2.525f, 3.554f, 0.112f };
inline float gcx(const GlbAabb& a){ return (a.minx+a.maxx)*0.5f; }
inline float gcz(const GlbAabb& a){ return (a.minz+a.maxz)*0.5f; }
constexpr float kDoorGlbW = kDoorAabb.maxx - kDoorAabb.minx;  // 2.35 natural width (X)
constexpr float kDoorGlbH = kDoorAabb.maxy - kDoorAabb.miny;  // 3.50 natural height (Y)
} // namespace

uint32_t DoorSystem::add(const Door& d) {
    uint32_t i = (uint32_t)m_doors.size();
    m_doors.push_back(d);
    return i;
}

Door* DoorSystem::findByEntity(uint32_t entityId) {
    for (Door& d : m_doors)
        if (d.entity == entityId || d.entity2 == entityId) return &d;
    return nullptr;
}

const Door* DoorSystem::findByEntity(uint32_t entityId) const {
    for (const Door& d : m_doors)
        if (d.entity == entityId || d.entity2 == entityId) return &d;
    return nullptr;
}

// W2-A2: 3D door-sound emission at the door's body position. Silent when the host
// never wired audio (headless tests) or a WAV failed to load (clean machines).
void DoorSystem::playDoorSound(const Door& d, x3::audio::SoundHandle h, float vol) const {
    if (!m_audio || !h.valid()) return;
    m_audio->playSound3D(h, d.closedPos.x, d.closedPos.y, d.closedPos.z, vol, 1.0f);
}

bool DoorSystem::startOpening(Door& d) const {
    if (d.locked) { playDoorSound(d, m_sndLocked, 0.55f); return false; }  // §6.4 + denied buzz
    if (d.state != DoorState::Closed) return false;
    d.state = DoorState::Opening;
    d.t = 0.0f;
    playDoorSound(d, m_sndOpen, 0.8f);
    return true;
}

bool DoorSystem::toggle(Door& d) const {
    switch (d.state) {
        case DoorState::Closed:
            if (d.locked) { playDoorSound(d, m_sndLocked, 0.55f); return false; }  // §6.4
            d.state = DoorState::Opening;         // t is already 0
            playDoorSound(d, m_sndOpen, 0.8f);
            return true;
        case DoorState::Open:
            d.state = DoorState::Closing;         // t is already == duration
            playDoorSound(d, m_sndClose, 0.8f);
            return true;
        case DoorState::Opening:
            d.state = DoorState::Closing;         // reverse mid-slide (keep t)
            playDoorSound(d, m_sndClose, 0.6f);
            return true;
        case DoorState::Closing:
            d.state = DoorState::Opening;         // reverse mid-slide (keep t)
            playDoorSound(d, m_sndOpen, 0.6f);
            return true;
    }
    return false;
}

bool DoorSystem::tryDoorCode(const x3::phys::Vec3& eye, int code, float range) {
    const float r2 = range * range;
    int best = -1; float bestD2 = r2;
    for (uint32_t i = 0; i < (uint32_t)m_doors.size(); ++i) {
        const Door& d = m_doors[i];
        if (!d.locked || d.code == 0) continue;       // only locked, coded doors
        const float dx = eye.x - d.closedPos.x, dz = eye.z - d.closedPos.z;
        const float d2 = dx * dx + dz * dz;
        if (d2 <= bestD2) { bestD2 = d2; best = (int)i; }
    }
    if (best < 0) return false;
    Door& d = m_doors[(uint32_t)best];
    if (d.code != code) return false;                 // wrong code: stays locked
    unlock(d);                                         // clears locked (incl. the keycard gate)
    return startOpening(d);
}

Door* DoorSystem::nearestLockedDoor(const x3::phys::Vec3& eye, float range) {
    const float r2 = range * range;
    int best = -1; float bestD2 = r2;
    for (uint32_t i = 0; i < (uint32_t)m_doors.size(); ++i) {
        const Door& d = m_doors[i];
        if (!d.locked) continue;
        const float dx = eye.x - d.closedPos.x, dz = eye.z - d.closedPos.z;
        const float d2 = dx * dx + dz * dz;
        if (d2 <= bestD2) { bestD2 = d2; best = (int)i; }
    }
    return best < 0 ? nullptr : &m_doors[(uint32_t)best];
}

void DoorSystem::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics) {
    if (dt <= 0.0f) return;
    for (Door& d : m_doors) {
        // Move the shared progress cursor and settle to the terminal states.
        if (d.state == DoorState::Opening) {
            d.t += dt;
            if (d.t >= d.duration) { d.t = d.duration; d.state = DoorState::Open; }
        } else if (d.state == DoorState::Closing) {
            d.t -= dt;
            if (d.t <= 0.0f) { d.t = 0.0f; d.state = DoorState::Closed; }
        } else {
            continue;   // Closed / Open: nothing to animate this frame
        }
        const float u = d.duration > 0.0f ? (d.t / d.duration) : 1.0f;
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
        // SECOND PANEL (two-panel floor hatch): slide it the opposite way on the
        // SAME cursor so the two halves part from / close to the centre together.
        if (d.body2.valid()) {
            x3::phys::Vec3 p2{
                d.closedPos2.x + (d.openPos2.x - d.closedPos2.x) * u,
                d.closedPos2.y + (d.openPos2.y - d.closedPos2.y) * u,
                d.closedPos2.z + (d.openPos2.z - d.closedPos2.z) * u,
            };
            physics.setBodyPosition(d.body2, p2);
            if (d.entity2 != kNoLink && d.entity2 < scene.size()) {
                Entity& e2 = scene.get(d.entity2);
                e2.transform[12] = p2.x;
                e2.transform[13] = p2.y;
                e2.transform[14] = p2.z;
                e2.transform[15] = 1.0f;
            }
        }
    }
}

void DoorSystem::loadDoorMesh(x3::rhi::IRenderDevice& device, std::string_view convertedGlbDir) {
    if (m_meshOk || m_loader) return;   // already loaded (or already tried)

    m_assets.reset(x3::asset::createAssetSource());
    if (!m_assets->mountDir(convertedGlbDir, 0)) {
        x3::logWarn("[door] mountDir failed: " + std::string(convertedGlbDir) +
                    " — keeping graybox door box");
        return;
    }
    m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));
    m_doorModel = m_loader->load(kDoorGlbRel);
    if (m_doorModel.ok) {
        m_doorDrawables = x3::asset::makeDrawables(m_doorModel);
        m_meshOk = !m_doorDrawables.empty();
    }
    if (m_meshOk)
        x3::logInfo("[door] loaded " + std::string(kDoorGlbRel) + " — " +
                    std::to_string(m_doorDrawables.size()) + " drawable prim(s)");
    else
        x3::logWarn("[door] FAILED to load " + std::string(kDoorGlbRel) +
                    " (graybox door box kept)");
}

void DoorSystem::drawMeshes(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const {
    if (!m_meshOk) return;   // no real mesh -> the (still-visible) box renders instead

    // Uniform scale to fit each door's opening height: the GLB slab is 3.50 m tall;
    // scale so it stands at the door's clear-passage height (~2.1 m). The width then
    // becomes ~1.41 m (a touch wider than the 1.2 m opening — reads as a real door
    // straddling the frame, like env_art's 2.0 m door FRAME). Thickness ~0.13 m.
    const float ax  = gcx(kDoorAabb);   // GLB anchor X (slab center, ~-3.70)
    const float az  = gcz(kDoorAabb);   // GLB anchor Z (~0)
    const float ay  = kDoorAabb.miny;   // GLB anchor Y (slab BOTTOM, ~0.054)

    for (const Door& d : m_doors) {
        if (d.floorHatch) continue;   // horizontal hatch draws its own graybox box
        // W2-A2: room-visibility gate — walls cull per-room; without this, slabs in
        // culled rooms drew anyway and floated in void from outside-shell sightlines.
        if (m_visQuery && !m_visQuery(d)) continue;
        // CURRENT world center of this door (the slide animation lerps closed->open
        // by the same factor DoorSystem::update() uses). closedPos/openPos are the
        // body CENTER positions; the slab bottom is center.y - height/2.
        // Slide factor from the shared cursor — works for Opening, Closing AND
        // the terminal Open/Closed states (Open pins to 1, Closed to 0).
        const float u = (d.state == DoorState::Open)
            ? 1.0f
            : (d.duration > 0.0f ? std::min(std::max(d.t / d.duration, 0.0f), 1.0f) : 0.0f);
        const float cxw = d.closedPos.x + (d.openPos.x - d.closedPos.x) * u;
        const float cyw = d.closedPos.y + (d.openPos.y - d.closedPos.y) * u;
        const float czw = d.closedPos.z + (d.openPos.z - d.closedPos.z) * u;
        const float bottomY = cyw - d.height * 0.5f;   // floor-level bottom of the slab

        const float s = d.height / kDoorGlbH;          // uniform scale (height fit)
        // Orient the slab to the host wall: GLB is wide in X / thin in Z by default.
        //   AlongZ (axis 0): wall plane x=const, opening runs along Z -> yaw 90deg.
        //   AlongX (axis 1): wall plane z=const, opening runs along X -> yaw 0.
        const float yaw = (d.axis == 0) ? (3.14159265358979f * 0.5f) : 0.0f;
        const float cs = std::cos(yaw), sn = std::sin(yaw);

        // Column-major TRS: world = T(c) * R_y(yaw) * S(s) * T(-anchor), placing the
        // GLB anchor (ax, ay, az) at the world bottom-center (cxw, bottomY, czw).
        float m[16];
        m[0]=cs*s;  m[1]=0;   m[2]=-sn*s; m[3]=0;
        m[4]=0;     m[5]=s;   m[6]=0;     m[7]=0;
        m[8]=sn*s;  m[9]=0;   m[10]=cs*s; m[11]=0;
        const float rpx = (cs*ax + sn*az) * s;
        const float rpy = (ay) * s;
        const float rpz = (-sn*ax + cs*az) * s;
        m[12]=cxw - rpx; m[13]=bottomY - rpy; m[14]=czw - rpz; m[15]=1.0f;

        for (const auto& dr : m_doorDrawables) {
            float fin[16];
            x3::asset::mulMat4(m, dr.nodeTransform, fin);
            device.drawMesh(frame,
                            x3::rhi::MeshHandle{ dr.meshId },
                            x3::rhi::TextureHandle{ dr.baseColorTexId },
                            dr.baseColorFactor,
                            fin);
        }
    }
}

Door* pickAimedDoor(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir, float maxDist,
                    Scene& scene, DoorSystem& doors, x3::phys::IPhysicsWorld& physics) {
    // Ray against static geometry (walls, button, door are all Static-layer).
    x3::phys::RayHit hit = physics.rayCast(eye, dir, maxDist, x3::phys::Layer::Static);
    if (!hit.hit || !hit.body.valid()) return nullptr;

    uint32_t ent = scene.entityForBody(hit.body);
    if (ent == kNoLink || ent >= scene.size()) return nullptr;

    const Entity& e = scene.get(ent);

    // Aim at a wall BUTTON linked to a door...
    if (e.tag == (uint32_t)Tag::Button) {
        if (e.link == kNoLink || e.link >= scene.size()) return nullptr;
        return doors.findByEntity(e.link);
    }
    // ...OR aim directly at the DOOR slab itself (intuitive "open/close the door").
    if (e.tag == (uint32_t)Tag::Door)
        return doors.findByEntity(ent);
    return nullptr;
}

bool tryUse(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir, float maxDist,
            Scene& scene, DoorSystem& doors, x3::phys::IPhysicsWorld& physics) {
    Door* door = pickAimedDoor(eye, dir, maxDist, scene, doors, physics);
    return door ? doors.toggle(*door) : false;   // open if closed, close if open
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
// Two-panel flush floor hatch (blast-door / iris). See door.h.
// ---------------------------------------------------------------------------
uint32_t buildFloorHatch(Scene& scene, DoorSystem& doors,
                         x3::rhi::IRenderDevice& device,
                         x3::phys::IPhysicsWorld& physics,
                         const DoorSpec& spec) {
    const float hw = spec.halfWidth;          // half the SQUARE opening (covers full Z)
    const float ht = spec.thickness * 0.5f;   // panel Y half-thickness
    const float c  = spec.doorwayCenter.x;    // opening centre X
    const float cy = spec.doorwayCenter.y;    // floor top Y (panels sit flush BELOW this)
    const float cz = spec.doorwayCenter.z;    // opening centre Z

    // Each panel covers half the opening in X (half-extent hw*0.5) and the full
    // depth in Z (half-extent hw). Top FLUSH with the floor: panel centre at
    // cy - ht (so the top surface is at cy, matching the surrounding floor slab).
    const float panelHalfX = hw * 0.5f;
    const x3::phys::Vec3 panelHalf{ panelHalfX, ht, hw };
    const float panelY = cy - ht;

    // Closed: panel 1 fills the -X half (centre at c - panelHalfX), panel 2 the +X
    // half (centre at c + panelHalfX) — together they cover [c-hw, c+hw] flush.
    const x3::phys::Vec3 closed1{ c - panelHalfX, panelY, cz };
    const x3::phys::Vec3 closed2{ c + panelHalfX, panelY, cz };
    // Open: each panel slides OUTWARD by the full opening width (2*hw) so each
    // clears its half and the whole [c-hw, c+hw] X span is open — drop-through.
    const x3::phys::Vec3 open1{ closed1.x - 2.0f * hw, panelY, cz };
    const x3::phys::Vec3 open2{ closed2.x + 2.0f * hw, panelY, cz };

    // Panel material: the cell floor texture/tint when supplied (flush + textured),
    // else the steel `tint`. Use a per-panel render mesh authored centred at the
    // body origin so the Entity transform drives its slide.
    const bool useFloor = spec.floorTex.valid();
    const float* col = useFloor ? spec.floorTint : spec.tint;

    auto buildPanel = [&](const x3::phys::Vec3& closed) -> uint32_t {
        x3::prims::PrimMesh geo = x3::prims::makeBox(panelHalf.x, panelHalf.y, panelHalf.z, 0, 0, 0, 1.0f);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        if (useFloor) e.tex = spec.floorTex;
        e.baseColor[0]=col[0]; e.baseColor[1]=col[1]; e.baseColor[2]=col[2]; e.baseColor[3]=col[3];
        e.tag = (uint32_t)Tag::Door;
        e.visible = true;                       // the flush panels ARE the visual (no GLB)
        e.body = physics.addBox(panelHalf, closed, 0.0f, x3::phys::Layer::Static);
        e.transform[12]=closed.x; e.transform[13]=closed.y; e.transform[14]=closed.z;
        return scene.add(e);
    };

    const uint32_t ent1 = buildPanel(closed1);
    const uint32_t ent2 = buildPanel(closed2);

    Door d;
    d.entity     = ent1;
    d.body       = scene.get(ent1).body;
    d.closedPos  = closed1;
    d.openPos    = open1;
    d.entity2    = ent2;
    d.body2      = scene.get(ent2).body;
    d.closedPos2 = closed2;
    d.openPos2   = open2;
    d.duration   = spec.duration;
    d.state      = DoorState::Closed;
    d.locked     = spec.locked;
    d.code       = spec.code;
    d.keycard    = spec.keycard;
    d.requireBoth = spec.requireBoth;
    d.axis       = (uint32_t)spec.axis;
    d.halfWidth  = spec.halfWidth;
    d.height     = spec.height;
    d.floorHatch = true;
    const uint32_t doorIdx = doors.add(d);

    x3::logInfo("buildFloorHatch: two-panel hatch idx " + std::to_string(doorIdx) +
                " entities " + std::to_string(ent1) + "+" + std::to_string(ent2) +
                (spec.locked ? " [LOCKED]" : "") + " (flush, parts from centre)");
    return doorIdx;
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
    // Half-extents + slide direction.
    x3::phys::Vec3 half, closedPos, openPos;
    if (spec.floorHatch) {
        // FLOOR HATCH (two-panel blast-door / iris): handled below in its own
        // builder so it can lay TWO flush, floor-textured panels that part from the
        // centre. buildFloorHatch() returns the door index directly.
        return buildFloorHatch(scene, doors, device, physics, spec);
    } else if (spec.axis == DoorAxis::AlongX) {
        // Wall plane is Z = const: door is wide in X (the run), thin in Z. Slides UP.
        half      = x3::phys::Vec3{ hw, hh, ht };
        closedPos = x3::phys::Vec3{ spec.doorwayCenter.x, spec.doorwayCenter.y + hh, spec.doorwayCenter.z };
        openPos   = x3::phys::Vec3{ closedPos.x, closedPos.y + spec.height, closedPos.z };
    } else {
        // Wall plane is X = const: door is thin in X, wide in Z (the run). Slides UP.
        half      = x3::phys::Vec3{ ht, hh, hw };
        closedPos = x3::phys::Vec3{ spec.doorwayCenter.x, spec.doorwayCenter.y + hh, spec.doorwayCenter.z };
        openPos   = x3::phys::Vec3{ closedPos.x, closedPos.y + spec.height, closedPos.z };
    }

    // Load the shared real-door GLB once (idempotent). The visual is the GLB slab
    // drawn by drawMeshes(); the procedural box below stays as collision only.
    doors.loadDoorMesh(device, kDoorGlbDir());

    uint32_t doorEntId;
    {
        // Render mesh authored centered at the body origin (NOT world-baked) so
        // the Entity transform translation drives its position as the body moves.
        // The box is now COLLISION-ONLY (visible=false): the real SM_Door_A GLB is
        // drawn over it by DoorSystem::drawMeshes(). The box still blocks the
        // player while closed and is repositioned each frame as the door slides.
        x3::prims::PrimMesh geo = x3::prims::makeBox(half.x, half.y, half.z, 0, 0, 0, 1.0f);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.baseColor[0] = spec.tint[0]; e.baseColor[1] = spec.tint[1];
        e.baseColor[2] = spec.tint[2]; e.baseColor[3] = spec.tint[3];
        e.tag  = (uint32_t)Tag::Door;
        // Hide the flat-color box when a real (vertical) door mesh is available; if
        // the GLB failed to load, keep the box. A FLOOR HATCH always shows its box —
        // the vertical SM_Door GLB doesn't fit a horizontal hatch (drawMeshes skips it).
        e.visible = spec.floorHatch || !doors.hasDoorMesh();
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
    d.code      = spec.code;
    d.keycard   = spec.keycard;
    d.requireBoth = spec.requireBoth;
    d.axis      = (uint32_t)spec.axis;
    d.halfWidth = spec.halfWidth;
    d.height    = spec.height;
    d.floorHatch = spec.floorHatch;
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

// Headless IRenderDevice: the shared no-op test-double (app/headless_device.h).
// Mints monotonically-increasing valid handles so buildTestLevel()/
// buildDoorAndButton() run unchanged with no Vulkan; all draw/frame/camera
// calls are no-ops.
using HeadlessDevice = x3::game::HeadlessRenderDevice;

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

    // ---- T5: toggle OPENS then CLOSES a door (E to open AND close) -----------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> p5(x3::phys::createPhysicsWorld());
        p5->init();
        HeadlessDevice dev5;
        Scene s5;
        DoorSystem d5;
        buildTestLevel(s5, dev5, *p5);
        buildDoorAndButton(s5, d5, dev5, *p5);
        Door& dr = d5.at(0);
        // First toggle: Closed -> Opening; step it fully open.
        d5.toggle(dr);
        for (int i = 0; i < 70; ++i) { d5.update(kFixedDt, s5, *p5); p5->step(kFixedDt); }
        bool wasOpen = dr.state == DoorState::Open;
        // Second toggle: Open -> Closing; step it fully closed.
        bool tog = d5.toggle(dr);
        bool closing = dr.state == DoorState::Closing;
        for (int i = 0; i < 70; ++i) { d5.update(kFixedDt, s5, *p5); p5->step(kFixedDt); }
        bool backClosed = dr.state == DoorState::Closed;
        x3::phys::Vec3 now = p5->getBodyPosition(dr.body);
        bool downAgain = now.y < kPassageTop;   // slab dropped back to fill the doorway
        check(wasOpen && tog && closing && backClosed && downAgain,
              "T5 toggle opens then closes the door (E open/close)");
        p5->shutdown();
    }

    // ---- T6: a FLOOR HATCH blocks a body resting on it when CLOSED, and lets it
    // fall through when OPEN (the hatch slides aside). "The door on the floor." ----
    {
        auto runHatch = [](bool openIt) -> float {
            std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
            w->init();
            HeadlessDevice dev;
            Scene s;
            DoorSystem dd;
            // A floor hatch: 1.0 m half-size square opening at y=0, in open floor.
            DoorSpec spec;
            spec.doorwayCenter = x3::phys::Vec3{ 0.0f, 0.0f, 0.0f };
            spec.halfWidth = 1.0f; spec.thickness = 0.2f; spec.withButton = false;
            spec.floorHatch = true;
            buildLevelDoor(s, dd, dev, *w, spec);
            Door& h = dd.at(0);
            if (openIt) { dd.toggle(h); for (int i = 0; i < 70; ++i) { dd.update(kFixedDt, s, *w); w->step(kFixedDt); } }
            // Drop a dynamic box from just above the hatch centre.
            x3::phys::BodyId b = w->addBox(x3::phys::Vec3{0.2f,0.2f,0.2f}, x3::phys::Vec3{0,1.0f,0},
                                           2.0f, x3::phys::Layer::Dynamic);
            for (int i = 0; i < 120; ++i) { dd.update(kFixedDt, s, *w); w->step(kFixedDt); }
            float y = w->getBodyPosition(b).y;
            w->shutdown();
            return y;
        };
        float yClosed = runHatch(false);   // rests ON the closed hatch (stays up ~0.3+)
        float yOpen   = runHatch(true);    // falls THROUGH the open opening (goes well below 0)
        bool held = yClosed > 0.0f;
        bool fell = yOpen   < -1.0f;
        check(held && fell, "T6 floor hatch holds when closed, drops you through when open");
    }

    physics->shutdown();

    x3::logInfo(std::string("[interact-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
