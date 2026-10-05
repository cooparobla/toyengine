// editor/app/editor_theme.h -- the editor's own theme colours (Blender's per-editor sections).
//
// uicoopa's imm::Theme carries the widget Style plus free-form colour sections; this file
// gives the editor's sections names and types: `chrome` (top bar, status bar, play
// controls), `viewport` (grid, wires, overlays, navigation gizmo) and `outliner` (type icon
// tints). A theme that leaves a role out keeps the value below.

#pragma once

#include <filesystem>
#include <string>

#include <glm/glm.hpp>

#include <uicoopa/immediate/imm_theme.h>

namespace toy::editor {

struct EditorTheme {
    struct Chrome {
        glm::vec4 topbar_bg{0.137f, 0.137f, 0.137f, 1.0f};
        glm::vec4 statusbar_bg{0.137f, 0.137f, 0.137f, 1.0f};
        glm::vec4 play_group_bg{0.2f, 0.2f, 0.2f, 1.0f};
        glm::vec4 play_tint{0.30f, 0.22f, 0.12f, 1.0f};     ///< viewport header while playing (Unity)
        glm::vec4 setting_override{0.20f, 0.55f, 0.75f, 0.16f};      ///< a setting row the scene overrides
        glm::vec4 setting_override_bar{0.30f, 0.65f, 0.90f, 0.90f};  ///< ...and its left-edge marker
    } chrome;
    struct Viewport {
        glm::vec4 grid{1.0f, 1.0f, 1.0f, 0.055f};
        glm::vec4 grid_major{1.0f, 1.0f, 1.0f, 0.14f};
        glm::vec4 wire{0.80f, 0.80f, 0.82f, 0.90f};          ///< wireframe shading edges
        glm::vec4 edit_wire{0.04f, 0.04f, 0.05f, 0.90f};     ///< edit-mode edges
        glm::vec4 vertex{0.06f, 0.06f, 0.07f, 1.0f};         ///< edit-mode vertices (selected: accent)
        glm::vec4 face_dot{0.10f, 0.10f, 0.12f, 0.90f};
        glm::vec4 object_wire{0.75f, 0.78f, 0.82f, 0.85f};   ///< camera / light / empty glyphs
        glm::vec4 origin_outline{0.0f, 0.0f, 0.0f, 0.6f};
        glm::vec4 overlay_text{0.95f, 0.95f, 0.95f, 0.95f};
        glm::vec4 text_shadow{0.0f, 0.0f, 0.0f, 0.75f};
        glm::vec4 toolbar_bg{0.16f, 0.16f, 0.16f, 0.92f};
        glm::vec4 sidebar_bg{0.19f, 0.19f, 0.19f, 0.95f};
        glm::vec4 nav_backdrop{0.0f, 0.0f, 0.0f, 0.18f};
        glm::vec4 nav_hover{1.0f, 1.0f, 1.0f, 0.10f};
        glm::vec4 nav_label{0.05f, 0.05f, 0.05f, 1.0f};
        glm::vec4 box_select_fill{1.0f, 1.0f, 1.0f, 0.06f};
        glm::vec4 box_select_outline{1.0f, 1.0f, 1.0f, 0.6f};
        glm::vec4 cursor_ring_a{1.0f, 1.0f, 1.0f, 0.9f};     ///< 3D cursor's dashed ring
        glm::vec4 cursor_ring_b{0.9f, 0.2f, 0.2f, 0.9f};
        glm::vec4 cursor_cross{0.0f, 0.0f, 0.0f, 0.8f};
        glm::vec4 gizmo_free{0.85f, 0.85f, 0.85f, 0.8f};     ///< the gizmo's view-plane handle
        glm::vec4 modal_backdrop{0.0f, 0.0f, 0.0f, 0.6f};    ///< G / R / S header readout
    } viewport;
    struct Outliner {
        glm::vec4 active_label{1.0f, 0.9f, 0.7f, 1.0f};
        glm::vec4 mesh{0.96f, 0.62f, 0.32f, 1.0f};
        glm::vec4 light{1.0f, 0.86f, 0.4f, 1.0f};
        glm::vec4 camera{0.55f, 0.85f, 0.55f, 1.0f};
        glm::vec4 terrain{0.55f, 0.75f, 0.45f, 1.0f};
    } outliner;
};

/** @brief Calls `v(section, role, colour&)` for every editor role -- the single list. */
template <class T, class V>
void visit_editor_theme(T& t, V&& v) {
    v("chrome", "topbar_bg", t.chrome.topbar_bg);
    v("chrome", "statusbar_bg", t.chrome.statusbar_bg);
    v("chrome", "play_group_bg", t.chrome.play_group_bg);
    v("chrome", "play_tint", t.chrome.play_tint);
    v("chrome", "setting_override", t.chrome.setting_override);
    v("chrome", "setting_override_bar", t.chrome.setting_override_bar);
    v("viewport", "grid", t.viewport.grid);
    v("viewport", "grid_major", t.viewport.grid_major);
    v("viewport", "wire", t.viewport.wire);
    v("viewport", "edit_wire", t.viewport.edit_wire);
    v("viewport", "vertex", t.viewport.vertex);
    v("viewport", "face_dot", t.viewport.face_dot);
    v("viewport", "object_wire", t.viewport.object_wire);
    v("viewport", "origin_outline", t.viewport.origin_outline);
    v("viewport", "overlay_text", t.viewport.overlay_text);
    v("viewport", "text_shadow", t.viewport.text_shadow);
    v("viewport", "toolbar_bg", t.viewport.toolbar_bg);
    v("viewport", "sidebar_bg", t.viewport.sidebar_bg);
    v("viewport", "nav_backdrop", t.viewport.nav_backdrop);
    v("viewport", "nav_hover", t.viewport.nav_hover);
    v("viewport", "nav_label", t.viewport.nav_label);
    v("viewport", "box_select_fill", t.viewport.box_select_fill);
    v("viewport", "box_select_outline", t.viewport.box_select_outline);
    v("viewport", "cursor_ring_a", t.viewport.cursor_ring_a);
    v("viewport", "cursor_ring_b", t.viewport.cursor_ring_b);
    v("viewport", "cursor_cross", t.viewport.cursor_cross);
    v("viewport", "gizmo_free", t.viewport.gizmo_free);
    v("viewport", "modal_backdrop", t.viewport.modal_backdrop);
    v("outliner", "active_label", t.outliner.active_label);
    v("outliner", "mesh", t.outliner.mesh);
    v("outliner", "light", t.outliner.light);
    v("outliner", "camera", t.outliner.camera);
    v("outliner", "terrain", t.outliner.terrain);
}

/** @brief The editor roles a theme defines (others keep EditorTheme's values). */
inline EditorTheme editor_theme_from(const coopa::ui::imm::Theme& theme) {
    EditorTheme t;
    visit_editor_theme(t, [&](const char* section, const char* role, glm::vec4& c) { c = theme.color(section, role, c); });
    return t;
}

/** @brief Where the editor's bundled themes live (ROOT_DIR/editor/themes). */
inline std::filesystem::path editor_themes_dir() {
    return std::filesystem::path(ROOT_DIR) / "editor" / "themes";
}

/** @brief The theme the editor starts with when the preference names none. */
inline const char* default_theme_id() { return "blender_dark"; }

}  // namespace toy::editor
