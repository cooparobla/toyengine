# planet -- planetary worlds for toyengine, as a project rather than an engine feature.
#
# The engine is flat-terrain only and knows nothing about planets (tools/check_core_is_flat.sh
# enforces that). Everything that makes a world round lives here: the cube-sphere layout, the
# terrain streaming policy, the atmosphere, the camera fit, the six-face map bake, and the
# shaders. It is a planet build or a flat build, never both at once in one scene.
#
# This is an ordinary toyengine project -- the same toyengine_add_project() the engine's own
# game and editor targets go through -- that happens to live in the engine checkout so one
# build/ tree configures and verifies both worlds. Nothing here is referenced by src/toyengine,
# so lifting this directory out into its own repository is a move and no code change.
cmake_minimum_required(VERSION 3.21)

file(GLOB_RECURSE PLANET_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)

toyengine_add_project(NAME planet DIR ${CMAKE_CURRENT_SOURCE_DIR} SOURCES ${PLANET_SOURCES})
toyengine_add_project_tests(NAME planet DIR ${CMAKE_CURRENT_SOURCE_DIR} PREFIX planet
                            SOURCES ${PLANET_SOURCES})
