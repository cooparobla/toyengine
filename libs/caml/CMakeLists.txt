cmake_minimum_required(VERSION 3.16)

project(caml)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/includes)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Find OpenSSL
find_package(OpenSSL REQUIRED)

# Find or Fetch Zstd
find_package(PkgConfig QUIET)
if (PKG_CONFIG_FOUND)
    pkg_check_modules(ZSTD QUIET libzstd)
endif()

if (ZSTD_FOUND)
    set(ZSTD_TARGET ${ZSTD_LIBRARIES})
    include_directories(${ZSTD_INCLUDE_DIRS})
else()
    include(FetchContent)
    FetchContent_Declare(
        zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG        v1.5.6
        SOURCE_SUBDIR  build/cmake
    )
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zstd)
    set(ZSTD_TARGET libzstd_static)
    include_directories(${zstd_SOURCE_DIR}/lib)
endif()

# Executable target building test.cpp
add_executable(caml test.cpp)
target_link_libraries(caml PRIVATE ${ZSTD_TARGET} OpenSSL::Crypto)

# Installation targets
install(TARGETS caml DESTINATION bin)
install(DIRECTORY includes/caml DESTINATION include)
