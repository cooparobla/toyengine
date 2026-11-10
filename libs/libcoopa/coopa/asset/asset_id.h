/**
 * @file asset_id.h
 * @brief Normalized virtual-path identity for an asset, with a precomputed hash.
 */

#ifndef COOPA_ASSET_ASSET_ID_H
#define COOPA_ASSET_ASSET_ID_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace coopa {
namespace asset {

/**
 * @class AssetId
 * @brief Identity of an asset, derived from its virtual (logical) path.
 *
 * Two paths that differ only by backslash-vs-forward-slash separators or a
 * leading "./" normalize to the same AssetId. The hash (FNV-1a, 64-bit) is
 * what AssetManager actually keys its slot map on; the normalized path is
 * retained alongside it for logging, error messages, and hot-reload mtime
 * lookups. This mirrors how every existing loader in the workspace already
 * identifies an asset (by its scene-relative or root-relative path) — no
 * asset data needs a GUID or sidecar file to adopt this system.
 */
class AssetId {
public:
    /** @brief Constructs an empty, invalid AssetId. */
    AssetId() = default;

    /**
     * @brief Builds an AssetId from a virtual path, normalizing separators.
     * @param virtual_path Path as referenced from YAML/scene files, e.g. "textures/brick.png".
     * @return The normalized AssetId.
     */
    static AssetId from_path(const std::string& virtual_path) {
        return AssetId(normalize_(virtual_path));
    }

    /** @brief 64-bit FNV-1a hash of the normalized path; the key AssetManager stores slots under. */
    uint64_t hash() const { return hash_; }

    /** @brief The normalized virtual path this id was built from. */
    const std::string& path() const { return path_; }

    /** @brief True if this id was built from a non-empty path. */
    bool is_valid() const { return !path_.empty(); }

    bool operator==(const AssetId& other) const { return hash_ == other.hash_ && path_ == other.path_; }
    bool operator!=(const AssetId& other) const { return !(*this == other); }

private:
    explicit AssetId(std::string normalized_path)
        : path_(std::move(normalized_path)), hash_(fnv1a_(path_)) {}

    /** @brief Converts backslashes to '/' and strips a leading "./". */
    static std::string normalize_(const std::string& raw) {
        std::string out = raw;
        std::replace(out.begin(), out.end(), '\\', '/');
        while (out.size() >= 2 && out[0] == '.' && out[1] == '/') {
            out.erase(0, 2);
        }
        return out;
    }

    /** @brief FNV-1a 64-bit hash over the normalized path's bytes. */
    static uint64_t fnv1a_(const std::string& s) {
        uint64_t h = 1469598103934665603ull;  // FNV offset basis
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ull;  // FNV prime
        }
        return h;
    }

    std::string path_;
    uint64_t    hash_ = 0;
};

} // namespace asset
} // namespace coopa

namespace std {

/// @brief Allows AssetId to be used directly as an unordered_map/unordered_set key.
template<>
struct hash<coopa::asset::AssetId> {
    size_t operator()(const coopa::asset::AssetId& id) const noexcept {
        return static_cast<size_t>(id.hash());
    }
};

} // namespace std

#endif // COOPA_ASSET_ASSET_ID_H
