#pragma once
// ============================================================================
// vehcosmetics — the COSMETIC layer of the vehicle shop (x3.vehparts/1
// "cosmetic" sibling block).                       [LANE: inspx/veh-cosmetics]
//
// docs/design/VEHICLE_UPGRADES.md §2/§7 is the spec. The performance half
// (app/vehparts.{h,cpp}) is untouched: cosmetics live in a SEPARATE top-level
// block of assets/vehicles/parts.json, parsed by this module into a
// CosmeticCatalog, composed by composeVisual() into a render-side
// VehicleAppearance POD that DriveDemo::setAppearance() applies per draw.
// compose() (the physics composition) never sees any of this.
//
// v1 SCOPE (honest): paint (type + free player RGB + the two-layer clearcoat
// params — the "expensive" read), window tint, underglow lighting, wheel rim
// finish. NOT in v1: body kits (needs a GLB socket convention + art), vinyls
// (needs a decal system), pearlescent view-angle hue shift and metallic
// flake (need a shader term in mesh.frag, which the clustered-lights lane
// owns — the paint tiers still carry distinct clearcoat/metallic responses).
//
// PERSISTENCE: CosmeticBuild saves to vehlook.json BESIDE vehbuild.json
// (same versioned-tag + fail-gracefully discipline). Credits stay in the
// performance VehicleBuild — one wallet; the shop debits it for both.
//
// CLEAN-ROOM, original work. No other game or engine source consulted.
// ============================================================================
#include <string>
#include <vector>

namespace x3::game::vehcosmetics {

// One cosmetic part. Field union across categories (defaults inert), same
// pragmatic shape as vehparts::Part.
struct CosPart {
    std::string id;          // "paint_candy"
    std::string category;    // owning category id ("paint")
    std::string name;
    int   tier  = 1;
    int   price = 0;
    // paint
    std::string paintType;   // solid|matte|metallic|pearlescent|candy|chrome
    float clearcoat      = 0.0f;
    float clearcoatRough = 0.05f;
    float metallicScale  = 1.0f;
    bool  chrome         = false;   // base color forced bright-metal
    // tint
    float tintDark = 0.0f;          // 0 = clear glass, 1 = opaque black
    // lighting (underglow)
    float glowIntensity = 0.0f;     // 0 = not a glow part
    std::string glowMode;           // "static" | "pulse"
    // wheels
    float rimColor[3] = { 0, 0, 0 };
    bool  hasRimColor  = false;
    bool  rimMatchPaint = false;
    float rimMetallic  = 0.0f;
};

struct CosCategory { std::string id, label, kind; };

class CosmeticCatalog {
public:
    // Parse the SAME parts.json the performance catalog reads; only the
    // "cosmetic" block is consumed. A file without one yields ok() == true
    // and an empty catalog (older data keeps working).
    bool loadFile(const std::string& path);
    bool loadJson(const std::string& json);

    bool ok() const { return m_ok; }
    const std::vector<CosCategory>& categories() const { return m_categories; }
    const std::vector<CosPart>&     parts() const { return m_parts; }
    const CosPart* find(const std::string& id) const;
    std::vector<const CosPart*> inCategory(const std::string& cat) const;

private:
    bool m_ok = false;
    std::vector<CosCategory> m_categories;
    std::vector<CosPart> m_parts;
};

// ---------------------------------------------------------------------------
// CosmeticBuild — what is INSTALLED + the player-picked colors.
// ---------------------------------------------------------------------------
struct CosmeticBuild {
    // category id -> installed part id ("" / absent = stock look).
    std::vector<std::pair<std::string, std::string>> installed;
    float paintRGB[3]    = { 0.72f, 0.05f, 0.08f };   // player-picked body color
    float underglowRGB[3]= { 0.10f, 0.55f, 1.00f };   // player-picked glow color

    const std::string* installedIn(const std::string& category) const;
    void  install(const std::string& category, const std::string& partId);
    void  removeFrom(const std::string& category);

    std::string toJson() const;
    bool        fromJson(const std::string& json);
    bool        saveFile(const std::string& path) const;
    bool        loadFile(const std::string& path);
};

// Default look save path (vehlook.json beside vehbuild.json).
std::string defaultLookSavePath();

// ---------------------------------------------------------------------------
// VehicleAppearance — the composed render-side POD DriveDemo applies.
// Everything at defaults = the authored GLB look, byte-identical.
// ---------------------------------------------------------------------------
struct VehicleAppearance {
    bool  paintOn = false;
    float paintRGB[3] = { 1, 1, 1 };
    float clearcoat = 0.0f, clearcoatRough = 0.05f;
    float metallicScale = 1.0f;

    bool  tintOn = false;
    float tintDark = 0.0f;          // glass darkening 0..1

    bool  glowOn = false;
    float glowRGB[3] = { 0, 0, 0 };
    float glowIntensity = 0.0f;
    bool  glowPulse = false;

    bool  rimOn = false;
    float rimRGB[3] = { 0, 0, 0 };
    float rimMetallic = 0.0f;
};

// Compose the look. Pure; missing parts = that aspect stays authored.
VehicleAppearance composeVisual(const CosmeticCatalog& cat, const CosmeticBuild& build);

// Headless self-test (--test-vehcosmetics): catalog parse (categories /
// tiers / prices / field routing), composeVisual() (paint + clearcoat
// propagate, color-match rims take the paint RGB, tint ordering, glow mode),
// the CosmeticBuild JSON round-trip incl. player colors, a MISSING-block
// file yielding an empty-but-ok catalog, and a NEGATIVE CONTROL (truncated
// JSON must fail the load and leave the build untouched). Logs PASS/FAIL C#.
bool runVehCosmeticsSelfTest();

} // namespace x3::game::vehcosmetics
