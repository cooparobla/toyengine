#include "editor/app/editor_app.h"

namespace toy {
namespace editor {

void EditorApp::draw_properties_(imm::Context& ctx, const imm::Box& area) {
    using I = imm::Icon;
    properties_hovered_ = ctx.is_hovered(area);
    const ObjectId id = doc_.primary();
    const Node* obj = id ? doc_.find(id) : nullptr;
    if (id) runtime_sel_.clear();   // a document selection replaces a runtime one
    coopa::scene::SceneObject* runtime_obj = !obj && active_type_ == AssetType::Scene ? runtime_selected_() : nullptr;
    Node shown_mr;
    const bool is_mesh = obj && shown_component_(id, "MeshRenderer", shown_mr);   // an instance's asset counts

    struct TabDef { PropTab tab; I icon; const char* tip; };
    std::vector<TabDef> tabs;
    int object_start = -1;
    const bool scene_like = active_type_ == AssetType::Scene || active_type_ == AssetType::Object;
    if (active_type_ == AssetType::Scene) {
        tabs = {
            {PropTab::Tool, I::Tool, "Tool\nActive tool settings"},
            {PropTab::Render, I::Render, "Render\nThis scene's render settings: overrides of the project's (Edit > Project Settings)"},
            {PropTab::Output, I::Output, "Output\nPackaging; window and jobs are in Project Settings"},
            {PropTab::Scene, I::Scene, "Scene\nThe scene block: name, transforms, startup scene, physics"},
            {PropTab::World, I::World, "World\nSky, ambient light and fog"},
        };
    } else if (active_type_ == AssetType::Object) {
        tabs = {{PropTab::Tool, I::Tool, "Tool\nActive tool settings"}};
    } else if (active_type_ == AssetType::Mesh) {
        tabs = {{PropTab::Tool, I::Tool, "Tool\nMesh tools (Edit Mode) and sculpt brushes (Sculpt Mode)"},
                {PropTab::Data, I::Mesh, "Mesh\nThe mesh asset: statistics, material slots, save"}};
    } else if (active_type_ == AssetType::Material) {
        tabs = {{PropTab::Material, I::Material, "Material\nThe material asset and its lookdev preview"}};
    } else if (active_type_ == AssetType::Texture) {
        tabs = {{PropTab::Data, I::Image, "Texture\nThe texture asset"}};
    } else if (active_type_ == AssetType::Audio) {
        tabs = {{PropTab::Data, I::Play, "Sound\nThe sound's import settings and a preview"}};
    } else if (active_type_ == AssetType::UI) {
        tabs = {{PropTab::Canvas, I::UiCanvas, "Canvas\nPreview resolution, the canvas's scaling, the theme"},
                {PropTab::Bindings, I::Link, "Bindings\nThe named widgets game code reaches (UiHandle), and their signals"}};
        if (obj) {
            object_start = static_cast<int>(tabs.size());
            tabs.push_back({PropTab::Object, I::UiAnchor, "Element\nName, Rect Transform (anchors, position, size), visibility"});
            tabs.push_back({PropTab::Components, I::Component, "Components\nThe element's widgets and composites, Add Component"});
        }
    }
    if (runtime_obj) {
        object_start = static_cast<int>(tabs.size());
        tabs.push_back({PropTab::Object, I::Lock, "Runtime Object\nCreated at runtime by a system -- read-only"});
    }
    if (scene_like && obj) {
        object_start = static_cast<int>(tabs.size());
        tabs.push_back({PropTab::Object, I::Object, "Object\nName, transform, relations, visibility"});
        tabs.push_back({PropTab::Components, I::Component, "Components\nThe object's components (Unity-style), Add Component"});
        tabs.push_back({PropTab::Physics, I::Physics, "Physics\nRigidbody, colliders, joints"});
        if (is_mesh) {
            tabs.push_back({PropTab::Data, I::Mesh, "Mesh\nThe mesh asset this object draws, LODs, statistics"});
            tabs.push_back({PropTab::Material, I::Material, "Materials\nOne material per mesh slot: inline, asset or asset + overrides"});
        }
    }
    // An open game UI theme keeps its tab whatever else is open (ui/theme_editor.inl).
    if (game_theme_.open()) tabs.push_back({PropTab::Theme, I::Palette, "Theme\nThe open game UI theme: colours, fonts, sizes"});
    int active = -1;
    for (size_t i = 0; i < tabs.size(); ++i) if (tabs[i].tab == prop_tab_) active = static_cast<int>(i);
    if (active < 0) { active = object_start >= 0 ? object_start : 0; prop_tab_ = tabs[static_cast<size_t>(active)].tab; }

    // The area's own title bar, like every other area's header.
    const imm::Box ah = area_header_(ctx, area);
    ctx.icon(I::Gear, {ah.x + 6, ah.y + 4, ah.h - 8, ah.h - 8}, ctx.style.text_dim);
    ctx.text_in({ah.x + ah.h + 2, ah.y, ah.w - ah.h - 4, ah.h}, "Properties", ctx.style.text, 0.0f);
    const imm::Box below{area.x, ah.bottom(), area.w, area.h - ah.h};

    const float strip_w = 30;
    std::vector<std::pair<I, std::string>> items;
    for (const auto& t : tabs) items.push_back({t.icon, t.tip});
    const imm::Box strip{below.x, below.y + 4, strip_w, below.h - 8};
    std::vector<int> separators;
    if (object_start > 0) separators.push_back(object_start);
    if (ctx.vertical_tabs("prop_tabs", strip, items, &active, separators)) prop_tab_ = tabs[static_cast<size_t>(active)].tab;
    // A dot on the Render / World / Scene tabs while this scene overrides settings there
    // (the tab layout mirrors imm::Context::vertical_tabs()).
    if (scene_settings_layer_()) {
        const float s = std::min(strip.w - 6, ctx.style.row_height + 5);
        float y = strip.y + 5;
        for (size_t i = 0; i < tabs.size(); ++i) {
            if (std::find(separators.begin(), separators.end(), static_cast<int>(i)) != separators.end()) y += 8;
            const PropTab t = tabs[i].tab;
            const bool dot = (t == PropTab::Render && doc_.scene_setting_count("render") > 0) ||
                             (t == PropTab::Scene && doc_.scene_setting_count("physics") > 0);
            if (dot) ctx.fill_rounded({strip.x + (strip.w + s) * 0.5f - 7, y + 1, 6, 6}, et_.chrome.setting_override_bar, 3);
            y += s + 2;
        }
    }

    // Title row: which tab this is (its icon and name, from the tab's tooltip), then -- dimmed
    // -- what it is showing: the open asset, then the selected object.
    const imm::Box content{below.x + strip_w, below.y, below.w - strip_w, below.h};
    const imm::Box hb{content.x, content.y, content.w, 28};
    const TabDef& cur = tabs[static_cast<size_t>(active)];
    const std::string tab_title = std::string(cur.tip).substr(0, std::string(cur.tip).find('\n'));
    std::string crumb = active_asset_label_();
    if ((scene_like || active_type_ == AssetType::UI) && (prop_tab_ == PropTab::Object || prop_tab_ == PropTab::Components) && obj) {
        crumb += "  >  " + get_string(*obj, "name");
    } else if (runtime_obj && prop_tab_ == PropTab::Object) {
        crumb += "  >  " + runtime_obj->name() + " (runtime)";
    }
    ctx.icon(cur.icon, {hb.x + 8, hb.y + 6, hb.h - 12, hb.h - 12}, ctx.style.text);
    const float title_x = hb.x + hb.h + 4;
    ctx.text_in({title_x, hb.y, hb.w - (title_x - hb.x), hb.h}, tab_title, ctx.style.text, 0.0f);
    const float crumb_x = title_x + ctx.text_width(tab_title) + 10;
    if (crumb_x < hb.right() - 20) {
        ctx.text_in({crumb_x, hb.y, hb.right() - crumb_x - 4, hb.h}, crumb, ctx.style.text_dim, 0.0f);
    }
    ctx.fill({hb.x + 6, hb.bottom() - 1, hb.w - 12, 1}, ctx.style.border);
    ctx.begin_region("prop_body", {content.x, hb.bottom(), content.w, content.h - hb.h}, true);
    if (prop_tab_ == PropTab::Theme) draw_theme_props_(ctx);
    else if (active_type_ == AssetType::UI) {
        switch (prop_tab_) {
            case PropTab::Canvas:     draw_ui_canvas_props_(ctx); break;
            case PropTab::Bindings:   draw_ui_bindings_(ctx); break;
            case PropTab::Object:     draw_ui_object_props_(ctx, id); break;
            default:                  draw_component_list_(ctx, id, false); break;
        }
    }
    else if (active_type_ == AssetType::Material) draw_material_asset_props_(ctx);
    else if (active_type_ == AssetType::Texture) draw_texture_properties_(ctx);
    else if (active_type_ == AssetType::Audio) draw_audio_properties_(ctx);
    else if (active_type_ == AssetType::Mesh && prop_tab_ == PropTab::Data) draw_mesh_asset_props_(ctx);
    else switch (prop_tab_) {
        case PropTab::Tool:       draw_tool_tab_(ctx); break;
        case PropTab::Render:     draw_render_props_(ctx); break;
        case PropTab::Output:     draw_output_props_(ctx); break;
        case PropTab::Scene:      draw_scene_props_(ctx); break;
        case PropTab::World:      draw_world_props_(ctx); break;
        case PropTab::Object:     if (runtime_obj) draw_runtime_object_props_(ctx, *runtime_obj); else draw_object_props_(ctx, id); break;
        case PropTab::Components: draw_component_list_(ctx, id, false); break;
        case PropTab::Physics:    draw_component_list_(ctx, id, true); break;
        case PropTab::Data:       draw_data_props_(ctx, id); break;
        case PropTab::Material:   draw_material_props_(ctx, id); break;
    }
    ctx.end_region();
}

std::string EditorApp::active_asset_label_() const {
    switch (active_type_) {
        case AssetType::Scene: return doc_.scene_name();
        case AssetType::Object: return doc_.scene_name();
        case AssetType::Mesh: return mesh_.name;
        case AssetType::Material: return material_.open() ? fs::path(material_.ref).filename().string() : std::string("material");
        case AssetType::Texture: return active_path_.filename().string();
        case AssetType::Audio: return active_path_.filename().string();
        case AssetType::UI: return doc_.scene_name();
        default: return "";
    }
}

void EditorApp::draw_mesh_asset_props_(imm::Context& ctx) {
    using I = imm::Icon;
    auto& md = mesh_;
    if (ctx.collapsing_header("Mesh", true, nullptr, I::Mesh)) {
        std::string name = md.name;
        if (ctx.input_text("Name", &name) && !name.empty()) md.name = name;
        ctx.label_dim(md.path.empty() ? std::string("(not saved yet: meshes/") + md.name + ".yaml)" : project_.relative(md.path));
        ctx.label_dim(std::to_string(md.mesh.positions.size()) + " verts  " + std::to_string(md.mesh.faces.size()) + " faces  " +
                      std::to_string(md.mesh.triangle_count()) + " tris" + (md.dirty() ? "  (modified)" : ""));
        if (md.mesh.passthrough.is_mapping() && md.mesh.passthrough.contains("lods")) {
            ctx.label_dim(std::to_string(md.mesh.passthrough.at("lods").size()) + " LOD level(s) generated at load");
        }
        if (ctx.button("Save Mesh", -1, md.dirty() || md.path.empty(), I::Save)) save_mesh();
        if (ctx.button("Make Object Asset", -1, true, I::Object)) {
            if (md.path.empty() || md.dirty()) save_mesh();
            if (!md.path.empty()) create_object_asset_from_mesh_(project_.relative(md.path));
        }
        ctx.tooltip("Make Object Asset\nWrites objects/<mesh>.yaml: an object with a MeshRenderer using this mesh");
    }
    draw_material_slots_(ctx);
}

void EditorApp::draw_material_slots_(imm::Context& ctx) {
    using I = imm::Icon;
    if (!ctx.collapsing_header("Material Slots", true, nullptr, I::Material)) return;
    auto& md = mesh_;
    uint32_t count = std::max<uint32_t>(1, static_cast<uint32_t>(md.mesh.slots.size()));
    std::vector<size_t> faces_in(64, 0);
    for (const auto& f : md.mesh.faces) { count = std::max(count, f.slot + 1); if (f.slot < faces_in.size()) ++faces_in[f.slot]; }
    const bool edit = in_edit_mode_();
    auto ensure_names = [&](EditMesh& m) {
        while (m.slots.size() < count) m.slots.push_back(m.slots.empty() ? "default" : "slot" + std::to_string(m.slots.size()));
    };
    const auto materials = material_choices_();
    std::vector<std::string> mat_names = {"(palette colour)"};
    for (const auto& m : materials) mat_names.push_back(fs::path(m).stem().string());
    for (uint32_t sidx = 0; sidx < count; ++sidx) {
        ctx.push_id(static_cast<int64_t>(sidx));
        imm::Box row = ctx.next_box(ctx.style.row_height);
        ctx.circle({row.x + 9, row.y + row.h * 0.5f}, 6, glm::vec4(slot_color_(sidx), 1.0f));
        std::string nm = sidx < md.mesh.slots.size() ? md.mesh.slots[sidx] : (sidx == 0 ? std::string("default") : "slot" + std::to_string(sidx));
        const float bw = 54;
        if (ctx.input_text_box("slot_name", {row.x + 20, row.y, row.w - 20 - bw * 2 - 30, row.h}, &nm) && !nm.empty()) {
            md.edit("Rename Slot", [&](EditMesh& m, MeshSelection&) { ensure_names(m); m.slots[sidx] = nm; }, "slotname");
        }
        ctx.text_in({row.right() - bw * 2 - 28, row.y, 28, row.h}, std::to_string(sidx < faces_in.size() ? faces_in[sidx] : 0), ctx.style.text_dim, 1.0f);
        if (edit) {
            ctx.same_line();
            const imm::Box ab{row.right() - bw * 2 + 2, row.y, bw - 2, row.h};
            bool hov = false, held = false;
            if (ctx.invisible_button("assign", ab, &hov, &held)) {
                md.edit("Assign Material Slot", [&](EditMesh& m, MeshSelection& sel) {
                    ensure_names(m);
                    for (uint32_t f : sel.affected_faces(m)) m.faces[f].slot = sidx;
                });
            }
            ctx.fill_rounded(ab, hov ? ctx.style.button_hover : ctx.style.button);
            ctx.text_in(ab, "Assign", ctx.style.text, 0.0f, true);
            ctx.tooltip("Assign\nPut the selected faces in this slot");
            const imm::Box sb{row.right() - bw + 2, row.y, bw - 2, row.h};
            if (ctx.invisible_button("select", sb, &hov, &held)) {
                md.selection.clear();
                md.selection.mode = SelectMode::Face;
                for (uint32_t f = 0; f < md.mesh.faces.size(); ++f) if (md.mesh.faces[f].slot == sidx) md.selection.faces.insert(f);
            }
            ctx.fill_rounded(sb, hov ? ctx.style.button_hover : ctx.style.button);
            ctx.text_in(sb, "Select", ctx.style.text, 0.0f, true);
            ctx.tooltip("Select\nSelect this slot's faces");
        }
        // Preview material (viewer only; MeshRenderers choose the real ones).
        int pick = 0;
        if (sidx < slot_preview_materials_.size() && !slot_preview_materials_[sidx].empty()) {
            for (size_t k = 0; k < materials.size(); ++k) {
                std::string ref = materials[k].substr(0, materials[k].size() - fs::path(materials[k]).extension().string().size());
                if (ref == slot_preview_materials_[sidx]) pick = static_cast<int>(k) + 1;
            }
        }
        if (ctx.combo("Preview", &pick, mat_names)) {
            if (slot_preview_materials_.size() <= sidx) slot_preview_materials_.resize(sidx + 1);
            slot_preview_materials_[sidx] = pick == 0 ? std::string()
                : materials[static_cast<size_t>(pick - 1)].substr(0, materials[static_cast<size_t>(pick - 1)].size() - 5);
            apply_slot_preview_materials_();
        }
        if (count > 1 && sidx > 0 && ctx.button("Remove Slot", -1, true, I::Trash)) {
            md.edit("Remove Material Slot", [&](EditMesh& m, MeshSelection&) {
                ensure_names(m);
                for (auto& f : m.faces) { if (f.slot == sidx) f.slot = 0; else if (f.slot > sidx) --f.slot; }
                m.slots.erase(m.slots.begin() + sidx);
            });
        }
        ctx.separator();
        ctx.pop_id();
    }
    if (ctx.button("Add Slot", -1, true, I::Plus)) {
        md.edit("Add Material Slot", [&](EditMesh& m, MeshSelection&) {
            ensure_names(m);
            m.slots.push_back("slot" + std::to_string(m.slots.size()));
        });
    }
    if (!edit) ctx.label_dim("Tab into Edit Mode to assign faces to slots.");
}

void EditorApp::draw_material_asset_props_(imm::Context& ctx) {
    using I = imm::Icon;
    if (!material_.open()) { ctx.label_dim("Open a material from the Materials tab."); return; }
    if (ctx.collapsing_header("Surface", true, nullptr, I::Material)) draw_material_editor_(ctx);
    if (ctx.collapsing_header("Preview", true, nullptr, I::ShadeMaterial)) {
        int shape = lookdev_.shape;
        if (ctx.combo("Shape", &shape, {"Shader Ball", "Sphere", "Rounded Cube", "Plane", "Cylinder"}) && shape != lookdev_.shape) {
            lookdev_.shape = shape;
            ++lookdev_.revision;
        }
        if (ctx.checkbox("Turntable", &lookdev_.turntable) && !lookdev_.turntable && preview_object_ && preview_object_->get_transform()) {
            preview_object_->get_transform()->transform().set_rotation_quat(glm::quat(1, 0, 0, 0));
        }
        if (ctx.checkbox("Ground", &lookdev_.ground) && preview_ground_) preview_ground_->set_active(lookdev_.ground);
        ctx.label_dim("The lookdev scene: key / fill / rim lights and an environment light.");
    }
}

void EditorApp::draw_texture_properties_(imm::Context& ctx) {
    using I = imm::Icon;
    if (ctx.collapsing_header("Texture", true, nullptr, I::Image)) {
        ctx.label(active_path_.filename().string());
        ctx.label_dim(project_.relative(active_path_));
        std::error_code ec;
        const auto bytes = fs::file_size(active_path_, ec);
        if (!ec) ctx.label_dim(std::to_string(bytes / 1024) + " KB on disk");
        // TODO(texture-editor): channel / mip views, import settings (sRGB, filtering,
        // compression), resizing and painting go here -- see refresh_texture_preview_().
        ctx.label_dim("Texture editing is not available yet.");
    }
}

void EditorApp::draw_audio_properties_(imm::Context& ctx) {
    using I = imm::Icon;
    if (ctx.collapsing_header("Sound", true, nullptr, I::Play)) {
        ctx.label(active_path_.filename().string());
        ctx.label_dim(project_.relative(active_path_));
        std::error_code ec;
        const auto bytes = fs::file_size(active_path_, ec);
        if (!ec) ctx.label_dim(std::to_string(bytes / 1024) + " KB on disk");
        const bool playing = audio_preview_.is_valid() && engine_.audio().engine().is_voice_active(audio_preview_);
        if (ctx.button(playing ? "Restart" : "Preview", 110, true, I::Play)) preview_audio();
        ctx.same_line();
        if (ctx.button("Stop", 90, playing, I::Stop)) stop_audio_preview();
        if (!engine_.audio().has_output()) ctx.label_dim("No audio output device -- previews are silent.");
    }
    if (ctx.collapsing_header("Import Settings", true, nullptr, I::Gear)) {
        auto& s = audio_import_;
        bool changed = false;
        changed |= ctx.slider_float("Volume", &s.volume, 0.0f, 2.0f);
        changed |= ctx.slider_float("Pitch", &s.pitch, 0.25f, 4.0f);
        changed |= ctx.property_bool("Loop", &s.loop);
        changed |= ctx.property_bool("Force Mono", &s.force_mono);
        ctx.tooltip("Downmix to one channel at load -- required for a sound that plays positioned in 3D");
        changed |= ctx.property_bool("Spatialize", &s.spatialize);
        static const std::vector<std::string> buses = {"Master", "Music", "SFX", "UI"};
        int bus = 0;
        for (size_t i = 0; i < buses.size(); ++i) if (buses[i] == s.default_bus) bus = static_cast<int>(i);
        if (ctx.combo("Default Bus", &bus, buses)) { s.default_bus = buses[static_cast<size_t>(bus)]; changed = true; }
        if (changed) audio_import_dirty_ = true;
        if (ctx.button("Save Import Settings", 180, audio_import_dirty_, I::Save)) {
            try {
                s.save(active_path_.string());
                audio_import_dirty_ = false;
                log_info("Saved " + project_.relative(active_path_) + ".import");
            } catch (const std::exception& e) {
                log_error(std::string("Save import settings failed: ") + e.what());
            }
        }
    }
}

void EditorApp::draw_tool_tab_(imm::Context& ctx) {
    using I = imm::Icon;
    if (in_sculpt_mode_()) { draw_sculpt_tool_panel_(ctx); return; }
    if (in_paint_mode_()) { draw_paint_tool_panel_(ctx); return; }
    static const char* tool_names[] = {"Select Box", "Move", "Rotate", "Scale", "Cursor"};
    static const I tool_icons[] = {I::SelectBox, I::Move, I::Rotate, I::Scale, I::Cursor};
    const int t = static_cast<int>(tool_);
    if (ctx.collapsing_header("Active Tool", true, nullptr, tool_icons[t])) {
        ctx.label(tool_names[t]);
        int orient = static_cast<int>(orient_);
        if (ctx.combo("Orientation", &orient, {"Global", "Local", "Normal"})) orient_ = static_cast<Orientation>(orient);
        ctx.property_bool("Snap (Ctrl inverts)", &snap_on_);
        ctx.property_bool("Increment Follows Grid", &snap_adaptive_);
        ctx.tooltip("Move snapping uses the grid's spacing, which gets finer as you zoom in (Blender)");
        if (snap_adaptive_) {
            char buf[48];
            std::snprintf(buf, sizeof buf, "Move Increment  %g m (grid)", grid_step_());
            ctx.label_dim(buf);
        }
        else ctx.drag_float("Move Increment", &gizmo_.translate_snap, 0.01f, 0.001f, 100.0f);
        ctx.drag_float("Rotate Increment", &gizmo_.rotate_snap, 0.5f, 0.1f, 90.0f, "%.1f");
    }
    if (mesh_edit_view_() && mesh_.open()) {
        if (ctx.collapsing_header("Symmetry", true)) draw_symmetry_panel_(ctx, edit_symmetry_);
        draw_mesh_tools_(ctx);
        draw_vertex_groups_panel_(ctx);
    }
    if (ctx.collapsing_header("Workspace", false, nullptr, I::Asset)) {
        ctx.property_bool("Grid", &show_grid_);
        ctx.property_bool("Gizmos", &show_gizmo_);
        ctx.property_bool("Toolbar (T)", &show_toolbar_);
        ctx.property_bool("Sidebar (N)", &show_sidebar_);
    }
}

const FieldDesc* EditorApp::render_field_(const std::string& key) {
    for (const auto& g : render_settings_groups()) {
        if (g.has_toggle() && g.toggle.key == key) return &g.toggle;
        for (const auto& f : g.fields) if (f.key == key) return &f;
    }
    return nullptr;
}

const Node* EditorApp::setting_value_(Node& sec, const std::string& section, const FieldDesc& f) const {
    if (scene_overridable_(section, f)) if (const Node* o = doc_.scene_setting(section, f.key)) return o;
    return sec.contains(f.key) ? &sec.at(f.key) : nullptr;
}

bool EditorApp::setting_bool_(Node& sec, const std::string& section, const FieldDesc& f) const {
    const Node* n = setting_value_(sec, section, f);
    return n && n->is_boolean() ? n->get_value<bool>() : f.def.x != 0.0f;
}

std::string EditorApp::setting_string_(Node& sec, const std::string& section, const FieldDesc& f) const {
    const Node* n = setting_value_(sec, section, f);
    if (n && n->is_string()) return n->get_value<std::string>();
    if (!f.default_string.empty()) return f.default_string;
    return f.options.empty() ? std::string() : f.options.front();
}

void EditorApp::set_setting_(Node& sec, const std::string& section, const FieldDesc& f, const Node& value) {
    if (scene_overridable_(section, f)) {
        settings_edit_was_scene_ = true;
        const bool back = equals_project_value_(f, sec, value);   // back to the project's value: drop the override
        apply_(doc_.set_scene_setting(section, f.key, back ? nullptr : &value, (back ? "Revert " : "Override ") + section + "." + f.key));
        return;
    }
    const Node before = config_.node;
    sec[f.key] = value;
    commit_setting_(before, section, f, false);
}

bool EditorApp::contains_ci_(std::string text, const std::string& needle) {
    std::transform(text.begin(), text.end(), text.begin(), ::tolower);
    return text.find(needle) != std::string::npos;
}

std::vector<bool> EditorApp::group_matches_(const SettingsGroup& g, const std::string& needle, bool& any) {
    std::vector<bool> m(g.fields.size(), true);
    any = true;
    if (needle.empty()) return m;
    const bool whole = contains_ci_(g.title + " " + g.tip, needle) || (g.has_toggle() && field_matches_(g.toggle, needle));
    any = whole;
    for (size_t i = 0; i < g.fields.size(); ++i) {
        m[i] = whole || field_matches_(g.fields[i], needle);
        any |= m[i];
    }
    return m;
}

void EditorApp::draw_setting_group_(imm::Context& ctx, const SettingsGroup& g, const std::string& section, const InspectorEnv& env,
                         const std::string& needle) {
    Node& sec = config_.section(section);
    bool any = true;
    const std::vector<bool> match = group_matches_(g, needle, any);
    if (!any) return;
    ctx.push_id(g.title);
    // Overrides inside the group are counted on its header, so they show while it is closed.
    const bool toggle_over = g.has_toggle() && scene_overridable_(section, g.toggle) && doc_.scene_setting(section, g.toggle.key);
    const int row_overrides = group_override_count_(g, section) - (toggle_over ? 1 : 0);
    const std::string title = g.title + (row_overrides > 0 ? "   (" + std::to_string(row_overrides) + " overridden)" : "");
    bool open = false;
    if (g.has_toggle()) {
        bool on = setting_bool_(sec, section, g.toggle);
        const bool was = on;
        open = ctx.collapsing_header(title, g.open, nullptr, imm::Icon::None, &on);
        const imm::Box hb = ctx.last_rect();
        std::string tip = g.title + "\n" + g.tip + "\nThe checkbox switches it " + (was ? "off" : "on");
        if (g.toggle.startup_only) tip += " (rebuilds the renderer)";
        if (toggle_over) {
            const Node* proj = sec.contains(g.toggle.key) ? &sec.at(g.toggle.key) : nullptr;
            tip += "\nOverridden by this scene -- Scene: " + std::string(was ? "On" : "Off") + "   Project: " +
                   setting_text_(proj, g.toggle.def.x != 0.0f ? "On" : "Off") + "\nRight-click to revert or apply to the project";
        }
        ctx.tooltip(tip + "\nconfig key: " + section + "." + g.toggle.key);
        test_rects_["setting_group:" + g.title] = hb;
        // A scene override of the switch is marked like an overridden row (band + edge bar).
        if (toggle_over) ctx.fill(hb, et_.chrome.setting_override);
        if (toggle_over || row_overrides > 0) ctx.fill({hb.x, hb.y + 2.0f, 2.0f, hb.h - 4.0f}, et_.chrome.setting_override_bar);
        if (on != was) set_setting_(sec, section, g.toggle, Node(on));
        setting_context_menu_(ctx, hb, g.toggle, sec, section);
    } else {
        open = ctx.collapsing_header(title, g.open);
        const imm::Box hb = ctx.last_rect();
        ctx.tooltip(g.title + "\n" + g.tip);
        test_rects_["setting_group:" + g.title] = hb;
        if (row_overrides > 0) ctx.fill({hb.x, hb.y + 2.0f, 2.0f, hb.h - 4.0f}, et_.chrome.setting_override_bar);
    }
    if (!open && needle.empty()) { ctx.pop_id(); return; }
    ctx.indent(6);
    if (g.has_toggle() && !setting_bool_(sec, section, g.toggle)) ctx.label_dim("Off: these apply once " + g.title + " is on.");
    if (!g.needs.empty()) {
        if (const FieldDesc* need = render_field_(g.needs); need && !setting_bool_(sec, section, *need)) {
            ctx.label_dim("Needs " + need->display() + " on.");
        }
    }
    size_t sub = 0;
    bool visible = true;
    for (size_t i = 0; i < g.fields.size(); ++i) {
        while (sub < g.subheads.size() && g.subheads[sub].index == i) {
            const SettingsSubhead& s = g.subheads[sub++];
            visible = true;
            if (!s.show_key.empty()) {
                const FieldDesc* mode = render_field_(s.show_key);
                visible = mode && setting_string_(sec, section, *mode) == s.show_value;
            }
            size_t end = sub < g.subheads.size() ? g.subheads[sub].index : g.fields.size();
            const bool has_rows = std::any_of(match.begin() + static_cast<std::ptrdiff_t>(i), match.begin() + static_cast<std::ptrdiff_t>(end),
                                              [](bool b) { return b; });
            if (visible && has_rows) {
                ctx.spacing(3);
                const imm::Box hb = ctx.next_box(ctx.style.row_height - 4);
                ctx.text_in(hb, s.title, ctx.style.text_dim, 0.0f);
                const float tw = ctx.text_width(s.title) + 8;
                ctx.fill({hb.x + tw, hb.y + hb.h * 0.5f, std::max(0.0f, hb.w - tw), 1.0f}, ctx.style.separator);
            }
        }
        if (!visible || !match[i]) continue;
        FieldDesc f = g.fields[i];
        f.tooltip += std::string(f.tooltip.empty() ? "" : "\n") + (f.startup_only ? "Changing it rebuilds the renderer.\n" : "") +
                     "config key: " + section + "." + f.key;
        draw_setting_row_(ctx, f, sec, env, section);
    }
    ctx.unindent(6);
    ctx.spacing(4);
    ctx.pop_id();
}

void EditorApp::draw_setting_groups_(imm::Context& ctx, const std::vector<SettingsGroup>& groups, const std::string& section,
                          const std::vector<std::string>& only) {
    const InspectorEnv env = inspector_env_();
    for (const auto& g : groups) {
        if (!only.empty() && std::find(only.begin(), only.end(), g.title) == only.end()) continue;
        draw_setting_group_(ctx, g, section, env);
    }
}

bool EditorApp::draw_render_groups_(imm::Context& ctx, const std::string& needle, const std::string& category_only) {
    const InspectorEnv env = inspector_env_();
    std::string category;
    bool shown_any = false;
    for (const auto& g : render_settings_groups()) {
        if (!category_only.empty() && g.category != category_only) continue;
        bool any = true;
        group_matches_(g, needle, any);
        if (!any) continue;
        if (g.category != category && category_only.empty()) {
            category = g.category;
            int n = 0;
            for (const auto& o : render_settings_groups()) if (o.category == category) n += group_override_count_(o, "render");
            ctx.spacing(shown_any ? 8 : 0);
            ctx.heading(n > 0 ? category + "   (" + std::to_string(n) + " overridden)" : category);
            const imm::Box line = ctx.last_rect();
            ctx.fill({line.x, line.bottom() - 2, line.w, 1}, n > 0 ? et_.chrome.setting_override_bar : ctx.style.separator);
            ctx.spacing(2);
        }
        draw_setting_group_(ctx, g, "render", env, needle);
        shown_any = true;
    }
    return shown_any;
}

std::string EditorApp::settings_search_box_(imm::Context& ctx, const char* id, std::string& filter) {
    const imm::Box sb = ctx.next_box(22);
    ctx.input_text_box(id, sb, &filter, "    Search settings");
    if (ctx.editing_text(id).value_or(filter).empty()) ctx.icon(imm::Icon::Search, {sb.x + 4, sb.y + 4, 14, 14}, ctx.style.text_disabled);
    test_rects_[id] = sb;
    std::string needle = ctx.editing_text(id).value_or(filter);
    std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
    return needle;
}

void EditorApp::rebuild_renderer_button_(imm::Context& ctx, float w) {
    if (ctx.button("Rebuild Renderer", w, true, imm::Icon::Restart)) rebuild_renderer_();
    ctx.tooltip("Rebuild Renderer\nRecreates the render pipeline in place from the current settings. "
                "Settings that need it rebuild on their own; this forces one. The window stays open.");
}

void EditorApp::draw_render_props_(imm::Context& ctx) {
    using I = imm::Icon;
    if (ctx.button("Project Settings...", 150, true, I::Gear)) open_project_settings_("General");
    ctx.tooltip("Project Settings\nThe project's default render, output and physics settings (config.yaml)");
    ctx.same_line();
    rebuild_renderer_button_(ctx, 150);
    if (!scene_settings_layer_()) {
        ctx.label_dim("Render settings are per project (config.yaml), overridable per scene.");
        ctx.label_dim("Open a scene to override them, or use Project Settings.");
        return;
    }
    draw_scene_override_summary_(ctx, "render", "render settings");
    ctx.spacing(2);
    const std::string needle = settings_search_box_(ctx, "render_settings_filter", render_settings_filter_);
    ctx.spacing(4);
    if (!draw_render_groups_(ctx, needle)) ctx.label_dim("No setting matches that search.");
}

void EditorApp::draw_scene_override_summary_(imm::Context& ctx, const std::string& section, const std::string& what) {
    const size_t n = doc_.scene_setting_count(section);
    if (n == 0) {
        ctx.label_dim("Showing the project's " + what + ". An edit here overrides it for this scene only.");
        return;
    }
    const imm::Box b = ctx.next_box(ctx.style.row_height);
    ctx.fill(b, et_.chrome.setting_override);
    ctx.fill({b.x, b.y + 1.0f, 2.0f, b.h - 2.0f}, et_.chrome.setting_override_bar);
    ctx.text_in({b.x + 8, b.y, b.w - 8, b.h}, "This scene overrides " + std::to_string(n) + " " + (n == 1 ? what.substr(0, what.size() - 1) : what), ctx.style.text, 0.0f);
    if (ctx.button("Revert All Scene Overrides", -1, true, imm::Icon::Restart)) {
        settings_edit_was_scene_ = true;
        apply_(doc_.clear_scene_settings(section, "Revert all scene " + section + " overrides"));
    }
    ctx.tooltip("Revert All\nDrops every " + section + " override, so this scene uses the project's values");
    test_rects_["revert_all:" + section] = ctx.last_rect();
}

void EditorApp::draw_world_props_(imm::Context& ctx) {
    draw_weather_section_(ctx);   // ui/weather.inl
    if (scene_settings_layer_()) ctx.label_dim("Edits override the project's values for this scene (tinted).");
    if (weather_on_()) ctx.label_dim("Locked rows are driven by the weather while it is on.");
    ctx.spacing(4);
    draw_setting_groups_(ctx, render_settings_groups(), "render", {"Lighting & Sky", "Clouds", "Fog"});
}

void EditorApp::draw_output_props_(imm::Context& ctx) {
    using I = imm::Icon;
    if (ctx.button("Package Project...", 150, true, I::Package)) open_package_dialog_();
    ctx.tooltip("Package Project\nCopies assets/ with every YAML file encoded to .caml, ready to ship.");
    ctx.same_line();
    if (ctx.button("Project Settings...", 150, true, I::Gear)) open_project_settings_("Output");
    ctx.tooltip("Project Settings\nWindow, jobs and debug settings (config.yaml)");
    ctx.spacing(4);
    ctx.label_dim("Window, jobs and debug settings are project-wide: Edit > Project Settings > Output.");
}

void EditorApp::draw_scene_props_(imm::Context& ctx) {
    using I = imm::Icon;
    if (ctx.collapsing_header("Scene", true, nullptr, I::Scene)) {
        std::string scene_name = doc_.scene_name();
        if (!playing() && ctx.input_text("Name", &scene_name)) apply_(doc_.set_scene_key("scene_name", Node(scene_name)));
        bool auto_t = get_bool(doc_.node().at("scene"), "auto_transform", true);
        if (!playing() && ctx.property_bool("Auto Transform", &auto_t)) apply_(doc_.set_scene_key("auto_transform", Node(auto_t)));
        if (doc_.node().at("scene").contains("inherit_from")) ctx.label_dim("Inherits " + get_string(doc_.node().at("scene"), "inherit_from"));
        ctx.label_dim(doc_.path().empty() ? std::string("Not saved yet") : project_.relative(doc_.path()));
    }
    if (ctx.collapsing_header("Startup", true, nullptr, I::Play)) {
        Node& sc = config_.section("scene");
        std::vector<std::string> scenes;
        for (const auto& s : project_.scenes()) scenes.push_back("assets/" + s);
        const std::string cur = get_string(sc, "default_scene");
        int idx = -1;
        for (size_t i = 0; i < scenes.size(); ++i) if (scenes[i] == cur) idx = static_cast<int>(i);
        if (idx < 0 && !cur.empty()) { scenes.push_back(cur); idx = static_cast<int>(scenes.size()) - 1; }
        const Node before = config_.node;
        if (ctx.combo("Default Scene", &idx, scenes) && idx >= 0) {
            sc["default_scene"] = Node(scenes[static_cast<size_t>(idx)]);
            config_.commit("Default Scene", before, {});
        }
        if (!doc_.path().empty() && ctx.button("Use This Scene at Startup", -1, true, I::Check)) {
            sc["default_scene"] = Node("assets/" + project_.relative(doc_.path()));
            config_.commit("Default Scene", before, {});
        }
    }
    if (ctx.collapsing_header("Physics", true, nullptr, I::Physics)) {
        InspectorEnv env = inspector_env_();
        Node& section = config_.section("physics");
        draw_scene_override_summary_(ctx, "physics", "physics settings");
        for (const auto& g : project_settings_groups()) {
            if (g.title != "Physics") continue;
            for (const auto& f : g.fields) draw_setting_row_(ctx, f, section, env, "physics");
        }
    }
}

void EditorApp::draw_object_props_(imm::Context& ctx, ObjectId id) {
    using I = imm::Icon;
    const Node* obj = doc_.find(id);
    if (!obj) return;
    const bool editable = !playing();
    // Name row with the object's icon.
    {
        imm::Box row = ctx.next_box(ctx.style.row_height + 2);
        auto [icon, tint] = object_icon_(effective_(*obj), ctx.style);
        ctx.icon(icon, {row.x + 2, row.y + 3, row.h - 6, row.h - 6}, tint);
        std::string name = get_string(*obj, "name", "Object");
        const imm::Box nb{row.x + row.h + 2, row.y, row.w - row.h - 2, row.h};
        if (doc_.is_inherited(id)) {
            // Overrides find an inherited child by its name: rename it in the object asset.
            ctx.text_in(nb, name, ctx.style.text_dim, 0.0f);
            ctx.tooltip(name + "\nFrom the object asset -- rename it there");
        } else if (editable && ctx.input_text_box("objname", nb, &name) && !name.empty()) {
            apply_(doc_.set_object_key(id, "name", Node(name), "Rename"));
        }
    }
    if (ctx.collapsing_header("Transform", true, nullptr, I::Orientation)) {
        glm::vec3 p, r, s;
        get_object_transform_(id, p, r, s);
        glm::vec3 np = p, nr = r, ns = s;
        bool active = false, finished = false;
        auto track = [&]() { active |= ctx.last_group_active(); finished |= ctx.last_deactivated(); };
        // Animating (a Timeline clip open on this object's rig): a diamond per channel keys it at
        // the playhead -- filled when it is keyed on this frame.
        const bool animatable = anim_clip_.open() && in_anim_rig_(id) && editable;
        auto key_diamond = [&](const char* idk, const char* prop) {
            if (!animatable) return;
            const imm::Box row = ctx.last_rect();
            const glm::vec2 c{row.x + 8, row.y + 10};
            const bool keyed = anim_keyed_now_(id, prop);
            const imm::Box hit{c.x - 7, c.y - 7, 14, 14};
            bool hov = false, held = false;
            if (ctx.invisible_button(idk, hit, &hov, &held)) anim_key_channel_(id, prop);
            const glm::vec4 col = keyed ? glm::vec4(1.0f, 0.75f, 0.2f, 1) : hov ? ctx.style.text : ctx.style.text_dim;
            const float r = 4.5f;
            if (keyed) {
                ctx.triangle({c.x, c.y - r}, {c.x + r, c.y}, {c.x, c.y + r}, col);
                ctx.triangle({c.x, c.y - r}, {c.x, c.y + r}, {c.x - r, c.y}, col);
            } else {
                ctx.line({c.x, c.y - r}, {c.x + r, c.y}, col);
                ctx.line({c.x + r, c.y}, {c.x, c.y + r}, col);
                ctx.line({c.x, c.y + r}, {c.x - r, c.y}, col);
                ctx.line({c.x - r, c.y}, {c.x, c.y - r}, col);
            }
            ctx.tooltip(keyed ? "Keyed at this frame\nClick to key it again with the current value" : "Insert Keyframe\nKey this channel at the playhead");
        };
        const bool cp = ctx.drag_float_stacked("Location", &np.x, 3, 0.02f, "%.3f m");
        track();
        key_diamond("kd_pos", "position");
        const bool cr = ctx.drag_float_stacked("Rotation", &nr.x, 3, 0.5f, "%.1f");
        track();
        key_diamond("kd_rot", "rotation_quat");
        const bool cs = ctx.drag_float_stacked("Scale", &ns.x, 3, 0.01f, "%.3f");
        track();
        key_diamond("kd_scl", "scale");
        if ((cp || cr || cs) && editable) {
            set_object_transform_(id, np, nr, ns, cp ? "Move" : cr ? "Rotate" : "Scale", active ? "props_transform" : std::string());
        }
        if (finished) doc_.end_merge();
    }
    if (ctx.collapsing_header("Relations", true, nullptr, I::Link)) {
        // Every object but this one and its descendants: built once per (object, revision),
        // not every frame -- it lists the whole scene.
        const auto key = std::make_pair(id, doc_.undo_revision());
        if (key != parent_choices_key_) {
            parent_choices_key_ = key;
            parent_choice_names_ = {"(none)"};
            parent_choice_ids_ = {0};
            for (ObjectId other : doc_.all_ids()) {
                if (other == id || doc_.is_ancestor(id, other)) continue;
                parent_choice_names_.push_back(get_string(*doc_.find(other), "name"));
                parent_choice_ids_.push_back(other);
            }
        }
        const std::vector<std::string>& names = parent_choice_names_;
        const std::vector<ObjectId>& ids = parent_choice_ids_;
        const ObjectId parent = doc_.parent_of(id).value_or(0);
        int idx = 0;
        for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == parent) idx = static_cast<int>(i);
        if (ctx.combo("Parent", &idx, names) && editable && !doc_.is_inherited(id)) {
            const glm::mat4 w = world_of_(id);
            const ObjectId np = ids[static_cast<size_t>(idx)];
            if (doc_.reparent(id, np).scope != ChangeScope::None) set_world_transform_(id, w, np ? world_of_(np) : glm::mat4(1.0f), "Keep Transform");
            queue_rebuild_();
        }
    }
    if (ctx.collapsing_header("Visibility", true, nullptr, I::Eye)) {
        bool shown = !hidden_.count(id);
        if (ctx.property_bool("Show in Viewport", &shown)) { if (shown) { hidden_.erase(id); if (auto* l = sync_.live(id)) l->set_active(get_bool(*doc_.find(id), "active", true)); } else hide_({id}); }
        ctx.tooltip("Show in Viewport\nEditor-only visibility (H / Alt H); not saved");
        bool enabled = get_bool(*doc_.find(id), "active", true);
        if (ctx.property_bool("Enabled in Game", &enabled) && editable) apply_(doc_.set_object_key(id, "active", Node(enabled), "Toggle Active"));
        ctx.tooltip("Enabled in Game\nThe object's `active` flag (Unity's GameObject.SetActive)");
    }
    if (obj->contains("inherit_from")) ctx.label_dim("Prefab: " + get_string(*obj, "inherit_from") + " (edits override)");
}

void EditorApp::draw_component_list_(imm::Context& ctx, ObjectId id, bool physics) {
    using I = imm::Icon;
    const bool editable = !playing();
    InspectorEnv env = inspector_env_();
    const Node* obj = doc_.find(id);
    if (!obj) return;
    // Inside a prefab instance (its root or a child it inherits) the list is the RESOLVED
    // components -- the object asset merged with this instance's overrides -- and every edit
    // is saved as the smallest override on the instance (ui/instances.inl).
    const ObjectId inst_root = doc_.instance_root_of(id);
    const bool instance = inst_root != 0;
    const Node resolved = instance ? effective_(*obj) : Node();
    if (instance) {
        const std::string item = instance_asset_item_(inst_root);
        imm::Box row = ctx.next_box(ctx.style.row_height + 4);
        ctx.fill_rounded(row, imm::with_alpha(ctx.style.accent, 0.18f));
        ctx.icon(I::Link, {row.x + 4, row.y + 4, row.h - 8, row.h - 8}, ctx.style.accent);
        const std::string what = (inst_root == id ? "Instance of " : "Part of ") + strip_yaml_ext(item);
        ctx.text_in({row.x + row.h + 2, row.y, row.w - row.h - 70, row.h}, what, ctx.style.text, 0.0f);
        const imm::Box ob{row.right() - 64, row.y + 2, 60, row.h - 4};
        bool hov = false, held = false;
        if (ctx.invisible_button("open_prefab", ob, &hov, &held)) open_asset(AssetType::Object, item);
        ctx.fill_rounded(ob, hov ? ctx.style.button_hover : ctx.style.button);
        ctx.text_in(ob, "Open", ctx.style.text, 0.0f, true);
        ctx.tooltip("Open\nEdit the object asset itself (every instance follows)");
        ctx.label_dim("Marked values are overrides (right-click: revert / apply)");
    }
    const Node& src = instance ? resolved : *obj;
    if (!src.contains("components")) return;
    const size_t count = src.at("components").size();
    bool any = false;
    for (size_t i = 0; i < count; ++i) {
        obj = doc_.find(id);
        if (!obj) break;
        const Node& list_now = instance ? resolved : *obj;
        if (i >= list_now.at("components").size()) break;
        Node comp = list_now.at("components").as_seq()[i];
        const std::string type = component_type(comp);
        if (type == "Transform") continue;
        if (type == "RectTransform" && active_type_ == AssetType::UI) continue;   // the Element tab edits it
        const ComponentSchema* schema = find_schema(type);
        const bool is_physics = schema && schema->category == "Physics";
        if (is_physics != physics) continue;
        any = true;
        ctx.push_id(static_cast<int64_t>(i));
        bool remove = false;
        const bool removable = editable && (!schema || schema->removable);
        const Node* own = instance ? own_override_(id, comp) : nullptr;
        const bool added = instance && own && !base_component_(id, comp);   // not in the asset at all
        const bool overridden = own != nullptr;
        const bool open = ctx.collapsing_header((type.empty() ? std::string("(untyped)") : type) +
                                                (added ? "  (added)" : overridden ? "  (override)" : ""), true,
                                                removable ? &remove : nullptr, icon_for_component_(type));
        ctx.tooltip(type + "\nRight-click for more");
        if (editable) ctx.open_context_popup_on_last("comp_ctx");
        if (ctx.begin_popup("comp_ctx", 240)) {
            if (instance) {
                if (ctx.menu_item(added ? "Remove Added Component" : "Revert Override", "", nullptr, overridden, I::Restart)) {
                    Node empty = Node::mapping();
                    empty["type"] = Node(type);
                    if (comp.contains("id")) empty["id"] = comp.at("id");
                    write_override_(id, comp, empty);
                }
                if (ctx.menu_item("Apply to Object Asset", "", nullptr, overridden, I::Save)) apply_component_to_asset_(id, comp);
                ctx.tooltip("Apply to Object Asset\nWrite this component's overrides into the object asset (every instance follows)");
            } else {
                if (ctx.menu_item("Move Up", "", nullptr, i > 0, I::ArrowRight)) apply_(doc_.move_component(id, static_cast<int>(i), -1));
                if (ctx.menu_item("Move Down", "", nullptr, i + 1 < count, I::ArrowDown)) apply_(doc_.move_component(id, static_cast<int>(i), +1));
            }
            if (ctx.menu_item("Reset", "", nullptr, schema != nullptr, I::Restart)) {
                Node fresh = default_component(type);
                if (comp.contains("id")) fresh["id"] = comp.at("id");
                if (instance) write_override_(id, comp, minimal_override_(id, fresh));
                else apply_(doc_.set_component(id, static_cast<int>(i), fresh, "Reset " + type));
            }
            if (ctx.menu_item("Copy as YAML", "", nullptr, true, I::Duplicate)) {
                Node c = comp;
                strip_private_keys(c);
                if (ctx.input().set_clipboard) ctx.input().set_clipboard(coopa::yaml::emit(c));
            }
            if (ctx.menu_item("Remove Component", "", nullptr, removable, I::Trash)) remove = true;
            ctx.end_popup();
        }
        if (remove) {
            if (instance) {
                // Removing a component the asset has is an override too: `remove: true`.
                // One the instance added just goes.
                Node ov = Node::mapping();
                ov["type"] = Node(type);
                if (comp.contains("id")) ov["id"] = comp.at("id");
                if (!added) ov["remove"] = Node(true);
                write_override_(id, comp, ov);
            } else {
                apply_(doc_.remove_component(id, static_cast<int>(i)));
            }
            ctx.pop_id();
            break;
        }
        if (open) {
            ctx.indent(6);
            if (type == "DirectionalLight" && weather_drives_sun_()) {
                // The weather aims, colours and dims the scene's first directional light.
                const imm::Box nb = ctx.next_box(ctx.style.row_height);
                ctx.icon(I::Lock, {nb.x + 2, nb.y + 3, nb.h - 6, nb.h - 6}, ctx.style.text_dim);
                ctx.text_in({nb.x + nb.h + 2, nb.y, nb.w - nb.h - 2, nb.h}, "Driven by Weather: direction, colour, intensity", ctx.style.text_dim, 0.0f);
                ctx.tooltip("Driven by Weather\nWhile the scene's weather is on with Drive Sun, the clock aims this light at the sun "
                            "(the moon at night) and sets its colour and intensity every frame. These values come back when it is off.");
            }
            const Node before = comp;
            if (instance) {
                // Overridden fields: a bar beside the label, and a right-click menu.
                env.after_field = [&, own_copy = own ? *own : Node()](imm::Context& c, const std::string& key, const imm::Box& label) {
                    if (!own_copy.is_mapping() || !own_copy.contains(key) || added) return;
                    c.fill({label.x, label.y + 3, 2, label.h - 6}, c.style.accent);
                    c.push_id("ov_" + key);
                    if (editable) c.open_context_popup_in("field_ov", label);
                    if (c.begin_popup("field_ov", 220)) {
                        if (c.menu_item("Revert to Asset Value", "", nullptr, true, I::Restart)) revert_override_field_(id, before, key);
                        if (c.menu_item("Apply to Object Asset", "", nullptr, true, I::Save)) apply_field_to_asset_(id, before, key);
                        c.end_popup();
                    }
                    c.pop_id();
                };
            }
            EditResult r;
            if (type == "MeshRenderer") {
                // The material lives on its own tab, as in Blender.
                std::vector<FieldDesc> fields;
                for (const auto& f : schema->fields) if (f.kind != FieldKind::Material) fields.push_back(f);
                r = draw_fields(ctx, fields, comp, env, true, {"type", "id", "material"});
                if (ctx.button("Material...", -1, true, I::Material)) prop_tab_ = PropTab::Material;
            } else {
                r = draw_component(ctx, comp, env, [this](const std::string& rel) { open_asset(AssetType::Material, rel); });
            }
            env.after_field = nullptr;
            ctx.unindent(6);
            if (r.changed && editable && !(comp == before)) {
                const std::string key = "c" + std::to_string(id) + ":" + std::to_string(i) + ":" + r.key;
                if (instance) write_override_(id, before, minimal_override_(id, comp), r.active ? key : std::string());
                else apply_(doc_.set_component(id, static_cast<int>(i), comp, "Edit " + type + "." + r.key, r.active ? key : std::string()));
            }
            if (r.finished) doc_.end_merge();
        }
        ctx.pop_id();
        ctx.spacing(5);
    }
    if (!any) ctx.label_dim(physics ? "No physics components." : active_type_ == AssetType::UI ? "No components besides its Rect Transform."
                                                                : "No components besides Transform.");
    if (!editable) return;
    ctx.spacing(6);
    if (ctx.button(physics ? "Add Physics Component" : "Add Component", -1, true, I::Plus)) {
        add_component_filter_.clear();
        ctx.open_popup(physics ? "add_phys_comp" : "add_comp", glm::vec2(ctx.last_rect().x, ctx.last_rect().bottom() + 2));
    }
    // What the object already has: an instance's asset components count too.
    auto has_type = [&](const std::string& t) {
        for (const auto& c : src.at("components").as_seq()) if (component_type(c) == t) return true;
        return false;
    };
    for (int which = 0; which < 2; ++which) {
        if (!ctx.begin_popup(which == 0 ? "add_comp" : "add_phys_comp", std::max(220.0f, ctx.last_rect().w))) continue;
        const bool phys_only = which == 1;
        imm::Box sb = ctx.next_box(ctx.style.row_height);
        ctx.input_text_box("comp_search", sb, &add_component_filter_, "Search...");
        std::map<std::string, std::vector<std::string>> by_cat;
        for (const auto& [t, sc] : schemas()) {
            if (t == "Transform" || (sc.unique && has_type(t))) continue;
            if (phys_only && sc.category != "Physics") continue;
            if (active_type_ == AssetType::UI && sc.category.rfind("UI", 0) != 0) continue;   // UI elements take UI components
            if (!add_component_filter_.empty()) {
                std::string a = t, b = add_component_filter_;
                std::transform(a.begin(), a.end(), a.begin(), ::tolower);
                std::transform(b.begin(), b.end(), b.begin(), ::tolower);
                if (a.find(b) == std::string::npos) continue;
            }
            by_cat[sc.category].push_back(t);
        }
        for (const auto& [cat, types] : by_cat) {
            ctx.label_dim(cat);
            for (const auto& t : types) {
                if (ctx.menu_item(t, "", nullptr, true, icon_for_component_(t))) apply_(doc_.add_component(id, default_component(t)));
            }
        }
        if (by_cat.empty()) ctx.label_dim("Nothing matches.");
        ctx.end_popup();
    }
}

void EditorApp::draw_data_props_(imm::Context& ctx, ObjectId id) {
    using I = imm::Icon;
    Node comp;
    if (!shown_component_(id, "MeshRenderer", comp)) return;   // resolved inside an instance
    InspectorEnv env = inspector_env_();
    if (ctx.collapsing_header("Mesh", true, nullptr, I::Mesh)) {
        const ComponentSchema* sc = find_schema("MeshRenderer");
        std::vector<FieldDesc> fields;
        for (const auto& f : sc->fields) if (f.key == "mesh_path") fields.push_back(f);
        const Node before = comp;
        EditResult r = draw_fields(ctx, fields, comp, env, false);
        if (r.changed && !(comp == before) && !playing()) edit_component_(id, comp, "Change Mesh");
        if (const CachedMesh* cm = mesh_for_object_(*doc_.find(id))) {
            ctx.label_dim(std::to_string(cm->mesh.positions.size()) + " vertices   " + std::to_string(cm->edges.size()) + " edges   " +
                          std::to_string(cm->mesh.faces.size()) + " faces   " + std::to_string(cm->mesh.triangle_count()) + " triangles");
            const glm::vec3 d = cm->hi - cm->lo;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "Bounds  %.2f x %.2f x %.2f m", d.x, d.y, d.z);
            ctx.label_dim(buf);
        }
        if (!playing() && ctx.button(in_edit_mode_() ? "Leave Edit Mode (Tab)" : "Edit Mesh (Tab)", -1, true, I::EditMode)) toggle_edit_mode_();
    }
    if (ctx.collapsing_header("Level of Detail", false, nullptr, I::Zoom)) {
        const ComponentSchema* sc = find_schema("MeshRenderer");
        std::vector<FieldDesc> fields;
        for (const auto& f : sc->fields) if (f.key == "lod_bias" || f.key == "lods_enabled" || f.key == "affects_reflection_probes") fields.push_back(f);
        const Node before = comp;
        EditResult r = draw_fields(ctx, fields, comp, env, false);
        if (r.changed && !(comp == before) && !playing()) edit_component_(id, comp, "Edit Mesh Renderer", r.active ? "lod" : std::string());
        if (r.finished) doc_.end_merge();
    }
}

void EditorApp::draw_material_props_(imm::Context& ctx, ObjectId id) {
    using I = imm::Icon;
    Node comp;
    if (!shown_component_(id, "MeshRenderer", comp)) return;   // resolved inside an instance
    // Slot row with a shaded-ball preview, Blender-style.
    {
        imm::Box row = ctx.next_box(44);
        ctx.fill_rounded(row, ctx.style.panel_alt);
        const glm::vec3 alb = material_preview_color_(comp);
        ctx.circle({row.x + 22, row.y + 22}, 16, glm::vec4(alb, 1.0f));
        ctx.circle({row.x + 17, row.y + 17}, 6, glm::vec4(1, 1, 1, 0.35f));
        const std::string label = comp.contains("material") && comp.at("material").is_string() ? comp.at("material").get_value<std::string>()
                                : comp.contains("material") && comp.at("material").contains("base") ? get_string(comp.at("material"), "base") + " (+ overrides)"
                                : std::string("Inline material");
        ctx.text_in({row.x + 46, row.y, row.w - 46, row.h}, label, ctx.style.text, 0.0f);
    }
    InspectorEnv env = inspector_env_();
    const Node before = comp;
    EditResult r = draw_material_field(ctx, comp, env, [this](const std::string& rel) {
        open_asset(AssetType::Material, rel);
    });
    if (r.changed && !(comp == before) && !playing()) {
        edit_component_(id, comp, "Edit Material." + r.key, r.active ? "mat:" + r.key : std::string());
    }
    if (r.finished) doc_.end_merge();
    ctx.spacing(6);
    if (comp.contains("material") && comp.at("material").is_mapping() && !comp.at("material").contains("base") && !playing()) {
        if (ctx.button("Save as Material Asset", -1, true, I::Save)) extract_material_(id, comp);
        ctx.tooltip("Save as Material Asset\nWrites materials/<name>.yaml and references it from this object");
    }
    draw_slot_materials_(ctx, id);
}

void EditorApp::draw_slot_materials_(imm::Context& ctx, ObjectId id) {
    using I = imm::Icon;
    const Node* node = doc_.find(id);
    if (!node) return;
    const CachedMesh* cm = mesh_for_object_(*node);
    if (!cm || cm->mesh.slots.size() <= 1) return;
    if (!ctx.collapsing_header("Slot Materials", true, nullptr, I::Material)) return;
    Node comp;
    if (!shown_component_(id, "MeshRenderer", comp)) return;
    const auto mats = material_choices_();
    std::vector<std::string> names = {"(same as " + cm->mesh.slots[0] + ")"};
    for (const auto& m : mats) names.push_back(fs::path(m).stem().string());
    ctx.label_dim(cm->mesh.slots[0] + ": the material above");
    for (size_t sidx = 1; sidx < cm->mesh.slots.size(); ++sidx) {
        const std::string& slot = cm->mesh.slots[sidx];
        ctx.push_id(static_cast<int64_t>(sidx));
        int pick = 0;
        if (comp.contains("materials") && comp.at("materials").is_mapping() && comp.at("materials").contains(slot) &&
            comp.at("materials").at(slot).is_string()) {
            const std::string cur = comp.at("materials").at(slot).get_value<std::string>();
            for (size_t k = 0; k < mats.size(); ++k) {
                if (mats[k].substr(0, mats[k].size() - fs::path(mats[k]).extension().string().size()) == cur) pick = static_cast<int>(k) + 1;
            }
        }
        if (ctx.combo(slot, &pick, names) && !playing()) {
            Node m = comp.contains("materials") && comp.at("materials").is_mapping() ? comp.at("materials") : Node::mapping();
            if (pick == 0) erase_key(m, slot);
            else m[slot] = Node(mats[static_cast<size_t>(pick - 1)].substr(0, mats[static_cast<size_t>(pick - 1)].size() - 5));
            if (m.size() == 0) erase_key(comp, "materials");
            else comp["materials"] = m;
            edit_component_(id, comp, "Slot Material " + slot);
        }
        ctx.pop_id();
    }
}

glm::vec3 EditorApp::material_preview_color_(const Node& comp) {
    if (!comp.contains("material")) return glm::vec3(0.7f);
    const Node& m = comp.at("material");
    std::string ref;
    if (m.is_string()) ref = m.get_value<std::string>();
    else if (m.is_mapping() && m.contains("albedo")) return get_color(m, "albedo", glm::vec3(0.8f));
    else if (m.is_mapping() && m.contains("base")) ref = get_string(m, "base");
    if (ref.empty()) return glm::vec3(0.8f);
    try {
        const fs::path p = coopa::yaml::resolve_variant(project_.absolute(ref + ".yaml"));
        if (coopa::yaml::document_exists(p)) return get_color(coopa::yaml::load_document(p), "albedo", glm::vec3(0.8f));
    } catch (...) {}
    return glm::vec3(0.8f);
}

} // namespace editor
} // namespace toy
