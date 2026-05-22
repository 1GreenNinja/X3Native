add_executable(X3Engine WIN32
    main.cpp
    scene.cpp
    level.cpp
    level1.cpp
    player.cpp
    door.cpp
    elevator.cpp
    weapon.cpp
    monster.cpp
    anim.cpp
    melee.cpp
    objective.cpp
    trigger.cpp
    rescue.cpp
    level1_game.cpp
    spire_mid.cpp
    env_art.cpp
    club1127.cpp
    terrain.cpp
    vehicle.cpp
    fx.cpp
    hud.cpp
    ui.cpp)

target_link_libraries(X3Engine PRIVATE x3engine glfw)

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
    ${CMAKE_SOURCE_DIR}/shaders/mesh.frag
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
    ${CMAKE_SOURCE_DIR}/shaders/debris.frag)
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
add_custom_target(x3shaders DEPENDS ${SPV_FILES})
add_dependencies(X3Engine x3shaders)

# Copy compiled .spv next to the exe so runtime loads <exedir>/shaders/*.spv
add_custom_command(TARGET X3Engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:X3Engine>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${SPV_DIR} $<TARGET_FILE_DIR:X3Engine>/shaders)
