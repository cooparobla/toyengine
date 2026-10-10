cmake_minimum_required(VERSION 3.21)  # PROJECT_IS_TOP_LEVEL
project(toyengine)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# This file builds toyengine standalone (the top-level project: engine, editor, tests, test
# scenes in assets/) AND is add_subdirectory()'d by a game project from <project>/.libs/toyengine
# (see templates/project/ and tools/toyhub). Every path that means "the engine checkout" is
# therefore TOYENGINE_DIR, never CMAKE_SOURCE_DIR -- which, in a project build, is the project.
set(TOYENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# --- Vendored coopa libraries (libs/, git submodules) ---
# ROOT_DIR_PARENT is "the directory the coopa repos live in" -- the pinned libs/
# submodule tree, so the build never depends on what happens to be checked out
# beside toyengine. configuration/root_directory.h.in exports it as PROJ_DIR,
# which toyengine/core/runtime_paths.h uses to locate gfxcoopa's and uicoopa's shaders.
set(ROOT_DIR_PARENT "${TOYENGINE_DIR}/libs")

set(LIBCOOPA_DIR   "${ROOT_DIR_PARENT}/libcoopa")
set(CAML_DIR       "${ROOT_DIR_PARENT}/caml")
set(GFXCOOPA_DIR   "${ROOT_DIR_PARENT}/gfxcoopa")
set(PHYSXCOOPA_DIR "${ROOT_DIR_PARENT}/physxcoopa")
set(SFXCOOPA_DIR   "${ROOT_DIR_PARENT}/sfxcoopa")
set(UICOOPA_DIR    "${ROOT_DIR_PARENT}/uicoopa")
set(MAPCOOPA_DIR   "${ROOT_DIR_PARENT}/mapcoopa")

# --- Compiled shaders go in the BUILD tree, never the checkout ---
# gfx_add_shader_target() (gfxcoopa's cmake/GfxShaders.cmake) writes each target's .spv to
# <GFX_SHADER_OUTPUT_ROOT>/<target>/ when this is set -- here before any library is added, so
# gfxcoopa's and uicoopa's shader targets follow it too. A game project linked to this
# checkout (.libs/toyengine -> here) therefore compiles every shader, the engine's included,
# into its OWN build/shaders/, and neither it nor this checkout's build/ ever writes into the
# other's tree. The runtime finds them through TOY_SHADER_BUILD_DIR (toyengine::engine below,
# RuntimeLayout::shader_roots()). CACHE INTERNAL so toyengine_add_project(), called from a
# project's own scope, sees it for the project's shaders.
set(TOY_SHADER_BUILD_DIR "${CMAKE_BINARY_DIR}/shaders" CACHE INTERNAL "Where every compiled .spv goes")
set(GFX_SHADER_OUTPUT_ROOT "${TOY_SHADER_BUILD_DIR}")

if(NOT EXISTS "${LIBCOOPA_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "libs/ is empty -- the coopa libraries are git submodules. "
        "Run: git submodule update --init --recursive")
endif()

# !! ORDER IS LOAD-BEARING -- DO NOT REORDER !!
# The library repos are deliberately left unpatched, so each still computes its
# own libcoopa path as "parent of CMAKE_SOURCE_DIR" -- which, with toyengine (or a
# game project) as the top-level project, resolves OUTSIDE this checkout. Each only dereferences
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

# --- SMAA reference textures (SearchTex.h / AreaTex.h) ---
# gfxcoopa's SMAA_TEXTURES_DIR cache default points at a fixed local checkout
# (/home/coopa/third-party/smaa/Textures). Where that checkout exists -- or an
# explicit -DSMAA_TEXTURES_DIR=... is given -- nothing here runs. Otherwise
# (e.g. a macOS machine), fetch the reference implementation at a pinned commit
# so render/toy_render_pipeline.h's unconditional SmaaPass include compiles.
set(_toy_smaa_default "/home/coopa/third-party/smaa/Textures")
if(DEFINED SMAA_TEXTURES_DIR)
    set(_toy_smaa_dir "${SMAA_TEXTURES_DIR}")
else()
    set(_toy_smaa_dir "${_toy_smaa_default}")
endif()
if(NOT EXISTS "${_toy_smaa_dir}/AreaTex.h")
    include(FetchContent)
    FetchContent_Declare(smaa
        GIT_REPOSITORY https://github.com/iryoku/smaa.git
        GIT_TAG        71c806a838bdd7d517df19192a20f0c61b3ca29d)
    FetchContent_Populate(smaa)
    set(SMAA_TEXTURES_DIR "${smaa_SOURCE_DIR}/Textures" CACHE PATH
        "Directory containing the SMAA reference implementation's SearchTex.h/AreaTex.h" FORCE)
    message(STATUS "toyengine: SMAA textures not found at ${_toy_smaa_dir}; fetched to ${SMAA_TEXTURES_DIR}")
endif()

add_subdirectory(${GFXCOOPA_DIR}   ${CMAKE_CURRENT_BINARY_DIR}/gfxcoopa-build)
add_subdirectory(${PHYSXCOOPA_DIR} ${CMAKE_CURRENT_BINARY_DIR}/physxcoopa-build)
add_subdirectory(${SFXCOOPA_DIR}   ${CMAKE_CURRENT_BINARY_DIR}/sfxcoopa-build)
add_subdirectory(${UICOOPA_DIR}    ${CMAKE_CURRENT_BINARY_DIR}/uicoopa-build)
# mapcoopa is header-only and pure data (a MapGraph; no Vulkan, no GLFW), consumed by
# toyengine/world/ to build terrain from a generated world. Its own executables, tests and
# ctest entries sit behind a `CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR` guard, so
# add_subdirectory() here contributes exactly one INTERFACE target, coopa::maps.
add_subdirectory(${MAPCOOPA_DIR}   ${CMAKE_CURRENT_BINARY_DIR}/mapcoopa-build)

# Backstop for the ordering dependency above: if a reorder ever lets a library
# resolve libcoopa through its own stale path, we end up building an unpinned
# checkout from outside this repo. Fail at configure time instead of silently.
get_target_property(_libcoopa_inc libcoopa_lib INTERFACE_INCLUDE_DIRECTORIES)
if(NOT _libcoopa_inc MATCHES "${TOYENGINE_DIR}/libs/libcoopa")
    message(FATAL_ERROR
        "coopa::lib resolved to '${_libcoopa_inc}', outside libs/. "
        "Check add_subdirectory() order above -- libs/libcoopa must come first.")
endif()

# caml ships no consumable target (just a `caml` test executable), and like the other
# libs/ repos it stays unpatched -- so coopa::caml is defined here. add_subdirectory()
# still runs first: when pkg-config can't find libzstd it FetchContent's zstd and
# creates the global libzstd_static target this links. Its OpenSSL/zstd variables are
# directory-scoped, so discovery is repeated below.
#
# Only toyengine/core/caml_codec.h includes caml.h; everything else reads YAML through
# libcoopa's coopa/yaml/document.h, which decodes .caml via the codec it registers.
add_subdirectory(${CAML_DIR} ${CMAKE_CURRENT_BINARY_DIR}/caml-build)
find_package(OpenSSL REQUIRED)
add_library(toy_caml INTERFACE)
add_library(coopa::caml ALIAS toy_caml)
target_include_directories(toy_caml INTERFACE ${CAML_DIR}/includes)
target_link_libraries(toy_caml INTERFACE OpenSSL::Crypto)
if(TARGET libzstd_static)
    target_link_libraries(toy_caml INTERFACE libzstd_static)
    target_include_directories(toy_caml INTERFACE ${zstd_SOURCE_DIR}/lib)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TOY_ZSTD REQUIRED IMPORTED_TARGET libzstd)
    target_link_libraries(toy_caml INTERFACE PkgConfig::TOY_ZSTD)
endif()

# uicoopa's `uicoopa_shaders` target is deliberately NOT behind the
# `CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR` guard its demos and ctest
# registration sit behind: UiPass and UiWorldPass are constructed from .spv paths,
# so this build needs them compiled too -- hence the add_dependencies() below, and
# the uicoopa search root in RuntimeLayout::shader_roots() (toyengine/core/runtime_paths.h).

configure_file(configuration/root_directory.h.in configuration/root_directory.h)

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
    SHADER_DIR   "${TOYENGINE_DIR}/assets/shaders"
)

# --- toyengine::engine: everything a game / editor / test executable links ---
# The engine is a static library: each toyengine/**/*.cpp compiles once, so a game, editor or
# test TU only compiles its own code against the engine's declarations. Its PUBLIC include roots
# are the checkout (for <toyengine/...> and <editor/...>) and the configured root_directory.h. A
# target-level include path rather than include_directories(), which is directory-scoped and
# would not reach a game project's targets. The coopa libraries stay header-only (INTERFACE).
#
# coopa::ui LAST on purpose: uicoopa's includes/ also carries stb_image.h /
# stb_image_write.h, whose implementations live in gfxcoopa_impl. Linking coopa::gfx
# first keeps gfxcoopa's copies of those two headers ahead in the include search, so a
# TU can never compile against uicoopa's copy while linking gfxcoopa's body.
#
# coopa::maps sits between them for exactly the same reason -- mapcoopa's includes/ carries
# a third copy of stb_image_write.h. Nothing in toyengine may include mapcoopa's
# image_writer.h / map_export.h, which compile stb's *implementation*; toyengine/world/ uses
# only the data headers (map_config.h, map_data.h, map_generator.h, noise.h, biome.h).
#
# coopa::caml after coopa::ui: its includes/ carries a second fkYAML copy, which must
# never shadow libcoopa's.
file(GLOB_RECURSE _toyengine_sources CONFIGURE_DEPENDS ${TOYENGINE_DIR}/toyengine/*.cpp)
add_library(toyengine_engine STATIC ${_toyengine_sources})
add_library(toyengine::engine ALIAS toyengine_engine)
target_include_directories(toyengine_engine PUBLIC
    ${TOYENGINE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/configuration)
target_link_libraries(toyengine_engine PUBLIC coopa::gfx coopa::physx coopa::sfx coopa::maps coopa::ui)
target_link_libraries(toyengine_engine PUBLIC coopa::caml)
# The compiled shaders' root (see TOY_SHADER_BUILD_DIR above): <dir>/toyengine_shaders,
# /shaders (gfxcoopa's), /uicoopa_shaders and /project_shaders.
target_compile_definitions(toyengine_engine PUBLIC TOY_SHADER_BUILD_DIR="${TOY_SHADER_BUILD_DIR}")

# --- toyengine::editor: the editor (editor/), compiled once ---
# Every project's editor executable links it, as do the editor tests and the docs tool; only
# editor/editor.cpp (main) compiles into the executable itself. editor/editor_test.cpp is the
# retired monolithic test file and is not built.
file(GLOB_RECURSE _toyengine_editor_sources CONFIGURE_DEPENDS ${TOYENGINE_DIR}/editor/*.cpp)
list(FILTER _toyengine_editor_sources EXCLUDE REGEX "/editor/(editor|editor_test)\\.cpp$")
add_library(toyengine_editor_lib STATIC ${_toyengine_editor_sources})
add_library(toyengine::editor ALIAS toyengine_editor_lib)
target_link_libraries(toyengine_editor_lib PUBLIC toyengine::engine)

# toyengine_add_project(): the game + editor executables for a project directory. This repo is
# itself the project "toyengine" (its assets/ holds the test scenes), which is what keeps the
# standalone build's toyengine / toyengine_editor targets exactly as they were.
include(${TOYENGINE_DIR}/cmake/ToyProject.cmake)

# The build flavour (ToyProject.cmake's options) is a property of the whole build tree, set on
# the engine library PUBLIC so the library and every executable linking it agree: a Shipping
# build is its own tree (editor/build/build_pipeline.h configures <project>/build-ship).
if(TOY_SHIPPING)
    target_compile_definitions(toyengine_engine PUBLIC TOY_SHIPPING=1)
    if(TOY_SHIPPING_DEBUG_OVERLAY)
        target_compile_definitions(toyengine_engine PUBLIC TOY_DEBUG_OVERLAY=1)
    endif()
endif()
if(TOY_CAML_KEY_BAKED)
    target_compile_definitions(toyengine_engine PRIVATE TOY_CAML_KEY_BAKED="${TOY_CAML_KEY_BAKED}")
endif()

# Everything below is engine development only (the engine's own game/editor over this repo's
# assets/, tests, the hub, the icon exporter, the leak gate): a game project that
# add_subdirectory()s this checkout gets just the engine, its shaders and toyengine_add_project().
if(NOT PROJECT_IS_TOP_LEVEL)
    return()
endif()

toyengine_add_project(NAME toyengine DIR "${TOYENGINE_DIR}" EDITOR_NAME toyengine_editor)

enable_testing()
# Tests: tests/CMakeLists.txt (one suite per file, tiered unit / gpu / extended).
add_subdirectory(tests)

# --- Editor ---
# toyengine_editor (defined by toyengine_add_project() above) embeds the same Engine, so its
# "Full Render" viewport IS the game's renderer, and builds its UI with uicoopa's
# immediate-mode layer.

# toyengine_hub: the project launcher (hub/) -- a GUI over tools/toyhub, drawn with the editor's
# UI and themes. Engine-level, so it is not part of a game project's build.
add_executable(toyengine_hub hub/hub.cpp)
add_dependencies(toyengine_hub toyengine_shaders uicoopa_shaders)
target_link_libraries(toyengine_hub PRIVATE toyengine::editor)

# toyengine_icon: exports the logo (toyengine/core/branding.h) as PNGs + a macOS .icns into
# editor/branding/ for packaging (not assets/: that ships with games). Not needed at runtime -- Engine rasterizes the same shapes.
add_executable(toyengine_icon tools/icon/export_icon.cpp)
target_include_directories(toyengine_icon PRIVATE ${TOYENGINE_DIR})
target_link_libraries(toyengine_icon PRIVATE coopa::gfx)

# --- Vulkan/GLFW leak gate ---
# OFF by default here, unlike uicoopa/pixengine which are clean:
# render/toy_render_pipeline.h still names raw Vulkan symbols, forced by the
# gfxcoopa internals that have no sealed equivalent yet -- GBufferPipeline's
# MRT render-pass construction, Renderer::begin_frame()'s
# raw VkClearColorValue, and SsaoPass's raw view accessors. Run
# `${GFXCOOPA_DIR}/tools/check_no_vulkan.sh ${TOYENGINE_DIR}` for the current
# count; flip ON once those gain wrappers.
option(GFX_LEAK_CHECK "Fail the build if consumer source names raw Vulkan/GLFW symbols" OFF)
if(GFX_LEAK_CHECK)
    add_custom_target(toyengine_leak_check ALL
        COMMAND ${GFXCOOPA_DIR}/tools/check_no_vulkan.sh ${TOYENGINE_DIR})
    add_dependencies(toyengine toyengine_leak_check)
    if(TARGET toyengine_tests)
        add_dependencies(toyengine_tests toyengine_leak_check)
    endif()
    add_test(NAME toyengine_no_vulkan COMMAND ${GFXCOOPA_DIR}/tools/check_no_vulkan.sh ${TOYENGINE_DIR})
endif()
