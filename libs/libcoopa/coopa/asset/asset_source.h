/**
 * @file asset_source.h
 * @brief Resolves virtual asset paths against a list of search roots and reads their bytes.
 */

#ifndef COOPA_ASSET_ASSET_SOURCE_H
#define COOPA_ASSET_ASSET_SOURCE_H

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <coopa/asset/asset_index.h>
#include <coopa/yaml/document.h>

namespace coopa {
namespace asset {

/**
 * @class AssetSource
 * @brief Resolves virtual asset paths against a list of search roots and reads their bytes.
 *
 * Resolution order for a virtual path:
 *   1. Absolute path — used as-is if it exists.
 *   2. `base_dir` (if given) — e.g. the loading scene's own directory, matching
 *      coopa::scene::SceneLoader::ParseContext::scene_dir.
 *   3. Each registered search root, in registration order — first
 *      `root + "/" + path` that exists wins.
 *   4. By NAME (AssetIndex): the same type folder and file name anywhere under each root, in
 *      root order -- `materials/brick.yaml` finds `materials/metal/brick.yaml`, so an asset's
 *      tag folders can change without breaking what refers to it.
 *   5. Falls through to the path unchanged (resolved relative to the
 *      process's CWD by whatever opens it).
 *
 * Resolution is deliberately self-contained: a virtual path is resolved
 * against this source's own base directory, with no dependency on process-wide
 * roots or environment variables. That is what lets two scenes in different
 * directories refer to `mesh.obj` and get different assets.
 */
class AssetSource {
public:
    /**
     * @brief Registers a directory to search, in the order roots should be tried.
     * @param dir Absolute or CWD-relative directory path.
     */
    void add_search_root(const std::string& dir) {
        search_roots_.push_back(dir);
    }

    /** @brief Removes every registered search root. */
    void clear_search_roots() { search_roots_.clear(); }

    /** @brief The registered search roots, in try-order. */
    const std::vector<std::string>& search_roots() const { return search_roots_; }

    /**
     * @brief Resolves a virtual path to a concrete filesystem path.
     * @param virtual_path Path as referenced from YAML/scene files.
     * @param base_dir     Optional extra root tried before the registered search roots.
     * @return The resolved path, or virtual_path unchanged if nothing matched.
     */
    std::string resolve(const std::string& virtual_path, const std::string& base_dir = "") const {
        auto existing = [](const std::filesystem::path& candidate) -> std::optional<std::string> {
            std::filesystem::path found = coopa::yaml::resolve_variant(candidate);
            if (std::filesystem::exists(found)) return found.string();
            return std::nullopt;
        };
        std::filesystem::path p(virtual_path);
        if (p.is_absolute()) {
            if (auto found = existing(p)) return *found;
        }
        if (!base_dir.empty()) {
            if (auto found = existing(std::filesystem::path(base_dir) / virtual_path)) return *found;
        }
        for (const auto& root : search_roots_) {
            if (auto found = existing(std::filesystem::path(root) / virtual_path)) return *found;
        }
        if (!p.is_absolute()) {
            if (auto found = AssetIndex::find_in(search_roots_, virtual_path)) return found->string();
        }
        return virtual_path;
    }

    /** @brief True if resolve() would find an existing file for this path. */
    bool exists(const std::string& virtual_path, const std::string& base_dir = "") const {
        return std::filesystem::exists(resolve(virtual_path, base_dir));
    }

    /**
     * @brief Reads a resolved (already-concrete) file path fully into memory.
     * @param resolved_path Concrete filesystem path, e.g. from resolve().
     * @return The file's raw bytes.
     * @throws std::runtime_error if the file cannot be opened or read.
     */
    static std::vector<std::byte> read_bytes(const std::string& resolved_path) {
        std::ifstream ifs(resolved_path, std::ios::binary | std::ios::ate);
        if (!ifs) {
            throw std::runtime_error("[AssetSource] Failed to open file: " + resolved_path);
        }
        std::streamsize size = ifs.tellg();
        ifs.seekg(0, std::ios::beg);
        std::vector<std::byte> buffer(static_cast<size_t>(size));
        if (size > 0 && !ifs.read(reinterpret_cast<char*>(buffer.data()), size)) {
            throw std::runtime_error("[AssetSource] Failed to read file: " + resolved_path);
        }
        return buffer;
    }

    /**
     * @brief Returns a file's last-write time as an opaque, monotonically-ordered tick count.
     *
     * Only meaningful for ordering comparisons between two calls on the same
     * path (which is all AssetManager's hot-reload polling needs) — not a
     * real wall-clock timestamp. std::filesystem::file_time_type's epoch is
     * implementation-defined and on at least one libstdc++ build in this
     * workspace does not align with system_clock's, so the returned value
     * can be negative even for a freshly-written file; it still increases
     * with each subsequent write to that file.
     *
     * @param resolved_path Concrete filesystem path.
     * @return Ordered tick count, or 0 if the file cannot be queried.
     */
    static long long last_write_time_ns(const std::string& resolved_path) {
        std::error_code ec;
        auto t = std::filesystem::last_write_time(resolved_path, ec);
        if (ec) return 0;
        return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    }

private:
    std::vector<std::string> search_roots_;
};

} // namespace asset
} // namespace coopa

#endif // COOPA_ASSET_ASSET_SOURCE_H
