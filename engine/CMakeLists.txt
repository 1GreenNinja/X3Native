add_library(x3engine STATIC
    core/x3_log.cpp
    core/x3_boot.cpp
    core/Console.cpp
    core/JobSystem.cpp
    ecs/Ecs.cpp
    rhi/VulkanRenderDevice.cpp
    rhi/RenderGraph.cpp
    rhi/GpuCull.cpp
    rhi/vma_impl.cpp
    asset/PakAssetSource.cpp
    physics/JoltPhysicsWorld.cpp
    physics/JoltVehicle.cpp
    physics/Destruction.cpp
    physics/StructuralCollapse.cpp
    physics/Ragdoll.cpp
    ai/Navigation.cpp
    asset/ModelLoader.cpp
    audio/MiniaudioSystem.cpp
    audio/RtAcoustics.cpp
    net/LoopbackTransport.cpp
    net/Replication.cpp
    net/NetworkSystem.cpp
    net/SnapshotInterpolator.cpp
    net/ClientPredictor.cpp
    script/LuaScriptSystem.cpp
    llm/MockLlmSystem.cpp
    llm/LlamaLlmSystem.cpp
    llm/LlmSelfTest.cpp
)

# ---- In-engine LLM backend: llama.cpp, VENDORED via FetchContent pinned to a
# release tag (no vcpkg port exists for it). CPU backend ONLY for v1
# (GGML_VULKAN OFF) so the inference stack can never interact with the engine's
# Vulkan device/validation; the host CPU runs a 3B Q4 at conversational speed.
# Built STATIC and linked PRIVATE below — llama/ggml types are quarantined
# inside LlamaLlmSystem.cpp (engine/llm/ILlmSystem.h stays llama-free, the same
# discipline as IRenderDevice/IConsole).
include(FetchContent)
set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)   # static llama/ggml
set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON   OFF CACHE BOOL "" FORCE)   # core libllama only (ChatML is hand-rolled)
set(LLAMA_CURL           OFF CACHE BOOL "" FORCE)   # no network dep in the engine
set(GGML_VULKAN          OFF CACHE BOOL "" FORCE)   # never touch the engine's Vulkan
# ---- GPU inference backend: CUDA on the RTX 5090/3090 Ti. CUDA is a SEPARATE
# API from the engine's Vulkan (no renderer entanglement — GGML_VULKAN stays
# OFF). It is enabled ONLY when the CUDA toolkit (nvcc) is present at configure
# time; when it is absent the CPU backend builds unchanged and the exact same
# code runs on the GPU the moment the toolkit is installed + CMake is
# re-configured. The runtime cvar `ai_gpu` selects the path; the backend
# auto-falls back to CPU (one log line) if the compiled build has no GPU offload.
# Requires CUDA Toolkit >= 12.8 for Blackwell/sm_120 (RTX 5090); Ampere (3090 Ti,
# sm_86) works on any recent toolkit.
include(CheckLanguage)
check_language(CUDA)
if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    set(GGML_CUDA        ON  CACHE BOOL "" FORCE)   # GPU inference
    message(STATUS "[x3-llm] CUDA toolkit found (${CMAKE_CUDA_COMPILER}) -> GPU inference ENABLED (GGML_CUDA=ON, all layers offloaded via ai_gpu)")
else()
    set(GGML_CUDA        OFF CACHE BOOL "" FORCE)   # CPU-only build (toolkit absent)
    message(STATUS "[x3-llm] CUDA toolkit NOT found -> CPU inference backend only. Install the CUDA Toolkit >= 12.8 (nvcc) and re-run cmake to enable GPU (GGML_CUDA). ai_gpu will auto-fall back to CPU until then.")
endif()
FetchContent_Declare(llama_cpp
    GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
    GIT_TAG        b9590
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(llama_cpp)
# llama.cpp targets C++17; the repo-global CMAKE_CXX_STANDARD 20 leaks into the
# subproject and breaks MSVC (u8"..." literals become char8_t in C++20 —
# llama-chat.cpp C2664/C2088). Pin the vendored target back to its native 17.
set_target_properties(llama PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# Header-only model-loader deps (M2): cgltf (glTF/GLB parse) + stb (image decode)
find_path(CGLTF_INCLUDE_DIRS "cgltf.h")
find_package(Stb REQUIRED)

# Header-only audio dep (M9): miniaudio (single header, Unlicense/MIT-0). The
# vcpkg port installs only miniaudio.h (no CMake config), so locate the header.
find_path(MINIAUDIO_INCLUDE_DIRS "miniaudio.h")

# Lua scripting (D14): Lua 5.4 + sol2 (the "sol3" C++ binding). sol2 stays
# PRIVATE — nothing in engine/script/IScriptSystem.h needs it, same quarantine
# discipline as Vulkan/ImGui. sol2 generates a very large TU on MSVC, so the
# one .cpp that pulls in <sol/sol.hpp> is compiled with /bigobj.
find_package(Lua REQUIRED)
find_package(sol2 CONFIG REQUIRED)
if(MSVC)
    set_source_files_properties(script/LuaScriptSystem.cpp
        PROPERTIES COMPILE_OPTIONS /bigobj)
endif()

# Repo root on the include path so consumers use the namespaced prefix,
# e.g. #include "engine/rhi/IRenderDevice.h"
target_include_directories(x3engine PUBLIC ${CMAKE_SOURCE_DIR})

# cgltf + stb live only inside ModelLoader.cpp (Pimpl) and miniaudio lives only
# inside MiniaudioSystem.cpp — all header-only, so they're PRIVATE include dirs
# and never leak to engine consumers (the audio header exposes no ma_* types).
target_include_directories(x3engine PRIVATE
    ${CGLTF_INCLUDE_DIRS} ${Stb_INCLUDE_DIR} ${MINIAUDIO_INCLUDE_DIRS}
    ${LUA_INCLUDE_DIR})

target_link_libraries(x3engine
    PUBLIC
        Vulkan::Vulkan
    PRIVATE
        vk-bootstrap::vk-bootstrap
        GPUOpen::VulkanMemoryAllocator
        glfw
        miniz::miniz
        glm::glm
        Jolt::Jolt
        # Dear ImGui (docking) — EDITOR-ONLY. PRIVATE so ImGui types never leak into
        # IRenderDevice.h (it stays graphics/ImGui-API-free); the vcpkg port compiles
        # imgui_impl_vulkan.cpp / imgui_impl_glfw.cpp into the package, so the editor-UI
        # methods in VulkanRenderDevice.cpp call the backends with no vendoring.
        imgui::imgui
        # Lua scripting (D14) — PRIVATE so sol/Lua types never leak through
        # engine/script/IScriptSystem.h (it stays scripting-API-free).
        sol2::sol2
        ${LUA_LIBRARIES}
        # llama.cpp core (static, FetchContent-pinned). PRIVATE: llama/ggml types
        # never leak past engine/llm/LlamaLlmSystem.cpp.
        llama
)

target_compile_features(x3engine PUBLIC cxx_std_20)
