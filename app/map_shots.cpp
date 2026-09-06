// map_shots.cpp — see map_shots.h.
#include "map_shots.h"
#include "world_map.h"
#include "cutscene.h"     // StoryFlags
#include "ui.h"
#include "engine/core/x3_log.h"
#include "engine/rhi/IRenderDevice.h"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <filesystem>

namespace x3::game {

bool runWorldMapShotSequence(WorldMapSystem& map, x3::rhi::IRenderDevice& device,
                             GLFWwindow* window, StoryFlags& flags,
                             const WorldMapShotParams& p) {
    namespace fs = std::filesystem;
    std::error_code ec; fs::create_directories(p.outDir, ec);

    // The explored map: every POI found, every region but the ocean base seen.
    for (const MapPoi& pp : map.pois().pois) {
        flags.set(poiFoundFlag(pp.id));
        if (!pp.region.empty() && pp.region != "ocean_base") flags.set(regionSeenFlag(pp.region));
    }
    for (const char* r : { "spire_f1", "city", "surface_landmarks" }) flags.set(regionSeenFlag(r));

    x3::ui::UiContext mui;
    const float fbw = (float)p.fbW, fbh = (float)p.fbH;
    struct Shot { const char* png; float cx, cz, scale, rot; };
    const Shot shots[] = {
        { "after_overview.png",  150.0f,  -50.0f, 0.18f, 0.0f },
        { "after_district.png", -150.0f,  420.0f, 1.10f, 0.0f },
        { "after_street.png",    195.0f,  360.0f, 10.0f, 0.0f },
        { "after_rotated.png",     0.0f,  300.0f, 0.80f, 0.61f },
    };
    bool all = true;
    for (const Shot& s : shots) {
        map.open(p.playerX, p.playerY, p.playerZ, fbw, fbh);
        map.camera().jumpTo(s.cx, s.cz, s.scale);
        map.camera().scale = s.scale;   // no lerp-in: the still is the settled view
        map.camera().rot = map.camera().tRot = s.rot;
        // Block on the bakes this view wants (overview + a detail tile at this
        // zoom), so the still shows the finished tiles, not the in-flight state.
        map.atlasFlush(device);
        const std::string path = (fs::path(p.outDir) / s.png).string();
        for (int i = 0; i < 4; ++i) {
            glfwPollEvents();
            if (i == 3) device.armCapture(path.c_str());
            auto f = device.beginFrame();
            if (f.valid) {
                x3::ui::UiInput ui0{};
                ui0.mouseX = fbw * 0.62f; ui0.mouseY = fbh * 0.40f;
                mui.begin(device, f, ui0);
                WorldMapSystem::ScreenInput msi{};
                msi.mouseX = ui0.mouseX; msi.mouseY = ui0.mouseY;
                msi.playerX = p.playerX; msi.playerY = p.playerY; msi.playerZ = p.playerZ;
                msi.playerYaw = p.playerYaw;
                msi.locationName = p.locationName;
                map.drawScreen(mui, device, f, msi, flags, 1.0f / 60.0f);
                mui.end();
            }
            device.endFrame(f);
            map.atlasFlush(device);   // a detail kick from this frame lands before the capture
        }
        const bool ok = device.captureFrame(path.c_str());
        if (ok) x3::logInfo("--screenshot-worldmap: wrote " + path);
        else    x3::logError("--screenshot-worldmap: capture FAILED: " + path);
        all &= ok;
        // Label receipts (what the collision pass actually placed).
        {
            std::string ls = "--screenshot-worldmap: labels:";
            for (const WorldMapSystem::PlacedLabel& l : map.lastLabels()) {
                char b[160];
                std::snprintf(b, sizeof(b), " [%s @%.0f,%.0f %gx%g]", l.text.c_str(), l.x, l.y, l.w, l.h);
                ls += b;
            }
            x3::logInfo(ls);
        }
        // Bake receipts for the report.
        if (const MapBakeStats* st = map.overviewBakeStats())
            x3::logInfo("--screenshot-worldmap: overview bake " + std::to_string((int)st->totalMs) + " ms");
        if (const MapBakeStats* st = map.detailBakeStats())
            x3::logInfo("--screenshot-worldmap: detail bake " + std::to_string((int)st->totalMs) + " ms @ " +
                        std::to_string(s.scale) + " px/m");
    }
    (void)window;
    map.close();
    return all;
}

} // namespace x3::game
