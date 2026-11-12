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
#   <name>_editor   the editor (src/editor/editor.cpp), opening <DIR> by default
# Both compile SOURCES directly (not via a static library), so a TOY_MODULE() in them registers
# at static-init time and can never be dead-stripped (src/toyengine/core/module.h), and the editor's
# in-process Play runs the project's components exactly as the game does.
#
# <DIR>/src is on the include path for the project's own headers. Engine code is compiled into
# the toyengine::engine library, so a project cannot replace an engine file from src/ -- to change
# engine code, fork the engine (the project's .libs/toyengine checkout).
#
# The project directory reaches the engine at static-init time (io/runtime_paths.h,
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

# Build > Build (src/editor/build/build_pipeline.h) configures a separate build-ship/ directory with
# these for a Shipping build: Release, no debug env hooks / validation / exit screenshots
# (src/toyengine/io/runtime_paths.h's k_shipping), and optionally the .caml passphrase compiled in.
option(TOY_SHIPPING "Build the game for players (no debug hooks; see runtime_paths.h)" OFF)
# The debug stats overlay (src/toyengine/debug/debug_overlay.h) is compiled out of a Shipping build;
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
    set(_main "${_engine_dir}/src/main.cpp")
    if(EXISTS "${TP_DIR}/src/main.cpp")
        set(_main "${TP_DIR}/src/main.cpp")
        list(FILTER _sources EXCLUDE REGEX "/src/main\\.cpp$")
    endif()

    add_executable(${TP_NAME} ${_main} ${_sources})
    add_executable(${TP_EDITOR_NAME} "${_engine_dir}/src/editor/editor.cpp" ${_sources})

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
    # library before main() (src/editor/build/build_pipeline.h, BuildEnvironment::set_compiled()).
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
    # bundled into a .app (src/editor/build/bundle_macos.h); on Linux, look for bundled libraries
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
                    -I "${ENGINE_DIR}/src/libs/gfxcoopa/assets/shaders"
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

# toyengine_add_project_tests(NAME <n> DIR <project dir> [PREFIX <p>] SOURCES <file>...)
#
# The suite runners for a toyengine PROJECT, written against coopa/testing/test.h exactly as the
# engine's own are (src/tests/CMakeLists.txt, whose _toy_add_test_area() this generalises). One
# suite per file, tiered by directory:
#
#   <DIR>/tests/support/*.cpp        fixtures shared by the project's suites (not suites)
#   <DIR>/tests/unit/*_test.cpp      no Vulkan device, no window: milliseconds. ctest label `unit`
#   <DIR>/tests/gpu/*_test.cpp       constructs an Engine (device, pipelines, a scene). label `gpu`
#   <DIR>/tests/editor_gpu/*_test.cpp   the same against the editor. label `gpu`
#
# Produces <n>_tests (unit + gpu, linked to toyengine::engine) and, when editor_gpu/ exists,
# <n>_editor_tests (linked to toyengine::editor). Adding a test file needs no edit here -- the
# directories are globbed.
#
# SOURCES (the project's own src/) is compiled into each runner, not linked as a library, for the
# same reason toyengine_add_project() does it: a TOY_MODULE() in a static library would be
# dead-stripped and the project's components would silently not exist inside its own tests.
#
# Callable only from a top-level engine build (the engine's CMakeLists.txt returns before
# enable_testing() when it is a subdirectory), which is why projects/ lives in this checkout.
function(toyengine_add_project_tests)
    cmake_parse_arguments(TPT "" "NAME;DIR;PREFIX" "SOURCES" ${ARGN})
    if(NOT TPT_NAME OR NOT TPT_DIR)
        message(FATAL_ERROR "toyengine_add_project_tests: NAME and DIR are required")
    endif()
    if(NOT TPT_PREFIX)
        set(TPT_PREFIX "${TPT_NAME}")
    endif()
    get_filename_component(TPT_DIR "${TPT_DIR}" ABSOLUTE)
    get_filename_component(_engine_dir "${TOYENGINE_CMAKE_DIR}/.." ABSOLUTE)
    set(_tests "${TPT_DIR}/tests")

    # A runner resolves the project's assets (its scenes, its shaders) through the same generated
    # project-root source the game and editor executables carry, so a test names a scene by the
    # path the project itself uses.
    set(_root_src "${CMAKE_CURRENT_BINARY_DIR}/${TPT_NAME}_tests_project_root.cpp")
    set(TOY_PROJECT_ROOT_DIR "${TPT_DIR}")
    configure_file("${TOYENGINE_CMAKE_DIR}/project_root.cpp.in" "${_root_src}" @ONLY)

    # The project's sources minus its game entry point: a runner has coopa::test_main's main().
    set(_proj_src ${TPT_SOURCES})
    list(FILTER _proj_src EXCLUDE REGEX "/src/main\\.cpp$")

    file(GLOB _support CONFIGURE_DEPENDS "${_tests}/support/*.cpp")
    file(GLOB _unit    CONFIGURE_DEPENDS "${_tests}/unit/*_test.cpp")
    file(GLOB _gpu     CONFIGURE_DEPENDS "${_tests}/gpu/*_test.cpp")
    file(GLOB _ed_gpu  CONFIGURE_DEPENDS "${_tests}/editor_gpu/*_test.cpp")
    list(SORT _unit)
    list(SORT _gpu)
    list(SORT _ed_gpu)

    # _toyengine_project_test_target(<target> <sources...>) -- the parts both runners share.
    macro(_toyengine_project_test_target target)
        # The engine's own fixtures too (render_fixture, scene_variant, checks): a project's GPU
        # suites are written against them, reached by the same "engine/support/..." include the
        # engine's suites use. Compiled in rather than linked: the engine's test runners are
        # executables, so there is no library to link against.
        file(GLOB _engine_support CONFIGURE_DEPENDS
             "${_engine_dir}/src/tests/engine/support/*.cpp")
        add_executable(${target} ${ARGN} ${_support} ${_engine_support} ${_proj_src} "${_root_src}")
        # Without the shader targets a runner throws `could not resolve 'smaa_edge.vert.spv'` the
        # moment a test constructs an Engine, and passes only while a previous build's .spv
        # happen to still be on disk. The project's own shaders are a dependency for the same
        # reason -- see _toyengine_project_shaders().
        add_dependencies(${target} toyengine_shaders uicoopa_shaders shaders)
        if(TARGET ${TPT_NAME}_shaders)
            add_dependencies(${target} ${TPT_NAME}_shaders)
        endif()
        # <engine>/src/tests so a project suite reuses the engine's fixtures by the same include
        # the engine's own suites use ("engine/support/scene_variant.h"); <DIR>/src for the
        # project's own headers; <DIR>/tests for its fixtures.
        # TOYENGINE_CONFIG_INCLUDE_DIR first: it holds the only root_directory.h that defines
        # SRC_DIR, which editor/app/editor_theme.h needs, and every coopa library ships one of
        # its own on an equally PUBLIC include path (see the engine's configure_file).
        target_include_directories(${target} BEFORE PRIVATE
            "${TOYENGINE_CONFIG_INCLUDE_DIR}"
            "${TPT_DIR}/src" "${_tests}" "${_engine_dir}/src/tests")
        # The engine fixtures load shipped assets (tile-set pieces, meshes) and resolve them
        # against ROOT_DIR, which in a project's tree is the PROJECT root -- where the engine's
        # assets/ is not. This is the second place they look.
        target_compile_definitions(${target} PRIVATE
            TOY_ENGINE_ASSETS_DIR="${_engine_dir}/assets")
        # Alongside the engine's runners: their paths are user-facing (CLAUDE.md, src/tests/README.md).
        set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
        target_link_libraries(${target} PRIVATE coopa::test_main)
    endmacro()

    if(_unit OR _gpu)
        _toyengine_project_test_target(${TPT_NAME}_tests ${_unit} ${_gpu})
        target_link_libraries(${TPT_NAME}_tests PRIVATE toyengine::engine)
        coopa_add_test_suites(${TPT_NAME}_tests PREFIX ${TPT_PREFIX}
                              SOURCES ${_unit} LABELS unit TIMEOUT 30)
        # PROCESSORS 4 lets one `ctest -j8` run two GPU suites at a time; SLOT gpu:2 holds that
        # bound machine-wide, across concurrent ctest runs -- shared with the engine's suites, so
        # a project's GPU tier and the engine's together still cannot exhaust the box.
        coopa_add_test_suites(${TPT_NAME}_tests PREFIX ${TPT_PREFIX}
                              SOURCES ${_gpu} LABELS gpu TIMEOUT 300 SLOT gpu:2 PROCESSORS 4)
    endif()

    if(_ed_gpu)
        # The editor fixtures (EditorSession, the input driver, tick/dump) are compiled, not a
        # library -- the engine's own editor runner builds them the same way. Only this target
        # gets them: they pull in toyengine::editor, which the engine-only runner does not link.
        file(GLOB _editor_support CONFIGURE_DEPENDS
             "${_engine_dir}/src/tests/editor/support/*.cpp")
        _toyengine_project_test_target(${TPT_NAME}_editor_tests ${_ed_gpu} ${_editor_support})
        target_link_libraries(${TPT_NAME}_editor_tests PRIVATE toyengine::editor)
        target_compile_definitions(${TPT_NAME}_editor_tests PRIVATE TOY_EDITOR=1)
        coopa_add_test_suites(${TPT_NAME}_editor_tests PREFIX ${TPT_PREFIX}_editor
                              SOURCES ${_ed_gpu} LABELS gpu TIMEOUT 300 SLOT gpu:2 PROCESSORS 4)
    endif()
endfunction()
