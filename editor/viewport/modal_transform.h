/**
 * @file modal_transform.h
 * @brief Blender's modal transform operators (G / R / S, plus interactive inset and bevel)
 *        as a small state machine independent of what is being transformed.
 *
 * While active, mouse motion drives the operation; keys refine it exactly as in Blender:
 *   X / Y / Z        constrain to that axis (press again: the object's local axis; again: off)
 *   Shift+X/Y/Z      constrain to the plane perpendicular to that axis
 *   digits . - Bksp  type an exact value (units, degrees, factor)
 *   Ctrl (held)      snap: 1 unit / 5 degrees / 0.1 factor
 *   Shift (held)     precision: one tenth of the mouse motion
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

enum class ModalKind { None, Grab, Rotate, Scale, Inset, Bevel };

class ModalTransform {
public:
    struct Result {
        glm::vec3 translate{0.0f};
        glm::vec3 rotate_axis{0, 0, 1};
        float rotate_deg = 0.0f;
        glm::vec3 scale{1.0f};
        float amount = 0.0f;          ///< Inset fraction / bevel width.
    };

    enum class Outcome { Running, Confirmed, Cancelled };

    bool active() const { return kind_ != ModalKind::None; }
    ModalKind kind() const { return kind_; }

    /**
     * @param pivot        World-space pivot (selection centre).
     * @param local_basis  The active object's axes (columns), for double-pressed constraints.
     * @param custom_axis  Optional pre-set constraint (extrude moves along the normal).
     */
    void begin(ModalKind kind, const ViewProj& vp, const glm::vec3& pivot, const glm::mat3& local_basis, glm::vec2 mouse,
               std::optional<glm::vec3> custom_axis = std::nullopt) {
        kind_ = kind;
        pivot_ = pivot;
        local_ = local_basis;
        start_mouse_ = mouse;
        last_mouse_ = mouse;
        virtual_mouse_ = mouse;
        numeric_.clear();
        axis_ = -1;
        axis_local_ = false;
        plane_ = false;
        custom_axis_ = custom_axis;
        accumulated_deg_ = 0.0f;
        const auto c = vp.project(pivot);
        pivot_px_ = c.value_or(mouse);
        last_angle_ = std::atan2(mouse.y - pivot_px_.y, mouse.x - pivot_px_.x);
        start_dist_ = std::max(4.0f, glm::distance(mouse, pivot_px_));
        wpp_ = vp.world_per_pixel(pivot);
        if (kind == ModalKind::Bevel) start_dist_ = std::max(4.0f, start_dist_);
    }

    void cancel() { kind_ = ModalKind::None; }

    /**
     * @brief Feeds one frame of input.
     * @param keys  This frame's key events (Press/Repeat are used).
     * @param lmb_pressed/rmb_pressed  Mouse buttons pressed this frame.
     */
    Outcome update(const ViewProj& vp, glm::vec2 mouse, const std::vector<coopa::input::KeyEvent>& keys,
                   bool lmb_pressed, bool rmb_pressed, bool ctrl, bool shift) {
        using coopa::input::Key;
        using coopa::input::KeyAction;
        using coopa::input::Mods;
        if (!active()) return Outcome::Cancelled;
        // Precision: Shift scales further mouse motion by 1/10.
        const glm::vec2 d = mouse - last_mouse_;
        virtual_mouse_ += shift ? d * 0.1f : d;
        last_mouse_ = mouse;
        ctrl_ = ctrl;
        for (const auto& e : keys) {
            if (e.action == KeyAction::Release) continue;
            const bool eshift = has(e.mods, Mods::Shift);
            switch (e.key) {
                case Key::Escape: kind_ = ModalKind::None; return Outcome::Cancelled;
                case Key::Enter: case Key::KpEnter: result_ = compute_(vp); kind_ = ModalKind::None; return Outcome::Confirmed;
                case Key::X: press_axis_(0, eshift); break;
                case Key::Y: press_axis_(1, eshift); break;
                case Key::Z: press_axis_(2, eshift); break;
                case Key::Backspace: if (!numeric_.empty()) numeric_.pop_back(); break;
                case Key::Minus: case Key::KpSubtract:
                    numeric_ = (!numeric_.empty() && numeric_[0] == '-') ? numeric_.substr(1) : "-" + numeric_;
                    break;
                case Key::Period: case Key::KpDecimal: if (numeric_.find('.') == std::string::npos) numeric_ += '.'; break;
                default: {
                    const int k = static_cast<int>(e.key);
                    if (k >= static_cast<int>(Key::Num0) && k <= static_cast<int>(Key::Num9)) numeric_ += char('0' + k - static_cast<int>(Key::Num0));
                    else if (k >= static_cast<int>(Key::Kp0) && k <= static_cast<int>(Key::Kp9)) numeric_ += char('0' + k - static_cast<int>(Key::Kp0));
                    break;
                }
            }
        }
        if (rmb_pressed) { kind_ = ModalKind::None; return Outcome::Cancelled; }
        // Rotation accumulates angle frame to frame so it can go past +-180.
        const float ang = std::atan2(virtual_mouse_.y - pivot_px_.y, virtual_mouse_.x - pivot_px_.x);
        float da = ang - last_angle_;
        while (da > 3.14159265f) da -= 6.2831853f;
        while (da < -3.14159265f) da += 6.2831853f;
        accumulated_deg_ += glm::degrees(da);
        last_angle_ = ang;
        result_ = compute_(vp);
        if (lmb_pressed) { kind_ = ModalKind::None; return Outcome::Confirmed; }
        return Outcome::Running;
    }

    const Result& result() const { return result_; }

    /** @brief Status text, Blender style: "Move  X: 1.250  (local)". */
    std::string header() const {
        char buf[160];
        const char* axes[] = {"X", "Y", "Z"};
        std::string con;
        if (custom_axis_ && axis_ < 0) con = "  along normal";
        else if (axis_ >= 0) con = std::string("  ") + (plane_ ? "plane !" : "") + axes[axis_] + (axis_local_ ? " (local)" : " (global)");
        const std::string num = numeric_.empty() ? "" : "  [" + numeric_ + "]";
        switch (last_kind_for_header_()) {
            case ModalKind::Grab:
                std::snprintf(buf, sizeof(buf), "Move  D: %.3f  %.3f  %.3f%s%s", result_.translate.x, result_.translate.y,
                              result_.translate.z, con.c_str(), num.c_str());
                break;
            case ModalKind::Rotate: std::snprintf(buf, sizeof(buf), "Rotate  %.2f deg%s%s", result_.rotate_deg, con.c_str(), num.c_str()); break;
            case ModalKind::Scale:
                std::snprintf(buf, sizeof(buf), "Scale  %.3f  %.3f  %.3f%s%s", result_.scale.x, result_.scale.y, result_.scale.z,
                              con.c_str(), num.c_str());
                break;
            case ModalKind::Inset: std::snprintf(buf, sizeof(buf), "Inset  thickness %.3f%s", result_.amount, num.c_str()); break;
            case ModalKind::Bevel: std::snprintf(buf, sizeof(buf), "Bevel  width %.3f%s", result_.amount, num.c_str()); break;
            default: return {};
        }
        return buf;
    }

    /** @brief The constraint axis in world space, or nullopt (for drawing a guide line). */
    std::optional<glm::vec3> constraint_axis() const {
        if (axis_ >= 0 && !plane_) return axis_vec_(axis_);
        if (custom_axis_ && axis_ < 0) return custom_axis_;
        return std::nullopt;
    }
    glm::vec3 pivot() const { return pivot_; }

private:
    ModalKind last_kind_for_header_() const { return kind_; }

    void press_axis_(int a, bool plane) {
        if (axis_ == a && plane_ == plane) {
            if (!axis_local_) axis_local_ = true;
            else { axis_ = -1; axis_local_ = false; plane_ = false; }
        } else {
            axis_ = a;
            axis_local_ = false;
            plane_ = plane;
        }
    }

    glm::vec3 axis_vec_(int a) const {
        glm::vec3 v(0.0f);
        v[a] = 1.0f;
        return axis_local_ ? glm::normalize(local_[a]) : v;
    }

    std::optional<float> typed_() const {
        if (numeric_.empty() || numeric_ == "-" || numeric_ == ".") return std::nullopt;
        return std::strtof(numeric_.c_str(), nullptr);
    }

    Result compute_(const ViewProj& vp) const {
        Result r;
        const auto typed = typed_();
        const glm::vec2 m = virtual_mouse_;
        switch (kind_) {
            case ModalKind::Grab: {
                std::optional<glm::vec3> axis = constraint_axis();
                if (axis) {
                    float t;
                    if (typed) t = *typed;
                    else {
                        const auto a = vp.project(pivot_), b = vp.project(pivot_ + *axis);
                        glm::vec2 sa = (a && b) ? *b - *a : glm::vec2(1, 0);
                        const float len2 = std::max(1e-6f, glm::dot(sa, sa));
                        t = glm::dot(m - start_mouse_, sa) / len2;
                    }
                    if (ctrl_ && !typed) t = std::round(t);
                    r.translate = *axis * t;
                } else if (plane_ && axis_ >= 0) {
                    const glm::vec3 n = axis_vec_(axis_);
                    glm::vec3 o0, d0, o1, d1;
                    vp.ray(start_mouse_, o0, d0);
                    vp.ray(m, o1, d1);
                    auto hit = [&](const glm::vec3& o, const glm::vec3& d) {
                        const float den = glm::dot(d, n);
                        return std::abs(den) > 1e-6f ? o + d * (glm::dot(pivot_ - o, n) / den) : pivot_;
                    };
                    glm::vec3 t = hit(o1, d1) - hit(o0, d0);
                    if (ctrl_) t = glm::round(t);
                    r.translate = t;
                } else {
                    // Free move in the view plane.
                    const glm::mat4 inv = glm::inverse(vp.view);
                    const glm::vec3 right = glm::normalize(glm::vec3(inv[0])), up = glm::normalize(glm::vec3(inv[1]));
                    const glm::vec2 dm = m - start_mouse_;
                    glm::vec3 t = (right * dm.x - up * dm.y) * wpp_;
                    if (typed) t = glm::vec3(*typed, 0, 0);
                    if (ctrl_ && !typed) t = glm::round(t);
                    r.translate = t;
                }
                break;
            }
            case ModalKind::Rotate: {
                const glm::vec3 view_axis = -vp.camera_forward();
                glm::vec3 axis = view_axis;
                if (axis_ >= 0) axis = axis_vec_(axis_);
                // Screen angles grow clockwise (y down); match the on-screen drag direction.
                const float sign = glm::dot(axis, vp.camera_forward()) > 0.0f ? 1.0f : -1.0f;
                float deg = typed ? *typed : accumulated_deg_ * sign;
                if (ctrl_ && !typed) deg = std::round(deg / 5.0f) * 5.0f;
                r.rotate_axis = axis;
                r.rotate_deg = deg;
                break;
            }
            case ModalKind::Scale: {
                float f = typed ? *typed : glm::distance(m, pivot_px_) / start_dist_;
                if (ctrl_ && !typed) f = std::round(f * 10.0f) / 10.0f;
                glm::vec3 s(f);
                if (axis_ >= 0 && !plane_) { s = glm::vec3(1.0f); s[axis_] = f; }
                else if (axis_ >= 0 && plane_) { s = glm::vec3(f); s[axis_] = 1.0f; }
                r.scale = s;
                break;
            }
            case ModalKind::Inset: {
                // Moving the mouse toward the pivot thickens the inset, as in Blender.
                const float f = typed ? *typed : std::clamp(1.0f - glm::distance(m, pivot_px_) / start_dist_, 0.0f, 0.99f);
                r.amount = ctrl_ && !typed ? std::round(f * 20.0f) / 20.0f : f;
                break;
            }
            case ModalKind::Bevel: {
                const float w = typed ? *typed : std::max(0.0f, (glm::distance(m, pivot_px_) - start_dist_) * wpp_ + 0.0f);
                r.amount = ctrl_ && !typed ? std::round(w * 20.0f) / 20.0f : w;
                break;
            }
            default: break;
        }
        return r;
    }

    ModalKind kind_ = ModalKind::None;
    glm::vec3 pivot_{0.0f};
    glm::mat3 local_{1.0f};
    glm::vec2 start_mouse_{0.0f}, last_mouse_{0.0f}, virtual_mouse_{0.0f}, pivot_px_{0.0f};
    float start_dist_ = 1.0f, wpp_ = 0.01f;
    float last_angle_ = 0.0f, accumulated_deg_ = 0.0f;
    std::string numeric_;
    int axis_ = -1;
    bool axis_local_ = false;
    bool plane_ = false;
    bool ctrl_ = false;
    std::optional<glm::vec3> custom_axis_;
    Result result_;
};

/** @brief A cumulative transform delta as a world matrix about `pivot`. */
inline glm::mat4 delta_matrix(const glm::vec3& translate, const glm::vec3& rot_axis, float rot_deg, const glm::vec3& scale,
                              const glm::vec3& pivot) {
    const glm::mat4 to = glm::translate(glm::mat4(1.0f), pivot), from = glm::translate(glm::mat4(1.0f), -pivot);
    glm::mat4 m = glm::translate(glm::mat4(1.0f), translate);
    if (rot_deg != 0.0f) m = m * to * glm::rotate(glm::mat4(1.0f), glm::radians(rot_deg), glm::normalize(rot_axis)) * from;
    if (scale != glm::vec3(1.0f)) m = m * to * glm::scale(glm::mat4(1.0f), scale) * from;
    return m;
}

} // namespace toy::editor

#endif // TOYEDITOR_VIEWPORT_MODAL_TRANSFORM_H
