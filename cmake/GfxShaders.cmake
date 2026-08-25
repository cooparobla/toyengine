# GfxShaders.cmake -- incremental GLSL -> SPIR-V compilation, shared across
# gfxcoopa, blendy, and toyengine (copied verbatim into each repo's cmake/;
# these repos have no submodules, so there's no include-from-sibling option).
#
# Mirrors `cbuild --vulkan` (the primary build tool for this workspace, see
# /home/coopa/.sww/packages/cbuild/cbuild), which reads the same
# assets/shaders/.glslc_flags file this does. Unlike cbuild -- which always
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

# gfx_add_shader_target(<target-name>
#   SHADER_DIR <dir containing *.vert/*.frag to compile>
#   INCLUDE_DIR <dir passed to glslc -I, for #include <gfx/...>>
# )
function(gfx_add_shader_target TARGET_NAME)
    cmake_parse_arguments(ARG "" "SHADER_DIR;INCLUDE_DIR" "" ${ARGN})

    if(NOT GLSLC)
        add_custom_target(${TARGET_NAME})
        message(WARNING "glslc not found; shaders must be compiled manually or via cbuild --vulkan")
        return()
    endif()

    # CONFIGURE_DEPENDS re-globs at build time so a newly added shader source
    # doesn't require a manual re-configure. It does NOT see .glsl includes --
    # that's the depfile's job below, not the glob's; gfx/*.glsl bodies are
    # deliberately never globbed here (only *.vert/*.frag entry points are).
    file(GLOB SHADER_SRCS CONFIGURE_DEPENDS
        "${ARG_SHADER_DIR}/*.vert"
        "${ARG_SHADER_DIR}/*.frag")

    set(SPV_OUTPUTS "")
    foreach(SHADER ${SHADER_SRCS})
        add_custom_command(
            OUTPUT  "${SHADER}.spv"
            COMMAND ${GLSLC}
                    -I "${ARG_INCLUDE_DIR}"
                    -MD -MF "${SHADER}.spv.d" -MT "${SHADER}.spv"
                    "${SHADER}" -o "${SHADER}.spv"
            DEPENDS "${SHADER}"
            DEPFILE "${SHADER}.spv.d"
            COMMENT "glslc ${SHADER}"
            VERBATIM
        )
        list(APPEND SPV_OUTPUTS "${SHADER}.spv")
    endforeach()

    add_custom_target(${TARGET_NAME} ALL DEPENDS ${SPV_OUTPUTS})
endfunction()
