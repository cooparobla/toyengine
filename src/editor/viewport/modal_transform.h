/**
 * @file modal_transform.h
 * @brief Blender's modal transform operators (G / R / S, plus interactive inset and bevel)
 *        as a small state machine independent of what is being transformed.
 *
 * While active, mouse motion drives the operation; keys refine it exactly as in Blender:
 *   X / Y / Z        constrain to that global axis (press again: the header orientation's
 *                    axis -- Local, or Normal in edit mode; again: off)
 *   Shift+X/Y/Z      constrain to the plane perpendicular to that axis
 *   MMB (held)       pick the axis the mouse moves along (auto constraint)
 *   digits . - Bksp  type an exact value (units, degrees, factor); Tab moves to the next
 *                    component (free move: X Y Z, plane: its two axes, scale: X Y Z)
 *   Ctrl (held)      snap: `snap_step` units (the editor passes the grid's current spacing,
 *                    so it follows the zoom as Blender's increment snap does) / 5 degrees / 0.1 factor
 *   Shift (held)     precision: one tenth of the mouse motion
 *   G (during Grab)  in edit mode: switch to Edge Slide, as Blender's G G
 *   LMB / Enter      confirm          RMB / Esc   cancel
 *
 * The result is cumulative since the operator started (see Result), so the caller applies
 * it to the values captured at start -- cancel is "put the captured values back".
 */

#ifndef TOYEDITOR_VIEWPORT_MODAL_TRANSFORM_H
#define TOYEDITOR_VIEWPORT_MODAL_TRANSFORM_H

#include "gizmo.h"

#include <coopa/input/keys.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace toy::editor {

enum class ModalKind { None, Grab, Rotate, Scale, Inset, Bevel, EdgeSlide };

class ModalTransform {
public:
    struct Result {
        glm::vec3 translate{0.0f};
        glm::vec3 rotate_axis{0, 0, 1};
        float rotate_deg = 0.0f;
        glm::vec3 scale{1.0f};
        glm::mat3 scale_basis{1.0f};  ///< Axes `scale` is expressed in (columns).
        float amount = 0.0f;          ///< Inset fraction / bevel width / edge-slide factor.
    };

    enum class Outcome { Running, Confirmed, Cancelled, SwitchToSlide };

    bool active() const { return kind_ != ModalKind::None; }
    ModalKind kind() const { return kind_; }

    float snap_step = 1.0f;   ///< Ctrl's move increment, in world units.

    /**
     * @param pivot        World-space pivot (selection centre).
     * @param second_basis Axes for a second axis press (columns): the header orientation's,
     *                     or the object's local axes when the header says Global.
     * @param second_name  How the header names them ("local", "normal").
     * @param custom_axis  Optional pre-set constraint (extrude moves along the normal; Edge
     *                     Slide's rail direction, whose length is the full slide).
     * @param allow_slide  A Grab in edit mode: pressing G again asks for Edge Slide.
     */
    void begin(ModalKind kind, const ViewProj& vp, const glm::vec3& pivot, const glm::mat3& second_basis, glm::vec2 mouse,
               std::optional<glm::vec3> custom_axis = std::nullopt, const std::string& second_name = "local",
               bool allow_slide = false);

    void cancel() { kind_ = ModalKind::None; }

    /**
     * @brief Feeds one frame of input.
     * @param keys  This frame's key events (Press/Repeat are used).
     * @param lmb_pressed/rmb_pressed  Mouse buttons pressed this frame.
     * @param mmb_down/mmb_pressed     Middle button, for the auto constraint.
     */
    Outcome update(const ViewProj& vp, glm::vec2 mouse, const std::vector<coopa::input::KeyEvent>& keys,
                   bool lmb_pressed, bool rmb_pressed, bool ctrl, bool shift, bool mmb_down = false, bool mmb_pressed = false);

    const Result& result() const { return result_; }

    /** @brief Status text, Blender style: "Move  D: 1.250 0 0  X (local)  [1.25|]". */
    std::string header() const;

    /** @brief The single constraint axis in world space, or nullopt. */
    std::optional<glm::vec3> constraint_axis() const;

    /**
     * @brief Guide lines to draw: the constraint axis, or both axes of a constraint plane
     *        (second = primary; plane axes draw dimmer).
     */
    std::vector<std::pair<glm::vec3, bool>> guide_axes() const;
    glm::vec3 pivot() const { return pivot_; }
    int axis() const { return axis_; }
    bool axis_is_second() const { return axis_local_; }
    bool plane() const { return plane_; }

private:
    bool axis_kind_() const { return kind_ == ModalKind::Grab || kind_ == ModalKind::Rotate || kind_ == ModalKind::Scale; }

    /** @brief How many numeric fields Tab cycles through for the current mode. */
    int field_count_() const;

    void press_axis_(int a, bool plane);

    glm::vec3 axis_vec_(int a) const;
    float snap_(float v) const { return snap_step > 0.0f ? std::round(v / snap_step) * snap_step : v; }
    glm::mat3 basis_() const { return axis_local_ ? local_ : glm::mat3(1.0f); }

    std::optional<float> typed_(int i = 0) const;
    bool any_typed_() const { return typed_(0) || typed_(1) || typed_(2) || field_ > 0; }

    Result compute_(const ViewProj& vp) const;

    ModalKind kind_ = ModalKind::None;
    glm::vec3 pivot_{0.0f};
    glm::mat3 local_{1.0f};
    std::string second_name_ = "local";
    glm::vec2 start_mouse_{0.0f}, last_mouse_{0.0f}, virtual_mouse_{0.0f}, pivot_px_{0.0f}, mmb_start_{0.0f};
    float start_dist_ = 1.0f, wpp_ = 0.01f;
    float last_angle_ = 0.0f, accumulated_deg_ = 0.0f;
    std::string fields_[3];
    int field_ = 0;
    bool tabbed_ = false;
    int axis_ = -1;
    bool axis_local_ = false;
    bool plane_ = false;
    bool ctrl_ = false;
    bool allow_slide_ = false;
    bool mmb_active_ = false;
    std::optional<glm::vec3> custom_axis_;
    Result result_;
};

/** @brief A cumulative transform delta as a world matrix about `pivot`. */
glm::mat4 delta_matrix(const glm::vec3& translate, const glm::vec3& rot_axis, float rot_deg, const glm::vec3& scale,
                              const glm::vec3& pivot, const glm::mat3& scale_basis = glm::mat3(1.0f));

} // namespace toy::editor

#endif // TOYEDITOR_VIEWPORT_MODAL_TRANSFORM_H
