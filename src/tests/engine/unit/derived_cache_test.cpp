/**
 * @file derived_cache_test.cpp
 * @brief The derived cache: one directory per source, orphans dropped, oldest evicted.
 *
 * The cache is only worth having if it can be trusted to clean up after itself -- it holds
 * hundreds of megabytes of regenerable data under the user's cache directory, and nothing else
 * ever looks at it. So what is pinned here is the lifetime rules rather than the happy path:
 * that two sources never share an entry, that an entry whose source is gone is removed, and that
 * a cap is honoured oldest-first.
 *
 * Every case redirects `XDG_CACHE_HOME` at the test's own scratch directory, so none of this
 * touches the real cache.
 */

#include <coopa/testing/test.h>

#include <toyengine/io/derived_cache.h>
#include <toyengine/io/runtime_paths.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

COOPA_TEST_SUITE("derived_cache");

namespace {

using coopa::test::expect;
using coopa::test::require;

/// Points the cache at this test's scratch directory and hands back the root it will use.
std::filesystem::path isolate(const std::string& tag) {
    const std::filesystem::path home = coopa::test::scratch_dir(tag);
    std::filesystem::remove_all(home);
    std::filesystem::create_directories(home);
    setenv("XDG_CACHE_HOME", home.string().c_str(), 1);
    return home;
}

/// A file to derive from, inside `dir`.
std::filesystem::path make_source(const std::filesystem::path& dir, const std::string& name) {
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / name;
    std::ofstream(path) << "source\n";
    return path;
}

/// Puts `bytes` of payload in an entry, so the cap has something to measure.
void fill(const std::filesystem::path& entry, std::size_t bytes) {
    std::ofstream out(entry / "payload.bin", std::ios::binary | std::ios::trunc);
    out << std::string(bytes, 'x');
}

} // namespace

namespace toy::io::testing {

COOPA_TEST(two_sources_never_share_an_entry) {
    const std::filesystem::path home = isolate("derived_two_sources");
    require(derived_cache_enabled(), "the cache is on by default");

    // Same FILENAME, different directories -- the case a key built from the name alone gets
    // wrong, and the one that matters: every scene in a project is called scene.yaml.
    const auto a = make_source(home / "a", "scene.yaml");
    const auto b = make_source(home / "b", "scene.yaml");

    const std::filesystem::path da = derived_cache_dir("kind", a);
    const std::filesystem::path db = derived_cache_dir("kind", b);
    require(!da.empty() && !db.empty(), "both sources got an entry");
    expect(da != db, "two sources with the same filename get different entries");
    expect(std::filesystem::is_directory(da), "the entry is a directory that exists");

    // And the same source twice is the same entry, which is the whole point: a second run has to
    // find what the first one wrote.
    expect(derived_cache_dir("kind", a) == da, "the same source resolves to the same entry");
    // A different kind is a different entry even for one source.
    expect(derived_cache_dir("other", a) != da, "a kind scopes the entry");
}

COOPA_TEST(an_entry_whose_source_is_gone_is_pruned) {
    const std::filesystem::path home = isolate("derived_orphan");
    const auto kept = make_source(home / "kept", "scene.yaml");
    const auto doomed = make_source(home / "doomed", "scene.yaml");

    const std::filesystem::path kept_dir = derived_cache_dir("kind", kept);
    const std::filesystem::path doomed_dir = derived_cache_dir("kind", doomed);
    fill(kept_dir, 1024);
    fill(doomed_dir, 1024);

    // The scene is deleted, as the editor would delete it; nothing tells the cache.
    std::filesystem::remove(doomed);

    const std::uint64_t freed = prune_derived_cache();
    expect(freed >= 1024, "the orphan's bytes were freed (" + std::to_string(freed) + ")");
    expect(!std::filesystem::exists(doomed_dir), "the orphaned entry is gone");
    expect(std::filesystem::exists(kept_dir), "and the live one is untouched");
}

COOPA_TEST(a_cap_evicts_the_oldest_entries_first) {
    const std::filesystem::path home = isolate("derived_cap");
    const auto older = make_source(home / "older", "scene.yaml");
    const auto newer = make_source(home / "newer", "scene.yaml");

    const std::filesystem::path older_dir = derived_cache_dir("kind", older);
    fill(older_dir, 4096);
    // Age is the entry's newest file, so the second entry has to be stamped later than the
    // first. Written explicitly rather than by sleeping: a test that waits on a clock is a test
    // that fails on a busy machine.
    const std::filesystem::path newer_dir = derived_cache_dir("kind", newer);
    fill(newer_dir, 4096);
    const auto now = std::filesystem::file_time_type::clock::now();
    for (const auto& f : std::filesystem::recursive_directory_iterator(older_dir)) {
        std::filesystem::last_write_time(f.path(), now - std::chrono::hours(48));
    }
    for (const auto& f : std::filesystem::recursive_directory_iterator(newer_dir)) {
        std::filesystem::last_write_time(f.path(), now);
    }

    // Room for one of the two.
    prune_derived_cache(6000);
    expect(!std::filesystem::exists(older_dir), "the older entry was evicted");
    expect(std::filesystem::exists(newer_dir), "the newer one survived the cap");

    // And a cap of zero empties it, live sources and all -- which is what "clear the cache"
    // has to mean.
    prune_derived_cache(0);
    expect(!std::filesystem::exists(newer_dir), "a zero cap empties the cache");
}

COOPA_TEST(a_source_with_no_file_gets_nothing) {
    isolate("derived_no_source");
    expect(derived_cache_dir("kind", "").empty(), "no source, no entry");
    expect(derived_cache_dir("kind", "/nowhere/at/all/scene.yaml").empty(),
           "a source that does not exist has no identity to key on");
    expect(derived_cache_dir("", "/etc/hostname").empty(), "a kind is required");
}

} // namespace toy::io::testing
