/**
 * @file writer.h
 * @brief Writes fkYAML nodes back out as clean, stable, re-loadable YAML text.
 *
 * fkYAML's own serializer is a debugging aid, not an authoring tool: its mappings are
 * std::map (keys come out alphabetical, so `name` lands after `components`), every
 * collection is block style (a vec3 becomes four lines), and it never quotes ambiguous
 * strings. This emitter is what tools that WRITE the engine's assets use (the toyengine
 * editor saves scenes, meshes, materials and config through it):
 *
 *   - Keys in a canonical order: identity keys first (`format`, `scene`, `name`, `type`,
 *     ...), structural keys last (`components`, `children`, `root_objects`, `faces`, ...),
 *     anything else in caller-supplied order (KeyOrder), then alphabetically.
 *   - Flow style for short all-scalar collections: `position: { x: 0.0, y: 1.5, z: 0.0 }`,
 *     `- [0.5, 0.5, 0.0]` -- the shape every hand-written file in this repo already uses.
 *   - Floats in their shortest round-tripping form, always with a '.', so a float stays a
 *     float on reload; integers stay integers.
 *   - Strings quoted exactly when YAML would otherwise read them as something else.
 *
 * Comments are not preserved -- fkYAML discards them on parse, so a document loaded and
 * re-saved loses them. load -> save -> load -> save is byte-stable.
 */

#ifndef COOPA_YAML_WRITER_H
#define COOPA_YAML_WRITER_H

#include <fkYAML/node.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace coopa {
namespace yaml {

/**
 * @brief Extra ordering for keys of mappings the canonical lists don't cover: returns a rank
 *        (lower first) or -1 for "no opinion" (alphabetical after every ranked key).
 *
 * Called with the key and the key of the mapping's parent ("" at the document root, the
 * sequence's own key for mappings inside a sequence), e.g. ("roughness", "material").
 */
using KeyOrder = std::function<int(const std::string& key, const std::string& parent_key)>;

namespace detail {

inline const std::vector<std::string>& leading_keys() {
    static const std::vector<std::string> keys = {
        "format", "version", "scene", "scene_name", "inherit_from", "auto_transform",
        "name", "prefab", "active", "type", "id", "remove", "base",
    };
    return keys;
}

inline const std::vector<std::string>& trailing_keys() {
    static const std::vector<std::string> keys = {
        "material_slots", "vertices", "normals", "uvs", "tangents", "colors", "weights", "joints", "joint_weights", "faces",
        "face_materials",
        "lods", "cull_screen_size",
        "components", "children", "root_objects",
    };
    return keys;
}

inline int index_in(const std::vector<std::string>& v, const std::string& k) {
    auto it = std::find(v.begin(), v.end(), k);
    return it == v.end() ? -1 : static_cast<int>(it - v.begin());
}

inline std::string key_string(const fkyaml::node& k) {
    if (k.is_string()) return k.get_value<std::string>();
    if (k.is_integer()) return std::to_string(k.get_value<int64_t>());
    if (k.is_boolean()) return k.get_value<bool>() ? "true" : "false";
    return "";
}

/** @brief Shortest decimal that parses back to exactly `v`, always float-looking. */
inline std::string format_float(double v) {
    if (std::isnan(v)) return ".nan";
    if (std::isinf(v)) return v > 0 ? ".inf" : "-.inf";
    if (v == 0.0) return std::signbit(v) ? "-0.0" : "0.0";
    char buf[64];
    for (int precision = 1; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    std::string s = buf;
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    // "1e+20" is not a YAML 1.2 float without a '.': make it "1.0e+20".
    const size_t e = s.find_first_of("eE");
    if (e != std::string::npos && s.find('.') == std::string::npos) s.insert(e, ".0");
    return s;
}

inline bool looks_like_number(const std::string& s) {
    if (s.empty()) return false;
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    if (end && *end == '\0') return true;
    const std::string l = [&] { std::string t = s; for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return t; }();
    return l == ".inf" || l == "-.inf" || l == "+.inf" || l == ".nan" || (l.size() > 2 && l[0] == '0' && (l[1] == 'x' || l[1] == 'o'));
}

/** @brief Quotes `s` if a YAML reader would not read it back as this exact string. */
inline std::string format_string(const std::string& s) {
    bool quote = s.empty();
    if (!quote) {
        std::string l = s;
        for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // YAML 1.2's core schema (what fkYAML implements) plus 1.1's yes/no/on/off, which other
        // readers still treat as booleans; single-letter y/n are plain strings in 1.2, and
        // quoting them would turn every { x, y, z } key into "y".
        static const char* reserved[] = {"null", "~", "true", "false", "yes", "no", "on", "off"};
        for (const char* r : reserved) if (l == r) quote = true;
    }
    if (!quote && looks_like_number(s)) quote = true;
    if (!quote) {
        const char c0 = s.front();
        if (std::string("-?:,[]{}#&*!|>'\"%@` ").find(c0) != std::string::npos) quote = true;
        if (s.back() == ' ' || s.back() == ':') quote = true;
        if (s.find(": ") != std::string::npos || s.find(" #") != std::string::npos) quote = true;
        if (s.find_first_of(",[]{}") != std::string::npos) quote = true;   // flow-context safety
        for (unsigned char c : s) if (c < 0x20 || c == 0x7f) quote = true;
    }
    if (!quote) return s;
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:   out += c; break;
        }
    }
    out += "\"";
    return out;
}

inline std::string format_scalar(const fkyaml::node& n) {
    if (n.is_null()) return "null";
    if (n.is_boolean()) return n.get_value<bool>() ? "true" : "false";
    if (n.is_integer()) return std::to_string(n.get_value<int64_t>());
    if (n.is_float_number()) return format_float(n.get_value<double>());
    if (n.is_string()) return format_string(n.get_value<std::string>());
    return "null";
}

inline bool is_flow_candidate(const fkyaml::node& n, size_t max_items) {
    if (n.is_sequence()) {
        const auto& seq = n.as_seq();
        if (seq.size() > max_items) return false;
        for (const auto& e : seq) if (!e.is_scalar()) return false;
        return true;
    }
    if (n.is_mapping()) {
        const auto& map = n.as_map();
        if (map.size() > 6) return false;
        for (const auto& kv : map) if (!kv.second.is_scalar()) return false;
        return true;
    }
    return false;
}

class Emitter {
public:
    explicit Emitter(KeyOrder order) : order_(std::move(order)) {}

    std::string run(const fkyaml::node& root) {
        out_.clear();
        if (root.is_mapping() && !root.as_map().empty()) emit_mapping_body_(root, 0, "");
        else if (root.is_sequence() && !root.as_seq().empty()) emit_sequence_body_(root, 0, "");
        else out_ += flow_(root, "") + "\n";
        return out_;
    }

private:
    std::vector<std::pair<std::string, const fkyaml::node*>> sorted_(const fkyaml::node& map, const std::string& parent) const {
        std::vector<std::pair<std::string, const fkyaml::node*>> entries;
        for (const auto& kv : map.as_map()) entries.emplace_back(key_string(kv.first), &kv.second);
        auto rank = [&](const std::string& k) -> std::pair<int, int> {
            if (int i = index_in(leading_keys(), k); i >= 0) return {0, i};
            if (int i = index_in(trailing_keys(), k); i >= 0) return {3, i};
            if (order_) if (int r = order_(k, parent); r >= 0) return {1, r};
            return {2, 0};
        };
        std::stable_sort(entries.begin(), entries.end(), [&](const auto& a, const auto& b) {
            const auto ra = rank(a.first), rb = rank(b.first);
            if (ra != rb) return ra < rb;
            return a.first < b.first;
        });
        return entries;
    }

    std::string tag_prefix_(const fkyaml::node& n) const {
        return n.has_tag_name() ? n.get_tag_name() + " " : std::string();
    }

    std::string flow_(const fkyaml::node& n, const std::string& parent) const {
        if (n.is_sequence()) {
            std::string s = tag_prefix_(n) + "[";
            bool first = true;
            for (const auto& e : n.as_seq()) {
                if (!first) s += ", ";
                s += flow_(e, parent);
                first = false;
            }
            return s + "]";
        }
        if (n.is_mapping()) {
            if (n.as_map().empty()) return tag_prefix_(n) + "{}";
            std::string s = tag_prefix_(n) + "{ ";
            bool first = true;
            for (const auto& [k, v] : sorted_(n, parent)) {
                if (!first) s += ", ";
                s += format_string(k) + ": " + flow_(*v, k);
                first = false;
            }
            return s + " }";
        }
        return format_scalar(n);
    }

    bool use_flow_(const fkyaml::node& n, const std::string& parent, size_t indent) const {
        if (n.is_mapping() && n.as_map().empty()) return true;
        if (n.is_sequence() && n.as_seq().empty()) return true;
        // A tagged collection is always flow: the vendored fkYAML misparses block-style tags.
        if (n.has_tag_name() && (n.is_mapping() || n.is_sequence())) return true;
        if (!is_flow_candidate(n, 16)) return false;
        return flow_(n, parent).size() + indent < 110;
    }

    void line_(size_t indent, const std::string& text) {
        out_.append(indent, ' ');
        out_ += text;
        out_ += '\n';
    }

    void emit_mapping_body_(const fkyaml::node& map, size_t indent, const std::string& parent) {
        for (const auto& [k, v] : sorted_(map, parent)) {
            const std::string key = format_string(k) + ":";
            if (v->is_scalar()) {
                line_(indent, key + " " + format_scalar(*v));
            } else if (use_flow_(*v, k, indent + key.size() + 1)) {
                line_(indent, key + " " + flow_(*v, k));
            } else if (v->is_sequence()) {
                line_(indent, key);
                emit_sequence_body_(*v, indent + 2, k);
            } else {
                line_(indent, key);
                emit_mapping_body_(*v, indent + 2, k);
            }
        }
    }

    void emit_sequence_body_(const fkyaml::node& seq, size_t indent, const std::string& parent) {
        for (const auto& e : seq.as_seq()) {
            if (e.is_scalar()) {
                line_(indent, "- " + format_scalar(e));
            } else if (use_flow_(e, parent, indent + 2)) {
                line_(indent, "- " + flow_(e, parent));
            } else if (e.is_mapping()) {
                // First key on the dash line, the rest aligned under it.
                const size_t start = out_.size();
                emit_mapping_body_(e, indent + 2, parent);
                out_[start + indent] = '-';
            } else {
                line_(indent, "-");
                emit_sequence_body_(e, indent + 2, parent);
            }
        }
    }

    KeyOrder order_;
    std::string out_;
};

} // namespace detail

/** @brief Renders `root` as YAML text (see the file doc for the conventions). */
inline std::string emit(const fkyaml::node& root, KeyOrder order = {}) {
    return detail::Emitter(std::move(order)).run(root);
}

/**
 * @brief Writes `text` to `path` atomically: a sibling temp file, then a rename, so a crash
 *        mid-write never leaves a half-written asset. Creates parent directories.
 * @throws std::runtime_error on any I/O failure.
 */
inline void write_text_atomic(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += ".tmp~";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("[coopa::yaml] Cannot write '" + tmp.string() + "'");
        out << text;
        if (!out) throw std::runtime_error("[coopa::yaml] Failed writing '" + tmp.string() + "'");
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        throw std::runtime_error("[coopa::yaml] Cannot replace '" + path.string() + "'");
    }
}

/** @brief emit() + write_text_atomic(). */
inline void save_document(const std::filesystem::path& path, const fkyaml::node& root, KeyOrder order = {}) {
    write_text_atomic(path, emit(root, std::move(order)));
}

} // namespace yaml
} // namespace coopa

#endif // COOPA_YAML_WRITER_H
