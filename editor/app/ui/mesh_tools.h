/**
 * @file mesh_tools.h
 * @brief Blender's quad-modelling tools in the viewport:
 *
 * - visibility-aware element picking (a BVH over the edited mesh: in Solid shading only
 * what you can see is selectable, unless X-ray is on);
 * - Alt+click edge loop, Ctrl+Alt+click ring (Shift toggles), deferred so that Alt+drag
 * still orbits;
 * - Loop Cut and Slide (Ctrl+R): a hover preview of the ring under the cursor, the wheel or
 * digits set the number of cuts, LMB cuts and slides (RMB leaves it centred);
 * - Edge Slide (G G);
 * - the topology operators (Subdivide, Triangulate, Tris to Quads, Dissolve, Bridge, Add
 * in Edit Mode) and the "Adjust Last Operation" panel that re-runs the last one.
 *
 * One class per panel, owning its own state (see editor/app/README.md). EditorApp holds it
 * behind a forward declaration, so editing this header recompiles this panel and
 * editor_app.cpp -- not every panel.
 */

#ifndef TOYEDITOR_APP_UI_MESH_TOOLS_H
#define TOYEDITOR_APP_UI_MESH_TOOLS_H

#include "editor/app/editor_panel.h"

// Types this panel's declarations name (pre-seeded from the repo's type index;
// prune anything the declarations do not actually need).
#include "editor/mesh/edit_mesh.h"
#include "editor/mesh/mesh_bvh.h"
#include "editor/mesh/mesh_loops.h"
#include "editor/mesh/mesh_ops.h"
#include "editor/mesh/primitives.h"
#include "editor/viewport/gizmo.h"
#include "editor/viewport/modal_transform.h"
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/types/enums.h>

namespace toy::editor {

/** @brief See the file doc. */
class MeshToolsPanel : public EditorPanel {
public:
    using EditorPanel::EditorPanel;

    /// EditorApp owns this panel and drives it; the declarations below keep the access level
    /// they had inside EditorApp's body.
    friend class EditorApp;

    // Called by sibling panels, so part of this panel's interface.
    void mirror_mesh_selection_(int a, bool global);

    // Called by sibling panels, so part of this panel's interface.
    void run_subdivide_(int cuts);
    void run_catmull_clark_(int levels);

    // =================================================================================
    // BVH, visibility and picking
    // =================================================================================

    const TriangleBVH& mesh_bvh_();

    /** @brief Mesh-local ray through window point `px`. */
    void mesh_local_ray_(const ViewProj& vp, glm::vec2 px, glm::vec3& o, glm::vec3& d);

    /** @brief X-ray or Wireframe: surfaces hide nothing, so picks go by screen distance, not ray hits. */
    bool see_through_() const;

    /** @brief Does the surface hide this mesh-local point from the camera? (X-ray: never.) */
    bool mesh_point_visible_(const ViewProj& vp, const glm::vec3& local_p);

    /** @brief The occlusion test itself, regardless of X-ray. */
    bool mesh_point_unoccluded_(const ViewProj& vp, const glm::vec3& local_p);

    /**
     * @brief Per-element visibility for the edit overlay, cached per (geometry, view): 0 hidden,
     *        kFront in view, kBehind behind the surface but shown and selectable (X-ray, which
     *        draws it dimmed). Large meshes (> 60k vertices) skip the test and draw everything.
     */

    const EditVisibility& edit_visibility_(const ViewProj& vp);

    /**
     * @brief The edge under the cursor, as Blender's Loop Cut and edge picking choose it: the
     *        nearest side (on screen) of the face under the cursor; failing a face hit, the
     *        nearest visible edge within 12 px. X-ray: always the nearest edge on screen, in
     *        front or behind (the first face a ray hits means nothing when all show).
     */
    std::optional<Edge> pick_edge_(const ViewProj& vp, glm::vec2 px);

    /** @brief Click selection of one vertex / edge / face (visible ones, unless X-ray). */
    void pick_mesh_element_(glm::vec2 px, bool additive);

    /**
     * @brief X-ray face picking, as Blender's: the face whose centre dot is nearest the click on
     *        screen -- among the faces whose outline contains the click, else any dot within
     *        40 px. Front or behind counts the same; an exact tie (a face straight behind
     *        another) goes to the nearer one.
     */
    std::optional<uint32_t> pick_face_screen_(const ViewProj& vp, glm::vec2 px);

    /** @brief Alt+click (loop) / Ctrl+Alt+click (ring) at `px`; Shift toggles. */
    void loop_select_at_(glm::vec2 px, bool ring, bool additive);

    // =================================================================================
    // Loop Cut and Slide, Edge Slide
    // =================================================================================



public:
    /** @brief Ctrl+R: starts Loop Cut and Slide (hover a ring, wheel for cuts, LMB to cut). */
    void begin_loop_cut();
    bool loop_cut_active() const;
    int loop_cut_cuts() const;

private:
    /** @brief One frame of the Loop Cut tool; owns the mouse (navigation still works). */
    void run_loop_cut_(imm::Context& ctx, const ViewProj& vp, bool hovered);

    /** @brief Starts the Edge Slide modal for `rails` (mesh-local). */
    void start_edge_slide_(const ViewProj& vp, glm::vec2 m, const std::vector<SlideRail>& rails, const std::string& merge_key);

    /** @brief G G: slide the selected edges (vertex mode: the edges between selected vertices). */
    void begin_edge_slide_from_selection_(const ViewProj& vp, glm::vec2 m);

    /** @brief Loop Cut preview (yellow, like Blender) over the edit overlay. */
    void draw_mesh_tool_overlay_(imm::Context& ctx, const ViewProj& vp);

    // =================================================================================
    // Topology operators + Adjust Last Operation
    // =================================================================================

    /** @brief What the "Adjust Last Operation" panel can re-run. */


    /** @brief Records a mesh op for the panel (call right after its mesh_.edit). */
    void record_mesh_op_(LastOp op);

    bool last_op_valid_() const;

    /** @brief Runs a mesh operator as one undo step, recording it for the panel. */
    /** @brief Shift+A in Edit Mode: a primitive added into the mesh at the 3D cursor. */
    void add_mesh_primitive_(const std::string& kind);

    /** @brief Re-runs the last operation with its edited parameters (replacing it in undo). */
    void rerun_last_op_();

    /**
     * @brief Object-mode Add with changed parameters: writes the mesh under a name made of
     *        its parameters (never touching a shared default mesh) and repoints the object.
     */
    void rerun_object_primitive_();

    /** @brief Bottom-left panel: the last operation's parameters (Blender's F9 / redo panel). */
    void draw_last_op_panel_(imm::Context& ctx, const imm::Box& view);

    /** @brief A Loop Cut's slide ended: the panel's Factor, and its validity, follow it. */
    void note_slide_finished_(ModalKind kind, float factor);

    /**
     * @brief Mesh > Mirror: reflects the selection along axis `a` (the world's, or the mesh's
     *        own) through the selection's centre. Wholly selected faces keep facing outward.
     */

    void bridge_selected_();

    void dissolve_selected_edges_();

    /** @brief Dissolve Faces: merges each connected group of selected faces into one. */
    void dissolve_selected_faces_();
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_UI_MESH_TOOLS_H
