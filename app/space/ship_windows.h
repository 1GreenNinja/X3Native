// app/space/ship_windows.h
//
// S6 — the TRUE-PORTAL ship windows (the "Star Trek" moving space outside the glass).
//
// THE SIGNATURE INTERIOR TECH. When the player stands inside the static ship
// (S5 ShipInterior, static at the scene origin), the windows show REAL space
// outside — and it MOVES (parallaxes) as the ship "travels." Per decision 2.4
// the ship is static and the ENVIRONMENT carries the motion; so here the space
// backdrop ROTATES (envYaw/envPitch) while the window frames stay fixed, which
// reads as flying through space seen from inside.
//
// Approach (tractable within the existing RHI — NO custom pipelines, engine/rhi
// is untouched; mirrors sky_stars.* + ship_interior.*):
//   1) A SPACE BACKDROP — a procedural star/nebula dome (a re-used SkyStars
//      instance) drawn BEHIND all interior geometry (far, no depth write). Its
//      MODEL is rotated by envYaw/envPitch so successive frames sample a different
//      slice of space: the parallax/motion that sells "the ship is moving."
//   2) WINDOW PANES — a thin translucent GLASS quad placed at each manifest
//      window placement {x,y,z,w,h,yaw}. The interior hull (S5) has real GAPS at
//      the window placements (or the pane sits in the opening); because the dome
//      is drawn first and the pane is see-through, the moving space shows THROUGH
//      the opening. The pane gives the glass a faint reflective sheen.
//   3) LIGHT BLEED — a point light placed just INSIDE each window, tinted by the
//      space scene (cool starlight blue), so the interior near each window is lit
//      by "outside" — the give-away that the window is a real opening, not a decal.
//
// Game/slice code only — engine/ stays pure. REUSES (never modifies) ShipInterior
// (its public ShipManifest/window placements) + SkyStars (the backdrop dome). The
// host stands a Player inside a built interior and calls render() each frame with
// the moving-environment orientation.
#pragma once

#include "ship_interior.h"
#include "../sky_stars.h"

#include "engine/rhi/IRenderDevice.h"

#include <array>
#include <cstdint>
#include <vector>

namespace x3::space {

// Renders the true-portal space view through a ShipManifest's window placements:
// a moving space backdrop seen through translucent window panes + per-window
// light-bleed point lights. One instance owns the dome + pane meshes + textures
// it creates; shutdown() releases them.
class ShipWindows {
public:
    // Build the space backdrop (a SkyStars nebula dome) + one translucent glass
    // pane per manifest window + the per-window light-bleed point lights. The
    // panes are placed at each window's {x,y,z,w,h,yaw}. Call once.
    void init(rhi::IRenderDevice& device, const ShipManifest& manifest);

    // Per-frame draw: rotates the space backdrop by (envYaw,envPitch) so it
    // parallaxes past the static window frames (the motion-through-space trick),
    // emits the dome + the translucent window panes, and refreshes the light-bleed
    // point lights (so the host's other lights are preserved, the caller should
    // either let this own the point lights or merge them). `viewProj16` is accepted
    // for API parity (the dome is camera-anchored); `timeSec` drives the dome twinkle.
    void render(rhi::IRenderDevice& device, const rhi::FrameContext& frame,
                const float* viewProj16, float timeSec, float envYaw, float envPitch);

    // Anchor the backdrop dome + light-bleed reference on the eye (call before
    // render() each frame). If never called the dome stays centered on the origin
    // (fine for the cockpit showcase, where the interior straddles the origin).
    void setCamera(float ex, float ey, float ez);

    // The light-bleed point lights this system wants active this frame (filled by
    // render()). The host can upload them via device.setPointLights() — either
    // alone, or merged with its own interior fixtures. Stable across a frame.
    const std::vector<rhi::PointLight>& bleedLights() const { return m_bleed; }

    // Release the dome + pane meshes/textures. Idempotent.
    void shutdown(rhi::IRenderDevice& device);

    // ---- Introspection (used by --test-ship-windows) ----------------------
    uint32_t windowCount() const { return (uint32_t)m_panes.size(); }
    bool     initialized() const { return m_initialized; }
    // Last environment orientation passed to render() (the test asserts the
    // backdrop responds to a changed envYaw).
    float    lastEnvYaw()   const { return m_lastEnvYaw; }
    float    lastEnvPitch() const { return m_lastEnvPitch; }

private:
    // One window pane: a translucent quad mesh + its world placement.
    struct Pane {
        rhi::MeshHandle mesh{};
        std::array<float, 6> placement{}; // {x,y,z, w,h, yaw}
    };

    SkyStars                m_sky;          // the moving space backdrop dome
    std::vector<Pane>       m_panes;        // one glass pane per window
    rhi::TextureHandle      m_paneTex{};    // shared faint-blue glass tint texture
    std::vector<rhi::PointLight> m_bleed;   // per-window light-bleed lights
    bool                    m_initialized = false;
    float                   m_camX = 0.0f, m_camY = 0.0f, m_camZ = 0.0f;
    float                   m_lastEnvYaw = 0.0f, m_lastEnvPitch = 0.0f;
};

// Headless self-test (--test-ship-windows, >=5 checks). Uses HeadlessRenderDevice
// (no window / Vulkan):
//   T1 init() from makeSmallCockpit() populates windowCount()==2 (the cockpit's
//      forward viewport + port-side port) and initialized()==true;
//   T2 render() runs VUID-safe (headless draw is a no-op) and produces bleed
//      lights (one per window);
//   T3 a DIFFERENT envYaw drives a different backdrop orientation (lastEnvYaw
//      round-trips the changed angle — the parallax/motion input is live);
//   T4 sampling two env orientations both run without crashing (the moving-
//      environment model is exercised at >1 angle);
//   T5 shutdown() is clean + idempotent (initialized()==false, windowCount()==0,
//      no crash on a second call).
// Logs PASS/FAIL T#; returns true iff all pass. Lives in ship_windows.cpp.
bool runShipWindowsSelfTest();

} // namespace x3::space
