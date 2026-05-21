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

# ---- Global C++ settings ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---- HYBRID license scaffold flag ----
# ON  = build with the GPL RBDOOM scaffold present (engine/_gpl_rbdoom/)
# OFF = clean build, no GPL code (the commercial-ship target).
# The 13700K clean-room checkout has no GPL dir, so it always builds with OFF.
option(USE_GPL_SCAFFOLD "Include the GPL RBDOOM scaffold (prototype only)" OFF)

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
