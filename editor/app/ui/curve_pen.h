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

#include <toyengine/curve/curve_data.h>

#include "editor/app/editor_panel.h"
#include "editor/viewport/curve_edit.h"

namespace toy {
namespace editor {

/**
 * @class CurvePenPanel
 * @brief Viewport editing of a Curve component.
 */
class CurvePenPanel : public EditorPanel {
public:
    using EditorPanel::EditorPanel;

    /** @brief True while the Pen tool owns the viewport. */
    bool in_pen_mode_() const;

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
    void draw_scene_curves_(imm::Context& ctx, const ViewProj& vp);

    /// @brief The selected control point, or -1. Public so a test can check what a click did.
    int selected_point() const { return selected_; }
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

    ObjectId target_ = 0;
    int      selected_ = -1;

    /** @brief A point or handle being dragged. */
    struct Drag {
        bool        active = false;
        curve_tool::HitKind kind = curve_tool::HitKind::None;
        std::size_t index = 0;
        bool        moved = false;
    };
    Drag drag_;

    /// @brief Place new points on the world Z plane rather than a camera-facing one.
    bool  on_ground_ = true;
    /// @brief The Z the ground rule uses.
    float ground_height_ = 0.0f;
};

} // namespace editor
} // namespace toy
