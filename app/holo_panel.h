#pragma once
// ============================================================================
// HoloPanel — the reusable HOLO-GLASS PLATFORM (Tim: "I want to use that
// technology all over the game, in variant form").
//
// Factors the perfected flagship terminal into one parameterised fixture:
//   * a GLOSSY BLACK-GLASS screen pane (near-black dielectric — Cook-Torrance
//     fresnel + specular highlight — with a baked RGBA texture driving its
//     EMISSIVE, so glowing text/line-art reads crisp on the dark glass),
//   * a FRAME around the screen edge: a SHINY METALLIC round-pipe picture frame,
//     a slim dark bezel, or none,
//   * a MOUNT that physically carries the panel: a ceiling SUPPORT PIPE (the
//     panel hangs from it, with collar joints), a flush WALL back-box, or a
//     freestanding floor STAND,
//   * a soft GLOW-LIGHT pool suggestion (the caller wires the point light).
//
// A new variant is ~20 lines: fill a HoloPanelParams, supply a `contentBake`
// that returns the n×n RGBA for the screen, call build(). See holo_panel.cpp's
// four shipped variants (terminal / elevator floor-select / keypad / placard).
//
// Game/slice code only; engine/ stays pure.
// ============================================================================
#include "scene.h"
#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace x3::game {

// How the panel is physically supported (a HoloPanel PARAMETER so variants reuse it).
enum class HoloMount {
    CeilingPipe,   // a metallic round pipe rises from the frame top-center to the ceiling (hangs)
    WallFlush,     // a slim back-box seats it flush against a wall
    FreeStand,     // a floor stand: a base disc + a pole up to the frame bottom-center
};

// The border around the screen glass (a HoloPanel PARAMETER).
enum class HoloFrame {
    Pipe,          // a shiny metallic round-pipe picture frame (the flagship look)
    Bezel,         // a slim dark matte bezel ring
    None,          // frameless (the glass edge is the edge)
};

struct HoloPanelParams {
    // ---- Placement ----
    x3::phys::Vec3 pos{};                 // panel CENTER, world
    float yaw    = 0.0f;                  // facing (0 => front face +Z toward the room)
    float width  = 0.85f, height = 1.05f; // screen size (m)
    float cornerRadius = 0.0f;            // 0 => auto (min(hw,hh)*0.30)
    float ceilingY = 0.0f;                // CeilingPipe target; 0 => pos.y + 1.7
    float floorY   = 0.0f;                // FreeStand base; 0 => pos.y - height

    // ---- Screen glass ----
    float paneDarkness = 0.03f;           // near-black albedo of the black-glass pane
    float paneRoughness = 0.10f;          // dielectric gloss (low => sharp reflections)
    float glassTint[3] = { 0.12f, 0.15f, 0.22f }; // (optional front slab tint; unused by default)

    // ---- Content (the glowing text/line-art) ----
    // Returns the n×n RGBA8 baked screen texture (drives BOTH albedo + emissive).
    // Bright pixels glow; near-black pixels stay dark. Required.
    std::function<std::vector<uint8_t>(uint32_t n)> contentBake;
    uint32_t texN = 1024;
    float    emissiveStrength = 2.3f;     // glow multiplier (bloom source)
    float    emissiveTint[3] = { 1.0f, 1.0f, 1.0f }; // neutral => the baked status colors survive
    float    shimmerIntensity = 1.0f;     // 0..N: emissive pulse amount in update()

    // ---- Frame + mount ----
    HoloFrame frame = HoloFrame::Pipe;
    HoloMount mount = HoloMount::CeilingPipe;
    float frameColor[3] = { 0.80f, 0.82f, 0.86f }; // brushed-steel albedo (Pipe) / dark (Bezel)

    // ---- Glow-light pool ----
    bool  glowLight = true;
    float glowColor[3] = { 0.32f, 0.66f, 1.55f };  // premultiplied blue pool
    float glowRange = 3.4f;
};

class HoloPanel {
public:
    // Build all render entities into `scene` via `device`. Idempotent per instance.
    void build(Scene& scene, x3::rhi::IRenderDevice& device, const HoloPanelParams& p);

    // Advance the shimmer (subtle emissive pulse). Safe to call every frame.
    void update(float dt);

    // Re-bake the screen texture from a fresh content buffer (dynamic panels:
    // floor-select highlight moves, keypad digits, VIGIL streaming). n = texN.
    void setContent(const std::vector<uint8_t>& rgba);

    bool built() const { return m_pane != kNoLink; }
    x3::phys::Vec3 anchor() const { return m_pos; }
    uint32_t paneEntity() const { return m_pane; }

    // The suggested glow-light the caller should add to its point-light set (so the
    // pool is fed into the SAME lighting path the world uses). Valid when p.glowLight.
    bool  hasGlowLight() const { return m_hasGlow; }
    const float* glowLightPos()   const { return m_glowPos; }
    const float* glowLightColor() const { return m_glowColor; }
    float glowLightRange() const { return m_glowRange; }

private:
    Scene*  m_scene = nullptr;
    x3::rhi::IRenderDevice* m_device = nullptr;
    x3::phys::Vec3 m_pos{};
    uint32_t m_pane = kNoLink;
    x3::rhi::TextureHandle m_screenTex{};
    uint32_t m_texN = 1024;
    float m_emBase[4] = { 1.0f, 1.0f, 1.0f, 2.3f };
    float m_shimmer = 1.0f;
    float m_clock = 0.0f;
    std::vector<uint32_t> m_decor;   // frame + mount entity ids (visual only)

    bool  m_hasGlow = false;
    float m_glowPos[3] = { 0, 0, 0 };
    float m_glowColor[3] = { 0, 0, 0 };
    float m_glowRange = 3.4f;
};

// ---- Ready-made content bakers for the shipped variants (each ~20 lines). ----
// Elevator FLOOR-SELECT list: floor labels down the glass, one HIGHLIGHTED (green),
// up/down chevrons. `sel` is the selected row.
std::vector<uint8_t> bakeFloorSelect(uint32_t n, const std::vector<std::string>& floors, int sel);
// KEYPAD: a 3x4 digit grid + the entered code line (amber).
std::vector<uint8_t> bakeKeypad(uint32_t n, const std::string& entered);
// Freestanding amber INFO PLACARD: a title + body lines, warm amber, slim border.
std::vector<uint8_t> bakePlacard(uint32_t n, const std::vector<std::string>& lines);

} // namespace x3::game
