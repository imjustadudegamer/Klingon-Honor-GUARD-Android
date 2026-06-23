# [KHG] VulkanDrv — Android Vulkan render device (build-219). Phase 1 = clear-screen bring-up.
# Only the device shell is compiled now; the staged UT99 manager classes (Buffer/Texture/etc.) are
# added in later phases. ZVulkan provides volk/VMA/builders.
project(VulkanDrv C CXX)

if(NOT USE_SDL)
  message(FATAL_ERROR "VulkanDrv requires NSDLDrv to function.")
endif()

set(SRC_FILES
  "VulkanDrv.cpp"
  "UVulkanRenderDevice.cpp"
  # Phase 4: the UT99 manager architecture (bindless scene, OpenGL 1:1 math) adapted to build-219.
  "BufferManager.cpp"
  "CommandBufferManager.cpp"
  "DescriptorSetManager.cpp"
  "FramebufferManager.cpp"
  "RenderPassManager.cpp"
  "SamplerManager.cpp"
  "ShaderManager.cpp"
  "TextureManager.cpp"
  "TextureUploader.cpp"
  "UploadManager.cpp"
  "SceneTextures.cpp"
  "FileResource.cpp"
  "mat.cpp"
  "halffloat.cpp"
)

add_library(${PROJECT_NAME} ${LIB_TYPE} ${SRC_FILES})
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${SDL2_INCLUDE_DIR}
)

target_link_libraries(${PROJECT_NAME} ${SDL2_LIBRARY})
target_link_libraries(${PROJECT_NAME} ZVulkan)
target_link_libraries(${PROJECT_NAME} Render Engine Core)

target_compile_definitions(${PROJECT_NAME} PRIVATE VULKANDRV_EXPORTS UPACKAGE_NAME=${PROJECT_NAME})
