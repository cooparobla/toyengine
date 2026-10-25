/**
 * @file viewport_chrome.h
 * @brief The 3D Viewport's Blender furniture: the header (mode menu, View / Select / Add / Object
 *        menus, orientation, snapping, overlays, the four shading buttons), the toolbar column,
 *        the navigation gizmo, the N sidebar, the view-name overlay text, and Blender-style
 *        glyphs for lights, cameras and empties.
 *
 * One class per panel, owning its own state (see editor/app/README.md). EditorApp holds it
 * behind a forward declaration, so editing this header recompiles this panel and
 * editor_app.cpp -- not every panel.
 */

#ifndef TOYEDITOR_APP_UI_VIEWPORT_CHROME_H
#define TOYEDITOR_APP_UI_VIEWPORT_CHROME_H

#include "editor/app/editor_panel.h"

// Types this panel's declarations name (pre-seeded from the repo's type index;
// prune anything the declarations do not actually need).
#include "editor/viewport/gizmo.h"
#include <coopa/util/transform.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <physxcoopa/components/collider.h>

namespace toy::editor {

/** @brief See the file doc. */
class ViewportChromePanel : public EditorPanel {
public:
    using EditorPanel::EditorPanel;

    /// EditorApp owns this panel and drives it; the declarations below keep the access level
    /// they had inside EditorApp's body.
    friend class EditorApp;

    // --- layout of the overlay furniture (computed before input so clicks on it never
    //     reach the viewport's select / navigate handlers) ---

    static constexpr float kToolSize = 32.0f;

    imm::Box toolbar_rect_(bool mesh_edit) const;
    float sidebar_w_() const;
    imm::Box sidebar_rect_() const;
    imm::Box nav_gizmo_rect_() const;
    /** @brief True if `p` is over viewport furniture (toolbar, nav gizmo, sidebar). */
    /** @brief Material and texture views only look -- no selection, tools or transforms. */
    bool preview_only_view_() const;

    bool over_viewport_chrome_(glm::vec2 p, bool mesh_edit) const;

    // --- header ---

    void draw_viewport_header_(imm::Context& ctx, const imm::Box& hb, bool mesh_edit);

    /** @brief The proportional editing popover: falloff curve, distance mode, radius. */
    void draw_proportional_popover_(imm::Context& ctx);

    void draw_view_menu_(imm::Context& ctx);

    void draw_select_menu_(imm::Context& ctx, bool mesh_edit);

    void draw_add_menu_items_(imm::Context& ctx);

    void draw_object_menu_(imm::Context& ctx);

    /** @brief Mesh > Mirror (and the Ctrl M menu): flip the selection along an axis through the pivot. */
    void draw_mirror_menu_items_(imm::Context& ctx);

    void draw_mesh_menu_(imm::Context& ctx);

    // --- toolbar (T) ---

    void draw_toolbar_(imm::Context& ctx, bool mesh_edit);

    // --- navigation gizmo ---

    void draw_nav_gizmo_(imm::Context& ctx);

    // --- N sidebar ---

    void draw_sidebar_(imm::Context& ctx, bool mesh_edit);

    // --- overlay text and object glyphs ---

    std::string view_name_() const;

    /** @brief Viewport text with Blender's soft drop shadow, legible over any render. */
    void shadow_text_(imm::Context& ctx, glm::vec2 p, const std::string& s, const glm::vec4& c) const;

    void draw_overlay_text_(imm::Context& ctx, const ViewProj& vp);

    /** @brief Blender's glyphs for a non-mesh object: camera frustum, light symbols, empty axes. */
    /**
     * @brief One object's viewport glyph: a camera frustum, a light symbol, or -- for anything
     *        else -- its origin, drawn in the Origin Style at the Origin Size.
     * @param id The object, so the Bone style can read its Transform's bone length.
     */
    void draw_object_glyph_(imm::Context& ctx, const ViewProj& vp, ObjectId id, const Node& node, const glm::mat4& w,
                            const glm::vec4& col);
    /** @brief Pose mode's display for a rig joint: one octahedral bone per child joint. */
    void draw_pose_bones_(imm::Context& ctx, const ViewProj& vp, ObjectId id, const glm::mat4& w, const glm::vec4& col);
    /** @brief The origin glyph itself (the `kind`-less tail of draw_object_glyph_). */
    void draw_origin_glyph_(imm::Context& ctx, const ViewProj& vp, ObjectId id, const glm::mat4& w, const glm::vec4& col);
    /**
     * @brief How far `px` is from `id`'s origin glyph on screen, in pixels; 0 when inside it.
     *
     * The shape the style actually draws, so a bone can be clicked anywhere along its body and
     * an axis anywhere along its line. Falls back to the origin point when no glyph is drawn.
     */
    float origin_glyph_pick_px_(ObjectId id, const ViewProj& vp, const glm::mat4& world, glm::vec2 px);

    /** @brief Object / Edit / Sculpt Mode entries (header dropdown, Ctrl+Tab, viewport RMB in Sculpt). */
    void draw_mode_menu_items_(imm::Context& ctx);

    /**
     * @brief Viewport Shading popover: per shading mode. Solid / Material Preview: ambient
     *        occlusion (the renderer's own AO terms), X-ray. Rendered: the renderer's live
     *        toggles; startup-only features are listed but need a renderer restart
     *        (Properties > Render).
     */
    void draw_shading_popover_(imm::Context& ctx);

    /** @brief Collider Display popover: which colliders the Colliders toggle draws. */
    void draw_colliders_popover_(imm::Context& ctx);

    /** @brief Viewport Overlays popover. */
    void draw_overlays_popover_(imm::Context& ctx);
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_UI_VIEWPORT_CHROME_H
