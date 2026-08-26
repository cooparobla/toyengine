cmake_minimum_required(VERSION 3.20)
project(toyengine)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(ROOT_DIR_PARENT "${CMAKE_SOURCE_DIR}" DIRECTORY)

set(GFXCOOPA_DIR "${ROOT_DIR_PARENT}/gfxcoopa")
if(NOT TARGET coopa::gfx)
    add_subdirectory(${GFXCOOPA_DIR} ${CMAKE_CURRENT_BINARY_DIR}/gfxcoopa-build)
endif()

include_directories(${CMAKE_SOURCE_DIR})

configure_file(configuration/root_directory.h.in configuration/root_directory.h)
include_directories(${CMAKE_BINARY_DIR}/configuration)

# --- Shader compilation ---
# cbuild --vulkan (the primary build tool for this workspace) already walks
# assets/shaders and invokes glslc before CMake ever configures, reading the
# same assets/shaders/.glslc_flags this does. This target is the fallback for
# the plain `cmake -B build && cmake --build build` path, and is incremental
# (unlike cbuild) via glslc -MD depfiles, so editing a shared gfx/*.glsl body
# in gfxcoopa correctly retriggers toyengine's dependent shaders.
#
# INCLUDE_DIR no longer needs to be passed explicitly -- gfxcoopa's own
# assets/shaders/ is always searched by gfx_add_shader_target() now (see
# gfxcoopa/cmake/GfxShaders.cmake).
#
# Target name is toyengine_shaders, not the bare "shaders" every repo used
# to use in isolation: now that gfxcoopa is add_subdirectory()'d for real
# (see coopa::gfx above), gfxcoopa's OWN "shaders" target is also present in
# this same build, and CMake target names must be unique across the whole
# build, not just per-directory.
include(${GFXCOOPA_DIR}/cmake/GfxShaders.cmake)
gfx_add_shader_target(toyengine_shaders
    SHADER_DIR   "${CMAKE_SOURCE_DIR}/assets/shaders"
)

add_executable(toyengine main.cpp)
add_dependencies(toyengine toyengine_shaders)
target_link_libraries(toyengine PRIVATE coopa::gfx)

enable_testing()
add_executable(toyengine_tests test.cpp)
add_dependencies(toyengine_tests toyengine_shaders)
target_link_libraries(toyengine_tests PRIVATE coopa::gfx)
add_test(NAME toyengine_render COMMAND toyengine_tests)

# --- Vulkan/GLFW leak gate ---
# OFF by default here (unlike uicoopa/pixengine, which are clean): as of this
# writing, render/pixel_render_pipeline.h still names ~46 raw Vulkan symbols,
# all forced by gfxcoopa's own un-redesigned engine/passes/*/engine/targets/*
# internal APIs (raw VkImageView/VkPipelineLayout/VkDescriptorSetLayout params,
# Mesh::Vertex::binding_description(), etc.) -- eliminating them needs a
# redesign of those ~25 gfxcoopa headers, explicitly out of scope for the
# Vulkan-sealing refactor. Run `${GFXCOOPA_DIR}/tools/check_no_vulkan.sh
# ${CMAKE_SOURCE_DIR}` manually to see the current count. Flip ON once that
# redesign lands.
option(GFX_LEAK_CHECK "Fail the build if consumer source names raw Vulkan/GLFW symbols" OFF)
if(GFX_LEAK_CHECK)
    add_custom_target(toyengine_leak_check ALL
        COMMAND ${GFXCOOPA_DIR}/tools/check_no_vulkan.sh ${CMAKE_SOURCE_DIR})
    add_dependencies(toyengine toyengine_leak_check)
    add_dependencies(toyengine_tests toyengine_leak_check)
    add_test(NAME toyengine_no_vulkan COMMAND ${GFXCOOPA_DIR}/tools/check_no_vulkan.sh ${CMAKE_SOURCE_DIR})
endif()
