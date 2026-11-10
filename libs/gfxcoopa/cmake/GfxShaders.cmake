# GfxShaders.cmake -- incremental GLSL -> SPIR-V compilation.
#
# Single copy, owned by gfxcoopa. Consumers reach it via
# `include(${GFXCOOPA_DIR}/cmake/GfxShaders.cmake)` (already implicitly
# available after `add_subdirectory(gfxcoopa)`, since CMake function
# definitions are global once the defining file has been processed) rather
# than copying this file into their own cmake/.
#
# Mirrors `cbuild --vulkan` (the primary build tool for this workspace, see
# /home/coopa/.sww/packages/cbuild/cbuild), which takes its -I paths from
# assets/shaders/.glslc_flags; this function passes the equivalent -I paths
# itself and does not read that file. Unlike cbuild -- which always
# recompiles everything -- this target is incremental via glslc -MD depfiles:
# editing a shared gfx/*.glsl body correctly retriggers every .vert/.frag
# that includes it, which a file(GLOB) + DEPENDS-on-the-source-only rule
# cannot see (glslc's -I search means an includer's dependency set isn't
# knowable from its own path alone).
#
# Requires CMake >= 3.20: DEPFILE on add_custom_command was Ninja-only before
# 3.20, when Makefile-generator support was added. This workspace has no
# ninja installed, so it's on the Makefile generator and needs this floor.
cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0116 NEW)  # depfile paths are relative to CMAKE_CURRENT_BINARY_DIR
                                # under NEW; irrelevant here since glslc -MT/-MF below
                                # are always given absolute paths, but pin it so behavior
                                # doesn't depend on the including project's policy stack.

find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/bin)

# Captured here, at include()-time, rather than read from CMAKE_CURRENT_LIST_DIR
# inside gfx_add_shader_target() below: CMAKE_CURRENT_LIST_DIR inside a
# function body tracks the CALLER's listfile (whichever CMakeLists.txt
# invoked gfx_add_shader_target), not this file's own directory, so a
# consumer's call would resolve gfxcoopa's shader dir relative to the
# consumer. At this file's top level CMAKE_CURRENT_LIST_DIR is this file's
# directory; CMake variables are dynamically scoped, so the function sees the
# captured value regardless of who calls it.
set(GFX_SHADERS_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# gfx_add_shader_target(<target-name>
#   SHADER_DIR  <dir containing *.vert/*.frag/*.tesc/*.tese/*.comp to compile>
#   [INCLUDE_DIR <dir passed to glslc -I, for a consumer's OWN #include <...>
#                headers>]
# )
#
# INCLUDE_DIR is optional and defaults to SHADER_DIR. gfxcoopa's own
# assets/shaders/ (home of the shared `gfx/brdf|ibl|sky|spot_light.glsl` and
# `gfx/surface2d/` headers) is ALWAYS additionally searched, so callers never
# need to know or pass gfxcoopa's shader dir themselves.
#
# Where the .spv files go:
#   OUTPUT_DIR <dir>               this target's .spv (and glslc depfiles) go there;
#   else GFX_SHADER_OUTPUT_ROOT    if the including build sets this variable, into
#                                  <root>/<TARGET_NAME>/ -- a build tree that shares a
#                                  source checkout with other build trees (toyengine's
#                                  linked game projects) keeps its shaders to itself;
#   else                           next to each source (the standalone default, which
#                                  the repos' own demos and tests load from).
# The runtime must look where they went (e.g. a ShaderLibrary over the output dirs).
function(gfx_add_shader_target TARGET_NAME)
    cmake_parse_arguments(ARG "" "SHADER_DIR;INCLUDE_DIR;OUTPUT_DIR" "" ${ARGN})
    if(NOT ARG_OUTPUT_DIR AND GFX_SHADER_OUTPUT_ROOT)
        set(ARG_OUTPUT_DIR "${GFX_SHADER_OUTPUT_ROOT}/${TARGET_NAME}")
    endif()

    if(NOT ARG_INCLUDE_DIR)
        set(ARG_INCLUDE_DIR "${ARG_SHADER_DIR}")
    endif()
    get_filename_component(GFX_BASE_SHADER_DIR "${GFX_SHADERS_CMAKE_DIR}/../assets/shaders" ABSOLUTE)

    if(NOT GLSLC)
        add_custom_target(${TARGET_NAME})
        message(WARNING "glslc not found; shaders must be compiled manually or via cbuild --vulkan")
        return()
    endif()

    # CONFIGURE_DEPENDS re-globs at build time so a newly added shader source
    # doesn't require a manual re-configure. It does NOT see .glsl includes --
    # that's the depfile's job below, not the glob's; gfx/*.glsl bodies are
    # deliberately never globbed here (only the *.vert/*.frag/*.tesc/*.tese/*.comp entry points are).
    file(GLOB SHADER_SRCS CONFIGURE_DEPENDS
        "${ARG_SHADER_DIR}/*.vert"
        "${ARG_SHADER_DIR}/*.frag"
        "${ARG_SHADER_DIR}/*.tesc"
        "${ARG_SHADER_DIR}/*.tese"
        "${ARG_SHADER_DIR}/*.comp")

    if(ARG_OUTPUT_DIR)
        file(MAKE_DIRECTORY "${ARG_OUTPUT_DIR}")
    endif()
    set(SPV_OUTPUTS "")
    foreach(SHADER ${SHADER_SRCS})
        if(ARG_OUTPUT_DIR)
            get_filename_component(SHADER_NAME "${SHADER}" NAME)
            set(SPV "${ARG_OUTPUT_DIR}/${SHADER_NAME}.spv")
        else()
            set(SPV "${SHADER}.spv")
        endif()
        add_custom_command(
            OUTPUT  "${SPV}"
            COMMAND ${GLSLC}
                    -I "${ARG_INCLUDE_DIR}"
                    -I "${GFX_BASE_SHADER_DIR}"
                    -MD -MF "${SPV}.d" -MT "${SPV}"
                    "${SHADER}" -o "${SPV}"
            DEPENDS "${SHADER}"
            DEPFILE "${SPV}.d"
            COMMENT "glslc ${SHADER}"
            VERBATIM
        )
        list(APPEND SPV_OUTPUTS "${SPV}")
    endforeach()

    add_custom_target(${TARGET_NAME} ALL DEPENDS ${SPV_OUTPUTS})
endfunction()
