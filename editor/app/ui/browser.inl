// editor/app/ui/browser.inl -- included inside EditorApp's class body.
//
// The bottom area (Blender's timeline slot): an Asset Browser -- a folder tree of assets/
// beside an icon grid, Blender's file-browser look with Unity's Project-window job -- and
// a Console (Unity's log, with severity icons and filters).

    void draw_bottom_area_(imm::Context& ctx, const imm::Box& area) {
        using I = imm::Icon;
        const imm::Box hb = area_header_(ctx, area);
        static const std::vector<std::string> tabs = {"Asset Browser", "Console"};
        static const std::vector<I> icons = {I::Asset, I::Console};
        ctx.tab_bar("bottom_tabs", {hb.x + 4, hb.y, 320, hb.h}, tabs, &bottom_tab_, &icons, false);
        const imm::Box body{area.x, hb.bottom(), area.w, area.h - hb.h};
        if (bottom_tab_ == 1) { draw_console_(ctx, hb, body); return; }
        draw_asset_browser_(ctx, hb, body);
    }

    /** @brief Icon and kind for an asset path (assets-relative). */
    std::pair<imm::Icon, std::string> asset_kind_of_(const std::string& rel) const {
        using I = imm::Icon;
        const fs::path p(rel);
        const std::string ext = p.extension().string();
        const std::string fname = p.filename().string();
        const std::string dir = p.parent_path().generic_string();
        if (ext == ".png" || ext == ".jpg") return {I::Image, "texture"};
        if (ext == ".ttf" || ext == ".otf") return {I::File, "font"};
        if (ext == ".yaml" || ext == ".caml") {
            if (fname == "config.yaml" || fname == "config.caml") return {I::Gear, "config"};
            if (dir.find("physics_materials") != std::string::npos) return {I::Physics, "physics"};
            if (dir.find("materials") != std::string::npos) return {I::Material, "material"};
            if (dir.find("meshes") != std::string::npos) return {I::Mesh, "mesh"};
            if (fname.rfind("scene", 0) == 0 || dir == "scenes") return {I::Scene, "scene"};
            return {ext == ".caml" ? I::Package : I::File, "yaml"};
        }
        return {I::File, "file"};
    }

    /** @brief Directories under assets/ (relative, "" = root), cached until the next scan. */
    const std::vector<std::string>& asset_dirs_() {
        if (!asset_dirs_cache_.empty()) return asset_dirs_cache_;
        asset_dirs_cache_.push_back("");
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(project_.assets(), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_directory(ec)) asset_dirs_cache_.push_back(project_.relative(it->path()));
        }
        std::sort(asset_dirs_cache_.begin(), asset_dirs_cache_.end());
        return asset_dirs_cache_;
    }

    void draw_folder_tree_(imm::Context& ctx, const std::string& dir, int depth) {
        using I = imm::Icon;
        const auto& dirs = asset_dirs_();
        std::vector<std::string> children;
        for (const auto& d : dirs) {
            if (d.empty() || d == dir) continue;
            const fs::path pd = fs::path(d).parent_path();
            if (pd.generic_string() == dir) children.push_back(d);
        }
        const std::string label = dir.empty() ? std::string("assets") : fs::path(dir).filename().string();
        ctx.push_id(dir.empty() ? std::string("__root") : dir);
        const glm::vec4 folder_col{0.85f, 0.78f, 0.55f, 1.0f};
        auto r = ctx.tree_node(ctx.get_id("dir"), label, children.empty(), browser_dir_ == dir, depth == 0, nullptr, I::Folder, 0.0f,
                               browser_dir_ == dir, &folder_col);
        if (r.clicked) browser_dir_ = dir;
        if (r.open) {
            for (const auto& c : children) draw_folder_tree_(ctx, c, depth + 1);
            ctx.tree_pop();
        }
        ctx.pop_id();
    }

    void draw_asset_browser_(imm::Context& ctx, const imm::Box& hb, const imm::Box& body) {
        using I = imm::Icon;
        // Header right side: breadcrumb, search, rescan.
        const imm::Box search{hb.right() - 220, hb.y + 4, 180, hb.h - 8};
        ctx.input_text_box("asset_search", search, &asset_filter_, "    Search");
        if (asset_filter_.empty()) ctx.icon(I::Search, {search.x + 4, search.y + 3, search.h - 6, search.h - 6}, ctx.style.text_disabled);
        if (ctx.icon_button("rescan", I::Restart, "Rescan\nRe-read the assets folder from disk", false, hb.h - 8, imm::Context::kAll,
                            imm::Box{hb.right() - 32, hb.y + 4, hb.h - 8, hb.h - 8})) {
            project_.refresh();
            asset_dirs_cache_.clear();
        }
        const std::string crumb = "assets" + std::string(browser_dir_.empty() ? "" : " / ") + browser_dir_;
        ctx.text_in({hb.x + 330, hb.y, search.x - hb.x - 340, hb.h}, crumb, ctx.style.text_dim, 0.0f);

        // Left: folder tree.
        const float tree_w = std::min(200.0f, body.w * 0.3f);
        const imm::Box tree{body.x, body.y, tree_w, body.h};
        const glm::vec4 tree_bg = ctx.style.panel_alt;
        ctx.begin_region("asset_tree", tree, true, &tree_bg);
        draw_folder_tree_(ctx, "", 0);
        ctx.end_region();

        // Right: grid of the folder's files (or search results across all of assets/).
        const imm::Box grid{tree.right(), body.y, body.w - tree_w, body.h};
        ctx.begin_region("asset_grid", grid, true);
        std::vector<std::string> files;
        std::error_code ec;
        if (!asset_filter_.empty()) {
            for (const auto& f : project_.list("", "")) if (f.find(asset_filter_) != std::string::npos) files.push_back(f);
        } else {
            const fs::path d = project_.assets() / browser_dir_;
            for (auto it = fs::directory_iterator(d, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                const std::string name = it->path().filename().string();
                if (name.empty() || name[0] == '.' || name.find(".tmp~") != std::string::npos || name.find(".spv") != std::string::npos) continue;
                files.push_back(project_.relative(it->path()));
            }
            std::sort(files.begin(), files.end());
        }
        const float tw = 92, th = 92;
        const int cols = std::max(1, static_cast<int>((ctx.available_width() + 4) / (tw + 4)));
        int col = 0;
        for (const auto& f : files) {
            const auto [icon, kind] = asset_kind_of_(f);
            if (col > 0) ctx.same_line();
            ctx.push_id(f);
            const glm::vec4* tint = nullptr;
            static const glm::vec4 mesh_tint{0.96f, 0.62f, 0.32f, 1.0f}, mat_tint{0.92f, 0.45f, 0.45f, 1.0f}, scene_tint{0.6f, 0.75f, 0.95f, 1.0f};
            if (kind == "mesh") tint = &mesh_tint;
            else if (kind == "scene") tint = &scene_tint;
            (void)mat_tint;
            const bool sel = selected_asset_ == f;
            if (ctx.tile("t", icon, fs::path(f).filename().string(), sel, tw, th, tint)) {
                if (sel && ctx.time() - asset_click_time_ < 0.4) open_asset_(kind, f);
                selected_asset_ = f;
                asset_click_time_ = ctx.time();
            }
            ctx.tooltip(fs::path(f).filename().string() + "\n" + f + "  (" + kind + ")  -- double-click to open, drag into the viewport");
            ctx.drag_source("asset", f, fs::path(f).filename().string());
            if (ctx.open_context_popup_on_last("asset_ctx")) selected_asset_ = f;
            if (ctx.begin_popup("asset_ctx", 200)) {
                if (ctx.menu_item("Open", "", nullptr, true, I::Folder)) open_asset_(kind, f);
                if (kind == "mesh" && ctx.menu_item("Add to Scene", "", nullptr, !playing(), I::Plus)) add_mesh_to_scene_(f);
                if (kind == "material" && ctx.menu_item("Assign to Selected", "", nullptr, !playing(), I::Material)) assign_material_(f);
                if (ctx.menu_item("Copy Path", "", nullptr, true, I::Duplicate)) { if (ctx.input().set_clipboard) ctx.input().set_clipboard(f); }
                ctx.end_popup();
            }
            ctx.pop_id();
            col = (col + 1) % cols;
        }
        if (files.empty()) ctx.label_dim(asset_filter_.empty() ? "This folder is empty." : "Nothing matches.");
        ctx.end_region();
    }

    void draw_console_(imm::Context& ctx, const imm::Box& hb, const imm::Box& body) {
        using I = imm::Icon;
        // Header: severity filters + clear (Unity's console toolbar).
        float x = hb.right() - 30;
        const float s = hb.h - 8;
        if (ctx.icon_button("clear", I::Trash, "Clear\nEmpty the console", false, s, imm::Context::kAll, imm::Box{x, hb.y + 4, s, s})) log_.clear();
        x -= s + 10;
        const I sev_icons[3] = {I::Info, I::Warning, I::Error};
        const char* sev_tips[3] = {"Info\nShow messages", "Warnings\nShow warnings", "Errors\nShow errors"};
        for (int i = 2; i >= 0; --i) {
            ctx.push_id(i);
            if (ctx.icon_button("sev", sev_icons[i], sev_tips[i], console_show_[i], s, imm::Context::kAll, imm::Box{x, hb.y + 4, s, s})) {
                console_show_[i] = !console_show_[i];
            }
            ctx.pop_id();
            x -= s + 2;
        }
        ctx.begin_region("console", body, true);
        for (auto it = log_.rbegin(); it != log_.rend(); ++it) {
            if (!console_show_[std::clamp(it->first, 0, 2)]) continue;
            imm::Box row = ctx.next_box(ctx.style.row_height);
            ctx.icon(sev_icons[std::clamp(it->first, 0, 2)], {row.x + 2, row.y + 3, row.h - 6, row.h - 6},
                     it->first == 0 ? ctx.style.text_dim : glm::vec4(1));
            const glm::vec4 c = it->first == 2 ? ctx.style.error : it->first == 1 ? ctx.style.warning : ctx.style.text;
            ctx.text_in({row.x + row.h + 2, row.y, row.w - row.h, row.h}, it->second, c, 0.0f);
        }
        if (log_.empty()) ctx.label_dim("No messages.");
        ctx.end_region();
    }
