#pragma once
// ============================================================================
// SKY SCALE — the invariant that keeps the sky IN THE SKY.   (KNOWN_BUGS B10)
//
// The celestial bodies (app/cinematic.{h,cpp} — the FORGE3D planet port) are NOT
// a skybox. They are REAL, depth-tested triangles, pinned to a shell a fixed
// distance from the eye and drawn AFTER the opaque meshes
// (VulkanRenderDevice::recordPlanetDraws):
//
//   * PASS 1, the opaque bodies, depth-test AND depth-WRITE. A body nearer than a
//     hull therefore PUNCHES THE HULL OUT of the frame.
//   * PASS 2, the ring / atmosphere / corona shells, depth-test LEQUAL with
//     depth-write OFF and ALPHA/ADDITIVE blending. They composite over ANYTHING
//     further from the eye than the shell — i.e. they SMEAR ACROSS the hull.
//
// So the whole system rests on ONE unstated assumption:
//
//        *** THE SKY SHELL MUST BE FARTHER AWAY THAN EVERY MESH. ***
//
// That held for the walk-around hosts it was written for (nightsky / showroom /
// car: a building a few dozen metres out, shell at 140 m). It was FALSE for the
// cold open, whose capital ship is a 200 m hull framed from ~90 m — the "sky" hung
// 140 m from the eye, INSIDE the fleet. The gas giant's red disc cut the hull in
// half and its tan ring + atmosphere bled across it, which is why the ship read as
// SEE-THROUGH GLASS WITH RED/PINK/YELLOW SMEARS. Nothing about the ship was wrong;
// it was never on a glass path, and its albedo was fine. The SKY was in the way.
//
// The same 200 m far plane also CLIPPED the capital ship clean out of frame for
// the whole first half of its reveal (t=20..27, at 340-550 m range), so the
// authored "distant speck -> looming hull" ramp never rendered — the ship simply
// popped into existence when it crossed 200 m.
//
// Both failures are the same root cause: THE SCENE IS BIGGER THAN ITS FRUSTUM.
// The far plane and the sky anchor are a PAIR. Move one, move the other, and hold
// the two predicates below. `--test-cutscene` asserts them over the whole cold-open
// timeline, with negative controls pinned to the old numbers.
//
// Header is deliberately DEPENDENCY-FREE (no RHI / GLFW / Vulkan) so the pure
// data+logic modules (cutscene.cpp) can assert the invariant without linking a
// renderer.
// ============================================================================

namespace x3::sky {

// ---- COLD-OPEN scale ------------------------------------------------------
// A deep-space film: Jake flies from z=0 to z=-920 and the capital ship is a 200 m
// hull. 15 km is a range the depth buffer already runs at elsewhere in the game
// (the streamed-planet host sets exactly this far plane), so it is proven.
inline constexpr float kColdOpenFar     = 15000.0f;             // camera far plane
inline constexpr float kColdOpenSkyDist = 0.70f * kColdOpenFar; // 10.5 km sky shell

// PREDICATE 1 — THE SKY IS BEHIND THE SHIP.
// True when a sky shell at `anchorDist` clears a hull whose centre is `hullDist`
// from the eye and whose bounding radius is `hullRadius`. If this is false, the
// planet occludes / bleeds onto the hull. THIS is the assertion B10 needed.
inline bool skyClearsHull(float anchorDist, float hullDist, float hullRadius) {
    return anchorDist > (hullDist + hullRadius);
}

// PREDICATE 2 — THE SHIP IS INSIDE THE FRUSTUM.
// True when the whole hull fits within the far plane. If this is false the ship is
// clipped away and pops in (the broken capital-ship reveal).
inline bool hullInsideFrustum(float farPlane, float hullDist, float hullRadius) {
    return (hullDist + hullRadius) < farPlane;
}

// PREDICATE 3 — THE SKY ITSELF IS NOT CLIPPED.
// A body of radius `bodyRadius` carrying a companion shell `shellMult` x its radius
// (1.0 = bare body, 1.06 = atmosphere, 2.2 = sun corona, 2.5 = ring annulus) must
// still land inside the far plane, or the sky itself gets cut. Checked PER BODY,
// against the real sky table, in CinematicScene::load — a blanket worst-case check
// is wrong here, because the WIDEST body (the terrestrial hero) carries only a thin
// atmosphere, while the body with the 2.5x ring (the gas giant) is far smaller.
inline bool bodyInsideFrustum(float farPlane, float anchorDist,
                              float bodyRadius, float shellMult) {
    return (anchorDist + bodyRadius * shellMult) < farPlane;
}

} // namespace x3::sky
