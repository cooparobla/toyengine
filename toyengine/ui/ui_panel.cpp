#include <toyengine/ui/ui_panel.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <fkYAML/node.hpp>

#include <coopa/asset/asset_loader.h>
#include <coopa/asset/asset_manager.h>
#include <coopa/yaml/document.h>

#include <toyengine/core/user_settings.h>
#include <toyengine/ui/ui_tunables.h>

namespace toy::ui {
namespace {

using coopa::ui::imm::Box;
using coopa::ui::imm::Context;

RowKind parse_kind(const std::string& s) {
    if (s == "slider") return RowKind::Slider;
    if (s == "toggle") return RowKind::Toggle;
    if (s == "int") return RowKind::Int;
    if (s == "combo") return RowKind::Combo;
    if (s == "button") return RowKind::Button;
    if (s == "separator") return RowKind::Separator;
    if (s == "spacer") return RowKind::Spacer;
    return RowKind::Label;
}

float num(const fkyaml::node& n, float def) {
    if (n.is_integer()) return static_cast<float>(n.get_value<int64_t>());
    if (n.is_float_number()) return static_cast<float>(n.get_value<double>());
    return def;
}

std::string str(const fkyaml::node& n, const char* key, const std::string& def = {}) {
    return (n.is_mapping() && n.contains(key)) ? n.at(key).get_value<std::string>() : def;
}

/// Snaps `v` to the nearest multiple of `step`. A step of 0 (the default) leaves it alone.
float quantize(float v, float step) {
    if (step <= 0.0f) return v;
    return std::round(v / step) * step;
}

UiRow parse_row(const fkyaml::node& n) {
    UiRow r;
    if (!n.is_mapping()) return r;
    r.kind = parse_kind(str(n, "kind", "label"));
    r.label = str(n, "label");
    r.bind = str(n, "bind");
    r.action = str(n, "action");
    r.tooltip = str(n, "tooltip");
    if (n.contains("min")) r.min = num(n.at("min"), r.min);
    if (n.contains("max")) r.max = num(n.at("max"), r.max);
    if (n.contains("step")) r.step = num(n.at("step"), r.step);
    if (n.contains("options") && n.at("options").is_sequence()) {
        for (const auto& o : n.at("options")) r.options.push_back(o.get_value<std::string>());
    }
    // `default:` is typed by the row's kind, so one YAML key covers all four shapes.
    if (n.contains("default")) {
        const fkyaml::node& d = n.at("default");
        switch (r.kind) {
            case RowKind::Toggle:
                if (d.is_boolean()) r.default_on = d.get_value<bool>();
                else if (d.is_integer()) r.default_on = d.get_value<int64_t>() != 0;
                break;
            case RowKind::Combo:
                if (d.is_string()) r.default_option = d.get_value<std::string>();
                break;
            case RowKind::Slider:
            case RowKind::Int:
                if (d.is_integer() || d.is_float_number()) r.default_num = num(d, 0.0f);
                break;
            default: break;
        }
    }
    // A button with no action still has a name worth reporting, so the label stands in.
    if (r.kind == RowKind::Button && r.action.empty()) r.action = r.label;
    return r;
}

glm::vec2 parse_vec2(const fkyaml::node& n, glm::vec2 def) {
    if (n.is_sequence() && n.size() >= 2) return {num(n[0], def.x), num(n[1], def.y)};
    if (n.is_mapping() && n.contains("x") && n.contains("y")) {
        return {num(n.at("x"), def.x), num(n.at("y"), def.y)};
    }
    return def;
}

/// Loads a UiPanel from a .yaml file. Pure data, so there is nothing to finalize on the
/// main thread -- decode does all of it and finalize hands the same object through.
class PanelLoader : public coopa::asset::TypedAssetLoader<UiPanel> {
public:
    std::shared_ptr<UiPanel> decode_typed(const coopa::asset::AssetId&,
                                          const coopa::asset::LoadContext& ctx) override {
        const fkyaml::node root = coopa::yaml::load_document(ctx.resolved_path);
        return std::make_shared<UiPanel>(parse_panel_node(root));
    }

    std::shared_ptr<UiPanel> finalize_typed(std::shared_ptr<UiPanel> decoded,
                                            const coopa::asset::AssetId&,
                                            const coopa::asset::LoadContext&) override {
        return decoded;
    }

    const char* type_name() const override { return "UiPanel"; }

    static UiPanel parse_panel_node(const fkyaml::node& root);
};

UiPanel PanelLoader::parse_panel_node(const fkyaml::node& root) {
    if (!root.is_mapping()) throw std::runtime_error("a UI panel must be a mapping");
    UiPanel p;
    p.title = str(root, "title");
    p.theme = str(root, "theme");
    if (root.contains("closable")) p.closable = root.at("closable").get_value<bool>();
    if (root.contains("offset")) p.offset = parse_vec2(root.at("offset"), p.offset);
    if (root.contains("size")) p.size = parse_vec2(root.at("size"), p.size);
    if (root.contains("anchor")) {
        coopa::ui::imm::Anchor a = p.anchor;
        if (parse_anchor(root.at("anchor").get_value<std::string>(), a)) p.anchor = a;
    }

    if (root.contains("tabs") && root.at("tabs").is_sequence()) {
        for (const auto& t : root.at("tabs")) {
            UiTab tab;
            tab.name = str(t, "name");
            if (t.is_mapping() && t.contains("rows") && t.at("rows").is_sequence()) {
                for (const auto& r : t.at("rows")) tab.rows.push_back(parse_row(r));
            }
            p.tabs.push_back(std::move(tab));
        }
    } else if (root.contains("rows") && root.at("rows").is_sequence()) {
        // An untabbed panel is one unnamed tab, so drawing has a single shape to handle.
        UiTab tab;
        for (const auto& r : root.at("rows")) tab.rows.push_back(parse_row(r));
        p.tabs.push_back(std::move(tab));
    }

    if (root.contains("buttons") && root.at("buttons").is_sequence()) {
        for (const auto& b : root.at("buttons")) {
            UiRow r = parse_row(b);
            r.kind = RowKind::Button;
            if (r.action.empty()) r.action = r.label;
            p.buttons.push_back(r);
        }
    }
    return p;
}

}  // namespace

// =====================================================================================
// Bindings
// =====================================================================================

void PanelBindings::provide(const std::string& key, FloatFn get, SetFloatFn set) {
    Binding& b = values_[key];
    b.get_f = std::move(get);
    b.set_f = std::move(set);
}

void PanelBindings::provide_bool(const std::string& key, BoolFn get, SetBoolFn set) {
    Binding& b = values_[key];
    b.get_b = std::move(get);
    b.set_b = std::move(set);
}

void PanelBindings::provide_int(const std::string& key, IntFn get, SetIntFn set) {
    Binding& b = values_[key];
    b.get_i = std::move(get);
    b.set_i = std::move(set);
}

void PanelBindings::provide_text(const std::string& key, TextFn get, SetTextFn set) {
    Binding& b = values_[key];
    b.get_t = std::move(get);
    b.set_t = std::move(set);
}

void PanelBindings::on_action(const std::string& action, ActionFn fn) {
    actions_[action] = std::move(fn);
}

bool PanelBindings::provided(const std::string& key) const {
    auto it = values_.find(key);
    if (it == values_.end()) return false;
    const Binding& b = it->second;
    return static_cast<bool>(b.get_f) || static_cast<bool>(b.get_b) ||
           static_cast<bool>(b.get_i) || static_cast<bool>(b.get_t);
}

float PanelBindings::get(const std::string& key, float fallback) const {
    auto it = values_.find(key);
    if (it != values_.end() && it->second.get_f) return it->second.get_f();
    return core::UserSettings::instance().get_float(key, fallback);
}

bool PanelBindings::get_bool(const std::string& key, bool fallback) const {
    auto it = values_.find(key);
    if (it != values_.end() && it->second.get_b) return it->second.get_b();
    return core::UserSettings::instance().get_bool(key, fallback);
}

int PanelBindings::get_int(const std::string& key, int fallback) const {
    auto it = values_.find(key);
    if (it != values_.end() && it->second.get_i) return it->second.get_i();
    return core::UserSettings::instance().get_int(key, fallback);
}

std::string PanelBindings::get_text(const std::string& key, const std::string& fallback) const {
    auto it = values_.find(key);
    if (it != values_.end() && it->second.get_t) return it->second.get_t();
    return core::UserSettings::instance().get_string(key, fallback);
}

void PanelBindings::set(const std::string& key, float value) {
    auto it = values_.find(key);
    if (it != values_.end() && it->second.set_f) {
        it->second.set_f(value);
        return;
    }
    core::UserSettings::instance().set_float(key, value);
}

void PanelBindings::set_bool(const std::string& key, bool value) {
    auto it = values_.find(key);
    if (it != values_.end() && it->second.set_b) {
        it->second.set_b(value);
        return;
    }
    core::UserSettings::instance().set_bool(key, value);
}

void PanelBindings::set_int(const std::string& key, int value) {
    auto it = values_.find(key);
    if (it != values_.end() && it->second.set_i) {
        it->second.set_i(value);
        return;
    }
    core::UserSettings::instance().set_int(key, value);
}

void PanelBindings::set_text(const std::string& key, const std::string& value) {
    auto it = values_.find(key);
    if (it != values_.end() && it->second.set_t) {
        it->second.set_t(value);
        return;
    }
    core::UserSettings::instance().set_string(key, value);
}

bool PanelBindings::invoke(const std::string& action) const {
    auto it = actions_.find(action);
    if (it == actions_.end() || !it->second) return false;
    it->second();
    return true;
}

// =====================================================================================
// Drawing
// =====================================================================================

namespace {

/// Draws one row and reports an action if it fired.
std::string draw_row(Context& ctx, const UiRow& row, PanelBindings& bindings) {
    switch (row.kind) {
        case RowKind::Separator: ctx.separator(); return {};
        case RowKind::Spacer:    ctx.spacing();   return {};
        case RowKind::Label:     ctx.label(row.label); return {};
        case RowKind::Button: {
            const bool hit = ctx.button(row.label);
            if (!row.tooltip.empty()) ctx.tooltip(row.tooltip);
            return hit ? row.action : std::string{};
        }
        case RowKind::Slider: {
            float v = bindings.get(row.bind, row.default_num.value_or(row.min));
            if (ctx.slider_float(row.label, &v, row.min, row.max, "%.2f")) {
                bindings.set(row.bind, quantize(v, row.step));
            }
            break;
        }
        case RowKind::Toggle: {
            bool on = bindings.get_bool(row.bind, row.default_on.value_or(false));
            if (ctx.property_bool(row.label, &on)) bindings.set_bool(row.bind, on);
            break;
        }
        case RowKind::Int: {
            const int def = static_cast<int>(std::lround(row.default_num.value_or(row.min)));
            int v = bindings.get_int(row.bind, def);
            // One unit per pixel: drag_int's third argument is a SPEED, and an integer row
            // means one-at-a-time. `step` quantizes the result instead.
            if (ctx.drag_int(row.label, &v, 1.0f,
                             static_cast<int>(row.min), static_cast<int>(row.max))) {
                bindings.set_int(row.bind, static_cast<int>(std::lround(quantize(
                                               static_cast<float>(v), row.step))));
            }
            break;
        }
        case RowKind::Combo: {
            // Nothing to choose from: say so rather than clamping into an empty range, which
            // would be clamp(v, 0, -1) -- a precondition violation, not a clamp.
            if (row.options.empty()) {
                ctx.label_dim(row.label.empty() ? "(combo)" : row.label);
                ctx.same_line();
                ctx.label("needs options:", &ctx.style.warning);
                break;
            }
            // Stored by NAME, not index: an index silently changes meaning for every existing
            // player the moment an option is inserted into the list.
            const std::string def = row.default_option.value_or(row.options.front());
            const std::string cur = bindings.get_text(row.bind, def);
            int v = 0;
            for (size_t i = 0; i < row.options.size(); ++i) {
                if (row.options[i] == cur) { v = static_cast<int>(i); break; }
            }
            if (ctx.combo(row.label, &v, row.options)) {
                bindings.set_text(row.bind, row.options[static_cast<size_t>(v)]);
            }
            break;
        }
    }
    if (!row.tooltip.empty()) ctx.tooltip(row.tooltip);
    return {};
}

}  // namespace

std::string draw_panel(Context& ctx, const Box& b, const UiPanel& panel, PanelBindings& bindings,
                       int* active_tab) {
    std::string fired;
    bool close_clicked = false;
    const Box inner = panel.title.empty()
                          ? b
                          : ctx.panel_frame("frame", b, panel.title, panel.closable ? &close_clicked : nullptr);
    if (close_clicked) fired = "close";
    if (panel.tabs.empty()) return fired;

    Box body = inner;

    // Tabs, when there is more than one group to choose between.
    int tab = active_tab ? *active_tab : 0;
    if (panel.tabs.size() > 1) {
        std::vector<std::string> names;
        names.reserve(panel.tabs.size());
        for (const UiTab& t : panel.tabs) names.push_back(t.name);
        const float h = ctx.style.row_height + 6.0f;
        ctx.tab_bar("tabs", {inner.x, inner.y, inner.w, h}, names, &tab);
        body = {inner.x, inner.y + h + ctx.style.spacing, inner.w,
                std::max(0.0f, inner.h - h - ctx.style.spacing)};
    }
    tab = std::clamp(tab, 0, static_cast<int>(panel.tabs.size()) - 1);
    if (active_tab) *active_tab = tab;

    // Buttons take the bottom strip, so a long row list scrolls above them rather than
    // pushing them off the panel.
    float buttons_h = 0.0f;
    if (!panel.buttons.empty()) buttons_h = ctx.style.row_height + 14.0f;

    ctx.begin_region("rows", {body.x, body.y, body.w, std::max(0.0f, body.h - buttons_h)}, true);
    const auto& rows = panel.tabs[static_cast<size_t>(tab)].rows;
    for (size_t i = 0; i < rows.size(); ++i) {
        // Scoped per row, like the button strip below: a row widget's id comes from its
        // label, so two rows labelled the same -- or two unlabelled ones -- would otherwise
        // share hover and drag state.
        ctx.push_id(static_cast<int64_t>(i));
        const std::string a = draw_row(ctx, rows[i], bindings);
        ctx.pop_id();
        if (!a.empty()) fired = a;
    }
    ctx.end_region();

    if (buttons_h > 0.0f) {
        const float gap = ctx.style.spacing;
        const float w = (body.w - gap * static_cast<float>(panel.buttons.size() - 1)) /
                        static_cast<float>(panel.buttons.size());
        const float y = body.bottom() - buttons_h + 7.0f;
        for (size_t i = 0; i < panel.buttons.size(); ++i) {
            ctx.push_id(static_cast<int64_t>(i));
            const Box cell{body.x + static_cast<float>(i) * (w + gap), y, w, ctx.style.row_height};
            if (ctx.button(panel.buttons[i].label, cell)) fired = panel.buttons[i].action;
            ctx.pop_id();
        }
    }

    if (!fired.empty()) bindings.invoke(fired);
    return fired;
}

UiPanel parse_panel(const std::string& yaml_text) {
    return PanelLoader::parse_panel_node(fkyaml::node::deserialize(yaml_text));
}

void register_panel_loader(coopa::asset::AssetManager& assets) {
    assets.register_loader<UiPanel>(std::make_unique<PanelLoader>());
}

namespace {
coopa::asset::AssetManager* g_panel_assets = nullptr;
}  // namespace

void set_panel_assets(coopa::asset::AssetManager* assets) { g_panel_assets = assets; }

coopa::asset::AssetManager* panel_assets() { return g_panel_assets; }

std::vector<std::string> unbound_keys(const UiPanel& panel, const PanelBindings& bindings) {
    const std::vector<std::string>& engine_keys = core::engine_setting_keys();
    std::vector<std::string> out;
    auto check = [&](const UiRow& r) {
        if (r.bind.empty()) return;
        if (bindings.provided(r.bind)) return;
        if (std::find(engine_keys.begin(), engine_keys.end(), r.bind) != engine_keys.end()) return;
        if (std::find(out.begin(), out.end(), r.bind) != out.end()) return;
        out.push_back(r.bind);
    };
    for (const UiTab& t : panel.tabs) {
        for (const UiRow& r : t.rows) check(r);
    }
    for (const UiRow& r : panel.buttons) check(r);
    return out;
}

}  // namespace toy::ui
