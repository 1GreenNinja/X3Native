#pragma once
// ============================================================================
// HoloPanel — THE holo-glass PLATFORM (Tim: "I want to use that technology all
// over the game, in variant form").
//
// There is exactly ONE holo implementation in this game and this is it. Every
// glowing screen — the canon cell terminal, the rifthub portal consoles, the
// elevator floor indicator, keypads, placards — is a HoloPanel with a different
// CONTENT BAKE. Do not hand-roll another emissive screen quad; add a baker.
//
// THE OWNER'S CANONICAL HOLO SPEC (binding):
//   * BLACK GLASS slab — dark, glossy, reflective, catches highlights.
//   * Glowing BLUE / GREEN / ORANGE status text. BLUE, NOT CYAN.
//   * A SHINY METALLIC ROUND-PIPE frame around the glass.
//   * A single support pipe from top-centre UP TO THE CEILING — it HANGS.
//   * Crisp, READABLE text. Readability beats decoration: the panel exists to
//     be READ, at [E] range, not admired in a close-up.
//
// TWO STRUCTURAL LAWS, both learned the hard way (~10 re-fixes):
//   1. NOTHING GOES IN FRONT OF THE SCREEN. Glass writes depth in the depth
//      pre-pass, so any overlay/backer/scanline pane between the eye and the
//      readout DEPTH-REJECTS it — that is the "featureless blue slab" bug. The
//      frame is OPAQUE METAL (coplanar rims cannot occlude). A shimmer belongs
//      in the TEXTURE, never in geometry.
//   2. THE GLOW IS PER-TEXEL. The pane is black glass with
//      GlassMaterial::emissiveMap = 1, so it lights up exactly where the baked
//      content is bright and the black substrate stays black. A FLAT emissive
//      cannot see the text, so the only way it can "glow" is to flood the pane
//      — which IS the featureless-slab look.
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

// ===========================================================================
// holo:: — THE CANVAS TOOLKIT. Every variant's content baker draws with these.
// One rasterizer, one font, one palette, one readability model.
// ===========================================================================
namespace holo {

// ---- THE PALETTE (canonical). Values are HDR-ish: strokes sit well above the
// black-glass base so they bloom. Tim: "text color BLUE, not cyan" — blue means
// a high BLUE channel with a low-mid green and a low red. Cyan (g ~= b) is banned.
struct Ink { float r, g, b; };
inline constexpr Ink kBlue   { 0.26f, 0.56f, 1.60f };   // headers / data / structure
inline constexpr Ink kBlueHi { 0.60f, 0.92f, 1.75f };   // bright blue-white: titles, rules
inline constexpr Ink kGreen  { 0.24f, 1.55f, 0.52f };   // OK / ONLINE / SECURE / STABLE
inline constexpr Ink kOrange { 1.65f, 0.72f, 0.14f };   // WARNING / ALERT / FAIL / LOCKED
inline constexpr Ink kAmber  { 1.34f, 0.80f, 0.22f };   // the live typed input prompt

// Keyword-driven STATUS COLOR — what makes a readout read like a real console
// instead of one flat colour. GREEN for good, ORANGE for bad, BLUE for the rest.
Ink statusInk(const std::string& line);

// Float RGB accumulation buffer. Strokes ADD light (they bloom over black glass).
struct Canvas {
    uint32_t n;
    std::vector<float> r, g, b;
    explicit Canvas(uint32_t nn)
        : n(nn), r((size_t)nn*nn, 0), g((size_t)nn*nn, 0), b((size_t)nn*nn, 0) {}
    inline void add(int x, int y, float rr, float gg, float bb, float a) {
        if (x < 0 || y < 0 || x >= (int)n || y >= (int)n) return;
        const size_t i = (size_t)y * n + x;
        r[i] += rr * a; g[i] += gg * a; b[i] += bb * a;
    }
    inline void addInk(int x, int y, Ink k, float a) { add(x, y, k.r, k.g, k.b, a); }
};

// ---- Line-art primitives (anti-aliased, additive). ----
void plot(Canvas& c, float fx, float fy, Ink k, float a);
void line(Canvas& c, float x0, float y0, float x1, float y1, Ink k, float a, float thick);
void rectFrame(Canvas& c, float x0, float y0, float x1, float y1, Ink k, float a, float thick);
void rectFill(Canvas& c, float x0, float y0, float x1, float y1, Ink k, float a);
void roundRectFrame(Canvas& c, float x0, float y0, float x1, float y1, float rad,
                    Ink k, float a, float thick);
void bracketFrame(Canvas& c, float x0, float y0, float x1, float y1,
                  float arm, float chamf, Ink k, float a, float thick);
void hexagon(Canvas& c, float cx, float cy, float rad, Ink k, float a, float thick);
void warnTriangle(Canvas& c, float cx, float topY, float halfW, float h, Ink k, float a, float thick);
void chevronDown(Canvas& c, float cx, float topY, float halfW, float h, Ink k, float a, float thick);
void chevronUp(Canvas& c, float cx, float botY, float halfW, float h, Ink k, float a, float thick);

// ---- Text (stb_truetype, CPU-rasterized INTO the texture so it lives ON the
// glass and tilts with the panel — a Babylon DynamicTexture, not a 2D overlay).
//
// `xScale` PRE-SQUASHES the glyphs horizontally. A square texture UV-mapped onto a
// NON-square panel is stretched to the panel's aspect, so on a wide strip (the keypad
// readout, the elevator floor indicator) square-rasterized type comes out ~2x too wide
// — smeared, and exactly the sort of thing that makes a readout "not quite readable"
// without anyone being able to say why. Pass xScale = 1/aspect to cancel the stretch.
bool  fontReady();
float textWidth(const std::string& s, float px, float xScale = 1.0f);
float drawText(Canvas& c, const std::string& s, float penX, float topY, float px,
               Ink k, float a, float xScale = 1.0f);

// ---- THE BLACK-GLASS BASE. Near-black with the faintest cool cast, fine
// scanlines + a data-grid + vignette. It TEXTURES the black; it does not fill it.
void blackGlassBase(Canvas& c);
// A warm near-black field (the amber placard variant).
void warmBase(Canvas& c);

// ---- READABILITY: THE QUIET BAND. ------------------------------------------
// The owner's note: "the way it conflicts with the graphics behind the text."
// Decorative line-art and glowing text were both drawn ADDITIVELY into the same
// pixels, so every tick-mark and schematic stroke under a glyph ADDED to it and
// ate its contrast — the text fought the art and the art won.
//
// The fix is compositional, not a brightness nudge: text gets its OWN ZONE. Call
// quietBand() over the text rect AFTER the line-art is drawn and BEFORE the text
// is: it MULTIPLIES DOWN everything already in that rect (feathered at the edges
// so it reads as a deliberate inset, not a hard cut), leaving a dark, quiet field
// for the glyphs to glow against. Decoration keeps to the margins; the readout wins.
//   `keep`    = how much of the underlying art survives inside the band (0.10 = 10%).
//   `feather` = px of soft falloff at the band edge.
void quietBand(Canvas& c, float x0, float y0, float x1, float y1,
               float keep = 0.10f, float feather = 10.0f);

// ---- INK PROBE (the regression gate). Fraction of pixels inside a rect whose
// luminance clears a legibility threshold. A readout with real text returns a
// healthy few percent; a blank/washed screen returns ~0. This is what lets a test
// assert "the screen HAS CONTENT" with no GPU — and, with a NEGATIVE CONTROL, what
// stops a featureless slab from ever shipping again.
float inkFraction(const Canvas& c, float x0, float y0, float x1, float y1,
                  float lumaThresh = 0.45f);
// Fraction of pixels in the rect that are dominantly BLUE / GREEN / ORANGE — so a
// test can assert the PALETTE, not merely that something is lit. (Cyan fails blue:
// blue requires b > g by a real margin.)
float blueFraction(const Canvas& c, float x0, float y0, float x1, float y1);
float greenFraction(const Canvas& c, float x0, float y0, float x1, float y1);
float orangeFraction(const Canvas& c, float x0, float y0, float x1, float y1);

// ---- Composite out: rounded-corner + edge fade, packed to RGBA8 (srgb).
std::vector<uint8_t> finish(const Canvas& c);

} // namespace holo

// ===========================================================================
// The FIXTURE.
// ===========================================================================

// How the panel is physically carried.
enum class HoloMount {
    CeilingPipe,   // a metallic pipe rises from the frame top-centre to the ceiling: it HANGS
    WallFlush,     // a slim back-box seats it flush to a wall (BEHIND the pane — never in front)
    FreeStand,     // a floor stand: base disc + pole to the frame bottom-centre
    None,          // caller mounts it (it is already inside a housing: keypad, elevator cab)
};

// The border around the glass.
enum class HoloFrame {
    Pipe,          // SHINY METALLIC round-pipe picture frame — the flagship look
    Bezel,         // a slim dark matte bezel
    None,          // frameless (the housing IS the bezel)
};

struct HoloPanelParams {
    // ---- Placement ----
    x3::phys::Vec3 pos{};                  // panel CENTRE, world
    float yaw    = 0.0f;                   // facing (0 => front face +Z)
    float width  = 0.85f, height = 1.05f;  // screen size (m)
    float cornerRadius = 0.0f;             // 0 => auto: min(hw,hh)*0.30
    float ceilingY = 0.0f;                 // CeilingPipe target; 0 => pos.y + 1.7
    float floorY   = 0.0f;                 // FreeStand base;    0 => pos.y - height

    // ---- The black-glass screen ----
    float paneOpacity   = 0.94f;           // HIGH => a black SLAB, not a blue window
    float paneRoughness = 0.03f;           // polished: it catches the room (the "shiny")
    float paneSpecular  = 1.0f;

    // ---- Content (the glowing text / line-art) ----
    // Returns the n x n RGBA8 screen bake. Bright texels glow; black texels stay
    // black (GlassMaterial::emissiveMap). Required.
    std::function<std::vector<uint8_t>(uint32_t n)> contentBake;
    uint32_t texN = 1024;
    float    emissiveStrength = 2.1f;      // glow multiplier (bloom source)
    float    shimmerIntensity = 1.0f;      // 0..N: emissive pulse in update()

    // ---- Frame + mount ----
    HoloFrame frame = HoloFrame::Pipe;
    HoloMount mount = HoloMount::CeilingPipe;

    // ---- Glow-light pool (the caller wires the point light into its own rig).
    // HONEST LIGHTING: the frame and the mount carry NO emissive. They are metal.
    // Emissive belongs to the SCREEN CONTENT only. A support pipe holds the weight;
    // it does not light the room.
    bool  glowLight = true;
    float glowColor[3] = { 0.32f, 0.66f, 1.55f };
    float glowRange = 3.4f;

    // Scene room tag for the per-room cull (kNoRoom => always visible).
    uint32_t roomId = kNoRoom;
};

class HoloPanel {
public:
    void build(Scene& scene, x3::rhi::IRenderDevice& device, const HoloPanelParams& p);

    // Advance the shimmer (a subtle emissive pulse on the SCREEN — because
    // emissiveMap is on, the pulse rides the readout ink itself: the text breathes).
    void update(float dt);

    // Re-bake the screen from a fresh content buffer (dynamic panels: the floor
    // highlight moves, keypad status flips, VIGIL streaming onto the glass).
    void setContent(const std::vector<uint8_t>& rgba);

    // Free every GPU resource build() created. The rifthub stands EIGHT of these and
    // its smoketest gates on allocationCount == 0. Safe to call unbuilt / twice.
    void shutdown(x3::rhi::IRenderDevice& device);

    bool built() const { return m_pane != kNoLink; }
    x3::phys::Vec3 anchor() const { return m_pos; }
    uint32_t paneEntity() const { return m_pane; }
    x3::rhi::TextureHandle screenTexture() const { return m_screenTex; }

    // REGRESSION GUARD: true only when the panel is standing in a Scene AND its pane
    // actually has a baked screen texture BOUND. A blank pane returns false, so a
    // caller can fail the build instead of shipping a black rectangle.
    bool screenHasContent() const;

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
    float m_emBase[4] = { 1.0f, 1.0f, 1.0f, 2.1f };
    float m_shimmer = 1.0f;
    float m_clock = 0.0f;
    std::vector<uint32_t> m_decor;                 // frame + mount entities (visual only)
    std::vector<x3::rhi::MeshHandle> m_meshes;     // everything build() created (shutdown frees)

    bool  m_hasGlow = false;
    float m_glowPos[3] = { 0, 0, 0 };
    float m_glowColor[3] = { 0, 0, 0 };
    float m_glowRange = 3.4f;
};

// ===========================================================================
// THE SHIPPED CONTENT BAKERS (one per variant). A new variant is a baker + params.
// The terminal's own baker lives in holo_terminal.cpp (it is the richest one).
//
// `aspect` = panel width / height. The bakers pre-squash their type by 1/aspect so
// the glyphs land correctly proportioned on wide strips (see holo::drawText).
// ===========================================================================

// ELEVATOR floor indicator: the floor list, the CURRENT floor highlighted GREEN,
// a state caption (IDLE / DOORS OPEN / DESCENDING), travel chevrons.
std::vector<uint8_t> bakeFloorSelect(uint32_t n, const std::vector<std::string>& floors,
                                     int sel, const std::string& caption, float aspect = 1.0f);
// KEYPAD readout: ENTER CODE, the entered digits in amber, and the lock status
// (GREEN granted / ORANGE locked). The keypad has REAL keys — this is the screen above
// them, not a virtual grid.
std::vector<uint8_t> bakeKeypad(uint32_t n, const std::string& entered, bool locked,
                                float aspect = 1.0f);
// PLACARD: a title + body lines on a warm amber field (signage / room labels).
std::vector<uint8_t> bakePlacard(uint32_t n, const std::vector<std::string>& lines,
                                 float aspect = 1.0f);

} // namespace x3::game
