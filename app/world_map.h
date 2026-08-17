#pragma once
// INTERACTIVE WORLD MAP — the GTA-grade full-screen map: top-down map tiles
// BAKED FROM THE REAL GEOMETRY, smooth lerped pan + cursor-anchored exponential
// zoom, POI discovery (StoryFlags-persisted), one waypoint (fed to the existing
// minimap as an edge chevron), and fast travel to DISCOVERED POIs through the
// streaming-aware teleport path (WorldStreamer's proxy fallback covers the
// realize window).
//
// CLEAN-ROOM, original work, built only on X3Native's OWN systems:
//   * TILES: a CPU top-down rasterizer. The Spire floors bake from the parsed
//     LevelDoc (level_loader CanonFloor — rooms + doorways, the same data that
//     BUILDS the world, so the map auto-corrects after every editor change);
//     code-built regions (city / ocean base / surface landmarks) bake from their
//     live region-ledger entities' world AABBs (IRenderDevice::meshBounds x the
//     entity transform). Style: readable blueprint, depth-banded tinting (walls
//     bright, floors dark, deeper rooms darker), NOT a screenshot. Tiles upload
//     once via createTexture and composite with drawHudImage; bake-on-first-
//     open per region/floor, cached; invalidate* rebakes (hot-reload hook).
//   * CAMERA: MapCamera — pixel-per-meter scale + world center, exponential
//     lerp toward targets; mousewheel zoom is CURSOR-ANCHORED (the world point
//     under the cursor stays fixed through the whole zoom animation — the
//     invariant the self-test asserts). Drag + WASD pan.
//   * POIs: assets/world/map_pois.json (format x3.mappois/1). Hidden until
//     DISCOVERED by proximity -> StoryFlag "poi.<id>.found" (persisted with the
//     same flags file dialog/missions use). Undiscovered regions draw fogged.
//   * FAST TRAVEL RULES: only to discovered POIs with fast_travel=true; blocked
//     while the "alert.active" StoryFlag is set (this lineage has no alert
//     system — the flag IS the hook); blocked while the current mission stage
//     sets no_fasttravel (optional x3.mission/1 stage field).
//
// Headless self-test: --test-worldmap (runWorldMapSelfTest).

#include "scene.h"
#include "level_loader.h"
#include "story_ops.h"
#include "ui.h"

#include "engine/rhi/IRenderDevice.h"

#include <string>
#include <vector>

namespace x3::game {

// ---------------------------------------------------------------------------
// Map camera — pan/zoom math (pure; headless-testable).
// World XZ maps to screen pixels: +X right, +Z down; `scale` is px per meter.
// ---------------------------------------------------------------------------
struct MapCamera {
    float vw = 1280.0f, vh = 720.0f;   // viewport pixels
    float cx = 0.0f, cz = 0.0f;        // current world center
    float tCx = 0.0f, tCz = 0.0f;      // pan target
    float scale = 1.0f, tScale = 1.0f; // current / target px-per-meter
    float minScale = 0.05f;            // world overview (~20 km across a 1080p view)
    float maxScale = 48.0f;            // room detail
    // MAP ROTATION (Q/E, W-MAP v3). Radians, world-frame: rot=0 is the
    // original unrotated north-per-worldToPx orientation every existing
    // caller/self-test assumes. `rot` is what's actually drawn with;
    // `tRot` is what Q/E steer toward, lerped in update() same as pan/zoom —
    // so a held key sweeps smoothly instead of snapping.
    float rot = 0.0f, tRot = 0.0f;
    // The GTA zoom feel: wheel re-anchors the world point under the cursor and
    // the camera holds that point fixed under the cursor pixel through the
    // whole zoom lerp. Cleared by any pan.
    bool  anchorActive = false;
    float anchorWx = 0.0f, anchorWz = 0.0f, anchorPx = 0.0f, anchorPy = 0.0f;

    static constexpr float kZoomLerpRate  = 12.0f;  // 1/s — exp converge (~90% in 0.19 s)
    static constexpr float kPanLerpRate   = 14.0f;  // 1/s
    static constexpr float kRotLerpRate   = 10.0f;  // 1/s
    static constexpr float kWheelStepMul  = 1.30f;  // zoom factor per wheel notch

    void setViewport(float w, float h) { if (w > 0) vw = w; if (h > 0) vh = h; }
    void jumpTo(float wx, float wz, float s);              // snap (map open) — also resets rotation
    void zoomAt(float pxX, float pxY, float wheelSteps);   // cursor-anchored exponential zoom
    void panPixels(float dxPx, float dyPx);                // mouse drag (immediate)
    void panWorld(float dxM, float dzM);                   // WASD (target shift)
    void rotateBy(float dRad);                             // Q/E (target shift, shortest-path lerp)
    void update(float dt);                                 // lerp toward targets
    void worldToPx(float wx, float wz, float& pxX, float& pxY) const;
    void pxToWorld(float pxX, float pxY, float& wx, float& wz) const;
    // World-space unit direction -> unit screen-space direction, same
    // rotation worldToPx applies (no translation/scale) — how the compass
    // rose places its N/E/S/W letters so they stay truthful as the map spins.
    void worldDirToScreenDir(float dx, float dz, float& sx, float& sy) const;
    // Screen-pixel vector -> world-metre vector at scale `s` (the shared
    // inverse the anchor/drag/WASD math uses — see the .cpp note; it carries
    // the north-up flip and the rotation so those paths can never drift from
    // pxToWorld again).
    void screenVecToWorldVec(float dxPx, float dyPx, float s,
                             float& wdx, float& wdz) const;
    bool settled(float scaleEps = 1e-3f, float panEpsM = 1e-2f) const;
};

// ---------------------------------------------------------------------------
// POI table (assets/world/map_pois.json — format x3.mappois/1).
// ---------------------------------------------------------------------------
struct MapPoi {
    std::string id;          // unique ("jakes_cell")
    std::string name;        // display ("Jake's Cell")
    std::string type;        // icon class: cell|hall|security|armory|secret|boss|
                             // elevator|door|club|landmark|city|base
    std::string region;      // owning region id (regions.json) — "" = global
    float x = 0, y = 0, z = 0;
    int   floor = 0;         // Spire floor (0 = not floor-bound)
    float discoverRadius = 8.0f;  // proximity (m, XZ; Y within +-6 m) that discovers it
    bool  fastTravel = true;      // is this a fast-travel anchor once discovered?
};

struct MapPoiTable {
    std::vector<MapPoi> pois;
    bool load(const std::string& path, std::vector<std::string>& errors);
    int  indexOf(std::string_view id) const;   // -1 if absent
    bool empty() const { return pois.empty(); }
};

// Canonical POIs path (next to regions.json), with the same machine fallbacks.
std::string worldMapPoisJsonPath();

// Discovery flags. "poi.<id>.found" rides the SAME StoryFlags the dialog/
// mission systems persist, so discovery saves with the rest of the story.
std::string poiFoundFlag(const std::string& poiId);
std::string regionSeenFlag(const std::string& regionId);
bool poiDiscovered(const StoryFlags& flags, const MapPoi& poi);

// ---------------------------------------------------------------------------
// Fast-travel gating.
// ---------------------------------------------------------------------------
enum class FastTravelGate : uint8_t {
    Ok = 0,
    NotAnAnchor,    // poi.fastTravel == false
    Undiscovered,   // poi not found yet
    Alert,          // "alert.active" StoryFlag set (the hook — no alert system here)
    Mission,        // current mission stage says no_fasttravel
};
FastTravelGate fastTravelGate(const MapPoi& poi, const StoryFlags& flags,
                              bool missionBlocksTravel);
const char* fastTravelGateText(FastTravelGate g);

// ---------------------------------------------------------------------------
// Waypoint (one, v1).
// ---------------------------------------------------------------------------
struct Waypoint {
    bool  active = false;
    float x = 0.0f, z = 0.0f;
    int   floor = 0;     // 0 = surface/any
};

// ---------------------------------------------------------------------------
// Route overlays (road-network worlds).
//
// A road world (--world tunnel) has no region ledgers to bake tiles from; its
// map content IS the road network — polylines the host already owns (RoadSpec
// centrelines, TunnelRoute spines). Each overlay is a world-XZ centreline the
// map screen draws as a dark asphalt line with a lighter outline; `dashed`
// marks reaches that run under something else (tunnel bores, bridge decks).
// Purely additive: worlds that never call setRouteOverlays draw exactly as
// before.
// ---------------------------------------------------------------------------
struct MapRouteOverlay {
    std::string name;
    std::vector<float> x, z;   // centreline nodes, world XZ (same length, >= 2)
    bool  dashed = false;      // bored / decked reach
    float widthM = 26.8f;      // drawn width, metres (the 88 ft paved section)
};

// ---------------------------------------------------------------------------
// Point markers (road-network worlds) — tunnel portals, the LNSS garage, and
// similar fixed landmarks that aren't POI-table entries (a road world has no
// StoryFlags fog of war, so these are always visible once registered; no
// discovery gating). Purely additive, like the route overlays: worlds that
// never call setMapMarkers draw exactly as before.
// ---------------------------------------------------------------------------
struct MapMarker {
    std::string label;      // display name (hover / zoomed-in)
    std::string type;       // icon class: "portal" | "garage"
    float x = 0.0f, z = 0.0f;
};

// ---------------------------------------------------------------------------
// Tile bake — CPU top-down rasterization into RGBA8 (uploaded via createTexture).
// ---------------------------------------------------------------------------
struct MapTile {
    x3::rhi::TextureHandle tex{};
    float wx0 = 0, wz0 = 0, wx1 = 0, wz1 = 0;   // world rect the texture covers
    uint32_t res = 0;                           // square pixel resolution
    bool baked = false;
};

// Blueprint-bake one parsed LevelDoc floor: room floors filled (depth-banded —
// rooms deeper below the floor's median Y tint darker), 2px bright wall
// perimeters, doorway openings cut through the shared walls. Background is
// transparent. `outRgba` is resized to res*res*4.
void bakeFloorTilePixels(const CanonFloor& floor, std::vector<uint8_t>& outRgba,
                         uint32_t res, float wx0, float wz0, float wx1, float wz1);

// Low-contrast, DESATURATED hypsometric terrain bake for the road-network
// map underlay: app/terrain.h's terrainHeightAtWorld() drives a charcoal-
// green -> olive -> pale-stone elevation gradient, darkened by slope (read
// back from the ALREADY-SAMPLED height buffer's neighbors — one
// terrainHeightAtWorld() call per pixel, not a second terrainNormalAtWorld()
// call — see the .cpp for why that halves the bake's query count); pixels
// below the river/ocean datum (worldWaterLevelAt() != kWorldWaterDry) paint
// flat pale blue-gray water instead. Deliberately muted: the road polylines
// drawn OVER this are the map's brightest layer by design (GTA figure-
// ground) and must never have to compete with the ground. `outRgba` is
// resized to res*res*4.
void bakeTerrainTilePixels(std::vector<uint8_t>& outRgba, uint32_t res,
                           float wx0, float wz0, float wx1, float wz1);

// Stamp the road network INTO an already-baked terrain tile buffer (casing +
// bright core, dashed bores, same palette as drawRouteOverlays — the two are
// PAIRED, see the color constants at both sites). Why baked at all when
// drawRouteOverlays exists: the per-frame HUD path stamps quads into the
// device's ~4096-quad/frame vertex ring, and the WHOLE 46-mile network at
// world-overview zoom needs more stamps than the ring holds — the casing pass
// ate the budget and the bright cores silently vanished, which is exactly the
// owner's "I just cant see ANYTHING on the MAP" blank-network overview.
// Texture pixels cost no ring space, so the baked layer carries the network at
// low zoom; drawRouteOverlays takes over above kRouteOverlayMinScale where the
// visible (bounded) subset fits the ring and draws crisper at true width.
void overlayRoadsOntoTilePixels(std::vector<uint8_t>& rgba, uint32_t res,
                                float wx0, float wz0, float wx1, float wz1,
                                const std::vector<MapRouteOverlay>& routes);

// Rasterize live scene entities (a region's ownership ledger) by their world
// AABBs (meshBounds x transform): height-banded tint (low = dark floor teal,
// tall = bright wall cyan), painter's order by top Y, 1px darker rim. Entities
// whose XZ footprint covers > half the tile (ground slabs) paint as the base
// ground tone first. Returns the number of footprints rasterized.
uint32_t bakeEntityTilePixels(const Scene& scene, x3::rhi::IRenderDevice& device,
                              const std::vector<uint32_t>& entities,
                              std::vector<uint8_t>& outRgba, uint32_t res,
                              float wx0, float wz0, float wx1, float wz1,
                              float yMin, float yMax);

// ---------------------------------------------------------------------------
// WorldMapSystem — POIs + discovery + waypoint + fast travel + tiles + the
// full-screen map screen. One instance per world host.
// ---------------------------------------------------------------------------
class WorldMapSystem {
public:
    // `poisPath` -> MapPoiTable (missing file = empty table, logged, not fatal).
    // `spireLevelDocPath` ("" = none) names the canonical project JSON whose
    // floors back the Spire floor tiles + the floor selector.
    bool init(const std::string& poisPath, const std::string& spireLevelDocPath);

    // Release baked tile textures (call before device shutdown).
    void shutdown(x3::rhi::IRenderDevice& device);

    // ---- Discovery (call each sim tick; cheap) -----------------------------
    // Proximity-discovers POIs (XZ radius, |dy| <= 6 m) -> "poi.<id>.found".
    void discoveryTick(StoryFlags& flags, float px, float py, float pz);

    const MapPoiTable& pois() const { return m_pois; }

    // ---- Waypoint ----------------------------------------------------------
    const Waypoint& waypoint() const { return m_waypoint; }
    void setWaypoint(float x, float z, int floor);
    void clearWaypoint() { m_waypoint = Waypoint{}; }

    // ---- Route overlays (road worlds; see MapRouteOverlay) -----------------
    void setRouteOverlays(std::vector<MapRouteOverlay> routes) { m_routes = std::move(routes); }
    const std::vector<MapRouteOverlay>& routeOverlays() const { return m_routes; }

    // ---- Point markers (road worlds; see MapMarker) ------------------------
    void setMapMarkers(std::vector<MapMarker> markers) { m_markers = std::move(markers); }
    const std::vector<MapMarker>& mapMarkers() const { return m_markers; }

    // ---- Terrain underlay (road-network worlds; see bakeTerrainTilePixels) -
    // Bake-or-fetch the low-contrast hypsometric tile covering the registered
    // route overlays' bounds (padded). Cached — bakes ONCE, on first call
    // (measured cost logged at `[worldmap] terrain underlay baked...`).
    // Returns null until setRouteOverlays has real geometry.
    const MapTile* ensureTerrainTile(x3::rhi::IRenderDevice& device);
    void invalidateTerrainTile(x3::rhi::IRenderDevice& device);

    // Below this camera scale (px per metre) the BAKED terrain+roads tile
    // carries the road network and drawRouteOverlays draws nothing; at or
    // above it the per-frame overlay pass draws the (bounded) visible subset
    // crisp at true width. One constant, two consumers — drawScreen's tile
    // pick and drawRouteOverlays' early-out MUST agree or the network
    // double-draws (halo) or vanishes at the boundary.
    static constexpr float kRouteOverlayMinScale = 0.20f;

    // ---- Fast travel (host polls after drawScreen) -------------------------
    bool travelRequested() const { return m_travelRequested; }
    const MapPoi* travelTarget() const;     // valid while travelRequested()
    void clearTravelRequest() { m_travelRequested = false; m_travelPoi = -1; }
    // The gating check the screen used (exposed for the host + self-test).
    FastTravelGate travelGate(int poiIndex, const StoryFlags& flags,
                              bool missionBlocksTravel) const;

    // ---- Spire floors ------------------------------------------------------
    int  floorCount() const { return (int)m_floors.size(); }
    int  selectedFloor() const { return m_selFloor; }
    void selectFloor(int floorNum);
    // The floor whose median room Y is nearest `y` (1-based floor numbers; 0 if
    // no floors are loaded). The floor-slice selection logic the test asserts.
    int  floorForY(float y) const;

    // ---- Tiles -------------------------------------------------------------
    // Bake-or-fetch the Spire tile for `floorNum` (parses the LevelDoc floor on
    // first use; cached). Returns null if the doc/floor is unavailable.
    const MapTile* ensureSpireTile(x3::rhi::IRenderDevice& device, int floorNum);
    // Bake-or-fetch a code-built region's tile from its live ledger entities.
    const MapTile* ensureRegionTile(x3::rhi::IRenderDevice& device, const Scene& scene,
                                    const std::string& regionId,
                                    const std::vector<uint32_t>& entities,
                                    float wx0, float wz0, float wx1, float wz1,
                                    float yMin, float yMax);
    const MapTile* regionTile(const std::string& regionId) const;
    // Hot-reload hooks: drop the baked tiles so the next map open rebakes.
    void invalidateSpireTiles(x3::rhi::IRenderDevice& device);
    void invalidateRegionTile(x3::rhi::IRenderDevice& device, const std::string& regionId);

    // ---- The map screen ----------------------------------------------------
    // Open the map centered on the player (auto-selects the player's floor).
    void open(float playerX, float playerY, float playerZ, float vpW, float vpH);
    void close() { m_open = false; }
    bool isOpen() const { return m_open; }
    MapCamera& camera() { return m_cam; }
    const MapCamera& camera() const { return m_cam; }
    bool confirmOpen() const { return m_confirmPoi >= 0; }
    // Test/screenshot seams: drive the confirm prompt + hover without a mouse.
    void openConfirm(int poiIndex) { m_confirmPoi = poiIndex; }
    void closeConfirm() { m_confirmPoi = -1; }

    struct ScreenInput {
        float mouseX = 0, mouseY = 0;
        bool  mouseDown = false, mousePressed = false;
        float wheel = 0.0f;                     // scroll notches this frame
        bool  keyW = false, keyA = false, keyS = false, keyD = false;
        bool  keyQ = false, keyE = false;       // MAP ROTATION (W-MAP v3) — held, not edge
        bool  enterEdge = false, escEdge = false;
        float playerX = 0, playerY = 0, playerZ = 0, playerYaw = 0;
        int   compCount = 0;
        const float* compX = nullptr; const float* compZ = nullptr;   // companions
        bool  objValid = false; float objX = 0, objZ = 0;   // live objective marker
        bool  missionBlocksTravel = false;
        const char* locationName = nullptr;     // header (current region display name)
    };

    // Draw + interact for one frame while open. Caller has ui.begin()'d the
    // frame. Reads/writes waypoint + discovery flags; may set travelRequested.
    void drawScreen(x3::ui::UiContext& ui, x3::rhi::IRenderDevice& device,
                    const x3::rhi::FrameContext& frame, const ScreenInput& in,
                    StoryFlags& flags, float dt);

private:
    struct SpireFloor {
        int        num = 0;
        CanonFloor floor;
        float      medianY = 0.0f;
        MapTile    tile;
    };
    int spireFloorIndex(int floorNum) const;
    void drawPoiIcon(x3::ui::UiContext& ui, const MapPoi& poi, float px, float py,
                     bool hovered, float t) const;
    void drawRouteOverlays(x3::ui::UiContext& ui, float W, float H) const;
    // Route names, sparse + zoom-gated (fade in above a threshold): one label
    // per unique MapRouteOverlay::name, at the on-screen node closest to the
    // viewport center — so the name "travels" with the view like a GTA road
    // label instead of clumping every reach's label onto one spot.
    void drawRouteLabels(x3::ui::UiContext& ui, float W, float H) const;
    void drawMapMarker(x3::ui::UiContext& ui, const MapMarker& mk, float px, float py,
                       bool hovered) const;
    // N/E/S/W compass rose (W-MAP v3, Q/E rotation). The letters COUNTER-
    // ROTATE around the rose center as the map rotates — they stay upright
    // (HUD text can't be drawn rotated) but their POSITIONS track the same
    // rotation worldToPx applies to everything else, so "N" is always over
    // true +Z regardless of how the view is spun. Native compass: +Z north
    // (CLAUDE.md axes), +X east.
    void drawCompassRose(x3::ui::UiContext& ui, float W, float H) const;

    MapPoiTable m_pois;
    std::string m_spireDocPath;
    std::vector<SpireFloor> m_floors;
    float m_spireRect[4] = {0, 0, 0, 0};   // shared world rect (x0,z0,x1,z1) all floors

    struct RegionTileEntry { std::string id; MapTile tile; };
    std::vector<RegionTileEntry> m_regionTiles;

    std::vector<MapRouteOverlay> m_routes;   // road-network worlds (else empty)
    std::vector<MapMarker>       m_markers;  // portals / garage / landmarks (else empty)
    MapTile m_terrainTile;                   // road-network underlay (see ensureTerrainTile)
    MapTile m_terrainRoadsTile;              // same tile WITH the network baked in — drawn
                                             // below kRouteOverlayMinScale (one height pass
                                             // bakes both; see ensureTerrainTile)

    Waypoint m_waypoint;
    bool m_open = false;
    MapCamera m_cam;
    int  m_selFloor = 1;
    int  m_confirmPoi = -1;        // POI index the confirm prompt is up for
    bool m_travelRequested = false;
    int  m_travelPoi = -1;
    float m_pulse = 0.0f;          // marker pulse clock
    float m_dragLastX = 0.0f, m_dragLastY = 0.0f;
    bool  m_dragging = false;
    float m_dragMoved = 0.0f;      // px moved while held (click-vs-drag)
};

// Headless self-test (--test-worldmap): POI discovery + flags round-trip,
// waypoint set/clear, fast-travel gates (discovery / alert flag / mission
// stage) + the streaming teleport (region realizes or proxies; ledger sane),
// zoom/pan lerp convergence + the cursor-anchored zoom invariant, floor-slice
// selection, and the tile bakes (floor pixels + entity AABB pixels non-empty,
// correct banding). Logs PASS/FAIL per check; true iff all pass. No window.
bool runWorldMapSelfTest();

} // namespace x3::game
