# @NAME@ -- built on the toyengine checkout in .libs/toyengine (./setup.sh fetches or links it).
cmake_minimum_required(VERSION 3.21)
project(@TARGET@ CXX)

# The engine and its coopa libraries. Its own tests/tools are skipped when it isn't top-level.
add_subdirectory(.libs/toyengine ${CMAKE_BINARY_DIR}/toyengine)

# Everything under src/ compiles into both the game and the editor (so the editor's Play runs
# your components). src/main.cpp, if present, replaces the engine's game entry point. Engine code
# is a compiled library: to change it, edit (or fork) the checkout in .libs/toyengine.
file(GLOB_RECURSE PROJECT_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
toyengine_add_project(NAME @TARGET@ DIR ${CMAKE_CURRENT_SOURCE_DIR} SOURCES ${PROJECT_SOURCES})
