cmake_minimum_required(VERSION 3.20)
project(toyengine)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Vendored coopa libraries (libs/, git submodules) ---
# ROOT_DIR_PARENT is "the directory the coopa repos live in" -- the pinned libs/
# submodule tree, so the build never depends on what happens to be checked out
# beside toyengine. configuration/root_directory.h.in exports it as PROJ_DIR,
# which toyengine/core/engine.h uses to locate gfxcoopa's shared shader library.
set(ROOT_DIR_PARENT "${CMAKE_SOURCE_DIR}/libs")

set(LIBCOOPA_DIR   "${ROOT_DIR_PARENT}/libcoopa")
set(CAML_DIR       "${ROOT_DIR_PARENT}/caml")
set(GFXCOOPA_DIR   "${ROOT_DIR_PARENT}/gfxcoopa")
set(PHYSXCOOPA_DIR "${ROOT_DIR_PARENT}/physxcoopa")
set(SFXCOOPA_DIR   "${ROOT_DIR_PARENT}/sfxcoopa")
set(UICOOPA_DIR    "${ROOT_DIR_PARENT}/uicoopa")

if(NOT EXISTS "${LIBCOOPA_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "libs/ is empty -- the coopa libraries are git submodules now. "
        "Run: git submodule update --init --recursive")
endif()

# !! ORDER IS LOAD-BEARING -- DO NOT REORDER !!
# The library repos are deliberately left unpatched, so each still computes its
# own libcoopa path as "parent of CMAKE_SOURCE_DIR" -- which, with toyengine as
# the top-level project, resolves OUTSIDE this checkout. Each only dereferences
# that path inside an `if(NOT TARGET coopa::lib)` guard, so adding libs/libcoopa
# FIRST defines coopa::lib, every guard goes false, and the outside path is
# never used. Move this line down and the build silently compiles an unpinned
# checkout from outside the repo -- the assertion after this block exists to
# catch exactly that.
#
# Leaving the libraries unpatched preserves their standalone contract: each
# builds on its own (cbuild/cplay) as long as the repos it needs sit beside it
# under a shared parent, which libs/ satisfies -- so `cd libs/gfxcoopa && cbuild`
# works in place.
add_subdirectory(${LIBCOOPA_DIR}   ${CMAKE_CURRENT_BINARY_DIR}/libcoopa-build)
add_subdirectory(${GFXCOOPA_DIR}   ${CMAKE_CURRENT_BINARY_DIR}/gfxcoopa-build)
add_subdirectory(${PHYSXCOOPA_DIR} ${CMAKE_CURRENT_BINARY_DIR}/physxcoopa-build)
add_subdirectory(${SFXCOOPA_DIR}   ${CMAKE_CURRENT_BINARY_DIR}/sfxcoopa-build)
add_subdirectory(${UICOOPA_DIR}    ${CMAKE_CURRENT_BINARY_DIR}/uicoopa-build)

# Backstop for the ordering dependency above: if a reorder ever lets a library
# resolve libcoopa through its own stale path, we end up building an unpinned
# checkout from outside this repo. Fail at configure time instead of silently.
get_target_property(_libcoopa_inc libcoopa_lib INTERFACE_INCLUDE_DIRECTORIES)
if(NOT _libcoopa_inc MATCHES "${CMAKE_SOURCE_DIR}/libs/libcoopa")
    message(FATAL_ERROR
        "coopa::lib resolved to '${_libcoopa_inc}', outside libs/. "
        "Check add_subdirectory() order above -- libs/libcoopa must come first.")
endif()

# caml has no consumable target (no INTERFACE library, no coopa:: alias) -- it
# is consumed by pointing -I at its source tree, the same way uicoopa does it
# (uicoopa/CMakeLists.txt:41-42). add_subdirectory() is still needed for its
# OpenSSL/zstd discovery; it also contributes a `caml` executable target.
add_subdirectory(${CAML_DIR} ${CMAKE_CURRENT_BINARY_DIR}/caml-build)

# uicoopa's `uicoopa_shaders` target is deliberately NOT behind the
# `CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR` guard its demos and ctest
# registration sit behind: UiPass and UiWorldPass are constructed from .spv paths,
# so this build needs them compiled too -- hence the add_dependencies() below, and
# the uicoopa search root in toyengine/core/engine.h's make_render_config_().

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
# gfxcoopa's own assets/shaders/ is always searched by gfx_add_shader_target(),
# so INCLUDE_DIR needs no explicit value here.
#
# The target is toyengine_shaders, not a bare "shaders": gfxcoopa is
# add_subdirectory()'d into this same build and brings its own "shaders" target,
# and CMake target names must be unique across the whole build.
include(${GFXCOOPA_DIR}/cmake/GfxShaders.cmake)
gfx_add_shader_target(toyengine_shaders
    SHADER_DIR   "${CMAKE_SOURCE_DIR}/assets/shaders"
)

add_executable(toyengine main.cpp)
add_dependencies(toyengine toyengine_shaders uicoopa_shaders)
# coopa::ui LAST on purpose: uicoopa's includes/ also carries stb_image.h /
# stb_image_write.h, whose implementations live in gfxcoopa_impl. Linking coopa::gfx
# first keeps gfxcoopa's copies of those two headers ahead in the include search, so a
# TU can never compile against uicoopa's copy while linking gfxcoopa's body.
target_link_libraries(toyengine PRIVATE coopa::gfx coopa::physx coopa::sfx coopa::ui)
target_include_directories(toyengine PRIVATE ${CAML_DIR} ${CAML_DIR}/includes)

enable_testing()
add_executable(toyengine_tests test.cpp)
add_dependencies(toyengine_tests toyengine_shaders uicoopa_shaders)
target_link_libraries(toyengine_tests PRIVATE coopa::gfx coopa::physx coopa::sfx coopa::ui)
target_include_directories(toyengine_tests PRIVATE ${CAML_DIR} ${CAML_DIR}/includes)
# One ctest entry per test GROUP, not one for the whole binary: each render_* group
# constructs its own Vulkan device, pipelines and scene, and that cost is CPU-bound
# setup rather than GPU work -- so `ctest -j4` overlaps them and the suite finishes in
# roughly the time of its slowest group. It also means `ctest -R toyengine_math` is an
# instant, device-free check a dev can run on every save. See test.cpp's file doc for
# the argument forms the binary accepts.
foreach(_toy_test_group math config scene render_pixel render_ui render_material render_cloth)
    add_test(NAME toyengine_${_toy_test_group}
             COMMAND toyengine_tests --group ${_toy_test_group})
endforeach()

# The device-free groups have no business taking seconds; the render groups get a ceiling
# generous enough for a cold shader cache on a slow GPU but short enough that a hung
# frame fails instead of hanging the run.
set_tests_properties(toyengine_math toyengine_config toyengine_scene PROPERTIES TIMEOUT 60)
set_tests_properties(toyengine_render_pixel toyengine_render_ui toyengine_render_material
                     toyengine_render_cloth PROPERTIES TIMEOUT 300)

# --- Vulkan/GLFW leak gate ---
# OFF by default here, unlike uicoopa/pixengine which are clean:
# render/pixel_render_pipeline.h still names raw Vulkan symbols, forced by the
# gfxcoopa internals that have no sealed equivalent yet -- GBufferPipeline and
# TransparentCapturePass's MRT render-pass construction, Renderer::begin_frame()'s
# raw VkClearColorValue, and SsaoPass's raw view accessors. Run
# `${GFXCOOPA_DIR}/tools/check_no_vulkan.sh ${CMAKE_SOURCE_DIR}` for the current
# count; flip ON once those gain wrappers.
option(GFX_LEAK_CHECK "Fail the build if consumer source names raw Vulkan/GLFW symbols" OFF)
if(GFX_LEAK_CHECK)
    add_custom_target(toyengine_leak_check ALL
        COMMAND ${GFXCOOPA_DIR}/tools/check_no_vulkan.sh ${CMAKE_SOURCE_DIR})
    add_dependencies(toyengine toyengine_leak_check)
    add_dependencies(toyengine_tests toyengine_leak_check)
    add_test(NAME toyengine_no_vulkan COMMAND ${GFXCOOPA_DIR}/tools/check_no_vulkan.sh ${CMAKE_SOURCE_DIR})
endif()
