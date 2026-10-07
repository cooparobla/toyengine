# ToyProject.cmake -- the game + editor executables for a toyengine project.
#
# Included by toyengine's own CMakeLists.txt, so it is available to the engine itself (whose
# project is the checkout: assets/ holds the test scenes) and to a game project that
# add_subdirectory()s <project>/.libs/toyengine (see templates/project/CMakeLists.txt):
#
#   toyengine_add_project(NAME <name> DIR <project dir>
#                         [EDITOR_NAME <name>]        # default: <name>_editor
#                         [SOURCES <file>...])        # the project's src/ files
#
# Produces:
#   <name>          the game: toyengine's main.cpp, or <DIR>/src/main.cpp when the project has one
#   <name>_editor   the editor (editor/editor.cpp), opening <DIR> by default
# Both compile SOURCES directly (not via a static library), so a TOY_MODULE() in them registers
# at static-init time and can never be dead-stripped (toyengine/core/module.h), and the editor's
# in-process Play runs the project's components exactly as the game does.
#
# <DIR>/src is searched BEFORE the engine checkout, so a project header at
# src/toyengine/<path>.h replaces the engine's -- the engine is header-only, so that is a real
# per-file override. An override must keep the API of the file it replaces.
#
# Shaders: <DIR>/assets/shaders/*.vert|*.frag compile with -I <project shaders>, -I <engine
# shaders>, -I <gfxcoopa shaders> (the runtime ShaderLibrary search order, see
# RuntimeLayout::shader_roots()), so a project shader can #include any engine/gfx header and a
# project file shadows the engine's of the same name.

cmake_minimum_required(VERSION 3.21)

# CACHE INTERNAL, not a plain variable: this file is included from the engine's directory scope,
# but a game project calls toyengine_add_project() from its own (parent) scope, which a normal
# variable set here would not reach.
set(TOYENGINE_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "toyengine's cmake/ directory")

# Build > Build (editor/build/build_pipeline.h) configures a separate build-ship/ directory with
# these for a Shipping build: Release, no debug env hooks / validation / exit screenshots
# (toyengine/core/runtime_paths.h's k_shipping), and optionally the .caml passphrase compiled in.
option(TOY_SHIPPING "Build the game for players (no debug hooks; see runtime_paths.h)" OFF)
set(TOY_CAML_KEY_BAKED "" CACHE STRING "Passphrase compiled into the game for .caml assets (empty: TOY_CAML_KEY / default)")

function(toyengine_add_project)
    cmake_parse_arguments(TP "" "NAME;DIR;EDITOR_NAME" "SOURCES" ${ARGN})
    if(NOT TP_NAME OR NOT TP_DIR)
        message(FATAL_ERROR "toyengine_add_project: NAME and DIR are required")
    endif()
    if(NOT TP_EDITOR_NAME)
        set(TP_EDITOR_NAME "${TP_NAME}_editor")
    endif()
    get_filename_component(TP_DIR "${TP_DIR}" ABSOLUTE)
    get_filename_component(_engine_dir "${TOYENGINE_CMAKE_DIR}/.." ABSOLUTE)

    # A project main.cpp replaces the engine's for the game only -- the editor has its own main.
    set(_sources ${TP_SOURCES})
    set(_main "${_engine_dir}/main.cpp")
    if(EXISTS "${TP_DIR}/src/main.cpp")
        set(_main "${TP_DIR}/src/main.cpp")
        list(FILTER _sources EXCLUDE REGEX "/src/main\\.cpp$")
    endif()

    add_executable(${TP_NAME} ${_main} ${_sources})
    add_executable(${TP_EDITOR_NAME} "${_engine_dir}/editor/editor.cpp" ${_sources})

    set(_deps toyengine_shaders uicoopa_shaders)
    set(_shader_dir "${TP_DIR}/assets/shaders")
    if(NOT TP_DIR STREQUAL _engine_dir AND IS_DIRECTORY "${_shader_dir}")
        _toyengine_project_shaders(${TP_NAME}_shaders "${_shader_dir}" "${_engine_dir}")
        list(APPEND _deps ${TP_NAME}_shaders)
    endif()

    foreach(_t ${TP_NAME} ${TP_EDITOR_NAME})
        if(IS_DIRECTORY "${TP_DIR}/src")
            target_include_directories(${_t} BEFORE PRIVATE "${TP_DIR}/src")
        endif()
        target_link_libraries(${_t} PRIVATE toyengine::engine)
        add_dependencies(${_t} ${_deps})
        target_compile_definitions(${_t} PRIVATE TOY_PROJECT_ROOT="${TP_DIR}")
    endforeach()
    target_compile_definitions(${TP_EDITOR_NAME} PRIVATE TOY_EDITOR=1
        TOY_GAME_BINARY="$<TARGET_FILE:${TP_NAME}>"
        # What Build compiles: this build tree's game target, and (for Shipping) the source
        # directory a fresh Release tree is configured from.
        TOY_BUILD_DIR="${CMAKE_BINARY_DIR}"
        TOY_GAME_TARGET="${TP_NAME}"
        TOY_GAME_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    if(TOY_SHIPPING)
        target_compile_definitions(${TP_NAME} PRIVATE TOY_SHIPPING=1)
    endif()
    if(TOY_CAML_KEY_BAKED)
        target_compile_definitions(${TP_NAME} PRIVATE TOY_CAML_KEY_BAKED="${TOY_CAML_KEY_BAKED}")
    endif()
    # Relocation: room for install_name_tool to rewrite library paths to @rpath when the game is
    # bundled into a .app (editor/build/bundle_macos.h); on Linux, look for bundled libraries
    # in <exe dir>/lib first (DT_RPATH, which also covers those libraries' own dependencies).
    if(APPLE)
        target_link_options(${TP_NAME} PRIVATE "LINKER:-headerpad_max_install_names")
    elseif(UNIX)
        target_link_options(${TP_NAME} PRIVATE "LINKER:--disable-new-dtags")
        set_property(TARGET ${TP_NAME} APPEND PROPERTY BUILD_RPATH "\$ORIGIN/lib")
    endif()
endfunction()

# Incremental glslc for a project's own shaders -- gfx_add_shader_target() (gfxcoopa) with the
# engine's shader directory added to the -I chain, which that function's single INCLUDE_DIR
# cannot express.
function(_toyengine_project_shaders TARGET_NAME SHADER_DIR ENGINE_DIR)
    find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/bin)
    if(NOT GLSLC)
        add_custom_target(${TARGET_NAME})
        message(WARNING "glslc not found; project shaders must be compiled manually")
        return()
    endif()
    file(GLOB _srcs CONFIGURE_DEPENDS "${SHADER_DIR}/*.vert" "${SHADER_DIR}/*.frag")
    set(_outs "")
    foreach(_s ${_srcs})
        add_custom_command(
            OUTPUT  "${_s}.spv"
            COMMAND ${GLSLC}
                    -I "${SHADER_DIR}"
                    -I "${ENGINE_DIR}/assets/shaders"
                    -I "${ENGINE_DIR}/libs/gfxcoopa/assets/shaders"
                    -MD -MF "${_s}.spv.d" -MT "${_s}.spv"
                    "${_s}" -o "${_s}.spv"
            DEPENDS "${_s}"
            DEPFILE "${_s}.spv.d"
            COMMENT "glslc ${_s}"
            VERBATIM)
        list(APPEND _outs "${_s}.spv")
    endforeach()
    add_custom_target(${TARGET_NAME} ALL DEPENDS ${_outs})
endfunction()
