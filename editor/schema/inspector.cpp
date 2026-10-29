#include "editor/schema/inspector.h"

#include "editor/schema/inspector_sections.h"
#include "editor/ui/curve_widget.h"

#include <cstdio>

namespace toy {
namespace editor {

void EditResult::absorb(const EditResult& o) {
    if (o.changed && key.empty()) key = o.key;
    changed |= o.changed;
    active |= o.active;
    finished |= o.finished;
}

} // namespace editor
} // namespace toy

namespace toy {
namespace editor {
namespace detail {

bool has_numeric_keys(const Node& n, std::initializer_list<const char*> ks) {
    if (!n.is_mapping() || n.size() != ks.size()) return false;
    for (const char* k : ks) if (!n.contains(k) || !is_numeric(n.at(k))) return false;
    return true;
}

std::string ref_to_path(const FieldDesc& f, const std::string& v) {
    if (v.empty()) return v;
    std::string p = v;
    if (!f.ref_prefix.empty() && p.rfind(f.ref_prefix, 0) == 0) p = p.substr(f.ref_prefix.size());
    if (f.strip_ext && !f.asset_ext.empty() && p.find('.') == std::string::npos) p += f.asset_ext;
    if (f.strip_dir && !f.asset_dir.empty() && p.rfind(f.asset_dir + "/", 0) != 0) p = f.asset_dir + "/" + p;
    return p;
}

std::string path_to_ref(const FieldDesc& f, const std::string& path) {
    std::string v = path;
    // Tag folders between the type folder and the file are dropped (materials/metal/brick ->
    // materials/brick): the engine finds assets by type and name, so the reference survives
    // re-tagging (coopa::asset::AssetIndex). A theme keeps its ui/themes/ folder.
    if (!f.asset_dir.empty() && v.rfind(f.asset_dir + "/", 0) == 0) {
        const std::string rest = v.substr(f.asset_dir.size() + 1);
        const size_t last = rest.rfind('/');
        if (last != std::string::npos) {
            const bool theme = f.asset_dir == "ui" && rest.rfind("themes/", 0) == 0;
            v = f.asset_dir + "/" + (theme ? "themes/" : "") + rest.substr(last + 1);
        }
    }
    if (f.strip_dir && !f.asset_dir.empty() && v.rfind(f.asset_dir + "/", 0) == 0) v = v.substr(f.asset_dir.size() + 1);
    if (f.strip_ext && !f.asset_ext.empty() && v.size() > f.asset_ext.size() &&
        v.compare(v.size() - f.asset_ext.size(), f.asset_ext.size(), f.asset_ext) == 0) {
        v = v.substr(0, v.size() - f.asset_ext.size());
    }
    return f.ref_prefix + v;
}

void finish(imm::Context& ctx, EditResult& r, const std::string& key, bool changed, bool group) {
    r.key = key;
    r.changed = changed;
    r.active = group ? ctx.last_group_active() : ctx.last_active();
    r.finished = ctx.last_deactivated();
}

} // namespace detail
} // namespace editor
} // namespace toy

namespace toy {
namespace editor {

namespace {

/** @brief `a` and `b` the same, ignoring case -- enums are written in several spellings. */
bool same_ci(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

std::string number_text(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

/**
 * @brief The choices an AssetRef-ish field offers: "" first, then every matching asset as the
 *        value the field would store. `idx` comes back as the current value's place in it.
 */
std::vector<std::string> asset_choices(const FieldDesc& f, const std::string& current, const InspectorEnv& env, int& idx) {
    std::vector<std::string> values{""};
    if (env.list_assets) {
        for (const auto& p : env.list_assets(f.asset_dir, f.asset_ext)) values.push_back(detail::path_to_ref(f, p));
    }
    idx = -1;
    for (size_t i = 0; i < values.size(); ++i) if (values[i] == current) idx = static_cast<int>(i);
    if (idx < 0) { values.push_back(current); idx = static_cast<int>(values.size()) - 1; }
    return values;
}

/** @brief Whether `path` is one this field would accept from a drag-and-drop. */
bool asset_matches(const FieldDesc& f, const std::string& p) {
    if (!f.asset_dir.empty() && p.rfind(f.asset_dir + "/", 0) != 0) return false;
    if (f.asset_ext.empty()) return true;
    return p.size() >= f.asset_ext.size() &&
           p.compare(p.size() - f.asset_ext.size(), f.asset_ext.size(), f.asset_ext) == 0;
}

/**
 * @brief A texture map as a picture: the image itself, its name, and the ways to change it.
 *
 * Falls back to the plain dropdown when the host offers no previews (tests, headless), so the
 * field works everywhere and only looks better where there is a GPU.
 */
EditResult draw_texture_slot(imm::Context& ctx, const FieldDesc& f, Node& block, const InspectorEnv& env) {
    EditResult r;
    r.key = f.key;
    const std::string cur = block.contains(f.key) ? get_string(block, f.key) : std::string();
    const float h = 42.0f;
    const imm::Box row = ctx.property_row(f.display() + "##" + f.key, h);
    const imm::Box thumb{row.x, row.y + 2.0f, h - 6.0f, h - 6.0f};

    ctx.fill_rounded(thumb, ctx.style.field);
    coopa::gfx::TextureView view;
    if (!cur.empty() && env.texture_preview) view = env.texture_preview(detail::ref_to_path(f, cur));
    if (view.valid()) ctx.image(view, thumb.shrink(1.0f));
    else ctx.icon(imm::Icon::Image, thumb.shrink(7.0f), ctx.style.text_disabled);
    ctx.outline_rounded(thumb, ctx.style.border);

    int idx = -1;
    std::vector<std::string> values = asset_choices(f, cur, env, idx);
    std::vector<std::string> shown = values;
    shown[0] = "(none)";
    for (auto& v : shown) {
        const size_t slash = v.find_last_of('/');   // the file's name is what identifies it here
        if (slash != std::string::npos) v = v.substr(slash + 1);
    }
    const float left = thumb.right() + 6.0f;
    const float clear_w = cur.empty() ? 0.0f : 22.0f;
    const imm::Box picker{left, row.y + 3.0f, std::max(40.0f, row.right() - left - clear_w), ctx.style.row_height};
    bool changed = ctx.combo_box(f.key, picker, &idx, shown);
    if (auto dropped = ctx.drop_target("asset", row)) {
        if (asset_matches(f, *dropped)) {
            values.push_back(detail::path_to_ref(f, *dropped));
            idx = static_cast<int>(values.size()) - 1;
            changed = true;
        }
    }
    if (!cur.empty()) {
        const imm::Box clear{row.right() - 20.0f, row.y + 3.0f, 18.0f, ctx.style.row_height};
        if (ctx.icon_button("clear_" + f.key, imm::Icon::X, "Clear\nUse no " + f.display(), false, 0.0f, imm::Context::kAll, clear)) {
            idx = 0;
            changed = true;
        }
        // The resolution and the path, where there is room for them.
        ctx.text_in({left, thumb.bottom() - 13.0f, row.w - (left - row.x), 13.0f}, cur, ctx.style.text_dim, 0.0f);
    }
    if (changed) {
        const std::string& v = values[static_cast<size_t>(idx)];
        if (v.empty()) erase_key(block, f.key);
        else block[f.key] = Node(v);
        r.changed = r.finished = true;
    }
    return r;
}

} // namespace

std::string condition_value(const Node& block, const std::string& key, const std::vector<FieldDesc>& fields) {
    if (block.is_mapping() && block.contains(key)) {
        const Node& n = block.at(key);
        if (n.is_boolean()) return n.get_value<bool>() ? "true" : "false";
        if (n.is_integer()) return std::to_string(n.get_value<int64_t>());
        if (n.is_float_number()) return number_text(n.get_value<double>());
        if (n.is_string()) return n.get_value<std::string>();
        return {};
    }
    for (const auto& f : fields) {
        if (f.key != key) continue;
        switch (f.kind) {
            case FieldKind::Bool:  return f.def.x != 0.0f ? "true" : "false";
            case FieldKind::Int:   return std::to_string(static_cast<int64_t>(std::lround(f.def.x)));
            case FieldKind::Float: return number_text(f.def.x);
            default:               return f.default_string;
        }
    }
    return {};
}

bool show_when_holds(const Node& block, const ShowWhen& w, const std::vector<FieldDesc>& fields) {
    if (w.always()) return true;
    const std::string v = condition_value(block, w.key, fields);
    bool found = false;
    for (const auto& want : w.equals) if (same_ci(v, want)) found = true;
    return w.negate ? !found : found;
}

EditResult draw_field(imm::Context& ctx, const FieldDesc& f, Node& block, const InspectorEnv& env) {
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
            // "%.3f m" -- the unit rides in the number's format, so it shows while dragging too.
            const std::string fmt = f.suffix.empty() ? std::string("%.3f") : "%.3f " + f.suffix;
            const bool bounded = f.min > -1e29f && f.max < 1e29f;
            const bool ch = f.slider && bounded ? ctx.slider_float(label, &v, f.min, f.max, fmt.c_str())
                                                : ctx.drag_float(label, &v, f.speed, f.min, f.max, fmt.c_str());
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
            int idx = -1;
            std::vector<std::string> values = asset_choices(f, cur, env, idx);
            std::vector<std::string> shown = values;
            shown[0] = "(none)";
            imm::Box row = ctx.property_row(label);
            bool ch = ctx.combo_box(f.key, row, &idx, shown);
            // Drag an asset from the browser onto the field.
            if (auto dropped = ctx.drop_target("asset", row)) {
                if (asset_matches(f, *dropped)) {
                    values.push_back(detail::path_to_ref(f, *dropped));
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
        case FieldKind::Texture:
            r = draw_texture_slot(ctx, f, block, env);
            break;
        case FieldKind::Material:
            break;  // drawn by draw_material_field()
        case FieldKind::Vec2: {
            const Node* n = present ? &block.at(f.key) : nullptr;
            float v[2] = {n ? get_float(*n, "x", f.def.x) : f.def.x, n ? get_float(*n, "y", f.def.y) : f.def.y};
            const bool ch = ctx.drag_floatn(label, v, 2, f.speed);
            if (ch) {
                Node o = Node::mapping();
                o["x"] = make_float(v[0]);
                o["y"] = make_float(v[1]);
                block[f.key] = o;
            }
            detail::finish(ctx, r, f.key, ch, true);
            break;
        }
        case FieldKind::Color4: {
            const Node* n = present ? &block.at(f.key) : nullptr;
            float rgba[4] = {f.def.r, f.def.g, f.def.b, f.def.a};
            if (n && n->is_mapping()) {
                rgba[0] = get_float(*n, "r", f.def.r); rgba[1] = get_float(*n, "g", f.def.g);
                rgba[2] = get_float(*n, "b", f.def.b); rgba[3] = get_float(*n, "a", f.def.a);
            } else if (n && n->is_sequence()) {
                const auto& seq = n->as_seq();
                for (size_t i = 0; i < std::min<size_t>(4, seq.size()); ++i) rgba[i] = as_float(seq[i]);
            }
            const bool ch = ctx.color_edit(label, rgba, true);
            if (ch) {
                if (f.as_list) {
                    block[f.key] = make_float_seq(rgba, 4);
                } else {
                    Node o = make_color({rgba[0], rgba[1], rgba[2]});
                    o["a"] = make_float(rgba[3]);
                    block[f.key] = o;
                }
            }
            detail::finish(ctx, r, f.key, ch, true);
            break;
        }
        case FieldKind::Padding: {
            static const char* keys[4] = {"left", "right", "top", "bottom"};
            const Node* n = present ? &block.at(f.key) : nullptr;
            float v[4];
            for (int i = 0; i < 4; ++i) v[i] = n ? get_float(*n, keys[i], f.def[i]) : f.def[i];
            const bool ch = ctx.drag_floatn(label, v, 4, f.speed);
            ctx.tooltip(f.display() + "\nLeft, right, top, bottom");
            if (ch) {
                Node o = Node::mapping();
                for (int i = 0; i < 4; ++i) o[keys[i]] = make_float(v[i]);
                block[f.key] = o;
            }
            detail::finish(ctx, r, f.key, ch, true);
            break;
        }
        case FieldKind::StringList: {
            // One line, comma separated: quick to edit, and the list stays visible at a glance.
            std::string joined;
            const Node& src = present ? block.at(f.key) : f.def_node;
            if (src.is_sequence()) {
                for (const auto& e : src.as_seq()) {
                    if (!joined.empty()) joined += ", ";
                    if (e.is_string()) joined += e.get_value<std::string>();
                }
            }
            const bool ch = ctx.input_text(label, &joined);
            ctx.tooltip(f.display() + "\nComma-separated");
            if (ch) {
                Node seq = Node::sequence();
                size_t start = 0;
                while (start <= joined.size()) {
                    size_t comma = joined.find(',', start);
                    std::string part = joined.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                    while (!part.empty() && part.front() == ' ') part.erase(part.begin());
                    while (!part.empty() && part.back() == ' ') part.pop_back();
                    if (!part.empty()) seq.as_seq().push_back(Node(part));
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
                block[f.key] = seq;
            }
            r.key = f.key; r.changed = ch; r.finished = ch;
            break;
        }
        case FieldKind::ChildRef: {
            const std::string cur = present ? get_string(block, f.key) : f.default_string;
            std::vector<std::string> values{""};
            if (env.object_names) for (const auto& n : env.object_names()) values.push_back(n);
            int idx = -1;
            for (size_t i = 0; i < values.size(); ++i) if (values[i] == cur) idx = static_cast<int>(i);
            if (idx < 0) { values.push_back(cur); idx = static_cast<int>(values.size()) - 1; }
            std::vector<std::string> shown = values;
            shown[0] = "(none)";
            bool ch = ctx.combo(label, &idx, shown);
            // Drop an object straight out of the outliner, Unity-style. Its payload is an id, so
            // the name has to be looked up; an id with no name is ignored rather than written.
            if (auto dropped = ctx.drop_target("object", ctx.last_rect())) {
                std::string name;
                if (env.object_name) {
                    try { name = env.object_name(static_cast<ObjectId>(std::stoull(*dropped))); }
                    catch (const std::exception&) { name.clear(); }
                }
                if (!name.empty()) {
                    values.push_back(name);
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
        case FieldKind::Curve: {
            std::vector<glm::vec2> pts = ui::curve_from_node(present ? block.at(f.key) : f.def_node);
            ctx.label_dim(f.display());
            const ui::ShapeEdit se = ui::curve_edit(ctx, f.key, pts);
            if (se.changed) block[f.key] = ui::curve_to_node(pts);
            r.key = f.key;
            r.changed = se.changed;
            r.active = se.dragging;
            r.finished = se.finished;
            break;
        }
        case FieldKind::Gradient: {
            std::vector<ui::ColorStop> stops = ui::gradient_from_node(present ? block.at(f.key) : f.def_node);
            ctx.label_dim(f.display());
            ui::ShapeEdit se = ui::gradient_edit(ctx, f.key, stops);
            // The selected stop's colour is an ordinary colour row, so its picker and its undo
            // step behave like every other colour in the inspector.
            if (se.selected >= 0 && se.selected < static_cast<int>(stops.size())) {
                glm::vec3& c = stops[static_cast<size_t>(se.selected)].color;
                float rgba[4] = {c.r, c.g, c.b, 1.0f};
                if (ctx.color_edit("Stop Colour##" + f.key, rgba)) {
                    c = {rgba[0], rgba[1], rgba[2]};
                    se.changed = true;
                    se.dragging = ctx.last_group_active();
                    se.finished = se.finished || ctx.last_deactivated();
                }
            }
            if (se.changed) block[f.key] = ui::gradient_to_node(stops);
            r.key = f.key;
            r.changed = se.changed;
            r.active = se.dragging;
            r.finished = se.finished;
            break;
        }
        case FieldKind::ItemList: {
            if (!present || !block.at(f.key).is_sequence()) block[f.key] = f.def_node.is_sequence() ? f.def_node : Node::sequence();
            Node& list = block[f.key];
            ctx.label_dim(f.display());
            int remove = -1, move_up = -1;
            for (size_t i = 0; i < list.size(); ++i) {
                ctx.push_id(static_cast<int64_t>(i));
                Node& item = list.as_seq()[i];
                if (item.is_string()) {   // shorthand `- Label` -> a full item
                    Node m = Node::mapping();
                    m["label"] = item;
                    item = m;
                }
                ctx.indent(6);
                for (const auto& sub : f.item_fields) {
                    ctx.push_id(sub.key);
                    EditResult er = draw_field(ctx, sub, item, env);
                    if (er.changed) er.key = f.key;
                    r.absorb(er);
                    ctx.pop_id();
                }
                if (ctx.button("Remove", 80)) remove = static_cast<int>(i);
                if (i > 0) {
                    ctx.same_line();
                    if (ctx.button("Move Up", 80)) move_up = static_cast<int>(i);
                }
                ctx.unindent(6);
                ctx.separator();
                ctx.pop_id();
            }
            if (ctx.button(("Add " + f.display()).c_str(), -1)) {
                Node m = Node::mapping();
                for (const auto& sub : f.item_fields) {
                    if (sub.kind == FieldKind::Enum && !sub.options.empty()) m[sub.key] = Node(sub.options.front());
                    else m[sub.key] = Node(sub.key == "label" ? std::string("Item ") + std::to_string(list.size() + 1)
                                                               : sub.key == "name" ? std::string("Item") + std::to_string(list.size() + 1)
                                                                                   : std::string());
                }
                list.as_seq().push_back(m);
                r.key = f.key; r.changed = r.finished = true;
            }
            if (remove >= 0) { list.as_seq().erase(list.as_seq().begin() + remove); r.key = f.key; r.changed = r.finished = true; }
            if (move_up > 0) {
                std::swap(list.as_seq()[static_cast<size_t>(move_up)], list.as_seq()[static_cast<size_t>(move_up - 1)]);
                r.key = f.key; r.changed = r.finished = true;
            }
            if (r.changed) r.key = f.key;
            break;
        }
    }
    if (!f.tooltip.empty() && f.kind != FieldKind::Padding && f.kind != FieldKind::StringList) ctx.tooltip(f.display() + "\n" + f.tooltip);
    return r;
}

EditResult draw_generic(imm::Context& ctx, const std::string& key, Node& block) {
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

EditResult draw_fields(imm::Context& ctx, const std::vector<FieldDesc>& fields, Node& block,
                              const InspectorEnv& env, bool show_unknown,
                              const std::set<std::string>& skip) {
    EditResult total;
    std::set<std::string> known = skip;
    for (const auto& f : fields) {
        known.insert(f.key);
        if (f.kind == FieldKind::Material || skip.count(f.key)) continue;   // skipped keys are drawn by the caller
        if (!show_when_holds(block, f.show, fields)) continue;   // this row does nothing in the current mode
        ctx.push_id(f.key);
        total.absorb(draw_field(ctx, f, block, env));
        ctx.pop_id();
        if (env.after_field && f.kind != FieldKind::Padding) env.after_field(ctx, f.key, ctx.last_label_box());
    }
    if (show_unknown && block.is_mapping()) {
        for (const std::string& k : keys_of(block)) {
            if (known.count(k) || k.rfind("__", 0) == 0) continue;
            ctx.push_id(k);
            total.absorb(draw_generic(ctx, k, block));
            ctx.pop_id();
            if (env.after_field) env.after_field(ctx, k, ctx.last_label_box());
        }
    }
    return total;
}

EditResult draw_shader_section(imm::Context& ctx, Node& m) {
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
        erase_key(m, "shader_params_ext");
        if (!s.params.empty()) {
            float d[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            for (size_t i = 0; i < s.params.size() && i < 8; ++i) d[i] = s.params[i].def;
            m["shader_params"] = make_float_seq(d, 4);
            if (s.params.size() > 4) m["shader_params_ext"] = make_float_seq(d + 4, 4);
        }
        const bool blend = m.contains("alpha_mode") && get_string(m, "alpha_mode") == "BLEND";
        if (s.transparent && !blend) m["alpha_mode"] = Node(std::string("BLEND"));
        if (!s.transparent && !s.name.empty() && blend) m["alpha_mode"] = Node(std::string("OPAQUE"));
        r.key = "shader"; r.changed = r.finished = true;
    }
    const SurfaceShaderInfo* info = find_surface_shader(m.contains("shader") ? get_string(m, "shader") : std::string());
    if (!info) return r;
    if (!info->note.empty()) ctx.paragraph(info->note, &ctx.style.text_dim);   // wraps: these are sentences
    if (info->driven) {
        ctx.paragraph("Parameters are set by the object's WaterBody.", &ctx.style.text_dim);
        return r;
    }
    if (info->params.empty()) return r;

    // Slots 0-3 ride in shader_params; 4-7 in shader_params_ext, which only the forward
    // transparent backbone reads (PBRMaterial::shader_params_ext).
    float v[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (size_t i = 0; i < info->params.size() && i < 8; ++i) v[i] = info->params[i].def;
    auto read = [&](const char* key, int base) {
        if (!m.contains(key) || !m.at(key).is_sequence()) return;
        const auto& seq = m.at(key).as_seq();
        for (size_t i = 0; i < std::min<size_t>(4, seq.size()); ++i) v[base + static_cast<int>(i)] = as_float(seq[i]);
    };
    read("shader_params", 0);
    read("shader_params_ext", 4);
    for (size_t i = 0; i < info->params.size() && i < 8; ++i) {
        const ShaderParamDesc& p = info->params[i];
        const std::string label = p.label + "##shader_param" + std::to_string(i);
        bool ch = false;
        EditResult pr;
        if (!p.options.empty()) {
            int sel = std::clamp(static_cast<int>(std::lround(v[i])), 0, static_cast<int>(p.options.size()) - 1);
            ch = ctx.combo(label, &sel, p.options);
            if (ch) v[i] = static_cast<float>(sel);
            pr.key = i < 4 ? "shader_params" : "shader_params_ext"; pr.changed = ch; pr.finished = ch;
        } else {
            ch = ctx.drag_float(label, &v[i], p.speed, p.min, p.max);
            detail::finish(ctx, pr, i < 4 ? "shader_params" : "shader_params_ext", ch);
        }
        if (ch) {
            m["shader_params"] = make_float_seq(v, 4);
            if (info->params.size() > 4) m["shader_params_ext"] = make_float_seq(v + 4, 4);
        }
        r.absorb(pr);
    }
    return r;
}

namespace {

/**
 * @brief The colour space each of this material's maps is read in.
 *
 * The engine takes it as a map from texture path to "srgb" / "linear" (PBRMaterial's
 * texture_color_space), and gets it right on its own for the usual cases -- so this only
 * appears for maps that are actually set, and only writes a key when the author overrides it.
 * Reading a normal map as sRGB, or an albedo map as linear, is a classic silent mistake; this
 * is where it gets fixed.
 */
EditResult draw_color_space_rows(imm::Context& ctx, Node& m, const std::vector<FieldDesc>& fields) {
    EditResult r;
    std::vector<std::pair<std::string, std::string>> maps;   // label, path
    for (const auto& f : fields) {
        if (f.kind != FieldKind::Texture || !m.contains(f.key)) continue;
        const std::string path = get_string(m, f.key);
        if (!path.empty()) maps.push_back({f.display(), path});
    }
    if (maps.empty()) return r;
    ctx.spacing(3);
    ctx.label_dim("Colour space");
    Node spaces = m.contains("texture_color_space") && m.at("texture_color_space").is_mapping() ? m.at("texture_color_space")
                                                                                                : Node::mapping();
    bool changed = false;
    for (const auto& [label, path] : maps) {
        // Auto first: the engine's own choice (colour data sRGB, data maps linear) stays the
        // default, and nothing is written to the file until someone picks otherwise.
        static const std::vector<std::string> kChoices = {"Auto", "sRGB", "Linear"};
        const std::string cur = spaces.contains(path) ? get_string(spaces, path) : std::string();
        int pick = cur == "srgb" ? 1 : cur == "linear" ? 2 : 0;
        ctx.push_id(path);
        if (ctx.segmented(label, &pick, kChoices)) {
            if (pick == 0) erase_key(spaces, path);
            else spaces[path] = Node(std::string(pick == 1 ? "srgb" : "linear"));
            changed = true;
        }
        ctx.tooltip(label + " colour space\n" + path +
                    "\nAuto: albedo and emissive maps are read as sRGB, everything else linear");
        ctx.pop_id();
    }
    if (changed) {
        if (spaces.size() == 0) erase_key(m, "texture_color_space");
        else m["texture_color_space"] = spaces;
        r.key = "texture_color_space";
        r.changed = r.finished = true;
    }
    return r;
}

} // namespace

EditResult draw_material_block(imm::Context& ctx, Node& m, const InspectorEnv& env, bool show_unknown) {
    const ComponentSchema& schema = material_schema();
    EditResult r = draw_shader_section(ctx, m);
    ctx.spacing();
    // The shader keys are the Shader section's; `base` belongs to the reference, not the material.
    std::set<std::string> drawn{"base", "shader", "shader_params", "shader_params_ext", "texture_color_space"};
    for (const auto& s : schema.sections) {
        r.absorb(draw_section(ctx, s, schema, m, env, drawn));
        if (s.title == "Maps") r.absorb(draw_color_space_rows(ctx, m, schema.fields));
    }
    if (!show_unknown) return r;
    std::vector<FieldDesc> rest;
    for (const auto& f : schema.fields) if (!drawn.count(f.key)) rest.push_back(f);
    bool unknown = false;
    if (m.is_mapping()) {
        for (const std::string& k : keys_of(m)) {
            if (drawn.count(k) || k.rfind("__", 0) == 0 || schema.field(k)) continue;
            unknown = true;
        }
    }
    if ((rest.empty() && !unknown) || !ctx.collapsing_header("More", false, nullptr, imm::Icon::Dots)) return r;
    ctx.indent(6);
    r.absorb(draw_fields(ctx, rest, m, env, true, drawn));
    ctx.unindent(6);
    return r;
}

EditResult draw_material_field(imm::Context& ctx, Node& comp, const InspectorEnv& env,
                                      const std::function<void(const std::string&)>& open_material) {
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
    const bool mode_changed = ctx.segmented("Material##mode", &new_mode, {"Inline", "Asset", "Asset +"});
    ctx.tooltip("Material\nInline: this object's own material, stored on the object.\n"
                "Asset: a shared materials/*.yaml -- editing it changes every object using it.\n"
                "Asset +: that shared material with a few values changed here only.");
    if (mode_changed && new_mode != mode) {
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
        // Either the caller's "open this material" or, failing that, the editor-wide one.
        const auto open = open_material ? open_material : env.open_asset;
        if (open && !ref.empty() && ctx.button("Edit Material Asset", -1, true, imm::Icon::Material)) open(ref + ".yaml");
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

EditResult draw_section(imm::Context& ctx, const InspectorSection& s, const ComponentSchema& schema, Node& comp,
                        const InspectorEnv& env, std::set<std::string>& drawn,
                        const std::function<void(const std::string&)>& open_material) {
    EditResult r;
    // A section whose mode is not the current one is not just closed, it does not apply: its
    // keys count as drawn so they are not repeated down in "More".
    auto claim_keys = [&] { for (const auto& k : s.keys) drawn.insert(k); };
    if (!show_when_holds(comp, s.show, schema.fields)) { claim_keys(); return r; }
    // Nothing left to put in it: every row is hidden by its own condition, or the caller drew
    // them itself (a MeshRenderer's material, which has its own tab). An empty header with a
    // sentence under it and no rows reads as broken, so the section does not appear at all.
    const bool has_rows = std::any_of(s.keys.begin(), s.keys.end(), [&](const std::string& k) {
        const FieldDesc* f = schema.field(k);
        return !drawn.count(k) && f && show_when_holds(comp, f->show, schema.fields);
    });
    if (!has_rows && s.custom.empty() && s.toggle.empty()) { claim_keys(); return r; }

    ctx.push_id(s.title.empty() ? std::string("section") : s.title);
    const imm::Icon icon = imm::icon_from_name(s.icon);
    bool open = false;
    if (!s.toggle.empty()) {
        bool on = condition_value(comp, s.toggle, schema.fields) == "true";
        const bool was = on;
        open = ctx.collapsing_header(s.title, s.open, nullptr, icon, &on);
        if (on != was) {
            comp[s.toggle] = Node(on);
            r.key = s.toggle;
            r.changed = r.finished = true;
        }
        if (env.after_field) env.after_field(ctx, s.toggle, ctx.last_rect());
        drawn.insert(s.toggle);
    } else {
        open = ctx.collapsing_header(s.title, s.open, nullptr, icon);
    }
    if (!open) { claim_keys(); ctx.pop_id(); return r; }

    ctx.indent(6);
    if (!s.blurb.empty()) ctx.paragraph(s.blurb, &ctx.style.text_dim);
    if (const SectionDrawFn* fn = find_inspector_section(s.custom)) {
        SectionContext sc{comp, env, env.object_id, &drawn};
        r.absorb((*fn)(ctx, sc));
    }

    size_t next_sub = 0;
    for (size_t i = 0; i < s.keys.size(); ++i) {
        while (next_sub < s.subheads.size() && s.subheads[next_sub].index == i) {
            const InspectorSubhead& h = s.subheads[next_sub++];
            if (!show_when_holds(comp, h.show, schema.fields)) continue;
            ctx.spacing(3);
            const imm::Box hb = ctx.next_box(ctx.style.row_height - 4);
            ctx.text_in(hb, h.title, ctx.style.text_dim, 0.0f);
            const float tw = ctx.text_width(h.title) + 8;
            ctx.fill({hb.x + tw, hb.y + hb.h * 0.5f, std::max(0.0f, hb.w - tw), 1.0f}, ctx.style.separator);
        }
        const std::string& key = s.keys[i];
        if (!drawn.insert(key).second) continue;            // a custom drawer already did it
        const FieldDesc* f = schema.field(key);
        if (!f) continue;                                   // a key no schema field describes: the tests catch it
        if (!show_when_holds(comp, f->show, schema.fields)) continue;
        if (f->kind == FieldKind::Material) {
            const float y = ctx.cursor().y;
            r.absorb(draw_material_field(ctx, comp, env, open_material));
            // The whole material block is one override value: its first row stands for it.
            if (env.after_field) env.after_field(ctx, key, imm::Box{ctx.cursor().x, y, 120.0f, ctx.style.row_height});
            continue;
        }
        ctx.push_id(key);
        r.absorb(draw_field(ctx, *f, comp, env));
        ctx.pop_id();
        if (env.after_field && f->kind != FieldKind::Padding) env.after_field(ctx, key, ctx.last_label_box());
    }
    ctx.unindent(6);
    ctx.spacing(4);
    ctx.pop_id();
    return r;
}

namespace {

/** @brief A sectioned component: each section, then everything no section claimed. */
EditResult draw_sectioned_(imm::Context& ctx, const ComponentSchema& schema, Node& comp, const InspectorEnv& env,
                           const std::function<void(const std::string&)>& open_material,
                           const std::set<std::string>* hide_keys) {
    EditResult r;
    std::set<std::string> drawn{"type", "id"};
    if (hide_keys) drawn.insert(hide_keys->begin(), hide_keys->end());
    for (const auto& s : schema.sections) r.absorb(draw_section(ctx, s, schema, comp, env, drawn, open_material));

    // Everything the layout did not place. A key added to the schema without being put in a
    // section still shows up here rather than disappearing.
    std::vector<FieldDesc> rest;
    for (const auto& f : schema.fields) if (!drawn.count(f.key)) rest.push_back(f);
    bool unknown = false;
    if (comp.is_mapping()) {
        for (const std::string& k : keys_of(comp)) {
            if (drawn.count(k) || k.rfind("__", 0) == 0) continue;
            if (schema.field(k)) continue;
            unknown = true;
        }
    }
    if (rest.empty() && !unknown) return r;
    if (ctx.collapsing_header("More", false, nullptr, imm::Icon::Dots)) {
        ctx.indent(6);
        std::set<std::string> skip = drawn;
        r.absorb(draw_fields(ctx, rest, comp, env, true, skip));
        ctx.unindent(6);
    }
    return r;
}

} // namespace

EditResult draw_component(imm::Context& ctx, Node& comp, const InspectorEnv& env,
                                 const std::function<void(const std::string&)>& open_material,
                                 const std::set<std::string>* hide_keys) {
    const std::string type = component_type(comp);
    const ComponentSchema* schema = find_schema(type);
    EditResult r;
    // A component may take its whole inspector over (ParticleSystem, TileTerrain).
    if (const SectionDrawFn* fn = find_inspector_section(type)) {
        SectionContext sc{comp, env, env.object_id, nullptr};
        return (*fn)(ctx, sc);
    }
    if (schema && !schema->sections.empty()) return draw_sectioned_(ctx, *schema, comp, env, open_material, hide_keys);
    std::set<std::string> skip{"type", "id"};
    if (hide_keys) skip.insert(hide_keys->begin(), hide_keys->end());
    if (schema) {
        r.absorb(draw_fields(ctx, schema->fields, comp, env, true, skip));
        for (const auto& f : schema->fields) {
            if (f.kind != FieldKind::Material || skip.count(f.key)) continue;
            const float y = ctx.cursor().y;
            r.absorb(draw_material_field(ctx, comp, env, open_material));
            // The whole material block is one override value: its first row stands for it.
            if (env.after_field) env.after_field(ctx, f.key, imm::Box{ctx.cursor().x, y, 120.0f, ctx.style.row_height});
        }
    } else {
        r.absorb(draw_fields(ctx, {}, comp, env, true, skip));
        if (comp.is_mapping() && comp.size() <= 1) ctx.label_dim("(no properties)");
    }
    return r;
}

} // namespace editor
} // namespace toy
