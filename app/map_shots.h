// ===========================================================================
// map_shots.h — the canonlevel world-map still sequence (W-MAP v4).
//
// `--screenshot-worldmap --world canonlevel` (headless): after the canon world
// has booted and the map is bound (WorldMapSystem::bindWorld), render the map
// screen at the four review zooms and write them to docs/screenshots/worldmap/:
//   after_overview.png   the world: sea, river, city, freeway, Spire
//   after_district.png   the city districts + the freeway interchange
//   after_street.png     max zoom on the dealership block
//   after_rotated.png    the district view spun 35 deg (Q/E) — compass + labels
// Lives in its own file so the host (app_run.cpp) carries one call.
// ===========================================================================
#pragma once

#include <string>

struct GLFWwindow;
namespace x3::rhi { class IRenderDevice; }

namespace x3::game {

class WorldMapSystem;
class StoryFlags;

struct WorldMapShotParams {
    std::string outDir = "docs/screenshots/worldmap";
    float playerX = 170.0f, playerY = 0.0f, playerZ = 380.0f, playerYaw = 1.2f;
    const char* locationName = "NEW DISTRICT";
    uint32_t fbW = 1920, fbH = 1080;
};

// Renders + writes the four stills. Every POI is pre-discovered and every
// region but the ocean base marked seen (so one fog disc is on record).
// Returns true when every capture wrote.
bool runWorldMapShotSequence(WorldMapSystem& map, x3::rhi::IRenderDevice& device,
                             GLFWwindow* window, StoryFlags& flags,
                             const WorldMapShotParams& p);

} // namespace x3::game
