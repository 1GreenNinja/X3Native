// Procedural graybox test level (S2). See app/level.h + docs/LEVEL_GEOMETRY.md.
#include "level.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

namespace {

// Identity transform; level pieces author world-space geometry directly (the
// box builder bakes the world center in), so each Entity's model matrix is I.
constexpr float kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

// Helper: register a PrimMesh as a static collision body, upload its render
// mesh + texture, and add an Entity. Returns the new entity id.
struct GrayboxPiece {
    x3::prims::PrimMesh geo;
    x3::rhi::TextureHandle tex;   // may be invalid (=> default white, flat color)
    float color[4];
    uint32_t tag = (uint32_t)Tag::Static;
    bool collide = true;
};

uint32_t addPiece(Scene& scene,
                  x3::rhi::IRenderDevice& device,
                  x3::phys::IPhysicsWorld& physics,
                  const GrayboxPiece& piece) {
    Entity e;
    e.mesh = device.createMesh(piece.geo.verts.data(), (uint32_t)piece.geo.verts.size(),
                               piece.geo.index.data(), (uint32_t)piece.geo.index.size());
    e.tex = piece.tex;
    e.baseColor[0] = piece.color[0];
    e.baseColor[1] = piece.color[1];
    e.baseColor[2] = piece.color[2];
    e.baseColor[3] = piece.color[3];
    for (int i = 0; i < 16; ++i) e.transform[i] = kIdentity[i];
    e.tag = piece.tag;
    e.visible = true;

    if (piece.collide) {
        e.body = physics.addStaticMesh(piece.geo.cverts.data(),
                                       (uint32_t)(piece.geo.cverts.size() / 3),
                                       piece.geo.cindex.data(),
                                       (uint32_t)piece.geo.cindex.size());
    }
    // NOTE: makeBox() bakes world-space positions into the geometry, so each
    // piece's authored model transform is identity. The static mesh body sits at
    // the origin (position (0,0,0)), so Scene::update()'s position sync writes
    // (0,0,0) into the translation column — a harmless no-op that keeps identity.
    // The body handle is retained so future slices can query/remove the piece.
    return scene.add(e);
}

} // namespace

void buildTestLevel(Scene& scene,
                    x3::rhi::IRenderDevice& device,
                    x3::phys::IPhysicsWorld& physics) {
    // ---- Dimensions (meters) ----
    const float kRoomHalf   = 8.0f;   // 16 x 16 m floor
    const float kWallH      = 3.0f;   // wall height
    const float kWallT      = 0.2f;   // wall thickness (half = 0.1)
    const float kDoorHalf   = 0.6f;   // 1.2 m doorway opening (half width)
    const float kStepH      = 0.3f;   // 0.3 m step/ledge

    x3::logInfo("buildTestLevel: 16x16 m graybox room, 3 m walls, 1.2 m doorway, 0.3 m step");

    // ---- Shared graybox textures (distinct so surfaces read clearly) ----
    // Floor: blue-grey checker. Walls: warm checker. Step: green solid.
    auto floorPx = x3::prims::makeCheckerRGBA(256, 32,  210,210,220,  45,60,95);
    x3::rhi::TextureHandle floorTex = device.createTexture(floorPx.data(), 256, 256, true);

    auto wallPx = x3::prims::makeCheckerRGBA(256, 32,  205,170,120,  90,60,40);
    x3::rhi::TextureHandle wallTex = device.createTexture(wallPx.data(), 256, 256, true);

    auto stepPx = x3::prims::makeCheckerRGBA(128, 32,  150,210,150,  40,90,55);
    x3::rhi::TextureHandle stepTex = device.createTexture(stepPx.data(), 128, 128, true);

    // ---- Floor (thin slab so it has real collision volume) ----
    {
        GrayboxPiece floor;
        floor.geo = x3::prims::makeBox(kRoomHalf, 0.05f, kRoomHalf,  0.0f, -0.05f, 0.0f, 0.5f);
        floor.tex = floorTex;
        floor.color[0]=1; floor.color[1]=1; floor.color[2]=1; floor.color[3]=1;
        addPiece(scene, device, physics, floor);
    }

    // ---- Walls. Three solid walls + one wall split around a doorway gap. ----
    // Wall centers sit just inside the room edge. Each wall spans the full
    // 16 m run (length 2*kRoomHalf) along its axis.
    const float wallCY = kWallH * 0.5f;
    const float edge   = kRoomHalf;  // wall inner face roughly at +/-8

    // -Z wall (solid), runs along X.
    {
        GrayboxPiece w;
        w.geo = x3::prims::makeBox(kRoomHalf, kWallH*0.5f, kWallT*0.5f,
                                   0.0f, wallCY, -edge, 0.5f);
        w.tex = wallTex;
        w.color[0]=1; w.color[1]=0.85f; w.color[2]=0.75f; w.color[3]=1;
        addPiece(scene, device, physics, w);
    }
    // +X wall (solid), runs along Z.
    {
        GrayboxPiece w;
        w.geo = x3::prims::makeBox(kWallT*0.5f, kWallH*0.5f, kRoomHalf,
                                   edge, wallCY, 0.0f, 0.5f);
        w.tex = wallTex;
        w.color[0]=0.78f; w.color[1]=1; w.color[2]=0.80f; w.color[3]=1;
        addPiece(scene, device, physics, w);
    }
    // -X wall (solid), runs along Z.
    {
        GrayboxPiece w;
        w.geo = x3::prims::makeBox(kWallT*0.5f, kWallH*0.5f, kRoomHalf,
                                   -edge, wallCY, 0.0f, 0.5f);
        w.tex = wallTex;
        w.color[0]=0.80f; w.color[1]=0.82f; w.color[2]=1; w.color[3]=1;
        addPiece(scene, device, physics, w);
    }
    // +Z wall WITH a doorway gap centered at X=0. Split into two segments:
    // left segment spans [-kRoomHalf, -kDoorHalf], right [+kDoorHalf, +kRoomHalf].
    {
        const float segHalf = (kRoomHalf - kDoorHalf) * 0.5f;
        const float leftCX  = -(kDoorHalf + segHalf);
        const float rightCX =  (kDoorHalf + segHalf);
        // left segment
        {
            GrayboxPiece w;
            w.geo = x3::prims::makeBox(segHalf, kWallH*0.5f, kWallT*0.5f,
                                       leftCX, wallCY, edge, 0.5f);
            w.tex = wallTex;
            w.color[0]=1; w.color[1]=0.95f; w.color[2]=0.70f; w.color[3]=1;
            addPiece(scene, device, physics, w);
        }
        // right segment
        {
            GrayboxPiece w;
            w.geo = x3::prims::makeBox(segHalf, kWallH*0.5f, kWallT*0.5f,
                                       rightCX, wallCY, edge, 0.5f);
            w.tex = wallTex;
            w.color[0]=1; w.color[1]=0.95f; w.color[2]=0.70f; w.color[3]=1;
            addPiece(scene, device, physics, w);
        }
        // lintel above the doorway (so the opening reads as a doorway, not a
        // floor-to-ceiling slot). Spans the door width, from kStepH*... up.
        {
            const float lintelBottom = 2.1f;                 // head clearance
            const float lintelHalf   = (kWallH - lintelBottom) * 0.5f;
            const float lintelCY     = lintelBottom + lintelHalf;
            GrayboxPiece w;
            w.geo = x3::prims::makeBox(kDoorHalf, lintelHalf, kWallT*0.5f,
                                       0.0f, lintelCY, edge, 0.5f);
            w.tex = wallTex;
            w.color[0]=1; w.color[1]=0.95f; w.color[2]=0.70f; w.color[3]=1;
            addPiece(scene, device, physics, w);
        }
    }

    // ---- A 0.3 m step / ledge inside the room (a low platform to step onto). ----
    {
        GrayboxPiece step;
        step.geo = x3::prims::makeBox(2.0f, kStepH*0.5f, 2.0f,
                                      3.0f, kStepH*0.5f, -3.0f, 0.5f);
        step.tex = stepTex;
        step.color[0]=1; step.color[1]=1; step.color[2]=1; step.color[3]=1;
        addPiece(scene, device, physics, step);
    }

    x3::logInfo("buildTestLevel: " + std::to_string(scene.size()) + " static entities built");
}

} // namespace x3::game
