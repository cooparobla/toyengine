cmake_minimum_required(VERSION 3.20)
project(toyengine)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Vendored coopa libraries (libs/, git submodules) ---
# ROOT_DIR_PARENT means "the directory the coopa repos live in". It used to be
# the parent of this checkout (/home/coopa/git), which made the build depend on
# whatever happened to be checked out beside toyengine; it is now the pinned
# libs/ submodule tree. Still named ROOT_DIR_PARENT because
# configuration/root_directory.h.in exports it as PROJ_DIR, which
# toyengine/core/engine.h uses to locate gfxcoopa's shared shader library --
# redefining it here keeps that resolution correct with no C++ change.
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
# the top-level project, resolves OUTSIDE this checkout to
# /home/coopa/git/libcoopa. Each only dereferences that stale path inside an
# `if(NOT TARGET coopa::lib)` guard (gfxcoopa:23-26, physxcoopa:13-16,
# sfxcoopa:14-17), so adding libs/libcoopa FIRST defines coopa::lib, the guards
# go false, and the stale path is never used. Move this line down and the build
# silently starts compiling an unpinned checkout from outside the repo -- see
# the assertion after this block, which exists to catch exactly that.
#
# Leaving the libraries unpatched also preserves their standalone contract: each
# builds on its own (cbuild/cplay) as long as the repos it needs sit beside it
# under a shared parent. libs/ satisfies that just as /home/coopa/git does, so
# `cd libs/gfxcoopa && cbuild` works in place.
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

# libs/uicoopa IS part of this build now -- see the add_subdirectory() above, which
# sits in the ordered block for the same reason its siblings do. It could not be
# before: it used CMAKE_SOURCE_DIR for its OWN include/shader/config paths rather than
# just for locating siblings, so as a subdirectory of toyengine it aimed its includes
# at toyengine's root, recompiled toyengine's shaders under a duplicate target,
# hard-wired /home/coopa/git/caml, and flipped GFX_LEAK_CHECK on (failing the build).
#
# Fixed in uicoopa itself, backward-compatibly: every path is CMAKE_CURRENT_*_DIR now,
# it exports a real coopa::ui INTERFACE target (plus a uicoopa_impl STATIC TU owning
# stb_truetype's implementation block, which a directory-scoped add_definitions() could
# never hand to a parent project), and its six demos, ctest registration, generated
# root_directory.h and leak gate all sit behind a
# `CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR` guard -- the one
# physxcoopa:36/sfxcoopa:53 already use. `cd libs/uicoopa && cbuild` is unaffected.
#
# uicoopa's `uicoopa_shaders` target is deliberately NOT behind that guard: UiPass and
# UiWorldPass are constructed from .spv paths, so this build needs them compiled too --
# hence the add_dependencies() on it below, and the uicoopa search root in
# toyengine/core/engine.h's make_render_config_() ShaderLibrary.

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
add_test(NAME toyengine_render COMMAND toyengine_tests)

# --- Vulkan/GLFW leak gate ---
# OFF by default here (unlike uicoopa/pixengine, which are clean): as of this
# writing, render/pixel_render_pipeline.h still names 28 raw Vulkan symbols
# (down from 46 after gfxcoopa's mechanical engine/passes/* reseal -- see the
# refactor plan's Phase 4), all forced by the remaining "hard tier" gfxcoopa
# internals still out of scope: GBufferPipeline/TransparentCapturePass's own
# MRT/raw-render-pass construction, Renderer::begin_frame()'s raw
# VkClearColorValue, and SsaoPass's raw view accessors (no Framebuffer
# wrapper / MRT RenderPass / per-subresource layout tracking exist yet -- see
# gfxcoopa/README.md's layering rule and the plan's "explicitly out of
# scope" list). Run `${GFXCOOPA_DIR}/tools/check_no_vulkan.sh
# ${CMAKE_SOURCE_DIR}` manually to see the current count. Flip ON once that
# remaining redesign lands.
option(GFX_LEAK_CHECK "Fail the build if consumer source names raw Vulkan/GLFW symbols" OFF)
if(GFX_LEAK_CHECK)
    add_custom_target(toyengine_leak_check ALL
        COMMAND ${GFXCOOPA_DIR}/tools/check_no_vulkan.sh ${CMAKE_SOURCE_DIR})
    add_dependencies(toyengine toyengine_leak_check)
    add_dependencies(toyengine_tests toyengine_leak_check)
    add_test(NAME toyengine_no_vulkan COMMAND ${GFXCOOPA_DIR}/tools/check_no_vulkan.sh ${CMAKE_SOURCE_DIR})
endif()
