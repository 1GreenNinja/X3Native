// ===========================================================================
// TEST REGISTRY — the headless --test-* / --demo-* / --list-clips dispatch
// ladder, moved VERBATIM out of app/main.cpp's main() (#28 monolith split).
//
// Every handler body below is byte-for-byte identical to the chain that used to
// live inline in main(); the ONLY change is that the flag conditions read from
// the TestFlags struct (tf.testX) instead of main()'s local bools, and the
// whole ladder is wrapped in dispatchTests() which returns -1 (instead of
// falling through to boot) when no flag matched. The C1061 chain-breaks are
// preserved trivially: the ladder was already a sequence of independent `if`s,
// each returning, not one giant else-if — that structure is unchanged.
// ===========================================================================
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "engine/core/x3_log.h"
#include "engine/core/IConsole.h"
#include "engine/core/IJobSystem.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/rhi/FrustumCull.h"
#include "engine/rhi/GpuCull.h"
#include <glm/gtc/matrix_transform.hpp>
#include "engine/asset/IAssetSource.h"
#include "engine/asset/IModelLoader.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/physics/Destruction.h"
#include "strata.h"              // R-3 fold: runStrataSelfTest
#include "elevator_showcase.h"  // R-4 fold: runElevatorShowcaseSelfTest
#include "inventory.h"          // W9-3 RPG: runInventorySelfTest
#include "progression.h"        // W9-3 RPG: runProgressionSelfTest
#include "skilltree.h"          // W9-3 RPG: runSkillTreeSelfTest
#include "engine/physics/StructuralCollapse.h"
#include "engine/physics/Ragdoll.h"
#include "engine/physics/BodyContact.h"   // x3::phys::runBodyContactSelfTest (--test-bodycontact)
#include "engine/physics/IVehicle.h"
#include "engine/audio/IAudioSystem.h"
#include "engine/audio/RtAcoustics.h"
#include "engine/net/INetworkSystem.h"
#include "engine/net/ISnapshotInterpolator.h"   // x3::net::runNetInterpSelfTest
#include "engine/net/IClientPredictor.h"          // x3::net::runNetPredictSelfTest
#include "engine/ai/INavigation.h"
#include "engine/script/IScriptSystem.h"
#include "engine/llm/ILlmSystem.h"
#include "engine/ecs/Ecs.h"

#include "scene.h"
#include "asset_root.h"
#include "anim.h"
#include "thirdperson.h"
#include "level1.h"
#include "level_loader.h"
#include "level_lint.h"
#include "keypad.h"      // PB fold: --test-keypad (realistic access keypad geometry)
#include "leveldoc_world.h"
#include "player.h"
#include "monster.h"
#include "level1_game.h"
#include "canon_play.h"
#include "desc_mechanics.h"   // W9-1: runDescMechSelfTest (--test-descmech)
#include "canon_aliens.h"                    // EFLZ canon-alien roster (Mantis/Grey/Reptilian/Nordic) — --test-canonaliens
#include "intro_coldopen.h"
#include "intro_orchestrator.h"   // x3::intro::runIntroOrchestratorSelfTest (--test-introorch)
#include "world_hosts.h"          // x3::apphost::runSurfaceStartSelfTest (--test-surfacestart)
#include "cutscene.h"
#include "npc_dialog.h"
#include "chat_tree.h"
#include "mission.h"
#include "physprops.h"
#include "ragdoll.h"
#include "ragdoll_demo.h"   // x3::game::runRagdollBlendCheck (--test-ragdoll)
#include "vehicle.h"        // x3::game::runDriveEnterExitSelfTest (--test-vehicle)
#include "world_cars.h"     // x3::game::runCanonVehicleSelfTest (--test-canonvehicle)
#include "editor/editor.h"
#include "editor/editor_ai.h"
#include "editor/editor_host.h"
#include "barrels.h"
#include "glass_test.h"
#include "holo_terminal.h"
#include "secret_room.h"
#include "headless_device.h"
#include "ecs_render.h"
#include "spire_mid.h"
#include "spire_top.h"
#include "spire_nexus.h"
#include "spire_sublevels.h"
#include "timeline.h"
#include "act2_world.h"
#include "act2_desert.h"
#include "act2_caves.h"
#include "rifthub.h"                        // --test-rifthub (Stargate portal hub)
#include "basis.h"                          // --test-basis (KNOWN_BUGS R3: the MIRROR invariant)
#include "tod.h"
#include "weather.h"
#include "world_regions.h"
#include "world_stream.h"
#include "world_map.h"
#include "city.h"
#include "ocean_base.h"
#include "elevator.h"
#include "club1127.h"
#include "perfshop.h"
#include "valley.h"
#include "cliffs.h"
#include "ui.h"
#include "loading_screen.h"
#include "save.h"
#include "dialog.h"
#include "vehparts.h"
#include "ecology.h"
#include "crowd.h"
#include "alert.h"
#include "space_pilot.h"          // x3::game::runSpaceSelfTest (--test-space)
#include "space/ship_ai.h"        // x3::space::runShipAiSelfTest (--test-ship-ai)
#include "space/ship_interior.h"  // x3::space::runShipInteriorSelfTest (--test-shipinterior, S5 fold)
#include "space/ship_windows.h"   // x3::space::runShipWindowsSelfTest (--test-shipwindows, S6 fold)
#include "space/wormhole_vfx.h"      // x3::space::runWormholeSelfTest (--test-wormhole, feast fold)
#include "space/wormhole_transit.h"  // x3::space::runWormholeTransitSelfTest (--test-wormhole-transit)
#include "space/tractor_beam.h"      // x3::space::runTractorSelfTest (--test-tractor, feast fold)
#include "wing_dressing.h"        // x3::game::runWingDressingSelfTest (--test-wingdressing)
#include "descent_slide.h"        // x3::game::runDescentSlideSelfTest (--test-descentslide, Wave 2C)
#include "space/targeting.h"      // x3::space::TargetingSystem (--test-targeting)
#include "space/ship_damage.h"    // x3::space::runShipDamageSelfTest (--test-ship-damage)
#include "space/eva.h"            // x3::space::runEvaSelfTest (--test-eva)

#include "test_registry.h"
#include "self_tests.h"          // x3::apphost run*SelfTest device-driven helpers
#include "speaking_monster.h"    // x3::apphost::SpeakingMonster (--demo-dialog)

#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>

// Keypad code for the showroom HIDDEN HATCH — used by the --test-hatchcode smoke
// below. Mirrors the file-scope constant in app/main.cpp (the --world showroom
// hatch); kept in sync there. (See app/main.cpp kShowroomHatchCode.)
namespace { constexpr int kShowroomHatchCode = 2742; }

namespace x3::apphost {

// The run*SelfTest helpers that live in x3::apphost (self_tests.cpp) and were
// called unqualified from main(); keep them unqualified here too so the bodies
// are byte-identical.
using x3::apphost::runFrustumCullSelfTest;
using x3::apphost::runDebrisSelfTest;
using x3::apphost::runGpuSkinSelfTest;
using x3::apphost::runGpuCullSelfTest;
using x3::apphost::runVisUnifySelfTest;
using x3::apphost::runHatchChainSelfTest;

int dispatchTests(const TestFlags& tf) {
    // Headless self-tests (no window / Vulkan needed)
    if (tf.testJobs) {
        x3::logInfo("running job system (Subsystem A) self-test...");
        return x3::jobs::runJobSystemSelfTest() ? 0 : 1;
    }
    if (tf.testAsset) {
        x3::logInfo("running asset (D5) self-test...");
        return x3::asset::runAssetSelfTest() ? 0 : 1;
    }
    if (tf.testConsole) {
        x3::logInfo("running console (D6) self-test...");
        return x3::con::runConsoleSelfTest() ? 0 : 1;
    }
    if (tf.testPhysics) {
        x3::logInfo("running physics (M3) self-test...");
        return x3::phys::runPhysicsSelfTest() ? 0 : 1;
    }
    if (tf.testPhysJoint) {
        x3::logInfo("running Physics §1 suspended/constrained-body (--test-physjoint) self-test...");
        return x3::phys::runPhysJointSelfTest() ? 0 : 1;
    }
    if (tf.testRagdoll) {
        x3::logInfo("running Physics §2 ragdoll+blend (--test-ragdoll) self-test...");
        // Engine-side: the Jolt ragdoll fall/settle/chain-hold + blend-math check.
        bool engineOk = x3::phys::runRagdollSelfTest();
        // App-side: drive the REAL anim::Skinner ragdoll-blend across weight 0->1
        // over a synthetic skinned model and assert the palette interpolates
        // monotonically (the §2 skin-follows-ragdoll acceptance, end to end).
        int bPass = 0, bTotal = 0;
        bool blendOk = x3::game::runRagdollBlendCheck(bPass, bTotal);
        x3::logInfo("ragdoll-blend: " + std::to_string(bPass) + "/" +
                    std::to_string(bTotal) + " passed");
        // App-side physics-death ragdoll (this session's app/ragdoll.cpp path).
        bool deathOk = x3::game::runRagdollSelfTest();
        return (engineOk && blendOk && deathOk) ? 0 : 1;
    }
    if (tf.testGltf) {
        x3::logInfo("running glTF/GLB model loader (M2) self-test...");
        return x3::asset::runModelLoaderSelfTest() ? 0 : 1;
    }
    if (tf.testPlayer) {
        x3::logInfo("running player/character-controller (S3) self-test...");
        return x3::game::runPlayerSelfTest() ? 0 : 1;
    }
    if (tf.testInteract) {
        x3::logInfo("running button->door interaction (S4) self-test...");
        return x3::game::runInteractSelfTest() ? 0 : 1;
    }
    if (tf.testPhysprops) {
        x3::logInfo("running physics-props (hanging cubes / joints) self-test...");
        return x3::game::runPhysPropsSelfTest() ? 0 : 1;
    }
    if (tf.testRagdollSkin) {
        x3::logInfo("running ragdoll-skin (rigid bone attach) self-test...");
        return x3::game::runRagdollSkinSelfTest() ? 0 : 1;
    }
    if (tf.testEditor) {
        x3::logInfo("running Level Editor E1 (JSON/pick/gizmo) self-test...");
        return x3::editor::runEditorSelfTest() ? 0 : 1;
    }
    if (tf.testEditorAi) {
        x3::logInfo("running AI Architect (plan parse/validate/transact) self-test...");
        return x3::editor::runEditorAiSelfTest() ? 0 : 1;
    }
    if (tf.testBlockout) {
        x3::logInfo("running Level Architect BLOCKOUT (brushes[] JSON / snap / mesh) self-test...");
        return x3::editor::runBlockoutSelfTest() ? 0 : 1;
    }
    if (tf.testLoader) {
        x3::logInfo("running LevelDoc data-driven loader (save->load->hot-reload) self-test...");
        return x3::game::runLevelDocLoaderSelfTest() ? 0 : 1;
    }
    if (tf.testBarrels) {
        x3::logInfo("running explosive-barrels self-test...");
        return x3::game::runBarrelSelfTest() ? 0 : 1;
    }
    if (tf.testGlass) {
        x3::logInfo("running translucent-glass material (M1 see-through) self-test...");
        return x3::game::runGlassSelfTest() ? 0 : 1;
    }
    if (tf.testFrustumCull) {
        x3::logInfo("running CPU per-object frustum-cull (D15 baseline) self-test...");
        return runFrustumCullSelfTest() ? 0 : 1;
    }
    if (tf.testHoloterm) {
        x3::logInfo("running holo-terminal (text + input) self-test...");
        return x3::game::runHoloTerminalSelfTest() ? 0 : 1;
    }
    if (tf.testSecretRoom) {
        x3::logInfo("running secret-room (code-locked trapdoor) self-test...");
        return x3::game::runSecretRoomSelfTest() ? 0 : 1;
    }
    if (tf.testHatch) {
        x3::logInfo("running END-TO-END secret-hatch chain self-test "
                    "(terminal -> fire(terminal_code) -> secret_room.lua -> "
                    "openTrapdoor -> REAL DoorSystem hatch + objective; C1-C8)...");
        return runHatchChainSelfTest() ? 0 : 1;
    }
    if (tf.testLlm) {
        // Mock plumbing always; the real-model round-trip is gated on the .gguf
        // existing (see assets/models/llm/README.md for the download command).
        x3::logInfo("running in-engine LLM (NPC minds) self-test...");
        const std::string llmModel = x3::game::assetRoot() + "/models/llm/qwen2.5-3b-instruct-q4_k_m.gguf";
        return x3::llm::runLlmSelfTest(llmModel.c_str()) ? 0 : 1;
    }
    if (tf.testEcs) {
        x3::logInfo("running ECS (sparse-set, 50k entities) self-test...");
        return x3::ecs::runEcsSelfTest() ? 0 : 1;
    }
    if (tf.testEcsRender) {
        x3::logInfo("running ECS->GPU render-feed self-test...");
        return x3::game::runEcsRenderSelfTest() ? 0 : 1;
    }
    if (tf.testPickup) {
        x3::logInfo("running weapon pickup + arming (S5) self-test...");
        return x3::game::runPickupSelfTest() ? 0 : 1;
    }
    if (tf.testCombat) {
        x3::logInfo("running shoot-monster combat (S6) self-test...");
        return x3::game::runCombatSelfTest() ? 0 : 1;
    }
    if (tf.testDeathRagdoll) {
        x3::logInfo("running skinned death-ragdoll (TASK#12) self-test...");
        return x3::game::runDeathRagdollSelfTest() ? 0 : 1;
    }
    if (tf.testAudio) {
        x3::logInfo("running audio (M9) self-test...");
        return x3::audio::runAudioSelfTest() ? 0 : 1;
    }
    if (tf.testAcoustics) {
        x3::logInfo("running RT-acoustics self-test (occlusion rays + room estimate)...");
        return x3::audio::runAcousticsSelfTest() ? 0 : 1;
    }
    if (tf.testLevel1) {
        x3::logInfo("running EFLZ Level 1 (Awakening) self-test (T1-T6)...");
        return x3::game::runLevel1SelfTest() ? 0 : 1;
    }
    if (tf.testCanonLevel) {
        x3::logInfo("running EFLZ data-driven canonical-level self-test (C1-C8)...");
        return x3::game::runCanonLevelSelfTest() ? 0 : 1;
    }
    if (tf.testKeypad) {
        x3::logInfo("running realistic keypad geometry self-test (KP1-KP6)...");
        return x3::game::runKeypadSelfTest() ? 0 : 1;
    }
    if (tf.testLevelLint) {
        x3::logInfo("running GATE A geometric level lint (door-seat/junction/cut-span/reach)...");
        return x3::game::runLevelLintSelfTest() ? 0 : 1;
    }
    if (tf.testCanonPlay) {
        x3::logInfo("running EFLZ canon Floor-1 gameplay self-test (P1-P9)...");
        return x3::game::runCanonPlaySelfTest() ? 0 : 1;
    }
    if (tf.testGoldenPath) {
        x3::logInfo("running the ENDGAME SPINE self-test (G1-G9: tower -> clone gate -> Sarah -> Helipad WIN)...");
        return x3::game::runGoldenPathSelfTest() ? 0 : 1;
    }
    if (tf.testDescMech) {
        x3::logInfo("running the DESC-MECHANICS self-test (D1-D12: interact framework + the 5 Tier-A verbs)...");
        return x3::game::runDescMechSelfTest() ? 0 : 1;
    }
    if (tf.testInventory) {   // W9-3 RPG
        x3::logInfo("running the INVENTORY/BACKPACK self-test (I1-I7: item DB + stack/cap + keycard-gated door)...");
        return x3::game::runInventorySelfTest() ? 0 : 1;
    }
    if (tf.testProgression) {   // W9-3 RPG
        x3::logInfo("running the XP/LEVEL PROGRESSION self-test (X1-X6: curve + points + save round-trip)...");
        return x3::game::runProgressionSelfTest() ? 0 : 1;
    }
    if (tf.testSkillTree) {   // W9-3 RPG
        x3::logInfo("running the SKILL TREE self-test (S1-S7: prereq/cost + the stat fold reaching the player)...");
        return x3::game::runSkillTreeSelfTest() ? 0 : 1;
    }
    if (tf.testStrata) {
        x3::logInfo("running STRATA descent self-test (R-3 fold: bands + offshoots + on-foot route + club arrival)...");
        return x3::game::runStrataSelfTest() ? 0 : 1;
    }
    if (tf.testElevatorShowcase) {
        x3::logInfo("running CENTERPIECE elevator showcase self-test (R-4 fold: dark-glass cab + strata + 1127 club descent)...");
        return x3::game::runElevatorShowcaseSelfTest() ? 0 : 1;
    }
    if (tf.testIntro) {
        x3::logInfo("running intro cold-open self-test (flight -> hit -> whiteout -> titlecard -> handoff; skippable)...");
        return x3::intro::runIntroSelfTest() ? 0 : 1;
    }
    if (tf.testCutscene) {
        x3::logInfo("running x3.cutscene/1 self-test (format + splines/cuts + player tick/seek/skip + "
                    "StoryFlags + the shipped cold open)...");
        return x3::cut::runCutsceneSelfTest() ? 0 : 1;
    }
    if (tf.testIntroOrch) {
        x3::logInfo("running Phase 3 INTRO ORCHESTRATOR self-test (beat sequencing + skill->p "
                    "mapping bounds + deterministic chanceRoll outcome + StoryFlags['intro.outcome'] "
                    "write + input-cleared/deterministic headless interactive windows)...");
        return x3::intro::runIntroOrchestratorSelfTest() ? 0 : 1;
    }
    if (tf.testIntroCockpit) {
        x3::logInfo("running INTRO COCKPIT self-test (fighter_cockpit.glb -> Scene entities: "
                    "PBR route + emissiveTex content screens + transparent canopy glass, headless)...");
        return x3::apphost::runIntroCockpitSelfTest() ? 0 : 1;
    }
    if (tf.testShipInterior) {
        x3::logInfo("running S5 SHIP-INTERIOR self-test (walkable small-cockpit hull: "
                    "manifest windows + spawn + collide walls, headless)...");
        return x3::space::runShipInteriorSelfTest() ? 0 : 1;
    }
    if (tf.testWingDressing) {
        x3::logInfo("running FLOORS 2-7 WING-DRESSING self-test (synthetic wing floor: "
                    "every west-wing room classifies + dresses, no ZNone holes, headless)...");
        return x3::game::runWingDressingSelfTest() ? 0 : 1;
    }
    if (tf.testDescentSlide) {
        x3::logInfo("running Wave-2C DESCENT-SLIDE self-test (coaster-grade track spec: drop/"
                    "monotonic/winding/first-drop/overbank/airtime/choppers/windows + bounded "
                    "rider sim with unweight + crest tension, headless)...");
        return x3::game::runDescentSlideSelfTest() ? 0 : 1;
    }
    if (tf.testShipWindows) {
        x3::logInfo("running S6 SHIP-WINDOWS self-test (true-portal moving space: star/nebula "
                    "UV pan + per-window light-bleed against the interior manifest, headless)...");
        return x3::space::runShipWindowsSelfTest() ? 0 : 1;
    }
    if (tf.testBodyContact) {
        x3::logInfo("running BODY-CONTACT self-test (bone-surface solver: rigid rest + "
                    "soft settle + mattress indent bake + finite extents + determinism)...");
        return x3::phys::runBodyContactSelfTest() ? 0 : 1;
    }
    if (tf.testWormhole) {
        x3::logInfo("running Salvari crystal-matrix wormhole (WormholeVfx) self-test "
                    "(feast fold: init/render/shutdown + Tuning clamp + faceted bake, headless)...");
        return x3::space::runWormholeSelfTest() ? 0 : 1;
    }
    if (tf.testWormholeTransit) {
        x3::logInfo("running S3 WORMHOLE-TRANSIT self-test (feast fold: SpaceLayer spine "
                    "requestWormhole -> WormholeTransit -> DeepSpace, monotonic progress ramp, "
                    "re-arm, headless)...");
        return x3::space::runWormholeTransitSelfTest() ? 0 : 1;
    }
    if (tf.testTractor) {
        x3::logInfo("running capital-ship TRACTOR-BEAM (TractorBeam) self-test "
                    "(feast fold: init/render/shutdown + intensity ramp/clamp + degenerate "
                    "skip + energy bake, headless)...");
        return x3::space::runTractorSelfTest() ? 0 : 1;
    }
    if (tf.testIntroBranch) {
        x3::logInfo("running Phase 4 INTRO BRANCH-WIRING self-test (intro.outcome flag "
                    "round-trip + --intro-force dev override + per-save seed thread + "
                    "canon default — the app_run cell-vs-surface selection contract)...");
        return x3::intro::runIntroBranchSelfTest() ? 0 : 1;
    }
    if (tf.testSurfaceStart) {
        x3::logInfo("running Phase 7 SURFACE-START self-test (ESCAPED-branch Act-1: "
                    "cell-vs-surface selection + the surface scene standing up — glass "
                    "facility, player outside + armed, Sarah rescue target, objectives)...");
        return x3::apphost::runSurfaceStartSelfTest() ? 0 : 1;
    }
    if (tf.testPhase2a) {
        x3::logInfo("running EFLZ Phase 2a (player health + enemies fight back) self-test...");
        return x3::game::runPhase2aSelfTest() ? 0 : 1;
    }
    if (tf.testPhase2b) {
        x3::logInfo("running EFLZ Phase 2b (super-strength melee + Martinez boss phases) self-test...");
        return x3::game::runPhase2bSelfTest() ? 0 : 1;
    }
    if (tf.testAnim) {
        x3::logInfo("running J1 skeletal animation + CPU skinning self-test...");
        return x3::anim::runAnimSelfTest() ? 0 : 1;
    }
    if (tf.testLocomotion) {
        x3::logInfo("running T1 locomotion-blend (idle/walk/run + crossfade) self-test...");
        return x3::anim::runLocomotionSelfTest(tf.testLocomotionPath) ? 0 : 1;
    }
    if (tf.listClips) {
        // Asset-pipeline check for the retargeted multi-clip character GLBs:
        // load the GLB headless, list its clips, and confirm Walk sampled at
        // t=0 vs t=0.5 changes the joint palette (proves a real, distinct clip).
        if (tf.listClipsPath.empty()) {
            x3::logError("--list-clips: need a GLB path");
            return 1;
        }
        namespace fs = std::filesystem;
        fs::path p(tf.listClipsPath);
        if (!fs::exists(p)) { x3::logError("--list-clips: no such file: " + tf.listClipsPath); return 1; }
        std::unique_ptr<x3::asset::IAssetSource> src(x3::asset::createAssetSource());
        src->mountDir(p.parent_path().string(), 0);
        std::unique_ptr<x3::asset::IModelLoader> loader(
            x3::asset::createModelLoader(nullptr, src.get()));   // headless
        x3::asset::Model model = loader->load(p.filename().string());
        if (!model.ok) { x3::logError("--list-clips: load failed"); return 1; }
        x3::logInfo("--list-clips: " + tf.listClipsPath + " has " +
                    std::to_string(model.animations.size()) + " animation clip(s):");
        for (size_t c = 0; c < model.animations.size(); ++c)
            x3::logInfo("  clip[" + std::to_string(c) + "] = \"" +
                        model.animations[c].name + "\"  (" +
                        std::to_string(model.animations[c].duration) + "s)");
        x3::anim::Skinner sk;
        bool bound = sk.bind(model);
        if (!bound) { x3::logError("--list-clips: model not skinnable"); loader->unload(model); return 1; }
        int idle = sk.findClip({ "idle" });
        int walk = sk.findClip({ "walk" });
        int run  = sk.findClip({ "run" });
        x3::logInfo("--list-clips: findClip idle=" + std::to_string(idle) +
                    " walk=" + std::to_string(walk) + " run=" + std::to_string(run));
        bool ok = sk.clipCount() > 1 && walk >= 0 && run >= 0;
        // Confirm Walk is a live clip: palette differs between t=0 and t=0.5,
        // and (when an idle clip exists) Walk@0 differs from Idle@0.
        if (walk >= 0) {
            std::vector<float> w0, w5, i0;
            sk.computePalette(model, (uint32_t)walk, 0.0f, w0);
            sk.computePalette(model, (uint32_t)walk, 0.5f, w5);
            float dWalk = 0.0f;
            for (size_t i = 0; i < w0.size() && i < w5.size(); ++i)
                dWalk = std::max(dWalk, std::fabs(w0[i] - w5[i]));
            x3::logInfo("--list-clips: Walk palette max-delta t0->t0.5 = " + std::to_string(dWalk));
            ok = ok && dWalk > 1e-3f;
            if (idle >= 0) {
                sk.computePalette(model, (uint32_t)idle, 0.0f, i0);
                float dVsIdle = 0.0f;
                for (size_t i = 0; i < w0.size() && i < i0.size(); ++i)
                    dVsIdle = std::max(dVsIdle, std::fabs(w0[i] - i0[i]));
                x3::logInfo("--list-clips: Walk@0 vs Idle@0 max-delta = " + std::to_string(dVsIdle));
                ok = ok && dVsIdle > 1e-3f;
            }
        }
        loader->unload(model);
        x3::logInfo(std::string("--list-clips: ") + (ok ? "PASS (>1 clip, Walk+Run present, Walk animates)"
                                                        : "FAIL"));
        return ok ? 0 : 1;
    }
    if (tf.testTerrain) {
        x3::logInfo("running B2 tiled terrain world self-test (settle + LOD)...");
        return x3::game::runTerrainSelfTest() ? 0 : 1;
    }
    if (tf.testTerrainPlace) {
        x3::logInfo("running terrain placement API self-test (height/normal/place)...");
        return x3::game::runTerrainPlaceSelfTest() ? 0 : 1;
    }
    if (tf.testStreaming) {
        x3::logInfo("running B3 world-streaming self-test (residency ring + async gen)...");
        return x3::game::runStreamingSelfTest() ? 0 : 1;
    }
    if (tf.testWorldStream) {
        x3::logInfo("running SEAMLESS region-graph world-streaming self-test "
                    "(region residency + ledgers + hysteresis + budget + proxy)...");
        return x3::game::runWorldStreamSelfTest() ? 0 : 1;
    }
    if (tf.testWorldMap) {
        x3::logInfo("running INTERACTIVE WORLD MAP self-test (POI discovery + "
                    "waypoint + fast-travel gates/streaming + zoom/pan math + "
                    "floor slices + tile bakes)...");
        return x3::game::runWorldMapSelfTest() ? 0 : 1;
    }
    if (tf.testAi) {
        x3::logInfo("running D-ai monster combat behaviour state-machine self-test...");
        return x3::game::runAiSelfTest() ? 0 : 1;
    }
    if (tf.testBestiary) {
        x3::logInfo("running data-driven enemy bestiary roster self-test...");
        return x3::game::runBestiarySelfTest() ? 0 : 1;
    }
    if (tf.testBosses) {
        x3::logInfo("running EFLZ Act-1 mid-boss roster + machine-extension "
                    "(multi-pod + scripted pre-fight hook) self-test...");
        return x3::game::runBossesSelfTest() ? 0 : 1;
    }
    if (tf.testAdaptiveHide) {
        x3::logInfo("running canon-aliens Adaptive-Hide rhythm self-test "
                    "(type-keyed resist + 8 s window; Warlord-tuned)...");
        return x3::game::runAdaptiveHideSelfTest() ? 0 : 1;
    }
    if (tf.testAct2Bosses) {
        x3::logInfo("running EFLZ Act-2 roster (5 alien-planet-surface enemies + "
                    "4 single-body bosses on the existing phase machine) self-test...");
        return x3::game::runAct2BossesSelfTest() ? 0 : 1;
    }
    if (tf.testCanonAliens) {
        x3::logInfo("running EFLZ canon-alien roster (Mantis/Grey/Reptilian/Nordic — "
                    "the 'most reported' species ported into MonsterSystem::Tuning rows: "
                    "Saurian Soldier/Warlord, Grey Tasked, Nordic Steward, Mantis Arbiter) "
                    "self-test...");
        return x3::game::runCanonAliensSelfTest() ? 0 : 1;
    }
    if (tf.testSpireMid) {
        x3::logInfo("running EFLZ Spire mid-floor (F3 Labs / F4 Offices / F5 Synth bay) "
                    "encounter-content self-test...");
        return x3::game::runSpireMidSelfTest() ? 0 : 1;
    }
    if (tf.testNexus) {
        x3::logInfo("running EFLZ Floor 4.5 Nexus Chamber / The Chorus "
                    "(off-elevator multi-pod boss) self-test...");
        return x3::game::runNexusSelfTest() ? 0 : 1;
    }
    if (tf.testSpireTop) {
        x3::logInfo("running EFLZ Spire top-floor (F6 Alien Technology Lab / F7 Executive "
                    "Laboratory Act-1 finale) encounter-content self-test...");
        return x3::game::runSpireTopSelfTest() ? 0 : 1;
    }
    if (tf.testTimeline) {
        x3::logInfo("running EFLZ morality/timeline backbone (infection 4-stage timers + "
                    "cure rates, Omega/Alpha/Beta/Gamma selector, morality axes, 12-ending "
                    "eligibility) self-test...");
        return x3::game::runTimelineSelfTest() ? 0 : 1;
    }
    if (tf.testDroneHack) {
        x3::logInfo("running EFLZ F5 Drone Manufacturing — Sarah's master hack "
                    "(strip Swarm AI HP + flip the drone army) self-test...");
        return x3::game::runDroneHackSelfTest() ? 0 : 1;
    }
    if (tf.testSubLevels) {
        x3::logInfo("running EFLZ hidden Floor-7 sub-levels (Waste Disposal / Cryo Storage "
                    "[Frozen Collective] / Enhanced Interrogation -> Dr. Chen Return Mission) "
                    "self-test...");
        return x3::game::runSubLevelsSelfTest() ? 0 : 1;
    }
    if (tf.testTod) {
        x3::logInfo("running EFLZ Time-of-Day cycle (4-phase dawn/day/dusk/night driving "
                    "sky/sun dir+color+intensity+haze+ambient via SkyParams; deterministic) "
                    "self-test...");
        return x3::game::runTodSelfTest() ? 0 : 1;
    }
    if (tf.testWeather) {
        x3::logInfo("running EFLZ Weather (7 states clear/cloudy/rain/storm/fog/sandstorm/snow; "
                    "smooth timed transitions; biome-gated; hazard flag for HazardZone) "
                    "self-test...");
        return x3::game::runWeatherSelfTest() ? 0 : 1;
    }
    if (tf.testAct2) {
        x3::logInfo("running EFLZ Act-2 open-world surface (L8 Surface Emergence "
                    "+ L9 Crystalline Desert Edge: alien terrain/sky host, lab-exit "
                    "gauntlet, Emergence-Point companions, crystal desert + hazard zone) "
                    "self-test...");
        return x3::game::runAct2WorldSelfTest() ? 0 : 1;
    }
    if (tf.testAct2Desert) {
        x3::logInfo("running EFLZ Act-2 desert depths (L10 Crystalline Desert Depths: "
                    "first-contact allied Salvari + injured-Salvari side-quest, hidden "
                    "crystal-cave camp entrance, light Overlord patrol; + L11 Salvari Camp "
                    "'Refugee Haven': cave settlement, survivors incl. K'thara, upgrade "
                    "station + cultural-exchange beat; reachable L9->L10->L11) self-test...");
        return x3::game::runAct2DesertSelfTest() ? 0 : 1;
    }
    if (tf.testAct2Caves) {
        x3::logInfo("running EFLZ Act-2 mid biomes (L12 Advanced Cave System + Crystal "
                    "Heart dual-gated interactable + Memory Hunter abyss boss; L13 Toxic "
                    "Swamplands Edge + poison hazard [inert at load]; L14 Research Station "
                    "+ timeline-gated Siren ambush; L15 Tree Cities + trading post) "
                    "self-test...");
        return x3::game::runAct2CavesSelfTest() ? 0 : 1;
    }
    if (tf.testBasis) {
        x3::logInfo("running BASIS/MIRROR self-test (KNOWN_BUGS R3: every model-instancing "
                    "basis must have a POSITIVE determinant — a negative one is a REFLECTION "
                    "that draws the model inside-out and unlit; scans every entity of every "
                    "built world, with negative controls, headless)...");
        return x3::game::runBasisSelfTest() ? 0 : 1;
    }
    if (tf.testRifthub) {
        x3::logInfo("running RIFTHUB Stargate portal-hub (8 grey-stone torus gates + "
                    "amber chevrons + event-horizon membrane + blue core; trigger ids "
                    "200-207; tick-driven chevron/core/ripple animation) self-test...");
        return x3::game::runRifthubSelfTest() ? 0 : 1;
    }
    if (tf.testWorldRegions) {
        x3::logInfo("running EFLZ open-world surface regions (crash site + outposts + "
                    "4 mountain ranges) self-test...");
        return x3::game::runWorldRegionsSelfTest() ? 0 : 1;
    }
    if (tf.testCity) {
        x3::logInfo("running EFLZ open-world metropolis (Scrapyard / New District / Industrial "
                    "+ road grid + 4 freeway tunnels) self-test...");
        return x3::game::runCitySelfTest() ? 0 : 1;
    }
    if (tf.testOceanBase) {
        x3::logInfo("running EFLZ open-world ocean + undersea base + submarine combat self-test...");
        return x3::game::runOceanBaseSelfTest() ? 0 : 1;
    }
    if (tf.testDoorCode) {
        x3::logInfo("running door-code keypad (locked coded door) self-test (K1-K6)...");
        return x3::game::runDoorCodeSelfTest() ? 0 : 1;
    }
    if (tf.testHatchCode) {
        // Showroom HIDDEN-HATCH keypad smoke (H1-H4). Drives the SAME KeypadEntry state
        // machine + the SAME submit comparison (value() == kShowroomHatchCode) the
        // --world showroom hatch uses, headlessly: a wrong code is rejected (buffer
        // cleared, hatch stays shut), the correct code (with a typo + backspace fixup)
        // is accepted and "opens" the hatch. Pure logic — no Vulkan/GLFW needed.
        x3::logInfo("running showroom hidden-hatch keypad smoke (H1-H4)...");
        bool ok = true;
        x3::game::KeypadEntry kp;
        bool hatchOpen = false;
        auto submit = [&](){
            if (kp.value() == kShowroomHatchCode) { hatchOpen = true; kp.clear(); return true; }
            kp.clear(); return false;  // wrong -> clear, stay shut (mirrors DENIED)
        };
        // H1: a WRONG code is rejected and the hatch stays shut.
        for (int d : {1,2,3,4}) kp.pushDigit(d);
        bool h1 = (!submit() && !hatchOpen);
        ok = ok && h1; x3::logInfo(std::string("  H1 wrong-code rejected: ") + (h1?"PASS":"FAIL"));
        // H2: digits append + Backspace fixes a typo so the buffer == the code.
        const int code = kShowroomHatchCode;                 // 4-digit (2742)
        kp.clear();
        kp.pushDigit((code/1000)%10);
        kp.pushDigit((code/100)%10);
        kp.pushDigit(9);            // deliberate typo
        kp.backspace();             // ...corrected
        kp.pushDigit((code/10)%10);
        kp.pushDigit(code%10);
        bool h2 = (kp.value() == code);
        ok = ok && h2; x3::logInfo(std::string("  H2 digit/backspace -> code: ") + (h2?"PASS":"FAIL"));
        // H3: submitting the CORRECT code opens the hatch + clears the buffer.
        bool h3 = (submit() && hatchOpen && kp.empty());
        ok = ok && h3; x3::logInfo(std::string("  H3 correct-code opens hatch: ") + (h3?"PASS":"FAIL"));
        // H4: the code is NOT the Spire/Club 1127 secret (guards against re-keying drift).
        bool h4 = (kShowroomHatchCode != 1127);
        ok = ok && h4; x3::logInfo(std::string("  H4 code != 1127 (Spire secret): ") + (h4?"PASS":"FAIL"));
        x3::logInfo(std::string("hatch-keypad smoke: ") + (ok?"ALL PASS":"FAILED"));
        return ok ? 0 : 1;
    }
    if (tf.testElevator) {
        x3::logInfo("running advanced elevator (call/travel/carry) self-test (E1-E6)...");
        return x3::game::runElevatorSelfTest() ? 0 : 1;
    }
    if (tf.testElevatorFsm) {
        x3::logInfo("running souped-up strata/disco elevator FSM self-test "
                    "(10-state FSM + strata + 1127 disco -> Club 1127)...");
        return x3::game::runElevatorFsmSelfTest() ? 0 : 1;
    }
    if (tf.testNet) {
        x3::logInfo("running netcode (Subsystem N, Phase 0) self-test "
                    "(loopback round-trip + generation-stale reject + fixed-step determinism)...");
        return x3::net::runNetworkSelfTest() ? 0 : 1;
    }
    if (tf.testNetSync) {
        x3::logInfo("running netcode (Subsystem N, Phase 0b) client/server "
                    "input->snapshot routing self-test "
                    "(command send -> server apply+sim -> snapshot -> client mirror)...");
        return x3::net::runNetSyncSelfTest() ? 0 : 1;
    }
    if (tf.testNetInterp) {
        x3::logInfo("running netcode (Subsystem N, Phase 0c) client snapshot "
                    "interpolation + jitter-buffer self-test "
                    "(jittered snapshots -> bracketed lerp/slerp -> smooth render)...");
        return x3::net::runNetInterpSelfTest() ? 0 : 1;
    }
    if (tf.testNetPredict) {
        x3::logInfo("running netcode (Subsystem N, Phase 1) client prediction + "
                    "server reconciliation self-test "
                    "(predict immediately -> lagged authority+ack -> rollback/resim)...");
        return x3::net::runNetPredictSelfTest() ? 0 : 1;
    }
    if (tf.testRescue) {
        x3::logInfo("running F2 rescue (victim/companion/transform) self-test (R0-R5)...");
        return x3::game::runRescueSelfTest() ? 0 : 1;
    }
    if (tf.testThirdPerson) {
        x3::logInfo("running third-person view (Jake avatar + follow cam + held weapon) self-test (TP1-TP9)...");
        return x3::game::runThirdPersonSelfTest() ? 0 : 1;
    }
    if (tf.testNpcTalk) {
        x3::logInfo("running rescued-NPC talk/dialog -> companion self-test (T1-T7)...");
        return x3::game::runNpcTalkSelfTest() ? 0 : 1;
    }
    if (tf.testChatTree) {
        x3::logInfo("running x3.chattree/1 dialog-runner self-test (parse all 8 trees + "
                    "the lena walk: gates/fx/follow/1278/banter/flags round-trip)...");
        return x3::game::runChatTreeSelfTest() ? 0 : 1;
    }
    if (tf.testMission) {
        x3::logInfo("running x3.mission/1 mission-runner self-test (doc parse/validate + "
                    "stage advance via flag/trigger/kill bridges + branch/fail/resume + "
                    "the Level-1 doc-vs-hardcoded objective EQUIVALENCE walk)...");
        return x3::game::runMissionSelfTest() ? 0 : 1;
    }
    if (tf.testDestruction) {
        x3::logInfo("running K-T0/T1 destruction (fracture/impact/hit/explosion) self-test...");
        return x3::phys::runDestructionSelfTest() ? 0 : 1;
    }
    if (tf.testDebris) {
        x3::logInfo("running K-T2 GPU-compute persistent debris world self-test "
                    "(spawn burst -> compute integrate -> fall/settle/sleep -> lifetime free)...");
        return runDebrisSelfTest() ? 0 : 1;
    }
    if (tf.testGpuSkin) {
        x3::logInfo("running GPU compute-skinning self-test (register skinned mesh -> "
                    "set known palette -> compute skin -> readback -> assert vs CPU LBS)...");
        return runGpuSkinSelfTest() ? 0 : 1;
    }
    if (tf.testMeshlet) {
        x3::logInfo("running D15 Tier-2 meshlet builder self-test "
                    "(grid mesh -> buildMeshlets -> assert budgets/locality/sphere/"
                    "cone/triangle-conservation/degenerate-input)...");
        return x3::rhi::runMeshletSelfTest() ? 0 : 1;
    }
    if (tf.testGpuCull) {
        x3::logInfo("running D15 Tier-0 GPU cull equivalence self-test "
                    "(headless device + validation, r_cullpath 1, pose sweep, "
                    "GPU statDrawn vs CPU predicate — must match EXACTLY)...");
        return runGpuCullSelfTest() ? 0 : 1;
    }
    if (tf.testVisUnify) {
        x3::logInfo("running vis-unify acceptance self-test (policy table + "
                    "conservation across r_vis levels on a still camera + alias-cvar "
                    "mapping + TLAS-mutation ZERO-sync-wait proof on the double-buffer)...");
        return runVisUnifySelfTest() ? 0 : 1;
    }
    if (tf.testCollapse) {
        x3::logInfo("running K-T3 structural collapse (support graph) self-test "
                    "(destroy a support -> unsupported sub-graph falls, anchored stays, "
                    "rubble settles, GPU debris fires)...");
        return x3::phys::runCollapseSelfTest() ? 0 : 1;
    }
    if (tf.testNav) {
        x3::logInfo("running GENERAL navigation (nav grid + A* + path-follow) self-test...");
        return x3::ai::runNavSelfTest() ? 0 : 1;
    }
    if (tf.testWeapons) {
        x3::logInfo("running data-driven weapon arsenal (switch/fire/reload/spread) self-test...");
        return x3::game::runWeaponsSelfTest() ? 0 : 1;
    }
    if (tf.testLightningCharge) {
        x3::logInfo("running lightning-gun CHARGE model self-test...");
        return x3::game::runLightningChargeSelfTest() ? 0 : 1;
    }
    if (tf.testScript) {
        x3::logInfo("running D14 Lua script-system self-test "
                    "(load/init/update/events/sandbox/error-containment/hot-reload/"
                    "timers/eval/cvar-bridge/memory)...");
        return x3::script::runScriptSelfTest() ? 0 : 1;
    }
    if (tf.testVehicle) {
        x3::logInfo("running vehicle framework self-test "
                    "(wheeled accel/steer + buoyancy waterline + flight thrust/lift)...");
        const bool frameworkOk = x3::phys::runVehicleSelfTest();
        x3::logInfo("running DRIVE enter/exit self-test "
                    "(spawn -> E enter -> throttle 4 s -> displacement + wheel contact -> E exit restores control)...");
        const bool driveOk = x3::game::runDriveEnterExitSelfTest();
        return (frameworkOk && driveOk) ? 0 : 1;
    }
    if (tf.testCanonVehicle) {
        x3::logInfo("running WORLD CARS canon-vehicle self-test "
                    "(park/enter/drive/exit + hold-E hack + unlocked-latch region persistence)...");
        return x3::game::runCanonVehicleSelfTest() ? 0 : 1;
    }
    if (tf.testVehParts) {
        x3::logInfo("running PERFORMANCE PARTS (x3.vehparts/1) self-test "
                    "(catalog parse + composition math + REAL Jolt physics deltas + dyno pop thresholds)...");
        return x3::game::vehparts::runVehPartsSelfTest() ? 0 : 1;
    }
    if (tf.testEcology) {
        x3::logInfo("running AMBIENT ECOLOGY self-test "
                    "(herd cohesion + flee + patrol routes + schedule switch + soft-radius + leak)...");
        return x3::game::runEcologySelfTest() ? 0 : 1;
    }
    if (tf.testCrowd) {
        x3::logInfo("running CROWDS self-test "
                    "(idle clusters + wander points + scatter/cower on violence + return after calm)...");
        return x3::game::runCrowdSelfTest() ? 0 : 1;
    }
    if (tf.testAlert) {
        x3::logInfo("running FACILITY ALERT LEVEL self-test "
                    "(stimulus->level transitions + decay clocks + lockdown doors + witness-vs-unseen + x3.fire)...");
        return x3::game::runAlertSelfTest() ? 0 : 1;
    }
    if (tf.testFootIk) {
        x3::logInfo("running foot-IK (two-bone + plant + pelvis) self-test...");
        return x3::anim::runFootIkSelfTest() ? 0 : 1;
    }
    if (tf.testUi) {
        x3::logInfo("running GENERAL game-UI self-test "
                    "(button hit-test + Menu<->Playing<->Paused transitions + settings cvar wiring)...");
        return x3::ui::runUiSelfTest() ? 0 : 1;
    }
    if (tf.testLoading) {
        x3::logInfo("running EFLZ loading-screen self-test "
                    "(monotonic progress 0->1 + tip rotation + fade-in/out)...");
        return x3::game::runLoadingSelfTest() ? 0 : 1;
    }
    if (tf.testSaveLoad) {
        x3::logInfo("running GENERAL versioned checkpoint save/load self-test "
                    "(round-trip field-by-field + magic/version/checksum/truncation reject)...");
        return x3::save::runSaveLoadSelfTest() ? 0 : 1;
    }
    if (tf.testDialog) {
        x3::logInfo("running AI-dialog + TTS self-test "
                    "(offline tree advance + branches; stub AI provider used/fallback; "
                    "stub TTS drives NPC speaking state; no network; leak-clean)...");
        return x3::dialog::runDialogSelfTest() ? 0 : 1;
    }
    if (tf.demoDialog) {
        // Headless, fully offline demo: drive the sample Sarah conversation onto a
        // REAL skinned NPC (chief_martinez_anim.glb) via the SpeakingMonster adapter
        // + an offline stub TTS hook. Proves requirement 4 end-to-end against the
        // actual anim Skinner (read-only) without a window / device / network.
        namespace fs = std::filesystem;
        std::string glb = tf.demoDialogPath.empty()
            ? (x3::game::riggedGlbRoot() + "/chief_martinez_anim.glb")
            : tf.demoDialogPath;
        x3::logInfo("--demo-dialog: NPC model = " + glb);

        // Load the skinned model headlessly (loader pattern from --list-clips).
        x3::asset::Model model;
        std::unique_ptr<x3::asset::IAssetSource> src;
        std::unique_ptr<x3::asset::IModelLoader> loader;
        bool haveModel = false;
        if (fs::exists(glb)) {
            fs::path p(glb);
            src.reset(x3::asset::createAssetSource());
            src->mountDir(p.parent_path().string(), 0);
            loader.reset(x3::asset::createModelLoader(nullptr, src.get())); // headless
            model = loader->load(p.filename().string());
            haveModel = model.ok;
        }
        if (!haveModel) {
            x3::logInfo("--demo-dialog: model absent/failed (clean checkout) — running "
                        "subtitle-only (the conversation + speaking lifecycle still run).");
        }

        SpeakingMonster npc;
        if (haveModel) {
            bool sk = npc.bind(model);
            x3::logInfo(std::string("--demo-dialog: NPC skinnable = ") + (sk ? "yes" : "no"));
        }

        // Offline stub TTS: deterministic clip duration from the line length; NO
        // file I/O, NO network. (Tim's real voice vendor drops in here as the hook.)
        auto stubTts = [](const std::string& line, x3::dialog::VoiceId v) {
            x3::dialog::AudioClip c;
            c.path = std::string("stub://voice/") + x3::dialog::voiceName(v);
            c.durationSec = 0.6f + 0.012f * (float)line.size();
            return c;
        };

        std::unique_ptr<x3::dialog::IDialogSystem> d(x3::dialog::createDialogSystem());
        d->setTtsProvider(stubTts);
        d->setSpeakingNpc(&npc);
        // No AI provider set -> the authored tree is used (offline baseline). Mode
        // stays Tree; the demo shows the guaranteed path.
        x3::dialog::Tree tree = x3::dialog::sampleSarahTree();

        if (!d->start(tree)) { x3::logError("--demo-dialog: tree failed to start"); return 1; }

        // Drive the conversation: tick each line to completion, print the choices,
        // auto-pick choice 0 (deterministic), until the conversation ends.
        int safety = 0;
        while (d->active() && safety++ < 64) {
            // Tick the current line to completion (the speaking state runs here).
            int t = 0;
            while (d->speaking() && t++ < 4000) { d->update(0.02f); }
            const auto& ch = d->choices();
            if (ch.empty()) {
                // Terminal node: the line finished, conversation will end on the next
                // active() check (the system ended it inside the final exitSpeaking()).
                break;
            }
            x3::logInfo("  [choices]");
            for (size_t k = 0; k < ch.size(); ++k)
                x3::logInfo("    " + std::to_string(k) + ") " + ch[k].text);
            d->choose(0);   // auto-advance down the first branch
        }

        bool ok = npc.beginCount() >= 1 && npc.beginCount() == npc.endCount() && !d->active();
        x3::logInfo("--demo-dialog: lines spoken = " + std::to_string(npc.beginCount()) +
                    ", speaking enter==exit = " + (npc.beginCount() == npc.endCount() ? "yes" : "NO") +
                    ", conversation ended = " + (!d->active() ? "yes" : "NO"));
        if (haveModel && npc.skinnable()) {
            // The talk-bob must have actually moved the skeleton while speaking.
            bool moved = npc.maxPaletteDelta() > 1e-4f;
            x3::logInfo("--demo-dialog: talk-bob palette max-delta = " +
                        std::to_string(npc.maxPaletteDelta()) + (moved ? " (animated)" : " (STATIC!)"));
            ok = ok && moved;
        }
        if (loader && haveModel) loader->unload(model);
        x3::logInfo(std::string("--demo-dialog: ") + (ok ? "PASS" : "FAIL"));
        return ok ? 0 : 1;
    }
    if (tf.testValley) {
        x3::logInfo("running Crystal Valleys (Act 2, L15) self-test "
                    "(terrain placement + crash/K'thara on surface + Dominion + water)...");
        return x3::game::runValleySelfTest() ? 0 : 1;
    }
    if (tf.testCliffs) {
        x3::logInfo("running Salvari cliffs finale self-test (pad/sea/placement/streaming)...");
        return x3::game::runCliffsSelfTest() ? 0 : 1;
    }
    if (tf.testClub) {
        x3::logInfo("running Club 1127 (\"THE DEEP\") self-test "
                    "(build at Y=-200; assert DJ booth/ORB/bars/stair/PA/blacklights/TVs/footprint; leak-clean)...");
        return x3::game::runClubSelfTest() ? 0 : 1;
    }
    if (tf.testPerfshop) {
        x3::logInfo("running LATE NIGHT SPEED perf-shop self-test "
                    "(build headless; assert the terminal glass + neon sign are DISPLAYS: "
                    "per-texel emissive, ink on a dark substrate, texture-gated tubes)...");
        return x3::game::runPerfShopSelfTest() ? 0 : 1;
    }
    // ---- Space-combat stack (folded from feat/cockpit-vattalus) -----------
    if (tf.testSpace) {
        x3::logInfo("running space-pilot (Act-3 6DOF) --test-space self-test...");
        return x3::game::runSpaceSelfTest() ? 0 : 1;
    }
    if (tf.testEva) {
        x3::logInfo("running EVA spacewalk (Act-3 S12 zero-G) --test-eva self-test...");
        return x3::space::runEvaSelfTest() ? 0 : 1;
    }
    if (tf.testShipAi) {
        x3::logInfo("running S8 enemy ship-AI (dogfight) self-test...");
        return x3::space::runShipAiSelfTest() ? 0 : 1;
    }
    // --test-targeting: S9 targeting / radar / lock-on self-test. Headless.
    // Lifted VERBATIM from the pre-split main() inline block.
    if (tf.testTargeting) {
        x3::logInfo("running S9 targeting / radar / lock-on self-test...");
        int pass = 0, total = 0;
        auto check = [&](bool c, const char* name) {
            ++total;
            if (c) { ++pass; x3::logInfo(std::string("  [ok] ") + name); }
            else   {          x3::logError(std::string("  [FAIL] ") + name); }
        };

        using x3::space::TargetingSystem;
        using x3::space::Contact;
        using x3::space::LeadSolution;

        // A deterministic scene: two hostiles (ids 10, 20) ahead along +X at
        // different ranges, one friendly (id 5) ahead, one hostile (id 30)
        // behind. Hostile 10 sits dead on the +X boresight; 20 is off-axis.
        Contact cs[4]{};
        cs[0] = { 10u, { 100.0f,   0.0f,   0.0f }, { 0.0f, 0.0f, 0.0f }, true  }; // on-axis, near
        cs[1] = { 20u, { 200.0f,  40.0f,   0.0f }, { 0.0f, 0.0f, 0.0f }, true  }; // off-axis, far
        cs[2] = {  5u, { 120.0f,   0.0f,   0.0f }, { 0.0f, 0.0f, 0.0f }, false }; // friendly, on-axis
        cs[3] = { 30u, { -80.0f,   0.0f,   0.0f }, { 0.0f, 0.0f, 0.0f }, true  }; // behind

        // T1: setContacts + no lock initially.
        {
            TargetingSystem ts;
            ts.setContacts(cs, 4);
            check(ts.contactCount() == 4u, "T1 setContacts stores all contacts");
            check(!ts.hasLock(), "T1b no lock before any lock request");
        }

        // T2: lockNearest picks the in-cone hostile, skipping the closer friendly.
        {
            TargetingSystem ts;
            ts.setContacts(cs, 4);
            const float from[3] = { 0.0f, 0.0f, 0.0f };
            const float fwd[3]  = { 1.0f, 0.0f, 0.0f };
            ts.lockNearest(from, fwd);
            check(ts.hasLock() && ts.lockedId() == 10u,
                  "T2 lockNearest locks the on-axis hostile (id 10), not the friendly");
        }

        // T3: cycleTarget advances through HOSTILES only, skipping the friendly.
        {
            TargetingSystem ts;
            ts.setContacts(cs, 4);
            ts.cycleTarget(+1); // first hostile in list order: id 10
            check(ts.hasLock() && ts.lockedId() == 10u, "T3 cycle +1 -> first hostile id 10");
            ts.cycleTarget(+1); // next hostile: id 20 (skips friendly id 5)
            check(ts.lockedId() == 20u, "T3b cycle +1 -> next hostile id 20 (skips friendly)");
            ts.cycleTarget(+1); // next hostile: id 30 (wraps past friendly)
            check(ts.lockedId() == 30u, "T3c cycle +1 -> hostile id 30");
            ts.cycleTarget(+1); // wraps back to id 10
            check(ts.lockedId() == 10u, "T3d cycle +1 wraps back to id 10");
            ts.cycleTarget(-1); // back to id 30
            check(ts.lockedId() == 30u, "T3e cycle -1 wraps to id 30");
        }

        // T4: computeLead returns a VALID intercept for a crossing mover.
        {
            TargetingSystem ts;
            // Single hostile crossing in +Z at 30 m/s, 100 m straight ahead.
            Contact mover{ 99u, { 100.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 30.0f }, true };
            ts.setContacts(&mover, 1);
            ts.cycleTarget(+1);
            const float shooter[3] = { 0.0f, 0.0f, 0.0f };
            LeadSolution s = ts.computeLead(shooter, 300.0f); // fast projectile
            // Lead must be AHEAD of the target in its direction of travel (+Z>0)
            // and within physical bounds of a real intercept.
            check(s.valid && s.aimPoint[2] > 0.5f && s.distance > 99.0f,
                  "T4 computeLead: valid lead point ahead of crossing target");
            // Sanity: the aim point's flight time matches projectile travel.
            float dz = s.aimPoint[2];
            float flightT = s.distance / 300.0f;
            check(std::fabs(dz - 30.0f * flightT) < 1.0f,
                  "T4b lead point is consistent with intercept time");
        }

        // T5: computeLead INVALID when the projectile is too slow to catch a
        // target receding faster than the projectile flies.
        {
            TargetingSystem ts;
            Contact fleeing{ 88u, { 100.0f, 0.0f, 0.0f }, { 50.0f, 0.0f, 0.0f }, true };
            ts.setContacts(&fleeing, 1);
            ts.cycleTarget(+1);
            const float shooter[3] = { 0.0f, 0.0f, 0.0f };
            LeadSolution s = ts.computeLead(shooter, 10.0f); // slower than the target
            check(!s.valid, "T5 computeLead invalid: projectile too slow to intercept");
        }

        // T6: computeLead invalid with no lock.
        {
            TargetingSystem ts;
            ts.setContacts(cs, 4);
            const float shooter[3] = { 0.0f, 0.0f, 0.0f };
            LeadSolution s = ts.computeLead(shooter, 300.0f);
            check(!s.valid, "T6 computeLead invalid without a lock");
        }

        // T7: radarBlips projects within range, drops out-of-range, flags locked.
        {
            TargetingSystem ts;
            ts.setContacts(cs, 4);
            ts.cycleTarget(+1); // lock id 10 (on +X boresight)
            const float pPos[3] = { 0.0f, 0.0f, 0.0f };
            const float pFwd[3] = { 1.0f, 0.0f, 0.0f }; // facing +X -> radar up
            TargetingSystem::Blip blips[8]{};
            // Range 150: includes id 10 (100), friendly 5 (120), behind 30 (80);
            // excludes id 20 (planar 200).
            uint32_t nb = ts.radarBlips(blips, 8, pPos, pFwd, 150.0f);
            check(nb == 3u, "T7 radarBlips range-gates out the far contact (3 of 4)");
            // Find id 10's blip: facing +X, a +X target projects to +Y (up),
            // near +1 (range 100 / 150 ~= 0.67), x near 0.
            bool okLockedBlip = false;
            for (uint32_t i = 0; i < nb; ++i) {
                if (blips[i].id == 10u) {
                    okLockedBlip = blips[i].locked &&
                                   blips[i].hostile &&
                                   blips[i].radarXY[1] > 0.5f &&
                                   std::fabs(blips[i].radarXY[0]) < 0.05f;
                }
            }
            check(okLockedBlip, "T7b locked on-axis hostile projects to radar up, flagged locked");
            // The contact behind (id 30) projects to -Y (down).
            bool behindDown = false;
            for (uint32_t i = 0; i < nb; ++i)
                if (blips[i].id == 30u && blips[i].radarXY[1] < -0.3f) behindDown = true;
            check(behindDown, "T7c contact behind the player projects to radar down");
        }

        // T8: clearLock + lock auto-drops when the locked contact disappears.
        {
            TargetingSystem ts;
            ts.setContacts(cs, 4);
            ts.cycleTarget(+1);
            check(ts.hasLock(), "T8 lock acquired");
            ts.clearLock();
            check(!ts.hasLock(), "T8b clearLock releases the lock");
            // Re-lock, then feed a list WITHOUT the locked id -> auto-clear.
            ts.cycleTarget(+1);
            check(ts.hasLock() && ts.lockedId() == 10u, "T8c re-locked id 10");
            Contact gone[1] = { { 20u, { 200.0f, 40.0f, 0.0f }, { 0,0,0 }, true } };
            ts.setContacts(gone, 1);
            check(!ts.hasLock(), "T8d lock auto-drops when locked contact leaves the feed");
        }

        x3::logInfo("targeting: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
        std::printf("targeting: %d/%d passed\n", pass, total);
        std::fflush(stdout);
        return (pass == total) ? 0 : 1;
    }
    // --test-ship-damage: S10 ship damage model self-test.
    if (tf.testShipDamage) {
        x3::logInfo("running S10 ship-damage model self-test...");
        return x3::space::runShipDamageSelfTest() ? 0 : 1;
    }

    return -1;   // no test flag matched — main() continues normal boot
}

} // namespace x3::apphost
