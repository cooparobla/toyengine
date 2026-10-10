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
# <DIR>/src is on the include path for the project's own headers. Engine code is compiled into
# the toyengine::engine library, so a project cannot replace an engine file from src/ -- to change
# engine code, fork the engine (the project's .libs/toyengine checkout).
#
# The project directory reaches the engine at static-init time (core/runtime_paths.h,
# set_build_project_root()) through a small generated source compiled into both executables,
# rather than a compile definition, so one compiled engine library serves every executable.
#
# Shaders: <DIR>/assets/shaders/*.vert|*.frag|*.comp (and *.tesc|*.tese) compile with -I <project shaders>, -I <engine
# shaders>, -I <gfxcoopa shaders> (the runtime ShaderLibrary search order, see
# RuntimeLayout::shader_roots()), so a project shader can #include any engine/gfx header and a
# project file shadows the engine's of the same name. The .spv go to
# <build>/shaders/project_shaders/ (TOY_SHADER_BUILD_DIR), like the engine's own.

cmake_minimum_required(VERSION 3.21)

# CACHE INTERNAL, not a plain variable: this file is included from the engine's directory scope,
# but a game project calls toyengine_add_project() from its own (parent) scope, which a normal
# variable set here would not reach.
set(TOYENGINE_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "toyengine's cmake/ directory")

# Build > Build (editor/build/build_pipeline.h) configures a separate build-ship/ directory with
# these for a Shipping build: Release, no debug env hooks / validation / exit screenshots
# (toyengine/core/runtime_paths.h's k_shipping), and optionally the .caml passphrase compiled in.
option(TOY_SHIPPING "Build the game for players (no debug hooks; see runtime_paths.h)" OFF)
# The debug stats overlay (toyengine/debug/debug_overlay.h) is compiled out of a Shipping build;
# this keeps it in (still off unless config.yaml's debug.overlay or F3 turns it on).
option(TOY_SHIPPING_DEBUG_OVERLAY "Keep the debug stats overlay in a TOY_SHIPPING build" OFF)
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

    set(_root_src "${CMAKE_CURRENT_BINARY_DIR}/${TP_NAME}_project_root.cpp")
    set(TOY_PROJECT_ROOT_DIR "${TP_DIR}")
    configure_file("${TOYENGINE_CMAKE_DIR}/project_root.cpp.in" "${_root_src}" @ONLY)
    foreach(_t ${TP_NAME} ${TP_EDITOR_NAME})
        if(IS_DIRECTORY "${TP_DIR}/src")
            target_include_directories(${_t} BEFORE PRIVATE "${TP_DIR}/src")
        endif()
        target_sources(${_t} PRIVATE "${_root_src}")
        add_dependencies(${_t} ${_deps})
    endforeach()
    target_link_libraries(${TP_NAME} PRIVATE toyengine::engine)
    target_link_libraries(${TP_EDITOR_NAME} PRIVATE toyengine::editor)   # brings toyengine::engine
    target_compile_definitions(${TP_EDITOR_NAME} PRIVATE TOY_EDITOR=1)
    # What Build compiles: this build tree's game target and executable, and (for Shipping) the
    # source directory a fresh Release tree is configured from -- registered with the editor
    # library before main() (editor/build/build_pipeline.h, BuildEnvironment::set_compiled()).
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${TP_EDITOR_NAME}_build_env.cpp" CONTENT
"// Generated by toyengine_add_project() (cmake/ToyProject.cmake) -- do not edit.
#include \"editor/build/build_pipeline.h\"

namespace {
const bool k_build_env_registered = [] {
    toy::editor::BuildEnvironment e;
    e.build_dir = \"${CMAKE_BINARY_DIR}\";
    e.game_target = \"${TP_NAME}\";
    e.source_dir = \"${CMAKE_SOURCE_DIR}\";
    e.game_binary = \"$<TARGET_FILE:${TP_NAME}>\";
    toy::editor::BuildEnvironment::set_compiled(e);
    return true;
}();
}
")
    target_sources(${TP_EDITOR_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/${TP_EDITOR_NAME}_build_env.cpp")

    # TOY_SHIPPING / TOY_DEBUG_OVERLAY / TOY_CAML_KEY_BAKED: set on the engine library for the
    # whole tree (CMakeLists.txt), so the library and both executables agree.
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
    file(GLOB _srcs CONFIGURE_DEPENDS "${SHADER_DIR}/*.vert" "${SHADER_DIR}/*.frag" "${SHADER_DIR}/*.tesc" "${SHADER_DIR}/*.tese" "${SHADER_DIR}/*.comp")
    set(_out_dir "${TOY_SHADER_BUILD_DIR}/project_shaders")
    file(MAKE_DIRECTORY "${_out_dir}")
    set(_outs "")
    foreach(_s ${_srcs})
        get_filename_component(_name "${_s}" NAME)
        set(_spv "${_out_dir}/${_name}.spv")
        add_custom_command(
            OUTPUT  "${_spv}"
            COMMAND ${GLSLC}
                    -I "${SHADER_DIR}"
                    -I "${ENGINE_DIR}/assets/shaders"
                    -I "${ENGINE_DIR}/libs/gfxcoopa/assets/shaders"
                    -MD -MF "${_spv}.d" -MT "${_spv}"
                    "${_s}" -o "${_spv}"
            DEPENDS "${_s}"
            DEPFILE "${_spv}.d"
            COMMENT "glslc ${_s}"
            VERBATIM)
        list(APPEND _outs "${_spv}")
    endforeach()
    add_custom_target(${TARGET_NAME} ALL DEPENDS ${_outs})
endfunction()
