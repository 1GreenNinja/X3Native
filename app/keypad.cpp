#include "keypad.h"

#include "mesh_prims.h"
#include "stairwell.h"   // KP7/KP8: the stairwell service-void code classification
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace x3::game {
namespace {

using x3::prims::PrimMesh;
using x3::prims::MeshVertex;

// ---- Mesh merge: append `src` into `dst`, offsetting indices. Keeps ONE high-poly
// mesh for the whole keypad (one entity, one draw). Render side only (the keypad is
// decorative — no collision). ----
void mergeMesh(PrimMesh& dst, const PrimMesh& src) {
    const uint32_t base = (uint32_t)dst.verts.size();
    dst.verts.insert(dst.verts.end(), src.verts.begin(), src.verts.end());
    for (uint32_t i : src.index) dst.index.push_back(base + i);
}

// A box authored in the keypad's LOCAL frame: front face at +Z. (x,y) is the panel
// plane (x right, y up); z is depth OUT of the wall (the front of the unit is at the
// largest +z). Centered at local (lx,ly,lz). Merged into `m`.
void localBox(PrimMesh& m, float hx, float hy, float hz, float lx, float ly, float lz) {
    mergeMesh(m, x3::prims::makeBox(hx, hy, hz, lx, ly, lz, 1.0f));
}

// Rotate a LOCAL-frame point (front = +Z) into WORLD space for the requested facing,
// then translate to the mount origin (ox,oy,oz). Local +Z maps to the facing normal;
// local +Y stays world +Y (keypads are upright); local +X is chosen so the unit reads
// correctly (right-handed). Returns the world position.
struct Basis { float rx[3]; float ry[3]; float rz[3]; };   // local x/y/z axes in world
Basis basisFor(KeypadFacing f) {
    // ry is always world up.
    Basis b{};
    b.ry[0] = 0; b.ry[1] = 1; b.ry[2] = 0;
    switch (f) {
        case KeypadFacing::PlusZ:  b.rz[0]=0;  b.rz[1]=0; b.rz[2]= 1; b.rx[0]= 1; b.rx[1]=0; b.rx[2]=0; break;
        case KeypadFacing::MinusZ: b.rz[0]=0;  b.rz[1]=0; b.rz[2]=-1; b.rx[0]=-1; b.rx[1]=0; b.rx[2]=0; break;
        case KeypadFacing::PlusX:  b.rz[0]=1;  b.rz[1]=0; b.rz[2]= 0; b.rx[0]= 0; b.rx[1]=0; b.rx[2]=-1; break;
        case KeypadFacing::MinusX: b.rz[0]=-1; b.rz[1]=0; b.rz[2]= 0; b.rx[0]= 0; b.rx[1]=0; b.rx[2]= 1; break;
    }
    return b;
}

// Transform an entire LOCAL-frame PrimMesh into world space (rotate by `b`, offset to
// the mount origin). Rotates normals too so lighting is correct on every facing.
void orientMesh(PrimMesh& m, const Basis& b, float ox, float oy, float oz) {
    auto xf = [&](const float v[3], float out[3]) {
        out[0] = b.rx[0]*v[0] + b.ry[0]*v[1] + b.rz[0]*v[2];
        out[1] = b.rx[1]*v[0] + b.ry[1]*v[1] + b.rz[1]*v[2];
        out[2] = b.rx[2]*v[0] + b.ry[2]*v[1] + b.rz[2]*v[2];
    };
    for (MeshVertex& v : m.verts) {
        float p[3] = { v.pos[0], v.pos[1], v.pos[2] };
        float n[3] = { v.normal[0], v.normal[1], v.normal[2] };
        float wp[3], wn[3];
        xf(p, wp); xf(n, wn);
        v.pos[0] = wp[0] + ox; v.pos[1] = wp[1] + oy; v.pos[2] = wp[2] + oz;
        v.normal[0] = wn[0]; v.normal[1] = wn[1]; v.normal[2] = wn[2];
    }
}

// Add a render-only entity from a finished world-space PrimMesh.
uint32_t addMeshEntity(Scene& scene, x3::rhi::IRenderDevice& device, const PrimMesh& m,
                       const float color[4], const float emissive[4], uint32_t roomId) {
    Entity e;
    e.mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                               m.index.data(), (uint32_t)m.index.size());
    for (int i = 0; i < 4; ++i) e.baseColor[i] = color[i];
    for (int i = 0; i < 4; ++i) e.emissive[i]  = emissive[i];
    e.tag     = (uint32_t)Tag::Prop;
    e.visible = true;
    e.roomId  = roomId;
    return scene.add(e);
}

// ---- Keypad local geometry constants (meters). Front face is at +Z. The unit is
// inset in layers so coplanar faces never coincide (no z-fighting): backplate at the
// rear, bezel frame proud of it, screen + keys proud of the recess floor. ----
constexpr float kHalfW   = 0.17f;   // half width  (0.34 m wide)
constexpr float kHalfH   = 0.23f;   // half height (0.46 m tall)
constexpr float kBackZ   = 0.010f;  // backplate front-face z (sits ~1 cm proud of wall)
constexpr float kBackT   = 0.020f;  // backplate half-thickness
constexpr float kBezelZ  = 0.034f;  // bezel front-face z
constexpr float kRecessZ = 0.020f;  // recessed inner face z (where screen+keys sit on)
constexpr float kKeyZ    = 0.030f;  // raised key front-face z (proud of the recess)
constexpr float kScreenZ = 0.026f;  // screen front-face z (proud of recess, behind keys)

// Build the keypad's BODY + GLOW geometry in WORLD space (no device needed). Exposed so
// the headless self-test can validate the geometry. `scrCy/scrW/scrH/ledCx/ledCy` are
// returned for the glow build. The body is the merged high-poly device; glow is the
// emissive screen+LED.
struct KeypadGeom { PrimMesh body; PrimMesh glow; };
KeypadGeom buildKeypadGeom(float x, float y, float z, KeypadFacing facing) {
    const Basis b = basisFor(facing);
    KeypadGeom g;
    PrimMesh& body = g.body;

    // Backplate (the full slab mounted to the wall). Its front face is the recess floor.
    localBox(body, kHalfW, kHalfH, kBackT, 0.0f, 0.0f, kBackZ + kBackT);

    // Bezel frame: four proud rails around the rim (top/bottom/left/right) standing
    // forward of the backplate, framing the recessed pan. Each rail is a slim box; the
    // rails overlap at the corners (clean — opaque boxes, no z-fight).
    const float railT  = 0.026f;                 // rail thickness across the rim
    const float railHz = (kBezelZ - kBackZ) * 0.5f;
    const float railCz = (kBezelZ + kBackZ) * 0.5f + kBackT; // center between back & bezel front
    // top & bottom rails (run in X)
    localBox(body, kHalfW, railT * 0.5f, railHz, 0.0f,  kHalfH - railT * 0.5f, railCz);
    localBox(body, kHalfW, railT * 0.5f, railHz, 0.0f, -kHalfH + railT * 0.5f, railCz);
    // left & right rails (run in Y), shortened so they don't double-cover the corners' depth
    const float sideH = kHalfH - railT;
    localBox(body, railT * 0.5f, sideH, railHz,  kHalfW - railT * 0.5f, 0.0f, railCz);
    localBox(body, railT * 0.5f, sideH, railHz, -kHalfW + railT * 0.5f, 0.0f, railCz);

    // Recessed inner pan floor (slightly proud of the backplate front so the keys/screen
    // have a surface to sit on without coinciding with the backplate face).
    const float panZ = kRecessZ;
    localBox(body, kHalfW - railT, kHalfH - railT, 0.004f, 0.0f, 0.0f, panZ + kBackT);

    // SCREEN housing (the dark inset the glowing screen sits in — top third of the pan).
    const float scrW = kHalfW - railT - 0.012f;
    const float scrH = 0.060f;
    const float scrCy = kHalfH - railT - scrH - 0.012f;
    localBox(body, scrW, scrH, 0.006f, 0.0f, scrCy, kScreenZ - 0.006f + kBackT);

    // 3x4 KEY GRID (digits 1..9, then * 0 #) — each a real raised, beveled key. The keys
    // occupy the lower two-thirds of the pan. Each key = a base box + a smaller proud cap
    // (the bevel) so it reads as a physical button with relief.
    const float gridTop = scrCy - scrH - 0.018f;   // just under the screen
    const int   cols = 3, rows = 4;
    const float keyPitchX = (2.0f * (kHalfW - railT - 0.012f)) / cols;
    const float keyPitchY = 0.066f;
    const float keyHx = keyPitchX * 0.40f;         // gap between keys => no z-fight, clean seams
    const float keyHy = keyPitchY * 0.40f;
    const float gridLeft = -(cols - 1) * 0.5f * keyPitchX;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const float kx = gridLeft + c * keyPitchX;
            const float ky = gridTop - r * keyPitchY;
            // key base (sits on the pan, rises toward the front)
            localBox(body, keyHx, keyHy, (kKeyZ - panZ) * 0.5f, kx, ky,
                     (kKeyZ + panZ) * 0.5f + kBackT - 0.002f);
            // beveled cap (smaller, proud of the base front) — the button relief
            localBox(body, keyHx * 0.78f, keyHy * 0.78f, 0.004f, kx, ky,
                     kKeyZ + 0.004f + kBackT);
        }
    }

    // STATUS LED housing (small nub at the screen's top-right corner).
    const float ledCx = scrW - 0.014f, ledCy = scrCy + scrH - 0.010f;
    localBox(body, 0.010f, 0.010f, 0.008f, ledCx, ledCy, kScreenZ + 0.004f + kBackT);

    // ---- SCREEN GLOW (separate emissive mesh so it blooms as live electronics). A thin
    // lit quad-box just proud of the screen housing + a tiny emissive LED cap. ----
    PrimMesh& glow = g.glow;
    mergeMesh(glow, x3::prims::makeBox(scrW - 0.006f, scrH - 0.006f, 0.003f,
                                       0.0f, scrCy, kScreenZ + 0.001f + kBackT, 1.0f));
    mergeMesh(glow, x3::prims::makeBox(0.008f, 0.008f, 0.003f,
                                       ledCx, ledCy, kScreenZ + 0.009f + kBackT, 1.0f));

    // Orient BOTH meshes into world space (rotate to the facing + offset to the mount).
    orientMesh(body, b, x, y, z);
    orientMesh(glow, b, x, y, z);
    return g;
}

} // namespace

KeypadHandles buildKeypad(Scene& scene, x3::rhi::IRenderDevice& device,
                          float x, float y, float z, KeypadFacing facing,
                          KeypadStatus status, uint32_t roomId) {
    KeypadGeom g = buildKeypadGeom(x, y, z, facing);

    const float bodyCol[4]  = { 0.38f, 0.41f, 0.46f, 1.0f };   // brushed gunmetal
    const float bodyEmit[4] = { 0, 0, 0, 0 };
    KeypadHandles h;
    h.body = addMeshEntity(scene, device, g.body, bodyCol, bodyEmit, roomId);

    float scol[4], semit[4];
    switch (status) {
        case KeypadStatus::Unlocked:
            scol[0]=0.15f; scol[1]=0.95f; scol[2]=0.35f; scol[3]=1.0f;
            semit[0]=0.12f; semit[1]=1.0f;  semit[2]=0.30f; semit[3]=2.2f; break;
        case KeypadStatus::Denied:                       // amber: valid code, sealed anyway
            scol[0]=0.95f; scol[1]=0.62f; scol[2]=0.12f; scol[3]=1.0f;
            semit[0]=1.34f; semit[1]=0.80f; semit[2]=0.22f; semit[3]=2.2f; break;
        case KeypadStatus::Locked:
        default:
            scol[0]=0.9f;  scol[1]=0.15f; scol[2]=0.12f; scol[3]=1.0f;
            semit[0]=1.0f; semit[1]=0.10f; semit[2]=0.08f; semit[3]=2.2f; break;
    }
    h.screen = addMeshEntity(scene, device, g.glow, scol, semit, roomId);
    return h;
}

void setKeypadStatus(Scene& scene, const KeypadHandles& kp, KeypadStatus status) {
    if (kp.screen == kNoLink || kp.screen >= scene.size()) return;
    Entity& e = scene.get(kp.screen);
    switch (status) {
        case KeypadStatus::Unlocked:
            e.baseColor[0]=0.15f; e.baseColor[1]=0.95f; e.baseColor[2]=0.35f; e.baseColor[3]=1.0f;
            e.emissive[0]=0.12f; e.emissive[1]=1.0f; e.emissive[2]=0.30f; e.emissive[3]=2.2f;
            break;
        case KeypadStatus::Denied:                       // amber: the lore-beat flash
            e.baseColor[0]=0.95f; e.baseColor[1]=0.62f; e.baseColor[2]=0.12f; e.baseColor[3]=1.0f;
            e.emissive[0]=1.34f; e.emissive[1]=0.80f; e.emissive[2]=0.22f; e.emissive[3]=2.2f;
            break;
        case KeypadStatus::Locked:
        default:
            e.baseColor[0]=0.9f; e.baseColor[1]=0.15f; e.baseColor[2]=0.12f; e.baseColor[3]=1.0f;
            e.emissive[0]=1.0f; e.emissive[1]=0.10f; e.emissive[2]=0.08f; e.emissive[3]=2.2f;
            break;
    }
}

bool runKeypadSelfTest() {
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char* what) {
        if (ok) { ++pass; x3::logInfo(std::string("  PASS ") + what); }
        else    { ++fail; x3::logInfo(std::string("  FAIL ") + what); }
    };
    x3::logInfo("running realistic KEYPAD geometry self-test (KP1-KP6)...");

    KeypadGeom g = buildKeypadGeom(10.0f, 1.4f, 5.0f, KeypadFacing::PlusZ);

    // KP1: NOT a flat quad — the body is a high-poly mesh with many triangles (back-
    // plate + 4 bezel rails + pan + screen housing + 12 keys (base+cap) + LED ~= 21
    // boxes * 12 tris = 252+ tris).
    const uint32_t bodyTris = (uint32_t)g.body.index.size() / 3;
    check(bodyTris >= 200, "KP1 keypad body is high-poly (>=200 tris, not a flat quad)");

    // KP2: real Z DEPTH front-to-back (a physical device, not a decal). The body spans
    // a meaningful range along the facing normal (here +Z).
    float zmin = 1e9f, zmax = -1e9f;
    for (const auto& v : g.body.verts) { zmin = std::min(zmin, v.pos[2]); zmax = std::max(zmax, v.pos[2]); }
    check((zmax - zmin) > 0.03f, "KP2 keypad has real front-to-back depth (>3 cm)");

    // KP3: the unit stands PROUD of its mount plane (z >= mount z), so no face is
    // coincident with the wall — no z-fighting against the wall surface.
    check(zmin >= 5.0f - 1e-3f, "KP3 keypad stands proud of the wall (no wall-coincident face)");

    // KP4: the screen GLOW mesh exists + is a thin emissive panel (the lit screen + LED).
    check(!g.glow.verts.empty() && (g.glow.index.size() / 3) >= 16,
          "KP4 screen-glow mesh present (screen + LED)");

    // KP5: orientation works on every facing — a -X facing keypad's depth runs along X.
    {
        KeypadGeom gx = buildKeypadGeom(0.0f, 1.4f, 0.0f, KeypadFacing::MinusX);
        float xmin = 1e9f, xmax = -1e9f;
        for (const auto& v : gx.body.verts) { xmin = std::min(xmin, v.pos[0]); xmax = std::max(xmax, v.pos[0]); }
        // front faces -X, so the device extends toward -X (xmin notably negative).
        check(xmin < -0.03f, "KP5 facing rotates the device onto the correct wall axis");
    }

    // KP6: all key positions are distinct (the 12-key grid is laid out, not stacked) —
    // sample by checking the body's X extent spans the grid width (~0.3 m).
    {
        float xmin = 1e9f, xmax = -1e9f;
        for (const auto& v : g.body.verts) { xmin = std::min(xmin, v.pos[0]); xmax = std::max(xmax, v.pos[0]); }
        check((xmax - xmin) > 0.25f, "KP6 key grid spans the panel width (3 columns laid out)");
    }

    // KP7: the stairwell SERVICE-VOID code (feat/secret-code-clues). 4545 is
    // ACCEPTED — as a denial: a numbered phantom door answers SERVICE VOID (green
    // -> amber, door sealed); the unnumbered 4.5-height door answers the sublevel
    // tell. The door itself opens in neither case (submitCode never unlocks).
    {
        using CR = FacilityStairwell::CodeResponse;
        check(FacilityStairwell::classifyCode(4545, false) == CR::ServiceVoid &&
              FacilityStairwell::classifyCode(4545, true)  == CR::SublevelTell,
              "KP7 service code 4545 accepted: SERVICE VOID on numbered doors, "
              "sublevel tell on the unnumbered door");
        // KP8 NEGATIVE CONTROL: 4544 (and the rest of the keyspace) stays a red
        // reject, and the phantom DoorSpec sentinel (-4545) can never match any
        // enterable 4-digit code — the slab is unopenable by keypad, period.
        bool sentinelSafe = (-FacilityStairwell::kServiceCode < 0);
        check(FacilityStairwell::classifyCode(4544, false) == CR::NotHandled &&
              FacilityStairwell::classifyCode(4544, true)  == CR::NotHandled &&
              FacilityStairwell::classifyCode(0, false)    == CR::NotHandled &&
              sentinelSafe,
              "KP8 negative control: 4544 red-rejects; door-code sentinel matches no entry");
        // KP9: the owner's MASTER BACKUP (7762). classifyCode passes it through
        // (NotHandled) — it is NOT a lore response; it reaches the door machinery,
        // where ONLY the unnumbered master door carries it (the lint gate asserts
        // the plan; the elevator FSM test asserts the cab lock). Distinct from the
        // service code, 4-digit, and matched by no other phantom slab (they carry
        // the negative sentinel).
        check(FacilityStairwell::kMasterCode == 7762 &&
              FacilityStairwell::kMasterCode != FacilityStairwell::kServiceCode &&
              FacilityStairwell::kMasterCode >= 1000 &&
              FacilityStairwell::kMasterCode <= 9999 &&
              FacilityStairwell::classifyCode(FacilityStairwell::kMasterCode, false) == CR::NotHandled &&
              FacilityStairwell::classifyCode(FacilityStairwell::kMasterCode, true)  == CR::NotHandled,
              "KP9 master backup 7762: passes through to the door machinery, "
              "distinct from the service code");
    }

    x3::logInfo("--test-keypad: " + std::to_string(pass) + " passed, " +
                std::to_string(fail) + " failed");
    return fail == 0;
}

} // namespace x3::game
