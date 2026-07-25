cmake_minimum_required(VERSION 3.20)
project(Vulkan2 CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (WIN32 AND MINGW)
    add_link_options(-static-libstdc++ -static-libgcc -static)
endif()

add_executable(vulkan src/main.cpp)