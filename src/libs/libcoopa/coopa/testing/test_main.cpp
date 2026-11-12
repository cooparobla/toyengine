/**
 * @file test_main.cpp
 * @brief The coopa test runner (`coopa::test_main`): test selection, isolation and reporting.
 *
 * @code
 * ./<tests>                          # every test
 * ./<tests> --list                   # suite<TAB>name for every test, run nothing
 * ./<tests> --suite water            # one suite (repeatable) -- what each ctest entry runs
 * ./<tests> buoyancy wave            # tests whose suite/name contains any substring
 * ./<tests> -v --suite water         # also print every passing check
 * ./<tests> --slot gpu:2 --suite sky # hold one of 2 machine-wide "gpu" slots while running
 * @endcode
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

extern char** environ;

namespace coopa::test {
namespace {

struct RunState {
    const TestCase* current         = nullptr;
    int             failures        = 0;   /**< Failed checks in the current test. */
    int             checks          = 0;   /**< Checks in the current test. */
    bool            verbose         = false;
    bool            scratch_cleared = false;
};

RunState& state() {
    static RunState s;
    return s;
}

/** @brief The environment as NAME -> value, for restoring it after each test. */
std::map<std::string, std::string> snapshot_env() {
    std::map<std::string, std::string> env;
    for (char** e = environ; e && *e; ++e) {
        const std::string kv = *e;
        const size_t eq = kv.find('=');
        if (eq != std::string::npos) env.emplace(kv.substr(0, eq), kv.substr(eq + 1));
    }
    return env;
}

/** @brief Puts the environment back to `saved`: removes additions, restores changes. */
void restore_env(const std::map<std::string, std::string>& saved) {
    const auto now = snapshot_env();
    for (const auto& [name, value] : now) {
        if (!saved.count(name)) unsetenv(name.c_str());
    }
    for (const auto& [name, value] : saved) {
        auto it = now.find(name);
        if (it == now.end() || it->second != value) setenv(name.c_str(), value.c_str(), 1);
    }
}

/**
 * @brief Blocks until this process holds one of `count` machine-wide slots named `name`.
 *
 * Why: every GPU suite is a process holding a Vulkan device, every pipeline and a loaded scene
 * in memory the GPU shares with the CPU. ctest's -j bounds one ctest invocation, but nothing
 * bounds several running at once (two developers' shells, two agents, a build plus a test run),
 * and on a 16 GB machine ~16 concurrent Engines stalled the kernel into a watchdog panic. A slot
 * is an flock() on `<tmp>/coopa_tests/slots/<name>.<i>.lock`: global across processes, and
 * released by the kernel when the holder exits or dies, so a killed test never strands one.
 * The descriptor is deliberately leaked -- the slot is held for the life of the process.
 */
void acquire_slot(const std::string& name, int count) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "coopa_tests" / "slots";
    std::error_code ec;
    fs::create_directories(dir, ec);
    bool announced = false;
    for (;;) {
        for (int i = 0; i < count; ++i) {
            const std::string path = (dir / (name + "." + std::to_string(i) + ".lock")).string();
            const int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0666);
            if (fd < 0) continue;
            if (::flock(fd, LOCK_EX | LOCK_NB) == 0) return;
            ::close(fd);
        }
        if (!announced) {
            std::cout << "[ WAIT ] all " << count << " '" << name << "' slots are busy; waiting" << std::endl;
            announced = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool matches(const TestCase& t, const std::vector<std::string>& filters) {
    if (filters.empty()) return true;
    const std::string full = std::string(t.suite) + "/" + t.name;
    return std::any_of(filters.begin(), filters.end(),
                       [&](const std::string& f) { return full.find(f) != std::string::npos; });
}

void print_usage(const char* exe) {
    std::cout << "usage: " << exe << " [-v] [--list] [--suite <name>]... [substring ...]\n"
                 "  --list            print suite<TAB>name for every selected test, run nothing\n"
                 "  --suite <name>    run one suite (repeatable); a suite is one *_test.cpp file\n"
                 "  --slot <name>:<n> hold one of n machine-wide <name> slots for the run (e.g. gpu:2)\n"
                 "  -v, --verbose     print every passing check too\n"
                 "  <substring>       run tests whose suite/name contains any of these\n";
}

} // namespace

void record(bool ok, std::string_view what, Location loc) {
    RunState& s = state();
    ++s.checks;
    if (!ok) {
        ++s.failures;
        const std::string file = std::filesystem::path(loc.file).filename().string();
        std::cerr << "  [FAIL] " << file << ":" << loc.line << ": " << what << "\n";
    } else if (s.verbose) {
        std::cout << "  [ ok ] " << what << "\n";
    }
}

bool verbose() { return state().verbose; }

std::string current_test() {
    const TestCase* t = state().current;
    return t ? std::string(t->suite) + "/" + t->name : std::string();
}

std::filesystem::path scratch_dir(std::string_view sub) {
    namespace fs = std::filesystem;
    RunState& s = state();
    const fs::path root = fs::temp_directory_path() / "coopa_tests" /
                          (s.current ? s.current->suite : "_") / (s.current ? s.current->name : "_");
    if (!s.scratch_cleared) {
        std::error_code ec;
        fs::remove_all(root, ec);
        s.scratch_cleared = true;
    }
    const fs::path dir = sub.empty() ? root : root / sub;
    fs::create_directories(dir);
    return dir;
}

ScopedEnv::ScopedEnv(const char* name, const std::string& value) : name_(name) {
    if (const char* old = std::getenv(name)) {
        had_previous_ = true;
        previous_     = old;
    }
    setenv(name, value.c_str(), 1);
}

ScopedEnv::~ScopedEnv() {
    if (had_previous_) setenv(name_.c_str(), previous_.c_str(), 1);
    else               unsetenv(name_.c_str());
}

int run(int argc, char** argv) {
    std::vector<std::string> filters;
    std::set<std::string>    suites;
    bool                     list_only = false;
    std::vector<std::pair<std::string, int>> slots;
    RunState&                s         = state();

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose") {
            s.verbose = true;
        } else if (arg == "--list") {
            list_only = true;
        } else if (arg == "--suite") {
            if (i + 1 >= argc) {
                std::cerr << "--suite needs a name\n";
                return 2;
            }
            suites.insert(argv[++i]);
        } else if (arg == "--slot") {
            const std::string spec = i + 1 < argc ? argv[++i] : "";
            const size_t colon = spec.rfind(':');
            const int n = colon == std::string::npos ? 0 : std::atoi(spec.c_str() + colon + 1);
            if (colon == 0 || n <= 0) {
                std::cerr << "--slot needs <name>:<count>, e.g. gpu:2\n";
                return 2;
            }
            slots.emplace_back(spec.substr(0, colon), n);
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "unknown option '" << arg << "'\n";
            print_usage(argv[0]);
            return 2;
        } else {
            filters.push_back(arg);
        }
    }

    // A duplicate suite/name means two files claim the same suite: refuse rather than let
    // `--suite` silently run a mix of both.
    {
        std::map<std::string, const TestCase*> seen;
        std::map<std::string, std::string>     suite_file;
        for (const TestCase& t : registry()) {
            const std::string key = std::string(t.suite) + "/" + t.name;
            if (!seen.emplace(key, &t).second) {
                std::cerr << "duplicate test " << key << " (" << t.file << ":" << t.line << ")\n";
                return 2;
            }
            auto [it, fresh] = suite_file.emplace(t.suite, t.file);
            if (!fresh && it->second != t.file) {
                std::cerr << "suite '" << t.suite << "' is declared in two files: " << it->second
                          << " and " << t.file << " -- one suite per file\n";
                return 2;
            }
        }
    }

    std::vector<const TestCase*> selected;
    for (const TestCase& t : registry()) {
        if (!suites.empty() && !suites.count(t.suite)) continue;
        if (!matches(t, filters)) continue;
        selected.push_back(&t);
    }

    if (list_only) {
        for (const TestCase* t : selected) std::cout << t->suite << "\t" << t->name << "\n";
        return 0;
    }
    if (selected.empty()) {
        std::cerr << "No test matched -- nothing ran, which is a failure, not a pass.\n";
        return 2;
    }
    for (const auto& [name, count] : slots) acquire_slot(name, count);

    namespace fs = std::filesystem;
    using clock  = std::chrono::steady_clock;
    const auto base_env = snapshot_env();
    std::error_code ec;
    const fs::path base_cwd = fs::current_path(ec);

    int  failed_tests   = 0;
    long total_failures = 0;
    std::vector<std::pair<long long, std::string>> timings;
    const auto suite_start = clock::now();

    for (const TestCase* t : selected) {
        s.current         = t;
        s.failures        = 0;
        s.checks          = 0;
        s.scratch_cleared = false;
        const std::string full = std::string(t->suite) + "/" + t->name;
        std::cout << "[ RUN  ] " << full << std::endl;

        const auto start = clock::now();
        try {
            t->fn();
        } catch (const AssertionFailure&) {
            // Already recorded; a fatal check just ends the test.
        } catch (const std::exception& e) {
            record(false, std::string("uncaught exception: ") + e.what(), Location{t->file, unsigned(t->line)});
        } catch (...) {
            record(false, "uncaught non-std exception", Location{t->file, unsigned(t->line)});
        }
        const long long ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count();

        restore_env(base_env);
        if (!base_cwd.empty()) fs::current_path(base_cwd, ec);

        if (s.failures > 0) {
            ++failed_tests;
            total_failures += s.failures;
            std::cout << "[ FAIL ] " << full << " -- " << s.failures << " of " << s.checks
                      << " checks failed (" << ms << " ms)" << std::endl;
        } else {
            std::cout << "[  OK  ] " << full << " -- " << s.checks << " checks (" << ms << " ms)"
                      << std::endl;
        }
        timings.emplace_back(ms, full);
    }
    s.current = nullptr;

    const long long total_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - suite_start).count();

    // The slowest tests, every run: a suite that creeps from milliseconds to seconds is
    // noticed when it happens rather than when the whole run has become slow.
    if (timings.size() > 1) {
        std::sort(timings.begin(), timings.end(), std::greater<>());
        std::cout << "\nSlowest:\n";
        for (size_t i = 0; i < std::min<size_t>(3, timings.size()); ++i) {
            std::cout << "  " << timings[i].first << " ms  " << timings[i].second << "\n";
        }
    }

    if (failed_tests > 0) {
        std::cerr << "\n" << total_failures << " check(s) failed across " << failed_tests << " of "
                  << selected.size() << " test(s), in " << total_ms << " ms.\n"
                  << "Scratch files are under " << (fs::temp_directory_path() / "coopa_tests").string()
                  << "\n";
        return 1;
    }
    std::cout << "\nAll " << selected.size() << " test(s) passed in " << total_ms << " ms.\n";
    return 0;
}

} // namespace coopa::test

#ifndef COOPA_TEST_NO_MAIN
int main(int argc, char** argv) { return coopa::test::run(argc, argv); }
#endif
