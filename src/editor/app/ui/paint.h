/**
 * @file paint.h
 * @brief Blender's Vertex Paint and Weight Paint Modes on the active mesh object. They are Sculpt
 *        Mode's siblings (ui/sculpt.h) and share its machinery: the same SculptCache
 *        (adjacency, normals, BVH) and VertexGrid (radius queries) -- the modes are exclusive --
 *        and strokes made of spaced dabs, one undo step per stroke (MeshDocument::begin_live /
 *        end_live).
 *
 * The brushes themselves are pure functions (mesh/paint.h). While painting, the mesh is drawn
 * through a PaintPreview whose colours are rewritten as the stroke goes, with the editor-only
 * `editor_paint` surface shader swapped onto the object's material (restored when the mode is
 * left). Solid shading shows paint through Material Preview's studio look (apply_shading_()).
 *
 * Keys (Blender's): LMB paint, Ctrl inverts (secondary colour / subtract weight), Shift blurs,
 * F / Shift+F radius / strength, X swaps the colours, S samples the colour or weight under the
 * cursor, Shift+K fills the whole mesh, Tab back to Object Mode.
 *
 * One class per panel, owning its own state (see editor/app/README.md). EditorApp holds it
 * behind a forward declaration, so editing this header recompiles this panel and
 * editor_app.cpp -- not every panel.
 */

#ifndef TOYEDITOR_APP_UI_PAINT_H
#define TOYEDITOR_APP_UI_PAINT_H

#include "editor/app/editor_panel.h"

// Types this panel's declarations name (pre-seeded from the repo's type index;
// prune anything the declarations do not actually need).
#include "editor/app/paint_preview.h"
#include "editor/mesh/edit_mesh.h"
#include "editor/mesh/paint.h"
#include "editor/viewport/gizmo.h"
#include <coopa/input/input.h>
#include <toyengine/destruct/fracture.h>
#include <toyengine/render/particle_types.h>

namespace toy::editor {

/** @brief See the file doc. */
class PaintPanel : public EditorPanel {
public:
    using EditorPanel::EditorPanel;

    /// EditorApp owns this panel and drives it; the declarations below keep the access level
    /// they had inside EditorApp's body.
    friend class EditorApp;

    // =================================================================================
    // Modes
    // =================================================================================

    static bool is_paint_mode_(InteractionMode m) { return m == InteractionMode::VertexPaint || m == InteractionMode::WeightPaint; }
    bool in_paint_mode_() const;
    bool in_weight_paint_() const;
    /** @brief Sculpt or a paint mode: brush strokes, no selection or transforms. */
    bool in_brush_mode_() const;

    /** @brief Starts / stops the per-mode machinery when the interaction mode changes. */
    void mode_begin_(InteractionMode m);
    void mode_end_(InteractionMode m);

    PaintSettings& paint_settings_();
    PaintPreview::Show paint_show_() const;

    // =================================================================================
    // Lifecycle
    // =================================================================================

    void paint_begin_();

    void paint_end_();

    /** @brief Every live renderer showing the edited mesh (or the mesh viewer's), keyed by object (0 = viewer). */
    std::vector<std::pair<ObjectId, coopa::gfx::engine::components::MeshRenderer*>> edited_mesh_renderers_();

    /** @brief Shows the paint preview (with the editor_paint shader) on every renderer of the mesh. */
    void assign_paint_preview_();

    /** @brief Puts each renderer's own surface shader back (where it still has the paint one). */
    void restore_paint_materials_();

    /** @brief Per frame (pre-render): follow undo / topology changes, upload painted colours. */
    void paint_frame_();

    // =================================================================================
    // Strokes
    // =================================================================================



    /** @brief A brush's screen radius `radius_px` as a mesh-local distance at `local_hit`. */
    float brush_local_radius_(const ViewProj& vp, const glm::vec3& local_hit, float radius_px);

    /** @brief The vertex group Weight Paint paints, creating "Group" if the mesh has none. */
    uint32_t paint_group_(EditMesh& m);

    void paint_dab_at_(const ViewProj& vp, glm::vec2 px, std::vector<uint32_t>& changed);

    void begin_paint_stroke_(const ViewProj& vp, glm::vec2 px, bool shift, bool ctrl);

    void continue_paint_stroke_(const ViewProj& vp, glm::vec2 px);

    void end_paint_stroke_();

    /** @brief S: picks up the colour (or weight) of the vertex nearest the surface under `px`. */
    void paint_sample_(const ViewProj& vp, glm::vec2 px);

    /** @brief Shift+K / the menu's Fill: the brush colour on every corner, or the brush weight on every vertex. */
    void paint_fill_all_();

    // =================================================================================
    // Input
    // =================================================================================

    /** @brief Paint Modes' mouse and keys (navigation is handled before this). */
    void handle_paint_input_(imm::Context& ctx, const ViewProj& vp, bool hovered);

    /** @brief The brush circle and the F / Shift+F gauge. */
    void draw_paint_overlay_(imm::Context& ctx, const ViewProj& vp);

    // =================================================================================
    // UI
    // =================================================================================

    /** @brief Header widgets in a paint mode: colour or weight, radius, strength. Returns the width used. */
    float draw_paint_header_(imm::Context& ctx, float x, const imm::Box& hb);

    /** @brief The tool column of the toolbar (paint modes). */
    void draw_paint_toolbar_(imm::Context& ctx, const imm::Box& r);

    /** @brief The header's "Paint" / "Weights" menu: whole-mesh operations. */
    void draw_paint_menu_(imm::Context& ctx);

    /**
     * @brief Properties > Tool > Vertex Groups (Weight Paint and Edit Mode): the group list,
     *        add / remove / rename, and in Edit Mode Blender's Assign / Remove / Select / Deselect
     *        on the selected vertices.
     */
    void draw_vertex_groups_panel_(imm::Context& ctx);

    /** @brief Properties > Tool in a paint mode: brush, symmetry, and the mode's data. */
    void draw_paint_tool_panel_(imm::Context& ctx);

public:
    PaintSettings& vertex_paint_settings();
    PaintSettings& weight_paint_settings();
    uint32_t active_vertex_group() const;
    void set_active_vertex_group(uint32_t g);
    bool paint_stroking() const;
    /** @brief The surface shader of the live renderer showing the edited mesh (tests). */
    std::string edited_mesh_shader();

private:
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_UI_PAINT_H
