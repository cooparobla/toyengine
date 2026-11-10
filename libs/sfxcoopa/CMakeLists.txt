cmake_minimum_required(VERSION 3.20)
cmake_policy(VERSION 3.20)

project(sfxcoopa)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(ROOT_DIR_PARENT "${CMAKE_SOURCE_DIR}" DIRECTORY)

# --- Sibling library ---
set(LIBCOOPA_DIR "${ROOT_DIR_PARENT}/libcoopa")
if(NOT TARGET coopa::lib)
    add_subdirectory(${LIBCOOPA_DIR} ${CMAKE_CURRENT_BINARY_DIR}/libcoopa-build)
endif()

# Configure and include root directory header
configure_file(configuration/root_directory.h.in configuration/root_directory.h)

# --- sfxcoopa_impl: the ONE translation unit that compiles miniaudio's
# implementation block for the entire dependency graph. See src/sfx_impl.cpp. ---
add_library(sfxcoopa_impl STATIC src/sfx_impl.cpp)
target_compile_features(sfxcoopa_impl PRIVATE cxx_std_20)
target_include_directories(sfxcoopa_impl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/includes)
target_compile_definitions(sfxcoopa_impl PRIVATE MINIAUDIO_IMPLEMENTATION)
# MA_NO_* must be IDENTICAL in this TU and every consumer TU or miniaudio's
# structs change shape across TUs. PUBLIC here, so the implementation compiles
# with them and every consumer inherits the same set through coopa::sfx.
target_compile_definitions(sfxcoopa_impl PUBLIC
    MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER MA_NO_GENERATION MA_NO_FLAC)
target_link_libraries(sfxcoopa_impl PUBLIC ${CMAKE_DL_LIBS} pthread m)

# --- coopa::sfx: the public, header-only consumable target. ---
# Named sfxcoopa_lib (not `sfxcoopa`) because `cplay` resolves the run
# binary as `./build/<project name>` -- the executable below must keep the
# bare `sfxcoopa` target name, matching project(sfxcoopa) above.
add_library(sfxcoopa_lib INTERFACE)
add_library(coopa::sfx ALIAS sfxcoopa_lib)
target_compile_features(sfxcoopa_lib INTERFACE cxx_std_20)
target_include_directories(sfxcoopa_lib INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/includes)
target_link_libraries(sfxcoopa_lib INTERFACE sfxcoopa_impl coopa::lib)

# --- sfxcoopa's own executables/tests. Guarded so a consumer that pulls this
# repo in via add_subdirectory() (e.g. uicoopa) gets only the coopa::sfx
# target above -- not a second `sfxcoopa`/`sfxcoopa_playback`/`sfxcoopa_bake`
# binary or ctest entry mixed into its own build. Standalone builds
# (CMAKE_SOURCE_DIR == CMAKE_CURRENT_SOURCE_DIR, i.e. cbuild/cplay/ctest run
# from this repo directly) are completely unaffected. ---
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # sfxcoopa's headless test suites: tests/<suite>_test.cpp, one ctest entry per suite
    # (sfxcoopa_<suite>), all linked into one executable that coopa::test_main runs with
    # --suite <suite>. Kept named `sfxcoopa` (matching the project() name above) so `cplay`
    # keeps working unchanged -- run with no arguments it runs every suite.
    file(GLOB SFXCOOPA_TEST_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/*.cpp)
    add_executable(sfxcoopa ${SFXCOOPA_TEST_SOURCES})
    target_include_directories(sfxcoopa PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(sfxcoopa PRIVATE coopa::sfx coopa::test_main)

    # Audible/offline-bounce demo -- not a test, a separate target so ctest never needs a
    # sound card (mirrors uicoopa's headless-test/interactive-demo split).
    add_executable(sfxcoopa_playback demos/demo_playback.cpp)
    target_link_libraries(sfxcoopa_playback PRIVATE coopa::sfx)

    # Offline WAV generator for the sample sound-effect library consumed by
    # uicoopa (assets/sounds/) -- see tools/bake_ui_sfx.cpp. Not a test, just
    # a one-shot content-generation tool; run manually, output is committed.
    add_executable(sfxcoopa_bake tools/bake_ui_sfx.cpp)
    target_link_libraries(sfxcoopa_bake PRIVATE coopa::sfx)

    enable_testing()
    coopa_add_test_suites(sfxcoopa PREFIX sfxcoopa SOURCES ${SFXCOOPA_TEST_SOURCES} LABELS unit)
endif()
