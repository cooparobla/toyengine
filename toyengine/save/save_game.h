/**
 * @file save_game.h
 * @brief SaveGame / SaveNode: the in-memory save document the game reads and writes.
 *
 * A save is one YAML document. The engine owns its frame; everything under `global` and
 * `objects` is whatever the game puts there:
 *
 * @code
 * version: 3                 # the game's save format version (SaveSystem::set_version())
 * meta:                      # written by SaveSystem: slot, scene, time, play_time, ...
 *   summary: { ... }         # game-defined, also copied to the .meta sidecar for slot menus
 * global: { ... }            # game-defined (SaveSystem::on_save / on_load)
 * objects:                   # one section per SaveId, one sub-section per ISaveable component
 *   player: { character: { position: { x: 1, y: 2, z: 0 }, yaw: 90 } }
 * @endcode
 *
 * SaveNode is a cheap handle onto one mapping in it, with typed get / set:
 * @code
 * SaveNode g = game.global();
 * g.set("gold", 120);
 * g.section("quests").set("done", std::vector<std::string>{"intro", "bridge"});
 * int gold = g.get("gold", 0);                       // the fallback when missing or mistyped
 * @endcode
 * Supported value types: bool, integers, float / double, std::string, glm::vec2/3/4, glm::quat,
 * std::vector of any of those, and raw fkyaml::node (get_node / set_node) for anything else.
 */

#ifndef TOYENGINE_SAVE_SAVE_GAME_H
#define TOYENGINE_SAVE_SAVE_GAME_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <coopa/yaml/document.h>

namespace toy {
namespace save {

namespace detail {

template<typename T> struct is_vector : std::false_type {};
template<typename T, typename A> struct is_vector<std::vector<T, A>> : std::true_type {};

fkyaml::node vec_node(const float* v, int n);

bool read_float(const fkyaml::node& n, float& out);

bool read_vec(const fkyaml::node& n, float* v, int n_comp);

/** @brief A value as a YAML node. */
template<typename T>
fkyaml::node to_node(const T& v) {
    if constexpr (std::is_same_v<T, fkyaml::node>) {
        return v;
    } else if constexpr (std::is_same_v<T, bool>) {
        return fkyaml::node(v);
    } else if constexpr (std::is_integral_v<T>) {
        return fkyaml::node(static_cast<int64_t>(v));
    } else if constexpr (std::is_floating_point_v<T>) {
        return fkyaml::node(static_cast<double>(v));
    } else if constexpr (std::is_convertible_v<T, std::string>) {
        return fkyaml::node(std::string(v));
    } else if constexpr (std::is_same_v<T, glm::vec2>) {
        return vec_node(&v.x, 2);
    } else if constexpr (std::is_same_v<T, glm::vec3>) {
        return vec_node(&v.x, 3);
    } else if constexpr (std::is_same_v<T, glm::vec4>) {
        return vec_node(&v.x, 4);
    } else if constexpr (std::is_same_v<T, glm::quat>) {
        const float xyzw[4] = {v.x, v.y, v.z, v.w};
        return vec_node(xyzw, 4);
    } else if constexpr (is_vector<T>::value) {
        fkyaml::node out = fkyaml::node::sequence();
        for (const auto& e : v) out.as_seq().push_back(to_node(e));
        return out;
    } else {
        static_assert(sizeof(T) == 0, "SaveNode: unsupported value type (use set_node for custom data)");
    }
}

/** @brief Reads `n` into `out`; false (out untouched) when the node holds another type. */
template<typename T>
bool from_node(const fkyaml::node& n, T& out) {
    if constexpr (std::is_same_v<T, fkyaml::node>) {
        out = n;
        return true;
    } else if constexpr (std::is_same_v<T, bool>) {
        if (!n.is_boolean()) return false;
        out = n.get_value<bool>();
        return true;
    } else if constexpr (std::is_integral_v<T>) {
        if (n.is_integer()) { out = static_cast<T>(n.get_value<int64_t>()); return true; }
        if (n.is_float_number()) { out = static_cast<T>(n.get_value<double>()); return true; }
        return false;
    } else if constexpr (std::is_floating_point_v<T>) {
        if (n.is_float_number()) { out = static_cast<T>(n.get_value<double>()); return true; }
        if (n.is_integer()) { out = static_cast<T>(n.get_value<int64_t>()); return true; }
        return false;
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (!n.is_string()) return false;
        out = n.get_value<std::string>();
        return true;
    } else if constexpr (std::is_same_v<T, glm::vec2>) {
        glm::vec2 v; if (!read_vec(n, &v.x, 2)) return false; out = v; return true;
    } else if constexpr (std::is_same_v<T, glm::vec3>) {
        glm::vec3 v; if (!read_vec(n, &v.x, 3)) return false; out = v; return true;
    } else if constexpr (std::is_same_v<T, glm::vec4>) {
        glm::vec4 v; if (!read_vec(n, &v.x, 4)) return false; out = v; return true;
    } else if constexpr (std::is_same_v<T, glm::quat>) {
        float xyzw[4];
        if (!read_vec(n, xyzw, 4)) return false;
        out = glm::quat(xyzw[3], xyzw[0], xyzw[1], xyzw[2]);
        return true;
    } else if constexpr (is_vector<T>::value) {
        if (!n.is_sequence()) return false;
        T result;
        for (const auto& e : n.as_seq()) {
            typename T::value_type v{};
            if (!from_node(e, v)) return false;
            result.push_back(std::move(v));
        }
        out = std::move(result);
        return true;
    } else {
        static_assert(sizeof(T) == 0, "SaveNode: unsupported value type (use get_node for custom data)");
    }
}

}  // namespace detail

/**
 * @class SaveNode
 * @brief A handle onto one mapping of a SaveGame: typed get / set and nested sections.
 *
 * Copies share the same underlying node, which the owning SaveGame keeps alive. A handle
 * obtained from a const SaveGame (or section() on a const SaveNode) is read-only: set() on it
 * throws. A section that does not exist reads as empty.
 */
class SaveNode {
public:
    SaveNode() = default;
    SaveNode(fkyaml::node* node, bool writable) : node_(node), writable_(writable && node) {}

    /** @brief True if this section exists (false for a missing section read through const). */
    bool valid() const { return node_ && node_->is_mapping(); }
    bool writable() const { return writable_; }
    bool empty() const { return !valid() || node_->size() == 0; }

    bool has(const std::string& key) const { return valid() && node_->contains(key); }

    /** @brief The value at `key`, or `fallback` if it is missing or holds another type. */
    template<typename T>
    T get(const std::string& key, T fallback) const {
        if (!has(key)) return fallback;
        T out = fallback;
        try {
            if (!detail::from_node(node_->at(key), out)) return fallback;
        } catch (const std::exception&) {
            return fallback;
        }
        return out;
    }
    std::string get(const std::string& key, const char* fallback) const { return get<std::string>(key, fallback); }

    /** @brief Writes `value` at `key` (replacing whatever was there). */
    template<typename T>
    void set(const std::string& key, const T& value) {
        mutable_node_()[key] = detail::to_node(value);
    }
    void set(const std::string& key, const char* value) { set<std::string>(key, value); }

    /** @brief The raw node at `key` (a null node when missing). */
    fkyaml::node get_node(const std::string& key) const { return has(key) ? node_->at(key) : fkyaml::node(); }
    void set_node(const std::string& key, const fkyaml::node& value) { mutable_node_()[key] = value; }

    void erase(const std::string& key) {
        if (has(key)) mutable_node_().as_map().erase(fkyaml::node(key));
    }

    /** @brief The child mapping `key`, created (replacing a non-mapping value) if needed;
     *         on a read-only handle, the read-only lookup below. */
    SaveNode section(const std::string& key);
    /** @brief The child mapping `key`, read-only; invalid (reads as empty) if missing. */
    SaveNode section(const std::string& key) const;

    /** @brief The keys of this mapping, in document order. */
    std::vector<std::string> keys() const;

    /** @brief The underlying mapping (null node for an invalid handle). */
    const fkyaml::node& node() const;

private:
    fkyaml::node* node_ = nullptr;
    bool writable_ = false;

    fkyaml::node& mutable_node_();
};

/**
 * @class SaveGame
 * @brief One save document: `version`, `meta`, `global` and `objects/<save_id>/<key>`.
 */
class SaveGame {
public:
    SaveGame() { normalize_(); }
    explicit SaveGame(fkyaml::node root) : root_(std::move(root)) { normalize_(); }

    /** @brief The save format version the document was written with (0 when it has none). */
    int version() const;
    void set_version(int v) { root_["version"] = fkyaml::node(static_cast<int64_t>(v)); }

    /** @brief Engine-written facts (slot, scene, time, play_time); read-only to most games. */
    SaveNode meta() { return SaveNode(&root_["meta"], true); }
    SaveNode meta() const { return SaveNode(&root_["meta"], false); }

    /** @brief Game-defined data shown in a slot menu (meta.summary; copied to the .meta sidecar). */
    SaveNode summary() { return meta().section("summary"); }
    SaveNode summary() const { return meta().section("summary"); }

    /** @brief Game-defined global state. */
    SaveNode global() { return SaveNode(&root_["global"], true); }
    SaveNode global() const { return SaveNode(&root_["global"], false); }

    /** @brief The section for one SaveId (created on write access). */
    SaveNode object(const std::string& save_id) { return objects_().section(save_id); }
    SaveNode object(const std::string& save_id) const { return objects_().section(save_id); }
    bool has_object(const std::string& save_id) const { return objects_().has(save_id); }
    std::vector<std::string> object_ids() const { return objects_().keys(); }
    void erase_object(const std::string& save_id) { objects_().erase(save_id); }

    /** @brief The whole document (what is written to disk). */
    const fkyaml::node& root() const { return root_; }
    fkyaml::node& root() { return root_; }

private:
    // Mutable so const accessors can hand out read-only handles without copying; the
    // sections always exist (normalize_), so a const access never inserts anything.
    mutable fkyaml::node root_ = fkyaml::node::mapping();

    SaveNode objects_() { return SaveNode(&root_["objects"], true); }
    SaveNode objects_() const { return SaveNode(&root_["objects"], false); }

    void normalize_();
};

}  // namespace save
}  // namespace toy

#endif  // TOYENGINE_SAVE_SAVE_GAME_H
