cmake_minimum_required(VERSION 3.20)
project(toyengine)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(ROOT_DIR_PARENT "${CMAKE_SOURCE_DIR}" DIRECTORY)

find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)

set(LIBCOOPA_DIR "${ROOT_DIR_PARENT}/libcoopa")
set(GFXCOOPA_DIR "${ROOT_DIR_PARENT}/gfxcoopa")

include_directories(${CMAKE_SOURCE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/includes/)
include_directories(${LIBCOOPA_DIR}/includes/)
include_directories(${LIBCOOPA_DIR})
include_directories(${GFXCOOPA_DIR}/includes/)
include_directories(${GFXCOOPA_DIR})

configure_file(configuration/root_directory.h.in configuration/root_directory.h)
include_directories(${CMAKE_BINARY_DIR}/configuration)

add_definitions(-DVOLK_IMPLEMENTATION)
add_definitions(-DVMA_IMPLEMENTATION)
add_definitions(-DGLM_FORCE_DEPTH_ZERO_TO_ONE)
# STB_IMAGE_IMPLEMENTATION/STB_IMAGE_WRITE_IMPLEMENTATION are deliberately NOT
# defined globally here: stb single-header libraries guard their declarations
# but not their implementation block against multiple #includes in one
# translation unit (see blendy/test.cpp's comment on the same hazard), and
# more than one header this project pulls in (gfxcoopa's palette_lut.h,
# toyengine's pixel_texture_loader.h, util/screenshot.h) includes
# stb_image.h/stb_image_write.h. Each entry point
# (main.cpp, test.cpp) instead #defines these locally, immediately before
# its own single chosen include site.

# --- Shader compilation ---
# cbuild --vulkan (the primary build tool for this workspace) already walks
# assets/shaders and invokes glslc before CMake ever configures, reading the
# same assets/shaders/.glslc_flags this does. This target is the fallback for
# the plain `cmake -B build && cmake --build build` path, and is incremental
# (unlike cbuild) via glslc -MD depfiles, so editing a shared gfx/*.glsl body
# in gfxcoopa correctly retriggers toyengine's dependent shaders.
include(cmake/GfxShaders.cmake)
gfx_add_shader_target(shaders
    SHADER_DIR   "${CMAKE_SOURCE_DIR}/assets/shaders"
    INCLUDE_DIR  "${GFXCOOPA_DIR}/assets/shaders"
)

add_executable(toyengine main.cpp)
add_dependencies(toyengine shaders)
target_link_libraries(toyengine PRIVATE Vulkan::Vulkan glfw dl)

enable_testing()
add_executable(toyengine_tests test.cpp)
add_dependencies(toyengine_tests shaders)
target_link_libraries(toyengine_tests PRIVATE Vulkan::Vulkan glfw dl)
add_test(NAME toyengine_render COMMAND toyengine_tests)
