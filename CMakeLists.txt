cmake_minimum_required(VERSION 3.25)

# Toolchain note: configure with the vcpkg toolchain file, e.g.
#   cmake --preset windows-vs2026
# (see CMakePresets.json) OR
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake

project(X3Native
    VERSION 0.0.1
    DESCRIPTION "Custom native C++/Vulkan game engine"
    LANGUAGES CXX
)

# ---- Version (MAJOR.MINOR.BUILD = 0.3.<git-commit-count>) ----
# MAJOR.MINOR is the SINGLE editable place for the pre-release line: bump
# X3_VERSION_MINOR here to move 0.3 -> 0.4. BUILD is the zero-padded git commit
# count, injected at build time. See docs/VERSIONING.md and cmake/GitVersion.cmake.
set(X3_VERSION_MAJOR 0 CACHE STRING "X3Native major version (0 until first release)")
set(X3_VERSION_MINOR 3 CACHE STRING "X3Native minor version (pre-release line, e.g. 0.3)")

set(X3_VERSION_IN  "${CMAKE_SOURCE_DIR}/engine/core/version.h.in")
set(X3_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(X3_VERSION_OUT "${X3_GENERATED_DIR}/engine/core/version.h")

# 1) Generate once at configure time so the header exists for the first build.
set(X3_SRC_DIR "${CMAKE_SOURCE_DIR}")
include(${CMAKE_SOURCE_DIR}/cmake/GitVersion.cmake)

# 2) Regenerate at every build (so hash + commit-count stay fresh when HEAD
#    moves). configure_file only rewrites when contents change, so this does
#    not force a full rebuild each time.
add_custom_target(x3_version ALL
    COMMAND ${CMAKE_COMMAND}
        -D "X3_SRC_DIR=${CMAKE_SOURCE_DIR}"
        -D "X3_VERSION_MAJOR=${X3_VERSION_MAJOR}"
        -D "X3_VERSION_MINOR=${X3_VERSION_MINOR}"
        -D "X3_VERSION_IN=${X3_VERSION_IN}"
        -D "X3_VERSION_OUT=${X3_VERSION_OUT}"
        -P "${CMAKE_SOURCE_DIR}/cmake/GitVersion.cmake"
    BYPRODUCTS "${X3_VERSION_OUT}"
    COMMENT "Stamping X3 version (git commit-count + short hash)"
    VERBATIM)

# ---- Global C++ settings ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---- MSVC niceties ----
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /MP /Zc:preprocessor)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN VK_USE_PLATFORM_WIN32_KHR)
endif()

# ---- Dependencies (Vulkan SDK + vcpkg) ----
find_package(Vulkan REQUIRED)                              # from the LunarG SDK
find_package(vk-bootstrap CONFIG REQUIRED)                # vcpkg
find_package(VulkanMemoryAllocator CONFIG REQUIRED)       # vcpkg (header-only)
find_package(glfw3 CONFIG REQUIRED)                       # vcpkg
find_package(miniz CONFIG REQUIRED)                       # vcpkg
find_package(glm CONFIG REQUIRED)                         # vcpkg
find_package(Jolt CONFIG REQUIRED)                        # vcpkg (joltphysics, MIT)

# ---- Engine + app ----
add_subdirectory(engine)
add_subdirectory(app)
