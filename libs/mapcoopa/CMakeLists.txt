cmake_minimum_required(VERSION 3.20)
cmake_policy(VERSION 3.20)

project(mapcoopa)

# --- Default the build type, for a standalone build only. ---
# Left unset, CMake passes no optimisation flags at all and GCC/Clang default to
# -O0. That is not a small difference here: a default map takes 40.9 s at -O0 and
# 9.5 s at -O3, because this is a software rasteriser and a deflate encoder
# pushing 23 megapixels nine times over, once per layer.
#
# Guarded on being the top-level project so a consumer that pulls mapcoopa in via
# add_subdirectory() keeps its own build type -- overriding a parent project's
# choice of flags is not ours to make. A developer wanting a debugger can still
# pass -DCMAKE_BUILD_TYPE=Debug or RelWithDebInfo explicitly.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    message(STATUS "mapcoopa: CMAKE_BUILD_TYPE was unset, defaulting to Release")
endif()

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Always CMAKE_CURRENT_SOURCE_DIR/CMAKE_CURRENT_BINARY_DIR, never
# CMAKE_SOURCE_DIR/CMAKE_BINARY_DIR: consumers pull this file in via
# add_subdirectory(), where the un-prefixed variables point at the *top-level*
# project's directories rather than mapcoopa's own. Same reasoning as
# libcoopa/CMakeLists.txt:11-18.
get_filename_component(ROOT_DIR_PARENT "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)

# --- Sibling library ---
# coopa/maps reaches into libcoopa for exactly two headers --
# coopa/collections/yaml_map.h (map_yaml.h) and coopa/debug/logger.h (every
# pass) -- and picks up glm and fkYAML from coopa::lib's include path on the
# way, which is why neither is vendored under includes/ here.
set(LIBCOOPA_DIR "${ROOT_DIR_PARENT}/libcoopa")
if(NOT TARGET coopa::lib)
    add_subdirectory(${LIBCOOPA_DIR} ${CMAKE_CURRENT_BINARY_DIR}/libcoopa-build)
endif()

# Configure and include root directory header
configure_file(configuration/root_directory.h.in configuration/root_directory.h)

# --- coopa::maps: the public, header-only consumable target. ---
# mapcoopa is header-only and produces *data* -- a MapGraph, plus optional
# software PNG renders. Nothing here touches Vulkan, GLFW or any sibling repo
# other than libcoopa. Named mapcoopa_lib (not `mapcoopa`) because `cplay`
# resolves the run binary as `./build/<project name>` -- the generator
# executable below must keep the bare `mapcoopa` target name, matching
# project(mapcoopa) above.
add_library(mapcoopa_lib INTERFACE)
add_library(coopa::maps ALIAS mapcoopa_lib)
target_compile_features(mapcoopa_lib INTERFACE cxx_std_20)
target_include_directories(mapcoopa_lib INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}            # coopa/maps/...
    ${CMAKE_CURRENT_SOURCE_DIR}/includes   # delaunator, noise (FastNoiseLite), stb
    ${CMAKE_CURRENT_BINARY_DIR}/configuration)
target_link_libraries(mapcoopa_lib INTERFACE coopa::lib)

# --- Cross-platform determinism (macOS) ---
# A seed must produce the same world everywhere. coopa/maps/portable_random.h and
# portable_sort.h remove the standard-library differences; the one left is the
# compiler's: Apple Clang contracts a*b+c into a fused multiply-add by default,
# which rounds differently from the separate multiply and add that GCC emits in
# its ISO (-std=c++20) mode. Turning contraction off makes a macOS build
# byte-identical to the Linux one (verified on map_out.yaml, --seed=42).
# Apple-only so the Linux build's flags are untouched.
if(APPLE)
    target_compile_options(mapcoopa_lib INTERFACE
        $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang>:-ffp-contract=off>)
endif()

# --- mapcoopa's own executables/tests. Guarded so a consumer that pulls this
# repo in via add_subdirectory() gets only the coopa::maps target above -- not
# a second `mapcoopa`/`mapcoopa_tests` binary or ctest entry mixed into its own
# build. Standalone builds (CMAKE_SOURCE_DIR == CMAKE_CURRENT_SOURCE_DIR, i.e.
# cbuild/cplay/ctest run from this repo directly) are completely unaffected.
# Same guard sfxcoopa/CMakeLists.txt and uicoopa/CMakeLists.txt already use.
#
# NOTE on the coopa::maps claim above ("nothing here touches Vulkan, GLFW or any
# sibling repo other than libcoopa"): that holds for the LIBRARY, which is what a
# consumer gets, but not for this repo built as an application -- mapcoopa_viewer
# below is a uicoopa/gfxcoopa GUI and therefore a Vulkan binary. Everything about
# it lives inside this guard precisely so the distinction holds. ---
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # The map generator owns the bare project name so `cplay` *is* the generate
    # command: `cbuild && cplay --seed=42 --out=world`. cplay forwards its
    # arguments verbatim, so every flag in examples/map_generator.cpp works.
    add_executable(mapcoopa examples/map_generator.cpp)
    target_link_libraries(mapcoopa PRIVATE coopa::maps)

    # The test suite is therefore a separate target, unlike the sibling repos
    # where the bare name is the test runner. One `<suite>_test.cpp` per system
    # under tests/, shared fixtures under tests/support/; coopa::test_main is the
    # runner, and coopa_add_test_suites() registers each file as its own ctest
    # entry (mapcoopa_<suite>) so `ctest -j` runs the suites in parallel.
    file(GLOB MAPCOOPA_TEST_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/*.cpp)
    add_executable(mapcoopa_tests ${MAPCOOPA_TEST_SOURCES})
    target_include_directories(mapcoopa_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(mapcoopa_tests PRIVATE coopa::maps coopa::test_main)

    enable_testing()
    # A suite takes a few seconds on an idle machine; the timeout is generous because
    # ctest -j shares the cores with every other suite (and whatever else is running).
    coopa_add_test_suites(mapcoopa_tests PREFIX mapcoopa
        SOURCES ${MAPCOOPA_TEST_SOURCES}
        LABELS unit
        TIMEOUT 120)

    # --- Optional GUI map viewer (uicoopa) ---
    # Degrades to "not built" rather than a configure failure when ../uicoopa simply
    # isn't checked out, mirroring how uicoopa's own UICOOPA_WITH_AUDIO handles a
    # missing sfxcoopa. Nothing here touches the mapcoopa_lib INTERFACE target.
    set(UICOOPA_DIR "${ROOT_DIR_PARENT}/uicoopa")
    option(MAPCOOPA_WITH_VIEWER "Build the uicoopa-based GUI map viewer" ON)
    if(MAPCOOPA_WITH_VIEWER AND EXISTS "${UICOOPA_DIR}/CMakeLists.txt")
        if(NOT TARGET coopa::ui)
            add_subdirectory(${UICOOPA_DIR} ${CMAKE_CURRENT_BINARY_DIR}/uicoopa-build)
        endif()

        # Two TUs on purpose: map_viewer_export.cpp is the only one that may include
        # mapcoopa's image_writer.h/map_export.h, because those compile stb_image_write's
        # body and gfxcoopa carries its own copy of the same header. See
        # examples/map_viewer_export.h for the full reasoning.
        add_executable(mapcoopa_viewer examples/map_viewer.cpp examples/map_viewer_export.cpp)
        # coopa::ui LAST: uicoopa's includes/ also carries stb_image.h / stb_image_write.h
        # whose implementations live in gfxcoopa_impl, so coopa::gfx must win the include
        # search -- the same ordering note toyengine/CMakeLists.txt carries.
        target_link_libraries(mapcoopa_viewer PRIVATE coopa::maps coopa::gfx coopa::ui)

        # gfx_add_shader_target() compiles .spv NEXT TO the source .vert/.frag rather than
        # into the build tree (see gfxcoopa/cmake/GfxShaders.cmake), so the runtime shader
        # path is uicoopa's own assets dir -- which is also where its fonts, themes and
        # icon sheets live. ROOT_DIR (configuration/root_directory.h) points at mapcoopa,
        # so the viewer needs this second root as well; it has no other way to find them.
        add_dependencies(mapcoopa_viewer uicoopa_shaders)
        target_compile_definitions(mapcoopa_viewer PRIVATE
            UICOOPA_ASSET_DIR="${UICOOPA_DIR}/assets")
    elseif(MAPCOOPA_WITH_VIEWER)
        message(STATUS "mapcoopa: ${UICOOPA_DIR} not found -- skipping mapcoopa_viewer.")
    endif()
endif()
