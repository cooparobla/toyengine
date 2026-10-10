# toyengine's tests -- written against coopa/testing/test.h (libcoopa), one suite per file.
#
#   tests/<area>/support/    fixtures and helpers shared by that area's suites (not tests)
#   tests/<area>/unit/       no Vulkan device, no window: milliseconds. ctest label `unit`.
#   tests/<area>/gpu/        constructs an Engine (device, pipelines, a scene).  label `gpu`.
#   tests/<area>/extended/   image-quality metrics and perf budgets -- tuned thresholds that are
#                            worth running when working on that system, not on every change.
#                            Built and registered only with -DTOY_EXTENDED_TESTS=ON. label `extended`.
#
# Every <suite>_test.cpp is one ctest entry, `<exe-prefix>_<suite>`, so `ctest -j` runs suites in
# parallel and a dev picks a system by name (`ctest -R water`) or a tier by label (`ctest -L unit`).
# Adding a test file needs no edit here: the directories are globbed.

option(TOY_EXTENDED_TESTS "Build and register the extended (quality/perf) test suites" OFF)

# _toy_test_sources(<out-var> <area> <tier>) -- the *_test.cpp files of one tier.
function(_toy_test_sources out area tier)
    file(GLOB files CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${area}/${tier}/*_test.cpp)
    list(SORT files)
    set(${out} ${files} PARENT_SCOPE)
endfunction()

# _toy_add_test_area(<target> <area> <prefix>) -- one executable per area, every tier's suites
# registered with ctest under its tier's label and timeout.
function(_toy_add_test_area target area prefix)
    file(GLOB support CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${area}/support/*.cpp)
    _toy_test_sources(unit_src ${area} unit)
    _toy_test_sources(gpu_src  ${area} gpu)
    set(ext_src "")
    if(TOY_EXTENDED_TESTS)
        _toy_test_sources(ext_src ${area} extended)
    endif()
    if(NOT unit_src AND NOT gpu_src AND NOT ext_src)
        return()
    endif()

    add_executable(${target} ${support} ${unit_src} ${gpu_src} ${ext_src})
    add_dependencies(${target} toyengine_shaders uicoopa_shaders)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${target} PRIVATE toyengine::engine coopa::test_main)

    coopa_add_test_suites(${target} PREFIX ${prefix} SOURCES ${unit_src} LABELS unit TIMEOUT 30)
    # GPU suites each hold a device, every pipeline and a loaded scene in memory the GPU shares
    # with the CPU. Unbounded, several concurrent runs have exhausted a 16 GB machine into a
    # kernel watchdog panic, so: PROCESSORS 4 lets one `ctest -j8` run two at a time, and
    # SLOT gpu:2 holds that bound machine-wide (across concurrent ctest runs, shells, agents).
    # The timeout covers waiting for a slot as well as a cold shader cache on a slow GPU.
    coopa_add_test_suites(${target} PREFIX ${prefix} SOURCES ${gpu_src} LABELS gpu
                          TIMEOUT 300 SLOT gpu:2 PROCESSORS 4)
    coopa_add_test_suites(${target} PREFIX ${prefix} SOURCES ${ext_src} LABELS extended
                          TIMEOUT 600 SLOT gpu:2 PROCESSORS 4)
endfunction()

_toy_add_test_area(toyengine_tests engine toyengine)

_toy_add_test_area(toyengine_editor_tests editor toyengine_editor)
if(TARGET toyengine_editor_tests)
    # The packaging/build suites run the game binary relocated.
    add_dependencies(toyengine_editor_tests toyengine)
endif()

# toyengine_editor_docs: the editor user manual's screenshot generator (see the editor-user-docs
# skill). Built on the same fixtures and runner, but it produces images rather than checking
# anything, so it is a tool and is never registered with ctest.
file(GLOB _docs_src CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/editor/docs/*.cpp)
file(GLOB _editor_support CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/editor/support/*.cpp)
if(_docs_src)
    add_executable(toyengine_editor_docs ${_editor_support} ${_docs_src})
    add_dependencies(toyengine_editor_docs toyengine_shaders uicoopa_shaders)
    target_include_directories(toyengine_editor_docs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(toyengine_editor_docs PRIVATE toyengine::engine coopa::test_main)
endif()
