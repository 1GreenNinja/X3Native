// ============================================================================
// host_gallery — THE CHARACTER GALLERY (--world gallery).
//
// Tim's port of the Predator II "Character Arena" concept (the walkable art
// gallery of the cast from the Empires of Shadow web game): a MUSEUM-NIGHT
// hall — low ambient, one warm key spot per exhibit (fake-volumetric cone,
// the street-light tech), cool rim accents, emissive floor-marker rings, a
// black mirror floor and a HoloPanel "THE GALLERY" title — with every rigged
// character on a pedestal (the CyberWolf gets floor space), each looping its
// Idle under a floating name label. NO combat.
//
// DIRECTOR MODE (round 2 — "control and MOVE the characters"):
//   * Aim at an exhibit + E  = SELECT it (its marker ring burns gold; one at
//     a time). E at empty space, or Esc, deselects. Esc with no selection quits.
//   * F (while selected)     = cycle the selected exhibit's clips (Idle -> ...
//     -> Wave -> IdleAlt -> wrap). There is no clip-cycle without a selection —
//     E is the selector now. (Round 1 cycled on E; noclip moved F -> V.)
//   * G (hold, selected)     = the character WALKS toward the floor point you
//     aim at (gold target ring): straight line, Walk clip via the locomotion
//     feed (setPropMotion), Run beyond 8 m; stops + returns to its calm clip
//     (Idle by default) on release/arrival. No pathfinding, no physics AI.
//   * Q / R (hold, selected) = rotate the character in place.
//   * V = noclip flycam. WASD + mouse, Space jump, LeftShift sprint.
//
//   * Windowed:   --world gallery
//   * Screenshot: --world gallery --screenshot [--screenshot-path p] [--shot-cam ...]
//                 env X3_GALLERY_DIRECTOR_SHOT=1 stages the director-mode still
//                 (an exhibit selected + mid-walk toward an aim point, HUD hints).
//   * Self-test:  --test-gallery  (headless: cast builds, clips list, the cycle
//                 advances, restored clips present, the DIRECTOR move tick
//                 converges + hands back to the calm loop, aim->floor solves)
// ============================================================================
#include "world_host_common.h"
#include "../scene.h"
#include "../player.h"
#include "../monster.h"
#include "../mesh_prims.h"
#include "../surface_library.h"
#include "../asset_root.h"
#include "../holo_panel.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>

namespace x3 { namespace apphost {

namespace {

using x3::game::Scene;
using x3::game::Entity;
using x3::game::MonsterSystem;
using x3::game::MonsterType;

// ---- The cast --------------------------------------------------------------
// One row per character: GLB stem (prefers <stem>_anim.glb like defRigged),
// display name, modelScale (from the proven call sites: canon_aliens.cpp,
// club1127.cpp, monster.cpp roster), pedestal or floor.
struct CastRow {
    const char* stem;      // rigged_glb GLB stem (no extension)
    const char* label;     // floating name label
    float       scale;     // MonsterSystem::Tuning::modelScale
    bool        pedestal;  // true = museum pedestal; false = floor space
};

const CastRow kCast[] = {
    { "canon_grey",       "Grey",             0.75f, true  },
    { "canon_mantis",     "Mantis Arbiter",   1.05f, true  },
    { "canon_nordic",     "Nordic Steward",   1.05f, true  },
    { "canon_saurian",    "Saurian Soldier",  1.10f, true  },
    { "RexBouncer",       "Rex the Bouncer",  1.00f, true  },
    { "BossBreederQueen", "Breeder Queen",    1.55f, true  },
    { "Oracle",           "The Oracle",       1.00f, true  },
    { "AnnaBodySuit",     "Anna (Bodysuit)",  1.00f, true  },
    { "SalvariPrincess",  "Salvari Princess", 1.00f, true  },
    { "chief_martinez",   "Chief Martinez",   1.15f, true  },
    { "marcus_webb",      "Marcus Webb",      1.00f, true  },
    { "AnnaCasual",       "Anna (Casual)",    1.00f, true  },
    { "alien_crawler",    "CyberWolf",        1.00f, false },   // floor space, no pedestal
    // Jake_22_actions is EXCLUDED: his Mixamo rig bakes an armature-object Y
    // offset the monster draw path doesn't compensate (the 3P path carries an
    // empirical +1.03 floor-plant for it) — through this path he stands ~1 m
    // sunk. His 22 clips already live retargeted on chief/marcus (guard-death
    // wave), so the gallery loses nothing it can't show elsewhere.
};
constexpr int kCastCount = (int)(sizeof(kCast) / sizeof(kCast[0]));

// ---- Hall dimensions (museum-simple, FAST to load) -------------------------
constexpr float kHallHalfW = 6.0f;    // X half-width
constexpr float kHallHalfL = 21.0f;   // Z half-length
constexpr float kHallH     = 5.0f;    // ceiling height
constexpr float kRowX      = -3.6f;   // character row X (left side, facing the aisle)
constexpr float kRowZ0     = -16.9f;  // first character Z
constexpr float kRowStep   = 2.6f;    // spacing along Z
constexpr float kPedTop    = 0.18f;   // pedestal top Y

constexpr float kPi = 3.14159265f;

// ---- Director-mode movement (the locomotion-blend anchors: monster.cpp calls
// setLocomotionClips(idle, walk, run, 1.5, 4.0), so feeding EXACTLY these
// speeds lands the blend square on the Walk / Run clips — no half-blend slide).
constexpr float kWalkSpeed = 1.5f;   // m/s — the Walk blend anchor
constexpr float kRunSpeed  = 4.0f;   // m/s — the Run  blend anchor
constexpr float kArrive    = 0.30f;  // stop within this of the target (m)
constexpr float kRunBeyond = 8.0f;   // farther than this -> Run

// Live per-exhibit pose (director mode moves exhibits off their home spots).
struct ExhibitPose {
    x3::phys::Vec3 pos;
    float          yaw;
};

// One DIRECTOR move tick: straight-line toward `target` on the flat floor
// (planar only — the caller owns Y). Turns the pose to face the travel
// direction and returns the planar speed to feed the locomotion blend
// (kRun/kWalk anchors; 0 = arrived, feet planted). Pure — self-testable.
float directorMoveTick(ExhibitPose& p, const x3::phys::Vec3& target, float dt) {
    const float dx = target.x - p.pos.x, dz = target.z - p.pos.z;
    const float dist = std::sqrt(dx * dx + dz * dz);
    if (dist < kArrive) return 0.0f;
    const float speed = (dist > kRunBeyond) ? kRunSpeed : kWalkSpeed;
    const float step  = std::min(speed * dt, dist);
    p.pos.x += dx / dist * step;
    p.pos.z += dz / dist * step;
    p.yaw = std::atan2(dz, dx);   // model yaw 0 faces +X — atan2(z,x) matches
    return speed;
}

// Where the player's aim ray meets the gallery floor (y = 0), clamped inside
// the hall. False when aiming level/up (no floor point). Pure — self-testable.
bool aimFloorPoint(float ex, float ey, float ez, float yaw, float pitch,
                   x3::phys::Vec3& out) {
    const float fx = std::cos(pitch) * std::cos(yaw);
    const float fy = std::sin(pitch);
    const float fz = std::cos(pitch) * std::sin(yaw);
    if (fy > -0.02f) return false;
    const float t = -ey / fy;
    auto clampf = [](float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    };
    out.x = clampf(ex + fx * t, -kHallHalfW + 0.7f, kHallHalfW - 0.7f);
    out.y = 0.0f;
    out.z = clampf(ez + fz * t, -kHallHalfL + 0.7f, kHallHalfL - 0.7f);
    return true;
}

// Prefer <stem>_anim.glb over <stem>.glb (the defRigged idiom).
std::string pickGlb(const char* stem) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string anim = std::string(stem) + "_anim.glb";
    if (fs::exists(fs::path(x3::game::riggedGlbRoot()) / anim, ec)) return anim;
    return std::string(stem) + ".glb";
}

x3::phys::Vec3 castSpot(int i) {
    const bool ped = kCast[i].pedestal;
    return { kRowX, ped ? kPedTop : 0.0f, kRowZ0 + kRowStep * (float)i };
}

// The exhibit's floor height at `pos`: pedestal top while standing on its home
// pedestal, gallery floor everywhere else (director walks step it off/on).
float exhibitFloorY(int i, const x3::phys::Vec3& pos) {
    if (!kCast[i].pedestal) return 0.0f;
    const x3::phys::Vec3 home = castSpot(i);
    const float dx = pos.x - home.x, dz = pos.z - home.z;
    return (dx * dx + dz * dz < 1.0f * 1.0f) ? kPedTop : 0.0f;
}

// Spawn one cast member as an INERT prop (the Club 1127 recipe: chaseSpeed 0 /
// damage 0 — the AI never moves it; the gallery owns pose + clip).
std::unique_ptr<MonsterSystem> spawnCastMember(Scene& scene, x3::rhi::IRenderDevice& device,
                                               x3::phys::IPhysicsWorld& physics, int i) {
    auto sys = std::make_unique<MonsterSystem>();
    MonsterSystem::Tuning t;
    t.type             = MonsterType::Guard;
    t.hp               = 100;
    t.chaseSpeed       = 0.0f;   // inert exhibit: never moves itself
    t.damage           = 0;      // never attacks
    t.ranged           = false;
    t.modelFile        = pickGlb(kCast[i].stem);
    t.modelDirOverride = x3::game::riggedGlbRoot();
    t.standUpZtoY      = false;  // rigged sources are authored Y-up
    t.modelScale       = kCast[i].scale;
    sys->buildMonsterTuned(scene, device, physics, x3::game::riggedGlbRoot(), castSpot(i), t);
    return sys;
}

// Exact-name clip lookup (findClip is fuzzy substring: it can't tell "Idle"
// from "IdleAlt"). Case-insensitive full-string compare.
int exactClip(const x3::anim::Skinner& sk, const char* name) {
    auto lower = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    const std::string want = lower(name);
    for (uint32_t c = 0; c < sk.clipCount(); ++c)
        if (lower(std::string(sk.clipName(c))) == want) return (int)c;
    return -1;
}

// The clip-cycle step: next index, wrapping. Pure (self-testable).
int nextClip(int cur, int clipCount) {
    if (clipCount <= 0) return -1;
    return (cur + 1) % clipCount;
}

// Start every exhibit on its exact Idle (else clip 0).
void startIdle(MonsterSystem& m) {
    if (!m.skinnable()) return;
    int idle = exactClip(m.skinner(), "Idle");
    if (idle < 0 && m.skinner().clipCount() > 0) idle = 0;
    if (idle >= 0) m.setCalmLoopClip(idle);
}

// Two-pass (shadow + ink) centered HUD text.
void hudTextCentered(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                     const char* s, float cx, float y, float px, const float rgba[4]) {
    const float w = device.textAdvance(x3::rhi::FontRole::Menu, s, px);
    const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.8f * rgba[3] };
    device.drawHudTextF(frame, x3::rhi::FontRole::Menu, s, cx - w * 0.5f + 1.5f, y + 1.5f, px, shadow);
    device.drawHudTextF(frame, x3::rhi::FontRole::Menu, s, cx - w * 0.5f,        y,        px, rgba);
}

// The character's current clip name for the label ("Idle", "Wave", ...).
std::string currentClipName(const MonsterSystem& m) {
    if (!m.skinnable()) return "static";
    const int c = m.calmLoopClip();
    if (c < 0 || (uint32_t)c >= m.skinner().clipCount()) return "Idle";
    return std::string(m.skinner().clipName((uint32_t)c));
}

// Draw every exhibit's floating label + return the aimed character index (or
// -1). Director pick: within 12 m of the eye AND within ~28 deg of the aim
// (selection works across the hall now, so the cone is tighter than round 1).
// `sel` renders gold (the selected exhibit's label matches its ring).
int drawLabelsAndPick(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                      const std::vector<std::unique_ptr<MonsterSystem>>& cast,
                      const std::vector<ExhibitPose>& pose, int sel,
                      float ex, float ey, float ez, float yaw, float pitch) {
    const float fx = std::cos(pitch) * std::cos(yaw);
    const float fy = std::sin(pitch);
    const float fz = std::cos(pitch) * std::sin(yaw);
    int   best = -1;
    float bestDot = 0.88f;   // ~28 deg cone
    for (int i = 0; i < (int)cast.size(); ++i) {
        if (!cast[i]) continue;
        const x3::phys::Vec3& p = pose[i].pos;
        const float hx = p.x - ex, hy = (p.y + 1.0f) - ey, hz = p.z - ez;
        const float d = std::sqrt(hx * hx + hy * hy + hz * hz);
        if (d < 12.0f && d > 0.01f) {
            const float dot = (hx * fx + hy * fy + hz * fz) / d;
            if (dot > bestDot) { bestDot = dot; best = i; }
        }
    }
    for (int i = 0; i < (int)cast.size(); ++i) {
        if (!cast[i]) continue;
        const x3::phys::Vec3& p = pose[i].pos;
        const float dx = p.x - ex, dz = p.z - ez;
        if (dx * dx + dz * dz > 12.0f * 12.0f) continue;      // label range gate
        const float headY = p.y + 2.1f * kCast[i].scale + 0.25f;
        float sx, sy;
        if (!device.worldToScreen(p.x, headY, p.z, sx, sy)) continue;
        const bool isSel = (i == sel);
        const bool aim   = (i == best);
        const float name[4] = { isSel ? 1.00f : (aim ? 0.95f : 0.85f),
                                isSel ? 0.82f : (aim ? 0.95f : 0.88f),
                                isSel ? 0.35f : (aim ? 0.55f : 0.95f), 1.0f };
        const float clip[4] = { 0.55f, 0.85f, 1.0f, (isSel || aim) ? 1.0f : 0.85f };
        hudTextCentered(device, frame, kCast[i].label, sx, sy - 22.0f, 17.0f, name);
        hudTextCentered(device, frame, currentClipName(*cast[i]).c_str(), sx, sy, 14.0f, clip);
    }
    return best;
}

// The director HUD (shared windowed/headless-still): selection status + the
// control hints, bottom-center.
void drawDirectorHud(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                     const std::vector<std::unique_ptr<MonsterSystem>>& cast,
                     int sel, int aimed) {
    uint32_t hw = 0, hh = 0; device.hudSize(hw, hh);
    const float cx = (hw > 0) ? hw * 0.5f : 640.0f;
    const float hy = (hh > 0) ? (float)hh : 720.0f;
    if (sel >= 0) {
        // ASCII only: the HUD Menu font has no em-dash glyph (renders "???").
        const std::string head = std::string(kCast[sel].label) + " - " +
                                 currentClipName(*cast[sel]);
        const float gold[4] = { 1.0f, 0.84f, 0.40f, 1.0f };
        const float hint[4] = { 0.72f, 0.86f, 1.0f, 0.95f };
        hudTextCentered(device, frame, head.c_str(), cx, hy * 0.76f, 20.0f, gold);
        hudTextCentered(device, frame,
                        "F: clip  G: move-to-aim  Q/R: rotate  E/Esc: deselect",
                        cx, hy * 0.80f, 16.0f, hint);
    } else if (aimed >= 0) {
        const std::string hint = std::string("[E] select - ") + kCast[aimed].label +
                                 ": " + currentClipName(*cast[aimed]);
        const float col[4] = { 0.62f, 0.92f, 1.0f, 1.0f };
        hudTextCentered(device, frame, hint.c_str(), cx, hy * 0.78f, 20.0f, col);
    }
}

// ---- Museum-night render kit ------------------------------------------------

float smoothstepf(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

// glTF-packed 1x1 metallic-roughness texel (G = roughness, B = metallic).
std::vector<uint8_t> makeMr1x1(uint8_t rough, uint8_t metal) {
    return { 255, rough, metal, 255 };
}

// Column-major translation-only transform.
void setTranslation(float* m16, float x, float y, float z) {
    for (int k = 0; k < 16; ++k) m16[k] = (k % 5 == 0) ? 1.0f : 0.0f;
    m16[12] = x; m16[13] = y; m16[14] = z;
}

// KEY-SPOT CONE, world-baked (the street-light doctrine: a scaled unit cone
// skews normals through the plain-mat3 path and kills the rim fade — bake the
// real dimensions, translate only). Apex at the origin opening DOWN `drop`
// to base radius `radius`; the axis DRIFTS `offX` in X over the drop so the
// housing can sit above/FRONT of the exhibit and throw the light back onto
// it (a museum key, not a street lamp). Open both ends; UV v rides the axial
// gradient. Profile + normals follow street_lights.cpp's makeConeMesh.
void makeKeyConeMesh(float radius, float drop, float offX,
                     std::vector<x3::rhi::MeshVertex>& verts, std::vector<uint32_t>& idx) {
    const int kRings = 9, kSegs = 24;
    verts.clear(); idx.clear();
    for (int ri = 0; ri < kRings; ++ri) {
        const float t = (float)ri / (float)(kRings - 1);
        const float r = radius * (0.10f + 0.90f * std::pow(t, 1.35f));
        const float drdy = (t > 0.0f)
            ? radius * 0.90f * 1.35f * std::pow(t, 0.35f) / drop : 0.0f;
        const float nrm = 1.0f / std::sqrt(1.0f + drdy * drdy);
        for (int si = 0; si <= kSegs; ++si) {
            const float a = (float)si / (float)kSegs * 2.0f * kPi;
            const float ca = std::cos(a), sa = std::sin(a);
            x3::rhi::MeshVertex v{};
            v.pos[0] = ca * r + offX * t; v.pos[1] = -t * drop; v.pos[2] = sa * r;
            v.normal[0] = ca * nrm; v.normal[1] = drdy * nrm; v.normal[2] = sa * nrm;
            v.uv[0] = (float)si / (float)kSegs; v.uv[1] = t;
            verts.push_back(v);
        }
    }
    const int stride = kSegs + 1;
    for (int ri = 0; ri + 1 < kRings; ++ri)
        for (int si = 0; si < kSegs; ++si) {
            const uint32_t a = (uint32_t)(ri * stride + si), b = a + 1;
            const uint32_t c = a + (uint32_t)stride, d = c + 1;
            idx.insert(idx.end(), { a, c, b,  b, c, d });
        }
}

// Axial cone gradient (v=0 bright at the apex -> dissolves before the base;
// row-flipped like street_lights' bake). LINEAR (srgb=false).
x3::rhi::TextureHandle makeConeGradient(x3::rhi::IRenderDevice& device) {
    const int W = 8, H = 64;
    std::vector<uint8_t> px(W * H * 4);
    for (int y = 0; y < H; ++y) {
        const float v = (float)(H - 1 - y) / (float)(H - 1);
        const float f = std::pow(1.0f - smoothstepf(0.0f, 0.80f, v), 2.2f);
        const uint8_t b = (uint8_t)std::lround(255.0f * f);
        for (int x = 0; x < W; ++x) {
            uint8_t* p = &px[(y * W + x) * 4];
            p[0] = p[1] = p[2] = b; p[3] = 255;
        }
    }
    return device.createTexture(px.data(), W, H, false);
}

// ---- Selection / marker-ring inks -------------------------------------------
constexpr float kRingIdle[4] = { 0.20f, 0.55f, 1.10f, 1.30f };   // canon BLUE, calm
constexpr float kRingSel[4]  = { 1.15f, 0.80f, 0.28f, 3.60f };   // gold — SELECTED

} // namespace

int hostGallery(HostContext& hc) {
    if (hc.worldMode != "gallery") return -1;
    auto* device = hc.device;
    GLFWwindow* window = hc.window;

    x3::logInfo("--world gallery: THE CHARACTER GALLERY — museum night + director mode");

    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    if (!phys->init()) {
        x3::logError("--world gallery: physics init failed");
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    Scene scene;

    // ---- The hall (museum NIGHT): black mirror floor, dark paneling with
    // emissive trim lines, dark ceiling, dark-metal pedestals. -----------------
    x3::game::SurfaceLibrary surf;
    surf.mount(x3::game::assetRoot() + "/surface_library");
    const x3::game::SurfaceSet& sWall = surf.get(*device, "mw_concrete_panels_a");
    const x3::game::SurfaceSet& sCeil = surf.get(*device, "hh_ceiling_01a");
    const x3::game::SurfaceSet& sPed  = surf.get(*device, "mw_metal_trim_b");

    // Shared 1x1 MR texels: the polished-black floor + brushed pedestal tops.
    auto mrFloorPx = makeMr1x1(/*rough*/ 58, /*metal*/ 210);   // polished black
    auto mrTrimPx  = makeMr1x1(/*rough*/ 90, /*metal*/ 200);   // brushed dark metal
    const x3::rhi::TextureHandle mrFloor = device->createTexture(mrFloorPx.data(), 1, 1, false);
    const x3::rhi::TextureHandle mrTrim  = device->createTexture(mrTrimPx.data(), 1, 1, false);

    auto addBox = [&](float cx, float cy, float cz, float hx, float hy, float hz,
                      const float col[4], const x3::game::SurfaceSet* sf,
                      bool collide) -> uint32_t {
        x3::prims::PrimMesh geo = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, 0.5f);
        Entity e;
        e.mesh = device->createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                    geo.index.data(), (uint32_t)geo.index.size());
        if (sf && sf->ok) { e.tex = sf->albedo; e.normalTex = sf->normal; e.mrTex = sf->mr; }
        for (int k = 0; k < 4; ++k) e.baseColor[k] = col[k];
        if (sf && sf->ok) {
            const float vt = sf->valueTint();
            for (int k = 0; k < 3; ++k) e.baseColor[k] *= vt;
        }
        if (collide)
            e.body = phys->addStaticMesh(geo.cverts.data(), (uint32_t)(geo.cverts.size() / 3),
                                         geo.cindex.data(), (uint32_t)geo.cindex.size());
        return scene.add(e);
    };

    // MUSEUM-NIGHT PALETTE: the hall is a dark stage; the exhibits carry the light.
    const float cFloor[4] = { 0.052f, 0.054f, 0.068f, 1.0f };   // black mirror
    const float cWall[4]  = { 0.17f, 0.175f, 0.21f, 1.0f };     // dark paneling
    const float cCeil[4]  = { 0.10f, 0.10f, 0.13f, 1.0f };      // dark lid
    const float cPed[4]   = { 0.30f, 0.30f, 0.36f, 1.0f };      // dark-metal plinth
    // Floor (top at y = 0): the BLACK MIRROR (club1127's tile recipe as one slab
    // — dark base + chrome-class MR so the key spots streak across it; the RT
    // tier reflects the exhibits for real, raster gets tight glossy hotspots).
    {
        const uint32_t fid = addBox(0.0f, -0.15f, 0.0f, kHallHalfW, 0.15f, kHallHalfL,
                                    cFloor, nullptr, true);
        scene.get(fid).mrTex = mrFloor;
    }
    addBox(0.0f, kHallH + 0.15f, 0.0f, kHallHalfW, 0.15f, kHallHalfL, cCeil, &sCeil, false);
    addBox(-kHallHalfW - 0.15f, kHallH * 0.5f, 0.0f, 0.15f, kHallH * 0.5f, kHallHalfL, cWall, &sWall, true);
    addBox( kHallHalfW + 0.15f, kHallH * 0.5f, 0.0f, 0.15f, kHallH * 0.5f, kHallHalfL, cWall, &sWall, true);
    addBox(0.0f, kHallH * 0.5f, -kHallHalfL - 0.15f, kHallHalfW, kHallH * 0.5f, 0.15f, cWall, &sWall, true);
    addBox(0.0f, kHallH * 0.5f,  kHallHalfL + 0.15f, kHallHalfW, kHallH * 0.5f, 0.15f, cWall, &sWall, true);
    // Emissive TRIM LINES: a low skirting line + a high rail down both long
    // walls and across both end walls (canon BLUE — flat emissive strips).
    {
        const float cTrim[4] = { 0.02f, 0.02f, 0.03f, 1.0f };
        const float emTrim[4] = { 0.24f, 0.52f, 1.30f, 0.92f };
        auto trim = [&](float cx, float cy, float cz, float hx, float hz) {
            const uint32_t id = addBox(cx, cy, cz, hx, 0.02f, hz, cTrim, nullptr, false);
            Entity& e = scene.get(id);
            for (int k = 0; k < 4; ++k) e.emissive[k] = emTrim[k];
        };
        for (float y : { 0.55f, 3.95f }) {
            trim(-kHallHalfW + 0.03f, y, 0.0f, 0.025f, kHallHalfL - 0.05f);
            trim( kHallHalfW - 0.03f, y, 0.0f, 0.025f, kHallHalfL - 0.05f);
            trim(0.0f, y, -kHallHalfL + 0.03f, kHallHalfW - 0.05f, 0.025f);
            trim(0.0f, y,  kHallHalfL - 0.03f, kHallHalfW - 0.05f, 0.025f);
        }
    }
    // Pedestals: dark metal plinths (brushed MR so the keys give them an edge).
    for (int i = 0; i < kCastCount; ++i) {
        if (!kCast[i].pedestal) continue;
        const x3::phys::Vec3 p = castSpot(i);
        const uint32_t pid = addBox(p.x, kPedTop * 0.5f, p.z, 0.85f, kPedTop * 0.5f, 0.85f,
                                    cPed, &sPed, true);
        scene.get(pid).mrTex = mrTrim;
    }

    // ---- Floor-marker RINGS: one emissive ring per exhibit (the selection
    // highlight burns the selected one gold) + the director move-target ring. --
    x3::rhi::MeshHandle ringMesh, targetRingMesh;
    {
        x3::prims::PrimMesh rg = x3::prims::makeRing(0.96f, 1.14f, 96);
        ringMesh = device->createMesh(rg.verts.data(), (uint32_t)rg.verts.size(),
                                      rg.index.data(), (uint32_t)rg.index.size());
        x3::prims::PrimMesh tg = x3::prims::makeRing(0.24f, 0.36f, 64);
        targetRingMesh = device->createMesh(tg.verts.data(), (uint32_t)tg.verts.size(),
                                            tg.index.data(), (uint32_t)tg.index.size());
    }
    std::vector<uint32_t> ringEnt(kCastCount);
    for (int i = 0; i < kCastCount; ++i) {
        const x3::phys::Vec3 p = castSpot(i);
        Entity e;
        e.mesh = ringMesh;
        setTranslation(e.transform, p.x, 0.015f, p.z);
        e.baseColor[0] = e.baseColor[1] = e.baseColor[2] = 0.02f;
        for (int k = 0; k < 4; ++k) e.emissive[k] = kRingIdle[k];
        ringEnt[i] = scene.add(e);
    }
    uint32_t targetRingId;
    {
        Entity e;
        e.mesh = targetRingMesh;
        setTranslation(e.transform, 0.0f, -2.0f, 0.0f);   // parked under the floor
        e.baseColor[0] = e.baseColor[1] = e.baseColor[2] = 0.02f;
        e.emissive[0] = 1.15f; e.emissive[1] = 0.80f; e.emissive[2] = 0.28f; e.emissive[3] = 3.2f;
        targetRingId = scene.add(e);
    }

    // ---- KEY SPOT RIG per exhibit: dark housing on the ceiling above/FRONT,
    // a fake-volumetric additive cone drifting back onto the character (the
    // street-light cone tech, angled), + the pooled warm key light. -----------
    x3::rhi::MeshHandle keyConeMesh;
    x3::rhi::TextureHandle coneGrad = makeConeGradient(*device);
    {
        std::vector<x3::rhi::MeshVertex> cv; std::vector<uint32_t> ci;
        // Apex under the housing at y=4.55, base washing the exhibit: drop to
        // just above pedestal-top, drifting -0.85 in X back over the character.
        makeKeyConeMesh(/*radius*/ 1.05f, /*drop*/ 4.30f, /*offX*/ -0.85f, cv, ci);
        keyConeMesh = device->createMesh(cv.data(), (uint32_t)cv.size(),
                                         ci.data(), (uint32_t)ci.size());
    }
    for (int i = 0; i < kCastCount; ++i) {
        const float z = castSpot(i).z;
        const float hx = kRowX + 0.85f;
        // Housing: small dark can on the ceiling with a warm emissive throat.
        {
            const float cHouse[4] = { 0.06f, 0.06f, 0.07f, 1.0f };
            const uint32_t id = addBox(hx, 4.72f, z, 0.14f, 0.10f, 0.14f, cHouse, nullptr, false);
            Entity& e = scene.get(id);
            e.emissive[0] = 1.0f; e.emissive[1] = 0.80f; e.emissive[2] = 0.55f; e.emissive[3] = 1.9f;
        }
        // The cone (additive glass — a whisper; the pooled light does the work).
        {
            Entity e;
            e.mesh = keyConeMesh;
            e.tex  = coneGrad;
            setTranslation(e.transform, hx, 4.55f, z);
            e.baseColor[3] = 1.0f;
            e.emissive[0] = 1.0f; e.emissive[1] = 0.78f; e.emissive[2] = 0.52f; e.emissive[3] = 0.18f;
            e.transparent = true;
            e.glass.opacity = 0.0f; e.glass.refraction = 0.0f;
            e.glass.roughness = 0.0f; e.glass.specular = 0.0f;
            e.glass.additive = 3.5f;   // soft silhouette rim fade
            scene.add(e);
        }
    }

    // ---- "THE GALLERY" title: a HoloPanel (the ONE holo platform) on the far
    // end wall — the vista terminus of the money shot. ------------------------
    x3::game::HoloPanel titlePanel;
    {
        x3::game::HoloPanelParams tp;
        tp.pos    = { 0.0f, 3.15f, kHallHalfL - 0.14f };
        tp.yaw    = kPi;    // back-box to the +Z wall, pane into the hall
        tp.width  = 9.0f; tp.height = 2.3f;
        tp.mount  = x3::game::HoloMount::WallFlush;
        tp.frame  = x3::game::HoloFrame::Pipe;
        tp.emissiveStrength = 3.6f;
        tp.shimmerIntensity = 0.5f;
        tp.glowLight = true;
        tp.glowRange = 6.5f;
        const float aspect = tp.width / tp.height;
        tp.contentBake = [aspect](uint32_t n) {
            namespace h = x3::game::holo;
            h::Canvas c(n);
            const float xs = 1.0f / aspect;      // cancel the panel stretch
            const std::string title = "THE GALLERY";
            if (h::fontReady()) {
                const float px = n * 0.46f;
                const float w  = h::textWidth(title, px, xs);
                h::drawText(c, title, (n - w) * 0.5f, n * 0.27f, px, h::kBlue, 1.0f, xs);
            } else {
                h::rectFrame(c, n * 0.2f, n * 0.35f, n * 0.8f, n * 0.65f, h::kBlueHi, 1.0f, 3.0f);
            }
            h::line(c, n * 0.06f, n * 0.22f, n * 0.94f, n * 0.22f, h::kBlue, 0.8f, 2.0f);
            h::line(c, n * 0.06f, n * 0.80f, n * 0.94f, n * 0.80f, h::kBlue, 0.8f, 2.0f);
            // MIRROR the bake horizontally: at yaw=pi the pane's FRONT fan shows
            // the texture through its own back (empirical: v3 shot read
            // "YRELLAG EHT"), so pre-flip it here and it lands legible.
            std::vector<uint8_t> px = h::finish(c);
            for (uint32_t y = 0; y < n; ++y)
                for (uint32_t x = 0; x < n / 2; ++x)
                    for (int k = 0; k < 4; ++k)
                        std::swap(px[(y * n + x) * 4 + k],
                                  px[(y * n + (n - 1 - x)) * 4 + k]);
            return px;
        };
        titlePanel.build(scene, *device, tp);
    }

    // ---- LIGHTING: museum night. LOW cool ambient; 13 warm per-exhibit keys;
    // 4 cool rims behind the row; 1 faint aisle fill; the title's glow pool.
    // 19 pooled lights — comfortably inside the 64-light budget (round 1's
    // two ceiling wash rows are GONE: the even wash was the dev-room tell). ----
    { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false; device->setSkyParams(sp); }
    { x3::rhi::IRenderDevice::SsaoParams ao{}; ao.enabled = false; device->setSsaoParams(ao); }
    { x3::rhi::IRenderDevice::GiParams gi{}; gi.enabled = false; device->setGiParams(gi); }
    device->setAmbient(0.045f, 0.050f, 0.068f);
    {
        // Pin auto-exposure near unity: the AE would otherwise adapt the dark
        // hall back up toward a dev-room wash (aeMax 2.2 default) and crush the
        // museum-night grade. Keys are tuned to expose the CHARACTERS at ~1.0.
        x3::rhi::IRenderDevice::PostFXParams fx{};
        fx.aeMin = 0.85f; fx.aeMax = 1.30f;
        device->setPostFX(fx);
    }
    {
        std::vector<x3::rhi::PointLight> pl;
        for (int i = 0; i < kCastCount; ++i) {           // warm KEY per exhibit
            x3::rhi::PointLight l{};
            l.pos[0] = kRowX + 1.05f; l.pos[1] = 3.15f; l.pos[2] = castSpot(i).z;
            l.range = 5.2f;
            l.color[0] = 2.45f; l.color[1] = 1.95f; l.color[2] = 1.38f;
            pl.push_back(l);
        }
        for (float z : { -13.0f, -5.2f, 2.6f, 10.4f }) { // cool RIM accents behind the row
            x3::rhi::PointLight l{};
            l.pos[0] = kRowX - 1.7f; l.pos[1] = 2.9f; l.pos[2] = z;
            l.range = 7.0f;
            l.color[0] = 0.30f; l.color[1] = 0.52f; l.color[2] = 1.05f;
            pl.push_back(l);
        }
        {                                                // faint warm aisle fill
            x3::rhi::PointLight l{};
            l.pos[0] = 1.6f; l.pos[1] = 2.4f; l.pos[2] = -2.0f;
            l.range = 13.0f;
            l.color[0] = 0.38f; l.color[1] = 0.35f; l.color[2] = 0.30f;
            pl.push_back(l);
        }
        if (titlePanel.hasGlowLight()) {                 // the title's blue pool
            x3::rhi::PointLight l{};
            const float* gp = titlePanel.glowLightPos();
            const float* gc = titlePanel.glowLightColor();
            l.pos[0] = gp[0]; l.pos[1] = gp[1]; l.pos[2] = gp[2];
            l.range = titlePanel.glowLightRange();
            l.color[0] = gc[0]; l.color[1] = gc[1]; l.color[2] = gc[2];
            pl.push_back(l);
        }
        device->setPointLights(pl.data(), (uint32_t)pl.size());
    }

    // ---- The cast. -----------------------------------------------------------
    std::vector<std::unique_ptr<MonsterSystem>> cast;
    cast.reserve(kCastCount);
    for (int i = 0; i < kCastCount; ++i) {
        cast.push_back(spawnCastMember(scene, *device, *phys, i));
        startIdle(*cast.back());
        if (!cast.back()->usingRealModel())
            x3::logError(std::string("--world gallery: fallback box for ") + kCast[i].stem +
                         " (GLB missing? run: python tools/asset_store.py fetch --all)");
    }

    // Live director state: per-exhibit pose + this frame's locomotion feed.
    const float kFaceYaw = 0.0f;   // exhibits face +X (the aisle)
    std::vector<ExhibitPose> pose(kCastCount);
    std::vector<float>       feed(kCastCount, 0.0f);
    for (int i = 0; i < kCastCount; ++i) pose[i] = { castSpot(i), kFaceYaw };

    auto tickCast = [&](float dt, const x3::phys::Vec3& viewer) {
        for (int i = 0; i < kCastCount; ++i) {
            // Y follows the local floor (steps off/on the home pedestal smoothly).
            const float yWant = exhibitFloorY(i, pose[i].pos);
            const float dy = yWant - pose[i].pos.y;
            const float step = 1.2f * dt;
            pose[i].pos.y += (std::fabs(dy) < step) ? dy : (dy > 0 ? step : -step);
            cast[i]->setPropPose(pose[i].pos, pose[i].yaw);
            cast[i]->setPropMotion(feed[i], 0.0f);
            cast[i]->update(dt, scene, *phys, viewer);
        }
    };
    auto drawCast = [&](const x3::rhi::FrameContext& frame) {
        for (const auto& c : cast) c->drawMonster(*device, frame, scene);
    };
    // Selection ring feedback: the selected exhibit's ring burns gold AND
    // follows the character; everyone else's ring waits at its home spot.
    auto updateRings = [&](int sel) {
        for (int i = 0; i < kCastCount; ++i) {
            Entity& e = scene.get(ringEnt[i]);
            const bool isSel = (i == sel);
            const float* ink = isSel ? kRingSel : kRingIdle;
            for (int k = 0; k < 4; ++k) e.emissive[k] = ink[k];
            // The selected ring FOLLOWS its character; the rest mark the homes.
            const float x = isSel ? pose[i].pos.x : castSpot(i).x;
            const float z = isSel ? pose[i].pos.z : castSpot(i).z;
            setTranslation(e.transform, x, 0.015f, z);
        }
    };
    auto placeTargetRing = [&](bool show, const x3::phys::Vec3& t) {
        Entity& e = scene.get(targetRingId);
        if (show) setTranslation(e.transform, t.x, 0.02f, t.z);
        else      setTranslation(e.transform, 0.0f, -2.0f, 0.0f);
    };

    // ---- Headless screenshot path. ------------------------------------------
    if (hc.headless) {
        int   sel = -1;
        bool  directorShot = std::getenv("X3_GALLERY_DIRECTOR_SHOT") != nullptr;
        x3::phys::Vec3 moveTarget{ 1.8f, 0.0f, 2.2f };
        if (directorShot) {
            // Stage DIRECTOR MODE for the still: Chief Martinez selected and
            // mid-WALK toward an aisle aim point (ring gold, target ring down,
            // HUD hints up) — exactly the windowed G-hold, driven headless.
            sel = 9;   // chief_martinez (carries the retargeted Walk/Run set)
        } else {
            // Showcase the RESTORED test clips in the still: Nordic mid-Wave
            // and the Oracle on her theatrical IdleAlt.
            if (cast[2]->skinnable()) {
                const int w = exactClip(cast[2]->skinner(), "Wave");
                if (w >= 0) cast[2]->setCalmLoopClip(w);
            }
            if (cast[6]->skinnable()) {
                const int a = exactClip(cast[6]->skinner(), "IdleAlt");
                if (a >= 0) cast[6]->setCalmLoopClip(a);
            }
        }
        // Default showcase vantage: from the entrance, straight down the hall —
        // the exhibit row recedes left, the title panel is the vista terminus.
        float cam[5] = { 1.2f, 1.85f, -17.5f, 1.64f, -0.03f };
        // Over-the-shoulder from behind the walking chief: his stride and the
        // gold target ring line up ahead — an unambiguous "walks to the aim".
        if (directorShot) { cam[0] = -4.6f; cam[1] = 2.4f; cam[2] = 8.2f; cam[3] = -0.80f; cam[4] = -0.20f; }
        if (hc.shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = hc.shotCam[k];
        const std::string outPath = hc.screenshot ? hc.screenshotPath
                                                  : std::string("captures/gallery.png");
        const float dt = 1.0f / 60.0f;
        // Director still: fewer settle frames so the chief is caught mid-STEP
        // just off his pedestal, still inside his key cone (not lost in the
        // dark aisle a longer walk would reach).
        const int kSettle = directorShot ? 55 : 90;
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            if (directorShot && sel >= 0)
                feed[sel] = directorMoveTick(pose[sel], moveTarget, dt);
            tickCast(dt, { cam[0], cam[1], cam[2] });
            updateRings(sel);
            placeTargetRing(directorShot, moveTarget);
            titlePanel.update(dt);
            phys->step(dt);
            scene.update(*phys);
            device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 60.0f);
            if (i == kSettle - 1) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                scene.render(*device, frame);
                drawCast(frame);
                const int aimed = drawLabelsAndPick(*device, frame, cast, pose, sel,
                                                    cam[0], cam[1], cam[2], cam[3], cam[4]);
                drawDirectorHud(*device, frame, cast, sel, aimed);
            }
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(outPath.c_str());
        if (wrote) x3::logInfo("--world gallery: wrote screenshot " + outPath);
        else       x3::logError("--world gallery: capture FAILED");
        titlePanel.shutdown(*device);
        phys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ---- Walkable windowed path. --------------------------------------------
    x3::game::Player player;
    player.spawn(*phys, 3.5f, 0.2f, -19.0f);
    player.setLook(1.9f, -0.02f);   // face down the exhibit row

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
    double prevTime = glfwGetTime();
    bool prevSpace = false, prevV = false, prevE = false, prevF = false, prevEsc = false;
    bool noclip = false;
    int  sel = -1;          // the selected exhibit (director mode), -1 = none
    int  aimedLast = -1;    // last frame's aim pick (input runs before the draw)
    float flyX = 3.5f, flyY = 1.8f, flyZ = -19.0f, flyYaw = 1.9f, flyPitch = -0.02f;
    x3::logInfo("--world gallery: WASD + mouse. E select exhibit, F cycle clip, "
                "G move-to-aim, Q/R rotate, Esc deselect/quit, V noclip");
    int lastW = (int)hc.W, lastH = (int)hc.H;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        double now = glfwGetTime();
        float dt = (float)(now - prevTime); prevTime = now;
        if (dt > 0.1f) dt = 0.1f;

        double mx, my; glfwGetCursorPos(window, &mx, &my);
        float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
        lastMX = mx; lastMY = my;

        auto kd = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
        // Esc: deselect first; quit only with nothing selected.
        const bool escNow = kd(GLFW_KEY_ESCAPE);
        if (escNow && !prevEsc) {
            if (sel >= 0) sel = -1;
            else break;
        }
        prevEsc = escNow;

        bool spaceNow = kd(GLFW_KEY_SPACE);
        bool vNow = kd(GLFW_KEY_V);
        if (vNow && !prevV) {
            noclip = !noclip;
            if (noclip) { float yy, pp; player.camera(flyX, flyY, flyZ, yy, pp); flyYaw = yy; flyPitch = pp; }
        }
        prevV = vNow;

        float camX, camY, camZ, camYaw, camPitch;
        if (!noclip) {
            x3::game::PlayerInput in;
            if (kd(GLFW_KEY_W)) in.moveFwd    += 1.0f;
            if (kd(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
            if (kd(GLFW_KEY_D)) in.moveStrafe += 1.0f;
            if (kd(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
            in.sprint      = kd(GLFW_KEY_LEFT_SHIFT);
            in.jumpPressed = spaceNow && !prevSpace;
            in.lookDX = ddx; in.lookDY = ddy;
            player.update(in, dt, *phys);
            player.camera(camX, camY, camZ, camYaw, camPitch);
        } else {
            const float sens = 0.0025f;
            flyYaw += ddx * sens; flyPitch -= ddy * sens;
            if (flyPitch >  1.55f) flyPitch =  1.55f;
            if (flyPitch < -1.55f) flyPitch = -1.55f;
            float fx = std::cos(flyPitch) * std::cos(flyYaw);
            float fy = std::sin(flyPitch);
            float fz = std::cos(flyPitch) * std::sin(flyYaw);
            float rl = std::sqrt(fx * fx + fz * fz); if (rl < 1e-4f) rl = 1e-4f;
            float rx = -fz / rl, rz = fx / rl;
            float spd = 6.0f * dt; if (kd(GLFW_KEY_LEFT_SHIFT)) spd *= 3.0f;
            if (kd(GLFW_KEY_W)) { flyX += fx * spd; flyY += fy * spd; flyZ += fz * spd; }
            if (kd(GLFW_KEY_S)) { flyX -= fx * spd; flyY -= fy * spd; flyZ -= fz * spd; }
            if (kd(GLFW_KEY_D)) { flyX += rx * spd; flyZ += rz * spd; }
            if (kd(GLFW_KEY_A)) { flyX -= rx * spd; flyZ -= rz * spd; }
            if (spaceNow) flyY += spd;
            if (kd(GLFW_KEY_LEFT_CONTROL)) flyY -= spd;
            camX = flyX; camY = flyY; camZ = flyZ; camYaw = flyYaw; camPitch = flyPitch;
        }
        prevSpace = spaceNow;

        // ---- DIRECTOR-MODE input (uses last frame's aim pick — one frame of
        // latency on a 60+ Hz loop is imperceptible and keeps input pre-draw).
        const bool eNow = kd(GLFW_KEY_E);
        if (eNow && !prevE) sel = aimedLast;              // select / reselect / clear
        prevE = eNow;
        const bool fNow = kd(GLFW_KEY_F);
        if (fNow && !prevF && sel >= 0 && cast[sel]->skinnable()) {
            const int n = nextClip(cast[sel]->calmLoopClip(),
                                   (int)cast[sel]->skinner().clipCount());
            if (n >= 0) cast[sel]->setCalmLoopClip(n);
        }
        prevF = fNow;
        for (int i = 0; i < kCastCount; ++i) feed[i] = 0.0f;
        bool showTarget = false;
        x3::phys::Vec3 moveTarget{};
        if (sel >= 0) {
            if (kd(GLFW_KEY_Q)) pose[sel].yaw += 2.0f * dt;
            if (kd(GLFW_KEY_R)) pose[sel].yaw -= 2.0f * dt;
            if (kd(GLFW_KEY_G) &&
                aimFloorPoint(camX, camY, camZ, camYaw, camPitch, moveTarget)) {
                feed[sel] = directorMoveTick(pose[sel], moveTarget, dt);
                showTarget = true;
            }
        }
        placeTargetRing(showTarget, moveTarget);

        tickCast(dt, { camX, camY, camZ });
        updateRings(sel);
        titlePanel.update(dt);
        phys->step(dt);
        scene.update(*phys);

        int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
        if (cw != lastW || ch != lastH) { lastW = cw; lastH = ch; if (cw > 0 && ch > 0) device->onResize((uint32_t)cw, (uint32_t)ch); }

        device->setCamera(camX, camY, camZ, camYaw, camPitch, 60.0f);
        auto frame = device->beginFrame();
        if (frame.valid) {
            scene.render(*device, frame);
            drawCast(frame);
            aimedLast = drawLabelsAndPick(*device, frame, cast, pose, sel,
                                          camX, camY, camZ, camYaw, camPitch);
            drawDirectorHud(*device, frame, cast, sel, aimedLast);
        }
        device->endFrame(frame);
    }

    titlePanel.shutdown(*device);
    phys->shutdown();
    device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost

// ============================================================================
// --test-gallery: headless self-test (no window, no Vulkan) — the canon_aliens
// scaffolding. G1 cast builds, G2 real GLBs, G3 skinnable + clips, G4 the
// clip-cycle advances + wraps, G5 the restored test clips are present, G6 the
// DIRECTOR move tick walks an exhibit to a target and hands back to the calm
// loop, G7 the aim->floor solve.
// ============================================================================
#include "../headless_device.h"

namespace x3 { namespace apphost {

namespace {
int g_pass = 0, g_fail = 0;
void gcheck(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[gallery-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[gallery-test] FAIL ") + name); }
}
bool hasClip(const MonsterSystem& m, const char* name) {
    return m.skinnable() && exactClip(m.skinner(), name) >= 0;
}
} // namespace

bool runGallerySelfTest() {
    g_pass = g_fail = 0;
    x3::game::HeadlessRenderDevice device;
    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    phys->init();
    Scene scene;

    std::vector<std::unique_ptr<MonsterSystem>> cast;
    for (int i = 0; i < kCastCount; ++i)
        cast.push_back(spawnCastMember(scene, device, *phys, i));

    // G1: every cast member BUILDS (alive, real entity).
    {
        int ok = 0;
        for (const auto& c : cast) if (c->alive() && c->entity() != x3::game::kNoLink) ++ok;
        gcheck(ok == kCastCount, "G1 all gallery cast members build");
    }
    // G2: every cast member loads its REAL GLB (no fallback boxes in the museum).
    {
        int real = 0;
        for (int i = 0; i < kCastCount; ++i) {
            if (cast[i]->usingRealModel()) ++real;
            else x3::logError(std::string("[gallery-test] fallback box: ") + kCast[i].stem);
        }
        gcheck(real == kCastCount, "G2 all cast members load their real GLB");
    }
    // G3: every cast member is skinnable with at least one clip.
    {
        int ok = 0;
        for (int i = 0; i < kCastCount; ++i) {
            if (cast[i]->skinnable() && cast[i]->skinner().clipCount() >= 1) ++ok;
            else x3::logError(std::string("[gallery-test] not skinnable / no clips: ") + kCast[i].stem);
        }
        gcheck(ok == kCastCount, "G3 all cast members are skinnable with clips");
    }
    // G4: the clip cycle ADVANCES and WRAPS (nordic: full loop returns to start).
    {
        MonsterSystem& m = *cast[2];   // canon_nordic
        bool ok = m.skinnable() && m.skinner().clipCount() >= 2;
        if (ok) {
            startIdle(m);
            const int start = m.calmLoopClip();
            const int n = (int)m.skinner().clipCount();
            int cur = start;
            bool advanced = false;
            for (int s = 0; s < n; ++s) {
                cur = nextClip(cur, n);
                m.setCalmLoopClip(cur);
                if (m.calmLoopClip() != cur) ok = false;
                if (cur != start) advanced = true;
            }
            ok = ok && advanced && (cur == start);
        }
        gcheck(ok, "G4 clip cycle advances every clip and wraps to the start");
    }
    // G5: the RESTORED test clips are present (the whole point of the gallery).
    gcheck(hasClip(*cast[2], "Wave"),     "G5a canon_nordic has the restored Wave clip");
    gcheck(hasClip(*cast[8], "Wave"),     "G5b SalvariPrincess has the restored Wave clip");
    gcheck(hasClip(*cast[6], "IdleAlt"),  "G5c Oracle has the restored IdleAlt clip");
    gcheck(hasClip(*cast[7], "IdleAlt"),  "G5d AnnaBodySuit has the restored IdleAlt clip");

    // G6: DIRECTOR MODE — the move tick walks the selected exhibit across the
    // hall (Run beyond 8 m, Walk inside, exact blend-anchor speeds), converges
    // on the target, faces the travel direction, comes to rest (speed 0), and
    // the pinned calm loop (the exhibit's clip) is untouched — the windowed
    // G-hold, driven headlessly through the same MonsterSystem feed.
    {
        MonsterSystem& m = *cast[9];   // chief_martinez — the retargeted loco set
        gcheck(hasClip(m, "Walk"), "G6a chief_martinez has a Walk clip for the loco feed");
        startIdle(m);
        const int pinned = m.calmLoopClip();
        ExhibitPose p{ castSpot(9), 0.0f };
        const x3::phys::Vec3 target{ 2.5f, 0.0f, castSpot(9).z - 6.0f };
        // Start distance ~8.6 m: the tick must open on RUN, drop to WALK, stop.
        const float dt = 1.0f / 60.0f;
        bool sawRun = false, sawWalk = false;
        float speed = -1.0f;
        int steps = 0;
        for (; steps < 60 * 20; ++steps) {
            speed = directorMoveTick(p, target, dt);
            if (speed == kRunSpeed)  sawRun  = true;
            if (speed == kWalkSpeed) sawWalk = true;
            m.setPropPose(p.pos, p.yaw);
            m.setPropMotion(speed, 0.0f);
            m.update(dt, scene, *phys, { 0.0f, 1.7f, 0.0f });
            if (speed == 0.0f) break;
        }
        const float ddx = target.x - p.pos.x, ddz = target.z - p.pos.z;
        const float dist = std::sqrt(ddx * ddx + ddz * ddz);
        gcheck(speed == 0.0f && dist < kArrive + 0.01f,
               "G6b move tick converges on the aim target and comes to rest");
        gcheck(sawRun && sawWalk,
               "G6c the feed hits the Run anchor beyond 8 m and the Walk anchor inside");
        // Facing: the final yaw points along the last travel direction.
        {
            const float want = std::atan2(target.z - castSpot(9).z, target.x - castSpot(9).x);
            float d = p.yaw - want;
            while (d >  kPi) d -= 2.0f * kPi;
            while (d < -kPi) d += 2.0f * kPi;
            gcheck(std::fabs(d) < 0.05f, "G6d the exhibit faces its travel direction");
        }
        gcheck(m.calmLoopClip() == pinned && m.calmLoopActive(),
               "G6e the pinned calm clip survives the trip (Idle resumes at rest)");
    }
    // G7: the aim->floor solve — a downward aim lands a clamped in-hall floor
    // point; a level/upward aim refuses.
    {
        x3::phys::Vec3 pt{};
        const bool hit = aimFloorPoint(4.5f, 1.7f, -14.0f, 2.04f, -0.35f, pt);
        const bool inHall = hit && pt.y == 0.0f &&
                            std::fabs(pt.x) <= kHallHalfW - 0.7f + 1e-3f &&
                            std::fabs(pt.z) <= kHallHalfL - 0.7f + 1e-3f;
        x3::phys::Vec3 up{};
        const bool refuse = !aimFloorPoint(4.5f, 1.7f, -14.0f, 2.04f, 0.30f, up);
        gcheck(inHall && refuse, "G7 aim ray solves to a clamped floor point (up-aim refused)");
    }

    phys->shutdown();
    x3::logInfo("[gallery-test] " + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

}} // namespace x3::apphost
