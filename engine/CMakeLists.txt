add_library(x3engine STATIC
    core/x3_log.cpp
    core/Console.cpp
    rhi/VulkanRenderDevice.cpp
    asset/PakAssetSource.cpp
)

# Repo root on the include path so consumers use the namespaced prefix,
# e.g. #include "engine/rhi/IRenderDevice.h"
target_include_directories(x3engine PUBLIC ${CMAKE_SOURCE_DIR})

target_link_libraries(x3engine
    PUBLIC
        Vulkan::Vulkan
    PRIVATE
        vk-bootstrap::vk-bootstrap
        GPUOpen::VulkanMemoryAllocator
        glfw
        miniz::miniz
        glm::glm
)

target_compile_features(x3engine PUBLIC cxx_std_20)

if(USE_GPL_SCAFFOLD)
    # Present only on the 14900K prototype checkout; the 13700K clean-room
    # checkout omits engine/_gpl_rbdoom/ via sparse-checkout.
    target_compile_definitions(x3engine PUBLIC X3_USE_GPL_SCAFFOLD=1)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/_gpl_rbdoom/CMakeLists.txt")
        add_subdirectory(_gpl_rbdoom)
        target_link_libraries(x3engine PRIVATE x3_gpl_rbdoom)
    endif()
endif()
