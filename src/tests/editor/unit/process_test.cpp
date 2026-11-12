/**
 * @file process_test.cpp
 * @brief Child processes for Build > Refresh: cancel() ends the whole process group promptly, compiler
 * diagnostics parse into file / line / column / message, and current_executable() is this
 * binary.
 */

#include <coopa/testing/test.h>

#include <chrono>
#include <thread>

#include "editor/core/process.h"
#include "editor/support/fixtures.h"

COOPA_TEST_SUITE("process");

namespace toy::editor::testing {

COOPA_TEST(cancel_ends_the_group_and_diagnostics_parse) {
    // Cancel stops the command and everything it started (its process group), promptly.
    Task t;
    const auto t0 = std::chrono::steady_clock::now();
    t.start("sleepy", "echo started; sleep 30 & sleep 30; echo never");
    while (t.line_count() < 2 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    t.cancel();
    t.wait();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    expect(!t.running() && t.cancelled() && !t.succeeded() && secs < 10.0, "cancel() ends a running task (" + std::to_string(secs) + " s)");
    const auto lines = t.lines();
    expect(std::find(lines.begin(), lines.end(), "never") == lines.end(), "...before it finishes");

    // Compiler diagnostics, as the build modal lists them.
    auto d = parse_diagnostic("/p/src/game.cpp:12:5: error: no member named 'x'");
    expect(d && d->error && d->file == "/p/src/game.cpp" && d->line == 12 && d->column == 5 && d->message == "no member named 'x'",
           "a clang error parses into file, line, column, message");
    d = parse_diagnostic("/p/src/game.h:7: warning: unused");
    expect(d && !d->error && d->line == 7 && d->column == 0, "a warning without a column parses");
    d = parse_diagnostic("ld: error: undefined symbol: foo");
    expect(d && d->error && d->line == 0, "a linker error without a location still counts");
    expect(!parse_diagnostic("[ 42%] Building CXX object CMakeFiles/x.dir/a.cpp.o"), "progress lines are not diagnostics");
    expect(current_executable().filename() == "toyengine_editor_tests" && fs::exists(current_executable()),
           "current_executable() finds this binary (what Relaunch Editor exec()s)");
}

} // namespace toy::editor::testing
