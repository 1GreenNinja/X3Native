add_executable(X3Engine WIN32
    main.cpp
    bindings.cpp
    self_tests.cpp
    test_registry.cpp
    cinematic.cpp
    # #28 deep monolith split — CLI parse (arg loop -> CliOptions)
    cli.cpp
    # #28 deep monolith split — default host (interactive render loop)
    app_run.cpp
    # #28 deep monolith split — headless screenshot/capture handlers
    screenshot_hosts.cpp
    # #28 deep monolith split — extracted self-contained --world hosts
    world_hosts/world_hosts.cpp
    world_hosts/host_destruct.cpp
    world_hosts/host_physjoint.cpp
    world_hosts/host_ragdoll.cpp
    world_hosts/host_drive.cpp
    world_hosts/host_club.cpp
    world_hosts/host_showroom.cpp
    world_hosts/host_valley.cpp
    world_hosts/host_cliffs.cpp
    world_hosts/host_streamed.cpp
    # Phase 2 — space-combat stack folded from feat/cockpit-vattalus
    world_hosts/host_space.cpp
    world_hosts/host_strata.cpp
    world_hosts/host_elevator.cpp
    # Phase 7 — ESCAPED-branch Act-1 surface-landing start (--world surface)
    world_hosts/host_surface_start.cpp
    space_pilot.cpp
    space/ship_ai.cpp
    space/targeting.cpp
    space/ship_damage.cpp
    space/space_layer.cpp
    space/descent.cpp
    space/eva.cpp
    space/lod.cpp
    space/decloak_vfx.cpp
    space/space_env.cpp
    # Phase 3 — interactive branching cold-open: the Intro Orchestrator.
    intro_orchestrator.cpp
    scene.cpp
    level.cpp
    level1.cpp
    level_lint.cpp
    level_loader.cpp
    leveldoc_world.cpp
    player.cpp
    door.cpp
    keypad.cpp
    physprops.cpp
    ragdoll.cpp
    barrels.cpp
    glass_test.cpp
    holo_terminal.cpp
    secret_room.cpp
    ecs_render.cpp
    editor/editor.cpp
    editor/editor_keybinds.cpp
    editor/editor_host.cpp
    elevator.cpp
    elevator_showcase.cpp
    strata.cpp
    weapon.cpp
    monster.cpp
    anim.cpp
    thirdperson.cpp
    melee.cpp
    objective.cpp
    trigger.cpp
    rescue.cpp
    npc_dialog.cpp
    story_ops.cpp
    chat_tree.cpp
    mission.cpp
    level1_game.cpp
    canon_play.cpp
    canon_45.cpp
    cell_dressing.cpp
    room_dressing.cpp
    surface_library.cpp
    intro_coldopen.cpp
    cutscene.cpp
    spire_mid.cpp
    spire_top.cpp
    spire_nexus.cpp
    spire_sublevels.cpp
    timeline.cpp
    act2_world.cpp
    act2_desert.cpp
    act2_caves.cpp
    tod.cpp
    weather.cpp
    world_regions.cpp
    world_stream.cpp
    world_map.cpp
    city.cpp
    ocean_base.cpp
    save.cpp
    dialog.cpp
    env_art.cpp
    club1127.cpp
    cues.cpp
    valley.cpp
    cliffs.cpp
    terrain.cpp
    vehicle.cpp
    vehparts.cpp
    perfshop.cpp
    ecology.cpp
    crowd.cpp
    alert.cpp
    fx.cpp
    hud.cpp
    loading_screen.cpp
    canon_aliens.cpp
    ui.cpp)

target_link_libraries(X3Engine PRIVATE x3engine glfw)

# Dear ImGui (docking) — EDITOR-ONLY. The Level Architect host (app/editor/
# editor_host.cpp) submits its panels with the CORE ImGui widget API directly; the
# GLFW/Vulkan backends still live (PRIVATE) in the engine device. Linking the imgui
# core lib here is additive — the game path never calls these unless --editor is set.
find_package(imgui CONFIG REQUIRED)
target_link_libraries(X3Engine PRIVATE imgui::imgui)

# holo_terminal.cpp rasterizes its on-glass readout text into the hologram
# texture with stb_truetype (the same single-header lib the engine font atlas
# uses). The implementation lives in the engine TU, so the app only needs the
# header on its include path (declarations, no STB_TRUETYPE_IMPLEMENTATION here).
find_package(Stb REQUIRED)
target_include_directories(X3Engine PRIVATE ${Stb_INCLUDE_DIR})

# WIN32 subsystem but we want a console for logs during dev:
if(MSVC)
    set_target_properties(X3Engine PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
endif()

set_target_properties(X3Engine PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---- Compile GLSL shaders to SPIR-V (glslc from the Vulkan SDK) ----
set(SHADER_SRCS
    ${CMAKE_SOURCE_DIR}/shaders/tri.vert
    ${CMAKE_SOURCE_DIR}/shaders/tri.frag
    ${CMAKE_SOURCE_DIR}/shaders/mesh.vert
    ${CMAKE_SOURCE_DIR}/shaders/mesh_probe.vert
    ${CMAKE_SOURCE_DIR}/shaders/mesh.frag
    ${CMAKE_SOURCE_DIR}/shaders/glass.frag
    ${CMAKE_SOURCE_DIR}/shaders/shadow.vert
    ${CMAKE_SOURCE_DIR}/shaders/sky.vert
    ${CMAKE_SOURCE_DIR}/shaders/sky.frag
    ${CMAKE_SOURCE_DIR}/shaders/water.vert
    ${CMAKE_SOURCE_DIR}/shaders/water.frag
    ${CMAKE_SOURCE_DIR}/shaders/particle.vert
    ${CMAKE_SOURCE_DIR}/shaders/particle.frag
    ${CMAKE_SOURCE_DIR}/shaders/decal.vert
    ${CMAKE_SOURCE_DIR}/shaders/decal.frag
    ${CMAKE_SOURCE_DIR}/shaders/hud.vert
    ${CMAKE_SOURCE_DIR}/shaders/hud.frag
    ${CMAKE_SOURCE_DIR}/shaders/fullscreen.vert
    ${CMAKE_SOURCE_DIR}/shaders/bloom_down.frag
    ${CMAKE_SOURCE_DIR}/shaders/bloom_up.frag
    ${CMAKE_SOURCE_DIR}/shaders/composite.frag
    ${CMAKE_SOURCE_DIR}/shaders/fog.frag
    ${CMAKE_SOURCE_DIR}/shaders/taa_resolve.frag
    ${CMAKE_SOURCE_DIR}/shaders/autoexposure.comp
    ${CMAKE_SOURCE_DIR}/shaders/depth.vert
    ${CMAKE_SOURCE_DIR}/shaders/depth_cutout.vert
    ${CMAKE_SOURCE_DIR}/shaders/depth_cutout.frag
    ${CMAKE_SOURCE_DIR}/shaders/ssao.frag
    ${CMAKE_SOURCE_DIR}/shaders/ssao_blur.frag
    ${CMAKE_SOURCE_DIR}/shaders/ssgi_gather.frag
    ${CMAKE_SOURCE_DIR}/shaders/ssgi_temporal.frag
    ${CMAKE_SOURCE_DIR}/shaders/ssgi_blur.frag
    ${CMAKE_SOURCE_DIR}/shaders/ssgi_apply.frag
    ${CMAKE_SOURCE_DIR}/shaders/ibl_brdf_lut.frag
    ${CMAKE_SOURCE_DIR}/shaders/ibl_env.frag
    ${CMAKE_SOURCE_DIR}/shaders/ibl_irradiance.frag
    ${CMAKE_SOURCE_DIR}/shaders/ibl_prefilter.frag
    ${CMAKE_SOURCE_DIR}/shaders/debris.comp
    ${CMAKE_SOURCE_DIR}/shaders/debris.vert
    ${CMAKE_SOURCE_DIR}/shaders/debris.frag
    ${CMAKE_SOURCE_DIR}/shaders/skin.comp
    ${CMAKE_SOURCE_DIR}/shaders/cull.comp
    ${CMAKE_SOURCE_DIR}/shaders/hzb_build.comp
    ${CMAKE_SOURCE_DIR}/shaders/rtao_apply.frag
    ${CMAKE_SOURCE_DIR}/shaders/planet.vert
    ${CMAKE_SOURCE_DIR}/shaders/planet_moon.frag
    ${CMAKE_SOURCE_DIR}/shaders/planet_ice.frag
    ${CMAKE_SOURCE_DIR}/shaders/planet_gas.frag
    ${CMAKE_SOURCE_DIR}/shaders/planet_lava.frag
    ${CMAKE_SOURCE_DIR}/shaders/planet_terrestrial.frag
    ${CMAKE_SOURCE_DIR}/shaders/planet_oceanic.frag
    ${CMAKE_SOURCE_DIR}/shaders/planet_sand.frag
    ${CMAKE_SOURCE_DIR}/shaders/planet_thunderstorm.frag
    ${CMAKE_SOURCE_DIR}/shaders/planet_sun.frag
    ${CMAKE_SOURCE_DIR}/shaders/planet_atmosphere.frag
    ${CMAKE_SOURCE_DIR}/shaders/planet_suncorona.frag
    ${CMAKE_SOURCE_DIR}/shaders/planet_ring.frag)
# Hardware ray-tracing (ray-query) shaders need SPIR-V 1.4+ and the ray-query
# extension target; compiled with explicit target flags below. Listed separately
# so the default glslc invocation (which targets an older SPIR-V) is not used.
set(SHADER_SRCS_RT
    ${CMAKE_SOURCE_DIR}/shaders/rtao.comp
    ${CMAKE_SOURCE_DIR}/shaders/ddgi_rays.comp
    ${CMAKE_SOURCE_DIR}/shaders/ddgi_update.comp
    ${CMAKE_SOURCE_DIR}/shaders/audio_rays.comp)
set(SPV_DIR ${CMAKE_BINARY_DIR}/shaders_spv)
file(MAKE_DIRECTORY ${SPV_DIR})
set(SPV_FILES "")
foreach(s ${SHADER_SRCS})
    get_filename_component(fn ${s} NAME)
    set(out ${SPV_DIR}/${fn}.spv)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} ${s} -o ${out}
        DEPENDS ${s}
        COMMENT "glslc ${fn}"
        VERBATIM)
    list(APPEND SPV_FILES ${out})
endforeach()
# Ray-query shaders: target Vulkan 1.3 + SPIR-V 1.4 (required for GL_EXT_ray_query).
foreach(s ${SHADER_SRCS_RT})
    get_filename_component(fn ${s} NAME)
    set(out ${SPV_DIR}/${fn}.spv)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 --target-spv=spv1.4 ${s} -o ${out}
        DEPENDS ${s}
        COMMENT "glslc (ray-query) ${fn}"
        VERBATIM)
    list(APPEND SPV_FILES ${out})
endforeach()
# Reflections (refl.comp) builds TWO variants from one source:
#   refl.comp.spv    — SSR-only (no ray query; runs on any Vulkan 1.3 device)
#   refl_rt.comp.spv — SSR + inline ray-query fallback (-DREFL_RT=1; RT devices)
add_custom_command(
    OUTPUT ${SPV_DIR}/refl.comp.spv
    COMMAND ${Vulkan_GLSLC_EXECUTABLE} ${CMAKE_SOURCE_DIR}/shaders/refl.comp -o ${SPV_DIR}/refl.comp.spv
    DEPENDS ${CMAKE_SOURCE_DIR}/shaders/refl.comp
    COMMENT "glslc refl.comp (SSR)"
    VERBATIM)
add_custom_command(
    OUTPUT ${SPV_DIR}/refl_rt.comp.spv
    COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 --target-spv=spv1.4 -DREFL_RT=1
            ${CMAKE_SOURCE_DIR}/shaders/refl.comp -o ${SPV_DIR}/refl_rt.comp.spv
    DEPENDS ${CMAKE_SOURCE_DIR}/shaders/refl.comp
    COMMENT "glslc refl.comp (SSR + ray-query) -> refl_rt.comp.spv"
    VERBATIM)
list(APPEND SPV_FILES ${SPV_DIR}/refl.comp.spv ${SPV_DIR}/refl_rt.comp.spv)
# RT soft shadows (r_rtshadows): mesh.frag builds a SECOND variant with inline
# ray-query shadow rays (-DRT_SHADOWS=1 -> mesh_rt.frag.spv; bound only on
# ray-query devices). The plain mesh.frag.spv above stays byte-for-byte the
# raster path (r_rtshadows 0 / Pascal).
add_custom_command(
    OUTPUT ${SPV_DIR}/mesh_rt.frag.spv
    COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 --target-spv=spv1.4 -DRT_SHADOWS=1
            -fshader-stage=frag ${CMAKE_SOURCE_DIR}/shaders/mesh.frag -o ${SPV_DIR}/mesh_rt.frag.spv
    DEPENDS ${CMAKE_SOURCE_DIR}/shaders/mesh.frag
    COMMENT "glslc mesh.frag (ray-query soft shadows) -> mesh_rt.frag.spv"
    VERBATIM)
list(APPEND SPV_FILES ${SPV_DIR}/mesh_rt.frag.spv)
add_custom_target(x3shaders DEPENDS ${SPV_FILES})
add_dependencies(X3Engine x3shaders)

# Copy compiled .spv next to the exe so runtime loads <exedir>/shaders/*.spv
add_custom_command(TARGET X3Engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:X3Engine>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${SPV_DIR} $<TARGET_FILE_DIR:X3Engine>/shaders)
