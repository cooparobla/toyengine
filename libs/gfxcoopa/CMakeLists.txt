cmake_minimum_required(VERSION 3.20)
cmake_policy(VERSION 3.20)

project(gfxcoopa)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(ROOT_DIR_PARENT "${CMAKE_SOURCE_DIR}" DIRECTORY)

# Vulkan headers only — volk provides the loader at runtime
find_package(Vulkan REQUIRED)

# Window system (GLFW)
find_package(glfw3 REQUIRED)

# --- Sibling library ---
# engine/components/ includes <coopa/scene/...> and <coopa/asset/...>, so
# gfxcoopa depends on libcoopa's coopa::lib target for those headers plus
# glm/fkYAML/phmap.
set(LIBCOOPA_DIR "${ROOT_DIR_PARENT}/libcoopa")
if(NOT TARGET coopa::lib)
    add_subdirectory(${LIBCOOPA_DIR} ${CMAKE_CURRENT_BINARY_DIR}/libcoopa-build)
endif()

# Configure and include root directory header
configure_file(configuration/root_directory.h.in configuration/root_directory.h)

# --- Shader compilation ---
# assets/shaders/ holds the shaders gfxcoopa's own code loads (GI: brdf_lut,
# env_prefilter, probe_capture; SmaaPass: smaa_*), the test.vert/.frag pair the
# test suites (tests/) draw with, and the shared gfx/ headers (brdf, ibl, sky, spot_light,
# surface2d) that consumers reach through -I. Every other shader lives with the
# app that loads it. INCLUDE_DIR defaults to gfxcoopa's own shaders (see
# cmake/GfxShaders.cmake), so a downstream consumer does not pass it explicitly.
include(cmake/GfxShaders.cmake)
gfx_add_shader_target(shaders
    SHADER_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders"
)

# --- gfxcoopa_impl: the ONE translation unit in the whole dependency graph
# that compiles VOLK_IMPLEMENTATION / VMA_IMPLEMENTATION / the two stb
# implementation blocks. See src/gfx_impl.cpp for why this must be singular.
add_library(gfxcoopa_impl STATIC src/gfx_impl.cpp)
target_compile_features(gfxcoopa_impl PRIVATE cxx_std_20)
target_compile_definitions(gfxcoopa_impl PRIVATE
    VOLK_IMPLEMENTATION
    VMA_IMPLEMENTATION
    STB_IMAGE_IMPLEMENTATION
    STB_IMAGE_WRITE_IMPLEMENTATION)
target_include_directories(gfxcoopa_impl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/includes)
# glfw must be PUBLIC here (not PRIVATE): Window's inline glfw*() calls
# compile into *consumer* TUs via the header-only gfxcoopa target below, so
# glfw symbols must resolve at the consumer's link step too. This leaks the
# glfw *target name* into a consumer's link line but not the glfw *API* --
# consumers still call zero find_package(glfw3) and name zero GLFW symbols.
# Making this PRIVATE would require pimpl'ing Window, at the cost of a .cpp,
# a heap allocation and lost inlining, for no seal benefit beyond what the
# header/type seal and the leak-check script already provide.
target_link_libraries(gfxcoopa_impl PUBLIC glfw Vulkan::Headers)
if(APPLE)
    # Window::set_icon() sets the Dock icon through the Objective-C runtime (AppKit).
    target_link_libraries(gfxcoopa_impl PUBLIC "-framework AppKit" objc)
endif()

# --- coopa::gfx: the public, header-only consumable target. ---
# Named gfxcoopa_lib (not `gfxcoopa`) because `cplay` resolves the run
# binary as `./build/<project name>` -- the executable below must keep the
# bare `gfxcoopa` target name, matching project(gfxcoopa) above.
add_library(gfxcoopa_lib INTERFACE)
add_library(coopa::gfx ALIAS gfxcoopa_lib)
target_compile_features(gfxcoopa_lib INTERFACE cxx_std_20)

# Location of the official Jimenez SMAA reference implementation's Textures/ directory
# (SearchTex.h / AreaTex.h), consumed by gfxcoopa/engine/util/smaa_textures.h. Overridable
# with -DSMAA_TEXTURES_DIR=... on a machine where the SMAA repo lives somewhere else; not
# vendored into gfxcoopa because AreaTex.h alone is a 1.1 MB generated C array.
set(SMAA_TEXTURES_DIR "/home/coopa/third-party/smaa/Textures" CACHE PATH
    "Directory containing the SMAA reference implementation's SearchTex.h/AreaTex.h")

target_include_directories(gfxcoopa_lib INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}                    # gfxcoopa/...
    ${CMAKE_CURRENT_SOURCE_DIR}/includes           # volk/, vma/, stb/
    ${CMAKE_CURRENT_BINARY_DIR}/configuration
    ${SMAA_TEXTURES_DIR})
target_link_libraries(gfxcoopa_lib INTERFACE gfxcoopa_impl coopa::lib ${CMAKE_DL_LIBS})

# meshoptimizer (MIT): vertex welding, vertex-cache ordering and LOD simplification for
# Mesh's load path (gfxcoopa/engine/data/mesh_build.h). A compiled library rather than
# header-only, so it is fetched and built here and linked through the interface target --
# consumers include <meshoptimizer.h> only via mesh_build.h. Pinned to a release tag.
if(NOT TARGET meshoptimizer)
    include(FetchContent)
    FetchContent_Declare(meshoptimizer
        GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
        GIT_TAG        v0.25
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(meshoptimizer)
endif()
target_link_libraries(gfxcoopa_lib INTERFACE meshoptimizer)

# --- coopa::gfx_pure: types/ only, with NO Vulkan/GLFW include path
# available at all. A stray `#include <gfxcoopa/memory/buffer.h>` in a
# target linked against this fails to compile -- this is what makes the
# "headless tests never touch Vulkan" invariant structural rather than
# aspirational (see tools/check_no_vulkan.sh for the complementary grep gate
# over source text). Keyboard/mouse input is Vulkan-free for the same reason
# but lives in coopa/input/ in libcoopa, which coopa::lib below brings in --
# see coopa/input/README.md.
add_library(gfxcoopa_pure INTERFACE)
add_library(coopa::gfx_pure ALIAS gfxcoopa_pure)
target_compile_features(gfxcoopa_pure INTERFACE cxx_std_20)
target_include_directories(gfxcoopa_pure INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gfxcoopa_pure INTERFACE coopa::lib)

# --- gfxcoopa's own test executable and ctest registration. Guarded so a consumer that pulls
# this repo in via add_subdirectory() (toyengine, uicoopa) gets only the library targets above --
# not a `gfxcoopa` binary or GPU ctest entries mixed into its own build. Standalone builds
# (CMAKE_SOURCE_DIR == CMAKE_CURRENT_SOURCE_DIR, i.e. cbuild/cplay/ctest run from this repo
# directly) are unaffected. ---
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # tests/<suite>_test.cpp, one ctest entry per suite (gfxcoopa_<suite>), all linked into one
    # executable that coopa::test_main runs with --suite <suite>. Must be named `gfxcoopa`,
    # matching project() above, because `cplay` runs `./build/<project name>` -- run with no
    # arguments it runs every suite (on a hidden window; see tests/support/gpu_fixture.h).
    file(GLOB GFXCOOPA_UNIT_TEST_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/vk_util_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/camera_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/material_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/surface_shader_registry_test.cpp)
    file(GLOB GFXCOOPA_TEST_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/*.cpp)
    set(GFXCOOPA_GPU_TEST_SOURCES ${GFXCOOPA_TEST_SOURCES})
    list(REMOVE_ITEM GFXCOOPA_GPU_TEST_SOURCES ${GFXCOOPA_UNIT_TEST_SOURCES})

    add_executable(gfxcoopa ${GFXCOOPA_TEST_SOURCES})
    add_dependencies(gfxcoopa shaders)
    if(GFX_SHADER_OUTPUT_ROOT)   # shaders compiled into the build tree (cmake/GfxShaders.cmake)
        target_compile_definitions(gfxcoopa PRIVATE GFX_TEST_SHADER_DIR="${GFX_SHADER_OUTPUT_ROOT}/shaders")
    endif()
    target_include_directories(gfxcoopa PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(gfxcoopa PRIVATE coopa::gfx coopa::test_main)

    enable_testing()
    # WORKING_DIRECTORY: the suites resolve shader paths relative to the repo root (matching
    # how `cplay` invokes the binary, cwd=repo root), not CTest's default of the build dir.
    # Suites that need no device are labelled `unit`; the rest open a (hidden) Vulkan device.
    coopa_add_test_suites(gfxcoopa PREFIX gfxcoopa SOURCES ${GFXCOOPA_UNIT_TEST_SOURCES}
        LABELS unit WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    coopa_add_test_suites(gfxcoopa PREFIX gfxcoopa SOURCES ${GFXCOOPA_GPU_TEST_SOURCES}
        LABELS gpu WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        SLOT gpu:2)   # machine-wide bound on concurrent device-holding test processes
endif()
