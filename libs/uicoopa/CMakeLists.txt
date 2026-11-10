cmake_minimum_required(VERSION 3.16)
cmake_policy(VERSION 3.16)

project(uicoopa_test_window)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Standalone vs consumed ---
# UICOOPA_STANDALONE is true only when this file IS the top-level project. Everything
# that belongs to "the uicoopa repo as an application" -- its test executable and five
# windowed demos, its ctest registration, its generated root_directory.h, and its leak gate -- is gated on it,
# so a consumer that add_subdirectory()'s this repo gets exactly one thing: the coopa::ui
# target below. physxcoopa and sfxcoopa use the same guard.
#
# Every path below is derived from CMAKE_CURRENT_SOURCE_DIR / CMAKE_CURRENT_BINARY_DIR,
# never CMAKE_SOURCE_DIR / CMAKE_BINARY_DIR. Those two mean "the TOP-LEVEL project", so
# under add_subdirectory() they would point this repo's own includes, shader dir and
# generated header at the consumer's tree instead of at uicoopa. They are still correct
# for the standalone test itself, which is precisely a question about the top-level project.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(UICOOPA_STANDALONE TRUE)
else()
    set(UICOOPA_STANDALONE FALSE)
endif()

get_filename_component(ROOT_DIR_PARENT "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)

# --- Sibling library paths ---
set(GFXCOOPA_DIR "${ROOT_DIR_PARENT}/gfxcoopa")
set(CAML_DIR     "${ROOT_DIR_PARENT}/caml")
if(NOT TARGET coopa::gfx)
    add_subdirectory(${GFXCOOPA_DIR} ${CMAKE_CURRENT_BINARY_DIR}/gfxcoopa-build)
endif()

# --- Optional audio support (sfxcoopa) ---
# ON by default, but degrades to a no-audio build (rather than a configure
# failure) if the sibling repo simply isn't checked out. Every audio call
# site in this repo is guarded by #ifdef UICOOPA_HAS_AUDIO, so both configs
# build cleanly -- see uicoopa/audio/ui_audio.h's file doc.
set(SFXCOOPA_DIR "${ROOT_DIR_PARENT}/sfxcoopa")
option(UICOOPA_WITH_AUDIO "Build uicoopa with sfxcoopa audio support" ON)
set(UICOOPA_AUDIO_LIBS "")
set(UICOOPA_AUDIO_DEFS "")
if(UICOOPA_WITH_AUDIO AND EXISTS "${SFXCOOPA_DIR}/CMakeLists.txt")
    if(NOT TARGET coopa::sfx)
        add_subdirectory(${SFXCOOPA_DIR} ${CMAKE_CURRENT_BINARY_DIR}/sfxcoopa-build)
    endif()
    set(UICOOPA_AUDIO_LIBS coopa::sfx)
    set(UICOOPA_AUDIO_DEFS UICOOPA_HAS_AUDIO)
elseif(UICOOPA_WITH_AUDIO)
    message(WARNING "UICOOPA_WITH_AUDIO is ON but ${SFXCOOPA_DIR} was not found -- building without audio.")
endif()

# --- coopa::ui: the public, header-only consumable target. ---
# Named uicoopa_lib (not `uicoopa`) because that bare name is already the headless test
# executable below, and because `cplay` resolves the run binary as `./build/<project
# name>`. Mirrors coopa::gfx / coopa::lib exactly: INTERFACE, since everything under
# uicoopa/ is header-only with no .cpp files at all.
#
# The include dirs, compile definitions and linked libraries live HERE rather than in
# directory-scoped include_directories()/add_definitions() calls, because those propagate
# only downward into subdirectories -- a parent project that add_subdirectory()'s this
# repo would never see them. Carrying them on the target is what makes
# `target_link_libraries(myapp PRIVATE coopa::ui)` sufficient.
# uicoopa_impl: the one non-header-only piece -- the single TU that compiles
# stb_truetype's implementation block. See src/ui_impl.cpp for why this must be
# singular, and why a directory-scoped add_definitions() can't provide it to a
# consumer. Mirrors gfxcoopa_impl exactly.
add_library(uicoopa_impl STATIC src/ui_impl.cpp)
target_compile_features(uicoopa_impl PRIVATE cxx_std_20)
target_compile_definitions(uicoopa_impl PRIVATE STB_TRUETYPE_IMPLEMENTATION)
target_include_directories(uicoopa_impl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/includes)

add_library(uicoopa_lib INTERFACE)
add_library(coopa::ui ALIAS uicoopa_lib)
target_compile_features(uicoopa_lib INTERFACE cxx_std_20)
# NOTE for consumers: includes/ here also carries stb_image.h / stb_image_write.h,
# whose *implementations* live in gfxcoopa_impl. Link coopa::ui AFTER coopa::gfx so
# gfxcoopa's copies of those two headers win the include search -- otherwise a
# consumer could compile against uicoopa's header while linking gfxcoopa's body.
# stb_truetype is unique to this repo, so it has no such ambiguity.
target_include_directories(uicoopa_lib INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}                 # uicoopa/
    ${CMAKE_CURRENT_SOURCE_DIR}/includes        # stb/stb_truetype.h
    ${CAML_DIR}/includes                        # caml/ + fkYAML
    ${CAML_DIR})                                # caml.h root
# UICOOPA_HAS_AUDIO must be INTERFACE, not a directory-scoped
# add_compile_definitions(): a consumer compiling uicoopa/audio/*.h without it would
# silently get the no-audio stubs while linking against a coopa::sfx that is present.
target_compile_definitions(uicoopa_lib INTERFACE ${UICOOPA_AUDIO_DEFS})
target_link_libraries(uicoopa_lib INTERFACE uicoopa_impl coopa::gfx ${UICOOPA_AUDIO_LIBS})

# --- Shader compilation ---
# ui.vert #includes <gfx/surface2d/quad_vs.glsl>, gfxcoopa's shared 2D quad backbone.
# gfx_add_shader_target() (owned by gfxcoopa, see its own file doc) adds this repo's
# assets/shaders/ and gfxcoopa's assets/shaders/ to the -I search path and tracks .glsl
# body dependencies via glslc -MD depfiles, writing each .spv next to its source.
# assets/shaders/.glslc_flags gives cbuild --vulkan the same -I paths.
#
# Deliberately NOT gated on UICOOPA_STANDALONE: a consumer needs these .spv files at
# runtime just as much as the demos do (UiPass/UiWorldPass take .spv paths), so it
# add_dependencies() its own targets on uicoopa_shaders. The target name is already
# unique across a combined build.
include(${GFXCOOPA_DIR}/cmake/GfxShaders.cmake)
gfx_add_shader_target(uicoopa_shaders SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders")

if(UICOOPA_STANDALONE)
    # --- Configure and include root directory header ---
    # Only the demo/test .cpp files include <root_directory.h>; nothing under uicoopa/
    # does. Generating it under a consumer would put a second, conflicting ROOT_DIR macro
    # on the include path alongside the consumer's own.
    configure_file(configuration/root_directory.h.in configuration/root_directory.h)
    include_directories(${CMAKE_CURRENT_BINARY_DIR}/configuration)

    # --- Headless test suites ---
    # `uicoopa` is the headless test executable: one tests/<suite>_test.cpp per system on the
    # shared coopa test framework (coopa/testing/test.h), plus area-local fixtures under
    # tests/support/. coopa_add_test_suites() registers one ctest entry per suite
    # (uicoopa_<suite>), each run as `uicoopa --suite <suite>`, so `ctest -j` parallelises
    # them and none of them ever needs a display.
    file(GLOB UICOOPA_TEST_SOURCES CONFIGURE_DEPENDS
         ${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp
         ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/*.cpp)
    # ui_audio_test.cpp exercises sfxcoopa-backed types that only exist with audio on; in a
    # no-audio build its suite would select zero tests, which the runner treats as a failure.
    if(NOT UICOOPA_AUDIO_DEFS)
        list(FILTER UICOOPA_TEST_SOURCES EXCLUDE REGEX "/ui_audio_test\\.cpp$")
    endif()
    add_executable(uicoopa ${UICOOPA_TEST_SOURCES})
    target_include_directories(uicoopa PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(uicoopa PRIVATE coopa::ui coopa::test_main)
    add_dependencies(uicoopa uicoopa_shaders)

    # --- Windowed demos ---
    # Interactive programs under demos/, NOT tests: each opens a visible window, so none is
    # ever registered with ctest. Executable names are kept stable (tools and docs refer to
    # them); see docs/reference.md's "Demos" section for what each one shows.
    #
    #   uicoopa_test_window       demos/demo_window.cpp            YAML-declared UI (assets/scenes/test_window)
    #   uicoopa_settings_builder  demos/demo_settings_builder.cpp  settings panel built through UIBuilder
    #   uicoopa_dialog_builder    demos/demo_dialog_builder.cpp    dialog()/split_rows()/split_columns()/tab_view()
    #   uicoopa_gamepad_builder   demos/demo_gamepad_builder.cpp   gamepad navigation (InputMode::Hybrid)
    #   uicoopa_hud_builder       demos/demo_hud_builder.cpp       gameplay HUD bound to coopa::item/coopa::stat
    set(UICOOPA_DEMOS
        uicoopa_test_window:demo_window
        uicoopa_settings_builder:demo_settings_builder
        uicoopa_dialog_builder:demo_dialog_builder
        uicoopa_gamepad_builder:demo_gamepad_builder
        uicoopa_hud_builder:demo_hud_builder)
    set(UICOOPA_DEMO_TARGETS "")
    foreach(entry IN LISTS UICOOPA_DEMOS)
        string(REPLACE ":" ";" pair "${entry}")
        list(GET pair 0 demo_target)
        list(GET pair 1 demo_source)
        add_executable(${demo_target} demos/${demo_source}.cpp)
        target_link_libraries(${demo_target} PRIVATE coopa::ui)
        add_dependencies(${demo_target} uicoopa_shaders)
        list(APPEND UICOOPA_DEMO_TARGETS ${demo_target})
    endforeach()

    enable_testing()
    coopa_add_test_suites(uicoopa PREFIX uicoopa SOURCES ${UICOOPA_TEST_SOURCES} LABELS unit)

    # --- Vulkan/GLFW leak gate ---
    # uicoopa names zero Vk*/VK_*/vk*/GLFW*/glfw* symbols outside its documented
    # gfx-allow-vulkan escapes -- each windowed demo's own save_screenshot()'s
    # ad-hoc capture framebuffer (gfxcoopa has no Framebuffer wrapper). ON by default: this
    # repo is done, and the gate's job is making sure it can't regress.
    #
    # The option() itself is inside the standalone guard, not just the block it controls:
    # option() writes a CACHE entry, and consumers legitimately run the gate with a
    # different default over their own source (toyengine keeps it OFF while its render
    # pipeline still names raw Vulkan symbols). Whichever add_subdirectory ran first would
    # otherwise win the cache and silently flip the other repo's policy.
    option(GFX_LEAK_CHECK "Fail the build if consumer source names raw Vulkan/GLFW symbols" ON)
    if(GFX_LEAK_CHECK)
        add_custom_target(uicoopa_leak_check ALL
            COMMAND ${GFXCOOPA_DIR}/tools/check_no_vulkan.sh ${CMAKE_CURRENT_SOURCE_DIR})
        add_dependencies(uicoopa uicoopa_leak_check)
        foreach(demo_target IN LISTS UICOOPA_DEMO_TARGETS)
            add_dependencies(${demo_target} uicoopa_leak_check)
        endforeach()
        add_test(NAME uicoopa_no_vulkan
                 COMMAND ${GFXCOOPA_DIR}/tools/check_no_vulkan.sh ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
endif()
