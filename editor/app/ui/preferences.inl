// editor/app/ui/preferences.inl -- included inside EditorApp's class body.
//
// Edit > Preferences: the editor's own settings, per user (~/.toyengine_editor.yaml, next to
// the recent projects), never per project. A category list on the left, the category's
// settings on the right; every edit applies at once and is saved when the mouse lets go.
//
//   Interface    theme (tiles with a preview of each), interface scale, tooltip delay
//   Viewport     field of view, gizmo size, grid, ambient occlusion, isolate in Edit Mode
//   Navigation   orbit / zoom / fly speeds, wheel direction, what a trackpad swipe does
//   Snapping     the Ctrl-snap increments and grid-adaptive move snapping
//   Assets       engine assets in the Asset panel, the default sort
//   Keymap       every shortcut, searchable (also Help > Controls)
//
// The numeric / toggle preferences are rows of a table (prefs_table_()) pointing at the live
// members they drive; the few older ones (theme, AO, isolate, asset panel) keep their own
// setters, which save themselves.

    /** @brief One numeric or toggle preference, pointing at the member it drives. */
    struct PrefRow {
        const char* key;        ///< In ~/.toyengine_editor.yaml.
        const char* category;
        const char* label;
        const char* tip;
        float* f = nullptr;     ///< A number...
        bool* b = nullptr;      ///< ...or a toggle.
        float def = 0.0f, lo = 0.0f, hi = 1.0f;
        const char* fmt = "%.2f";
        std::vector<std::string> choices;   ///< A toggle shown as a two-way choice (false, true).
        bool drag = false;                  ///< A drag field (unbounded feel) instead of a slider.
    };

    std::vector<PrefRow> prefs_table_() {
        auto num = [](const char* key, const char* cat, const char* label, const char* tip, float* f, float def, float lo, float hi,
                      const char* fmt, bool drag = false) {
            PrefRow r{key, cat, label, tip};
            r.f = f; r.def = def; r.lo = lo; r.hi = hi; r.fmt = fmt; r.drag = drag;
            return r;
        };
        auto flag = [](const char* key, const char* cat, const char* label, const char* tip, bool* b, bool def,
                       std::vector<std::string> choices = {}) {
            PrefRow r{key, cat, label, tip};
            r.b = b; r.def = def ? 1.0f : 0.0f; r.choices = std::move(choices);
            return r;
        };
        return {
            num("ui_scale", "Interface", "Interface Scale", "Size of the whole editor UI, on top of the display's own scaling", &ui_scale_pref_,
                1.0f, 0.75f, 2.0f, "%.2fx"),
            num("tooltip_delay", "Interface", "Tooltip Delay", "Seconds the mouse rests on something before its tooltip shows",
                &tooltip_delay_pref_, 0.6f, 0.0f, 3.0f, "%.2f s"),
            num("camera_fov", "Viewport", "Field of View", "The viewport camera's vertical field of view (the game's cameras have their own)",
                &camera_.fov, 50.0f, 20.0f, 110.0f, "%.0f deg"),
            num("gizmo_size", "Viewport", "Gizmo Size", "On-screen size of the move / rotate / scale gizmo", &gizmo_.size_px, 90.0f, 40.0f, 200.0f,
                "%.0f px"),
            flag("show_grid", "Viewport", "Show Grid", "The floor grid in the viewport (also in the Overlays popover)", &show_grid_, true),
            num("orbit_speed", "Navigation", "Orbit Speed", "How far a mouse drag or swipe turns the view", &camera_.orbit_speed, 1.0f, 0.1f, 4.0f,
                "%.2fx"),
            num("zoom_speed", "Navigation", "Zoom Speed", "How far one wheel notch, pinch or Ctrl+drag zooms", &camera_.zoom_speed, 1.0f, 0.1f, 4.0f,
                "%.2fx"),
            num("fly_speed", "Navigation", "Fly Speed", "Speed of fly navigation (hold the right button + W A S D Q E)", &camera_.fly_speed, 1.0f,
                0.1f, 10.0f, "%.2fx"),
            flag("invert_zoom", "Navigation", "Invert Wheel Zoom", "Scroll up zooms out instead of in", &invert_zoom_, false),
            flag("trackpad_swipe_pans", "Navigation", "Trackpad Swipe", "What a two-finger swipe does; Shift+swipe does the other",
                 &trackpad_swipe_pans_, false, {"Orbit (Blender)", "Pan"}),
            num("snap_move", "Snapping", "Move Increment", "Ctrl while moving snaps to this many metres (when Adaptive Grid is off)",
                &gizmo_.translate_snap, 0.25f, 0.001f, 100.0f, "%.3f m", true),
            num("snap_rotate", "Snapping", "Rotate Increment", "Ctrl while rotating snaps to this many degrees", &gizmo_.rotate_snap, 15.0f, 0.1f,
                180.0f, "%.1f deg", true),
            num("snap_scale", "Snapping", "Scale Increment", "Ctrl while scaling snaps to this step", &gizmo_.scale_snap, 0.1f, 0.001f, 10.0f, "%.3f",
                true),
            flag("snap_adaptive", "Snapping", "Adaptive Grid", "Move snapping follows the grid's spacing at the current zoom (Blender) instead of "
                                                               "Move Increment", &snap_adaptive_, true),
        };
    }

    /** @brief Reads the table's preferences from `prefs` (missing keys keep their defaults) and applies them. */
    void prefs_load_(const Node& prefs) {
        for (PrefRow& r : prefs_table_()) {
            if (!prefs.contains(r.key)) continue;
            const Node& n = prefs.at(r.key);
            if (r.f && (n.is_float_number() || n.is_integer())) *r.f = std::clamp(as_float(n, r.def), r.lo, r.hi);
            if (r.b && n.is_boolean()) *r.b = n.get_value<bool>();
        }
        prefs_apply_();
    }
    /** @brief Pushes preference members into what they drive (the rest read them each frame). */
    void prefs_apply_() {
        canvas_->context().style.tooltip_delay = tooltip_delay_pref_;
        camera_.apply();
        sync_ui_scale_();
    }
    /** @brief Writes the table's preferences into ~/.toyengine_editor.yaml (keeping every other key). */
    void prefs_save_() {
        Node prefs = Project::load_prefs();
        for (const PrefRow& r : prefs_table_()) {
            if (r.f) prefs[r.key] = make_float(*r.f);
            if (r.b) prefs[r.key] = Node(*r.b);
        }
        Project::save_prefs(prefs);
        prefs_dirty_ = false;
    }

public:
    /** @brief Opens Edit > Preferences on `category`. */
    void open_preferences(const std::string& category = "Interface") {
        prefs_category_ = category;
        pending_modal_ = "Preferences";
    }
    /**
     * @brief Sets one table preference ("orbit_speed", "invert_zoom"...) as the window would,
     *        applies and saves it. False for an unknown key.
     */
    bool set_preference(const std::string& key, const Node& value) {
        for (PrefRow& r : prefs_table_()) {
            if (key != r.key) continue;
            if (r.f) *r.f = std::clamp(as_float(value, r.def), r.lo, r.hi);
            if (r.b) *r.b = value.is_boolean() ? value.get_value<bool>() : as_float(value, r.def) != 0.0f;
            prefs_apply_();
            prefs_save_();
            return true;
        }
        return false;
    }
private:

    // --- the window ----------------------------------------------------------------------

    struct ThemeSwatch { std::string id, name; glm::vec4 bg, panel, header, accent, text, dim, button; };

    /** @brief The theme tiles' colours, read once per window opening. */
    const std::vector<ThemeSwatch>& prefs_theme_swatches_() {
        if (!prefs_themes_.empty()) return prefs_themes_;
        for (const auto& t : imm::list_themes(editor_themes_dir())) {
            try {
                const imm::Theme th = imm::load_theme(t.path);
                const imm::Style& s = th.style;
                prefs_themes_.push_back({t.id, t.name, s.window_bg, s.panel_bg, s.header, s.accent, s.text, s.text_dim, s.button});
            } catch (...) {}
        }
        return prefs_themes_;
    }

    /** @brief A titled block inside the pane: heading, a dim line saying what it's for, a rule. */
    void prefs_section_(imm::Context& ctx, const std::string& title, const std::string& what) {
        ctx.spacing(6);
        ctx.heading(title);
        if (!what.empty()) ctx.label_dim(what);
        const imm::Box r = ctx.next_box(1);
        ctx.fill(r, ctx.style.separator);
        ctx.spacing(4);
    }

    /** @brief A slider filling `b` (the property column, minus the reset arrow). */
    bool prefs_slider_(imm::Context& ctx, const imm::Box& b, float* v, float lo, float hi, const char* fmt) {
        bool hov = false, held = false;
        ctx.invisible_button("slider", b, &hov, &held);
        bool changed = false;
        if (held && b.w > 0) {
            const float nv = lo + (hi - lo) * std::clamp((ctx.mouse().x - b.x) / b.w, 0.0f, 1.0f);
            if (nv != *v) { *v = nv; changed = true; }
        }
        ctx.fill_rounded(b, hov || held ? ctx.style.number_hover : ctx.style.number);
        const float t = hi > lo ? std::clamp((*v - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
        if (t > 0) ctx.fill_rounded({b.x, b.y, std::max(b.h * 0.5f, b.w * t), b.h}, ctx.style.accent, -1,
                                    t > 0.98f ? imm::Context::kAll : imm::Context::kLeft);
        char buf[64];
        std::snprintf(buf, sizeof(buf), fmt, *v);
        ctx.text_in(b, buf, ctx.style.text, 0.0f, true);
        return changed;
    }

    /** @brief The table's rows for `category`, each with a reset arrow while it differs from its default. */
    void prefs_rows_(imm::Context& ctx, const std::string& category) {
        for (PrefRow& r : prefs_table_()) {
            if (category != r.category) continue;
            ctx.push_id(r.key);
            const float rs = ctx.style.row_height;
            bool changed = false;
            imm::Box wb = ctx.property_row(r.label);
            const imm::Box row{ctx.last_label_box().x, wb.y, wb.right() - ctx.last_label_box().x, wb.h};
            wb.w = std::max(20.0f, wb.w - rs - 4);
            if (r.f) {
                changed = r.drag ? ctx.drag_float_box("value", wb, r.f, (r.hi - r.lo) * 0.0005f + 0.001f, r.lo, r.hi, r.fmt)
                                 : prefs_slider_(ctx, wb, r.f, r.lo, r.hi, r.fmt);
            } else if (!r.choices.empty()) {
                int i = *r.b ? 1 : 0;
                if (ctx.combo_box("value", wb, &i, r.choices)) { *r.b = i == 1; changed = true; }
            } else {
                bool hov = false;
                if (ctx.invisible_button("value", {wb.x, wb.y, wb.h, wb.h}, &hov)) { *r.b = !*r.b; changed = true; }
                const imm::Box cb{wb.x, wb.y + 4, wb.h - 8, wb.h - 8};
                ctx.fill_rounded(cb, *r.b ? ctx.style.accent : hov ? ctx.style.field_hover : ctx.style.field, 3);
                if (*r.b) ctx.icon(imm::Icon::Check, cb.shrink(1), ctx.style.check_mark);
            }
            if (ctx.is_hovered(row)) ctx.tooltip(std::string(r.label) + "\n" + r.tip);
            test_rects_[std::string("pref:") + r.key] = wb;
            const bool is_default = r.f ? std::abs(*r.f - r.def) < 1e-4f : *r.b == (r.def != 0.0f);
            if (!is_default &&
                ctx.icon_button("reset", imm::Icon::Restart, "Reset to Default", false, rs - 2, imm::Context::kAll,
                                imm::Box{row.right() - rs + 2, row.y + 1, rs - 2, rs - 2})) {
                if (r.f) *r.f = r.def;
                else *r.b = r.def != 0.0f;
                changed = true;
            }
            if (changed) { prefs_apply_(); prefs_dirty_ = true; }
            ctx.pop_id();
        }
    }

    void draw_preferences_modal_(imm::Context& ctx) {
        using I = imm::Icon;
        const glm::vec2 canvas = ctx.canvas_size();
        const glm::vec2 size{std::min(860.0f, canvas.x - 40.0f), std::min(620.0f, canvas.y - 40.0f)};
        if (!ctx.begin_modal("Preferences", size)) { prefs_themes_.clear(); return; }
        const glm::vec2 top = ctx.cursor();
        const float w = ctx.available_width();
        const float footer_h = ctx.style.row_height + 8;
        const float box_bottom = std::floor((canvas.y - size.y) * 0.5f) + size.y;   // begin_modal() centres the box
        const float body_h = std::max(100.0f, box_bottom - ctx.style.padding - footer_h - top.y);
        const float list_w = 170;
        const imm::Box list{top.x, top.y, list_w, body_h};
        const imm::Box pane{top.x + list_w + 6, top.y, w - list_w - 6, body_h};

        static const std::vector<std::pair<I, std::string>> cats = {
            {I::Palette, "Interface"}, {I::Monitor, "Viewport"}, {I::Hand, "Navigation"},
            {I::Snap, "Snapping"},     {I::Asset, "Assets"},     {I::Keyboard, "Keymap"},
        };
        ctx.begin_region("prefs_categories", list, false, &ctx.style.panel_alt);
        ctx.spacing(4);
        for (const auto& [icon, c] : cats) {
            if (ctx.selectable(c, c == prefs_category_, 0.0f, icon)) prefs_category_ = c;
            test_rects_["prefs:" + c] = ctx.last_rect();
        }
        ctx.end_region();

        ctx.begin_region("prefs_pane", pane, true);
        ctx.push_id(prefs_category_);
        const std::string& cat = prefs_category_;
        if (cat == "Interface") {
            prefs_section_(ctx, "Theme", "The editor's colours. Themes are files in editor/themes; edits to them apply live.");
            const auto& themes = prefs_theme_swatches_();
            const float gap = 8, tile_h = 86;
            const int cols = std::max(1, static_cast<int>((ctx.available_width() + gap) / (150 + gap)));
            const float tw = std::floor((ctx.available_width() - gap * (cols - 1)) / cols);
            for (size_t i = 0; i < themes.size(); i += static_cast<size_t>(cols)) {
                const imm::Box row = ctx.next_box(tile_h + ctx.style.row_height);
                for (int k = 0; k < cols && i + k < themes.size(); ++k) {
                    const ThemeSwatch& t = themes[i + static_cast<size_t>(k)];
                    const imm::Box tile{row.x + k * (tw + gap), row.y, tw, tile_h};
                    bool hov = false;
                    ctx.push_id(t.id);
                    if (ctx.invisible_button("theme_tile", {tile.x, tile.y, tile.w, row.h}, &hov)) set_theme(t.id);
                    ctx.pop_id();
                    test_rects_["pref_theme:" + t.id] = tile;
                    // A tiny editor: top bar, a side panel, rows of text, an accent button.
                    ctx.fill_rounded(tile, t.bg, 5);
                    ctx.fill_rounded({tile.x + 4, tile.y + 4, tile.w - 8, 12}, t.header, 3);
                    ctx.fill_rounded({tile.x + 4, tile.y + 20, tile.w * 0.38f, tile.h - 24}, t.panel, 3);
                    ctx.fill_rounded({tile.x + tile.w * 0.38f + 8, tile.y + 20, tile.w * 0.62f - 12, tile.h - 24}, t.panel, 3);
                    for (int l = 0; l < 4; ++l) {
                        ctx.fill_rounded({tile.x + 9, tile.y + 27 + l * 12.0f, tile.w * 0.38f - 12 - (l % 2) * 14, 4},
                                         l == 1 ? t.accent : (l % 2 ? t.dim : t.text), 2);
                    }
                    ctx.fill_rounded({tile.x + tile.w * 0.38f + 14, tile.y + 27, tile.w * 0.3f, 10}, t.button, 3);
                    ctx.fill_rounded({tile.x + tile.w * 0.38f + 14, tile.y + 42, tile.w * 0.45f, 10}, t.accent, 3);
                    const bool on = t.id == theme_id_;
                    if (on) ctx.outline_rounded(tile.shrink(-2), ctx.style.accent, 6, 2.0f);
                    else if (hov) ctx.outline_rounded(tile.shrink(-1), ctx.style.text_dim, 6, 1.0f);
                    ctx.text_in({tile.x, tile.bottom() + 1, tile.w, ctx.style.row_height}, t.name + (on ? "  (active)" : ""),
                                on ? ctx.style.text : ctx.style.text_dim, 0.0f, true);
                }
            }
            ctx.spacing(4);
            if (ctx.button("Reload Theme", 130, true, I::Restart)) load_theme_(theme_id_);
            ctx.tooltip("Reload Theme\nRe-read the active theme's file");
            prefs_section_(ctx, "Display", "");
            prefs_rows_(ctx, cat);
        } else if (cat == "Viewport") {
            prefs_section_(ctx, "Viewport", "How the 3D view looks while editing. The game's own cameras are set per scene.");
            prefs_rows_(ctx, cat);
            bool ao = viewport_ao_;
            if (ctx.property_bool("Ambient Occlusion", &ao)) set_viewport_ao(ao);
            ctx.tooltip("Ambient Occlusion\nContact shadows in the Solid and Material Preview shading (also in the Shading popover)");
            bool iso = isolate_in_edit_;
            if (ctx.property_bool("Isolate in Edit Mode", &iso)) set_isolate_in_edit(iso);
            ctx.tooltip("Isolate in Edit Mode\nHide everything else while editing a mesh (also in the viewport header)");
        } else if (cat == "Navigation") {
            prefs_section_(ctx, "Mouse & Trackpad", "How the view moves. Middle-drag (or Alt+left-drag) orbits, Shift pans, Ctrl zooms.");
            prefs_rows_(ctx, cat);
            ctx.spacing(6);
            ctx.label_dim(trackpad_swipe_pans_ ? "Trackpad: swipe pans, Shift+swipe orbits, pinch or Ctrl+swipe zooms."
                                               : "Trackpad: swipe orbits, Shift+swipe pans, pinch or Ctrl+swipe zooms.");
        } else if (cat == "Snapping") {
            prefs_section_(ctx, "Snapping", "Hold Ctrl while moving, rotating or scaling to snap (or turn the magnet on in the header).");
            prefs_rows_(ctx, cat);
        } else if (cat == "Assets") {
            prefs_section_(ctx, "Asset Panel", "What the Asset panel and asset pickers list, and in what order.");
            bool eng = show_engine_assets_;
            if (ctx.property_bool("Show Engine Assets", &eng)) set_show_engine_assets(eng);
            ctx.tooltip("Show Engine Assets\nList toyengine's built-in (read-only) assets next to the project's");
            static const std::vector<std::string> sorts = {"Name", "Last Modified", "Tags", "Size"};
            static const AssetSort sort_of[] = {AssetSort::Name, AssetSort::Modified, AssetSort::Tags, AssetSort::Size};
            int si = 0;
            for (int i = 0; i < 4; ++i) if (sort_of[i] == asset_sort_) si = i;
            if (ctx.combo("Sort By", &si, sorts) && si >= 0 && si < 4) set_asset_sort_(sort_of[si], asset_sort_reverse_);
            bool rev = asset_sort_reverse_;
            if (ctx.property_bool("Reverse Order", &rev)) set_asset_sort_(asset_sort_, rev);
        } else if (cat == "Keymap") {
            prefs_section_(ctx, "Keymap", "Every shortcut. On macOS, Cmd works wherever Ctrl is listed.");
            draw_keymap_table_(ctx, prefs_keymap_filter_);
        }
        ctx.pop_id();
        ctx.end_region();

        // Footer.
        ctx.set_cursor_y(top.y + body_h + 6);
        if (ctx.button("Restore Defaults", 150, cat != "Keymap" && cat != "Assets", I::Restart)) {
            for (PrefRow& r : prefs_table_()) {
                if (cat != r.category) continue;
                if (r.f) *r.f = r.def;
                else *r.b = r.def != 0.0f;
            }
            if (cat == "Viewport") { set_viewport_ao(true); set_isolate_in_edit(false); }
            prefs_apply_();
            prefs_dirty_ = true;
        }
        ctx.tooltip("Restore Defaults\nPuts this category's preferences back to the editor's defaults");
        ctx.same_line();
        if (ctx.button("Close", 100)) ctx.close_modal();
        {
            const std::string where = "Saved for you in " + Project::prefs_path().string();
            const imm::Box fb = ctx.last_rect();
            const float x = fb.right() + 12;
            ctx.text_in({x, fb.y, std::max(0.0f, top.x + w - x), fb.h}, where, ctx.style.text_disabled, 0.0f);
        }
        // Drags apply live; the file is written once the mouse lets go.
        if (prefs_dirty_ && !ctx.mouse_down()) prefs_save_();
        ctx.end_modal();
    }

    // --- keymap --------------------------------------------------------------------------

    struct KeyBinding { const char* action; const char* keys; };
    struct KeyGroup { const char* title; std::vector<KeyBinding> keys; };

    static const std::vector<KeyGroup>& keymap_() {
        static const std::vector<KeyGroup> k = {
            {"View Navigation", {
                {"Orbit", "MMB drag | Alt+LMB drag"}, {"Pan", "Shift+MMB drag | Shift+Alt+LMB drag"},
                {"Zoom", "Wheel | Ctrl+MMB drag | Numpad + / -"}, {"Pan vertically / horizontally", "Shift+Wheel | Ctrl+Wheel"},
                {"Orbit with a tilt wheel", "Sideways scroll"}, {"Fly", "Hold RMB + W A S D Q E"},
            }},
            {"Trackpad", {
                {"Orbit", "Two-finger swipe"}, {"Pan", "Shift+swipe"}, {"Zoom", "Pinch | Ctrl+swipe"},
            }},
            {"Views", {
                {"Front / Right / Top", "Numpad 1 | Numpad 3 | Numpad 7"}, {"Opposite view", "Ctrl+Numpad 1 / 3 / 7"},
                {"Toggle orthographic", "Numpad 5"}, {"Through the scene camera", "Numpad 0"}, {"Orbit 15 deg", "Numpad 2 / 4 / 6 / 8"},
                {"Frame selected", "F | Numpad ."}, {"Frame all", "Home"}, {"View menu", "`"}, {"Camera to view", "Ctrl+Alt+Numpad 0"},
            }},
            {"Selection", {
                {"Select", "LMB"}, {"Toggle in selection", "Shift+LMB"}, {"Box select", "LMB drag"}, {"Box add / subtract", "Shift+drag | Ctrl+drag"},
                {"Select all / none", "A | Alt+A"}, {"Invert selection", "Ctrl+I"},
            }},
            {"Transform", {
                {"Move / Rotate / Scale", "G | R | S"}, {"Lock to an axis (twice: local)", "X | Y | Z"}, {"Lock to a plane", "Shift+X / Y / Z"},
                {"Type an exact value", "0-9 . -"}, {"Snap / precise", "Ctrl | Shift"}, {"Confirm", "LMB | Enter"}, {"Cancel", "RMB | Esc"},
                {"Clear location / rotation / scale", "Alt+G | Alt+R | Alt+S"},
            }},
            {"Objects", {
                {"Add", "Shift+A"}, {"Duplicate", "Shift+D"}, {"Delete", "X | Del"}, {"Hide / unhide all / isolate", "H | Alt+H | Shift+H"},
                {"Parent / clear parent", "Ctrl+P | Alt+P"}, {"Edit Mode", "Tab"}, {"Context menu", "RMB"}, {"Rename", "F2"},
            }},
            {"Edit Mode", {
                {"Vertex / Edge / Face", "1 | 2 | 3"}, {"Extrude / Inset / Bevel", "E | I | Ctrl+B"}, {"Fill / Merge / Delete", "F | M | X"},
                {"Select linked", "L | Ctrl+L"}, {"Duplicate", "Shift+D"}, {"UV menu / Normals menu", "U | Alt+N"},
            }},
            {"Viewport", {
                {"Shading menu / wireframe", "Z | Shift+Z"}, {"Snap menu", "Shift+S"}, {"Place the 3D cursor", "Shift+RMB"},
                {"Sidebar / toolbar", "N | T"}, {"Maximize the viewport", "Ctrl+Space"}, {"Tools menu", "Shift+Space"}, {"Stats overlay", "F3"},
            }},
            {"General", {
                {"Save all / save scene as", "Ctrl+S | Ctrl+Shift+S"}, {"Undo / redo", "Ctrl+Z | Ctrl+Shift+Z"}, {"New scene / open scene", "Ctrl+N | Ctrl+O"},
                {"Play / stop", "F5"}, {"Release the mouse from the game", "Esc"}, {"Rebuild code", "Ctrl+Shift+B"}, {"Quit", "Ctrl+Q"},
            }},
        };
        return k;
    }

    /** @brief The keymap as a searchable table: action on the left, key chips on the right. */
    void draw_keymap_table_(imm::Context& ctx, std::string& filter) {
        const std::string needle = settings_search_box_(ctx, "keymap_filter", filter);
        ctx.spacing(4);
        auto lower = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; };
        bool any = false;
        for (const KeyGroup& g : keymap_()) {
            std::vector<const KeyBinding*> rows;
            for (const auto& kb : g.keys) {
                if (needle.empty() || lower(kb.action).find(needle) != std::string::npos || lower(kb.keys).find(needle) != std::string::npos ||
                    lower(g.title).find(needle) != std::string::npos) rows.push_back(&kb);
            }
            if (rows.empty()) continue;
            any = true;
            weather_subhead_(ctx, g.title);
            for (const KeyBinding* kb : rows) {
                const imm::Box row = ctx.next_box(ctx.style.row_height + 2);
                const float split = std::floor(row.w * 0.45f);
                ctx.text_in({row.x, row.y, split - 6, row.h}, kb->action, ctx.style.text, 4.0f);
                // Alternatives are separated by " | "; each is a chip.
                float x = row.x + split;
                std::string keys = kb->keys;
                size_t start = 0;
                ctx.push_clip(row);
                while (start <= keys.size()) {
                    const size_t bar = keys.find(" | ", start);
                    const std::string k = keys.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
                    const float cw = ctx.text_width(k) + 12;
                    const imm::Box chip{x, row.y + 2, cw, row.h - 4};
                    ctx.fill_rounded(chip, ctx.style.button, 4);
                    ctx.fill({chip.x + 2, chip.bottom() - 1, chip.w - 4, 1}, ctx.style.border);
                    ctx.text_in(chip, k, ctx.style.text, 0.0f, true);
                    x += cw + 6;
                    if (bar == std::string::npos) break;
                    start = bar + 3;
                }
                ctx.pop_clip();
            }
        }
        if (!any) ctx.label_dim("No shortcut matches that search.");
    }
