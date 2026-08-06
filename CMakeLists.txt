cmake_minimum_required(VERSION 3.20)
project(Vulkan2 CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/..")

find_package(Vulkan REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(fastgltf CONFIG REQUIRED)
find_package(vk-bootstrap CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(imgui REQUIRED)
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
	imgui::imgui
    GPUOpen::VulkanMemoryAllocator
    vk-bootstrap::vk-bootstrap
	fastgltf::fastgltf
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
	src/loader.cpp
    src/application.cpp
)

target_link_libraries(${PROJECT_NAME} PRIVATE vkguide_shared)

if (WIN32)
    target_link_options(${PROJECT_NAME} PRIVATE 
        -static 
        -Wl,--allow-multiple-definition
    )
endif()

#
# SHADER COMPILATION
#

find_program(GLSLC_EXECUTABLE glslc)
if(NOT GLSLC_EXECUTABLE)
    message(FATAL_ERROR "glslc not found, add the vulkan sdk to your path")
endif()

file(GLOB_RECURSE SHADER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.glsl"
)

set(SPIRV_BINARIES "")

foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(FILE_NAME ${SHADER} NAME)
    
    set(SPIRV_OUT "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/bin/${FILE_NAME}.spv")
    
    add_custom_command(
        OUTPUT ${SPIRV_OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/bin"
        COMMAND ${GLSLC_EXECUTABLE} ${SHADER} -o ${SPIRV_OUT}
        DEPENDS ${SHADER}
        COMMENT "compiling ${FILE_NAME} to bin/${FILE_NAME}.spv" # fuck this syntax
    )
    
    list(APPEND SPIRV_BINARIES ${SPIRV_OUT})
endforeach()

add_custom_target(Shaders DEPENDS ${SPIRV_BINARIES})
add_dependencies(${PROJECT_NAME} Shaders)