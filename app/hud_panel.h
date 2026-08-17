#pragma once
// THE ONE HUD PANEL PRIMITIVE (feat/hud-restyle).
//
// Tim: "we need a rounded edges dark translucent box containing light text ...
// and a similar style for all the text .. make it look like it is supposed to
// be there like CP2077 - but on the left, not the right."
//
// Every HUD text block draws over a hudPanel() — a rounded-corner, dark
// translucent plate with an optional accent bar — so no HUD text ever sits raw
// over world pixels again (the objective line famously washed out on white
// terrain). ONE primitive, many customers: the objective block, the enemies
// counter, HP, weapon/ammo, interaction prompts ("[E] Use Terminal"), barks,
// dialog boxes, the dev console (HoloPanel green), the stats panel.
//
// Rounded corners are emulated with per-row strip quads through the existing
// IRenderDevice::drawHudQuad path (the engine's HUD layer has no SDF rect):
// a radius-R corner costs 2*R one-pixel strips, ~16 extra quads per panel at
// the default radius against a 24k-vert HUD ring — trivially cheap, and it
// needs no new shader, pipeline, or engine change.
//
// The palette echoes the in-world HoloPanel / terminal design language (black
// glass, glowing cyan/green/orange status text) so the HUD reads as part of
// the world, not painted over it.

#include "engine/rhi/IRenderDevice.h"

namespace x3::game {

// ---- Shared HUD panel palette (linear RGBA) -------------------------------
// Dark slate "black glass" fill, ~78% opaque: dark enough that light text
// reads over white terrain, translucent enough that the world stays present.
constexpr float kHudPanelFill[4]   = { 0.030f, 0.045f, 0.065f, 0.78f };
// The subtle 1px lighter top-edge line (cool, barely-there).
constexpr float kHudPanelEdge[4]   = { 0.45f, 0.75f, 0.95f, 0.30f };
// Accent bar colors — the game's established status-ink family.
constexpr float kHudAccentCyan[4]  = { 0.32f, 0.86f, 1.00f, 0.90f };
constexpr float kHudAccentGreen[4] = { 0.30f, 1.00f, 0.45f, 0.90f };
constexpr float kHudAccentAmber[4] = { 1.00f, 0.72f, 0.20f, 0.90f };
constexpr float kHudAccentRose[4]  = { 1.00f, 0.55f, 0.70f, 0.90f };
constexpr float kHudAccentRed[4]   = { 1.00f, 0.25f, 0.20f, 0.90f };
// Light text over the dark plate (off-white, cool-tinted).
constexpr float kHudTextLight[4]   = { 0.92f, 0.96f, 0.98f, 1.00f };
// The consistent corner radius (px) — one radius across the whole HUD.
constexpr float kHudPanelRadius    = 8.0f;

// Draw the rounded dark translucent panel.
//   radius  — corner radius in px (clamped to half the smaller dimension).
//   fill    — RGBA fill; nullptr = kHudPanelFill.
//   accent  — optional 3px accent bar down the LEFT inner edge; nullptr = none.
//   alpha   — whole-panel fade multiplier [0,1] (prompts fade with distance).
//   topEdge — draw the subtle 1px lighter line along the flat top span.
//   roundTop / roundBottom — corner rounding per edge pair (the console slab
//   keeps its top square: it slides from the screen edge).
void hudPanel(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              float x, float y, float w, float h,
              float radius = kHudPanelRadius,
              const float* fill   = nullptr,
              const float* accent = nullptr,
              float alpha = 1.0f,
              bool topEdge = true,
              bool roundTop = true,
              bool roundBottom = true);

// A one-line PROMPT CHIP: the panel sized to the text, centered on `cx`, top
// edge at `yTop`, light text (or `textCol`) with a drop shadow. The standard
// dress for "[E] ..." interaction prompts, toasts, and status tags.
// Returns the chip height (so callers can stack).
float hudPromptChip(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                    const char* text, float cx, float yTop, float px,
                    const float* textCol = nullptr,
                    float alpha = 1.0f,
                    const float* accent = nullptr,
                    x3::rhi::FontRole role = x3::rhi::FontRole::Menu);

} // namespace x3::game
