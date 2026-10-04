/**
 * @file inspector.h
 * @brief Draws editable property rows for a YAML block (a component, a material, a config
 *        section) with uicoopa's immediate-mode widgets, guided by a schema.
 *
 * Edits are applied to the node in place; the caller decides what an edit means (push an
 * undo step, rebuild a live object, mark a file dirty). EditResult says whether anything
 * changed this frame, whether a continuous edit (a drag) is still in progress -- so the
 * caller can merge it into one undo step -- and when it finished.
 */

#ifndef TOYEDITOR_SCHEMA_INSPECTOR_H
#define TOYEDITOR_SCHEMA_INSPECTOR_H

#include "component_schema.h"

#include <uicoopa/immediate/imm.h>

#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace toy::editor {

namespace imm = coopa::ui::imm;

/** @brief What the inspector needs from the editor around it. */
struct InspectorEnv {
    /// Project-relative asset paths ("meshes/cube.yaml") under `dir` with extension `ext`.
    std::function<std::vector<std::string>(const std::string& dir, const std::string& ext)> list_assets;
};

struct EditResult {
    bool changed = false;     ///< The node was modified this frame.
    bool active = false;      ///< A drag/edit is still in progress (merge undo steps).
    bool finished = false;    ///< A drag/edit ended this frame (close the undo merge).
    std::string key;          ///< The key that changed (for undo labels / merge keys).

    void absorb(const EditResult& o) {
        if (o.changed && key.empty()) key = o.key;
        changed |= o.changed;
        active |= o.active;
        finished |= o.finished;
    }
};

namespace detail {

inline bool is_numeric(const Node& n) { return n.is_integer() || n.is_float_number(); }

inline bool has_numeric_keys(const Node& n, std::initializer_list<const char*> ks) {
    if (!n.is_mapping() || n.size() != ks.size()) return false;
    for (const char* k : ks) if (!n.contains(k) || !is_numeric(n.at(k))) return false;
    return true;
}

/** @brief Value of an AssetRef as stored -> the project-relative path it denotes. */
inline std::string ref_to_path(const FieldDesc& f, const std::string& v) {
    if (v.empty()) return v;
    std::string p = v;
    if (!f.ref_prefix.empty() && p.rfind(f.ref_prefix, 0) == 0) p = p.substr(f.ref_prefix.size());
    if (f.strip_ext && !f.asset_ext.empty() && p.find('.') == std::string::npos) p += f.asset_ext;
    if (f.strip_dir && !f.asset_dir.empty() && p.rfind(f.asset_dir + "/", 0) != 0) p = f.asset_dir + "/" + p;
    return p;
}

/** @brief Project-relative path -> the value an AssetRef field stores. */
inline std::string path_to_ref(const FieldDesc& f, const std::string& path) {
    std::string v = path;
    if (f.strip_dir && !f.asset_dir.empty() && v.rfind(f.asset_dir + "/", 0) == 0) v = v.substr(f.asset_dir.size() + 1);
    if (f.strip_ext && !f.asset_ext.empty() && v.size() > f.asset_ext.size() &&
        v.compare(v.size() - f.asset_ext.size(), f.asset_ext.size(), f.asset_ext) == 0) {
        v = v.substr(0, v.size() - f.asset_ext.size());
    }
    return f.ref_prefix + v;
}

inline void finish(imm::Context& ctx, EditResult& r, const std::string& key, bool changed, bool group = false) {
    r.key = key;
    r.changed = changed;
    r.active = group ? ctx.last_group_active() : ctx.last_active();
    r.finished = ctx.last_deactivated();
}

} // namespace detail

/** @brief One field of `block` (the key may be absent: the schema default is shown). */
inline EditResult draw_field(imm::Context& ctx, const FieldDesc& f, Node& block, const InspectorEnv& env) {
    EditResult r;
    const std::string label = f.display() + "##" + f.key;
    const bool present = block.contains(f.key);
    switch (f.kind) {
        case FieldKind::Bool: {
            bool v = present ? get_bool(block, f.key) : f.def.x != 0.0f;
            const bool ch = ctx.property_bool(label, &v);
            if (ch) block[f.key] = Node(v);
            r.key = f.key; r.changed = ch; r.finished = ch;
            break;
        }
        case FieldKind::Int: {
            int v = present ? get_int(block, f.key) : static_cast<int>(f.def.x);
            if (!f.options.empty()) {
                // Labelled int enum: a dropdown of names, the index is what gets written.
                std::vector<std::string> shown = f.options;
                if (v < 0 || v >= static_cast<int>(shown.size())) {
                    shown.push_back(std::to_string(v) + " (unknown)");
                    v = static_cast<int>(shown.size()) - 1;
                }
                const int before = v;
                const bool ch = ctx.combo(label, &v, shown) && v != before && v < static_cast<int>(f.options.size());
                const size_t sel = static_cast<size_t>(v);
                if (sel < f.option_tips.size()) ctx.tooltip(shown[sel] + " (" + f.key + ": " + std::to_string(v) + ")\n" + f.option_tips[sel]);
                if (ch) block[f.key] = Node(static_cast<int64_t>(v));
                r.key = f.key; r.changed = ch; r.finished = ch;
                break;
            }
            const bool ch = ctx.drag_int(label, &v, f.speed, static_cast<int>(f.min), static_cast<int>(f.max));
            if (ch) block[f.key] = Node(static_cast<int64_t>(v));
            detail::finish(ctx, r, f.key, ch);
            break;
        }
        case FieldKind::Float: {
            float v = present ? get_float(block, f.key) : f.def.x;
            const bool ch = ctx.drag_float(label, &v, f.speed, f.min, f.max);
            if (ch) block[f.key] = make_float(v);
            detail::finish(ctx, r, f.key, ch);
            break;
        }
        case FieldKind::Vec3: {
            glm::vec3 v = present ? as_vec3(block.at(f.key), glm::vec3(f.def)) : glm::vec3(f.def);
            const bool ch = ctx.drag_floatn(label, &v.x, 3, f.speed);
            if (ch) block[f.key] = f.as_list ? make_float_seq(&v.x, 3) : make_vec3(v);
            detail::finish(ctx, r, f.key, ch, true);
            break;
        }
        case FieldKind::Vec4: {
            float v[4] = {f.def.x, f.def.y, f.def.z, f.def.w};
            if (present && block.at(f.key).is_sequence()) {
                const auto& seq = block.at(f.key).as_seq();
                for (size_t i = 0; i < std::min<size_t>(4, seq.size()); ++i) v[i] = as_float(seq[i]);
            }
            const bool ch = ctx.drag_floatn(label, v, 4, f.speed);
            if (ch) block[f.key] = make_float_seq(v, 4);
            detail::finish(ctx, r, f.key, ch, true);
            break;
        }
        case FieldKind::Color: {
            glm::vec3 c = present ? get_color(block, f.key, glm::vec3(f.def)) : glm::vec3(f.def);
            float rgba[4] = {c.r, c.g, c.b, 1.0f};
            const bool ch = ctx.color_edit(label, rgba);
            if (ch) block[f.key] = f.as_list ? make_float_seq(rgba, 3) : make_color({rgba[0], rgba[1], rgba[2]});
            detail::finish(ctx, r, f.key, ch, true);
            break;
        }
        case FieldKind::Enum: {
            const std::string cur = present ? get_string(block, f.key) : f.default_string;
            std::vector<std::string> opts = f.options;
            int idx = -1;
            for (size_t i = 0; i < opts.size(); ++i) {
                std::string a = opts[i], b = cur;
                std::transform(a.begin(), a.end(), a.begin(), ::tolower);
                std::transform(b.begin(), b.end(), b.begin(), ::tolower);
                if (a == b) idx = static_cast<int>(i);
            }
            if (idx < 0) { opts.push_back(cur); idx = static_cast<int>(opts.size()) - 1; }
            std::vector<std::string> shown = opts;
            for (auto& s : shown) if (s.empty()) s = "(none)";
            const bool ch = ctx.combo(label, &idx, shown);
            if (ch) {
                if (opts[static_cast<size_t>(idx)].empty()) erase_key(block, f.key);
                else block[f.key] = Node(opts[static_cast<size_t>(idx)]);
            }
            r.key = f.key; r.changed = ch; r.finished = ch;
            break;
        }
        case FieldKind::String: {
            std::string v = present ? get_string(block, f.key) : f.default_string;
            const bool ch = ctx.input_text(label, &v);
            if (ch) {
                if (v.empty()) erase_key(block, f.key);
                else block[f.key] = Node(v);
            }
            r.key = f.key; r.changed = ch; r.finished = ch;
            break;
        }
        case FieldKind::AssetRef: {
            const std::string cur = present ? get_string(block, f.key) : std::string();
            std::vector<std::string> paths = env.list_assets ? env.list_assets(f.asset_dir, f.asset_ext) : std::vector<std::string>{};
            std::vector<std::string> values{""};
            for (const auto& p : paths) values.push_back(detail::path_to_ref(f, p));
            int idx = -1;
            for (size_t i = 0; i < values.size(); ++i) if (values[i] == cur) idx = static_cast<int>(i);
            if (idx < 0) { values.push_back(cur); idx = static_cast<int>(values.size()) - 1; }
            std::vector<std::string> shown = values;
            shown[0] = "(none)";
            imm::Box row = ctx.property_row(label);
            bool ch = ctx.combo_box(f.key, row, &idx, shown);
            // Drag an asset from the browser onto the field.
            if (auto dropped = ctx.drop_target("asset", row)) {
                const std::string& p = *dropped;
                if ((f.asset_dir.empty() || p.rfind(f.asset_dir + "/", 0) == 0) &&
                    (f.asset_ext.empty() || (p.size() >= f.asset_ext.size() &&
                     p.compare(p.size() - f.asset_ext.size(), f.asset_ext.size(), f.asset_ext) == 0))) {
                    values.push_back(detail::path_to_ref(f, p));
                    idx = static_cast<int>(values.size()) - 1;
                    ch = true;
                }
            }
            if (ch) {
                const std::string& v = values[static_cast<size_t>(idx)];
                if (v.empty()) erase_key(block, f.key);
                else block[f.key] = Node(v);
            }
            r.key = f.key; r.changed = ch; r.finished = ch;
            break;
        }
        case FieldKind::Material:
            break;  // drawn by draw_material_field()
    }
    return r;
}

/** @brief A generic row for a key no schema describes, inferred from its value. */
inline EditResult draw_generic(imm::Context& ctx, const std::string& key, Node& block) {
    EditResult r;
    Node& v = block[key];
    const std::string label = key + "##g_" + key;
    if (v.is_boolean()) {
        bool b = v.get_value<bool>();
        if (ctx.property_bool(label, &b)) { v = Node(b); r.changed = r.finished = true; }
    } else if (v.is_integer()) {
        int i = static_cast<int>(v.get_value<int64_t>());
        const bool ch = ctx.drag_int(label, &i);
        if (ch) v = Node(static_cast<int64_t>(i));
        detail::finish(ctx, r, key, ch);
    } else if (v.is_float_number()) {
        float f = as_float(v);
        const bool ch = ctx.drag_float(label, &f, 0.01f);
        if (ch) v = make_float(f);
        detail::finish(ctx, r, key, ch);
    } else if (v.is_string()) {
        std::string s = v.get_value<std::string>();
        if (ctx.input_text(label, &s)) { v = Node(s); r.changed = r.finished = true; }
    } else if (detail::has_numeric_keys(v, {"x", "y", "z"})) {
        glm::vec3 vec = as_vec3(v);
        const bool ch = ctx.drag_floatn(label, &vec.x, 3, 0.01f);
        if (ch) v = make_vec3(vec);
        detail::finish(ctx, r, key, ch, true);
    } else if (detail::has_numeric_keys(v, {"r", "g", "b"})) {
        glm::vec3 c = get_color(block, key);
        float rgba[4] = {c.r, c.g, c.b, 1};
        const bool ch = ctx.color_edit(label, rgba);
        if (ch) v = make_color({rgba[0], rgba[1], rgba[2]});
        detail::finish(ctx, r, key, ch, true);
    } else if (v.is_sequence() && v.size() >= 2 && v.size() <= 4 &&
               std::all_of(v.as_seq().begin(), v.as_seq().end(), [](const Node& e) { return detail::is_numeric(e); })) {
        float f[4] = {};
        const int n = static_cast<int>(v.size());
        for (int i = 0; i < n; ++i) f[i] = as_float(v.as_seq()[static_cast<size_t>(i)]);
        const bool ch = ctx.drag_floatn(label, f, n, 0.01f);
        if (ch) v = make_float_seq(f, n);
        detail::finish(ctx, r, key, ch, true);
    } else {
        imm::Box row = ctx.property_row(key);
        std::string summary = v.is_sequence() ? "[" + std::to_string(v.size()) + " items]"
                            : v.is_mapping() ? "{" + std::to_string(v.size()) + " keys}" : "null";
        ctx.text_in(row, summary, ctx.style.text_dim);
        ctx.tooltip("Edit this value in the YAML file; it is preserved on save.");
    }
    r.key = key;
    return r;
}

/**
 * @brief Draws every schema field of `block`, then (optionally) any extra keys generically.
 * @param skip Keys never shown (e.g. "type").
 */
inline EditResult draw_fields(imm::Context& ctx, const std::vector<FieldDesc>& fields, Node& block,
                              const InspectorEnv& env, bool show_unknown = true,
                              const std::set<std::string>& skip = {"type", "id"}) {
    EditResult total;
    std::set<std::string> known = skip;
    for (const auto& f : fields) {
        known.insert(f.key);
        if (f.kind == FieldKind::Material || skip.count(f.key)) continue;   // skipped keys are drawn by the caller
        ctx.push_id(f.key);
        total.absorb(draw_field(ctx, f, block, env));
        ctx.pop_id();
    }
    if (show_unknown && block.is_mapping()) {
        for (const std::string& k : keys_of(block)) {
            if (known.count(k) || k.rfind("__", 0) == 0) continue;
            ctx.push_id(k);
            total.absorb(draw_generic(ctx, k, block));
            ctx.pop_id();
        }
    }
    return total;
}

/**
 * @brief A material's Shader section: which shader it renders with, and that shader's own
 *        shader_params under their names. Choosing a shader keeps alpha_mode in the shader's
 *        pass (a Transparent shader is only drawn for BLEND, an Opaque one never is -- the
 *        renderer would silently fall back to the stock shader) and resets the params to the
 *        new shader's defaults.
 */
inline EditResult draw_shader_section(imm::Context& ctx, Node& m) {
    EditResult r;
    const std::string cur = m.contains("shader") ? get_string(m, "shader") : std::string();
    const auto& shaders = surface_shaders();
    std::vector<std::string> shown;
    int idx = -1;
    for (size_t i = 0; i < shaders.size(); ++i) {
        shown.push_back(shaders[i].label);
        if (shaders[i].name == cur) idx = static_cast<int>(i);
    }
    if (idx < 0) { shown.push_back(cur + " (internal)"); idx = static_cast<int>(shown.size()) - 1; }
    const int before = idx;
    if (ctx.combo("Shader##shader", &idx, shown) && idx != before && idx < static_cast<int>(shaders.size())) {
        const SurfaceShaderInfo& s = shaders[static_cast<size_t>(idx)];
        if (s.name.empty()) erase_key(m, "shader");
        else m["shader"] = Node(s.name);
        erase_key(m, "shader_params");
        if (!s.params.empty()) {
            float d[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (size_t i = 0; i < s.params.size() && i < 4; ++i) d[i] = s.params[i].def;
            m["shader_params"] = make_float_seq(d, 4);
        }
        const bool blend = m.contains("alpha_mode") && get_string(m, "alpha_mode") == "BLEND";
        if (s.transparent && !blend) m["alpha_mode"] = Node(std::string("BLEND"));
        if (!s.transparent && !s.name.empty() && blend) m["alpha_mode"] = Node(std::string("OPAQUE"));
        r.key = "shader"; r.changed = r.finished = true;
    }
    const SurfaceShaderInfo* info = find_surface_shader(m.contains("shader") ? get_string(m, "shader") : std::string());
    if (!info) return r;
    if (!info->note.empty()) ctx.label_dim(info->note);
    if (info->driven) {
        ctx.label_dim("Parameters are set by the object's WaterBody.");
        return r;
    }
    if (info->params.empty()) return r;

    float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (size_t i = 0; i < info->params.size() && i < 4; ++i) v[i] = info->params[i].def;
    if (m.contains("shader_params") && m.at("shader_params").is_sequence()) {
        const auto& seq = m.at("shader_params").as_seq();
        for (size_t i = 0; i < std::min<size_t>(4, seq.size()); ++i) v[i] = as_float(seq[i]);
    }
    for (size_t i = 0; i < info->params.size() && i < 4; ++i) {
        const ShaderParamDesc& p = info->params[i];
        const std::string label = p.label + "##shader_param" + std::to_string(i);
        bool ch = false;
        EditResult pr;
        if (!p.options.empty()) {
            int sel = std::clamp(static_cast<int>(std::lround(v[i])), 0, static_cast<int>(p.options.size()) - 1);
            ch = ctx.combo(label, &sel, p.options);
            if (ch) v[i] = static_cast<float>(sel);
            pr.key = "shader_params"; pr.changed = ch; pr.finished = ch;
        } else {
            ch = ctx.drag_float(label, &v[i], p.speed, p.min, p.max);
            detail::finish(ctx, pr, "shader_params", ch);
        }
        if (ch) m["shader_params"] = make_float_seq(v, 4);
        r.absorb(pr);
    }
    return r;
}

/** @brief Every field of a material block: the Shader section first, then the PBR fields. */
inline EditResult draw_material_block(imm::Context& ctx, Node& m, const InspectorEnv& env, bool show_unknown = true) {
    EditResult r = draw_shader_section(ctx, m);
    ctx.spacing();
    r.absorb(draw_fields(ctx, material_fields(), m, env, show_unknown, {"base", "shader", "shader_params"}));
    return r;
}

/**
 * @brief The `material:` key of a renderer component: Inline / Asset / Asset + overrides.
 * @param open_material Called with a project-relative material path when "Edit" is clicked.
 */
inline EditResult draw_material_field(imm::Context& ctx, Node& comp, const InspectorEnv& env,
                                      const std::function<void(const std::string&)>& open_material = {}) {
    EditResult r;
    r.key = "material";
    ctx.push_id("material");
    const bool present = comp.contains("material");
    int mode = 0;
    std::string ref;
    if (present && comp.at("material").is_string()) { mode = 1; ref = comp.at("material").get_value<std::string>(); }
    else if (present && comp.at("material").is_mapping() && comp.at("material").contains("base")) {
        mode = 2; ref = get_string(comp.at("material"), "base");
    }
    int new_mode = mode;
    if (ctx.combo("Material##mode", &new_mode, {"Inline", "Asset", "Asset + overrides"}) && new_mode != mode) {
        Node old = present ? comp.at("material") : Node::mapping();
        if (new_mode == 0) {
            Node m = old.is_mapping() ? old : Node::mapping();
            erase_key(m, "base");
            if (m.size() == 0) { m["albedo"] = make_color(glm::vec3(0.8f)); m["roughness"] = make_float(0.5); }
            comp["material"] = m;
        } else if (new_mode == 1) {
            comp["material"] = Node(ref.empty() ? std::string("materials/default") : ref);
        } else {
            Node m = old.is_mapping() ? old : Node::mapping();
            m["base"] = Node(ref.empty() ? std::string("materials/default") : ref);
            comp["material"] = m;
        }
        r.changed = r.finished = true;
        mode = new_mode;
    }

    if (mode == 1 || mode == 2) {
        FieldDesc f = f_asset("base", "materials", ".yaml", true, false);
        f.label = "Asset";
        std::string value = ref;
        Node tmp = Node::mapping();
        if (!value.empty()) tmp["base"] = Node(value);
        EditResult er = draw_field(ctx, f, tmp, env);
        if (er.changed) {
            const std::string nv = get_string(tmp, "base");
            if (mode == 1) comp["material"] = Node(nv.empty() ? std::string("materials/default") : nv);
            else comp["material"]["base"] = Node(nv);
            ref = nv;
            r.changed = r.finished = true;
        }
        if (open_material && !ref.empty() && ctx.button("Edit Material Asset")) open_material(ref + ".yaml");
    }
    if (mode == 0 || mode == 2) {
        if (!comp.contains("material") || !comp.at("material").is_mapping()) comp["material"] = Node::mapping();
        Node& m = comp["material"];
        ctx.indent(6);
        if (mode == 2) {
            // Overrides: only keys present on the block are shown; others can be added.
            std::vector<FieldDesc> present_fields;
            std::vector<std::string> addable;
            for (const auto& f : material_fields()) {
                if (f.key == "shader_params") continue;   // part of the Shader override
                if (m.contains(f.key)) present_fields.push_back(f);
                else addable.push_back(f.key);
            }
            if (m.contains("shader")) r.absorb(draw_shader_section(ctx, m));
            r.absorb(draw_fields(ctx, present_fields, m, env, false, {"base", "shader", "shader_params"}));
            addable.insert(addable.begin(), "+ Override...");
            int pick = 0;
            if (ctx.combo("##add_override", &pick, addable) && pick > 0) {
                for (const auto& f : material_fields()) {
                    if (f.key != addable[static_cast<size_t>(pick)]) continue;
                    switch (f.kind) {
                        case FieldKind::Color: m[f.key] = make_color(glm::vec3(f.def)); break;
                        case FieldKind::Bool: m[f.key] = Node(f.def.x != 0); break;
                        case FieldKind::Float: m[f.key] = make_float(f.def.x); break;
                        case FieldKind::Enum: m[f.key] = Node(f.default_string); break;
                        default: m[f.key] = Node(std::string()); break;
                    }
                }
                r.changed = r.finished = true;
            }
        } else {
            r.absorb(draw_material_block(ctx, m, env));
        }
        ctx.unindent(6);
    }
    ctx.pop_id();
    return r;
}

/**
 * @brief The full inspector body for one component node.
 * @return The edit result; `comp` is modified in place.
 */
inline EditResult draw_component(imm::Context& ctx, Node& comp, const InspectorEnv& env,
                                 const std::function<void(const std::string&)>& open_material = {}) {
    const std::string type = component_type(comp);
    const ComponentSchema* schema = find_schema(type);
    EditResult r;
    if (schema) {
        r.absorb(draw_fields(ctx, schema->fields, comp, env, true));
        for (const auto& f : schema->fields) {
            if (f.kind == FieldKind::Material) r.absorb(draw_material_field(ctx, comp, env, open_material));
        }
    } else {
        r.absorb(draw_fields(ctx, {}, comp, env, true));
        if (comp.is_mapping() && comp.size() <= 1) ctx.label_dim("(no properties)");
    }
    return r;
}

} // namespace toy::editor

#endif // TOYEDITOR_SCHEMA_INSPECTOR_H
