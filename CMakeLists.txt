cmake_minimum_required(VERSION 3.24)
project(relight VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RELIGHT_BUILD_TESTS "Build the GoogleTest suite" ON)

add_library(relight_core STATIC
    core/src/camera.cpp
    core/src/depth.cpp
    core/src/layers.cpp
    core/src/mesh.cpp
    core/src/gif.cpp
    core/src/mat4.cpp
    core/src/shading.cpp
)
target_include_directories(relight_core PUBLIC core/include)
if(MSVC)
    target_compile_options(relight_core PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(relight_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Werror)
endif()

if(RELIGHT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(core/tests)
endif()
