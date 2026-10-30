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
#include <optional>
#include <vector>

#include <toyengine/curve/curve_data.h>

#include "editor/app/editor_panel.h"
#include "editor/viewport/curve_edit.h"
#include "editor/viewport/modal_transform.h"

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

    /** @brief Enters the mode on `id`, adding a Curve component when it has none. */
    bool begin_pen_(ObjectId id);
    /** @brief Leaves the mode, closing any open undo merge. */
    void end_pen_();

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
    /// @brief Where a click lands, by the current depth rule, in the curve's own space.
    glm::vec3 place_at_(const ViewProj& vp, glm::vec2 mouse, const glm::vec3& anchor) const;

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

    /// @brief Place new points on the world Z plane rather than a camera-facing one.
    bool  on_ground_ = true;
    /// @brief The Z the ground rule uses.
    float ground_height_ = 0.0f;
};

} // namespace editor
} // namespace toy
