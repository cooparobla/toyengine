/**
 * @file yaml_util.h
 * @brief Small fkYAML helpers the editor uses to read and edit documents in place.
 *
 * Every editor document (scene, mesh, material, config) is a plain fkyaml::node -- the same
 * value the engine's loaders parse -- so these helpers are the editor's whole data-access
 * layer: typed reads with defaults, vector/colour conversion in the engine's own
 * `{ x, y, z }` / `{ r, g, b }` spellings, and key removal.
 */

#ifndef TOYEDITOR_CORE_YAML_UTIL_H
#define TOYEDITOR_CORE_YAML_UTIL_H

#include <fkYAML/node.hpp>
#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace toy::editor {

using Node = fkyaml::node;

/** @brief A float read that accepts YAML ints too (`x: 0` is an integer node). */
inline float as_float(const Node& n, float def = 0.0f) {
    if (n.is_float_number()) return static_cast<float>(n.get_value<double>());
    if (n.is_integer()) return static_cast<float>(n.get_value<int64_t>());
    if (n.is_boolean()) return n.get_value<bool>() ? 1.0f : 0.0f;
    return def;
}

inline float get_float(const Node& map, const std::string& key, float def = 0.0f) {
    if (!map.is_mapping() || !map.contains(key)) return def;
    return as_float(map.at(key), def);
}
inline int get_int(const Node& map, const std::string& key, int def = 0) {
    if (!map.is_mapping() || !map.contains(key)) return def;
    const Node& n = map.at(key);
    if (n.is_integer()) return static_cast<int>(n.get_value<int64_t>());
    if (n.is_float_number()) return static_cast<int>(n.get_value<double>());
    return def;
}
inline bool get_bool(const Node& map, const std::string& key, bool def = false) {
    if (!map.is_mapping() || !map.contains(key)) return def;
    const Node& n = map.at(key);
    if (n.is_boolean()) return n.get_value<bool>();
    if (n.is_integer()) return n.get_value<int64_t>() != 0;
    return def;
}
inline std::string get_string(const Node& map, const std::string& key, const std::string& def = "") {
    if (!map.is_mapping() || !map.contains(key)) return def;
    const Node& n = map.at(key);
    if (n.is_string()) return n.get_value<std::string>();
    if (n.is_integer()) return std::to_string(n.get_value<int64_t>());
    if (n.is_float_number()) return std::to_string(n.get_value<double>());
    if (n.is_boolean()) return n.get_value<bool>() ? "true" : "false";
    return def;
}

/** @brief `{ x, y, z }` (or a 3-element sequence) -> vec3. */
inline glm::vec3 as_vec3(const Node& n, glm::vec3 def = glm::vec3(0.0f)) {
    if (n.is_mapping()) {
        return {get_float(n, "x", def.x), get_float(n, "y", def.y), get_float(n, "z", def.z)};
    }
    if (n.is_sequence() && n.size() >= 3) {
        return {as_float(n.as_seq()[0]), as_float(n.as_seq()[1]), as_float(n.as_seq()[2])};
    }
    return def;
}
inline glm::vec3 get_vec3(const Node& map, const std::string& key, glm::vec3 def = glm::vec3(0.0f)) {
    if (!map.is_mapping() || !map.contains(key)) return def;
    return as_vec3(map.at(key), def);
}
inline glm::vec3 get_color(const Node& map, const std::string& key, glm::vec3 def = glm::vec3(1.0f)) {
    if (!map.is_mapping() || !map.contains(key)) return def;
    const Node& n = map.at(key);
    if (n.is_mapping()) return {get_float(n, "r", def.r), get_float(n, "g", def.g), get_float(n, "b", def.b)};
    if (n.is_sequence() && n.size() >= 3) return as_vec3(n, def);
    return def;
}

/** @brief Rounds away float noise (1e-7 drift from drags) so saved files stay readable. */
inline double tidy(double v) {
    const double r = std::round(v * 1e5) / 1e5;
    return std::abs(r - v) < 1e-6 ? r : v;
}

inline Node make_float(double v) { return Node(tidy(v)); }
inline Node make_vec3(const glm::vec3& v) {
    Node n = Node::mapping();
    n["x"] = make_float(v.x);
    n["y"] = make_float(v.y);
    n["z"] = make_float(v.z);
    return n;
}
inline Node make_color(const glm::vec3& c) {
    Node n = Node::mapping();
    n["r"] = make_float(c.r);
    n["g"] = make_float(c.g);
    n["b"] = make_float(c.b);
    return n;
}
inline Node make_float_seq(const float* v, int n) {
    Node s = Node::sequence();
    for (int i = 0; i < n; ++i) s.as_seq().push_back(make_float(v[i]));
    return s;
}

/** @brief Removes `key` from a mapping (no-op if absent or not a mapping). */
inline void erase_key(Node& map, const std::string& key) {
    if (!map.is_mapping()) return;
    map.as_map().erase(Node(key));
}

/** @brief The mapping's keys as strings, in fkYAML's (sorted) order. */
inline std::vector<std::string> keys_of(const Node& map) {
    std::vector<std::string> out;
    if (!map.is_mapping()) return out;
    for (const auto& kv : map.as_map()) {
        if (kv.first.is_string()) out.push_back(kv.first.get_value<std::string>());
    }
    return out;
}

/** @brief `map[key]`, creating an empty sequence there if missing (or not a sequence). */
inline Node& ensure_seq(Node& map, const std::string& key) {
    if (!map.contains(key) || !map.at(key).is_sequence()) map[key] = Node::sequence();
    return map[key];
}

/** @brief `map[key]`, creating an empty mapping there if missing (or not a mapping). */
inline Node& ensure_map(Node& map, const std::string& key) {
    if (!map.contains(key) || !map.at(key).is_mapping()) map[key] = Node::mapping();
    return map[key];
}

/** @brief Removes every key starting with "__" (editor-private stamps) recursively. */
inline void strip_private_keys(Node& n) {
    if (n.is_mapping()) {
        std::vector<std::string> drop;
        for (auto& kv : n.as_map()) {
            if (kv.first.is_string() && kv.first.get_value<std::string>().rfind("__", 0) == 0) {
                drop.push_back(kv.first.get_value<std::string>());
            }
        }
        for (const auto& k : drop) erase_key(n, k);
        for (auto& kv : n.as_map()) strip_private_keys(kv.second);
    } else if (n.is_sequence()) {
        for (auto& e : n.as_seq()) strip_private_keys(e);
    }
}

/** @brief The component type name of a component node (`type:` key, or a `!Tag`). */
inline std::string component_type(const Node& comp) {
    if (comp.has_tag_name()) {
        std::string t = comp.get_tag_name();
        while (!t.empty() && t.front() == '!') t.erase(t.begin());
        return t;
    }
    return get_string(comp, "type");
}

} // namespace toy::editor

#endif // TOYEDITOR_CORE_YAML_UTIL_H
