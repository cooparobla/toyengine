/**
 * @file imm_theme.h
 * @brief Themes for the immediate-mode layer: the look (imm::Style) as a YAML / caml file.
 *
 * A theme file holds the widget metrics and colours plus any number of application colour
 * sections -- the per-editor sections of a Blender theme (a 3D viewport's grid and wire
 * colours, an outliner's icon tints) that only the application knows the meaning of:
 *
 * @code{.yaml}
 * name: Blender Dark
 * author: toyengine
 * inherits: other_theme      # optional: start from another theme in the same directory
 * font: fonts/MyFont.ttf     # optional, relative to this file
 * metrics:
 *   font_size: 12
 *   rounding: 4
 * colors:
 *   panel_bg: "#303030"
 *   selection: "#4772b3d9"   # #rrggbb or #rrggbbaa, [r, g, b, a] floats, or {r, g, b, a}
 * viewport:                  # any other mapping: an application section of colours
 *   grid: "#ffffff0e"
 * @endcode
 *
 * Every key is optional: a missing one keeps the value it inherits (from `inherits:`, else
 * Style's built-in fallback), so a theme can be a handful of overrides. load and save both
 * go through visit_style(), so the two can never disagree about which fields exist.
 *
 * Kept out of imm.h so that header stays free of YAML; only hosts that load themes pay for
 * fkYAML here.
 */

#ifndef UICOOPA_IMMEDIATE_IMM_THEME_H
#define UICOOPA_IMMEDIATE_IMM_THEME_H

#include <uicoopa/immediate/imm.h>

#include <coopa/yaml/document.h>
#include <fkYAML/node.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace coopa {
namespace ui {
namespace imm {

/**
 * @brief Calls `v.metric(key, float&)` for every size in a Style and `v.color(key, vec4&)`
 *        for every colour, in file order. The single list of themable fields.
 */
template <class S, class V>
void visit_style(S& s, V&& v) {
    v.metric("font_size", s.font_size);
    v.metric("row_height", s.row_height);
    v.metric("padding", s.padding);
    v.metric("spacing", s.spacing);
    v.metric("indent", s.indent);
    v.metric("label_ratio", s.label_ratio);
    v.metric("label_align", s.label_align);
    v.metric("scrollbar", s.scrollbar);
    v.metric("rounding", s.rounding);
    v.metric("tooltip_delay", s.tooltip_delay);

    v.color("window_bg", s.window_bg);
    v.color("panel_bg", s.panel_bg);
    v.color("panel_alt", s.panel_alt);
    v.color("header", s.header);
    v.color("header_hover", s.header_hover);
    v.color("subpanel", s.subpanel);
    v.color("border", s.border);
    v.color("text", s.text);
    v.color("text_dim", s.text_dim);
    v.color("text_disabled", s.text_disabled);
    v.color("accent", s.accent);
    v.color("button", s.button);
    v.color("button_hover", s.button_hover);
    v.color("button_active", s.button_active);
    v.color("field", s.field);
    v.color("field_hover", s.field_hover);
    v.color("number", s.number);
    v.color("number_hover", s.number_hover);
    v.color("selection", s.selection);
    v.color("selection_dim", s.selection_dim);
    v.color("row_hover", s.row_hover);
    v.color("popup_bg", s.popup_bg);
    v.color("scroll_grab", s.scroll_grab);
    v.color("axis_x", s.axis_x);
    v.color("axis_y", s.axis_y);
    v.color("axis_z", s.axis_z);
    v.color("object_selected", s.object_selected);
    v.color("object_active", s.object_active);
    v.color("warning", s.warning);
    v.color("error", s.error);
    v.color("hover_tint", s.hover_tint);
    v.color("edge_highlight", s.edge_highlight);
    v.color("separator", s.separator);
    v.color("shadow", s.shadow);
    v.color("modal_dim", s.modal_dim);
    v.color("check_mark", s.check_mark);
}

/** @brief A named look: widget Style plus application colour sections. */
struct Theme {
    std::string name = "Default";
    std::string author;
    std::string description;
    std::string font;    ///< Absolute font path, or empty for the host's default.
    Style style;
    /// Application sections: section -> role -> colour (e.g. "viewport" -> "grid").
    std::map<std::string, std::map<std::string, glm::vec4>> sections;

    /** @brief An application colour, or `fallback` when the theme doesn't define it. */
    glm::vec4 color(const std::string& section, const std::string& key, glm::vec4 fallback) const {
        auto s = sections.find(section);
        if (s == sections.end()) return fallback;
        auto c = s->second.find(key);
        return c == s->second.end() ? fallback : c->second;
    }
};

namespace theme_detail {

inline float number(const fkyaml::node& n) {
    if (n.is_integer()) return static_cast<float>(n.get_value<int64_t>());
    if (n.is_float_number()) return static_cast<float>(n.get_value<double>());
    if (n.is_string()) return std::stof(n.get_value<std::string>());
    throw std::runtime_error("expected a number");
}

inline int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/** @brief "#rgb", "#rrggbb", "#rrggbbaa", [r, g, b(, a)] or {r, g, b, a} (floats 0..1). */
inline glm::vec4 parse_color(const fkyaml::node& n, glm::vec4 c) {
    if (n.is_string()) {
        std::string h = n.get_value<std::string>();
        if (!h.empty() && h[0] == '#') h.erase(0, 1);
        if (h.size() == 3) h = std::string{h[0], h[0], h[1], h[1], h[2], h[2]};
        if (h.size() != 6 && h.size() != 8) throw std::runtime_error("bad colour '" + n.get_value<std::string>() + "'");
        float out[4] = {c.r, c.g, c.b, 1.0f};
        for (size_t i = 0; i < h.size() / 2; ++i) {
            const int hi = hex_digit(h[i * 2]), lo = hex_digit(h[i * 2 + 1]);
            if (hi < 0 || lo < 0) throw std::runtime_error("bad colour '" + n.get_value<std::string>() + "'");
            out[i] = static_cast<float>(hi * 16 + lo) / 255.0f;
        }
        return {out[0], out[1], out[2], out[3]};
    }
    if (n.is_sequence()) {
        const auto& seq = n.as_seq();
        for (size_t i = 0; i < std::min<size_t>(4, seq.size()); ++i) c[static_cast<int>(i)] = number(seq[i]);
        if (seq.size() == 3) c.a = 1.0f;
        return c;
    }
    if (n.is_mapping()) {
        if (n.contains("r")) c.r = number(n.at("r"));
        if (n.contains("g")) c.g = number(n.at("g"));
        if (n.contains("b")) c.b = number(n.at("b"));
        if (n.contains("a")) c.a = number(n.at("a"));
        return c;
    }
    throw std::runtime_error("expected a colour");
}

/** @brief "#rrggbb" when opaque, else "#rrggbbaa". */
inline std::string format_color(glm::vec4 c) {
    auto b = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    char buf[16];
    if (b(c.a) == 255) std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", b(c.r), b(c.g), b(c.b));
    else std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", b(c.r), b(c.g), b(c.b), b(c.a));
    return buf;
}

inline std::string format_number(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    return buf;
}

inline bool is_reserved_key(const std::string& k) {
    return k == "name" || k == "author" || k == "description" || k == "inherits" || k == "font" ||
           k == "metrics" || k == "colors";
}

}  // namespace theme_detail

/**
 * @brief Applies a parsed theme document on top of `t` (so a partial theme keeps what it
 *        doesn't mention). `dir` resolves `font:` and `inherits:`; `depth` guards cycles.
 * @throws std::runtime_error naming the offending key on a malformed value.
 */
inline void apply_theme_node(const fkyaml::node& root, Theme& t, const std::filesystem::path& dir = {}, int depth = 0);

/** @brief Loads a theme file (.yaml, or its .caml twin) on top of Style's built-in fallback. */
inline Theme load_theme(const std::filesystem::path& path, int depth = 0) {
    const std::filesystem::path resolved = coopa::yaml::resolve_variant(path);
    fkyaml::node root;
    try {
        root = coopa::yaml::load_document(resolved);
    } catch (const std::exception& e) {
        throw std::runtime_error("theme '" + path.string() + "': " + e.what());
    }
    Theme t;
    t.name = path.stem().string();
    try {
        apply_theme_node(root, t, path.parent_path(), depth);
    } catch (const std::exception& e) {
        throw std::runtime_error("theme '" + path.string() + "': " + e.what());
    }
    return t;
}

inline void apply_theme_node(const fkyaml::node& root, Theme& t, const std::filesystem::path& dir, int depth) {
    using namespace theme_detail;
    if (!root.is_mapping()) throw std::runtime_error("a theme is a mapping");
    if (root.contains("inherits")) {
        if (depth > 8) throw std::runtime_error("`inherits:` chain too deep (a cycle?)");
        const std::string base = root.at("inherits").get_value<std::string>();
        std::filesystem::path bp = dir / base;
        if (!bp.has_extension()) bp += ".yaml";
        const std::string keep_name = t.name;
        t = load_theme(bp, depth + 1);
        t.name = keep_name;
    }
    if (root.contains("name")) t.name = root.at("name").get_value<std::string>();
    if (root.contains("author")) t.author = root.at("author").get_value<std::string>();
    if (root.contains("description")) t.description = root.at("description").get_value<std::string>();
    if (root.contains("font")) {
        const std::filesystem::path f = root.at("font").get_value<std::string>();
        t.font = (f.is_absolute() || dir.empty() ? f : dir / f).lexically_normal().string();
    }
    struct Reader {
        const fkyaml::node* metrics;
        const fkyaml::node* colors;
        void metric(const char* k, float& v) const {
            if (!metrics || !metrics->contains(k)) return;
            try { v = number(metrics->at(k)); } catch (const std::exception& e) {
                throw std::runtime_error(std::string("metrics.") + k + ": " + e.what());
            }
        }
        void color(const char* k, glm::vec4& c) const {
            if (!colors || !colors->contains(k)) return;
            try { c = parse_color(colors->at(k), c); } catch (const std::exception& e) {
                throw std::runtime_error(std::string("colors.") + k + ": " + e.what());
            }
        }
    };
    Reader r{root.contains("metrics") ? &root.at("metrics") : nullptr, root.contains("colors") ? &root.at("colors") : nullptr};
    visit_style(t.style, r);
    for (const auto& [k, v] : root.as_map()) {
        const std::string key = k.get_value<std::string>();
        if (is_reserved_key(key) || !v.is_mapping()) continue;
        auto& section = t.sections[key];
        for (const auto& [rk, rv] : v.as_map()) {
            const std::string role = rk.get_value<std::string>();
            try {
                section[role] = parse_color(rv, section.count(role) ? section[role] : glm::vec4(1.0f));
            } catch (const std::exception& e) {
                throw std::runtime_error(key + "." + role + ": " + e.what());
            }
        }
    }
}

/** @brief Parses theme YAML text (tests, embedded themes). */
inline Theme parse_theme(const std::string& yaml_text, const std::filesystem::path& dir = {}) {
    Theme t;
    apply_theme_node(fkyaml::node::deserialize(yaml_text), t, dir);
    return t;
}

/**
 * @brief The theme as YAML, every field written out in visit_style() order -- a complete
 *        file a theme author can start from. (font: is written as given, absolute.)
 */
inline std::string theme_to_yaml(const Theme& t) {
    using namespace theme_detail;
    std::ostringstream o;
    auto quoted = [](const std::string& s) {
        std::string r = "\"";
        for (char c : s) { if (c == '"' || c == '\\') r += '\\'; r += c; }
        return r + "\"";
    };
    o << "name: " << quoted(t.name) << "\n";
    if (!t.author.empty()) o << "author: " << quoted(t.author) << "\n";
    if (!t.description.empty()) o << "description: " << quoted(t.description) << "\n";
    if (!t.font.empty()) o << "font: " << quoted(t.font) << "\n";
    struct Writer {
        std::ostringstream* metrics;
        std::ostringstream* colors;
        void metric(const char* k, float v) const { *metrics << "  " << k << ": " << format_number(v) << "\n"; }
        void color(const char* k, glm::vec4 c) const { *colors << "  " << k << ": \"" << format_color(c) << "\"\n"; }
    };
    std::ostringstream m, c;
    visit_style(t.style, Writer{&m, &c});
    o << "metrics:\n" << m.str() << "colors:\n" << c.str();
    for (const auto& [name, roles] : t.sections) {
        o << name << ":\n";
        for (const auto& [role, col] : roles) o << "  " << role << ": \"" << format_color(col) << "\"\n";
    }
    return o.str();
}

/** @brief One theme file found by list_themes(). */
struct ThemeEntry {
    std::string id;     ///< File stem -- what a preference stores.
    std::string name;   ///< Display name (the file's `name:`).
    std::filesystem::path path;
};

/** @brief The themes in `dir` (*.yaml / *.caml), sorted by display name. Unreadable files are skipped. */
inline std::vector<ThemeEntry> list_themes(const std::filesystem::path& dir) {
    std::vector<ThemeEntry> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return out;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const auto ext = e.path().extension();
        if (ext != ".yaml" && ext != ".caml") continue;
        const std::string id = e.path().stem().string();
        if (std::any_of(out.begin(), out.end(), [&](const ThemeEntry& t) { return t.id == id; })) continue;
        try {
            fkyaml::node root = coopa::yaml::load_document(e.path());
            std::string name = root.contains("name") ? root.at("name").get_value<std::string>() : id;
            out.push_back({id, name, e.path()});
        } catch (...) {}
    }
    std::sort(out.begin(), out.end(), [](const ThemeEntry& a, const ThemeEntry& b) { return a.name < b.name; });
    return out;
}

}  // namespace imm
}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_IMMEDIATE_IMM_THEME_H
