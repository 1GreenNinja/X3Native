add_library(x3engine STATIC
    core/x3_log.cpp
    core/Console.cpp
    core/JobSystem.cpp
    rhi/VulkanRenderDevice.cpp
    rhi/RenderGraph.cpp
    rhi/vma_impl.cpp
    asset/PakAssetSource.cpp
    physics/JoltPhysicsWorld.cpp
    physics/JoltVehicle.cpp
    physics/Destruction.cpp
    physics/StructuralCollapse.cpp
    ai/Navigation.cpp
    asset/ModelLoader.cpp
    audio/MiniaudioSystem.cpp
    net/LoopbackTransport.cpp
    net/Replication.cpp
    net/NetworkSystem.cpp
)

# Header-only model-loader deps (M2): cgltf (glTF/GLB parse) + stb (image decode)
find_path(CGLTF_INCLUDE_DIRS "cgltf.h")
find_package(Stb REQUIRED)

# Header-only audio dep (M9): miniaudio (single header, Unlicense/MIT-0). The
# vcpkg port installs only miniaudio.h (no CMake config), so locate the header.
find_path(MINIAUDIO_INCLUDE_DIRS "miniaudio.h")

# Repo root on the include path so consumers use the namespaced prefix,
# e.g. #include "engine/rhi/IRenderDevice.h"
target_include_directories(x3engine PUBLIC ${CMAKE_SOURCE_DIR})

# cgltf + stb live only inside ModelLoader.cpp (Pimpl) and miniaudio lives only
# inside MiniaudioSystem.cpp — all header-only, so they're PRIVATE include dirs
# and never leak to engine consumers (the audio header exposes no ma_* types).
target_include_directories(x3engine PRIVATE
    ${CGLTF_INCLUDE_DIRS} ${Stb_INCLUDE_DIR} ${MINIAUDIO_INCLUDE_DIRS})

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
)

target_compile_features(x3engine PUBLIC cxx_std_20)
