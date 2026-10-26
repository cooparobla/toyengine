/**
 * @file instance_paint.h
 * @brief Instance Paint Mode: brushing instances of object assets onto any surface in the scene
 *        (toyengine/instance/instance_painter.h).
 *
 * Entered from the mode menu, or the InstancePainter component's "Paint Instances" button, with
 * a painter object active. Terrain Mode's sibling, and built the same way: brushes sized in
 * metres, drawn as a circle draped over whatever is under the cursor, with one undo step per
 * stroke in the scene's timeline. The records are written to the painter's `.instances` file
 * with the scene (Ctrl S).
 *
 * Unlike Terrain Mode it is not bound to one surface. Each candidate point is raycast against
 * **everything** -- both kinds of terrain and every mesh object -- through a
 * `SurfaceRaycaster` (editor/viewport/surface_raycast.h), so one stroke can run off a hillside
 * and up a rock without changing tools.
 *
 * Tools (`editor/instance/instance_brush.h`): Paint (Ctrl erases), Erase, Place (one per click)
 * and Cluster (a clump per click). Snap is a modifier on all of them, not a tool: it moves
 * placement onto a grid and quantises yaw, for fences, walls and modular kits.
 *
 * Properties > Tool in this mode is the painter panel: Tools, Brush, Snap, Cluster and the
 * prototype palette.
 *
 * One class per panel, owning its own state (see editor/app/README.md). EditorApp holds it
 * behind a forward declaration, so editing this header recompiles this panel and
 * editor_app.cpp -- not every panel.
 */

#ifndef TOYEDITOR_APP_UI_INSTANCE_PAINT_H
#define TOYEDITOR_APP_UI_INSTANCE_PAINT_H

#include "editor/app/editor_panel.h"

// Types this panel's declarations name.
#include "editor/core/undo.h"
#include "editor/instance/instance_brush.h"
#include "editor/instance/instance_select.h"
#include "editor/mesh/mesh_bvh.h"
#include "editor/viewport/gizmo.h"
#include "editor/viewport/surface_raycast.h"
#include <coopa/input/input.h>
#include <toyengine/instance/instance_painter.h>
#include <toyengine/instance/instance_set.h>
#include <toyengine/world/terrain_field.h>

#include <map>
#include <memory>
#include <set>

namespace toy::editor {

/** @brief See the file doc. */
class InstancePaintPanel : public EditorPanel {
public:
    using EditorPanel::EditorPanel;

    /// EditorApp owns this panel and drives it; the declarations below keep the access level
    /// they had inside EditorApp's body.
    friend class EditorApp;

    // =================================================================================
    // Lifecycle
    // =================================================================================

    bool in_instance_mode_() const { return instance_mode_ && instance_object_ != 0; }

    /** @brief Enters or leaves the mode (the mode menu's checkbox). */
    void set_instance_mode(bool on);

    /** @brief The painter component of the live painter object (null when it is gone). */
    toy::instance::InstancePainter* painter_live_() const;

    /** @brief True when `id` carries an InstancePainter in the document. */
    bool is_painter_object_(ObjectId id) const;

    void enter_instance_mode_(ObjectId id);
    void exit_instance_mode_();

    /** @brief Per frame: leaves the mode when the painter went away or play started. */
    void instance_frame_();

    // =================================================================================
    // Strokes
    // =================================================================================

    struct InstanceStroke {
        bool active = false;
        bool erase = false;
        InstanceTool tool = InstanceTool::Paint;
        glm::vec2 last_px{0.0f};
        std::uint32_t seed = 1;
        std::uint32_t dab = 0;      ///< Dab index within the stroke: part of each candidate's seed.
        std::shared_ptr<toy::instance::InstanceSetData> data;
        /// Every chunk the stroke has touched, and its records as the stroke began -- the undo
        /// patch is cut from these once it ends.
        std::vector<std::pair<toy::instance::ChunkCoord, std::vector<toy::instance::InstanceRecord>>> before;
        std::set<std::pair<int, int>> touched;
    };

    /** @brief Where the ray through `px` meets any surface in the scene. */
    std::optional<SurfaceHit> surface_hit_(const ViewProj& vp, glm::vec2 px);

    // =================================================================================
    // Selecting (the Select tool)
    // =================================================================================

    /** @brief A rubber-band box drag, or the click that did not turn into one. */
    struct SelectDrag {
        bool active = false;
        bool add = false;       ///< Shift: add to the selection instead of replacing it.
        bool subtract = false;  ///< Ctrl: remove from it.
        glm::vec2 from{0.0f}, to{0.0f};
        /** @brief Travelled far enough to mean a box rather than a click. */
        bool is_box() const { return glm::distance(from, to) > 4.0f; }
    };

    /**
     * @brief Every selectable instance's screen position this frame.
     *
     * Projected from the instance's visual CENTRE (its position plus half the prototype's
     * height), not its origin, so clicking a tree hits the tree rather than the ground it
     * stands on. Anything projecting outside the viewport is dropped -- what you can see is
     * what you can click, which is the only rule that holds at every zoom level.
     */
    struct Pickable {
        toy::instance::InstanceRef ref;
        glm::vec2 screen{0.0f};
        float depth = 0.0f;   ///< Distance along the view direction, for nearest-first ties.
    };

    void gather_pickables_(const ViewProj& vp, std::vector<Pickable>& out) const;

    /** @brief The instance nearest `px` within a few pixels, or nullopt. */
    std::optional<toy::instance::InstanceRef> pick_instance_(const ViewProj& vp, glm::vec2 px) const;

    void handle_select_input_(imm::Context& ctx, const ViewProj& vp, bool hovered);
    void apply_select_drag_(const ViewProj& vp);

    /** @brief The selection outlines and the rubber-band box. */
    void draw_selection_overlay_(imm::Context& ctx, const ViewProj& vp);

    /** @brief Properties > Tool's Selection block: what you can do to what is selected. */
    void draw_selection_panel_(imm::Context& ctx, toy::instance::InstancePainter& painter);

    /** @brief Applies `fn` to every selected record as ONE undo step. */
    void edit_selection_(const std::string& label, const std::function<void(toy::instance::InstanceRecord&)>& fn,
                         const std::string& merge = {});

    /** @brief Turns the selection about its own centre (the Selection panel's Turn). */
    void turn_selection_(float radians);
    void delete_selection_();
    /** @brief Re-raycasts every selected instance onto whatever is under it now. */
    void drop_selection_to_surface_();

    /** @brief Clears the selection when the records changed under it (see instance_select.h). */
    void sync_selection_();

    /** @brief Records a chunk's starting state, once per stroke, before it is first edited. */
    void remember_chunk_(toy::instance::ChunkCoord c);

    void instance_dab_at_(const ViewProj& vp, glm::vec2 px);
    void instance_begin_stroke_(const ViewProj& vp, glm::vec2 px, bool ctrl);
    void instance_continue_stroke_(const ViewProj& vp, glm::vec2 px);
    void instance_end_stroke_();

    /** @brief scene_undo_() / scene_redo_()'s painted-instance half. */
    void instance_apply_undo_(bool redo);

    /** @brief Writes every painter's records to its own file (Ctrl S). */
    void save_instances_();

    // =================================================================================
    // Input / overlay / UI
    // =================================================================================

    void handle_instance_input_(imm::Context& ctx, const ViewProj& vp, bool hovered);

    /** @brief The brush circle draped over the surface, the snap grid, and ghost candidates. */
    void draw_instance_overlay_(imm::Context& ctx, const ViewProj& vp);

    /** @brief Header widgets in Instance Paint Mode: radius (m), density. Returns the width used. */
    float draw_instance_header_(imm::Context& ctx, float x, const imm::Box& hb);

    /** @brief The tool column of the toolbar (Instance Paint Mode). */
    void draw_instance_toolbar_(imm::Context& ctx, const imm::Box& r);

    /** @brief Properties > Tool in Instance Paint Mode. */
    void draw_instance_tool_panel_(imm::Context& ctx);

    /**
     * @brief The component inspector's own header: what this painter holds, and Paint Instances.
     *
     * Registered as the "InstancePainter.edit" inspector section, as HeightmapTerrain's is, so
     * the schema's rows keep their layout, their prefab-override marks and their "More" block.
     */
    EditResult draw_instance_inspector_header_(imm::Context& ctx, ObjectId id);

    void draw_instance_palette_(imm::Context& ctx, toy::instance::InstancePainter& painter);

    /** @brief Edits the painter's component node (one undo step; `merge` joins a drag). */
    void instance_edit_component_(const std::string& label, const std::function<void(Node&)>& fn,
                                  const std::string& merge = {});

    /** @brief Adds a prototype to the palette, as an object asset or a bare mesh. */
    void add_prototype_(const std::string& asset_ref, bool is_object);

    /** @brief Turns the instance nearest `px` into a real `prefab:` object, and drops the record. */
    void convert_nearest_to_object_(const ViewProj& vp, glm::vec2 px);

    // --- the test and command surface (mirroring TerrainPanel's) ------------------------

    InstanceBrushSettings& brush() { return brush_; }
    bool instance_mode() const { return in_instance_mode_(); }
    const UndoStack<InstancePatch>& instance_history() const { return instance_undo_; }
    /** @brief The painter being edited, or 0. */
    ObjectId instance_object() const { return instance_object_; }

    /** @brief The BVH for one mesh shape, built once and reused for every ray of every dab. */
    struct MeshRay {
        EditMesh mesh;
        TriangleBVH bvh;
    };

    /**
     * @brief One paintable surface in the scene, ready to offer a raycaster.
     *
     * Either a mesh (with the BVH built for its shape) or a terrain view, never both.
     */
    struct Surface {
        ObjectId id = 0;
        const EditMesh* mesh = nullptr;
        const TriangleBVH* bvh = nullptr;
        glm::mat4 world{1.0f};
        toy::world::TerrainFieldView terrain{};
    };

    /**
     * @brief Rebuilds `surfaces_` from the scene, at most once a frame.
     *
     * A dab fires one ray per candidate and the overlay ring fires 48 more, so the scene walk
     * -- live objects, document lookups, mesh resolution, BVH builds -- has to happen once a
     * frame and not once a ray.
     */
    void refresh_surfaces_();

    /** @brief Offers every surface in `surfaces_` to `rc`. */
    void aim_(SurfaceRaycaster& rc) const;

    /** @brief `refresh_surfaces_()` then a ray straight down onto whatever is under `xy`. */
    std::optional<SurfaceHit> drop_onto_surface_(glm::vec2 xy, float from_z);

    bool instance_mode_ = false;
    ObjectId instance_object_ = 0;

    InstanceBrushSettings brush_;
    InstanceStroke stroke_;
    SculptResize resize_;
    UndoStack<InstancePatch> instance_undo_{200};

    InstanceSelection selection_;
    SelectDrag select_drag_;
    /// The set revision the selection was last known good at; see sync_selection_().
    std::uint64_t selection_revision_ = 0;
    /// Absolute yaw / scale shown for a single selected instance, as last edited.
    float single_yaw_deg_ = 0.0f;
    float single_scale_ = 1.0f;
    /// The two RELATIVE controls (turn a group, raise it). Both spring back to zero after they
    /// are applied, so the widget reads as a nudge rather than as a value the selection has.
    float group_turn_deg_ = 0.0f;
    float group_raise_m_ = 0.0f;

    /// Keyed by the mesh's shape. Cleared when the editor's mesh cache is emptied, since an
    /// edited mesh would otherwise keep its old shape here for the rest of the session.
    std::map<std::string, std::unique_ptr<MeshRay>> mesh_rays_;
    std::uint64_t mesh_rays_revision_ = 0;
    /// This frame's paintable surfaces; see refresh_surfaces_().
    std::vector<Surface> surfaces_;
    std::uint64_t surfaces_frame_ = 0;
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_UI_INSTANCE_PAINT_H
