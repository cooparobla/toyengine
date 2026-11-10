cmake_minimum_required(VERSION 3.20)
cmake_policy(VERSION 3.20)

project(libcoopa)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Always CMAKE_CURRENT_SOURCE_DIR/CMAKE_CURRENT_BINARY_DIR, never
# CMAKE_SOURCE_DIR/CMAKE_BINARY_DIR: consumers pull this file in via
# add_subdirectory(), where the un-prefixed variables point at the *top-level*
# project's directories rather than libcoopa's own.
get_filename_component(ROOT_DIR_PARENT "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)

# Configure and include root directory header
configure_file(configuration/root_directory.h.in configuration/root_directory.h)

# --- Consumable target ---
# libcoopa is header-only (scene/asset/util layer, no Vulkan or windowing
# content whatsoever). This INTERFACE target lets gfxcoopa and its downstream
# consumers `target_link_libraries(... coopa::lib)` instead of re-deriving
# these include paths and defines by hand.
add_library(libcoopa_lib INTERFACE)
add_library(coopa::lib ALIAS libcoopa_lib)
target_compile_features(libcoopa_lib INTERFACE cxx_std_20)
target_include_directories(libcoopa_lib INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}            # coopa/...
    ${CMAKE_CURRENT_SOURCE_DIR}/includes   # glm, fkYAML, phmap
    ${CMAKE_CURRENT_BINARY_DIR}/configuration)
target_compile_definitions(libcoopa_lib INTERFACE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS)

# The job system uses std::thread/std::mutex/std::condition_variable directly
# (coopa/job/engine.h, thread.h), which on some platforms/toolchains requires
# explicitly linking a threading library (e.g. pthreads on Linux).
# Threads::Threads is a no-op where none is needed (e.g. most Windows configs).
find_package(Threads REQUIRED)
target_link_libraries(libcoopa_lib INTERFACE Threads::Threads)

# --- Test framework ---
# coopa/testing/test.h + this runner are what every coopa repo's tests (and toyengine's) are
# written against: link coopa::test_main and register suites with coopa_add_test_suites().
add_library(coopa_test_main STATIC ${CMAKE_CURRENT_SOURCE_DIR}/coopa/testing/test_main.cpp)
add_library(coopa::test_main ALIAS coopa_test_main)
target_link_libraries(coopa_test_main PUBLIC coopa::lib)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CoopaTesting.cmake)

# Off by default so a consumer that add_subdirectory()s this library (e.g.
# gfxcoopa) pays nothing for either unless it opts in.
option(COOPA_JOB_DIAGNOSTICS "Enable JobEngine per-type job counters and thread/queue-depth snapshots" OFF)
option(COOPA_SCENE_THREAD_CHECKS "Enable Scene's runtime single-owner (no concurrent access) assertions" OFF)
if(COOPA_JOB_DIAGNOSTICS)
    target_compile_definitions(libcoopa_lib INTERFACE COOPA_JOB_DIAGNOSTICS)
endif()
if(COOPA_SCENE_THREAD_CHECKS)
    target_compile_definitions(libcoopa_lib INTERFACE COOPA_SCENE_THREAD_CHECKS)
endif()

# --- Tests (standalone builds only) ---
# One ctest entry per tests/<suite>_test.cpp (see coopa/testing/test.h). Only built when
# libcoopa is the top-level project, so consumers that add_subdirectory() it never pay for it.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    file(GLOB LIBCOOPA_TEST_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/*.cpp)
    add_executable(libcoopa ${LIBCOOPA_TEST_SOURCES})
    target_include_directories(libcoopa PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(libcoopa PRIVATE coopa::lib coopa::test_main)
    # Always exercised in this repo's own test build, regardless of the option
    # above's default, so the test suite actually catches a Scene single-owner
    # contract violation if one is ever introduced.
    target_compile_definitions(libcoopa PRIVATE COOPA_SCENE_THREAD_CHECKS)
    coopa_add_test_suites(libcoopa PREFIX libcoopa SOURCES ${LIBCOOPA_TEST_SOURCES} LABELS unit)
endif()
