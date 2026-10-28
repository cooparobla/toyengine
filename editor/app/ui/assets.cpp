#include "editor/app/ui/assets.h"
#include "editor/app/ui/ui_canvas.h"

#include "editor/app/editor_app.h"
#include "editor/app/ui/theme_editor.h"
#include "editor/app/ui/console_panel.h"

#include <sfxcoopa/core/engine.h>   // PlayParams, for the audio preview

namespace toy {
namespace editor {

bool AssetsPanel::show_engine_assets() const {
    return app_.show_engine_assets_;
}

std::vector<std::string>& AssetsPanel::tag_filter_() {
    return app_.asset_tag_filter_[app_.asset_tab_];
}

const std::vector<AssetsPanel::AssetTypeInfo>& AssetsPanel::asset_types_() {
    using I = imm::Icon;
    static const std::vector<AssetTypeInfo> t = {
        {AssetType::Scene, "Scenes", "Scene", I::Scene, "scenes"},
        {AssetType::Object, "Objects", "Object", I::Object, "objects"},
        {AssetType::Mesh, "Meshes", "Mesh", I::Mesh, "meshes"},
        {AssetType::Material, "Materials", "Material", I::Material, "materials"},
        {AssetType::Texture, "Textures", "Texture", I::Image, "textures"},
        {AssetType::UI, "UI", "UI", I::UiCanvas, "ui"},
        {AssetType::Theme, "Themes", "Theme", I::Palette, "ui/themes"},
        {AssetType::Audio, "Audio", "Sound", I::Play, "audio"},
    };
    return t;
}

const AssetsPanel::AssetTypeInfo& AssetsPanel::asset_type_info_(AssetType t) {
    for (const auto& i : asset_types_()) if (i.type == t) return i;
    return asset_types_()[0];
}

std::vector<std::string> AssetsPanel::list_assets_(AssetType t) {
    switch (t) {
        case AssetType::Scene: return app_.project_.scenes();
        case AssetType::Object: return app_.project_.list("objects", ".yaml");
        case AssetType::Mesh: return app_.project_.list("meshes", ".yaml");
        case AssetType::Material: return app_.project_.list("materials", ".yaml");
        case AssetType::UI: {
            // ui/themes/ holds themes, not canvases.
            std::vector<std::string> out;
            for (const auto& p : app_.project_.list("ui", ".yaml")) if (p.rfind("ui/themes/", 0) != 0) out.push_back(p);
            return out;
        }
        case AssetType::Theme: return app_.project_.list("ui/themes", ".yaml");
        case AssetType::Audio: {
            std::vector<std::string> out;
            for (const char* dir : {"audio", "sounds"}) {
                for (const auto& p : app_.project_.list(dir, "")) if (is_audio_ext_(fs::path(p).extension().string())) out.push_back(p);
            }
            return out;
        }
        case AssetType::Texture: {
            std::vector<std::string> out;
            for (const auto& p : app_.project_.list("textures", "")) {
                const std::string e = fs::path(p).extension().string();
                if (e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga") out.push_back(p);
            }
            return out;
        }
        default: return {};
    }
}

std::vector<std::string> AssetsPanel::list_engine_assets_(AssetType t) {
    if (!app_.show_engine_assets_ || app_.project_.is_engine()) return {};
    switch (t) {
        case AssetType::Scene: return app_.project_.engine_scenes();
        case AssetType::Object: return app_.project_.list_engine("objects", ".yaml");
        case AssetType::Mesh: return app_.project_.list_engine("meshes", ".yaml");
        case AssetType::Material: return app_.project_.list_engine("materials", ".yaml");
        case AssetType::UI: {
            std::vector<std::string> out;
            for (const auto& p : app_.project_.list_engine("ui", ".yaml")) if (p.rfind("ui/themes/", 0) != 0) out.push_back(p);
            return out;
        }
        case AssetType::Theme: return app_.project_.list_engine("ui/themes", ".yaml");
        case AssetType::Audio: {
            std::vector<std::string> out;
            for (const char* dir : {"audio", "sounds"}) {
                for (const auto& p : app_.project_.list_engine(dir, "")) if (is_audio_ext_(fs::path(p).extension().string())) out.push_back(p);
            }
            return out;
        }
        case AssetType::Texture: {
            std::vector<std::string> out;
            for (const auto& p : app_.project_.list_engine("textures", "")) {
                const std::string e = fs::path(p).extension().string();
                if (e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga") out.push_back(p);
            }
            return out;
        }
        default: return {};
    }
}

std::vector<std::string> AssetsPanel::material_choices_() {
    std::vector<std::string> out = list_assets_(AssetType::Material);
    for (const auto& m : list_engine_assets_(AssetType::Material)) out.push_back(m);
    return out;
}

void AssetsPanel::set_show_engine_assets(bool on) {
    app_.show_engine_assets_ = on;
    Node prefs = Project::load_prefs();
    prefs["show_engine_assets"] = Node(on);
    Project::save_prefs(prefs);
}

bool AssetsPanel::copy_engine_asset_to_project(AssetType t, const std::string& rel) {
    fs::path src = coopa::yaml::resolve_variant(Project::engine_assets() / rel);
    std::error_code ec;
    if (!fs::exists(src, ec)) {   // a short reference: toyengine keeps it in a tag folder
        if (auto found = coopa::asset::AssetIndex::find(Project::engine_assets(), rel)) src = *found;
    }
    if (!fs::exists(src, ec)) { app_.log_error("Not a toyengine asset: " + rel); return false; }
    const fs::path dst = app_.project_.assets() / fs::relative(src, Project::engine_assets(), ec);
    if (t == AssetType::Scene && src.filename().string().rfind("scene", 0) == 0) {
        fs::create_directories(dst.parent_path().parent_path(), ec);
        fs::copy(src.parent_path(), dst.parent_path(), fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
    } else {
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(src, dst, fs::copy_options::skip_existing, ec);
        // A mesh's LOD sidecar travels with it.
        const fs::path lod = src.parent_path() / (src.stem().string() + ".lod" + src.extension().string());
        if (!ec && fs::exists(lod)) fs::copy_file(lod, dst.parent_path() / lod.filename(), fs::copy_options::skip_existing, ec);
    }
    app_.project_.refresh();
    if (ec) { app_.log_error("Copy to Project failed: " + ec.message()); return false; }
    app_.log_info("Copied " + rel + " into the project -- this copy is editable and now replaces toyengine's");
    open_asset(t, app_.project_.relative(dst));
    return true;
}

bool AssetsPanel::refuse_engine_asset_(const fs::path& abs) {
    if (!app_.project_.is_engine_path(abs)) return false;
    app_.log_warn(app_.project_.relative(abs) + " is a read-only toyengine asset: drag it into a scene to use it, or right-click > "
             "Copy to Project to edit it");
    return true;
}

bool AssetsPanel::is_audio_ext_(std::string ext) {
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".wav" || ext == ".mp3";
}

AssetType AssetsPanel::asset_type_of_(const std::string& rel) {
    const fs::path p(rel);
    const std::string ext = p.extension().string();
    const std::string g = p.generic_string();
    if (is_audio_ext_(ext)) return AssetType::Audio;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") return AssetType::Texture;
    if (g.rfind("objects/", 0) == 0) return AssetType::Object;
    if (g.rfind("ui/themes/", 0) == 0) return AssetType::Theme;
    if (g.rfind("ui/", 0) == 0) return AssetType::UI;
    if (g.rfind("materials/", 0) == 0) return AssetType::Material;
    if (g.find("meshes/") != std::string::npos) return AssetType::Mesh;
    if (g.rfind("scenes/", 0) == 0) return AssetType::Scene;
    return AssetType::None;
}

std::string AssetsPanel::asset_display_name_(AssetType t, const std::string& rel) {
    const fs::path p(rel);
    if (t == AssetType::Scene && p.filename().string().rfind("scene", 0) == 0 && p.has_parent_path()) {
        return p.parent_path().filename().string();
    }
    return p.stem().string();
}

bool AssetsPanel::asset_name_taken_(const std::string& type, const std::string& name, const std::string& except_rel) {
    if (type == "scenes") {
        for (const auto& rel : app_.project_.scenes()) {
            if (rel != except_rel && asset_display_name_(AssetType::Scene, rel) == name) return true;
        }
        return false;
    }
    for (const auto& rel : app_.project_.list(type, "")) {
        if (rel == except_rel) continue;
        if (type == "ui" && rel.rfind("ui/themes/", 0) == 0) continue;
        if (fs::path(rel).stem().string() == name) return true;
    }
    return false;
}

std::string AssetsPanel::new_asset_dir_(const std::string& dir) const {
    std::string d = dir;
    for (const auto& t : app_.creating_tags_) d += "/" + t;
    return d;
}

std::string AssetsPanel::clean_tag_(const std::string& raw) {
    const std::string t = snake_case(raw);
    // `meshes` would read as a scene's local mesh folder, `themes` as the theme folder.
    if (t.empty() || t == "meshes" || t == "themes") return "";
    return t;
}

std::map<std::string, int> AssetsPanel::tag_counts_(const std::vector<std::string>& items) {
    std::map<std::string, int> out;
    for (const auto& rel : items) {
        for (const auto& t : asset_tags(rel)) ++out[t];
    }
    return out;
}

bool AssetsPanel::passes_tag_filter_(const std::string& rel) const {
    const auto it = app_.asset_tag_filter_.find(app_.asset_tab_);
    if (it == app_.asset_tag_filter_.end() || it->second.empty()) return true;
    const auto tags = asset_tags(rel);
    auto has = [&](const std::string& t) { return std::find(tags.begin(), tags.end(), t) != tags.end(); };
    if (app_.asset_tag_match_all_) return std::all_of(it->second.begin(), it->second.end(), has);
    return std::any_of(it->second.begin(), it->second.end(), has);
}

void AssetsPanel::toggle_tag_filter_(const std::string& tag) {
    auto& f = tag_filter_();
    const auto it = std::find(f.begin(), f.end(), tag);
    if (it == f.end()) f.push_back(tag);
    else f.erase(it);
}

AssetStat AssetsPanel::asset_stat_(const std::string& rel) {
    const auto now = std::chrono::steady_clock::now();
    if (now - app_.asset_stats_time_ > std::chrono::seconds(2)) {
        app_.asset_stats_.clear();
        app_.asset_stats_time_ = now;
    }
    auto it = app_.asset_stats_.find(rel);
    if (it != app_.asset_stats_.end()) return it->second;
    AssetStat st;
    std::error_code ec;
    const fs::path p = coopa::yaml::resolve_variant(app_.project_.absolute(rel));
    const auto t = fs::last_write_time(p, ec);
    if (!ec) st.modified = std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
    const auto size = fs::file_size(p, ec);
    if (!ec) st.size = size;
    return app_.asset_stats_[rel] = st;
}

const char* AssetsPanel::asset_sort_name_(AssetSort s) {
    switch (s) {
        case AssetSort::Modified: return "modified";
        case AssetSort::Tags: return "tags";
        case AssetSort::Size: return "size";
        default: return "name";
    }
}

AssetSort AssetsPanel::asset_sort_from_name_(const std::string& n) {
    if (n == "modified") return AssetSort::Modified;
    if (n == "tags") return AssetSort::Tags;
    if (n == "size") return AssetSort::Size;
    return AssetSort::Name;
}

void AssetsPanel::set_asset_sort_(AssetSort s, bool reverse) {
    app_.asset_sort_ = s;
    app_.asset_sort_reverse_ = reverse;
    Node prefs = Project::load_prefs();
    prefs["asset_sort"] = Node(std::string(asset_sort_name_(s)));
    prefs["asset_sort_reverse"] = Node(reverse);
    Project::save_prefs(prefs);
}

void AssetsPanel::sort_assets_(AssetType t, std::vector<std::string>& items) {
    auto lower = [](std::string v) { std::transform(v.begin(), v.end(), v.begin(), ::tolower); return v; };
    std::vector<std::pair<std::string, std::string>> keyed;   // (name key, rel)
    keyed.reserve(items.size());
    for (const auto& rel : items) keyed.push_back({lower(asset_display_name_(t, rel)), rel});
    auto by_name = [](const auto& a, const auto& b) { return a.first != b.first ? a.first < b.first : a.second < b.second; };
    switch (app_.asset_sort_) {
        case AssetSort::Modified:
            for (const auto& k : keyed) asset_stat_(k.second);
            std::stable_sort(keyed.begin(), keyed.end(), [&](const auto& a, const auto& b) {
                const int64_t ma = app_.asset_stats_[a.second].modified, mb = app_.asset_stats_[b.second].modified;
                return ma != mb ? ma > mb : by_name(a, b);
            });
            break;
        case AssetSort::Size:
            for (const auto& k : keyed) asset_stat_(k.second);
            std::stable_sort(keyed.begin(), keyed.end(), [&](const auto& a, const auto& b) {
                const uintmax_t sa = app_.asset_stats_[a.second].size, sb = app_.asset_stats_[b.second].size;
                return sa != sb ? sa > sb : by_name(a, b);
            });
            break;
        case AssetSort::Tags: {
            auto tag_key = [](const std::string& rel) {
                std::string k;
                for (const auto& tg : asset_tags(rel)) k += tg + "/";
                return k;
            };
            std::stable_sort(keyed.begin(), keyed.end(), [&](const auto& a, const auto& b) {
                const std::string ta = tag_key(a.second), tb = tag_key(b.second);
                return ta != tb ? ta < tb : by_name(a, b);
            });
            break;
        }
        default: std::sort(keyed.begin(), keyed.end(), by_name); break;
    }
    if (app_.asset_sort_reverse_) std::reverse(keyed.begin(), keyed.end());
    for (size_t i = 0; i < keyed.size(); ++i) items[i] = keyed[i].second;
}

glm::vec4 AssetsPanel::tag_color_(const std::string& tag) {
    uint32_t h = 2166136261u;
    for (unsigned char c : tag) h = (h ^ c) * 16777619u;
    const float hue = static_cast<float>(h % 360u) / 60.0f;
    const float c = 0.42f, x = c * (1.0f - std::fabs(std::fmod(hue, 2.0f) - 1.0f)), m = 0.22f;
    glm::vec3 rgb = hue < 1 ? glm::vec3(c, x, 0) : hue < 2 ? glm::vec3(x, c, 0) : hue < 3 ? glm::vec3(0, c, x)
                  : hue < 4 ? glm::vec3(0, x, c) : hue < 5 ? glm::vec3(x, 0, c) : glm::vec3(c, 0, x);
    return glm::vec4(rgb + glm::vec3(m), 0.55f);
}

std::string AssetsPanel::draw_tag_chips_(imm::Context& ctx, const imm::Box& row, const std::vector<std::string>& tags, float min_x, bool clicked) {
    std::string hit;
    float x = row.right() - 4;
    const float h = row.h - 8;
    auto chip = [&](const std::string& text, const glm::vec4& col) -> imm::Box {
        const float w = ctx.text_width(text) + 10;
        const imm::Box b{x - w, row.y + 4, w, h};
        ctx.fill_rounded(b, col, h * 0.5f);
        ctx.text_in(b, text, ctx.style.text, 0.0f, true);
        x -= w + 3;
        return b;
    };
    for (size_t i = tags.size(); i-- > 0;) {
        const float need = ctx.text_width(tags[i]) + 13 + (i > 0 ? ctx.text_width("+" + std::to_string(i)) + 13 : 0);
        if (x - need < min_x) {
            const std::string more = "+" + std::to_string(i + 1);
            if (x - ctx.text_width(more) - 10 >= min_x) chip(more, imm::with_alpha(ctx.style.text_dim, 0.35f));
            break;
        }
        const imm::Box b = chip(tags[i], tag_color_(tags[i]));
        if (clicked && b.contains(ctx.mouse())) hit = tags[i];
    }
    return hit;
}

void AssetsPanel::draw_tag_editor_(imm::Context& ctx, std::vector<std::string>& tags, const std::vector<std::string>& known) {
    using I = imm::Icon;
    if (tags.empty()) ctx.label_dim("No tags");
    for (size_t i = 0; i < tags.size(); ++i) {
        ctx.push_id(static_cast<int64_t>(i));
        const imm::Box r = ctx.next_box(ctx.style.row_height);
        ctx.fill_rounded({r.x, r.y + 2, r.w, r.h - 4}, tag_color_(tags[i]), 4);
        ctx.icon(I::Tag, {r.x + 4, r.y + 4, r.h - 8, r.h - 8}, ctx.style.text);
        ctx.text_in({r.x + r.h, r.y, r.w - 2 * r.h, r.h}, tags[i], ctx.style.text, 0.0f);
        const float s = r.h - 6;
        if (ctx.icon_button("tag_remove", I::X, "Remove tag", false, s, imm::Context::kAll, imm::Box{r.right() - s - 3, r.y + 3, s, s})) {
            tags.erase(tags.begin() + static_cast<std::ptrdiff_t>(i));
            ctx.pop_id();
            break;
        }
        app_.test_rects_["tag_remove:" + tags[i]] = imm::Box{r.right() - s - 3, r.y + 3, s, s};
        ctx.pop_id();
    }
    bool header = false;
    for (const auto& k : known) {
        if (std::find(tags.begin(), tags.end(), k) != tags.end()) continue;
        if (!header) { ctx.spacing(2); ctx.label_dim("Add a tag"); header = true; }
        ctx.push_id("known:" + k);
        if (ctx.selectable(k, false, 0, I::Plus)) tags.push_back(k);
        app_.test_rects_["tag_add:" + k] = ctx.last_rect();
        ctx.pop_id();
    }
    ctx.spacing(2);
    const imm::Box row = ctx.next_box(22);
    const float bw = 52;
    const imm::Box field{row.x, row.y, row.w - bw - 4, row.h};
    ctx.input_text_box("tag_new", field, &app_.asset_tag_input_, "New tag");
    app_.test_rects_["tag_new_field"] = field;
    const imm::Box add{field.right() + 4, row.y, bw, row.h};
    bool hov = false, held = false;
    const std::string clean = clean_tag_(app_.asset_tag_input_);
    if (ctx.invisible_button("tag_new_add", add, &hov, &held) && !clean.empty()) {
        if (std::find(tags.begin(), tags.end(), clean) == tags.end()) tags.push_back(clean);
        app_.asset_tag_input_.clear();
    }
    ctx.fill_rounded(add, hov && !clean.empty() ? ctx.style.button_hover : ctx.style.button);
    ctx.text_in(add, "Add", clean.empty() ? ctx.style.text_disabled : ctx.style.text, 0.0f, true);
    app_.test_rects_["tag_new_add"] = add;
    if (!app_.asset_tag_input_.empty() && clean.empty()) ctx.label_dim("Not a usable tag name");
}

void AssetsPanel::retag_asset_(AssetType t, const std::string& rel, const std::vector<std::string>& tags) {
    const std::string to = with_asset_tags(rel, tags);
    if (to == rel) return;
    const fs::path dst = app_.project_.assets() / to;
    if (fs::exists(is_scene_folder_file(rel) ? dst.parent_path() : dst)) { app_.log_error(to + " already exists"); return; }
    app_.guarded_([this, t, rel, to] {
        const bool was_open = asset_is_open_(t, rel);
        rename_asset_(rel, to);
        if (was_open && fs::exists(app_.project_.assets() / to)) open_asset_now_(t, app_.project_.assets() / to);
    });
}

void AssetsPanel::set_asset_tag_filter(const std::vector<std::string>& tags, bool match_all) {
    tag_filter_() = tags;
    app_.asset_tag_match_all_ = match_all;
}

std::vector<std::string> AssetsPanel::assets_listed() {
    std::vector<std::string> items;
    for (const auto& rel : list_assets_(app_.asset_tab_)) if (passes_tag_filter_(rel)) items.push_back(rel);
    sort_assets_(app_.asset_tab_, items);
    return items;
}

bool AssetsPanel::asset_is_open_(AssetType t, const std::string& rel) const {
    if (t == AssetType::Theme) return app_.game_theme_.open() && app_.project_.relative(app_.game_theme_.path) == rel;   // open beside a UI
    if (app_.active_type_ != t) return false;
    if (t == AssetType::Mesh) return !app_.mesh_.path.empty() && app_.project_.relative(app_.mesh_.path) == rel;
    return !app_.active_path_.empty() && app_.project_.relative(app_.active_path_) == rel;
}

bool AssetsPanel::open_asset_dirty_() const {
    switch (app_.active_type_) {
        case AssetType::Scene: case AssetType::Object: case AssetType::UI: return app_.doc_.dirty();
        case AssetType::Mesh: return app_.mesh_.open() && app_.mesh_.dirty();
        case AssetType::Material: return app_.material_.open() && app_.material_.dirty();
        default: return false;
    }
}

void AssetsPanel::open_asset(AssetType t, const std::string& item) {
    fs::path abs = app_.project_.absolute(item);
    if (!fs::exists(abs) && !coopa::yaml::document_exists(abs) && !app_.doc_.path().empty()) abs = app_.doc_.path().parent_path() / item;
    if (refuse_engine_asset_(abs)) return;
    // A theme opens beside the open UI, replacing only the open theme (and, without a UI
    // open, the document it previews on) -- so only those need saving first.
    const bool replaces = t != AssetType::Theme || app_.game_theme_.dirty() || (app_.active_type_ != AssetType::UI && app_.doc_.dirty());
    if (replaces) app_.guarded_([this, t, abs] { open_asset_now_(t, abs); });
    else app_.deferred_.push_back([this, t, abs] { open_asset_now_(t, abs); });
}

void AssetsPanel::open_asset_now_(AssetType t, const fs::path& abs) {
    switch (t) {
        case AssetType::Scene: app_.open_scene(abs); break;
        case AssetType::Object: open_object_asset(abs); break;
        case AssetType::Mesh: app_.open_mesh(abs); break;
        case AssetType::Material: app_.open_material(abs); break;
        case AssetType::Texture: open_texture(abs); break;
        // Switching the view is what every other asset type's open_* does for itself; a panel
        // asset has to do it here, or the workspace never opens and the preview paints over
        // whichever workspace the editor was actually in.
        case AssetType::UI:
            if (app_.ui_canvas_panel_->open_ui_asset(abs)) app_.set_view_(AssetType::UI);
            break;
        case AssetType::Theme: app_.theme_editor_panel_->open_theme(abs); break;
        case AssetType::Audio: open_audio(abs); break;
        default: app_.log_info(abs.string()); break;
    }
}

void AssetsPanel::open_asset_(const std::string& kind, const std::string& item) {
    if (kind == "scene") open_asset(AssetType::Scene, item);
    else if (kind == "object") open_asset(AssetType::Object, item);
    else if (kind == "mesh") open_asset(AssetType::Mesh, item);
    else if (kind == "material") open_asset(AssetType::Material, item);
    else if (kind == "texture") open_asset(AssetType::Texture, item);
    else if (kind == "ui") open_asset(AssetType::UI, item);
    else if (kind == "theme") open_asset(AssetType::Theme, item);
    else if (kind == "audio") open_asset(AssetType::Audio, item);
    else app_.log_info(item);
}

bool AssetsPanel::open_object_asset(const fs::path& path) {
    // UI assets are object assets too, edited in the UI designer.
    if (asset_type_of_(app_.project_.relative(path)) == AssetType::UI) return app_.ui_canvas_panel_->open_ui_asset(path);
    app_.stop();
    try {
        app_.doc_.load(path);
    } catch (const std::exception& e) {
        app_.log_error(std::string("Open object failed: ") + e.what());
        return false;
    }
    if (!app_.doc_.is_object_asset()) { app_.log_error(app_.project_.relative(path) + " is not an object asset (no `object:`)"); return false; }
    app_.set_view_(AssetType::Object);
    app_.active_path_ = path;
    app_.sync_.fallback_path = path;
    app_.rebuild_scene_();
    app_.instances_panel_->remember_asset_names_();   // renamed parts are followed into other files on save
    if (ObjectId root = app_.doc_.object_root()) app_.doc_.select(root);
    app_.deferred_.push_back([this] { app_.frame_all(); });
    app_.log_info("Opened object " + app_.project_.relative(path));
    return true;
}

bool AssetsPanel::new_object_asset(const std::string& name) {
    const std::string dir = new_asset_dir_("objects");
    const std::string n = app_.unique_asset_name_(dir, name);
    const fs::path path = app_.project_.assets() / dir / (n + ".yaml");
    app_.doc_.reset_object(n);
    try {
        fs::create_directories(path.parent_path());
        app_.doc_.save(path);
        app_.project_.refresh();
    } catch (const std::exception& e) {
        app_.log_error(std::string("Create object failed: ") + e.what());
        return false;
    }
    return open_object_asset(path);
}

bool AssetsPanel::open_audio(const fs::path& path) {
    if (!fs::exists(path)) { app_.log_error("Sound not found: " + path.string()); return false; }
    stop_audio_preview();
    app_.set_view_(AssetType::Audio);
    app_.prop_tab_ = PropTab::Data;
    app_.active_path_ = path;
    app_.audio_import_ = coopa::sfx::data::ClipImportSettings::load(path.string());
    app_.audio_import_dirty_ = false;
    app_.log_info("Opened sound " + app_.project_.relative(path));
    return true;
}

void AssetsPanel::preview_audio() {
    stop_audio_preview();
    if (app_.active_type_ != AssetType::Audio || app_.active_path_.empty()) return;
    coopa::sfx::core::PlayParams p;
    p.bus_name = audio::k_bus_ui;
    p.gain = app_.audio_import_.volume;
    p.pitch = app_.audio_import_.pitch;
    try {
        app_.audio_preview_ = app_.engine_.audio().engine().play(app_.active_path_.string(), p);
    } catch (const std::exception& e) {
        app_.log_error(std::string("Preview failed: ") + e.what());
    }
}

void AssetsPanel::stop_audio_preview() {
    if (app_.audio_preview_.is_valid()) app_.engine_.audio().engine().stop(app_.audio_preview_);
    app_.audio_preview_ = {};
}

int AssetsPanel::import_audio(const std::vector<fs::path>& files) {
    int n = 0;
    std::error_code ec;
    const fs::path dir = app_.project_.assets() / new_asset_dir_("audio");
    fs::create_directories(dir, ec);
    for (const fs::path& f : files) {
        if (!is_audio_ext_(f.extension().string())) { app_.log_warn("Not a .wav / .mp3: " + f.string()); continue; }
        const fs::path dst = dir / f.filename();
        fs::copy_file(f, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) { app_.log_error("Import " + f.filename().string() + " failed: " + ec.message()); continue; }
        const fs::path sidecar = f.string() + ".import";
        if (fs::exists(sidecar, ec)) fs::copy_file(sidecar, dst.string() + ".import", fs::copy_options::overwrite_existing, ec);
        ++n;
    }
    app_.project_.refresh();
    if (n) app_.log_info("Imported " + std::to_string(n) + " sound" + (n == 1 ? "" : "s") + " into " + app_.project_.relative(dir) + "/");
    return n;
}

bool AssetsPanel::open_texture(const fs::path& path) {
    if (!fs::exists(path)) { app_.log_error("Texture not found: " + path.string()); return false; }
    app_.set_view_(AssetType::Texture);
    app_.active_path_ = path;
    app_.uploaded_revision_ = 0;
    app_.deferred_.push_back([this] {
        ensure_preview_();
        app_.camera_.axis_view('t', false);
        app_.frame_all();
    });
    app_.log_info("Opened texture " + app_.project_.relative(path));
    return true;
}

Node AssetsPanel::object_asset_node_(Node obj) {
    strip_private_keys(obj);
    if (obj.contains("components") && obj.at("components").is_sequence()) {
        for (auto& c : obj["components"].as_seq()) {
            if (component_type(c) != "Transform") continue;
            c = Node::mapping();
            c["type"] = Node(std::string("Transform"));
        }
    }
    return obj;
}

bool AssetsPanel::write_object_asset_(const fs::path& path, const Node& obj) {
    Node doc = Node::mapping();
    doc["object"] = obj;
    try {
        fs::create_directories(path.parent_path());
        coopa::yaml::save_document(path, doc);
        app_.project_.refresh();
        return true;
    } catch (const std::exception& e) {
        app_.log_error(std::string("Write object asset failed: ") + e.what());
        return false;
    }
}

void AssetsPanel::create_object_asset_from_mesh_(const std::string& mesh_rel) {
    const std::string stem = fs::path(mesh_rel).stem().string();
    const std::string n = app_.unique_asset_name_("objects", stem);
    Node obj = app_.doc_.make_object(n);
    Node mr = default_component("MeshRenderer");
    mr["mesh_path"] = Node(mesh_ref(mesh_rel));
    obj["components"].as_seq().push_back(mr);
    const fs::path path = app_.project_.assets() / "objects" / (n + ".yaml");
    if (write_object_asset_(path, object_asset_node_(obj))) open_asset(AssetType::Object, app_.project_.relative(path));
}

bool AssetsPanel::create_object_asset_from_selection() {
    const ObjectId id = app_.doc_.primary();
    const Node* src = id ? app_.doc_.find(id) : nullptr;
    if (!src || app_.doc_.is_object_asset() || app_.playing()) return false;
    const std::string name = get_string(*src, "name", "Object");
    const std::string n = app_.unique_asset_name_("objects", name);
    const fs::path path = app_.project_.assets() / "objects" / (n + ".yaml");
    if (!write_object_asset_(path, object_asset_node_(*src))) return false;
    Node transform = Node::mapping();
    transform["type"] = Node(std::string("Transform"));
    if (src->contains("components")) {
        for (const auto& c : src->at("components").as_seq()) if (component_type(c) == "Transform") transform = c;
    }
    app_.apply_(app_.doc_.edit("Create Object Asset", [&](Node&) -> Change {
        Node* o = app_.doc_.find(id);
        if (!o) return {};
        Node inst = Node::mapping();
        inst["name"] = Node(name);
        inst["prefab"] = Node("objects/" + n);
        Node comps = Node::sequence();
        comps.as_seq().push_back(transform);
        inst["components"] = comps;
        inst[kEidKey] = o->at(kEidKey);
        *o = inst;
        return {ChangeScope::Structure, id};
    }));
    app_.log_info("Created object asset objects/" + n + ".yaml");
    return true;
}

ObjectId AssetsPanel::place_object_asset(const std::string& object_rel, std::optional<glm::vec3> at) {
    if (app_.playing() || app_.asset_view_()) return 0;
    const std::string stem = fs::path(object_rel).stem().string();
    const std::string ref = strip_yaml_ext(short_ref(fs::path(object_rel).generic_string()));   // objects/crate
    if (app_.doc_.is_object_asset() && strip_yaml_ext(short_ref(app_.project_.relative(app_.active_path_))) == ref) { app_.log_warn("An object asset can't contain itself"); return 0; }
    Node obj = Node::mapping();
    obj["name"] = Node(app_.doc_.unique_name(stem));
    obj["prefab"] = Node(ref);
    Node comps = Node::sequence();
    Node t = Node::mapping();
    t["type"] = Node(std::string("Transform"));
    t["position"] = make_vec3(at.value_or(app_.spawn_point_()));
    comps.as_seq().push_back(t);
    obj["components"] = comps;
    const ObjectId id = app_.doc_.add_object(obj, 0, -1, "Place " + stem);   // add_object stamps a fresh id
    app_.after_structure_change_(id);
    return id;
}

void AssetsPanel::draw_asset_panel_(imm::Context& ctx, const imm::Box& area) {
    using I = imm::Icon;
    const imm::Box hb = app_.area_header_(ctx, area, 30);
    // Type tabs: one icon per asset type (tooltip names it).
    const auto& types = asset_types_();
    const float s = hb.h - 8;
    float x = hb.x + 6;
    for (size_t i = 0; i < types.size(); ++i) {
        const auto& t = types[i];
        const bool on = app_.asset_tab_ == t.type;
        std::string tip = std::string(t.label) + "\nassets/" + t.dir + "/";
        if (ctx.icon_button(std::string("atab_") + t.label, t.icon, tip, on, s, imm::Context::kAll, imm::Box{x, hb.y + 4, s, s}, true)) {
            app_.asset_tab_ = t.type;
        }
        x += s + 3;
    }
    const AssetTypeInfo& info = asset_type_info_(app_.asset_tab_);
    // New (+), and left of it the toyengine toggle (a game project only).
    const imm::Box nb{hb.right() - s - 6, hb.y + 4, s, s};
    if (!app_.project_.is_engine()) {
        const imm::Box eb{nb.x - s - 4, nb.y, s, s};
        if (ctx.icon_button("asset_engine", I::Package,
                            std::string(app_.show_engine_assets_ ? "Hide" : "Show") + " toyengine's assets\n"
                            "Listed below the project's, read-only: drag them into a scene to use them, or "
                            "right-click > Copy to Project to edit one",
                            app_.show_engine_assets_, s, imm::Context::kAll, eb, true)) {
            set_show_engine_assets(!app_.show_engine_assets_);
        }
        app_.test_rects_["asset_engine_toggle"] = eb;
    }
    const bool can_new = app_.asset_tab_ != AssetType::Texture;
    const auto items_all = list_assets_(app_.asset_tab_);
    const auto engine_all = list_engine_assets_(app_.asset_tab_);
    std::vector<std::string> known_tags;   // this tab's tags, the project's and toyengine's
    std::map<std::string, int> tag_counts = tag_counts_(items_all);
    for (const auto& [tg, n] : tag_counts_(engine_all)) tag_counts[tg] += n;
    for (const auto& [tg, n] : tag_counts) known_tags.push_back(tg);
    if (ctx.icon_button("asset_new", I::Plus,
                        app_.asset_tab_ == AssetType::Audio ? std::string("Import Sounds\nCopy .wav / .mp3 files into assets/audio/")
                        : can_new ? std::string("New ") + info.singular + "\nCreate one in assets/" + info.dir + "/, with tags"
                                  : std::string("Import textures by copying image files into assets/textures/"),
                        false, s, imm::Context::kAll, nb) && can_new) {
        // The new asset starts with the tags being filtered on; the popup can change them.
        app_.new_asset_tags_ = tag_filter_();
        app_.asset_tag_input_.clear();
        ctx.open_popup("asset_new", glm::vec2(nb.x, nb.bottom()));
    }
    app_.test_rects_["asset_new_button"] = nb;
    if (ctx.begin_popup("asset_new", 240)) {
        ctx.label_dim(app_.asset_tab_ == AssetType::Audio ? std::string("Import Sounds") : std::string("New ") + info.singular);
        ctx.spacing(2);
        ctx.label_dim("Tags");
        draw_tag_editor_(ctx, app_.new_asset_tags_, known_tags);
        std::string where = std::string("assets/") + info.dir;
        for (const auto& tg : app_.new_asset_tags_) where += "/" + tg;
        ctx.label_dim("In " + where + "/");
        ctx.menu_separator();
        if (app_.asset_tab_ == AssetType::Audio) {
            if (ctx.menu_item("Import Sounds...", "", nullptr, true, I::Play)) app_.import_audio_dialog_(app_.new_asset_tags_);
        } else {
            draw_new_asset_items_(ctx, app_.asset_tab_);
        }
        ctx.end_popup();
    }
    // Title + sort, search, tag filter.
    const imm::Box title{area.x + 8, hb.bottom() + 4, area.w - 16, 20};
    std::vector<std::string> items, engine_items;
    for (const auto& rel : items_all) if (passes_tag_filter_(rel)) items.push_back(rel);
    for (const auto& rel : engine_all) if (passes_tag_filter_(rel)) engine_items.push_back(rel);
    sort_assets_(app_.asset_tab_, items);
    sort_assets_(app_.asset_tab_, engine_items);
    ctx.text_in({title.x, title.y, title.w - 24, title.h},
                std::string(info.label) + "  (" + std::to_string(items.size()) + ")" +
                    (engine_items.empty() ? std::string() : "  + " + std::to_string(engine_items.size()) + " toyengine"),
                ctx.style.text, 0.0f);
    const char* sort_labels[] = {"Name", "Last Modified", "Tags", "Size"};
    const imm::Box sb{title.right() - 18, title.y + 1, 18, 18};
    if (ctx.icon_button("asset_sort", I::Sort,
                        std::string("Sort\nBy ") + sort_labels[static_cast<int>(app_.asset_sort_)] + (app_.asset_sort_reverse_ ? ", reversed" : ""),
                        app_.asset_sort_ != AssetSort::Name || app_.asset_sort_reverse_, 18, imm::Context::kAll, sb)) {
        ctx.open_popup("asset_sort_menu", glm::vec2(sb.x, sb.bottom()));
    }
    app_.test_rects_["asset_sort"] = sb;
    if (ctx.begin_popup("asset_sort_menu", 190)) {
        ctx.label_dim("Sort by");
        for (int i = 0; i < 4; ++i) {
            const bool on = static_cast<int>(app_.asset_sort_) == i;
            if (ctx.menu_item(sort_labels[i], "", &on)) set_asset_sort_(static_cast<AssetSort>(i), app_.asset_sort_reverse_);
            app_.test_rects_[std::string("asset_sort:") + sort_labels[i]] = ctx.last_rect();
        }
        ctx.menu_separator();
        const bool rev = app_.asset_sort_reverse_;
        if (ctx.menu_item("Reverse Order", "", &rev)) set_asset_sort_(app_.asset_sort_, !app_.asset_sort_reverse_);
        ctx.end_popup();
    }
    const imm::Box search{area.x + 6, title.bottom() + 2, area.w - 12, 22};
    ctx.input_text_box("asset_filter", search, &app_.asset_filter_, "    Search");
    if (ctx.editing_text("asset_filter").value_or(app_.asset_filter_).empty()) {
        ctx.icon(I::Search, {search.x + 4, search.y + 4, 14, 14}, ctx.style.text_disabled);
    }
    float list_top = search.bottom() + 4;
    // Tag filter: a dropdown of the tab's tags (several can be on; none = everything).
    auto& filter = tag_filter_();
    filter.erase(std::remove_if(filter.begin(), filter.end(), [&](const std::string& tg) { return !tag_counts.count(tg); }), filter.end());
    if (!known_tags.empty()) {
        const imm::Box tb{area.x + 6, search.bottom() + 3, area.w - 12, 22};
        bool hov = false, held = false;
        if (ctx.invisible_button("asset_tag_filter", tb, &hov, &held)) ctx.open_popup("asset_tag_menu", glm::vec2(tb.x, tb.bottom() + 1));
        ctx.fill_rounded(tb, hov ? ctx.style.button_hover : ctx.style.button);
        ctx.icon(I::Tag, {tb.x + 5, tb.y + 4, 14, 14}, filter.empty() ? ctx.style.text_dim : ctx.style.text);
        std::string shown;
        for (const auto& tg : filter) shown += (shown.empty() ? "" : app_.asset_tag_match_all_ ? " + " : ", ") + tg;
        ctx.text_in({tb.x + 22, tb.y, tb.w - 22 - tb.h, tb.h}, filter.empty() ? std::string("All tags") : shown,
                    filter.empty() ? ctx.style.text_dim : ctx.style.text, 0.0f);
        ctx.arrow({tb.right() - tb.h + 3, tb.y + 3, tb.h - 6, tb.h - 6}, true, ctx.style.text_dim);
        ctx.tooltip("Tags\nShow only assets with these tags (their folders under assets/" + std::string(info.dir) +
                    "/). None picked: everything.");
        app_.test_rects_["asset_tag_filter"] = tb;
        if (ctx.begin_popup("asset_tag_menu", std::max(200.0f, tb.w))) {
            for (const auto& [tg, n] : tag_counts) {
                bool on = std::find(filter.begin(), filter.end(), tg) != filter.end();
                ctx.push_id("tf:" + tg);
                if (ctx.checkbox(tg + "  (" + std::to_string(n) + ")", &on)) toggle_tag_filter_(tg);
                app_.test_rects_["asset_tag:" + tg] = ctx.last_rect();
                ctx.pop_id();
            }
            ctx.menu_separator();
            bool all = app_.asset_tag_match_all_;
            if (ctx.checkbox("Match all picked tags", &all)) app_.asset_tag_match_all_ = all;
            ctx.tooltip("Match all picked tags\nOn: an asset needs every picked tag. Off: any one of them.");
            if (ctx.button("Clear", 80, !filter.empty())) filter.clear();
            ctx.end_popup();
        }
        list_top = tb.bottom() + 4;
    }
    const imm::Box body{area.x, list_top, area.w, area.bottom() - list_top};
    ctx.begin_region("asset_list", body, true);
    std::string f = app_.asset_filter_;
    std::transform(f.begin(), f.end(), f.begin(), ::tolower);
    size_t shown = 0;
    bool row_hovered = false;
    std::string right_clicked, chip_clicked;
    // Name, then the asset's tags as chips at the row's right (click one to filter by it).
    auto row_chips = [&](const std::string& rel, const std::string& label) {
        const imm::Box r = ctx.last_rect();
        const float min_x = r.x + r.h + 4 + ctx.text_width(label) + 10;
        const std::string hit = draw_tag_chips_(ctx, r, asset_tags(rel), min_x, ctx.last_clicked());
        if (!hit.empty()) chip_clicked = hit;
        return !hit.empty();
    };
    for (const auto& rel : items) {
        const std::string name = asset_display_name_(app_.asset_tab_, rel);
        std::string ln = rel;
        std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
        if (!f.empty() && ln.find(f) == std::string::npos) continue;
        ++shown;
        ctx.push_id(rel);
        const bool open = asset_is_open_(app_.asset_tab_, rel);
        const bool dirty = app_.asset_tab_ == AssetType::Theme ? app_.game_theme_.dirty() : open_asset_dirty_();
        const std::string label = name + (open && dirty ? "  *" : "");
        const bool clicked = ctx.selectable(label, open, 0, info.icon);
        if (clicked && !row_chips(rel, label)) {
            if (!open) open_asset(app_.asset_tab_, rel);
            else if (app_.asset_tab_ == AssetType::Theme) app_.prop_tab_ = PropTab::Theme;
        } else if (!clicked) {
            row_chips(rel, label);
        }
        const auto tags = asset_tags(rel);
        std::string tag_line;
        for (const auto& tg : tags) tag_line += (tag_line.empty() ? "\nTags: " : ", ") + tg;
        ctx.tooltip(name + "\nassets/" + rel + tag_line +
                    (app_.asset_tab_ == AssetType::Object ? "\nDrag into a scene to place an instance" :
                     app_.asset_tab_ == AssetType::Material ? "\nDrag onto an object or a mesh slot" :
                     app_.asset_tab_ == AssetType::Mesh ? "\nDrag into a scene to add it" :
                     app_.asset_tab_ == AssetType::UI ? "\nDrag into a scene (or another UI) to place it" :
                     app_.asset_tab_ == AssetType::Theme ? "\nOpens in the Theme tab, previewed on a UI" : ""));
        // Dragged as its short reference (no tag folders): what gets written into the
        // scene keeps working when the asset is re-tagged.
        ctx.drag_source("asset", short_ref(rel), name);
        row_hovered |= ctx.last_hovered();
        app_.test_rects_["asset_row:" + rel] = ctx.last_rect();
        if (ctx.last_clicked(imm::Mouse::Right)) right_clicked = rel;
        ctx.pop_id();
    }
    // toyengine's assets, read-only: drag and drop (and the right-click uses), no editing.
    bool engine_header = false;
    for (const auto& rel : engine_items) {
        const std::string name = asset_display_name_(app_.asset_tab_, rel);
        std::string ln = rel;
        std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
        if (!f.empty() && ln.find(f) == std::string::npos) continue;
        if (!engine_header) {
            engine_header = true;
            ctx.spacing(4);
            const imm::Box sep = ctx.next_box(18);
            ctx.fill({sep.x, sep.y + 2, sep.w, 1}, ctx.style.separator);
            ctx.icon(I::Lock, {sep.x + 2, sep.y + 5, 12, 12}, ctx.style.text_disabled);
            ctx.text_in({sep.x + 18, sep.y + 2, sep.w - 18, sep.h}, "toyengine  (read-only)", ctx.style.text_disabled, 0.0f);
        }
        ++shown;
        ctx.push_id("engine:" + rel);
        const bool clicked = ctx.selectable(name, false, 0, info.icon);
        if (!row_chips(rel, name) && clicked) refuse_engine_asset_(app_.project_.absolute(rel));
        ctx.tooltip(name + "\ntoyengine: assets/" + rel + "\nRead-only -- drag it into a scene to use it, or right-click > "
                    "Copy to Project to edit a copy");
        ctx.drag_source("asset", short_ref(rel), name);
        row_hovered |= ctx.last_hovered();
        app_.test_rects_["asset_row:engine:" + rel] = ctx.last_rect();
        if (ctx.last_clicked(imm::Mouse::Right)) right_clicked = rel;
        ctx.pop_id();
    }
    if (!chip_clicked.empty()) toggle_tag_filter_(chip_clicked);
    // Opened OUTSIDE the row's push_id(): a popup's id is scoped like any widget's, so one
    // opened inside the row would never match the begin_popup("asset_ctx") below.
    if (!right_clicked.empty()) { app_.asset_context_ = right_clicked; ctx.open_popup("asset_ctx"); }
    // Right-click on empty space in the list: Add (what + offers for this tab).
    else if (!row_hovered && ctx.is_hovered(body) && !ctx.popup_hovered() && ctx.input().released[1]) {
        app_.new_asset_tags_ = tag_filter_();   // Add creates under the tags being filtered on
        ctx.open_popup("asset_list_ctx");
    }
    if (shown == 0) {
        ctx.label_dim(items_all.empty() && engine_all.empty() ? std::string("No ") + info.label + " yet -- press + to create one."
                                                              : "Nothing matches.");
        if (app_.asset_tab_ == AssetType::Texture && items_all.empty()) ctx.label_dim("Copy .png files into assets/textures/.");
    }
    if (ctx.begin_popup("asset_ctx", 210)) {
        draw_asset_context_menu_(ctx, app_.asset_tab_, app_.asset_context_);
        ctx.end_popup();
    }
    if (ctx.begin_popup("asset_list_ctx", 200)) {
        const bool can_add = app_.asset_tab_ != AssetType::Texture && app_.asset_tab_ != AssetType::Audio;
        std::string where = std::string("assets/") + info.dir;
        for (const auto& tg : app_.new_asset_tags_) where += "/" + tg;
        if (asset_new_has_choices_(app_.asset_tab_)) {
            if (ctx.begin_menu("Add", can_add, I::Plus)) {
                app_.test_rects_["asset_add"] = ctx.last_rect();
                draw_new_asset_items_(ctx, app_.asset_tab_);
                ctx.end_menu();
            } else {
                app_.test_rects_["asset_add"] = ctx.last_rect();
            }
        } else {
            if (ctx.menu_item(std::string("Add ") + info.singular, "", nullptr, can_add, I::Plus)) new_asset_(app_.asset_tab_);
            app_.test_rects_["asset_add"] = ctx.last_rect();
        }
        ctx.tooltip(can_add ? std::string("Add\nCreate a new ") + info.singular + " in " + where + "/ (+ also picks its tags)"
                            : std::string("Add\nImport textures by copying image files into assets/textures/"));
        ctx.menu_separator();
        if (ctx.menu_item("Refresh", "", nullptr, true, I::Restart)) app_.project_.refresh();
        ctx.tooltip("Refresh\nRescan assets/ for files added or removed outside the editor");
        ctx.end_popup();
    }
    ctx.end_region();
    draw_asset_modals_(ctx);
}

void AssetsPanel::draw_asset_context_menu_(imm::Context& ctx, AssetType t, const std::string& rel) {
    using I = imm::Icon;
    const bool is_engine = app_.project_.is_engine_asset(rel);
    ctx.label_dim(asset_display_name_(t, rel) + (is_engine ? "  (toyengine, read-only)" : ""));
    if (!is_engine && ctx.menu_item("Open", "", nullptr, true, asset_type_info_(t).icon)) open_asset(t, rel);
    if (t == AssetType::Object && ctx.menu_item("Place in Scene", "", nullptr, !app_.asset_view_() && !app_.playing(), I::Plus)) place_object_asset(rel);
    if (t == AssetType::UI && ctx.menu_item(app_.active_type_ == AssetType::UI ? "Place in this UI" : "Place in Scene", "", nullptr,
                                            !app_.asset_view_() && !app_.playing() && !asset_is_open_(t, rel), I::Plus)) {
        app_.ui_canvas_panel_->place_ui_asset(rel, app_.active_type_ == AssetType::UI ? app_.doc_.object_root() : 0);
    }
    if (t == AssetType::Mesh && ctx.menu_item("Make Object Asset", "", nullptr, true, I::Object)) create_object_asset_from_mesh_(rel);
    if (t == AssetType::Mesh && ctx.menu_item("Add to Scene", "", nullptr, !app_.asset_view_() && !app_.playing(), I::Plus)) app_.add_mesh_to_scene_(rel);
    if (t == AssetType::Material && ctx.menu_item("Assign to Selected", "", nullptr, !app_.asset_view_() && !app_.doc_.selection().empty(), I::Link)) {
        app_.assign_material_(rel);
    }
    ctx.menu_separator();
    if (is_engine) {
        // Using it is fine; changing it is toyengine's business. A copy is the project's own.
        if (t == AssetType::Object && is_tile_set_(rel)) {
            if (ctx.menu_item("Duplicate as New Tile Set", "", nullptr, true, I::Duplicate)) duplicate_tile_set(rel);
            ctx.tooltip("Duplicate as New Tile Set\nCopies this terrain tile style into the project under a new "
                        "name, with its own copy of every piece mesh: open it, Tab into a piece to reshape it, "
                        "and point a Terrain's styles: at it.");
        }
        if (ctx.menu_item("Copy to Project", "", nullptr, true, I::Duplicate)) copy_engine_asset_to_project(t, rel);
        ctx.tooltip("Copy to Project\nCopies it into this project's assets/" + std::string(asset_type_info_(t).dir) +
                    "/ under the same name. The copy is editable and replaces toyengine's everywhere it is used.");
        ctx.menu_separator();
        if (ctx.menu_item("Copy Path", "", nullptr, true) && ctx.input().set_clipboard) ctx.input().set_clipboard(rel);
        return;
    }
    if (ctx.menu_item("Duplicate", "", nullptr, true, I::Duplicate)) duplicate_asset_(t, rel);
    if (ctx.menu_item("Tags...", "", nullptr, true, I::Tag)) {
        app_.asset_tags_target_ = rel;
        app_.asset_tags_type_ = t;
        app_.asset_tags_edit_ = asset_tags(rel);
        app_.asset_tag_input_.clear();
        app_.pending_modal_ = "Edit Tags";
    }
    ctx.tooltip("Tags\nAdd or remove this asset's tags -- the folders it sits in under assets/" +
                std::string(asset_type_info_(t).dir) + "/. References to it keep working.");
    app_.test_rects_["asset_tags"] = ctx.last_rect();
    if (ctx.menu_item("Rename...", "", nullptr, !asset_is_open_(t, rel))) {
        app_.asset_rename_ = rel;
        app_.asset_rename_to_ = asset_display_name_(t, rel);
        app_.pending_modal_ = "Rename Asset";
    }
    if (ctx.menu_item("Delete...", "", nullptr, !asset_is_open_(t, rel), I::Trash)) { app_.asset_delete_ = rel; app_.pending_modal_ = "Delete Asset"; }
    app_.test_rects_["asset_delete"] = ctx.last_rect();
    if (asset_is_open_(t, rel)) ctx.tooltip("Delete\nClose it first: open another asset, then delete this one");
    ctx.menu_separator();
    if (ctx.menu_item("Copy Path", "", nullptr, true) && ctx.input().set_clipboard) ctx.input().set_clipboard(rel);
}

namespace {

/**
 * @brief The starting points the New Material menu offers.
 *
 * Only the keys that make the look are written; everything else stays at the material
 * defaults, so a preset reads as "a material, started from here" rather than a wall of values.
 */
const std::vector<std::pair<std::string, Node>>& material_presets() {
    static const std::vector<std::pair<std::string, Node>> presets = [] {
        std::vector<std::pair<std::string, Node>> out;
        auto add = [&out](const char* label, glm::vec3 albedo, float metallic, float roughness,
                          const std::function<void(Node&)>& extra = {}) {
            Node m = Node::mapping();
            m["albedo"] = make_color(albedo);
            m["metallic"] = make_float(metallic);
            m["roughness"] = make_float(roughness);
            if (extra) extra(m);
            out.push_back({label, m});
        };
        add("Plain", glm::vec3(0.8f), 0.0f, 0.5f);
        add("Metal", glm::vec3(0.56f, 0.57f, 0.58f), 1.0f, 0.25f);
        add("Plastic", glm::vec3(0.2f, 0.45f, 0.8f), 0.0f, 0.35f);
        add("Rough Stone", glm::vec3(0.42f, 0.41f, 0.38f), 0.0f, 0.95f);
        add("Glass", glm::vec3(0.9f, 0.95f, 1.0f), 0.0f, 0.05f, [](Node& m) {
            m["alpha_mode"] = Node(std::string("BLEND"));
            m["alpha"] = make_float(0.25);
            m["refraction"] = Node(true);
            m["ior"] = make_float(1.45);
        });
        add("Emissive", glm::vec3(0.05f), 0.0f, 0.6f, [](Node& m) {
            m["emissive"] = make_color(glm::vec3(1.0f, 0.75f, 0.35f));
            m["emissive_strength"] = make_float(4.0);
        });
        add("Foliage", glm::vec3(0.25f, 0.45f, 0.18f), 0.0f, 0.8f, [](Node& m) {
            m["shader"] = Node(std::string("foliage"));
            m["alpha_mode"] = Node(std::string("CUTOUT"));
            const float params[4] = {0.12f, 1.6f, 1.0f, 0.0f};
            m["shader_params"] = make_float_seq(params, 4);
        });
        return out;
    }();
    return presets;
}

} // namespace

void AssetsPanel::draw_new_asset_items_(imm::Context& ctx, AssetType t) {
    using I = imm::Icon;
    if (t == AssetType::Mesh) {
        for (const auto& p : primitive_names()) {
            if (ctx.menu_item(p, "", nullptr, true, I::Mesh)) {
                app_.guarded_([this, p, tags = app_.new_asset_tags_] { with_tags_(tags, [&] { app_.new_mesh(p); app_.save_mesh(); app_.project_.refresh(); }); });
            }
            app_.test_rects_["asset_new:" + p] = ctx.last_rect();
        }
        return;
    }
    if (t == AssetType::UI) {
        const auto tags = app_.new_asset_tags_;
        // Two kinds of UI file. A PANEL is a form -- tabs of labelled rows -- that
        // PanelScreen draws with no C++. A PLACEMENT file says where a C++ screen's boxes sit.
        if (ctx.menu_item("Blank Panel", "", nullptr, true, I::UiCanvas)) app_.guarded_([this, tags] { with_tags_(tags, [&] { app_.ui_canvas_panel_->new_ui_asset("new_panel", "blank"); }); });
        ctx.tooltip("Blank Panel\nA screen of labelled rows, drawn by PanelScreen -- no C++ needed");
        app_.test_rects_["asset_new:Blank Panel"] = ctx.last_rect();
        const auto templates = app_.ui_canvas_panel_->ui_templates();
        for (const auto& tpl : templates) {
            if (tpl == "blank") continue;
            std::string label = tpl;
            for (char& c : label) if (c == '_') c = ' ';
            if (!label.empty()) label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
            if (ctx.menu_item(label + " Panel", "", nullptr, true, I::UiCanvas)) app_.guarded_([this, tpl, tags] { with_tags_(tags, [&] { app_.ui_canvas_panel_->new_ui_asset(tpl, tpl); }); });
            ctx.tooltip(label + " Panel\nA panel pre-filled with the usual " + tpl + " rows");
        }

        // One entry per ImmScreen component the engine registers: the file is named after the
        // screen, which is how that screen finds it.
        const auto screens = toy::editor::UiCanvasPanel::screen_types();
        if (!screens.empty()) ctx.menu_separator();
        for (const auto& type : screens) {
            if (ctx.menu_item(type + " Placement", "", nullptr, true, I::Move)) app_.guarded_([this, type, tags] { with_tags_(tags, [&] { app_.ui_canvas_panel_->new_placement_asset(type); }); });
            ctx.tooltip(type + " Placement\nWhere " + type +
                        "'s panels sit (" + type + ".tune.yaml), edited as numbers beside a live preview");
        }
        return;
    }
    if (t == AssetType::Material) {
        const auto tags = app_.new_asset_tags_;
        for (const auto& [label, body] : material_presets()) {
            if (ctx.menu_item(label, "", nullptr, true, I::Material)) {
                const std::string name = snake_case(label);
                app_.guarded_([this, tags, name, body] { with_tags_(tags, [&] { app_.create_material(name, body); }); });
            }
            app_.test_rects_["asset_new:" + label] = ctx.last_rect();
        }
        return;
    }
    const AssetTypeInfo& info = asset_type_info_(t);
    if (ctx.menu_item(std::string("New ") + info.singular, "", nullptr, t != AssetType::Texture, info.icon)) new_asset_(t);
    app_.test_rects_["asset_new:" + std::string(info.singular)] = ctx.last_rect();
}

void AssetsPanel::new_asset_(AssetType t) {
    const auto tags = app_.new_asset_tags_;
    switch (t) {
        case AssetType::Scene: app_.guarded_([this, tags] { with_tags_(tags, [&] { new_scene_asset_("scene"); }); }); break;
        case AssetType::Object: app_.guarded_([this, tags] { with_tags_(tags, [&] { new_object_asset("object"); }); }); break;
        case AssetType::Material: app_.guarded_([this, tags] { with_tags_(tags, [&] { app_.create_material("material"); }); }); break;
        case AssetType::UI: app_.guarded_([this, tags] { with_tags_(tags, [&] { app_.ui_canvas_panel_->new_ui_asset("new_ui", "blank"); }); }); break;
        case AssetType::Theme: app_.guarded_([this, tags] { with_tags_(tags, [&] { app_.theme_editor_panel_->create_theme("theme"); }); }); break;
        default: break;
    }
}

void AssetsPanel::new_scene_asset_(const std::string& base) {
    std::string n = base;
    const std::string dir = new_asset_dir_("scenes");
    for (int i = 1; fs::exists(app_.project_.assets() / dir / n) || asset_name_taken_("scenes", n); ++i) n = base + "_" + std::to_string(i);
    const fs::path path = app_.project_.assets() / dir / n / "scene.yaml";
    app_.new_scene();
    app_.apply_(app_.doc_.set_scene_key("scene_name", Node(n)));
    try {
        fs::create_directories(path.parent_path());
        app_.save_scene_as(path);
    } catch (const std::exception& e) {
        app_.log_error(std::string("Create scene failed: ") + e.what());
        return;
    }
    app_.open_scene(path);
}

void AssetsPanel::duplicate_asset_(AssetType t, const std::string& rel) {
    const fs::path src = app_.project_.absolute(rel);
    std::error_code ec;
    fs::path dst;
    if (t == AssetType::Scene && src.filename().string().rfind("scene", 0) == 0) {
        const fs::path dir = src.parent_path();
        fs::path ndir = dir;
        for (int i = 1; fs::exists(ndir) || asset_name_taken_("scenes", ndir.filename().string()); ++i) {
            ndir = dir.parent_path() / (dir.filename().string() + "_" + std::to_string(i));
        }
        fs::copy(dir, ndir, fs::copy_options::recursive, ec);
        dst = ndir / src.filename();
    } else {
        const std::string stem = src.stem().string();
        const fs::path dir = src.parent_path();
        const std::string type = asset_type_dir(rel);
        auto taken = [&](const fs::path& p) { return fs::exists(p) || asset_name_taken_(type, p.stem().string()); };
        dst = dir / (stem + "_copy" + src.extension().string());
        for (int i = 2; taken(dst); ++i) dst = dir / (stem + "_copy" + std::to_string(i) + src.extension().string());
        fs::copy_file(src, dst, ec);
        if (!ec && t == AssetType::Object) duplicate_tile_set_pieces_(dst);
    }
    app_.project_.refresh();
    if (ec) app_.log_error("Duplicate failed: " + ec.message());
    else app_.log_info("Duplicated to " + app_.project_.relative(dst));
}

std::string AssetsPanel::duplicate_tile_set(const std::string& rel) {
    // absolute() already falls back to toyengine's copy (by path, then by name).
    const fs::path src = coopa::yaml::resolve_variant(app_.project_.absolute(rel));
    const std::string n = app_.unique_asset_name_("objects", src.stem().string() + "_copy");
    const fs::path dst = app_.project_.assets() / "objects" / (n + ".yaml");
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(src, dst, ec);
    if (ec) { app_.log_error("Duplicate tile set failed: " + ec.message()); return ""; }
    duplicate_tile_set_pieces_(dst);
    app_.project_.refresh();
    app_.log_info("Duplicated tile set to " + app_.project_.relative(dst));
    return app_.project_.relative(dst);
}

bool AssetsPanel::is_tile_set_(const std::string& rel) const {
    // absolute() already falls back to toyengine's copy (by path, then by name).
    const fs::path path = coopa::yaml::resolve_variant(app_.project_.absolute(rel));
    try {
        const Node doc = coopa::yaml::load_document(path);
        return doc.contains("object") && doc.at("object").contains("tile_set") &&
               doc.at("object").at("tile_set").is_boolean() && doc.at("object").at("tile_set").get_value<bool>();
    } catch (...) {
        return false;
    }
}

void AssetsPanel::duplicate_tile_set_pieces_(const fs::path& copy) {
    Node doc;
    try { doc = coopa::yaml::load_document(copy); } catch (...) { return; }
    if (!doc.contains("object")) return;
    Node& obj = doc["object"];
    if (!obj.contains("tile_set") || !obj.at("tile_set").is_boolean() || !obj.at("tile_set").get_value<bool>()) return;
    const std::string stem = copy.stem().string();
    obj["name"] = Node(stem);
    if (obj.contains("children") && obj.at("children").is_sequence()) {
        for (auto& child : obj["children"].as_seq()) {
            if (!child.contains("name") || !child.contains("components")) continue;
            const std::string piece = child.at("name").get_value<std::string>();
            for (auto& comp : child["components"].as_seq()) {
                if (component_type(comp) != "MeshRenderer" || !comp.contains("mesh_path")) continue;
                const std::string from_ref = comp.at("mesh_path").get_value<std::string>();
                // The project's piece, else toyengine's -- by name, in whatever tag folder.
                fs::path from = coopa::yaml::resolve_variant(app_.project_.absolute("meshes/" + from_ref + ".yaml"));
                if (!fs::exists(from)) {
                    if (auto e = coopa::asset::AssetIndex::find(Project::engine_assets(), "meshes/" + from_ref + ".yaml")) from = *e;
                }
                const std::string to_ref = stem + "_" + piece;
                const fs::path to = app_.project_.assets() / "meshes" / (to_ref + ".yaml");
                std::error_code ec;
                fs::create_directories(to.parent_path(), ec);
                if (!fs::exists(from) || !fs::copy_file(from, to, fs::copy_options::skip_existing, ec)) {
                    if (!fs::exists(to)) { app_.log_error("Tile set piece " + from_ref + " could not be copied"); continue; }
                }
                comp["mesh_path"] = Node(to_ref);
            }
        }
    }
    try { coopa::yaml::save_document(copy, doc); }
    catch (const std::exception& e) { app_.log_error(std::string("Duplicate tile set failed: ") + e.what()); }
}

void AssetsPanel::rename_asset_(const std::string& from, const std::string& to) {
    std::vector<std::string> rewritten;
    try {
        rewritten = app_.project_.rename_asset(from, to);
    } catch (const std::exception& e) {
        app_.log_error(std::string("Rename failed: ") + e.what());
        app_.project_.refresh();
        return;
    }
    for (const auto& f : rewritten) app_.log_info("Updated references in " + f);
    app_.log_info("Renamed " + from + " to " + to);
    auto touched = [&](const fs::path& p) {
        return !p.empty() && std::find(rewritten.begin(), rewritten.end(), app_.project_.relative(p)) != rewritten.end();
    };
    if ((app_.active_type_ == AssetType::Scene || app_.active_type_ == AssetType::Object || app_.active_type_ == AssetType::UI) && touched(app_.active_path_)) {
        open_asset_now_(app_.active_type_, app_.active_path_);
    }
    if (app_.active_type_ == AssetType::Material && touched(app_.material_.path)) app_.open_material(app_.material_.path);
    if (touched(app_.config_.path)) app_.config_.load(app_.config_.path);
}

void AssetsPanel::draw_asset_modals_(imm::Context& ctx) {
    if (ctx.begin_modal("Rename Asset", {360, 0})) {
        ctx.label("Rename " + app_.asset_rename_);
        ctx.input_text("New name", &app_.asset_rename_to_);
        if (ctx.button("Rename", 100) && !app_.asset_rename_to_.empty()) {
            const fs::path src = app_.project_.absolute(app_.asset_rename_);
            // A scene's name is its folder's; anything else's, its file's.
            const bool scene_dir = is_scene_folder_file(app_.asset_rename_);
            const fs::path dst = scene_dir ? src.parent_path().parent_path() / app_.asset_rename_to_ / src.filename()
                                           : src.parent_path() / (app_.asset_rename_to_ + src.extension().string());
            const std::string type = asset_type_dir(app_.asset_rename_);
            if (fs::exists(scene_dir ? dst.parent_path() : dst)) app_.log_error(app_.project_.relative(dst) + " already exists");
            else if (asset_name_taken_(type, app_.asset_rename_to_, app_.asset_rename_)) {
                app_.log_error("Another " + type + " asset is already named " + app_.asset_rename_to_ +
                          " -- names are unique per type, whatever their tags");
            }
            // References are rewritten on disk, so unsaved edits are settled first.
            else app_.guarded_([this, from = app_.asset_rename_, to = app_.project_.relative(dst)] { rename_asset_(from, to); });
            ctx.close_current_popup();
        }
        ctx.same_line();
        if (ctx.button("Cancel", 80)) ctx.close_current_popup();
        ctx.end_modal();
    }
    if (ctx.begin_modal("Edit Tags", {340, 0})) {
        const AssetType t = app_.asset_tags_type_;
        ctx.label("Tags of " + asset_display_name_(t, app_.asset_tags_target_));
        ctx.spacing(2);
        std::map<std::string, int> counts = tag_counts_(list_assets_(t));
        for (const auto& [tg, n] : tag_counts_(list_engine_assets_(t))) counts[tg] += n;
        std::vector<std::string> known;
        for (const auto& [tg, n] : counts) known.push_back(tg);
        draw_tag_editor_(ctx, app_.asset_tags_edit_, known);
        ctx.spacing(4);
        const std::string to = with_asset_tags(app_.asset_tags_target_, app_.asset_tags_edit_);
        ctx.label_dim(to == app_.asset_tags_target_ ? std::string("No change") : "Moves to assets/" + to);
        const bool apply = ctx.button("Apply", 100, to != app_.asset_tags_target_);
        app_.test_rects_["asset_tags_apply"] = ctx.last_rect();
        if (apply) {
            retag_asset_(t, app_.asset_tags_target_, app_.asset_tags_edit_);
            ctx.close_current_popup();
        }
        ctx.same_line();
        if (ctx.button("Cancel", 80)) ctx.close_current_popup();
        ctx.end_modal();
    }
    if (ctx.begin_modal("Delete Asset", {380, 0})) {
        ctx.label("Delete " + app_.asset_delete_ + "?");
        ctx.label_dim("Scenes and objects referring to it will fail to load it.");
        const bool confirm = ctx.button("Delete", 100);
        app_.test_rects_["asset_delete_confirm"] = ctx.last_rect();
        if (confirm) {
            std::error_code ec;
            const fs::path p = app_.project_.absolute(app_.asset_delete_);
            const bool scene_dir = asset_type_of_(app_.asset_delete_) == AssetType::Scene && p.filename().string().rfind("scene", 0) == 0;
            if (scene_dir) fs::remove_all(p.parent_path(), ec);
            else fs::remove(p, ec);
            if (ec) app_.log_error("Delete failed: " + ec.message());
            else app_.log_info("Deleted " + app_.asset_delete_);
            app_.project_.refresh();
            ctx.close_current_popup();
        }
        ctx.same_line();
        if (ctx.button("Cancel", 80)) ctx.close_current_popup();
        ctx.end_modal();
    }
}

void AssetsPanel::draw_console_area_(imm::Context& ctx, const imm::Box& area) {
    using I = imm::Icon;
    const imm::Box hb = app_.area_header_(ctx, area, 24);
    float x = hb.x + 4;
    auto tab = [&](const char* id, I ic, const char* label, int index) {
        const float w = ctx.text_width(label) + hb.h + 12;
        const imm::Box b{x, hb.y + 2, w, hb.h - 4};
        bool hov = false, held = false;
        if (ctx.invisible_button(id, b, &hov, &held)) app_.bottom_view_ = index;
        if (app_.bottom_view_ == index) ctx.fill_rounded(b, ctx.style.panel_bg);
        else if (hov) ctx.fill_rounded(b, ctx.style.header_hover);
        const glm::vec4 c = app_.bottom_view_ == index ? ctx.style.text : ctx.style.text_dim;
        ctx.icon(ic, {b.x + 4, b.y + 3, b.h - 6, b.h - 6}, c);
        ctx.text_in({b.x + b.h + 2, b.y, w - b.h, b.h}, label, c, 0.0f);
        x += w + 2;
    };
    tab("bt_console", I::Console, "Console", 0);
    tab("bt_timeline", I::Play, "Timeline", 1);
    const imm::Box body{area.x, hb.bottom(), area.w, area.h - hb.h};
    if (app_.bottom_view_ == 1) {
        app_.timeline_panel_->draw_timeline_(ctx, hb, body, x + 10);
    } else {
        app_.timeline_hovered_ = false;
        app_.console_->draw(ctx, hb, body);
    }
}

Node AssetsPanel::lookdev_object_(const std::string& name) {
    Node o = Node::mapping();
    o["name"] = Node(name);
    Node comps = Node::sequence();
    Node t = Node::mapping();
    t["type"] = Node(std::string("Transform"));
    comps.as_seq().push_back(t);
    o["components"] = comps;
    return o;
}

void AssetsPanel::ensure_preview_() {
    if (app_.preview_scene_) return;
    Node doc = Node::mapping();
    Node scene = Node::mapping();
    scene["scene_name"] = Node(std::string("EditorPreview"));
    Node roots = Node::sequence();
    Node key = lookdev_object_("LookdevKey");
    Node l = Node::mapping();
    l["type"] = Node(std::string("DirectionalLight"));
    l["direction"] = make_vec3({-0.45f, -0.55f, -0.7f});
    l["intensity"] = make_float(1.3);
    l["cast_shadows"] = Node(true);
    key["components"].as_seq().push_back(l);
    roots.as_seq().push_back(key);
    auto point = [&](const std::string& name, glm::vec3 pos, double intensity) {
        Node o = lookdev_object_(name);
        o["components"].as_seq()[0]["position"] = make_vec3(pos);
        Node pl = Node::mapping();
        pl["type"] = Node(std::string("PointLight"));
        pl["intensity"] = make_float(intensity);
        pl["range"] = make_float(30.0);
        o["components"].as_seq().push_back(pl);
        roots.as_seq().push_back(o);
    };
    point("LookdevFill", {3.0f, 4.0f, 3.0f}, 30.0);
    point("LookdevRim", {-3.0f, -3.5f, 2.5f}, 25.0);
    Node env = lookdev_object_("LookdevEnvironment");
    Node el = Node::mapping();
    el["type"] = Node(std::string("EnvironmentLight"));
    env["components"].as_seq().push_back(el);
    roots.as_seq().push_back(env);
    auto renderer = [&](const std::string& name, glm::vec3 albedo, double rough) {
        Node o = lookdev_object_(name);
        Node mr = Node::mapping();
        mr["type"] = Node(std::string("MeshRenderer"));
        Node mat = Node::mapping();
        mat["albedo"] = make_color(albedo);
        mat["roughness"] = make_float(rough);
        mr["material"] = mat;
        o["components"].as_seq().push_back(mr);
        roots.as_seq().push_back(o);
    };
    renderer("PreviewGround", glm::vec3(0.28f), 0.9);
    renderer("PreviewObject", glm::vec3(0.72f), 0.55);
    scene["root_objects"] = roots;
    doc["scene"] = scene;
    try {
        coopa::scene::Scene s = coopa::scene::SceneLoader::load_from_node(doc, (app_.project_.assets() / "__preview__.yaml").string());
        app_.preview_scene_ = &app_.engine_.push_scene(std::move(s), false);
        app_.preview_object_ = app_.preview_scene_->find_object("PreviewObject");
        app_.preview_ground_ = app_.preview_scene_->find_object("PreviewGround");
        app_.uploaded_revision_ = 0;
        if (app_.preview_ground_) {
            if (auto* gmr = app_.preview_ground_->get_component<coopa::gfx::engine::components::MeshRenderer>()) {
                const EditMesh disc = make_cylinder(1.8f, 0.04f, 48, true);
                auto cpu = coopa::gfx::engine::data::Mesh::build_cpu(mesh_to_node(disc));
                auto mesh = std::make_shared<coopa::gfx::engine::data::Mesh>(
                    coopa::gfx::engine::data::Mesh::from_cpu(app_.engine_.device(), app_.engine_.allocator(), std::move(cpu)));
                gmr->set_mesh(app_.engine_.assets().create<coopa::gfx::engine::data::Mesh>("editor/lookdev_ground", std::move(mesh)));
            }
        }
    } catch (const std::exception& e) {
        app_.log_error(std::string("Preview scene failed: ") + e.what());
    }
}

EditMesh AssetsPanel::lookdev_shape_() const {
    switch (app_.lookdev_.shape) {
        case 1: return make_uv_sphere(0.5f, 48, 24);
        case 2: {
            EditMesh m = make_cube();
            MeshSelection all;
            all.select_all(m);
            bevel_edges(m, all, 0.12f);
            for (auto& f : m.faces) f.smooth = true;
            return m;
        }
        case 3: return make_plane(1.2f, 1);
        case 4: return make_cylinder(0.45f, 1.0f, 48, true);
        default: return make_shader_ball();   // curves, flat planes and hard edges in one shape
    }
}

void AssetsPanel::place_preview_(const EditMesh& m, bool on_ground) {
    if (!app_.preview_object_ || !app_.preview_object_->get_transform()) return;
    glm::vec3 lo, hi;
    m.bounds(lo, hi);
    const float lift = on_ground ? -lo.z : 0.0f;
    app_.preview_object_->get_transform()->transform().set_position(glm::vec3(0, 0, lift));
    if (app_.preview_ground_ && app_.preview_ground_->get_transform()) {
        app_.preview_ground_->get_transform()->transform().set_position(glm::vec3(0, 0, -0.025f));
        app_.preview_ground_->set_active(on_ground && app_.lookdev_.ground);
    }
}

void AssetsPanel::update_preview_() {
    auto* mr = app_.preview_renderer_();
    if (!mr) return;
    switch (app_.active_type_) {
        case AssetType::Mesh:
            if (!app_.sculpt_active_ && !app_.paint_active_ && app_.uploaded_revision_ != app_.mesh_.geometry_revision) {
                app_.upload_preview_mesh_(app_.mesh_.mesh, "editor/preview_mesh");
                app_.refresh_material_preview_();
                apply_slot_preview_materials_();
                place_preview_(app_.mesh_.mesh, false);
                if (app_.preview_ground_) app_.preview_ground_->set_active(false);   // edit in empty space
                app_.uploaded_revision_ = app_.mesh_.geometry_revision;
            }
            break;
        case AssetType::Material:
            if (app_.uploaded_revision_ != app_.lookdev_.revision) {
                const EditMesh shape = lookdev_shape_();
                app_.upload_preview_mesh_(shape, "editor/lookdev_shape");
                app_.refresh_material_preview_();
                place_preview_(shape, true);
                app_.uploaded_revision_ = app_.lookdev_.revision;
            }
            if (app_.lookdev_.turntable && app_.preview_object_ && app_.preview_object_->get_transform()) {
                app_.lookdev_.angle += 0.4f * (1.0f / 60.0f);
                app_.preview_object_->get_transform()->transform().set_rotation_quat(glm::angleAxis(app_.lookdev_.angle, glm::vec3(0, 0, 1)));
            }
            break;
        case AssetType::Texture:
            if (app_.uploaded_revision_ == 0) {
                const EditMesh plane = make_plane(1.0f, 1);
                app_.upload_preview_mesh_(plane, "editor/texture_plane");
                refresh_texture_preview_();
                place_preview_(plane, false);
                if (app_.preview_ground_) app_.preview_ground_->set_active(false);
                app_.uploaded_revision_ = 1;
            }
            break;
        default: break;
    }
}

std::string AssetsPanel::texture_color_space_(const std::string& rel) {
    const std::string file = fs::path(rel).filename().string();
    int linear = 0, srgb = 0;
    std::function<void(const Node&)> scan = [&](const Node& n) {
        if (n.is_mapping()) {
            for (const auto& kv : n.as_map()) {
                const std::string k = kv.first.is_string() ? kv.first.get_value<std::string>() : std::string();
                if (kv.second.is_string() && k.rfind("texture_", 0) == 0) {
                    const std::string v = kv.second.get_value<std::string>();
                    if (v == rel || fs::path(v).filename().string() == file) (k == "texture_albedo" ? srgb : linear)++;
                } else {
                    scan(kv.second);
                }
            }
        } else if (n.is_sequence()) {
            for (const auto& e : n.as_seq()) scan(e);
        }
    };
    std::vector<std::string> docs = app_.project_.list("materials", ".yaml");
    for (const auto& sc : app_.project_.scenes()) docs.push_back(sc);
    for (const auto& o : app_.project_.list("objects", ".yaml")) docs.push_back(o);
    for (const auto& d : docs) {
        try { scan(coopa::yaml::load_document(coopa::yaml::resolve_variant(app_.project_.absolute(d)))); } catch (...) {}
    }
    if (linear || srgb) return linear > srgb ? "linear" : "srgb";
    std::string low = file;
    std::transform(low.begin(), low.end(), low.begin(), ::tolower);
    for (const char* tag : {"normal", "_nrm", "_n.", "_mr", "metal", "rough", "_ao", "occlusion", "mask", "height", "_orm"}) {
        if (low.find(tag) != std::string::npos) return "linear";
    }
    return "srgb";
}

void AssetsPanel::refresh_texture_preview_() {
    auto* mr = app_.preview_renderer_();
    if (!mr) return;
    // TODO(texture-editor): a texture editor (channel view, resize, compression / sampler
    // settings, painting) belongs here and in draw_texture_properties_(); for now the
    // viewer shows the image on a plane.
    coopa::scene::SceneLoader::ParseContext ctx;
    ctx.scene_path = (app_.project_.assets() / "__preview__.yaml").string();
    ctx.scene_dir = app_.project_.assets().string();
    ctx.search_dirs = {ctx.scene_dir};
    Node m = Node::mapping();
    m["albedo"] = make_color(glm::vec3(1.0f));
    m["roughness"] = make_float(1.0);
    m["metallic"] = make_float(0.0);
    const std::string rel = app_.project_.relative(app_.active_path_);
    m["texture_albedo"] = Node(rel);
    Node spaces = Node::mapping();
    spaces[rel] = Node(texture_color_space_(rel));
    m["texture_color_space"] = spaces;
    mr->material = coopa::gfx::engine::components::PBRMaterial{};
    try {
        coopa::gfx::engine::components::parse_material_value_(m, mr->material, app_.engine_.assets(), ctx);
    } catch (const std::exception& e) {
        app_.log_error(std::string("Texture: ") + e.what());
    }
}

glm::vec3 AssetsPanel::slot_color_(uint32_t slot) {
    static const glm::vec3 palette[] = {{0.72f, 0.72f, 0.72f}, {0.85f, 0.45f, 0.25f}, {0.30f, 0.55f, 0.85f}, {0.45f, 0.75f, 0.35f},
                                        {0.80f, 0.70f, 0.25f}, {0.65f, 0.40f, 0.80f}, {0.30f, 0.75f, 0.75f}, {0.85f, 0.35f, 0.55f}};
    return palette[slot % 8];
}

void AssetsPanel::apply_slot_preview_materials_() {
    auto* mr = app_.preview_renderer_();
    if (!mr) return;
    coopa::scene::SceneLoader::ParseContext ctx;
    ctx.scene_path = (app_.project_.assets() / "__preview__.yaml").string();
    ctx.scene_dir = app_.project_.assets().string();
    ctx.search_dirs = {ctx.scene_dir};
    const uint32_t n = std::max<uint32_t>(1, static_cast<uint32_t>(app_.mesh_.mesh.slots.size()));
    mr->slot_materials.clear();
    for (uint32_t s = 0; s < n; ++s) {
        coopa::gfx::engine::components::PBRMaterial pm;
        Node m = Node::mapping();
        m["albedo"] = make_color(slot_color_(s));
        m["roughness"] = make_float(0.55);
        try {
            if (s < app_.slot_preview_materials_.size() && !app_.slot_preview_materials_[s].empty()) {
                coopa::gfx::engine::components::parse_material_value_(Node(app_.slot_preview_materials_[s]), pm, app_.engine_.assets(), ctx);
            } else {
                coopa::gfx::engine::components::parse_material_value_(m, pm, app_.engine_.assets(), ctx);
            }
        } catch (const std::exception& e) {
            app_.log_warn(std::string("Slot preview material: ") + e.what());
        }
        mr->material_for_mut(s) = pm;
    }
}

void AssetsPanel::add_object_view_lights_() {
    if (!app_.doc_.is_object_asset() || !app_.sync_.scene() || app_.active_type_ == AssetType::UI) return;
    Node key = lookdev_object_("__ObjectViewKey");
    Node l = Node::mapping();
    l["type"] = Node(std::string("DirectionalLight"));
    l["direction"] = make_vec3({-0.45f, -0.55f, -0.7f});
    l["intensity"] = make_float(1.3);
    l["cast_shadows"] = Node(true);
    key["components"].as_seq().push_back(l);
    Node env = lookdev_object_("__ObjectViewEnvironment");
    Node el = Node::mapping();
    el["type"] = Node(std::string("EnvironmentLight"));
    env["components"].as_seq().push_back(el);
    for (const Node* n : {&key, &env}) {
        try {
            auto obj = coopa::scene::SceneLoader::build_object(*n, nullptr, (app_.project_.assets() / "__object_view__.yaml").string());
            coopa::scene::SceneObject* raw = app_.sync_.scene()->add_root_object(std::move(obj));
            app_.sync_.scene()->adopt(*raw);
            raw->start();
        } catch (const std::exception& e) {
            app_.log_warn(std::string("Object view lights: ") + e.what());
        }
    }
}

} // namespace editor
} // namespace toy
