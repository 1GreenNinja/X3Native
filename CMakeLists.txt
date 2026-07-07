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

# ---- MSB3073 guard (W6-1) ----
# vcpkg's applocal POST_BUILD bakes an ABSOLUTE pwsh path at configure time. With
# Store PowerShell that path is versioned (WindowsApps/Microsoft.PowerShell_<ver>_...)
# and MOVES on every auto-update, after which every already-configured build dir fails
# post-build with MSB3073 ("pwsh.exe not found") even though the exe linked fine.
# Pin vcpkg to the per-user app-execution ALIAS instead — a symlink Windows repoints
# on every update — and FORCE it so stale caches self-heal on any reconfigure.
if(WIN32 AND EXISTS "$ENV{LOCALAPPDATA}/Microsoft/WindowsApps/pwsh.exe")
    set(Z_VCPKG_PWSH_PATH "$ENV{LOCALAPPDATA}/Microsoft/WindowsApps/pwsh.exe"
        CACHE FILEPATH "pwsh via the stable app-execution alias (survives Store updates)" FORCE)
    set(Z_VCPKG_POWERSHELL_PATH "$ENV{LOCALAPPDATA}/Microsoft/WindowsApps/pwsh.exe"
        CACHE INTERNAL "The path to the PowerShell implementation to use.")
endif()

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
find_package(imgui CONFIG REQUIRED)                       # vcpkg (Dear ImGui, docking + glfw/vulkan backends) — EDITOR-ONLY

# ---- Engine + app ----
add_subdirectory(engine)
add_subdirectory(app)
