add_executable(X3Engine WIN32
    main.cpp
    scene.cpp
    level.cpp
    level1.cpp
    level_loader.cpp
    player.cpp
    swim_controller.cpp
    mech_pilot.cpp
    door.cpp
    physprops.cpp
    ragdoll.cpp
    barrels.cpp
    glass_test.cpp
    holo_terminal.cpp
    holo_terminal_system.cpp
    glass_lounge.cpp
    secret_room.cpp
    ecs_render.cpp
    editor/editor.cpp
    elevator.cpp
    weapon.cpp
    monster.cpp
    npc.cpp
    anim.cpp
    thirdperson.cpp
    melee.cpp
    objective.cpp
    trigger.cpp
    rescue.cpp
    npc_dialog.cpp
    level1_game.cpp
    canon_play.cpp
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
    city.cpp
    ocean_base.cpp
    save.cpp
    dialog.cpp
    env_art.cpp
    club1127.cpp
    cues.cpp
    valley.cpp
    cliffs.cpp
    companion.cpp
    companion_squad.cpp
    companion_controller.cpp
    terrain.cpp
    vehicle.cpp
    fx.cpp
    hud.cpp
    space_pilot.cpp
    space/ship_anim.cpp
    sky_stars.cpp
    space/space_layer.cpp
    space/lod.cpp
    space/space_env.cpp
    space/wormhole_vfx.cpp
    space/wormhole_transit.cpp
    space/descent.cpp
    space/ship_interior.cpp
    space/ship_ai.cpp
    space/targeting.cpp
    space/ship_damage.cpp
    ui.cpp)

target_link_libraries(X3Engine PRIVATE x3engine glfw)

# holo_terminal.cpp rasterizes its on-glass readout text into the hologram
# texture with stb_truetype (the same single-header lib the engine font atlas
# uses). The implementation lives in the engine TU, so the app only needs the
# header on its include path (declarations, no STB_TRUETYPE_IMPLEMENTATION here).
find_package(Stb REQUIRED)
target_include_directories(X3Engine PRIVATE ${Stb_INCLUDE_DIR})

# WIN32 subsystem but we want a console for logs during dev.
# /STACK:16MB — main() aggregates ~9 large inline `--world` showcase blocks; MSVC
# reserves the SUM of every block's locals as one frame at function entry, which
# already sits near the 1 MB default stack reserve. Reaching the worldMode dispatch
# (deep main frame) and then calling a showcase helper (e.g. runMechWorld) puts a
# second large frame on top and overflows the 1 MB default at startup. Bump the
# reserve so the host has comfortable headroom (and the showcase helpers can keep
# their natural stack-local scene setup).
if(MSVC)
    set_target_properties(X3Engine PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE /STACK:16777216")
endif()

set_target_properties(X3Engine PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---- Compile GLSL shaders to SPIR-V (glslc from the Vulkan SDK) ----
set(SHADER_SRCS
    ${CMAKE_SOURCE_DIR}/shaders/tri.vert
    ${CMAKE_SOURCE_DIR}/shaders/tri.frag
    ${CMAKE_SOURCE_DIR}/shaders/mesh.vert
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
    ${CMAKE_SOURCE_DIR}/shaders/depth.vert
    ${CMAKE_SOURCE_DIR}/shaders/ssao.frag
    ${CMAKE_SOURCE_DIR}/shaders/ssao_blur.frag
    ${CMAKE_SOURCE_DIR}/shaders/ssgi_gather.frag
    ${CMAKE_SOURCE_DIR}/shaders/ssgi_temporal.frag
    ${CMAKE_SOURCE_DIR}/shaders/ssgi_blur.frag
    ${CMAKE_SOURCE_DIR}/shaders/ssgi_apply.frag
    ${CMAKE_SOURCE_DIR}/shaders/debris.comp
    ${CMAKE_SOURCE_DIR}/shaders/debris.vert
    ${CMAKE_SOURCE_DIR}/shaders/debris.frag
    ${CMAKE_SOURCE_DIR}/shaders/skin.comp
    ${CMAKE_SOURCE_DIR}/shaders/rtao_apply.frag
    ${CMAKE_SOURCE_DIR}/shaders/starfield.vert
    ${CMAKE_SOURCE_DIR}/shaders/starfield.frag
    ${CMAKE_SOURCE_DIR}/shaders/wormhole.vert
    ${CMAKE_SOURCE_DIR}/shaders/wormhole.frag)
# Hardware ray-tracing (ray-query) shaders need SPIR-V 1.4+ and the ray-query
# extension target; compiled with explicit target flags below. Listed separately
# so the default glslc invocation (which targets an older SPIR-V) is not used.
set(SHADER_SRCS_RT
    ${CMAKE_SOURCE_DIR}/shaders/rtao.comp)
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
add_custom_target(x3shaders DEPENDS ${SPV_FILES})
add_dependencies(X3Engine x3shaders)

# Copy compiled .spv next to the exe so runtime loads <exedir>/shaders/*.spv
add_custom_command(TARGET X3Engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:X3Engine>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${SPV_DIR} $<TARGET_FILE_DIR:X3Engine>/shaders)
