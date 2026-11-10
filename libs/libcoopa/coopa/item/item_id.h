/**
 * @file item_id.h
 * @brief Normalized string identity for an item definition, with a precomputed hash.
 */

#ifndef COOPA_ITEM_ITEM_ID_H
#define COOPA_ITEM_ITEM_ID_H

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace coopa {
namespace item {

/**
 * @class ItemId
 * @brief Identity of an item definition, derived from its short authoring name.
 *
 * Mirrors coopa::asset::AssetId (coopa/asset/asset_id.h): a normalized string
 * plus a precomputed FNV-1a 64-bit hash, so ItemId is a cheap, hashable,
 * copyable key for both a std::unordered_map<ItemId, ...> and equality
 * comparisons, while the string survives for logging and error messages.
 *
 * Unlike AssetId, an ItemId is not a virtual path -- it's an author-chosen
 * short name ("potion_health", "sword_iron"), so normalization is
 * lowercase + leading/trailing-whitespace trim rather than separator
 * canonicalization. Two ids differing only by case or incidental whitespace
 * (e.g. copy-pasted from a YAML list) resolve to the same identity.
 */
class ItemId {
public:
    /** @brief Constructs an empty, invalid ItemId. */
    ItemId() = default;

    /**
     * @brief Builds an ItemId from an authoring name, normalizing case and whitespace.
     * @param name Short item identifier, e.g. "potion_health".
     * @return The normalized ItemId.
     */
    static ItemId from_name(const std::string& name) {
        return ItemId(normalize_(name));
    }

    /** @brief 64-bit FNV-1a hash of the normalized name; the key maps store this id under. */
    uint64_t hash() const { return hash_; }

    /** @brief The normalized name this id was built from. */
    const std::string& str() const { return name_; }

    /** @brief True if this id was built from a non-empty name. */
    bool is_valid() const { return !name_.empty(); }

    bool operator==(const ItemId& other) const { return hash_ == other.hash_ && name_ == other.name_; }
    bool operator!=(const ItemId& other) const { return !(*this == other); }

private:
    explicit ItemId(std::string normalized_name)
        : name_(std::move(normalized_name)), hash_(fnv1a_(name_)) {}

    /** @brief Lowercases and trims leading/trailing whitespace. */
    static std::string normalize_(const std::string& raw) {
        size_t begin = 0;
        size_t end = raw.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin]))) ++begin;
        while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1]))) --end;

        std::string out = raw.substr(begin, end - begin);
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    /** @brief FNV-1a 64-bit hash over the normalized name's bytes. */
    static uint64_t fnv1a_(const std::string& s) {
        uint64_t h = 1469598103934665603ull;  // FNV offset basis
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ull;  // FNV prime
        }
        return h;
    }

    std::string name_;
    uint64_t    hash_ = 0;
};

} // namespace item
} // namespace coopa

namespace std {

/// @brief Allows ItemId to be used directly as an unordered_map/unordered_set key.
template<>
struct hash<coopa::item::ItemId> {
    size_t operator()(const coopa::item::ItemId& id) const noexcept {
        return static_cast<size_t>(id.hash());
    }
};

} // namespace std

#endif // COOPA_ITEM_ITEM_ID_H
