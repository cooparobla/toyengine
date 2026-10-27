#include "editor/app/ui/ui_canvas.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

#include <coopa/scene/scene_loader.h>

#include "editor/app/editor_app.h"
#include "editor/schema/component_schema.h"
#include "toyengine/ui/imm_screen.h"
#include "toyengine/ui/panel_screen.h"

namespace toy {
namespace editor {
namespace {

using imm::Box;

/// The row kinds a panel can hold, in the order the Add menu offers them.
struct RowKindInfo {
    const char* id;
    const char* label;
    imm::Icon icon;
    bool binds;      ///< Has a value, so it needs a `bind` key.
};
constexpr RowKindInfo k_row_kinds[] = {
    {"slider",    "Slider",    imm::Icon::Tool,     true},
    {"toggle",    "Toggle",    imm::Icon::Check,    true},
    {"int",       "Number",    imm::Icon::Tool,     true},
    {"combo",     "Dropdown",  imm::Icon::ArrowDown, true},
    {"button",    "Button",    imm::Icon::UiButton, false},
    {"label",     "Label",     imm::Icon::UiText,   false},
    {"separator", "Separator", imm::Icon::Dots,     false},
    {"spacer",    "Spacer",    imm::Icon::UiLayout, false},
};

const RowKindInfo* kind_info(const std::string& id) {
    for (const RowKindInfo& k : k_row_kinds) {
        if (id == k.id) return &k;
    }
    return nullptr;
}

/// A new panel, by template id. Templates are deliberately small -- a starting point, not a
/// finished screen, because the finished one is what the author is about to write.
std::string panel_template(const std::string& id) {
    if (id == "settings") {
        return "title: Settings\nanchor: Center\nsize: [720, 520]\nclosable: true\n"
               "tabs:\n"
               "  - name: Audio\n"
               "    rows:\n"
               "      - { kind: slider, label: Master volume, bind: audio.master,\n"
               "          min: 0, max: 1, default: 1.0 }\n"
               "      - { kind: slider, label: Music, bind: audio.music,\n"
               "          min: 0, max: 1, default: 1.0 }\n"
               "  - name: Display\n"
               "    rows:\n"
               "      - { kind: slider, label: Interface scale, bind: ui.hud_scale,\n"
               "          min: 0.5, max: 2.0, step: 0.05, default: 1.0 }\n"
               "      - { kind: toggle, label: VSync, bind: display.vsync, default: true }\n"
               "buttons:\n  - { label: Close, action: close }\n";
    }
    if (id == "menu") {
        return "title: Menu\nanchor: Center\nsize: [360, 320]\n"
               "rows:\n"
               "  - { kind: button, label: Resume, action: resume }\n"
               "  - { kind: button, label: Quit, action: quit }\n";
    }
    return "title: Panel\nanchor: Center\nsize: [520, 400]\nclosable: true\n"
           "rows:\n"
           "  - { kind: label, label: A new panel }\n";
}

}  // namespace

UiCanvasPanel::~UiCanvasPanel() { release(); }

void UiCanvasPanel::release() {
    screen_ = nullptr;
    placed_ = nullptr;
    // The engine holds overlay layers NON-OWNING and updates them every frame
    // (Engine::add_overlay_layer's contract), so the layer has to go before the Scene does.
    if (preview_) app_.engine_.remove_overlay_layer(preview_.get());
    preview_.reset();
}

bool UiCanvasPanel::ui_mode() const { return open_ && app_.active_type_ == AssetType::UI; }

const std::vector<UiCanvasPanel::UiResolution>& UiCanvasPanel::ui_resolutions_() {
    static const std::vector<UiResolution> r = {
        {"1920 x 1080  (16:9)", 1920, 1080},   {"1280 x 720  (16:9)", 1280, 720},
        {"2560 x 1440  (16:9)", 2560, 1440},   {"3840 x 2160  (4K)", 3840, 2160},
        {"2560 x 1080  (21:9)", 2560, 1080},   {"1280 x 800  (16:10 handheld)", 1280, 800},
        {"1024 x 768  (4:3)", 1024, 768},      {"1080 x 1920  (portrait)", 1080, 1920},
    };
    return r;
}

std::vector<std::string> UiCanvasPanel::ui_templates() { return {"blank", "settings", "menu"}; }

// =====================================================================================
// Document
// =====================================================================================

bool UiCanvasPanel::is_placement_file(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    const std::string suffix = k_tune_suffix;
    return name.size() > suffix.size() &&
           name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> UiCanvasPanel::screen_types() {
    // The UI-category component schemas, which is exactly the set of ImmScreen subclasses the
    // engine registers. PanelScreen is excluded: its geometry belongs to the panel asset it
    // draws, so it declares no tunable boxes.
    std::vector<std::string> out;
    for (const auto& [name, schema] : schemas()) {
        if (schema.category == "UI" && name != "PanelScreen") out.push_back(name);
    }
    return out;
}

bool UiCanvasPanel::open_ui_asset(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) {
        app_.log_error("Open UI failed: " + path.string());
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();

    // A placement file names its screen in its own filename, which is what makes it
    // openable at all: the boxes come from running that screen, not from the file.
    if (is_placement_file(path)) {
        std::string stem = path.filename().string();
        stem.resize(stem.size() - std::string(k_tune_suffix).size());
        const std::vector<std::string> types = screen_types();
        if (std::find(types.begin(), types.end(), stem) == types.end()) {
            app_.log_error(app_.project_.relative(path) + ": no UI screen component named '" +
                           stem + "'");
            return false;
        }
        try {
            doc_ = ss.str().empty() ? Node::mapping() : Node::deserialize(ss.str());
        } catch (const std::exception& e) {
            app_.log_error(app_.project_.relative(path) + " will not parse (" + e.what() + ")");
            return false;
        }
        mode_ = Mode::Placement;
        screen_type_ = stem;
        box_ = -1;
        path_ = path;
        open_ = true;
        dirty_ = false;
        undo_.clear();
        redo_.clear();
        ui_fit_ = true;
        rebuild_preview_();
        return true;
    }

    try {
        // Parse once to check it is a panel at all; a file that will not load is not a
        // document the workspace should pretend to be editing.
        (void)toy::ui::parse_panel(ss.str());
        doc_ = Node::deserialize(ss.str());
    } catch (const std::exception& e) {
        app_.log_error(app_.project_.relative(path) + " is not a UI panel (" + e.what() + ")");
        return false;
    }
    mode_ = Mode::Panel;
    screen_type_.clear();
    path_ = path;
    open_ = true;
    dirty_ = false;
    undo_.clear();
    redo_.clear();
    tab_ = 0;
    row_ = -1;
    ui_fit_ = true;
    ui_refresh_preview_();
    return true;
}

bool UiCanvasPanel::new_ui_asset(const std::string& name, const std::string& template_id) {
    const std::filesystem::path dir = app_.project_.assets() / "ui";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path path = dir / (name + ".yaml");
    for (int i = 2; std::filesystem::exists(path, ec); ++i) {
        path = dir / (name + "_" + std::to_string(i) + ".yaml");
    }
    { std::ofstream(path) << panel_template(template_id); }
    app_.project_.refresh();
    return open_ui_asset(path);
}

bool UiCanvasPanel::new_placement_asset(const std::string& screen_type) {
    const std::vector<std::string> types = screen_types();
    if (std::find(types.begin(), types.end(), screen_type) == types.end()) {
        app_.log_error("No UI screen component named '" + screen_type + "'");
        return false;
    }
    const std::filesystem::path dir = app_.project_.assets() / "ui";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    // The name IS the binding: <Screen>.tune.yaml is what that screen looks for, so there is
    // no "which screen is this for" field to get wrong.
    const std::filesystem::path path = dir / (screen_type + k_tune_suffix);
    if (!std::filesystem::exists(path, ec)) {
        std::ofstream(path) << "# Where " << screen_type
                            << "'s panels sit. Empty means every box keeps the placement its\n"
                               "# own code states; edit it here or in the Placement tab.\n";
    }
    app_.project_.refresh();
    return open_ui_asset(path);
}

ObjectId UiCanvasPanel::place_ui_asset(const std::string& ui_rel, ObjectId parent) {
    // The scene links to a panel by reference, the way a MeshRenderer links to a mesh: one
    // object carrying a PanelScreen that names the asset.
    std::string ref = ui_rel;
    const size_t dot = ref.rfind(".yaml");
    if (dot != std::string::npos) ref.erase(dot);
    if (ref.rfind("assets/", 0) == 0) ref.erase(0, 7);

    Node obj = Node::mapping();
    obj["name"] = std::filesystem::path(ref).stem().string();
    Node comp = Node::mapping();
    comp["type"] = "PanelScreen";
    comp["panel"] = ref;
    Node comps = Node::sequence();
    comps.as_seq().push_back(std::move(comp));
    obj["components"] = std::move(comps);
    return app_.doc_.add_object(std::move(obj), parent);
}

void UiCanvasPanel::push_undo_() {
    undo_.push_back(doc_);
    if (undo_.size() > 64) undo_.erase(undo_.begin());
    redo_.clear();
    dirty_ = true;
}

void UiCanvasPanel::undo() {
    if (undo_.empty()) return;
    redo_.push_back(doc_);
    doc_ = undo_.back();
    undo_.pop_back();
    dirty_ = true;
    ui_refresh_preview_();
}

void UiCanvasPanel::redo() {
    if (redo_.empty()) return;
    undo_.push_back(doc_);
    doc_ = redo_.back();
    redo_.pop_back();
    dirty_ = true;
    ui_refresh_preview_();
}

void UiCanvasPanel::save() {
    if (!open_ || path_.empty()) return;
    if (mode_ == Mode::Placement) {
        // Tunables owns the file's shape, and refuses to write a set with nothing declared --
        // which is what stops a save before the screen has drawn from blanking the file.
        if (!placed_) {
            app_.log_error("Nothing to save: the screen is not running.");
            return;
        }
        if (!placed_->tune().save()) {
            app_.log_error("Could not write " + app_.project_.relative(path_) +
                           " (the screen has declared no boxes yet)");
            return;
        }
        dirty_ = false;
        app_.log_info("Saved " + app_.project_.relative(path_));
        return;
    }
    std::ofstream out(path_, std::ios::trunc);
    if (!out) {
        app_.log_error("Could not write " + path_.string());
        return;
    }
    out << doc_;
    dirty_ = false;
    app_.log_info("Saved " + app_.project_.relative(path_));
}

UiCanvasPanel::Node* UiCanvasPanel::rows_() {
    if (!open_ || !doc_.is_mapping()) return nullptr;
    if (doc_.contains("tabs") && doc_["tabs"].is_sequence() && !doc_["tabs"].empty()) {
        const int t = std::clamp(tab_, 0, static_cast<int>(doc_["tabs"].size()) - 1);
        Node& tab = doc_["tabs"][static_cast<size_t>(t)];
        if (!tab.contains("rows")) tab["rows"] = Node::sequence();
        return &tab["rows"];
    }
    if (!doc_.contains("rows")) doc_["rows"] = Node::sequence();
    return &doc_["rows"];
}

UiCanvasPanel::Node* UiCanvasPanel::selected_row_() {
    Node* rows = rows_();
    if (!rows || row_ < 0 || row_ >= static_cast<int>(rows->size())) return nullptr;
    return &(*rows)[static_cast<size_t>(row_)];
}

// =====================================================================================
// Preview
// =====================================================================================

void UiCanvasPanel::ensure_preview_() {
    if (preview_) return;
    rebuild_preview_();
}

void UiCanvasPanel::rebuild_preview_() {
    release();
    preview_ = std::make_unique<coopa::scene::Scene>("UiPreview");

    if (mode_ == Mode::Placement) {
        // The screen is built from its component key through the same parser registry a scene
        // uses, so the preview runs the real component -- which is the only way to learn which
        // boxes it declares. A fresh Scene simulates by default, so its behaviours run and
        // tune() fills in on the first frame.
        Node obj = Node::mapping();
        obj["name"] = std::string("Preview");
        Node comps = Node::sequence();
        Node c = Node::mapping();
        c["type"] = screen_type_;
        // Point the screen at the file being edited rather than the one its type name would
        // derive, so editing a copy under another name previews that copy.
        c["tunables_ref"] = app_.project_.relative(path_);
        comps.as_seq().push_back(std::move(c));
        obj["components"] = std::move(comps);
        try {
            std::unique_ptr<coopa::scene::SceneObject> built =
                coopa::scene::SceneLoader::build_object(obj, nullptr, path_.string(), true);
            coopa::scene::SceneObject* raw = preview_->add_root_object(std::move(built));
            preview_->adopt(*raw);
            preview_->start();
            placed_ = raw->get_component<toy::ui::ImmScreen>();
        } catch (const std::exception& e) {
            app_.log_error(std::string("UI preview failed: ") + e.what());
        }
    } else {
        auto obj = std::make_unique<coopa::scene::SceneObject>("Preview");
        screen_ = obj->add_component<toy::ui::PanelScreen>();
        coopa::scene::SceneObject* raw = preview_->add_root_object(std::move(obj));
        preview_->adopt(*raw);
        preview_->start();
    }
    app_.engine_.add_overlay_layer(preview_.get(), 90, /*in_display_rect=*/true, /*input=*/true);
}

void UiCanvasPanel::ui_refresh_preview_() {
    if (!open_) return;
    ensure_preview_();
    // Placement mode has no document to push: the preview runs the real screen and the edits
    // go straight into its tune(), which it reads on the next frame it draws.
    if (mode_ == Mode::Placement || !screen_) return;
    try {
        std::ostringstream ss;
        ss << doc_;
        screen_->set_panel(std::make_shared<toy::ui::UiPanel>(toy::ui::parse_panel(ss.str())));
    } catch (const std::exception&) {
        // Mid-edit the document can be briefly unparseable; keep showing the last good one
        // rather than blanking the preview under the author's hands.
    }
}

std::optional<core::Engine::ScreenUiPlacement> UiCanvasPanel::ui_preview_placement_() {
    if (ui_frame_.empty()) return std::nullopt;
    const float s = std::max(1.0f, app_.ui_scale_);
    core::Engine::ScreenUiPlacement p;
    p.rect.x = static_cast<int32_t>(std::lround(ui_frame_.x * s));
    p.rect.y = static_cast<int32_t>(std::lround(ui_frame_.y * s));
    p.rect.w = static_cast<uint32_t>(std::max(1L, std::lround(ui_frame_.w * s)));
    p.rect.h = static_cast<uint32_t>(std::max(1L, std::lround(ui_frame_.h * s)));
    p.zoom = static_cast<float>(p.rect.w) / std::max(1.0f, ui_res_.x);
    p.input = ui_interact_;
    return p;
}

void UiCanvasPanel::ui_set_interact_(bool on) {
    ui_interact_ = on;
    if (screen_) screen_->hidden = false;
}

// =====================================================================================
// View
// =====================================================================================

void UiCanvasPanel::ui_layout_frame_() {
    const Box vp = app_.viewport_box_;
    const float s = std::max(1.0f, app_.ui_scale_);
    if (ui_fit_ || ui_zoom_ <= 0.0f) {
        const float margin = 28.0f;
        ui_zoom_ = std::max(0.02f, std::min((vp.w - 2 * margin) * s / ui_res_.x,
                                            (vp.h - 2 * margin) * s / ui_res_.y));
        ui_pan_ = glm::vec2(0.0f);
        ui_fit_ = false;
    }
    const glm::vec2 size = ui_res_ * ui_zoom_ / s;
    const glm::vec2 c = vp.center() + ui_pan_;
    ui_frame_ = Box{std::round(c.x - size.x * 0.5f), std::round(c.y - size.y * 0.5f),
                    std::round(size.x), std::round(size.y)};
}

void UiCanvasPanel::ui_zoom_at_(glm::vec2 m, float factor) {
    const float s = std::max(1.0f, app_.ui_scale_);
    const float old_zoom = ui_zoom_;
    ui_zoom_ = std::clamp(ui_zoom_ * factor, 0.02f, 16.0f);
    const glm::vec2 old_size = ui_res_ * old_zoom / s, new_size = ui_res_ * ui_zoom_ / s;
    const glm::vec2 centre = app_.viewport_box_.center() + ui_pan_;
    const glm::vec2 rel = (m - centre) / old_size;
    ui_pan_ = (m - rel * new_size) - app_.viewport_box_.center();
}

void UiCanvasPanel::draw_ui_header_(imm::Context& ctx, const Box& b) {
    using I = imm::Icon;
    const bool placement = mode_ == Mode::Placement;
    ctx.icon(placement ? I::Move : I::UiCanvas, {b.x + 6, b.y + 5, b.h - 10, b.h - 10},
             ctx.style.text_dim);
    // A placement file's stem is "<Screen>.tune", which reads better as the screen's name.
    std::string stem = path_.empty() ? std::string("UI") : path_.stem().string();
    if (placement && stem.size() > 5 && stem.rfind(".tune") == stem.size() - 5) stem.resize(stem.size() - 5);
    const std::string name = stem + (dirty_ ? " *" : "");
    ctx.text_in({b.x + b.h + 2, b.y, 220, b.h}, name, ctx.style.text, 0.0f);
    if (placement) {
        ctx.text_in({b.x + b.h + 2 + 224, b.y, 160, b.h}, "placement", ctx.style.text_dim, 0.0f);
    }

    float x = b.right() - 30;
    const float s = b.h - 8;
    auto tool = [&](const char* id, I icon, const char* tip, bool on) {
        const bool hit = ctx.icon_button(id, icon, tip, on, s, imm::Context::kAll,
                                         Box{x, b.y + 4, s, s});
        x -= s + 2;
        return hit;
    };
    if (tool("ui_interact", I::MouseLeft, "Interact\nLet the preview take the pointer (Tab)",
             ui_interact_)) {
        ui_set_interact_(!ui_interact_);
    }
    if (tool("ui_fit", I::Zoom, "Fit\nFrame the preview (Home)", false)) ui_fit_ = true;
    if (tool("ui_backdrop", I::ShadeSolid, "Backdrop\nDark / light / the scene behind", false)) {
        ui_backdrop_ = (ui_backdrop_ + 1) % 3;
    }

    // Resolution: what a player's screen is, which is the thing a layout has to survive.
    const float w = 190.0f;
    x -= w;
    const std::vector<UiResolution>& res = ui_resolutions_();
    std::vector<std::string> labels;
    labels.reserve(res.size());
    int cur = 0;
    for (size_t i = 0; i < res.size(); ++i) {
        labels.emplace_back(res[i].label);
        if (static_cast<int>(ui_res_.x) == res[i].w && static_cast<int>(ui_res_.y) == res[i].h) {
            cur = static_cast<int>(i);
        }
    }
    if (ctx.combo_box("ui_res", Box{x, b.y + 4, w, s}, &cur, labels)) {
        ui_res_ = glm::vec2(static_cast<float>(res[static_cast<size_t>(cur)].w),
                            static_cast<float>(res[static_cast<size_t>(cur)].h));
        ui_fit_ = true;
    }
}

void UiCanvasPanel::handle_ui_input_(imm::Context& ctx) {
    const imm::FrameInput& in = ctx.input();
    const bool over = ctx.is_hovered(app_.viewport_box_);
    if (over && in.scroll.y != 0.0f && !ctx.any_popup_open()) {
        ui_zoom_at_(ctx.mouse(), in.scroll.y > 0 ? 1.1f : 1.0f / 1.1f);
    }
    // Middle-drag pans, as everywhere else in the editor.
    if (over && in.pressed[2]) ui_panning_ = true;
    if (!in.down[2]) ui_panning_ = false;
    if (ui_panning_) ui_pan_ += in.mouse_delta;
}

void UiCanvasPanel::draw_ui_overlays_(imm::Context& ctx) {
    // The frame's edge, so the author can see where a player's screen stops even when the
    // panel inside it is smaller.
    ctx.outline_rounded(ui_frame_, ctx.style.border, 0.0f, 1.0f);
    if (ui_interact_) {
        ctx.outline_rounded(ui_frame_, ctx.style.accent, 0.0f, 2.0f);
    }
}

void UiCanvasPanel::draw_ui_view_(imm::Context& ctx, const Box& area) {
    const Box hb = app_.area_header_(ctx, area);
    draw_ui_header_(ctx, hb);
    app_.viewport_box_ = Box{area.x, hb.bottom(), area.w, std::max(1.0f, area.h - hb.h)};
    ui_layout_frame_();

    ctx.push_clip(app_.viewport_box_);
    const glm::vec4 around = ui_backdrop_ == 1 ? glm::vec4(0.55f, 0.56f, 0.58f, 1.0f)
                                               : glm::vec4(0.10f, 0.10f, 0.11f, 1.0f);
    if (ui_backdrop_ != 2) {
        app_.fill_around_(ctx, app_.viewport_box_, ui_frame_.intersect(app_.viewport_box_), around);
    }
    handle_ui_input_(ctx);
    draw_ui_overlays_(ctx);
    ctx.pop_clip();
}

// =====================================================================================
// The row list, and the inspectors
// =====================================================================================

void UiCanvasPanel::draw_ui_palette_(imm::Context& ctx, const Box& area) {
    const bool placement = mode_ == Mode::Placement;
    const Box hb = app_.area_header_(ctx, area);
    ctx.text_in({hb.x + 8, hb.y, hb.w - 90, hb.h}, placement ? "Boxes" : "Rows", ctx.style.text, 0.0f);
    const float s = hb.h - 8;
    // Placement has nothing to add: the screen's code decides which boxes exist.
    if (!placement &&
        ctx.icon_button("row_add", imm::Icon::Plus, "Add a row", false, s, imm::Context::kAll,
                        Box{hb.right() - s - 6, hb.y + 4, s, s})) {
        ctx.open_popup("row_kinds");
    }

    const Box body = app_.area_(ctx, Box{area.x, hb.bottom(), area.w, area.h - hb.h});
    ctx.begin_region("ui_rows", body, true);
    if (!open_) {
        ctx.label_dim("No panel open.");
        ctx.end_region();
        return;
    }
    if (placement) {
        draw_placement_list_(ctx);
        ctx.end_region();
        return;
    }

    // Tabs, when the panel has them: which tab's rows are being edited.
    if (doc_.contains("tabs") && doc_["tabs"].is_sequence() && doc_["tabs"].size() > 1) {
        std::vector<std::string> names;
        for (const auto& t : doc_["tabs"]) {
            names.push_back(t.contains("name") ? t["name"].get_value<std::string>() : "Tab");
        }
        int t = tab_;
        if (ctx.combo("Tab", &t, names)) {
            tab_ = t;
            row_ = -1;
        }
        ctx.spacing();
    }

    Node* rows = rows_();
    if (rows) {
        for (size_t i = 0; i < rows->size(); ++i) {
            const Node& r = (*rows)[i];
            const std::string kind = r.contains("kind") ? r["kind"].get_value<std::string>() : "label";
            const std::string label = r.contains("label") ? r["label"].get_value<std::string>() : "";
            const RowKindInfo* info = kind_info(kind);
            ctx.push_id(static_cast<int64_t>(i));
            const bool sel = row_ == static_cast<int>(i);
            if (ctx.selectable(label.empty() ? kind : label, sel, 0.0f,
                               info ? info->icon : imm::Icon::Dots)) {
                row_ = static_cast<int>(i);
            }
            ctx.pop_id();
        }
        if (rows->empty()) ctx.label_dim("No rows yet.");
    }

    // Reorder, duplicate, remove. A list you can only append to is not an editor: order is
    // the layout here, since rows draw top to bottom in the order the file states them.
    if (selected_row_()) {
        ctx.spacing();
        ctx.separator();
        if (ctx.button("Up")) move_row_(-1);
        ctx.same_line();
        if (ctx.button("Down")) move_row_(1);
        ctx.same_line();
        if (ctx.button("Copy")) duplicate_row_();
        ctx.same_line();
        if (ctx.button("Delete")) remove_row_();
    }
    ctx.end_region();

    if (ctx.begin_popup("row_kinds", 180)) {
        for (const RowKindInfo& k : k_row_kinds) {
            if (!ctx.menu_item(k.label, "", nullptr, true, k.icon)) continue;
            push_undo_();
            Node row = Node::mapping();
            row["kind"] = k.id;
            if (k.id != std::string("separator") && k.id != std::string("spacer")) {
                row["label"] = std::string("New ") + k.label;
            }
            if (k.binds) row["bind"] = "";
            if (k.id == std::string("combo")) row["options"] = Node::sequence();
            if (k.id == std::string("button")) row["action"] = "";
            if (Node* rs = rows_()) {
                rs->as_seq().push_back(std::move(row));
                row_ = static_cast<int>(rs->size()) - 1;
            }
            ui_refresh_preview_();
            ctx.close_current_popup();
        }
        ctx.end_popup();
    }
}

// =====================================================================================
// Row operations
// =====================================================================================

void UiCanvasPanel::move_row_(int delta) {
    Node* rows = rows_();
    if (!rows || !selected_row_()) return;
    const int n = static_cast<int>(rows->size());
    const int to = row_ + delta;
    if (to < 0 || to >= n) return;
    push_undo_();
    std::swap(rows->as_seq()[static_cast<size_t>(row_)], rows->as_seq()[static_cast<size_t>(to)]);
    row_ = to;
    ui_refresh_preview_();
}

void UiCanvasPanel::duplicate_row_() {
    Node* rows = rows_();
    Node* row = selected_row_();
    if (!rows || !row) return;
    push_undo_();
    const Node copy = *row;
    rows->as_seq().insert(rows->as_seq().begin() + row_ + 1, copy);
    ++row_;
    ui_refresh_preview_();
}

void UiCanvasPanel::remove_row_() {
    Node* rows = rows_();
    if (!rows || !selected_row_()) return;
    push_undo_();
    rows->as_seq().erase(rows->as_seq().begin() + row_);
    // Keep a neighbour selected rather than nothing, so deleting a run of rows does not need
    // a re-click between each one.
    row_ = std::min(row_, static_cast<int>(rows->size()) - 1);
    ui_refresh_preview_();
}

// =====================================================================================
// Placement: a screen's boxes, as numbers
// =====================================================================================

void UiCanvasPanel::draw_placement_list_(imm::Context& ctx) {
    if (!placed_) {
        ctx.label_dim("Screen not running.");
        return;
    }
    const std::vector<toy::ui::Tunable>& boxes = placed_->tune().declared();
    if (boxes.empty()) {
        // A screen only declares a box when it DRAWS it, so an empty list before the first
        // frame is normal -- and a screen that asks for none has nothing to place.
        ctx.label_dim("No boxes yet.");
        ctx.label_dim("They appear as the screen draws them.");
        return;
    }
    for (size_t i = 0; i < boxes.size(); ++i) {
        ctx.push_id(static_cast<int64_t>(i));
        if (ctx.selectable(boxes[i].name, box_ == static_cast<int>(i), 0.0f, imm::Icon::Move)) {
            box_ = static_cast<int>(i);
        }
        ctx.pop_id();
    }
}

void UiCanvasPanel::draw_placement_props_(imm::Context& ctx) {
    if (!placed_) {
        ctx.label_dim("Screen not running.");
        return;
    }
    const std::vector<toy::ui::Tunable>& boxes = placed_->tune().declared();
    if (box_ < 0 || box_ >= static_cast<int>(boxes.size())) {
        ctx.label_dim("Select a box.");
        return;
    }
    const toy::ui::Tunable cur = boxes[static_cast<size_t>(box_)];
    ctx.label_dim(cur.name);
    ctx.separator();

    // Every edit goes through Tunables::set(), so the screen picks it up on its next frame and
    // the preview moves under the cursor. Nothing is written until Save.
    auto apply = [&](coopa::ui::imm::Anchor a, glm::vec2 pos, glm::vec2 size) {
        push_undo_();
        placed_->tune().set(cur.name, a, pos, size);
        dirty_ = true;
    };

    const std::vector<std::string>& anchors = toy::ui::anchor_names();
    int idx = 0;
    for (size_t i = 0; i < anchors.size(); ++i) {
        if (anchors[i] == toy::ui::anchor_name(cur.anchor)) idx = static_cast<int>(i);
    }
    if (ctx.combo("Anchor", &idx, anchors)) {
        coopa::ui::imm::Anchor a = cur.anchor;
        if (toy::ui::parse_anchor(anchors[static_cast<size_t>(idx)], a)) apply(a, cur.pos, cur.size);
    }

    glm::vec2 pos = cur.pos;
    if (ctx.drag_floatn("Position", &pos.x, 2, 1.0f, "%.0f")) apply(cur.anchor, pos, cur.size);
    ctx.tooltip("In UI space (+Y down). Moves the box, not the anchor -- so a right or bottom "
                "anchor insets with a negative value.");

    glm::vec2 size = cur.size;
    if (ctx.drag_floatn("Size", &size.x, 2, 1.0f, "%.0f")) apply(cur.anchor, pos, size);

    ctx.spacing();
    if (ctx.button("Reset to code default")) {
        // Dropping the entry is what "use the code's default" means: box() re-declares it from
        // the screen's own arguments the next time it draws.
        push_undo_();
        placed_->tune().forget(cur.name);
        box_ = -1;
        dirty_ = true;
    }
    ctx.tooltip("Forgets this entry, so the screen's own anchor/pos/size applies again.");
}

void UiCanvasPanel::draw_ui_canvas_props_(imm::Context& ctx) {
    if (!open_) {
        ctx.label_dim("No panel open.");
        return;
    }
    auto edit_string = [&](const char* key, const char* label) {
        std::string v = doc_.contains(key) ? doc_[key].get_value<std::string>() : "";
        if (ctx.input_text(label, &v)) {
            push_undo_();
            doc_[key] = v;
            ui_refresh_preview_();
        }
    };
    edit_string("title", "Title");
    edit_string("theme", "Theme");

    bool closable = doc_.contains("closable") && doc_["closable"].get_value<bool>();
    if (ctx.property_bool("Closable", &closable)) {
        push_undo_();
        doc_["closable"] = closable;
        ui_refresh_preview_();
    }

    // Anchor and size: where the panel sits on whatever screen it ends up on.
    const std::vector<std::string>& anchors = toy::ui::anchor_names();
    std::string cur = doc_.contains("anchor") ? doc_["anchor"].get_value<std::string>() : "Center";
    int idx = 0;
    for (size_t i = 0; i < anchors.size(); ++i) {
        if (anchors[i] == cur) idx = static_cast<int>(i);
    }
    if (ctx.combo("Anchor", &idx, anchors)) {
        push_undo_();
        doc_["anchor"] = anchors[static_cast<size_t>(idx)];
        ui_refresh_preview_();
    }

    auto edit_vec2 = [&](const char* key, const char* label, glm::vec2 def) {
        glm::vec2 v = def;
        if (doc_.contains(key) && doc_[key].is_sequence() && doc_[key].size() >= 2) {
            v = glm::vec2(static_cast<float>(doc_[key][0].get_value<double>()),
                          static_cast<float>(doc_[key][1].get_value<double>()));
        }
        if (ctx.drag_floatn(label, &v.x, 2, 1.0f, "%.0f")) {
            push_undo_();
            Node seq = Node::sequence();
            seq.as_seq().push_back(Node(static_cast<double>(v.x)));
            seq.as_seq().push_back(Node(static_cast<double>(v.y)));
            doc_[key] = std::move(seq);
            ui_refresh_preview_();
        }
    };
    edit_vec2("size", "Size", glm::vec2(640.0f, 480.0f));
    edit_vec2("offset", "Offset", glm::vec2(0.0f));

    // A bound key with no consumer is the one authoring mistake that cannot be seen: the row
    // reads and writes UserSettings either way, so it looks like it works, and it permanently
    // adds the key to the player's settings file. Say so here, where the panel is edited.
    if (screen_) {
        const toy::ui::UiPanel* p = screen_->asset();
        if (p) {
            const std::vector<std::string> dead = toy::ui::unbound_keys(*p, screen_->bindings());
            if (!dead.empty()) {
                ctx.spacing();
                ctx.separator();
                ctx.label("Keys nothing reads:", &ctx.style.warning);
                for (const std::string& k : dead) ctx.label_dim("  " + k);
                ctx.tooltip("Either the engine consumes this key (toy::core::engine_setting_keys) "
                            "or game code provides it (PanelScreen::bindings().provide). Neither "
                            "is true for these, so the row will do nothing at runtime.");
            }
        }
    }
}

void UiCanvasPanel::draw_ui_object_props_(imm::Context& ctx) {
    if (mode_ == Mode::Placement) {
        draw_placement_props_(ctx);
        return;
    }
    Node* row = selected_row_();
    if (!row) {
        ctx.label_dim(open_ ? "Select a row." : "No panel open.");
        return;
    }

    const std::string kind = row->contains("kind") ? (*row)["kind"].get_value<std::string>() : "label";
    const RowKindInfo* info = kind_info(kind);
    ctx.label_dim(info ? info->label : kind);
    ctx.separator();

    auto edit_string = [&](const char* key, const char* label) {
        std::string v = row->contains(key) ? (*row)[key].get_value<std::string>() : "";
        if (ctx.input_text(label, &v)) {
            push_undo_();
            (*row)[key] = v;
            ui_refresh_preview_();
        }
    };
    auto edit_float = [&](const char* key, const char* label, float def) {
        float v = def;
        if (row->contains(key)) {
            const Node& n = (*row)[key];
            if (n.is_integer()) v = static_cast<float>(n.get_value<int64_t>());
            else if (n.is_float_number()) v = static_cast<float>(n.get_value<double>());
        }
        if (ctx.drag_float(label, &v, 0.01f)) {
            push_undo_();
            (*row)[key] = static_cast<double>(v);
            ui_refresh_preview_();
        }
    };

    if (kind != "separator" && kind != "spacer") edit_string("label", "Label");
    if (info && info->binds) {
        edit_string("bind", "Bind");
        ctx.tooltip("The value's key. Falls through to UserSettings unless game code provides it.");
    }
    if (kind == "button") edit_string("action", "Action");
    if (kind == "slider" || kind == "int") {
        edit_float("min", "Min", 0.0f);
        edit_float("max", "Max", 1.0f);
        edit_float("step", "Step", 0.0f);
        ctx.tooltip("Quantum the value snaps to. 0 does not quantize.");
        edit_float("default", "Default", 0.0f);
        ctx.tooltip("What the row shows before anything has written its key. Without one a "
                    "slider starts at Min, which reads as zero volume on a fresh install.");
    }
    if (kind == "toggle") {
        bool on = row->contains("default") && (*row)["default"].is_boolean() &&
                  (*row)["default"].get_value<bool>();
        if (ctx.property_bool("Default", &on)) {
            push_undo_();
            (*row)["default"] = on;
            ui_refresh_preview_();
        }
        ctx.tooltip("Without one a toggle can only ever default to off.");
    }
    if (kind == "combo") {
        edit_string("default", "Default");
        ctx.tooltip("An option NAME. The value is stored by name, so inserting an option does "
                    "not change what an existing setting means.");
    }
    if (kind != "separator" && kind != "spacer") edit_string("tooltip", "Tooltip");

    ctx.spacing();
    ctx.separator();
    Node* rows = rows_();
    const int n = rows ? static_cast<int>(rows->size()) : 0;
    const float w = ctx.available_width() / 3.0f - ctx.style.spacing;
    if (ctx.button("Up", w, row_ > 0)) {
        push_undo_();
        std::swap((*rows)[static_cast<size_t>(row_)], (*rows)[static_cast<size_t>(row_ - 1)]);
        --row_;
        ui_refresh_preview_();
    }
    ctx.same_line();
    if (ctx.button("Down", w, row_ + 1 < n)) {
        push_undo_();
        std::swap((*rows)[static_cast<size_t>(row_)], (*rows)[static_cast<size_t>(row_ + 1)]);
        ++row_;
        ui_refresh_preview_();
    }
    ctx.same_line();
    if (ctx.button("Delete", w)) {
        push_undo_();
        rows->as_seq().erase(rows->as_seq().begin() + row_);
        row_ = -1;
        ui_refresh_preview_();
    }
}

void UiCanvasPanel::ui_store_editor_extras_() {}

}  // namespace editor
}  // namespace toy
