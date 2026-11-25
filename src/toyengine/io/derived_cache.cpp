#include <toyengine/io/derived_cache.h>

#include <toyengine/io/runtime_paths.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <system_error>
#include <vector>

namespace toy {
namespace io {
namespace {

/// `name` with everything but letters, digits, `_`, `-` and `.` replaced, so a path component
/// built from one is always a legal single directory name.
std::string sanitise(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        out.push_back(plain ? c : '_');
    }
    if (out.empty()) out = "_";
    return out;
}

/// A directory name for `source`: its own stem, plus a hash of the whole absolute path.
///
/// The stem alone would collide between two projects' `scene.yaml`, and the hash alone would be
/// unreadable in a directory someone is trying to clear by hand -- so both.
std::string entry_key(const std::filesystem::path& source) {
    const std::size_t h = std::hash<std::string>{}(source.string());
    char hex[17] = {};
    std::snprintf(hex, sizeof(hex), "%016zx", h);
    std::string stem = sanitise(source.parent_path().filename().string());
    if (stem.empty() || stem == "_") stem = sanitise(source.stem().string());
    return stem + "-" + hex;
}

/// The file naming an entry's source, read by prune_derived_cache().
constexpr const char* k_source_file = "source.path";

} // namespace

bool derived_cache_enabled() {
    static const bool on = [] {
        const char* v = std::getenv("TOY_DERIVED_CACHE");
        return !(v != nullptr && std::string(v) == "0");
    }();
    return on;
}

std::filesystem::path derived_cache_root() { return user_cache_dir() / "derived"; }

std::filesystem::path derived_cache_dir(const std::string& kind,
                                        const std::filesystem::path& source) {
    if (!derived_cache_enabled() || kind.empty() || source.empty()) return {};
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(source, ec);
    if (ec) return {};
    if (!std::filesystem::exists(absolute, ec)) return {};

    const std::filesystem::path dir = derived_cache_root() / sanitise(kind) / entry_key(absolute);
    std::filesystem::create_directories(dir, ec);
    if (ec) return {};
    // Rewritten every time rather than only on creation: it is one short line, and it is also
    // how an entry's mtime says it is still in use (see prune_derived_cache()'s age rule).
    std::ofstream out(dir / k_source_file, std::ios::trunc);
    if (!out) return {};
    out << absolute.string() << "\n";
    return dir;
}

std::uint64_t prune_derived_cache(std::uint64_t cap) {
    if (!derived_cache_enabled()) return 0;
    std::error_code ec;
    const std::filesystem::path root = derived_cache_root();
    if (!std::filesystem::is_directory(root, ec)) return 0;

    struct Entry {
        std::filesystem::path dir;
        std::uint64_t bytes = 0;
        std::filesystem::file_time_type newest{};
        bool orphan = true;
    };
    std::vector<Entry> entries;

    // <root>/<kind>/<entry>/..., two levels: a kind directory holds entries and nothing else.
    for (const auto& kind : std::filesystem::directory_iterator(root, ec)) {
        if (ec || !kind.is_directory()) continue;
        for (const auto& it : std::filesystem::directory_iterator(kind.path(), ec)) {
            if (ec || !it.is_directory()) continue;
            Entry e;
            e.dir = it.path();
            for (const auto& f : std::filesystem::recursive_directory_iterator(e.dir, ec)) {
                if (ec || !f.is_regular_file()) continue;
                e.bytes += static_cast<std::uint64_t>(f.file_size(ec));
                const auto when = f.last_write_time(ec);
                if (!ec && when > e.newest) e.newest = when;
            }
            std::ifstream in(e.dir / k_source_file);
            std::string source;
            if (in && std::getline(in, source) && !source.empty()) {
                e.orphan = !std::filesystem::exists(std::filesystem::path(source), ec);
            }
            entries.push_back(std::move(e));
        }
    }

    std::uint64_t freed = 0;
    const auto drop = [&freed, &ec](Entry& e) {
        std::filesystem::remove_all(e.dir, ec);
        if (!ec) freed += e.bytes;
        e.bytes = 0;
    };

    // Orphans first, whatever the cap: an entry whose source is gone is never coming back, so
    // keeping it only means evicting something still in use in its place.
    std::uint64_t held = 0;
    for (Entry& e : entries) {
        if (e.orphan) drop(e);
        else held += e.bytes;
    }
    if (held <= cap) return freed;

    // Then oldest-first until it fits. Age is the entry's NEWEST file, so an entry something is
    // still writing into or reading from sorts last.
    std::vector<Entry*> live;
    for (Entry& e : entries) {
        if (!e.orphan) live.push_back(&e);
    }
    std::sort(live.begin(), live.end(),
              [](const Entry* a, const Entry* b) { return a->newest < b->newest; });
    for (Entry* e : live) {
        if (held <= cap) break;
        held -= e->bytes;
        drop(*e);
    }
    return freed;
}

} // namespace io
} // namespace toy
