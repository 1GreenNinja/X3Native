// X3Engine host — opens a window, brings up the render device + physics, builds
// the S2 graybox test level, and runs the loop with a fly camera. Walking is S3.

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include "engine/core/x3_log.h"
#include "engine/core/IConsole.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/asset/IAssetSource.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/asset/IModelLoader.h"
#include "engine/audio/IAudioSystem.h"

#include "mesh_prims.h"
#include "scene.h"
#include "level.h"
#include "player.h"
#include "door.h"
#include "weapon.h"
#include "monster.h"
#include "fx.h"
#include "hud.h"

#include <memory>
#include <string_view>
#include <string>
#include <cmath>
#include <vector>
#include <cstdint>

namespace {
// Approximate the viewmodel muzzle in world space from the eye + look angles, so
// the FX tracer starts near the gun barrel (lower-right of the view) rather than
// dead center. Mirrors the camera-basis offsets used by WeaponSystem; tuned to
// sit just in front of and below/right of the eye.
x3::phys::Vec3 muzzleFromCamera(float ex, float ey, float ez, float yaw, float pitch) {
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    const x3::phys::Vec3 forward{ cp * cy, sp, cp * sy };
    const x3::phys::Vec3 right{ -sy, 0.0f, cy };
    const x3::phys::Vec3 up{
        right.y * forward.z - right.z * forward.y,
        right.z * forward.x - right.x * forward.z,
        right.x * forward.y - right.y * forward.x };
    // Muzzle offset (m) in the camera basis: a bit forward, a bit right + down.
    const float mFwd = 0.6f, mRight = 0.18f, mDown = 0.12f;
    return x3::phys::Vec3{
        ex + forward.x * mFwd + right.x * mRight - up.x * mDown,
        ey + forward.y * mFwd + right.y * mRight - up.y * mDown,
        ez + forward.z * mFwd + right.z * mRight - up.z * mDown };
}

// Bundle passed to GLFW via the window user-pointer so the char/key callbacks
// can route text input into the on-screen console.
struct InputContext {
    x3::game::Hud*       hud = nullptr;
    x3::con::IConsole*   console = nullptr;
};

// ---- Live-tunable viewmodel pose (FIX 1) ----------------------------------
// The held-gun pose is exposed as console cvars so the player can dial the
// barrel onto the crosshair in-game, then bake the values as defaults. Angles
// are entered in DEGREES in the console (intuitive) and converted to radians
// before being handed to WeaponSystem::drawViewmodel(); placement is in meters.
struct VmPose { float yawRad, pitchRad, rollRad, fwd, right, down; };

constexpr float kDegToRad = 3.14159265358979f / 180.0f;

// Register the six viewmodel cvars, seeded with the baked defaults (weapon.h).
void registerViewmodelCVars(x3::con::IConsole& console) {
    console.registerCVar("vm_yaw",   std::to_string(x3::game::kVmDefYawDeg),
                         "viewmodel yaw offset about camera up (degrees)");
    console.registerCVar("vm_pitch", std::to_string(x3::game::kVmDefPitchDeg),
                         "viewmodel pitch offset about camera right (degrees)");
    console.registerCVar("vm_roll",  std::to_string(x3::game::kVmDefRollDeg),
                         "viewmodel roll offset about camera forward (degrees)");
    console.registerCVar("vm_fwd",   std::to_string(x3::game::kVmDefFwd),
                         "viewmodel offset forward along the look dir (meters)");
    console.registerCVar("vm_right", std::to_string(x3::game::kVmDefRight),
                         "viewmodel offset to the right (meters)");
    console.registerCVar("vm_down",  std::to_string(x3::game::kVmDefDown),
                         "viewmodel offset below the eye line (meters)");
}

// Read the current cvar values, converting the angle cvars degrees->radians.
VmPose readViewmodelPose(const x3::con::IConsole& console) {
    return VmPose{
        console.getFloat("vm_yaw")   * kDegToRad,
        console.getFloat("vm_pitch") * kDegToRad,
        console.getFloat("vm_roll")  * kDegToRad,
        console.getFloat("vm_fwd"),
        console.getFloat("vm_right"),
        console.getFloat("vm_down"),
    };
}

// GLFW character callback: feed printable codepoints to the console input line.
void charCallback(GLFWwindow* win, unsigned int codepoint) {
    auto* ctx = static_cast<InputContext*>(glfwGetWindowUserPointer(win));
    if (ctx && ctx->hud) ctx->hud->onChar(codepoint);
}

// GLFW key callback: the '`'/'~' toggle (always), plus console editing keys when
// the console is open. Gameplay keys are polled in the loop and gated separately.
void keyCallback(GLFWwindow* win, int key, int /*scancode*/, int action, int /*mods*/) {
    auto* ctx = static_cast<InputContext*>(glfwGetWindowUserPointer(win));
    if (!ctx || !ctx->hud) return;
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
    x3::game::Hud& hud = *ctx->hud;

    if (key == GLFW_KEY_GRAVE_ACCENT) { if (action == GLFW_PRESS) hud.toggleConsole(); return; }
    if (!hud.consoleOpen()) return;

    switch (key) {
        case GLFW_KEY_ENTER:
        case GLFW_KEY_KP_ENTER: if (ctx->console) hud.onEnter(*ctx->console); break;
        case GLFW_KEY_BACKSPACE: hud.onBackspace(); break;
        case GLFW_KEY_UP:        hud.historyPrev(); break;
        case GLFW_KEY_DOWN:      hud.historyNext(); break;
        case GLFW_KEY_TAB:       if (ctx->console) hud.complete(*ctx->console); break;
        case GLFW_KEY_ESCAPE:    hud.closeConsole(); break;
        default: break;
    }
}
} // namespace

int main(int argc, char** argv) {
    bool smoketest = false, testAsset = false, testConsole = false, testPhysics = false,
         testGltf = false, testPlayer = false, testInteract = false, testPickup = false,
         testCombat = false, testAudio = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if (a == "--smoketest") smoketest = true;
        else if (a == "--test-asset") testAsset = true;
        else if (a == "--test-console") testConsole = true;
        else if (a == "--test-physics") testPhysics = true;
        else if (a == "--test-gltf") testGltf = true;
        else if (a == "--test-player") testPlayer = true;
        else if (a == "--test-interact") testInteract = true;
        else if (a == "--test-pickup") testPickup = true;
        else if (a == "--test-combat") testCombat = true;
        else if (a == "--test-audio") testAudio = true;
    }

    // Headless self-tests (no window / Vulkan needed)
    if (testAsset) {
        x3::logInfo("running asset (D5) self-test...");
        return x3::asset::runAssetSelfTest() ? 0 : 1;
    }
    if (testConsole) {
        x3::logInfo("running console (D6) self-test...");
        return x3::con::runConsoleSelfTest() ? 0 : 1;
    }
    if (testPhysics) {
        x3::logInfo("running physics (M3) self-test...");
        return x3::phys::runPhysicsSelfTest() ? 0 : 1;
    }
    if (testGltf) {
        x3::logInfo("running glTF/GLB model loader (M2) self-test...");
        return x3::asset::runModelLoaderSelfTest() ? 0 : 1;
    }
    if (testPlayer) {
        x3::logInfo("running player/character-controller (S3) self-test...");
        return x3::game::runPlayerSelfTest() ? 0 : 1;
    }
    if (testInteract) {
        x3::logInfo("running button->door interaction (S4) self-test...");
        return x3::game::runInteractSelfTest() ? 0 : 1;
    }
    if (testPickup) {
        x3::logInfo("running weapon pickup + arming (S5) self-test...");
        return x3::game::runPickupSelfTest() ? 0 : 1;
    }
    if (testCombat) {
        x3::logInfo("running shoot-monster combat (S6) self-test...");
        return x3::game::runCombatSelfTest() ? 0 : 1;
    }
    if (testAudio) {
        x3::logInfo("running audio (M9) self-test...");
        return x3::audio::runAudioSelfTest() ? 0 : 1;
    }

    x3::logInfo("X3Engine starting...");

    if (!glfwInit()) {
        x3::logError("glfwInit failed");
        return 1;
    }
    // No OpenGL/GLES context — we drive Vulkan ourselves.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    const uint32_t W = 1280, H = 720;
    GLFWwindow* window = glfwCreateWindow(static_cast<int>(W), static_cast<int>(H),
                                          "X3Engine", nullptr, nullptr);
    if (!window) {
        x3::logError("glfwCreateWindow failed");
        glfwTerminate();
        return 1;
    }

    // ---- Render device ----
    std::unique_ptr<x3::rhi::IRenderDevice> device(x3::rhi::createRenderDevice());

    x3::rhi::DeviceDesc desc{};
    desc.nativeWindowHandle = glfwGetWin32Window(window);
    desc.width  = W;
    desc.height = H;
    desc.vsync  = true;
#ifdef _DEBUG
    desc.validation = true;
#else
    desc.validation = false;
#endif

    if (!device->init(desc)) {
        x3::logError("render device init failed");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // ---- Asset source (stub until D5) ----
    std::unique_ptr<x3::asset::IAssetSource> assets(x3::asset::createAssetSource());
    assets->mountPak("base.x3pak", 0);  // stub: logs not-implemented for now

    // ---- Audio system (M9 / miniaudio) ----
    // init() is GRACEFUL: on a machine with no audio device it logs a warning and
    // runs silently (all play calls become no-ops) — never crashes. We load REAL
    // purchased WAV/music by ABSOLUTE G:\ path (like the GLBs); nothing is copied
    // into the public repo. Missing files load() to invalid handles -> silent.
    std::unique_ptr<x3::audio::IAudioSystem> audio(x3::audio::createAudioSystem());
    audio->init();
    // Concrete asset picks (see docs/ASSET_INVENTORY.md). Hardcoded absolute paths
    // with graceful fallback: a missing/undecodable file -> invalid handle -> the
    // corresponding event is simply silent (logged once at load).
    const x3::audio::SoundHandle sndGun = audio->load(
        "G:/Unity_Projects/EscapeFromLabZero/Assets/Sci-Fi_Guns_Game-Of-Weapons/"
        "Audio/SFX/Wave/Single_Gunshots/Single_Gunshot_Sci-Fi_Gun-01.wav");
    const x3::audio::SoundHandle sndDoor = audio->load(
        "G:/Unity_Projects/EscapeFromLabZero/Assets/ModularScifiInterior/Sound/"
        "S_ScifiDoor_A.WAV");
    const x3::audio::SoundHandle sndPickup = audio->load(
        "G:/Unity_Projects/EscapeLab48/Escape Lab 48/Assets/"
        "Sci-fi Evolution Gift Pack/Health or Energy Game Recharge 2.wav");
    const x3::audio::SoundHandle sndDeath = audio->load(
        "G:/Unity_Projects/EscapeLab48/Escape Lab 48/Assets/Free Pack/Explosion 1.wav");
    // Footsteps reuse the gunshot WAV pitched down + quiet (no dedicated footstep
    // WAV in the inventory). It reads as a soft step; replace with a real footstep
    // SFX later if one is added to the pack.
    const x3::audio::SoundHandle sndStep = sndGun;
    // Absolute path for the looping music/ambient bed (started after the world is
    // built, below). Spaceship-ambience-style sci-fi action loop.
    constexpr const char* kMusicPath =
        "G:/Unity_Projects/EscapeLab48/Escape Lab 48/Assets/Sci-Fi Music Pack 1/"
        "Loops/SMP1_LOOP_Zero8 _1.wav";

    // ---- Physics world (M3 / Jolt) ----
    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    if (!physics->init()) {
        x3::logError("physics world init failed");
        device->shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // ---- Build the S2 graybox test level into the scene ----
    x3::game::Scene scene;
    x3::game::buildTestLevel(scene, *device, *physics);

    // ---- S4: a sliding door filling the doorway gap + a wall button linked to
    // it. Press E while aiming at the button to slide the door open. ----
    x3::game::DoorSystem doors;
    x3::game::buildDoorAndButton(scene, doors, *device, *physics);

    // ---- One dynamic box dropped above the floor: proves render+physics+entity
    // sync work together (it falls and rests on the floor). ----
    x3::rhi::TextureHandle boxTex;  // invalid => flat color via baseColor
    {
        x3::prims::PrimMesh boxGeo = x3::prims::makeBox(0.4f, 0.4f, 0.4f, 0,0,0, 1.0f);
        x3::game::Entity box;
        box.mesh = device->createMesh(boxGeo.verts.data(), (uint32_t)boxGeo.verts.size(),
                                      boxGeo.index.data(), (uint32_t)boxGeo.index.size());
        box.tex = boxTex;
        box.baseColor[0]=0.95f; box.baseColor[1]=0.35f; box.baseColor[2]=0.15f; box.baseColor[3]=1;
        // Dynamic body: 0.4 m half-extent box, mass 5 kg, dropped at y=4.
        const x3::phys::Vec3 dropPos{ 0.0f, 4.0f, 0.0f };
        box.body = physics->addBox(x3::phys::Vec3{0.4f,0.4f,0.4f}, dropPos, 5.0f,
                                   x3::phys::Layer::Dynamic);
        box.tag  = (uint32_t)x3::game::Tag::Prop;
        // Authored transform translation (overwritten each frame by Scene::update).
        box.transform[12] = dropPos.x;
        box.transform[13] = dropPos.y;
        box.transform[14] = dropPos.z;
        scene.add(box);
        x3::logInfo("dynamic box added at y=4.0 (will fall and rest on floor)");
    }

    // ---- S5: weapon pickup. Load the purchased WeaponEnergyPistol.glb (fallback
    // to a procedural box if it can't be loaded) and place a bobbing/spinning
    // pickup in open floor between the spawn and the doorway. Walk into it (within
    // ~1.2 m) to arm; the first-person viewmodel then appears. ----
    x3::game::WeaponSystem weapon;
    weapon.buildWeaponPickup(scene, *device, "G:/GameModels/rigged_glb",
                             x3::phys::Vec3{ 0.0f, 1.0f, 2.0f });

    // ---- S6: the monster to shoot. Load alien_crawler.glb (fallback to a
    // procedural box), give it an Enemy-layer collision body for the shoot
    // raycast, and place it on open floor on the far (-Z) side of the room so the
    // player walks in, grabs the gun near the origin, then turns and shoots it.
    // It starts with 100 HP; 34 dmg/shot => 3 shots to kill. ----
    x3::game::MonsterSystem combat;
    combat.buildMonster(scene, *device, *physics, "G:/GameModels/rigged_glb",
                        x3::phys::Vec3{ 0.0f, 0.4f, -4.0f });

    // ---- Combat FX (gameplay-feel pass): shot tracers + muzzle flash. The
    // crosshair now lives in the screen-space HUD layer (S7), not here. ----
    x3::game::CombatFx combatFx;
    combatFx.init(*device);

    // ---- S7: console backend (D6) + screen-space HUD (FPS, console, crosshair).
    std::unique_ptr<x3::con::IConsole> console(x3::con::createConsole());
    x3::game::Hud hud;
    bool quitRequested = false;
    hud.init(*console, &quitRequested);

    // FIX 1: live-tunable viewmodel aim. Register vm_yaw/vm_pitch/vm_roll (deg)
    // and vm_fwd/vm_right/vm_down (m); read them each frame and feed the pose to
    // drawViewmodel so typing e.g. `vm_pitch 10` moves the held gun immediately.
    registerViewmodelCVars(*console);

    if (smoketest) {
        x3::logInfo("smoketest: stepping physics + rendering 30 frames (+ a mid-run swapchain recreate)");
        // Arm the player so the weapon GLB renders as the viewmodel this run
        // (validates the real model loads + draws under validation). Use a fixed
        // camera looking toward the room so the viewmodel + pickup are on screen.
        weapon.forceArm(scene);
        const float vmX = -3.0f, vmY = 1.7f, vmZ = 4.0f, vmYaw = -1.4f, vmPitch = 0.0f;
        device->setCamera(vmX, vmY, vmZ, vmYaw, vmPitch, 60.0f);
        // M9: exercise the audio system under the smoketest path too (init already
        // happened above; here we set the listener + start the music bed so the
        // smoketest proves audio comes up + plays without crashing under validation).
        audio->setListener(vmX, vmY, vmZ, vmYaw, vmPitch);
        audio->playMusic(kMusicPath, /*loop*/true, /*vol*/0.25f);
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 30; ++i) {
            glfwPollEvents();
            if (i == 15) { x3::logInfo("smoketest: triggering swapchain recreate"); device->onResize(960, 540); }
            physics->step(dt);
            scene.update(*physics);
            weapon.update(dt, scene, x3::phys::Vec3{ vmX, vmY, vmZ });
            combat.update(dt, scene, *physics, x3::phys::Vec3{ vmX, vmY, vmZ });
            audio->update(dt);
            // Exercise the FX path under validation: fire once mid-run (tracer +
            // muzzle flash) and crosshair every frame.
            if (i == 10) {
                x3::phys::Vec3 eye{ vmX, vmY, vmZ };
                x3::phys::Vec3 dir{ std::cos(vmPitch) * std::cos(vmYaw),
                                    std::sin(vmPitch),
                                    std::cos(vmPitch) * std::sin(vmYaw) };
                x3::game::FireResult r = combat.fire(eye, dir, scene, *physics);
                const x3::phys::Vec3 m = muzzleFromCamera(vmX, vmY, vmZ, vmYaw, vmPitch);
                combatFx.addTracer(m, r.endPoint);
                audio->playSound3D(sndGun, m.x, m.y, m.z, 0.85f, 1.0f);
            }
            combatFx.update(dt);
            // Exercise the HUD 2D path: drop some console output, and open the
            // console mid-run so the panel + scrollback + input line render too.
            if (i == 5)  { console->exec("echo smoketest hud line"); hud.toggleConsole(); hud.onChar('a'); hud.onChar('b'); }
            if (i == 20) { hud.closeConsole(); }
            auto frame = device->beginFrame();
            if (frame.valid) {
                scene.render(*device, frame);
                weapon.drawPickup(*device, frame, scene);
                combat.drawMonster(*device, frame, scene);
                const VmPose vmPose = readViewmodelPose(*console);
                weapon.drawViewmodel(*device, frame, vmX, vmY, vmZ, vmYaw, vmPitch,
                                     vmPose.yawRad, vmPose.pitchRad, vmPose.rollRad,
                                     vmPose.fwd, vmPose.right, vmPose.down);
                combatFx.draw(*device, frame, vmX, vmY, vmZ, vmYaw, vmPitch);
                // HUD overlay last: crosshair + FPS meter + console (when open).
                hud.drawCrosshair(*device, frame);
                hud.drawFps(*device, frame, *console, dt);
                hud.drawConsole(*device, frame, *console, dt);
                // Also exercise a raw quad + text string every frame.
                const float tag[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                device->drawHudText(frame, "X3 HUD SMOKETEST 0123", 8.0f, 40.0f, 16.0f, tag);
            }
            device->endFrame(frame);
        }
        x3::logInfo(std::string("smoketest: weapon viewmodel drawn (") +
                    (weapon.usingRealModel() ? "real GLB" : "fallback box") + ")");
        x3::logInfo(std::string("smoketest: monster drawn (") +
                    (combat.usingRealModel() ? "real GLB" : "fallback box") + ")");
        // Report where the dynamic box settled (proves physics drives the entity).
        for (uint32_t id = 0; id < scene.size(); ++id) {
            const auto& e = scene.get(id);
            if (e.tag == (uint32_t)x3::game::Tag::Prop && e.body.valid()) {
                x3::phys::Vec3 p = physics->getBodyPosition(e.body);
                x3::logInfo("smoketest: dynamic box settled at y=" + std::to_string(p.y));
            }
        }
        x3::logInfo("smoketest: 30 frames + recreate OK");
        audio->shutdown();
        combatFx.shutdown(*device);
        physics->shutdown();
        device->shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

    x3::logInfo("entering main loop — WASD walk, mouse look, LeftShift sprint, Space jump, E use, F noclip, ` console, Esc to quit");

    // ---- Walking player (S3). Spawn on open floor near the +Z doorway side,
    // clear of the falling box (origin) and the step platform (around x=3,z=-3).
    x3::game::Player player;
    player.spawn(*physics, -3.0f, 0.05f, 4.0f);

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
    double prevTime = glfwGetTime();

    // ---- S7: route keyboard text + editing into the on-screen console. The
    // char callback feeds printable codepoints; the key callback handles the
    // '`' toggle + Enter/Backspace/Up/Down/Tab/Esc while the console is open.
    InputContext inputCtx{ &hud, console.get() };
    glfwSetWindowUserPointer(window, &inputCtx);
    glfwSetCharCallback(window, charCallback);
    glfwSetKeyCallback(window, keyCallback);
    bool consoleWasOpen = false;   // tracks cursor-mode transitions

    // Rising-edge tracking for Space (jump), F (noclip toggle), E (use), and the
    // left mouse button (fire). A small fire cooldown gates the gun's rate.
    bool prevSpace = false, prevF = false, prevE = false, prevFire = false;
    float fireCooldown = 0.0f;          // seconds until the gun can fire again
    constexpr float kFireCooldown = 0.25f;

    // ---- M9 audio event edge-tracking + footstep cadence -------------------
    bool  prevArmed   = false;          // pickup chime on the arm rising edge
    float stepTimer   = 0.0f;           // accumulates while moving on the ground
    float prevCamX = 0.0f, prevCamZ = 0.0f; // for horizontal-speed footsteps
    bool  prevCamValid = false;

    // ---- M9: start the low-volume looping ambient/music bed at launch ----
    audio->playMusic(kMusicPath, /*loop*/true, /*vol*/0.25f);

    // ---- Optional debug noclip/fly camera (toggle with F). Not required by S3,
    // handy for inspecting the level. Off by default — gameplay is the walker.
    bool noclip = false;
    float flyX = -3.0f, flyY = 1.7f, flyZ = 4.0f, flyYaw = 0.0f, flyPitch = 0.0f;

    // ---- Main loop ----
    int lastW = static_cast<int>(W), lastH = static_cast<int>(H);
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // ---- S7: console gating. While the console is open, gameplay input is
        // suppressed and the cursor is shown so the user can read/type; Esc
        // closes the console (handled in keyCallback) rather than quitting.
        const bool consoleOpen = hud.consoleOpen();
        if (consoleOpen != consoleWasOpen) {
            glfwSetInputMode(window, GLFW_CURSOR,
                             consoleOpen ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
            consoleWasOpen = consoleOpen;
        }
        // Esc quits only when the console is closed (the `quit` command also sets
        // quitRequested via the HUD-registered command).
        if (!consoleOpen && glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, 1);
        if (quitRequested) glfwSetWindowShouldClose(window, 1);

        double nowT = glfwGetTime();
        float dt = static_cast<float>(nowT - prevTime); prevTime = nowT;
        if (dt > 0.1f) dt = 0.1f; // clamp huge hitches (e.g. after a stall)

        // Mouse delta this frame. Frozen (zeroed) while the console is open so
        // the view does not swing under a visible cursor.
        double mx, my; glfwGetCursorPos(window, &mx, &my);
        float ddx = static_cast<float>(mx - lastMX), ddy = static_cast<float>(my - lastMY);
        lastMX = mx; lastMY = my;
        if (consoleOpen) { ddx = 0.0f; ddy = 0.0f; }

        // Gameplay key reads are gated off while the console is open so typing
        // doesn't drive movement/use/jump/fire/noclip.
        auto keyDown = [&](int k) { return !consoleOpen && glfwGetKey(window, k) == GLFW_PRESS; };

        // F: toggle noclip on the rising edge. Seed the fly camera from the
        // player's current view so the transition is seamless.
        bool fNow = keyDown(GLFW_KEY_F);
        if (fNow && !prevF) {
            noclip = !noclip;
            if (noclip) player.camera(flyX, flyY, flyZ, flyYaw, flyPitch);
            x3::logInfo(noclip ? "noclip ON" : "noclip OFF");
        }
        prevF = fNow;

        bool spaceNow = keyDown(GLFW_KEY_SPACE);

        // ---- E: "use" on the rising edge. Raycast from the eye along the facing
        // direction; if it hits a button (within ~3 m) the linked door opens. ----
        bool eNow = keyDown(GLFW_KEY_E);
        if (eNow && !prevE) {
            float ex, ey, ez, yaw, pitch;
            player.camera(ex, ey, ez, yaw, pitch);   // in noclip the camera is the fly cam
            if (noclip) { ex = flyX; ey = flyY; ez = flyZ; yaw = flyYaw; pitch = flyPitch; }
            // Device forward convention: fwd = (cos p cos y, sin p, cos p sin y).
            x3::phys::Vec3 eye{ ex, ey, ez };
            x3::phys::Vec3 dir{ std::cos(pitch) * std::cos(yaw),
                                std::sin(pitch),
                                std::cos(pitch) * std::sin(yaw) };
            if (x3::game::tryUse(eye, dir, 3.0f, scene, doors, *physics)) {
                x3::logInfo("use: button pressed — door opening");
                // S4 door SFX at the eye (button is right in front of the player).
                audio->playSound3D(sndDoor, eye.x, eye.y, eye.z, 0.9f, 1.0f);
            }
        }
        prevE = eNow;

        // Camera state this frame (set by whichever branch runs), reused below
        // for the weapon viewmodel.
        float camX, camY, camZ, camYaw, camPitch;
        if (!noclip) {
            // ---- Walking player input ----
            x3::game::PlayerInput in;
            if (keyDown(GLFW_KEY_W)) in.moveFwd    += 1.0f;
            if (keyDown(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
            if (keyDown(GLFW_KEY_D)) in.moveStrafe += 1.0f;
            if (keyDown(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
            // Right mouse button = walk forward (hold to autorun)
            if (!consoleOpen && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
                in.moveFwd += 1.0f;
            in.sprint      = keyDown(GLFW_KEY_LEFT_SHIFT);
            in.jumpPressed = spaceNow && !prevSpace;   // rising edge
            in.lookDX = ddx;
            in.lookDY = ddy;

            player.update(in, dt, *physics);
            doors.update(dt, scene, *physics);   // advance any opening door first
            physics->step(dt);
            scene.update(*physics);

            player.camera(camX, camY, camZ, camYaw, camPitch);
            device->setCamera(camX, camY, camZ, camYaw, camPitch, 60.0f);
        } else {
            // ---- Debug fly camera (does not move the player body) ----
            const float sens = 0.0025f;
            flyYaw += ddx * sens; flyPitch -= ddy * sens;
            if (flyPitch >  1.55f) flyPitch =  1.55f;
            if (flyPitch < -1.55f) flyPitch = -1.55f;
            float fx = std::cos(flyPitch) * std::cos(flyYaw);
            float fy = std::sin(flyPitch);
            float fz = std::cos(flyPitch) * std::sin(flyYaw);
            float rl = std::sqrt(fx * fx + fz * fz); if (rl < 1e-4f) rl = 1e-4f;
            float rx = -fz / rl, rz = fx / rl;
            float spd = 4.0f * dt;
            if (keyDown(GLFW_KEY_LEFT_SHIFT)) spd *= 3.0f;
            if (keyDown(GLFW_KEY_W)) { flyX += fx*spd; flyY += fy*spd; flyZ += fz*spd; }
            if (keyDown(GLFW_KEY_S)) { flyX -= fx*spd; flyY -= fy*spd; flyZ -= fz*spd; }
            if (keyDown(GLFW_KEY_D)) { flyX += rx*spd; flyZ += rz*spd; }
            if (keyDown(GLFW_KEY_A)) { flyX -= rx*spd; flyZ -= rz*spd; }
            if (spaceNow) flyY += spd;
            if (keyDown(GLFW_KEY_LEFT_CONTROL)) flyY -= spd;

            // World still advances so props keep simulating while inspecting.
            doors.update(dt, scene, *physics);   // advance any opening door first
            physics->step(dt);
            scene.update(*physics);
            device->setCamera(flyX, flyY, flyZ, flyYaw, flyPitch, 60.0f);
            camX = flyX; camY = flyY; camZ = flyZ; camYaw = flyYaw; camPitch = flyPitch;
        }
        prevSpace = spaceNow;

        // ---- M9: drive the 3D listener from the player camera each frame ----
        audio->setListener(camX, camY, camZ, camYaw, camPitch);

        // ---- M9: footsteps. Time them to horizontal speed while grounded (not in
        // noclip): estimate speed from the camera's XZ delta this frame; while
        // moving, play a quiet pitched-down step every kStepInterval seconds. ----
        if (prevCamValid && !noclip && player.grounded() && dt > 0.0f) {
            const float dxc = camX - prevCamX, dzc = camZ - prevCamZ;
            const float speed = std::sqrt(dxc * dxc + dzc * dzc) / dt; // m/s
            if (speed > 0.6f) {
                // Cadence scales a little with speed (faster -> quicker steps).
                const float kStepInterval = (speed > 6.5f) ? 0.32f : 0.45f;
                stepTimer += dt;
                if (stepTimer >= kStepInterval) {
                    stepTimer = 0.0f;
                    audio->playSound2D(sndStep, 0.22f, 0.55f); // quiet, pitched down
                }
            } else {
                stepTimer = 0.0f; // reset cadence when stopped
            }
        }
        prevCamX = camX; prevCamZ = camZ; prevCamValid = true;

        // ---- S5: weapon pickup detection + viewmodel. Arm when the camera
        // (player eye) is within pickup radius; once armed, draw the viewmodel.
        weapon.update(dt, scene, x3::phys::Vec3{ camX, camY, camZ });
        // M9: pickup chime on the arm rising edge (S5).
        if (weapon.hasWeapon() && !prevArmed)
            audio->playSound2D(sndPickup, 0.8f, 1.0f);
        prevArmed = weapon.hasWeapon();

        // ---- S6: combat. Decay the monster's hit-flash (+ optional chase), then
        // handle FIRE on the left-mouse rising edge: only effective when armed,
        // gated by a small cooldown. Raycast from the eye along the look dir;
        // hitting the monster damages it (red flash) and eventually kills it.
        combat.update(dt, scene, *physics, x3::phys::Vec3{ camX, camY, camZ });
        if (fireCooldown > 0.0f) fireCooldown -= dt;
        bool fireNow = !consoleOpen && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        if (fireNow && !prevFire && weapon.hasWeapon() && fireCooldown <= 0.0f) {
            fireCooldown = kFireCooldown;
            // Device forward convention: fwd = (cos p cos y, sin p, cos p sin y).
            x3::phys::Vec3 eye{ camX, camY, camZ };
            x3::phys::Vec3 dir{ std::cos(camPitch) * std::cos(camYaw),
                                std::sin(camPitch),
                                std::cos(camPitch) * std::sin(camYaw) };
            x3::game::FireResult r = combat.fire(eye, dir, scene, *physics);
            // Shot feedback: a tracer from the viewmodel muzzle to the hit point
            // (or max range on a miss) + a muzzle flash.
            const x3::phys::Vec3 muzzle = muzzleFromCamera(camX, camY, camZ, camYaw, camPitch);
            combatFx.addTracer(muzzle, r.endPoint);
            // M9: gunshot SFX at the muzzle (3D).
            audio->playSound3D(sndGun, muzzle.x, muzzle.y, muzzle.z, 0.85f, 1.0f);
            // M9: monster death/impact SFX at the kill point (the ray hit point).
            if (r.killed)
                audio->playSound3D(sndDeath, r.endPoint.x, r.endPoint.y, r.endPoint.z, 1.0f, 1.0f);
            if (r.killed)          x3::logInfo("fire: monster killed!");
            else if (r.hitMonster) x3::logInfo("fire: monster hit — HP " + std::to_string(r.hpAfter));
        }
        prevFire = fireNow;

        // Advance FX timers (tracer lifetimes + muzzle flash).
        combatFx.update(dt);
        // M9: tick the audio system (reaps finished one-shot voices).
        audio->update(dt);

        int cw, ch;
        glfwGetFramebufferSize(window, &cw, &ch);
        if (cw != lastW || ch != lastH) {
            lastW = cw; lastH = ch;
            if (cw > 0 && ch > 0) device->onResize(static_cast<uint32_t>(cw), static_cast<uint32_t>(ch));
        }

        auto frame = device->beginFrame();
        if (frame.valid) {
            scene.render(*device, frame);
            weapon.drawPickup(*device, frame, scene);   // bobbing pickup (until armed)
            combat.drawMonster(*device, frame, scene);  // the monster (+ hit-flash / death-pop)
            const VmPose vmPose = readViewmodelPose(*console);
            weapon.drawViewmodel(*device, frame, camX, camY, camZ, camYaw, camPitch,
                                 vmPose.yawRad, vmPose.pitchRad, vmPose.rollRad,
                                 vmPose.fwd, vmPose.right, vmPose.down);
            // FX: active tracers + muzzle flash (world-space).
            combatFx.draw(*device, frame, camX, camY, camZ, camYaw, camPitch);
            // ---- S7 HUD overlay last: crosshair (hidden while console open),
            // FPS meter, then the console panel (when open). ----
            if (!consoleOpen) hud.drawCrosshair(*device, frame);
            hud.drawFps(*device, frame, *console, dt);
            hud.drawConsole(*device, frame, *console, dt);
        }
        device->endFrame(frame);
    }

    x3::logInfo("shutting down");
    audio->shutdown();
    combatFx.shutdown(*device);
    physics->shutdown();
    device->shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
