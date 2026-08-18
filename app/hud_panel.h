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
#include "engine/core/IConsole.h"

namespace x3::game {

// ---- Shared HUD panel palette (linear RGBA) -------------------------------
// NEAR-BLACK GLASS, not a grey card (Tim's calibration): the fill colour is
// almost pure black, so on any normal scene this reads as smoked glass rather
// than the pale slate card it used to be.
//
// ON THE ALPHA — this is a measured value, not a taste one. The swapchain is
// sRGB-encoded, so a translucent plate over white terrain lands much lighter
// than the linear math suggests: at 0.70 alpha the composite is linear 0.31,
// which encodes to sRGB ~0.60 — MID GREY, and light text on mid grey is the
// exact wash-out that started this work (verified in shots_hud/hud_white.png).
// 0.86 puts the plate at linear ~0.14 / sRGB ~0.41 over pure white: still
// visibly translucent (the world reads through it on dark and mid scenes) but
// dark enough that light text holds at the worst vantage in the game. Going
// more transparent than this trades away the bug we were asked to fix.
constexpr float kHudPanelFill[4]   = { 0.008f, 0.012f, 0.018f, 0.86f };
// The subtle 1px lighter top-edge line (cool, barely-there).
constexpr float kHudPanelEdge[4]   = { 0.45f, 0.75f, 0.95f, 0.26f };
// Accent bar colors — the game's established status-ink family.
constexpr float kHudAccentCyan[4]  = { 0.32f, 0.86f, 1.00f, 0.90f };
constexpr float kHudAccentGreen[4] = { 0.30f, 1.00f, 0.45f, 0.90f };
constexpr float kHudAccentAmber[4] = { 1.00f, 0.72f, 0.20f, 0.90f };
constexpr float kHudAccentRose[4]  = { 1.00f, 0.55f, 0.70f, 0.90f };
constexpr float kHudAccentRed[4]   = { 1.00f, 0.25f, 0.20f, 0.90f };
// Light text over the dark plate (off-white, cool-tinted).
constexpr float kHudTextLight[4]   = { 0.92f, 0.96f, 0.98f, 1.00f };
// The consistent corner radius (px) — one radius across the whole HUD.
//
// TINY on purpose (Tim, 2026-08-18: "tiny rounded corners"). 8px read as a
// friendly app card / soft pill; 3px reads as a MACHINED CHAMFER — the edge is
// broken so nothing looks laser-cut-sharp, but the silhouette is still a
// rectangle, which is what an instrument panel in this world should be. It is
// deliberately ONE constant: every consumer (HUD blocks, objective, HP, prompt
// chips, dialog boxes, the pause/main menus via UiContext::panel, the dev
// console slab, the weapon tuning panel) reads it, so the whole family changes
// together and no screen can drift into its own radius.
constexpr float kHudPanelRadius    = 3.0f;

// ---- THE ONE SPACING SCALE --------------------------------------------------
// Every HUD block uses these and nothing else, so margins and gaps agree
// everywhere instead of each block inventing its own 8/10/12/16/18/22.
constexpr float kHudMargin = 16.0f;   // screen edge -> panel
constexpr float kHudGap    = 12.0f;   // panel -> panel within a stack
constexpr float kHudPadX   = 14.0f;   // panel edge -> text (horizontal)
constexpr float kHudPadY   = 10.0f;   // panel edge -> text (vertical)

// ---- LIVE HUD GLASS TUNING (the colour-picker fold, 2026-08-18) ------------
// The two numbers above — the glass fill and the corner radius — were BOTH
// settled by rebuild-and-look cycles: the 0.86 alpha took a measured pass over a
// white-terrain vantage, and the radius took a second one to get from "friendly
// app card" to "machined chamfer". That is the exact loop a colour picker is
// supposed to delete, so they are now live cvars as well as constants:
//
//     hud_glass_r/g/b/a   the panel fill (linear, 0..1)
//     hud_radius          the ONE corner radius, in px, for every consumer
//
// DEFAULTS ARE THE CONSTANTS, byte-for-byte, so a process that never touches
// them renders identically to before — same discipline as FxTuning. Every
// hudPanel() caller reads this, including the ones that pass kHudPanelRadius
// explicitly, so a live radius change moves the WHOLE panel family together
// (that is the "one visual system" property; it must not be per-screen).
struct HudPanelTuning {
    float fill[4] = { 0.008f, 0.012f, 0.018f, 0.86f };
    float radius  = 3.0f;
};
HudPanelTuning& hudPanelTuning();          // process-wide live state
void registerHudPanelCVars(x3::con::IConsole& console);   // once, at startup
void applyHudPanelCVars(const x3::con::IConsole& console); // per frame, on the sync hub

// TRUE line height for stacked text at cap size `px`.
//
// THIS IS THE MULTI-LINE OVERPRINT FIX. A HUD glyph drawn at `px` is NOT px
// tall on screen: the atlas scales by px / cellAdvance('M'), so the inked
// ascender runs well past the nominal cap height. Advancing a wrapped block by
// px + a few pixels therefore draws line 2 through the belly of line 1. The
// console learned this the hard way years ago (its comment already reads "TTF
// glyphs need ~1.5x leading (was +2 -> lines overlapped)") — this hoists that
// constant to the one place every multi-line block can share.
constexpr float hudLineH(float px) { return px * 1.5f; }

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
