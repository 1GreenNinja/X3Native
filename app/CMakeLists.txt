# ---------------------------------------------------------------------------
# x3app — THE HOST LAYER, as a shared library.
#
# This used to be `add_executable(X3Engine WIN32 ...)`. It is now a DLL with two
# thin executables over it (X3Engine.exe, X3LevelArchitect.exe), so the game and
# the Level Architect physically cannot run different host code.
#
# The boundary is drawn around app/ and NOT just engine/ on purpose: both of the
# bugs that motivated this were host-layer divergence (the tunnel lit in headless
# capture but black when driven; assetRoot() resolving differently per build
# layout). An engine-only DLL split would have caught neither.
# See docs/design/LEVEL_ARCHITECT_EXE_PLAN.md.
#
# main.cpp stays HERE, in the DLL — it defines x3AppMain(), not main().
# ---------------------------------------------------------------------------
add_library(x3app SHARED
    main.cpp
    bindings.cpp
    self_tests.cpp
    test_registry.cpp
    cinematic.cpp
    # x3.starsys/1 — named star-systems registry (data + validator + self-test)
    star_systems.cpp
    # #28 deep monolith split — CLI parse (arg loop -> CliOptions)
    cli.cpp
    # #28 deep monolith split — default host (interactive render loop)
    app_run.cpp
    # D-CONSOLE fold — the shared cvar/command registry app_run AND HostShell
    # both call (one registry, no drift). See app/engine_console.h.
    engine_console.cpp
    # #28 deep monolith split — headless screenshot/capture handlers
    screenshot_hosts.cpp
    # #28 deep monolith split — extracted self-contained --world hosts
    world_hosts/world_hosts.cpp
    world_hosts/host_shell.cpp
    world_hosts/host_echoharbor.cpp
    # LIFT A (ported from the echotropolis lineage, branch inspx/city-blocks):
    # the EchoRoads procedural ROAD GRAPH + geometry. Free-standing — the only
    # dependency that did not already exist on this lineage is the sibling
    # header echo_heightfield.h. Deliberately UNWIRED into any host: on this
    # lineage the sole consumer is --test-echoroads (roads_test.cpp), which
    # locks the emitted RoadGraph to a stable checksum.
    world_hosts/echo_roads.cpp
    roads_test.cpp
    world_hosts/host_destruct.cpp
    world_hosts/host_physjoint.cpp
    world_hosts/host_ragdoll.cpp
    world_hosts/host_drive.cpp
    world_hosts/host_club.cpp
    world_hosts/host_showroom.cpp
    world_hosts/host_valley.cpp
    world_hosts/host_cliffs.cpp
    world_hosts/host_mines.cpp   # mine-entrance showcase (inspx/mines)
    # W-CUTAWAY — the Level Architect CUTAWAY VIEW: the whole canonical facility
    # in X-ray (--world cutaway / --screenshot-cutaway / --test-cutaway).
    # docs/design/LEVEL_ARCHITECT_CUTAWAY_REF{,2,3}.png is the spec.
    world_hosts/host_cutaway.cpp
    cutaway.cpp
    world_hosts/host_streamed.cpp
    # Phase 2 — space-combat stack folded from feat/cockpit-vattalus
    world_hosts/host_space.cpp
    world_hosts/host_introcockpit.cpp
    world_hosts/host_strata.cpp
    world_hosts/host_elevator.cpp
    # Phase 7 — ESCAPED-branch Act-1 surface-landing start (--world surface)
    world_hosts/host_surface_start.cpp
    world_hosts/host_rifthub.cpp
    world_hosts/host_gallery.cpp
    # W-MENU — the ESC game menu + F4 weather / F5 lighting panels (host_tunnel)
    world_hosts/host_menu.cpp
    world_hosts/host_tunnel.cpp
    tunnel_corridor.cpp
    # W-TREES — tall broadleaf groves along the tunnel road (--world tunnel)
    road_trees.cpp
    # W-NIGHT — roadside campfires at the grove benches (people roast hot dogs)
    campfire.cpp
    # W-TOWN — the sketch's Small Mountain Town on the summit spur
    town.cpp
    # W-FOREST — the sketch's brown regions: area forests, two-tier draw
    forest.cpp
    road_network.cpp    # chained-corridor road routes (the ring tours)
    interchange.cpp     # the diamond interchange — the first structural grade split
    stack.cpp           # THE MEGA STACK — four-level directional interchange (I-17/I-10)
    traffic.cpp         # W-TRAFFIC — AI traffic on the 16-lane freeway
    summit_lot.cpp      # the parking lot at the top of the summit spur
    ridge_road.cpp      # the dirt road along the tops, lot -> the bore's massif
    river_bridge.cpp    # the valley road + Bridge No.1 over the river
    underground_river.cpp # W-UNDERRIVER: the lake-to-canyon river under the mountain
    factory.cpp         # THE GLIMVALE WORKS + the golden-ticket hunt
    river_life.cpp      # W-RIVER — fish + AI speedboats on the bridge reach
    gas_station.cpp     # W-STATIONS — forecourts off the network + the fuel stub
    # The car gauge cluster. Lifted out of host_tunnel's interactive loop so the
    # headless proof set can photograph the dials at all (see gauge_hud.h).
    gauge_hud.cpp
    rifthub.cpp
    rift_depths.cpp
    rift_console.cpp
    # W-MENU — the ONE destination registry + the world/place selection menu
    destinations.cpp
    host_shell_lint.cpp
    world_menu.cpp
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
    space/hud_project.cpp
    space/cockpit_sway.cpp
    # Phase 3 — interactive branching cold-open: the Intro Orchestrator.
    intro_orchestrator.cpp
    scene.cpp
    level.cpp
    level1.cpp
    level_lint.cpp
    qa_propclip.cpp
    level_loader.cpp
    leveldoc_world.cpp
    player.cpp
    door.cpp
    bed_rest.cpp
    keypad.cpp
    physprops.cpp
    ragdoll.cpp
    barrels.cpp
    glass_test.cpp
    holo_panel.cpp
    holo_terminal.cpp
    holo_panel.cpp
    sky_stars.cpp
    space/ship_interior.cpp
    space/ship_windows.cpp
    space/ship_interior_art.cpp
    space/wormhole_vfx.cpp
    space/wormhole_transit.cpp
    space/tractor_beam.cpp
    descent_slide.cpp
    basis_test.cpp
    prim_light_test.cpp
    cluster_light_test.cpp
    world_hosts/host_descentslide.cpp
    world_hosts/host_shipwindows.cpp
    world_hosts/host_bodycontact.cpp
    world_hosts/host_wormhole.cpp
    secret_room.cpp
    ecs_render.cpp
    editor/editor.cpp
    editor/editor_keybinds.cpp
    editor/editor_armory.cpp
    editor/canon_import.cpp
    editor/editor_ai.cpp
    editor/editor_ai_test.cpp
    editor/editor_host.cpp
    elevator.cpp
    elevator_showcase.cpp
    # CLUB LISTEN MODE — WASAPI-loopback live-beat detector that drives the club
    # light show from whatever audio is playing on the PC.
    club_listen.cpp
    strata.cpp
    weapon.cpp
    monster.cpp
    anim.cpp
    character_anim.cpp
    # W-JETPACK: the `fly` command's pack visuals + thrust FX (flight physics
    # lives in player.cpp, the pose in character_anim.cpp).
    jetpack.cpp
    thirdperson.cpp
    melee.cpp
    objective.cpp
    trigger.cpp
    rescue.cpp
    sarah.cpp
    npc_dialog.cpp
    story_ops.cpp
    chat_tree.cpp
    vigil_barks.cpp
    mission.cpp
    level1_game.cpp
    canon_play.cpp
    interactables.cpp
    desc_mechanics.cpp
    canon_45.cpp
    stairwell.cpp
    cell_dressing.cpp
    room_dressing.cpp
    wing_dressing.cpp
    # SEAM 2 (world merge): the glass facility exterior, factored from
    # host_surface_start and wrapped around the REAL canon tower.
    facility_exterior.cpp
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
    map_poi.cpp         # W-MAP: shared MapPoi contract for lanes 4-6 (town/stations/factory)
    city.cpp
    ocean_base.cpp
    save.cpp
    dialog.cpp
    env_art.cpp
    club1127.cpp
    lns_shop.cpp        # the LNS shop material kit (CMU/checker/neon) — shared by club1127 + tunnel garage
    club_bedrock.cpp
    cave_atmosphere.cpp
    descent_fall.cpp
    gamma_probe.cpp
    survival_complex.cpp
    world_hosts/host_complex.cpp
    jukebox.cpp
    cues.cpp
    valley.cpp
    cliffs.cpp
    terrain.cpp
    vehicle.cpp
    engine_note.cpp     # multi-RPM engine-note bank player (4-voice bracket crossfade)
    world_cars.cpp
    street_lights.cpp
    wetness.cpp         # rain soak/dry + ice + tire grip (wetness lane)
    storm.cpp           # lightning flash + delayed thunder (weather lane)
    precip_fx.cpp       # falling snow + rain, camera-local volume (weather lane)
    tunnel_fitout.cpp   # bore interior program: lay-bys, lamps, signs, doors
    tunnel_rooms.cpp    # what is BEHIND those doors: halls, rooms, stairs, the complex landing
    mine_fx.cpp        # the mine-entrance family (Armory rock + glowing mouth)
    vehparts.cpp
    csm_test.cpp
    mesh_lod.cpp        # Lane 5: screen-space-error discrete LOD selection
    mesh_decimate.cpp   # Lane 5: QEM edge-collapse decimation (builds LOD chains)
    lod_chain.cpp       # Lane 5: build + own a device-side LOD chain
    geolod_test.cpp     # Lane 5: --test-geolod
    geolod_shot.cpp     # Lane 5: --screenshot-geolod (A/B capture + perf)
    glb_cpu_read.cpp    # Lane 5: CPU-side GLB re-read so real art can be decimated
    refl_denoise_test.cpp   # refl-denoise lane: --test-refldenoise
    perfshop.cpp
    ecology.cpp
    fish.cpp
    waterzap.cpp
    sealife.cpp
    god_rays.cpp
    crowd.cpp
    crowd_skin.cpp
    crowd_chatter.cpp
    npc_life.cpp
    npc_skin.cpp
    world_hosts/echo_regions.cpp
    world_hosts/echo_region_builders.cpp
    world_hosts/echo_woodlands.cpp
    world_hosts/echo_roads.cpp
    world_hosts/echo_interiors.cpp
    world_hosts/echo_water.cpp
    echo_sea_test.cpp
    hackables.cpp
    alert.cpp
    fx.cpp
    hud.cpp
    hud_panel.cpp
    loading_screen.cpp
    canon_aliens.cpp
    pack_spiders.cpp
    clone_boss.cpp
    item_db.cpp
    inventory.cpp
    progression.cpp
    skilltree.cpp
    rpg_ui.cpp
    ui.cpp
    ui_widgets.cpp
    weapon_tuning_menu.cpp)

# PRIVATE, not PUBLIC: the thin launchers only call x3AppMain() and must not
# inherit the engine static lib. PUBLIC propagated x3core to both exes — harmless
# today (each exe references nothing from it, so both stay ~13 KB) but a foot-gun
# the moment a launcher touches an engine symbol and gets its OWN copy of the
# engine's statics alongside the DLL's.
target_link_libraries(x3app PRIVATE x3core glfw)

# Dear ImGui (docking) — EDITOR-ONLY. The Level Architect host (app/editor/
# editor_host.cpp) submits its panels with the CORE ImGui widget API directly; the
# GLFW/Vulkan backends still live (PRIVATE) in the engine device. Linking the imgui
# core lib here is additive — the game path never calls these unless --editor is set.
find_package(imgui CONFIG REQUIRED)
target_link_libraries(x3app PRIVATE imgui::imgui)

# holo_terminal.cpp rasterizes its on-glass readout text into the hologram
# texture with stb_truetype (the same single-header lib the engine font atlas
# uses). The implementation lives in the engine TU, so the app only needs the
# header on its include path (declarations, no STB_TRUETYPE_IMPLEMENTATION here).
find_package(Stb REQUIRED)
target_include_directories(x3app PRIVATE ${Stb_INCLUDE_DIR})

# CLUB LISTEN MODE (club_listen.cpp) includes <miniaudio.h> for DECLARATIONS ONLY
# (no MINIAUDIO_IMPLEMENTATION — the impl + full WASAPI/loopback backend lives in
# engine/audio/MiniaudioSystem.cpp and links in). The vcpkg port installs just the
# header, so locate it and put it on the app's private include path (same pattern
# as Stb above).
find_path(MINIAUDIO_INCLUDE_DIRS "miniaudio.h")
target_include_directories(x3app PRIVATE ${MINIAUDIO_INCLUDE_DIRS})

# ---------------------------------------------------------------------------
# THE TWO FRONT-ENDS.
#
# Both are thin launchers over x3app.dll (app/app_entry.h). X3LevelArchitect
# differs from X3Engine ONLY by injecting "--editor" into argv — it has no host,
# world or editor code of its own, and must never grow any: the editor has to
# boot through the shipping path or the split buys us nothing.
# ---------------------------------------------------------------------------
add_executable(X3Engine         entry_game.cpp)
add_executable(X3LevelArchitect entry_editor.cpp)
target_link_libraries(X3Engine         PRIVATE x3app)
target_link_libraries(X3LevelArchitect PRIVATE x3app)

# MSVC: auto-export every symbol from the host DLL rather than annotating ~200
# TUs with __declspec(dllexport). Templates instantiated across the boundary are
# NOT covered by this and will surface as link errors if any appear.
set_target_properties(x3app PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

# WIN32 subsystem but we want a console for logs during dev:
if(MSVC)
    set_target_properties(X3Engine X3LevelArchitect PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
endif()

# Exes AND the DLLs land in bin/ together — on Windows the loader resolves
# dependent DLLs from the exe's own directory, so co-locating them is what makes
# both front-ends launch without a PATH dance. It is also what makes acceptance
# condition D1 (both exes resolve the SAME asset root) true by construction:
# assetRoot() walks up from the exe, and both exes sit in the same place.
set_target_properties(X3Engine X3LevelArchitect x3app PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# NOTE (fold merge/flight-modes-to-main): feat/flight-modes-neon carried an
# `OUTPUT_NAME "SpaceEngine"` here to keep that branch's exe/window/process
# namespace off Snake's X3Engine.exe dev builds. That rename is intentionally
# NOT promoted to trunk: main's exe is the canonical X3Engine.exe that the
# fleet-wide `Get-Process X3Engine` convention and dozens of docs/scripts
# reference. The SpaceEngine identity stays branch-local.

# ---- Version stamp: regenerate version_gen.h on EVERY build (git hash + time) ----
# so the window title tells you EXACTLY which build you are running.
add_custom_target(x3version ALL
    COMMAND ${CMAKE_COMMAND}
        -DSRC=${CMAKE_SOURCE_DIR}
        -DOUT=${CMAKE_BINARY_DIR}/version_gen.h
        -P ${CMAKE_SOURCE_DIR}/cmake/gen_version.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/version_gen.h
    COMMENT "Stamping build version (git hash + timestamp)")
add_dependencies(x3app x3version)
target_include_directories(x3app PRIVATE ${CMAKE_BINARY_DIR})

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
    ${CMAKE_SOURCE_DIR}/shaders/volumetric.frag
    ${CMAKE_SOURCE_DIR}/shaders/taa_resolve.frag
    ${CMAKE_SOURCE_DIR}/shaders/autoexposure.comp
    ${CMAKE_SOURCE_DIR}/shaders/depth.vert
    ${CMAKE_SOURCE_DIR}/shaders/depth_cutout.vert
    ${CMAKE_SOURCE_DIR}/shaders/depth_cutout.frag
    ${CMAKE_SOURCE_DIR}/shaders/shadow_cutout.vert
    ${CMAKE_SOURCE_DIR}/shaders/ssao.frag
    ${CMAKE_SOURCE_DIR}/shaders/ssao_blur.frag
    ${CMAKE_SOURCE_DIR}/shaders/ssgi_gather.frag
    ${CMAKE_SOURCE_DIR}/shaders/ssgi_temporal.frag
    ${CMAKE_SOURCE_DIR}/shaders/ssgi_blur.frag
    ${CMAKE_SOURCE_DIR}/shaders/ssgi_apply.frag
    # REFLECTION DENOISE (r_refldenoise) — the stage the reflection chain was
    # missing; the GI equivalent of ssgi_blur.frag above. Plain compute, no ray
    # query, so it belongs in the default (non-RT) list.
    # refl_aux.comp rebuilds the normal + view distance the denoiser weighs taps
    # against. Its own shader rather than a few lines in refl.comp, so refl.comp's
    # SPIR-V stays byte-identical and r_refldenoise 0 stays bit-exact.
    ${CMAKE_SOURCE_DIR}/shaders/refl_aux.comp
    ${CMAKE_SOURCE_DIR}/shaders/refl_denoise.comp
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
# ---- Shader INCLUDE path (shaders/inc/*.glsl) --------------------------------
# mesh.frag is a thin ORCHESTRATOR that #includes one file per concern (lighting /
# shadows / reflections / material / ...) so parallel lanes can own a concern
# without ever editing the same file. glslc resolves #include against -I.
#
# BIT-EXACTNESS NOTE: glslc's default (non -g) output embeds NO source filenames,
# so hoisting a block verbatim into an -I include produces BYTE-IDENTICAL SPIR-V.
# That is the gate the modularization was proved against — see docs/design/
# MESH_FRAG_MODULES.md. It only holds while declaration ORDER is preserved, so
# keep each #include exactly where its block used to sit.
set(SHADER_INC ${CMAKE_SOURCE_DIR}/shaders)
# Every include, listed so a plain glob-free build still reconfigures when one is
# added. The per-output DEPFILE below is what makes EDITS to them rebuild.
file(GLOB SHADER_INC_SRCS ${CMAKE_SOURCE_DIR}/shaders/inc/*.glsl)
set(SPV_FILES "")
foreach(s ${SHADER_SRCS})
    get_filename_component(fn ${s} NAME)
    set(out ${SPV_DIR}/${fn}.spv)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} -I${SHADER_INC} -MD -MF ${out}.d ${s} -o ${out}
        DEPENDS ${s}
        DEPFILE ${out}.d
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
            -I${SHADER_INC} -MD -MF ${SPV_DIR}/mesh_rt.frag.spv.d
            -fshader-stage=frag ${CMAKE_SOURCE_DIR}/shaders/mesh.frag -o ${SPV_DIR}/mesh_rt.frag.spv
    DEPENDS ${CMAKE_SOURCE_DIR}/shaders/mesh.frag
    DEPFILE ${SPV_DIR}/mesh_rt.frag.spv.d
    COMMENT "glslc mesh.frag (ray-query soft shadows) -> mesh_rt.frag.spv"
    VERBATIM)
list(APPEND SPV_FILES ${SPV_DIR}/mesh_rt.frag.spv)
# Copy compiled .spv next to the exe so runtime loads <exedir>/shaders/*.spv.
#
# BUILD BUG THIS FIXES (Lane 3 / csm; the refl-denoise lane independently
# replicated the same fix, and this is the merged single copy of it): the copy
# used to be a POST_BUILD on X3Engine.
# POST_BUILD only runs when the TARGET is relinked, so editing ONLY a shader
# recompiled the .spv into ${SPV_DIR} and then never copied it — the exe kept
# loading the previous SPIR-V. Shader-only iterations silently did nothing, and
# the trap is invisible because a build that also touches any .cpp relinks and
# copies, so it works "most of the time". Hanging the copy off x3shaders (which
# X3Engine already depends on) makes it run whenever a .spv is regenerated.
add_custom_target(x3shaders
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bin/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${SPV_DIR} ${CMAKE_BINARY_DIR}/bin/shaders
    DEPENDS ${SPV_FILES}
    COMMENT "copy compiled SPIR-V -> bin/shaders")
add_dependencies(x3app x3shaders)

# Belt and braces for multi-config generators, where the exe may not live in
# ${CMAKE_BINARY_DIR}/bin: relink-time copy as well (cheap, idempotent).
add_custom_command(TARGET x3app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:x3app>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${SPV_DIR} $<TARGET_FILE_DIR:x3app>/shaders)
