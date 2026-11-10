#pragma once

/**
 * @file test.h
 * @brief The coopa test framework: self-registering tests, one suite per file, hermetic runs.
 *
 * Every repo in the coopa family (libcoopa, gfxcoopa, physxcoopa, sfxcoopa, uicoopa, mapcoopa,
 * toyengine and its editor) writes its tests against this one header and links
 * `coopa::test_main` for the runner. A test file looks like:
 *
 * @code
 * #include <coopa/testing/test.h>
 *
 * COOPA_TEST_SUITE("signal");                // one suite per file, named after the file
 *
 * COOPA_TEST(emit_reaches_every_slot) {
 *     coopa::Signal<int> s;
 *     int sum = 0;
 *     s.connect([&](int v) { sum += v; });
 *     s.emit(3);
 *     EXPECT_EQ(sum, 3);                     // non-fatal: records, the test carries on
 *     ASSERT_TRUE(s.slot_count() == 1);      // fatal: records, then ends the test
 *     coopa::test::expect(sum > 0, "a described check reads like a sentence");
 * }
 * @endcode
 *
 * ## The rules this framework enforces (so no test file has to remember them)
 *
 * - **Registration is declaration.** COOPA_TEST registers itself; there is no table to keep in
 *   sync, so a test cannot be written and then silently never run.
 * - **One suite per file.** COOPA_TEST_SUITE() names the file's suite and COOPA_TEST() will not
 *   compile without it. CMake's coopa_add_test_suites() registers one ctest entry per
 *   `<suite>_test.cpp` and runs it with `--suite <suite>`, so the file name, the suite name
 *   and the ctest name are the same thing, and `ctest -j` runs suites in parallel.
 * - **Tests are hermetic.** The runner snapshots the environment and the working directory
 *   before every test and restores both after it, so a `setenv("FIXED_DT", ...)` cannot
 *   change the meaning of a later test. scratch_dir() is a fresh per-test directory under the
 *   system temp dir -- never the repo -- so parallel suites never share files.
 * - **A thrown exception fails the test, not the run.**
 * - **Selecting nothing is a failure**, not a vacuous pass (a renamed suite cannot silently
 *   drop out of ctest).
 */

#include <cmath>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace coopa::test {

/** @brief One registered test. */
struct TestCase {
    const char* suite;
    const char* name;
    const char* file;
    int         line;
    void (*fn)();
};

/** @brief Every test linked into this executable, in registration (file, then source) order. */
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

/** @brief Static-initialisation hook behind COOPA_TEST. */
struct Registrar {
    explicit Registrar(const TestCase& t) { registry().push_back(t); }
};

/**
 * @brief Where a check was written. Not std::source_location: Apple clang 15 evaluates its
 *        current() in a default argument at the callee, so every check would report test.h.
 *        __builtin_FILE/__builtin_LINE in a default argument are the caller's on clang and gcc.
 */
struct Location {
    const char* file;
    unsigned    line;
    static constexpr Location here(const char* f = __builtin_FILE(), unsigned l = __builtin_LINE()) {
        return {f, l};
    }
};

/** @brief Thrown by a fatal check (ASSERT_*, require()) to end the current test. */
struct AssertionFailure {};

// --- Recording (implemented in test_main.cpp) -----------------------------------------------

/** @brief Records one check. Failures always print (with file:line); passes only under -v. */
void record(bool ok, std::string_view what, Location loc);

/** @brief True when the runner was given -v: tests may print extra diagnostics. */
bool verbose();

/** @brief The running test's "suite/name", for messages and dumped-file names. */
std::string current_test();

/**
 * @brief A fresh, empty directory for the running test: `<tmp>/coopa_tests/<suite>/<test>/`.
 *
 * Wiped at first use within a test, so leftovers from an earlier (possibly killed) run never
 * leak into this one. Optional `sub` names a subdirectory inside it, also created.
 */
std::filesystem::path scratch_dir(std::string_view sub = {});

// --- Checks ---------------------------------------------------------------------------------

/** @brief Non-fatal check with a sentence describing the expectation. Returns `ok`. */
inline bool expect(bool ok, std::string_view what,
                   Location loc = Location::here()) {
    record(ok, what, loc);
    return ok;
}

/** @brief expect() that ends the test on failure -- for preconditions later checks rely on. */
inline void require(bool ok, std::string_view what,
                    Location loc = Location::here()) {
    record(ok, what, loc);
    if (!ok) throw AssertionFailure{};
}

/** @brief |actual - expected| <= tolerance, printing both values on failure. */
inline bool expect_near(double actual, double expected, double tolerance, std::string_view what,
                        Location loc = Location::here()) {
    const bool ok = std::fabs(actual - expected) <= tolerance;
    if (ok) {
        record(true, what, loc);
    } else {
        std::ostringstream m;
        m << what << " (got " << actual << ", expected " << expected << " +-" << tolerance << ")";
        record(false, m.str(), loc);
    }
    return ok;
}

/** @brief actual >= minimum, printing the count on failure. */
inline bool expect_at_least(long long actual, long long minimum, std::string_view what,
                            Location loc = Location::here()) {
    const bool ok = actual >= minimum;
    if (ok) {
        record(true, what, loc);
    } else {
        std::ostringstream m;
        m << what << " (got " << actual << ", needed >= " << minimum << ")";
        record(false, m.str(), loc);
    }
    return ok;
}

namespace detail {

template <class T, class = void> struct Streamable : std::false_type {};
template <class T>
struct Streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

/** @brief `v` as text when it is streamable, else a placeholder. */
template <class T> std::string show(const T& v) {
    if constexpr (Streamable<T>::value) {
        std::ostringstream s;
        if constexpr (std::is_same_v<T, bool>) s << std::boolalpha;
        s << v;
        return s.str();
    } else {
        return "<unprintable>";
    }
}

/** @brief The shared body of the binary-comparison macros. */
template <class A, class B, class Op>
bool compare(const A& a, const B& b, Op op, const char* expr, Location loc) {
    const bool ok = op(a, b);
    if (ok) {
        record(true, expr, loc);
    } else {
        record(false, std::string(expr) + " (lhs " + show(a) + ", rhs " + show(b) + ")", loc);
    }
    return ok;
}

/** @brief Component-wise |a - b| <= eps for glm-style vectors (any type with operator[] and length()). */
template <class V>
bool vec_near(const V& a, const V& b, double eps, const char* expr, Location loc) {
    bool ok = true;
    for (int i = 0; i < static_cast<int>(a.length()); ++i) ok = ok && std::fabs(double(a[i]) - double(b[i])) <= eps;
    if (ok) {
        record(true, expr, loc);
    } else {
        std::ostringstream m;
        m << expr << " (got (";
        for (int i = 0; i < static_cast<int>(a.length()); ++i) m << (i ? ", " : "") << a[i];
        m << "), expected (";
        for (int i = 0; i < static_cast<int>(b.length()); ++i) m << (i ? ", " : "") << b[i];
        m << ") +-" << eps << ")";
        record(false, m.str(), loc);
    }
    return ok;
}

} // namespace detail

/**
 * @brief Sets an environment variable for a scope and restores it after.
 *
 * The runner already restores the whole environment between tests; this is for a test that
 * needs a variable for only part of its body.
 */
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value);
    ~ScopedEnv();
    ScopedEnv(const ScopedEnv&)            = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string name_;
    bool        had_previous_ = false;
    std::string previous_;
};

/** @brief The runner's entry point; test_main.cpp's main() calls it. */
int run(int argc, char** argv);

} // namespace coopa::test

// --- Declaration macros ---------------------------------------------------------------------

/** @brief Names this file's suite. Exactly one per test file, before its first COOPA_TEST. */
#define COOPA_TEST_SUITE(name_literal) \
    namespace { [[maybe_unused]] constexpr const char* coopa_test_suite_name_ = name_literal; }

/** @brief Declares and registers a test in this file's suite. `name` is a plain identifier. */
#define COOPA_TEST(name)                                                                           \
    static void coopa_test_fn_##name();                                                            \
    static const ::coopa::test::Registrar coopa_test_reg_##name{                                   \
        ::coopa::test::TestCase{coopa_test_suite_name_, #name, __FILE__, __LINE__, &coopa_test_fn_##name}}; \
    static void coopa_test_fn_##name()

// --- Check macros: EXPECT_* records and continues, ASSERT_* records and ends the test --------

#define COOPA_TEST_CMP_(a, b, op) \
    ::coopa::test::detail::compare((a), (b), [](const auto& x, const auto& y) { return x op y; }, \
                                   #a " " #op " " #b, ::coopa::test::Location::here())

#define EXPECT_TRUE(c)  ::coopa::test::expect(static_cast<bool>(c), #c)
#define EXPECT_FALSE(c) ::coopa::test::expect(!static_cast<bool>(c), "!(" #c ")")
#define EXPECT_EQ(a, b) COOPA_TEST_CMP_(a, b, ==)
#define EXPECT_NE(a, b) COOPA_TEST_CMP_(a, b, !=)
#define EXPECT_LT(a, b) COOPA_TEST_CMP_(a, b, <)
#define EXPECT_LE(a, b) COOPA_TEST_CMP_(a, b, <=)
#define EXPECT_GT(a, b) COOPA_TEST_CMP_(a, b, >)
#define EXPECT_GE(a, b) COOPA_TEST_CMP_(a, b, >=)
#define EXPECT_NEAR(a, b, eps) \
    ::coopa::test::expect_near(static_cast<double>(a), static_cast<double>(b), static_cast<double>(eps), #a " ~= " #b)
#define EXPECT_VEC_NEAR(a, b, eps) \
    ::coopa::test::detail::vec_near((a), (b), (eps), #a " ~= " #b, ::coopa::test::Location::here())

#define COOPA_TEST_FATAL_(check) do { if (!(check)) throw ::coopa::test::AssertionFailure{}; } while (0)

#define ASSERT_TRUE(c)             COOPA_TEST_FATAL_(EXPECT_TRUE(c))
#define ASSERT_FALSE(c)            COOPA_TEST_FATAL_(EXPECT_FALSE(c))
#define ASSERT_EQ(a, b)            COOPA_TEST_FATAL_(EXPECT_EQ(a, b))
#define ASSERT_NE(a, b)            COOPA_TEST_FATAL_(EXPECT_NE(a, b))
#define ASSERT_LT(a, b)            COOPA_TEST_FATAL_(EXPECT_LT(a, b))
#define ASSERT_LE(a, b)            COOPA_TEST_FATAL_(EXPECT_LE(a, b))
#define ASSERT_GT(a, b)            COOPA_TEST_FATAL_(EXPECT_GT(a, b))
#define ASSERT_GE(a, b)            COOPA_TEST_FATAL_(EXPECT_GE(a, b))
#define ASSERT_NEAR(a, b, eps)     COOPA_TEST_FATAL_(EXPECT_NEAR(a, b, eps))
#define ASSERT_VEC_NEAR(a, b, eps) COOPA_TEST_FATAL_(EXPECT_VEC_NEAR(a, b, eps))
#define ASSERT_THROWS(expr)                                                                    \
    do {                                                                                       \
        bool coopa_threw_ = false;                                                             \
        try { (void)(expr); } catch (...) { coopa_threw_ = true; }                             \
        COOPA_TEST_FATAL_(::coopa::test::expect(coopa_threw_, #expr " throws"));               \
    } while (0)
