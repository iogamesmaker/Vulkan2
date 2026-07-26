cmake_minimum_required(VERSION 3.20)
project(Vulkan2 CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(vk-bootstrap CONFIG REQUIRED)
find_package(Stb REQUIRED)

add_library(vkguide_shared STATIC # copied from vkguide.dev
    src/base/vk_types.h
    src/base/vk_initializers.cpp
    src/base/vk_initializers.h
    src/base/vk_images.h
    src/base/vk_images.cpp 
    src/base/vk_descriptors.h
    src/base/vk_descriptors.cpp
    src/base/vk_pipelines.h
    src/base/vk_pipelines.cpp
    src/base/camera.cpp
    src/base/camera.h
    src/base/meshes.h
    src/base/meshes.cpp
)

target_compile_definitions(vkguide_shared PUBLIC 
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)
target_include_directories(vkguide_shared PUBLIC 
    "${CMAKE_CURRENT_SOURCE_DIR}/src/base"
	"${Stb_INCLUDE_DIR}"
)

target_link_libraries(vkguide_shared PUBLIC 
    Vulkan::Vulkan 
    SDL3::SDL3
    glm::glm
    fmt::fmt
    GPUOpen::VulkanMemoryAllocator
    vk-bootstrap::vk-bootstrap
)

target_precompile_headers(vkguide_shared PUBLIC 
    <optional> 
    <vector> 
    <memory> 
    <string> 
    <unordered_map> 
    <glm/mat4x4.hpp>  
    <glm/vec4.hpp> 
    <vulkan/vulkan.h>
)

add_executable(${PROJECT_NAME} 
    src/main.cpp
    src/application.cpp
)

target_link_libraries(${PROJECT_NAME} PRIVATE vkguide_shared)

if (WIN32)
    target_link_options(${PROJECT_NAME} PRIVATE 
        -static 
        -Wl,--allow-multiple-definition
    )
endif()