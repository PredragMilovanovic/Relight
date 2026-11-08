include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(relight_tests
    test_camera.cpp
    test_depth.cpp
    test_layers.cpp
    test_mesh.cpp
    test_gif.cpp
    test_mat4.cpp
    test_shading.cpp
)
target_link_libraries(relight_tests PRIVATE relight_core GTest::gtest_main)
if(MSVC)
    target_compile_options(relight_tests PRIVATE /W4 /WX /permissive-)
endif()

include(GoogleTest)
gtest_discover_tests(relight_tests)
