#pragma once

/**
 * @file curve_pen.h
 * @brief The Pen tool: drawing and reshaping a `Curve` component in the viewport.
 *
 * Input and drawing only. Every decision -- what is under the cursor, where a click lands in
 * depth, what a dragged handle does to its partner -- lives in `editor/viewport/curve_edit.h`,
 * which is unit-tested without a window. This file turns clicks into those calls and commits the
 * result as one undo step.
 *
 * The mode is entered on an object that has a Curve (or adds one), the way Edit Mode is entered
 * on a mesh.
 */

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <toyengine/curve/curve_data.h>
#include <toyengine/world/terrain_field.h>

#include "editor/app/editor_panel.h"
#include "editor/mesh/mesh_bvh.h"
#include "editor/schema/inspector.h"
#include "editor/schema/inspector_sections.h"
#include "editor/viewport/curve_edit.h"
#include "editor/viewport/modal_transform.h"
#include "editor/viewport/surface_raycast.h"

namespace toy {
namespace editor {

/**
 * @class CurvePenPanel
 * @brief Viewport editing of a Curve component.
 */
class CurvePenPanel : public EditorPanel {
public:
    using EditorPanel::EditorPanel;

    /**
     * @enum PenTool
     * @brief What a click does while Pen Mode is on -- the mode/tool split the viewport uses
     *        everywhere else: the MODE decides what you are editing (a curve's anchors), the
     *        TOOL decides what a click does to it.
     */
    enum class PenTool {
        Select,   ///< Click picks anchors (Shift adds); G / R / S transform them.
        Draw,     ///< Click appends a point, or inserts one on the line. The pen proper.
    };

    /** @brief True while Pen Mode owns the viewport. */
    bool in_pen_mode_() const;

    PenTool tool() const { return tool_; }
    void set_tool(PenTool t) { tool_ = t; }

    /** @brief The object being drawn on, or 0. */
    ObjectId target() const { return target_; }

    /**
     * @brief The Hair component's "Guides" inspector section.
     *
     * Lives here rather than with the Hair component's other rows because it is about CURVES:
     * it reports what the groom is currently following, and its button creates a guide and drops
     * straight into Pen Mode on it -- which only this panel can do.
     *
     * Registered as `"Hair.guides"` by EditorApp, which is also what tears it down.
     */
    EditResult draw_hair_guides_(imm::Context& ctx, SectionContext& sc);

    /**
     * @brief Enters the mode on `id`, adding a Curve component when it has none.
     *
     * On an object carrying a **Hair** component the target is redirected to that groom's guides:
     * a Hair holds no curve of its own, so editing the thing you selected would otherwise add a
     * stray Curve to the scalp and edit nothing. The first existing guide is picked, or one is
     * created (with its `Guides` group, and the component pointed at it) if there is none yet.
     */
    bool begin_pen_(ObjectId id);

    /// @brief Creates a guide curve under `hair_id`'s `group_name` child (making the group if
    ///        needed) and enters Pen Mode on it. @return The new guide's id.
    ObjectId add_hair_guide_(ObjectId hair_id, const std::string& group_name);

    /// @brief Copies `guide` beside itself, with its shape, and draws the copy.
    ObjectId duplicate_hair_guide_(ObjectId guide);

    /// @brief Deletes `guide` and moves the Pen to a neighbour -- the one after it, or the one
    ///        before when it was last. Deleting the only guide leaves the Pen detached rather
    ///        than pointed at an object that no longer exists.
    void delete_hair_guide_(ObjectId guide);

    /// @brief The first guide curve under the Hair object `hair_id`, creating one if it has none.
    ///        @return The guide object's id, or 0 if it could not be made.
    ObjectId first_or_new_hair_guide_(ObjectId hair_id);
    /** @brief Leaves the mode, closing any open undo merge. */
    void end_pen_();

    /**
     * @brief Points the Pen at another curve WITHOUT re-entering the mode (0 detaches it).
     *
     * What switching curves inside Pen Mode uses -- begin_pen_() would also add a Curve component
     * to whatever it was handed, which is wrong for something that already has one.
     */
    void focus_curve_(ObjectId id);

    /** @brief The curve whose line is under `mouse`, other than the target, or 0. */
    ObjectId curve_under_(const ViewProj& vp, glm::vec2 mouse) const;

    /** @brief Clicks, drags and keys while the tool is active. */
    void handle_pen_input_(imm::Context& ctx, const ViewProj& vp, bool hovered);
    /** @brief The curve, its points and (for the selection) its handles. */
    void draw_pen_overlay_(imm::Context& ctx, const ViewProj& vp);
    /** @brief The Pen tool's settings, in the Tool tab. */
    void draw_pen_props_(imm::Context& ctx);

    /** @brief Draws any Curve in the scene that asks to always be visible, or is selected. */
    /// @brief Draws every other object's Curve that is selected or asks to always show.
    /// @return How many it drew, which is what lets a test see the overlay toggles gate it.
    int draw_scene_curves_(imm::Context& ctx, const ViewProj& vp);

    /// @brief The active (last-picked) control point, or -1.
    int selected_point() const { return active_; }
    /// @brief Every selected anchor, in pick order. G / R / S act on all of them.
    const std::vector<std::size_t>& selection() const { return selection_; }
    /// @brief True while G / R / S is transforming the selection.
    bool transforming() const { return modal_.active(); }
    /// @brief Selects control point `i` alone (-1 clears), as a plain click would.
    void select(int i);
    /// @brief Adds `i` to the selection, as Shift+click would.
    void select_add(std::size_t i);

    /// @brief Sets every selected anchor's handle mode (Corner/Smooth/Vector/Auto), as one
    ///        undo step. A no-op with nothing selected.
    void set_selection_handle_mode_(toy::curve::HandleMode mode);
    /// @brief True while `mode` is `on` for every selected anchor and the selection is non-empty
    ///        -- what lights up the matching toolbar button.
    bool selection_handle_mode_is_(toy::curve::HandleMode mode) const;
    /// @brief Shows handles on every selected anchor, not only the last-picked (active) one.
    bool show_selected_handles_ = false;

    /**
     * @brief E: a new anchor continuing from the selected one, then straight into a grab.
     *
     * Mesh extrude's shape, and for the same reason -- the new thing is useless where it was
     * born, so the operator hands it to the move rather than making you start one. Needs
     * exactly one anchor selected: from two, "which way does it continue" has no answer.
     */
    bool extrude_selected_(const ViewProj& vp, glm::vec2 mouse);

    /** @brief The rubber band, while one is being dragged. */
    void draw_band_(imm::Context& ctx) const;

    /** @brief The Pen Mode toolbar: its tools, in place of the object ones. */
    void draw_pen_toolbar_(imm::Context& ctx, const imm::Box& r);
    /// @brief The target's curve, or null when the mode is not active.
    const toy::curve::CurveData* curve() const;

private:
    /// @brief Writes `data` back to the document as one undo step (merged while `active`).
    void commit_(const toy::curve::CurveData& data, const std::string& label, bool active);
    /// @brief The live component, or null.
    toy::curve::CurveData* live_curve_();
    /// @brief The target's object-to-world matrix.
    glm::mat4 to_world_() const;
    /**
     * @brief Where a click lands, in the curve's own space.
     *
     * Tries the real scene first -- whatever mesh or terrain is actually under the cursor --
     * so a guide's root lands ON the scalp it is meant to grow from rather than on the ground
     * plane behind it. Only when nothing is there does it fall back to the ground/camera-plane
     * rule `on_ground_` picks.
     */
    glm::vec3 place_at_(const ViewProj& vp, glm::vec2 mouse, const glm::vec3& anchor);

    /** @brief The BVH for one mesh shape, built once and reused across clicks; see refresh_surfaces_(). */
    struct MeshRay {
        EditMesh mesh;
        TriangleBVH bvh;
    };

    /** @brief One raycastable surface in the scene: either a mesh (with its BVH) or terrain. */
    struct Surface {
        ObjectId id = 0;
        const EditMesh* mesh = nullptr;
        const TriangleBVH* bvh = nullptr;
        glm::mat4 world{1.0f};
        toy::world::TerrainFieldView terrain{};
    };

    /** @brief Rebuilds `surfaces_` from the scene, at most once a frame. Mirrors
     *         InstancePaintPanel's cache of the same shape (editor/app/ui/instance_paint.h). */
    void refresh_surfaces_();
    /** @brief Where the ray through `px` meets the nearest mesh or terrain, or nullopt. */
    std::optional<SurfaceHit> surface_hit_(const ViewProj& vp, glm::vec2 px);

    /**
     * @brief Starts a modal G / R on the selected anchor, mouse-driven with axis locking.
     *
     * The SAME `ModalTransform` the object and mesh tools use, so X/Y/Z, Shift+axis planes,
     * typed numbers, Ctrl snapping and right-click-cancel all behave identically here. Writing
     * a second implementation is how a tool ends up with subtly different axis keys.
     */
    void start_modal_(ModalKind kind, const ViewProj& vp, glm::vec2 mouse,
                      const toy::curve::CurveData* base = nullptr);
    /// @brief Drives a running modal; returns true when it consumed the frame's input.
    bool run_modal_(imm::Context& ctx, const ViewProj& vp);

    /// @brief The anchors `selection_` names, and the pivot they transform about.
    glm::vec3 selection_pivot_() const;

    ObjectId target_ = 0;
    PenTool  tool_ = PenTool::Draw;
    std::vector<std::size_t> selection_;
    int      active_ = -1;

    ModalTransform modal_;
    /// @brief The curve as it was when the modal began. Every frame re-derives from this, so a
    ///        rotate never compounds its own output and spirals while the mouse sits still.
    toy::curve::CurveData modal_base_;
    /// @brief The pivot the running modal turns and scales about (the selection's centre).
    glm::vec3 modal_pivot_{0.0f};

    /** @brief A point or handle being dragged. */
    struct Drag {
        bool        active = false;
        curve_tool::HitKind kind = curve_tool::HitKind::None;
        std::size_t index = 0;
        bool        moved = false;
        /**
         * @brief Where the depth plane was hit when the drag STARTED.
         *
         * A drag moves what it grabbed by how far the mouse has travelled. Without this, the
         * first frame computes `mouse - anchor` and the anchor jumps to sit under the cursor --
         * the grab offset every drag needs and the thing that is instantly obvious when missing.
         */
        glm::vec3   press_world{0.0f};
        /// @brief The curve as it was at press, so the move is never applied to its own output.
        toy::curve::CurveData base;
    };
    Drag drag_;

    /** @brief A rubber band being dragged over empty space, in window pixels. */
    struct Band {
        bool      pending = false;   ///< Pressed on nothing; a band starts once it moves.
        bool      active = false;
        bool      additive = false;  ///< Shift: add to the selection rather than replace it.
        glm::vec2 press{0.0f};
        glm::vec2 now{0.0f};
    };
    Band band_;

    /// @brief Fallback for when the cursor is over nothing: place on the world Z plane rather
    ///        than a camera-facing one. place_at_() tries a real surface hit first.
    bool  on_ground_ = true;
    /// @brief The Z the ground rule uses.
    float ground_height_ = 0.0f;

    /// @brief Keyed by mesh shape; cleared when the editor's mesh cache is emptied. See
    ///        refresh_surfaces_().
    std::map<std::string, std::unique_ptr<MeshRay>> mesh_rays_;
    std::uint64_t mesh_rays_revision_ = 0;
    /// @brief This frame's raycastable surfaces.
    std::vector<Surface> surfaces_;
    std::uint64_t surfaces_frame_ = 0;
};

} // namespace editor
} // namespace toy
