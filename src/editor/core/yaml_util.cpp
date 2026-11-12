#include "editor/core/yaml_util.h"

namespace toy {
namespace editor {

float as_float(const Node& n, float def) {
    if (n.is_float_number()) return static_cast<float>(n.get_value<double>());
    if (n.is_integer()) return static_cast<float>(n.get_value<int64_t>());
    if (n.is_boolean()) return n.get_value<bool>() ? 1.0f : 0.0f;
    return def;
}

float get_float(const Node& map, const std::string& key, float def) {
    if (!map.is_mapping() || !map.contains(key)) return def;
    return as_float(map.at(key), def);
}

int get_int(const Node& map, const std::string& key, int def) {
    if (!map.is_mapping() || !map.contains(key)) return def;
    const Node& n = map.at(key);
    if (n.is_integer()) return static_cast<int>(n.get_value<int64_t>());
    if (n.is_float_number()) return static_cast<int>(n.get_value<double>());
    return def;
}

bool get_bool(const Node& map, const std::string& key, bool def) {
    if (!map.is_mapping() || !map.contains(key)) return def;
    const Node& n = map.at(key);
    if (n.is_boolean()) return n.get_value<bool>();
    if (n.is_integer()) return n.get_value<int64_t>() != 0;
    return def;
}

std::string get_string(const Node& map, const std::string& key, const std::string& def) {
    if (!map.is_mapping() || !map.contains(key)) return def;
    const Node& n = map.at(key);
    if (n.is_string()) return n.get_value<std::string>();
    if (n.is_integer()) return std::to_string(n.get_value<int64_t>());
    if (n.is_float_number()) return std::to_string(n.get_value<double>());
    if (n.is_boolean()) return n.get_value<bool>() ? "true" : "false";
    return def;
}

glm::vec3 as_vec3(const Node& n, glm::vec3 def) {
    if (n.is_mapping()) {
        return {get_float(n, "x", def.x), get_float(n, "y", def.y), get_float(n, "z", def.z)};
    }
    if (n.is_sequence() && n.size() >= 3) {
        return {as_float(n.as_seq()[0]), as_float(n.as_seq()[1]), as_float(n.as_seq()[2])};
    }
    return def;
}

glm::vec3 get_vec3(const Node& map, const std::string& key, glm::vec3 def) {
    if (!map.is_mapping() || !map.contains(key)) return def;
    return as_vec3(map.at(key), def);
}

glm::vec3 get_color(const Node& map, const std::string& key, glm::vec3 def) {
    if (!map.is_mapping() || !map.contains(key)) return def;
    const Node& n = map.at(key);
    if (n.is_mapping()) return {get_float(n, "r", def.r), get_float(n, "g", def.g), get_float(n, "b", def.b)};
    if (n.is_sequence() && n.size() >= 3) return as_vec3(n, def);
    return def;
}

double tidy(double v) {
    const double r = std::round(v * 1e5) / 1e5;
    return std::abs(r - v) < 1e-6 ? r : v;
}

Node make_vec3(const glm::vec3& v) {
    Node n = Node::mapping();
    n["x"] = make_float(v.x);
    n["y"] = make_float(v.y);
    n["z"] = make_float(v.z);
    return n;
}

Node make_color(const glm::vec3& c) {
    Node n = Node::mapping();
    n["r"] = make_float(c.r);
    n["g"] = make_float(c.g);
    n["b"] = make_float(c.b);
    return n;
}

Node make_float_seq(const float* v, int n) {
    Node s = Node::sequence();
    for (int i = 0; i < n; ++i) s.as_seq().push_back(make_float(v[i]));
    return s;
}

void erase_key(Node& map, const std::string& key) {
    if (!map.is_mapping()) return;
    map.as_map().erase(Node(key));
}

std::vector<std::string> keys_of(const Node& map) {
    std::vector<std::string> out;
    if (!map.is_mapping()) return out;
    for (const auto& kv : map.as_map()) {
        if (kv.first.is_string()) out.push_back(kv.first.get_value<std::string>());
    }
    return out;
}

Node& ensure_seq(Node& map, const std::string& key) {
    if (!map.contains(key) || !map.at(key).is_sequence()) map[key] = Node::sequence();
    return map[key];
}

Node& ensure_map(Node& map, const std::string& key) {
    if (!map.contains(key) || !map.at(key).is_mapping()) map[key] = Node::mapping();
    return map[key];
}

bool is_empty_placeholder(const Node& n) {
    if (!n.is_mapping() || !n.contains(kInheritedKey)) return false;
    for (const auto& kv : n.as_map()) {
        const std::string k = kv.first.is_string() ? kv.first.get_value<std::string>() : std::string();
        if (k == "name" || k.rfind("__", 0) == 0) continue;
        if (k == "components" && kv.second.is_sequence() && kv.second.size() == 0) continue;
        if (k == "children" && kv.second.is_sequence() &&
            std::all_of(kv.second.as_seq().begin(), kv.second.as_seq().end(), [](const Node& c) { return is_empty_placeholder(c); })) continue;
        return false;
    }
    return true;
}

void strip_private_keys(Node& n) {
    if (n.is_mapping()) {
        const bool placeholder = n.contains(kInheritedKey);
        const bool instance = n.contains("prefab") || n.contains("inherit_from");
        std::vector<std::string> drop;
        for (auto& kv : n.as_map()) {
            if (kv.first.is_string() && kv.first.get_value<std::string>().rfind("__", 0) == 0) {
                drop.push_back(kv.first.get_value<std::string>());
            }
        }
        for (const auto& k : drop) erase_key(n, k);
        for (auto& kv : n.as_map()) strip_private_keys(kv.second);
        // What an override node left empty is noise in the file.
        if (placeholder || instance) {
            for (const char* k : {"children", "components"}) {
                if (n.contains(k) && n.at(k).is_sequence() && n.at(k).size() == 0 && (placeholder || std::string(k) == "children")) erase_key(n, k);
            }
        }
    } else if (n.is_sequence()) {
        auto& seq = n.as_seq();
        seq.erase(std::remove_if(seq.begin(), seq.end(), [](const Node& e) { return is_empty_placeholder(e); }), seq.end());
        for (auto& e : seq) strip_private_keys(e);
    }
}

std::string component_type(const Node& comp) {
    if (comp.has_tag_name()) {
        std::string t = comp.get_tag_name();
        while (!t.empty() && t.front() == '!') t.erase(t.begin());
        return t;
    }
    return get_string(comp, "type");
}

} // namespace editor
} // namespace toy
