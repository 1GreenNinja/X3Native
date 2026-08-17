// --world cutaway — the LEVEL ARCHITECT CUTAWAY VIEW host.
//
// The whole facility as a stack of translucent floors, orbited from outside,
// with hover cards, per-floor + per-structure visibility, an opacity dial and a
// live readout. See app/cutaway.h for the design and the reference captures at
// docs/design/LEVEL_ARCHITECT_CUTAWAY_REF{,2,3}.png.
//
// The host is deliberately thin: it owns input, the low-key lighting state and
// the frame loop; CutawayView owns the model, the meshes, the camera and every
// pixel of the panel. No physics world is created (nothing here collides), no
// gameplay is spawned, no level is built into a Scene — this is a READ of the
// canonical project.
//
// HEADLESS: --screenshot-cutaway <dir> writes the proof set (the stacked view,
// the upper floors hidden, a hover card) through the SAME CutawayView the
// windowed path uses, so a capture cannot show something the tool doesn't.

#include "world_host_common.h"
#include "host_shell.h"                  // console (~), menu (ESC), FPS (F3)
#include "../cutaway.h"
#include "../input_globals.h"            // g_weaponScroll + scrollCallback (the shared wheel)

#include <filesystem>

using x3::game::kNoRoom;

namespace x3 { namespace apphost {

namespace {

// THE EXPOSURE, and why it is this low.
//
// The renderer's HDR colour attachment clears to a FIXED dark slate
// (0.04, 0.05, 0.08 linear, vk_graph.cpp) which ACES maps to a mid blue-grey at
// exposure 1 — the first capture of this view had a pale sky-blue background
// where the reference has near-black. That clear is engine-wide and shared by
// every world, so the honest lever for ONE low-key view is the exposure, not a
// renderer edit: at 0.10 the background falls to ~13/255 and the emitters are
// authored hot to compensate.
//
// PAIRED VALUE (NO_SLOP rule 4): every emissive strength in
// CutawayView::render() is quoted PRE-exposure against this number. Move this
// and the whole schematic goes dark or blows out — change both together.
constexpr float kCutawayExposure = 0.10f;

// The low-key, unlit look the reference has: no sky, near-black ambient, a
// FIXED exposure (auto-exposure would gain a mostly-black frame up until the
// emissive plates blew out) and just enough bloom to bleed the edge cages.
void applyCutawayLook(x3::rhi::IRenderDevice& device) {
    x3::rhi::IRenderDevice::SkyParams sp{};
    sp.enabled = false;                       // pure black behind the building
    device.setSkyParams(sp);
    device.setAmbient(0.012f, 0.014f, 0.020f);
    device.setPointLights(nullptr, 0);
    x3::rhi::IRenderDevice::SsaoParams ao{};
    ao.enabled = false;                       // meaningless on translucent shells
    device.setSsaoParams(ao);
    x3::rhi::IRenderDevice::GiParams gi{};
    gi.enabled = false;
    device.setGiParams(gi);
    x3::rhi::IRenderDevice::PostFXParams px{};
    px.autoExposure = false;                  // see above — the important one
    px.bloomEnabled = true;
    device.setPostFX(px);
    device.setBloom(0.22f);
    device.setExposure(kCutawayExposure);
}

} // namespace

int hostCutaway(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;

    const std::string jsonPath = x3::game::canonProjectJsonPath();
    x3::logInfo("--world cutaway: reading the canonical project at " + jsonPath);

    x3::game::CutawayView view;
    if (!view.build(*device, jsonPath)) {
        x3::logError("--world cutaway: the canonical project failed to load — nothing to show");
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    applyCutawayLook(*device);

    // ===== HEADLESS PROOF SET =============================================
    if (hc.headless) {
        namespace fs = std::filesystem;
        const std::string dir = hc.cutawayShotDir.empty()
                              ? std::string("docs/screenshots/cutaway") : hc.cutawayShotDir;
        std::error_code ec; fs::create_directories(dir, ec);

        struct Shot { const char* file; int mode; };
        // mode 0 = the stack as the reference frames it
        // mode 1 = the upper floors hidden (the actual "cut away")
        // mode 2 = a hover card up, over the Neural Interface Lab
        const Shot shots[] = {
            { "cutaway_stack.png",  0 },
            { "cutaway_cut.png",    1 },
            { "cutaway_card.png",   2 },
        };
        bool allOk = true;
        for (const Shot& s : shots) {
            view.showAllBands();
            view.setOpacity(0.12f);
            view.frameTower();
            uint32_t hover = kNoRoom;

            if (s.mode == 1) {
                // Hide F4..F7 + the sub-level: the lower decks are then read
                // straight down into, which is the whole point of a cutaway.
                for (uint32_t i = 3; i < view.model().bands.size(); ++i) view.toggleBand(i);
            }
            if (s.mode == 2) {
                hover = view.model().canon.roomByName("Neural Interface Lab");
                view.setOpacity(0.10f);
            }

            const std::string out = dir + "/" + s.file;
            // Settle a few frames so TAA/bloom converge before the grab.
            const int kFrames = 12;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                view.applyCamera(*device);
                if (i == kFrames - 1) device->armCapture(out.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    view.render(*device, frame);
                    view.drawUi(*device, frame, hover);
                }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(out.c_str());
            if (wrote) x3::logInfo("--screenshot-cutaway: wrote " + out);
            else     { x3::logError("--screenshot-cutaway: FAILED " + out); allOk = false; }
        }
        view.shutdown(*device);
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return allOk ? 0 : 1;
    }

    // ===== WINDOWED: orbit, toggle, hover ==================================
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);   // it's a TOOL, not a shooter
    double lastMX = 0, lastMY = 0;
    glfwGetCursorPos(window, &lastMX, &lastMY);

    // Install the shared wheel callback BEFORE attaching the shell: the shell
    // chains to whatever scroll callback it finds, so this order is what makes
    // the wheel reach BOTH the console scrollback and the dolly.
    glfwSetScrollCallback(window, scrollCallback);

    HostShell shell;
    shell.attach(hc);
    // Console dials, so every panel control is also reachable by name while
    // running (NO_SLOP rule 6: a feature behind a key nobody presses is a
    // feature nobody has). The panel and these read/write the SAME state.
    if (auto* con = shell.console()) {
        shell.addFloatCommand("cut_opacity", "cutaway: glass opacity 0.02..1",
                              [&](float v) { view.setOpacity(v); });
        shell.addToggleCommand("cut_solid", "cutaway: Solid View (opacity 1)",
                               [&] { return view.solid; }, [&](bool b) { view.solid = b; });
        shell.addToggleCommand("cut_plates", "cutaway: per-room floor plates",
                               [&] { return view.plates; }, [&](bool b) { view.plates = b; });
        shell.addToggleCommand("cut_wire", "cutaway: per-room edge cages",
                               [&] { return view.cages; }, [&](bool b) { view.cages = b; });
        shell.addToggleCommand("cut_doors", "cutaway: doorway markers",
                               [&] { return view.doors; }, [&](bool b) { view.doors = b; });
        shell.addToggleCommand("cut_planes", "cutaway: full-footprint floor planes",
                               [&] { return view.planes; }, [&](bool b) { view.planes = b; });
        shell.addToggleCommand("cut_envelope", "cutaway: the steel frame + exterior skin",
                               [&] { return view.envelope; }, [&](bool b) { view.envelope = b; });
        shell.addToggleCommand("cut_panel", "cutaway: the tool panel",
                               [&] { return view.panel; }, [&](bool b) { view.panel = b; });
        shell.addFloatCommand("cut_floor", "cutaway: toggle floor band N (1-based)",
                              [&](float v) { view.toggleBand((uint32_t)v - 1); });
        con->registerCommand("cut_all", [&](const std::vector<std::string>&) {
            view.showAllBands();
        }, "cutaway: show every floor + structure again");
    }

    x3::logInfo("--world cutaway: drag=orbit  wheel=zoom  MMB=pan  1-9=floors  "
                "shift+1-9=solo  ctrl+1-9=structures  [ ]=opacity  V/N/P/G/B/X  F=frame all  "
                "C=frame tower  0=show all  H=panel  L=legend");

    bool prevNum[10] = {}, prevKey[10] = {};
    bool lmbPrev = false, mmbPrev = false;
    uint32_t hovered = kNoRoom;

    while (!glfwWindowShouldClose(window) && !shell.wantQuit()) {
        glfwPollEvents();
        shell.beginFrame();

        int cw = 0, chh = 0;
        glfwGetFramebufferSize(window, &cw, &chh);
        if (cw > 0 && chh > 0) device->onResize((uint32_t)cw, (uint32_t)chh);

        double mx = 0, my = 0;
        glfwGetCursorPos(window, &mx, &my);
        const float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
        lastMX = mx; lastMY = my;

        const bool uiFree = shell.inputEnabled();
        const bool lmb = uiFree && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        const bool mmb = uiFree && (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS ||
                                    glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);
        if (lmb && lmbPrev) view.orbitDrag(ddx, ddy);
        if (mmb && mmbPrev) view.pan(ddx, ddy);
        lmbPrev = lmb; mmbPrev = mmb;

        if (g_weaponScroll != 0.0 && uiFree) view.dolly((float)g_weaponScroll);
        g_weaponScroll = 0.0;

        auto kd = [&](int k) { return shell.key(k); };
        const bool shiftDown = kd(GLFW_KEY_LEFT_SHIFT) || kd(GLFW_KEY_RIGHT_SHIFT);
        const bool ctrlDown  = kd(GLFW_KEY_LEFT_CONTROL) || kd(GLFW_KEY_RIGHT_CONTROL);
        for (int n = 1; n <= 9; ++n) {
            const bool now = kd(GLFW_KEY_0 + n);
            if (now && !prevNum[n]) {
                const uint32_t i = (uint32_t)(n - 1);
                if (ctrlDown)      view.toggleGroup(i);
                else if (shiftDown) view.soloBand(i);
                else                view.toggleBand(i);
            }
            prevNum[n] = now;
        }
        { const bool now = kd(GLFW_KEY_0); if (now && !prevNum[0]) view.showAllBands(); prevNum[0] = now; }

        auto edge = [&](int slot, int key) {
            const bool now = kd(key); const bool fired = now && !prevKey[slot];
            prevKey[slot] = now; return fired;
        };
        if (edge(0, GLFW_KEY_V)) view.solid    = !view.solid;
        if (edge(1, GLFW_KEY_P)) view.plates   = !view.plates;
        if (edge(2, GLFW_KEY_G)) view.cages    = !view.cages;
        if (edge(3, GLFW_KEY_B)) view.doors    = !view.doors;
        if (edge(4, GLFW_KEY_H)) view.panel    = !view.panel;
        if (edge(5, GLFW_KEY_L)) view.legend   = !view.legend;
        if (edge(6, GLFW_KEY_F)) view.frameAll();
        if (edge(7, GLFW_KEY_C)) view.frameTower();
        if (edge(8, GLFW_KEY_N)) view.planes   = !view.planes;
        if (edge(9, GLFW_KEY_X)) view.envelope = !view.envelope;
        if (kd(GLFW_KEY_LEFT_BRACKET))  view.setOpacity(view.opacity() - 0.4f * shell.frameDt());
        if (kd(GLFW_KEY_RIGHT_BRACKET)) view.setOpacity(view.opacity() + 0.4f * shell.frameDt());

        // HOVER — the ray is fired from the SAME camera the frame renders with.
        hovered = uiFree ? view.pickRoom((float)mx, (float)my, (uint32_t)cw, (uint32_t)chh)
                         : kNoRoom;

        view.applyCamera(*device);
        auto frame = device->beginFrame();
        if (frame.valid) {
            view.render(*device, frame);
            view.drawUi(*device, frame, hovered);
        }
        shell.draw(frame);
        device->endFrame(frame);
    }

    view.shutdown(*device);
    device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost
