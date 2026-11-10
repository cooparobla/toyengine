# CoopaTesting.cmake -- registers coopa test suites with CTest (see coopa/testing/test.h).
#
#   coopa_add_test_suites(<target>
#       PREFIX <prefix>                 # ctest names are <prefix>_<suite>
#       SOURCES <file>...               # the target's test files; each *_test.cpp is one suite
#       [LABELS <label>...]             # e.g. unit / gpu / extended -- select with `ctest -L`
#       [TIMEOUT <seconds>]             # per suite; default 30
#       [WORKING_DIRECTORY <dir>]
#       [SLOT <name>:<n>]               # each suite holds one of n machine-wide slots (runner --slot)
#       [PROCESSORS <n>])               # ctest -j budget each suite consumes (default 1)
#
# Heavy suites (a Vulkan device, pipelines, a loaded scene each) pass both, e.g.
# `SLOT gpu:2 PROCESSORS 4`: PROCESSORS stops one `ctest -j8` launching them all together, and
# the slot bounds them across every concurrent ctest/test process on the machine -- several
# test runs at once with unbounded GPU suites have exhausted a 16 GB machine into a kernel panic.
#
# The suite name is the file name minus `_test.cpp` (water_test.cpp -> "water"), and the file
# itself declares COOPA_TEST_SUITE("water"). They are the same thing by convention, and the
# runner enforces it: `--suite water` with no matching test exits non-zero, so a file whose
# declared suite drifts from its name fails ctest instead of silently running nothing.
function(coopa_add_test_suites target)
    cmake_parse_arguments(ARG "" "PREFIX;TIMEOUT;WORKING_DIRECTORY;SLOT;PROCESSORS" "SOURCES;LABELS" ${ARGN})
    if(NOT ARG_PREFIX)
        set(ARG_PREFIX ${target})
    endif()
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 30)
    endif()
    set(slot_args "")
    if(ARG_SLOT)
        set(slot_args --slot ${ARG_SLOT})
    endif()
    foreach(src IN LISTS ARG_SOURCES)
        get_filename_component(stem "${src}" NAME_WE)
        if(NOT stem MATCHES "_test$")
            continue()
        endif()
        string(REGEX REPLACE "_test$" "" suite "${stem}")
        set(name ${ARG_PREFIX}_${suite})
        if(ARG_WORKING_DIRECTORY)
            add_test(NAME ${name} COMMAND ${target} ${slot_args} --suite ${suite} WORKING_DIRECTORY ${ARG_WORKING_DIRECTORY})
        else()
            add_test(NAME ${name} COMMAND ${target} ${slot_args} --suite ${suite})
        endif()
        set_tests_properties(${name} PROPERTIES TIMEOUT ${ARG_TIMEOUT})
        if(ARG_PROCESSORS)
            set_tests_properties(${name} PROPERTIES PROCESSORS ${ARG_PROCESSORS})
        endif()
        if(ARG_LABELS)
            set_tests_properties(${name} PROPERTIES LABELS "${ARG_LABELS}")
        endif()
    endforeach()
endfunction()
