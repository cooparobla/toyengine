cmake_minimum_required(VERSION 3.20)
cmake_policy(VERSION 3.20)

project(physxcoopa)

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

# --- coopa::physx: the public, header-only consumable target. Pure INTERFACE --
# physxcoopa has no impl translation unit (no vendored C library to compile, unlike
# sfxcoopa's miniaudio), so unlike sfxcoopa there is no *_impl static library here.
add_library(physxcoopa_lib INTERFACE)
add_library(coopa::physx ALIAS physxcoopa_lib)
target_compile_features(physxcoopa_lib INTERFACE cxx_std_20)
target_include_directories(physxcoopa_lib INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/configuration)
target_link_libraries(physxcoopa_lib INTERFACE coopa::lib)

# --- physxcoopa's own test executable. Guarded so a consumer that pulls this
# repo in via add_subdirectory() (e.g. toyengine) gets only the coopa::physx
# target above, not a second `physxcoopa` binary/ctest entry mixed into its
# own build. ---
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # One suite per tests/<suite>_test.cpp (see libcoopa's coopa/testing/test.h); each runs as
    # its own ctest entry `physxcoopa_<suite>`, so `ctest -j` runs them in parallel.
    file(GLOB PHYSXCOOPA_TEST_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/*.cpp)
    add_executable(physxcoopa ${PHYSXCOOPA_TEST_SOURCES})
    target_include_directories(physxcoopa PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(physxcoopa PRIVATE coopa::physx coopa::test_main)

    enable_testing()
    coopa_add_test_suites(physxcoopa PREFIX physxcoopa SOURCES ${PHYSXCOOPA_TEST_SOURCES} LABELS unit)
endif()
