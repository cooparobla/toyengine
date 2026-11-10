/**
 * @file asset_index.h
 * @brief Finds an asset by TYPE and NAME when the exact path a reference names doesn't exist.
 */

#ifndef COOPA_ASSET_ASSET_INDEX_H
#define COOPA_ASSET_ASSET_INDEX_H

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <coopa/yaml/document.h>

namespace coopa {
namespace asset {

/**
 * @class AssetIndex
 * @brief Name-based fallback for asset references: an asset's folder path below its type
 *        folder is organisation (tags), not identity.
 *
 * An asset root is laid out as `<type>/<tag>/<tag>/.../<file>` -- `materials/metal/brick.yaml`
 * is the material `brick` tagged `metal`. A reference names the type folder and the file
 * (`materials/brick`, `meshes/rock.yaml`, `objects/crate`) and keeps working wherever the file
 * is moved inside its type folder, so re-tagging an asset never touches what refers to it.
 *
 * find() runs only after every resolver's exact lookups miss (the referencing file's folder,
 * then the search roots), so a direct path always wins and a project's `brick` still shadows
 * the engine's -- each root is searched in priority order.
 *
 * Matching `<type>/<s1>/.../<sk>/<file>` against the files under `root/<type>/`:
 *   - same file name (`.caml` counts as its `.yaml` twin -- packaged builds encode documents),
 *   - the reference's middle segments appear, in order, among the candidate's folders -- so
 *     `scenes/fog_test/scene.yaml` finds `scenes/tests/fog_test/scene.yaml`, and a partly-
 *     tagged reference (`meshes/terrain/rock.yaml`) still narrows the search,
 *   - several matches: the fewest extra folders wins, then the alphabetically first path.
 *     Names are meant to be unique per type (the editor enforces it), so this is a tie-break.
 *
 * Each root is scanned once and cached. A miss, or a hit whose file has since moved, rescans
 * that root (at most once a second, so a genuinely missing file doesn't rescan every frame);
 * invalidate() drops every cache at once (the editor calls it after moving files). Thread-safe.
 */
class AssetIndex {
public:
    /**
     * @brief The file `virtual_path` names under `root`, found by type and name.
     * @param root         An asset root (e.g. `<project>/assets`).
     * @param virtual_path A root-relative reference with at least a type folder and a file
     *                     (`materials/brick.yaml`); anything shorter, or absolute, never matches.
     * @return The existing file's path, or nullopt.
     */
    static std::optional<std::filesystem::path> find(const std::filesystem::path& root, const std::string& virtual_path) {
        const std::vector<std::string> seg = split_(virtual_path);
        if (seg.size() < 2 || std::filesystem::path(virtual_path).is_absolute()) return std::nullopt;
        for (const auto& s : seg) if (s == "..") return std::nullopt;
        const std::string key = seg.front() + "|" + normalize_name_(seg.back());
        const std::vector<std::string> wanted(seg.begin() + 1, seg.end() - 1);

        Instance& in = instance_();
        std::lock_guard<std::mutex> lock(in.mutex);
        const std::string root_key = root.lexically_normal().generic_string();
        for (int attempt = 0; attempt < 2; ++attempt) {
            Root& r = in.roots[root_key];
            const auto now = std::chrono::steady_clock::now();
            if (!r.built) build_(r, root);
            const auto it = r.files.find(key);
            if (it != r.files.end()) {
                if (auto rel = best_(it->second, wanted)) {
                    std::error_code ec;
                    const std::filesystem::path p = coopa::yaml::resolve_variant(root / *rel);
                    if (std::filesystem::exists(p, ec)) return p;
                }
            }
            // A miss (or a stale hit): rescan once, unless this root was scanned just now.
            if (attempt == 0 && now - r.built_at >= std::chrono::seconds(1)) { r.built = false; continue; }
            break;
        }
        return std::nullopt;
    }

    /** @brief find() over several roots in priority order (the first root with a match wins). */
    static std::optional<std::filesystem::path> find_in(const std::vector<std::string>& roots, const std::string& virtual_path) {
        for (const auto& root : roots) {
            if (auto p = find(root, virtual_path)) return p;
        }
        return std::nullopt;
    }

    /** @brief Forgets every scanned root; the next find() rescans. Call after moving asset files. */
    static void invalidate() {
        Instance& in = instance_();
        std::lock_guard<std::mutex> lock(in.mutex);
        in.roots.clear();
    }

    /**
     * @brief The type folder and file name an asset reference identifies: `<type>|<file>`, the
     *        file name with `.caml` / `.yml` spelled `.yaml`. Two references with the same key
     *        name the same asset (as far as name lookup goes). Empty for a path too short to
     *        have both.
     */
    static std::string identity(const std::string& virtual_path) {
        const std::vector<std::string> seg = split_(virtual_path);
        if (seg.size() < 2) return "";
        return seg.front() + "|" + normalize_name_(seg.back());
    }

private:
    struct Root {
        bool built = false;
        std::chrono::steady_clock::time_point built_at{};
        std::unordered_map<std::string, std::vector<std::string>> files;   ///< identity -> root-relative paths
    };
    struct Instance {
        std::mutex mutex;
        std::map<std::string, Root> roots;
    };
    static Instance& instance_() {
        static Instance in;
        return in;
    }

    static std::vector<std::string> split_(const std::string& path) {
        std::vector<std::string> out;
        for (const auto& part : std::filesystem::path(path).lexically_normal()) {
            const std::string s = part.generic_string();
            if (!s.empty() && s != "." && s != "/") out.push_back(s);
        }
        return out;
    }

    static std::string normalize_name_(const std::string& file) {
        const std::filesystem::path p(file);
        if (!coopa::yaml::is_document_ext(p)) return file;
        return p.stem().string() + ".yaml";
    }

    static void build_(Root& r, const std::filesystem::path& root) {
        r.files.clear();
        r.built = true;
        r.built_at = std::chrono::steady_clock::now();
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) return;
        for (auto e = std::filesystem::recursive_directory_iterator(root, ec); !ec && e != std::filesystem::recursive_directory_iterator();
             e.increment(ec)) {
            const std::string name = e->path().filename().string();
            if (!name.empty() && name[0] == '.') {
                if (e->is_directory(ec)) e.disable_recursion_pending();
                continue;
            }
            if (!e->is_regular_file(ec)) continue;
            const std::string rel = std::filesystem::relative(e->path(), root, ec).generic_string();
            if (ec) { ec.clear(); continue; }
            const std::string id = identity(rel);
            if (id.empty()) continue;
            // Stored spelled as a .yaml (resolve_variant() finds the .caml at lookup).
            std::string stored = rel;
            if (coopa::yaml::is_document_ext(e->path())) {
                stored = (std::filesystem::path(rel).parent_path() / normalize_name_(name)).generic_string();
            }
            auto& list = r.files[id];
            if (std::find(list.begin(), list.end(), stored) == list.end()) list.push_back(stored);
        }
    }

    /** @brief The candidate whose folders contain `wanted` in order, fewest extra folders first. */
    static std::optional<std::string> best_(const std::vector<std::string>& candidates, const std::vector<std::string>& wanted) {
        std::optional<std::string> best;
        size_t best_extra = 0;
        for (const auto& c : candidates) {
            const std::vector<std::string> seg = split_(c);
            if (seg.size() < 2) continue;
            size_t w = 0;
            for (size_t i = 1; i + 1 < seg.size() && w < wanted.size(); ++i) {
                if (seg[i] == wanted[w]) ++w;
            }
            if (w != wanted.size()) continue;
            const size_t extra = seg.size() - 2 - wanted.size();
            if (!best || extra < best_extra || (extra == best_extra && c < *best)) {
                best = c;
                best_extra = extra;
            }
        }
        return best;
    }
};

} // namespace asset
} // namespace coopa

#endif // COOPA_ASSET_ASSET_INDEX_H
