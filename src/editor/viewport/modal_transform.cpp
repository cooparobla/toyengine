#include "editor/viewport/modal_transform.h"

namespace toy {
namespace editor {

void ModalTransform::begin(ModalKind kind, const ViewProj& vp, const glm::vec3& pivot, const glm::mat3& second_basis, glm::vec2 mouse,
           std::optional<glm::vec3> custom_axis, const std::string& second_name,
           bool allow_slide) {
    kind_ = kind;
    pivot_ = pivot;
    local_ = second_basis;
    second_name_ = second_name;
    start_mouse_ = mouse;
    last_mouse_ = mouse;
    virtual_mouse_ = mouse;
    for (auto& f : fields_) f.clear();
    field_ = 0;
    tabbed_ = false;
    axis_ = -1;
    axis_local_ = false;
    plane_ = false;
    custom_axis_ = custom_axis;
    allow_slide_ = allow_slide;
    mmb_active_ = false;
    accumulated_deg_ = 0.0f;
    const auto c = vp.project(pivot);
    pivot_px_ = c.value_or(mouse);
    last_angle_ = std::atan2(mouse.y - pivot_px_.y, mouse.x - pivot_px_.x);
    start_dist_ = std::max(4.0f, glm::distance(mouse, pivot_px_));
    wpp_ = vp.world_per_pixel(pivot);
    result_ = Result{};
}

ModalTransform::Outcome ModalTransform::update(const ViewProj& vp, glm::vec2 mouse, const std::vector<coopa::input::KeyEvent>& keys,
               bool lmb_pressed, bool rmb_pressed, bool ctrl, bool shift, bool mmb_down, bool mmb_pressed) {
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
        std::string& num = fields_[field_];
        switch (e.key) {
            case Key::Escape: kind_ = ModalKind::None; return Outcome::Cancelled;
            case Key::Enter: case Key::KpEnter: result_ = compute_(vp); kind_ = ModalKind::None; return Outcome::Confirmed;
            case Key::G:
                if (kind_ == ModalKind::Grab && allow_slide_) { kind_ = ModalKind::None; return Outcome::SwitchToSlide; }
                break;
            case Key::X: if (axis_kind_()) press_axis_(0, eshift); break;
            case Key::Y: if (axis_kind_()) press_axis_(1, eshift); break;
            case Key::Z: if (axis_kind_()) press_axis_(2, eshift); break;
            case Key::Tab: field_ = (field_ + 1) % field_count_(); tabbed_ = true; break;
            case Key::Backspace: if (!num.empty()) num.pop_back(); break;
            case Key::Minus: case Key::KpSubtract:
                num = (!num.empty() && num[0] == '-') ? num.substr(1) : "-" + num;
                break;
            case Key::Period: case Key::KpDecimal: if (num.find('.') == std::string::npos) num += '.'; break;
            default: {
                const int k = static_cast<int>(e.key);
                if (k >= static_cast<int>(Key::Num0) && k <= static_cast<int>(Key::Num9)) num += char('0' + k - static_cast<int>(Key::Num0));
                else if (k >= static_cast<int>(Key::Kp0) && k <= static_cast<int>(Key::Kp9)) num += char('0' + k - static_cast<int>(Key::Kp0));
                break;
            }
        }
    }
    if (rmb_pressed) { kind_ = ModalKind::None; return Outcome::Cancelled; }
    // MMB auto constraint: while held, the global axis whose screen direction best
    // matches the mouse motion since the press.
    if (axis_kind_()) {
        if (mmb_pressed) { mmb_active_ = true; mmb_start_ = mouse; }
        if (mmb_active_ && !mmb_down) mmb_active_ = false;
        if (mmb_active_ && glm::distance(mouse, mmb_start_) > 8.0f) {
            const glm::vec2 dir = glm::normalize(mouse - mmb_start_);
            int best = -1;
            float best_dot = -1.0f;
            const auto c = vp.project(pivot_);
            for (int a = 0; a < 3; ++a) {
                glm::vec3 v(0.0f);
                v[a] = 1.0f;
                const auto p = vp.project(pivot_ + v);
                if (!c || !p || glm::length(*p - *c) < 1e-3f) continue;
                const float dd = std::abs(glm::dot(glm::normalize(*p - *c), dir));
                if (dd > best_dot) { best_dot = dd; best = a; }
            }
            if (best >= 0) { axis_ = best; axis_local_ = false; plane_ = false; }
        }
    }
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

std::string ModalTransform::header() const {
    char buf[200];
    const char* axes[] = {"X", "Y", "Z"};
    std::string con;
    if (custom_axis_ && axis_ < 0 && kind_ != ModalKind::EdgeSlide) con = "  along normal";
    else if (axis_ >= 0) con = std::string("  ") + (plane_ ? "plane !" : "") + axes[axis_] + " (" + (axis_local_ ? second_name_ : "global") + ")";
    std::string num;
    if (any_typed_()) {
        num = "  [";
        for (int i = 0; i < field_count_(); ++i) {
            if (i) num += "  ";
            num += fields_[i].empty() ? "0" : fields_[i];
            if (i == field_) num += "|";
        }
        num += "]";
    }
    switch (kind_) {
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
        case ModalKind::EdgeSlide: std::snprintf(buf, sizeof(buf), "Edge Slide  factor %.3f%s", result_.amount, num.c_str()); break;
        default: return {};
    }
    return buf;
}

std::optional<glm::vec3> ModalTransform::constraint_axis() const {
    if (axis_ >= 0 && !plane_) return axis_vec_(axis_);
    if (custom_axis_ && axis_ < 0) return glm::normalize(*custom_axis_);
    return std::nullopt;
}

std::vector<std::pair<glm::vec3, bool>> ModalTransform::guide_axes() const {
    std::vector<std::pair<glm::vec3, bool>> out;
    if (axis_ >= 0 && plane_) {
        for (int a = 0; a < 3; ++a) if (a != axis_) out.push_back({axis_vec_(a), false});
    } else if (auto ax = constraint_axis()) {
        out.push_back({*ax, true});
    }
    return out;
}

int ModalTransform::field_count_() const {
    if (kind_ == ModalKind::Grab) {
        if (axis_ >= 0 && plane_) return 2;
        if (axis_ >= 0 || custom_axis_) return 1;
        return 3;
    }
    if (kind_ == ModalKind::Scale) return (axis_ >= 0 && !plane_) ? 1 : 3;
    return 1;
}

void ModalTransform::press_axis_(int a, bool plane) {
    if (axis_ == a && plane_ == plane) {
        if (!axis_local_) axis_local_ = true;
        else { axis_ = -1; axis_local_ = false; plane_ = false; }
    } else {
        axis_ = a;
        axis_local_ = false;
        plane_ = plane;
    }
    field_ = std::min(field_, field_count_() - 1);
}

glm::vec3 ModalTransform::axis_vec_(int a) const {
    glm::vec3 v(0.0f);
    v[a] = 1.0f;
    return axis_local_ ? glm::normalize(local_[a]) : v;
}

std::optional<float> ModalTransform::typed_(int i) const {
    const std::string& n = fields_[i];
    if (n.empty() || n == "-" || n == "." || n == "-.") return std::nullopt;
    return std::strtof(n.c_str(), nullptr);
}

ModalTransform::Result ModalTransform::compute_(const ViewProj& vp) const {
    Result r;
    const glm::vec2 m = virtual_mouse_;
    const bool typed_any = typed_(0) || typed_(1) || typed_(2);
    switch (kind_) {
        case ModalKind::Grab: {
            std::optional<glm::vec3> axis = constraint_axis();
            if (axis) {
                float t;
                if (typed_(0)) t = *typed_(0);
                else {
                    const auto a = vp.project(pivot_), b = vp.project(pivot_ + *axis);
                    glm::vec2 sa = (a && b) ? *b - *a : glm::vec2(1, 0);
                    const float len2 = std::max(1e-6f, glm::dot(sa, sa));
                    t = glm::dot(m - start_mouse_, sa) / len2;
                }
                if (ctrl_ && !typed_(0)) t = snap_(t);
                r.translate = *axis * t;
            } else if (plane_ && axis_ >= 0) {
                const int u = axis_ == 0 ? 1 : 0, v = axis_ == 2 ? 1 : 2;
                if (typed_any) {
                    r.translate = axis_vec_(u) * typed_(0).value_or(0.0f) + axis_vec_(v) * typed_(1).value_or(0.0f);
                } else {
                    const glm::vec3 n = axis_vec_(axis_);
                    glm::vec3 o0, d0, o1, d1;
                    vp.ray(start_mouse_, o0, d0);
                    vp.ray(m, o1, d1);
                    auto hit = [&](const glm::vec3& o, const glm::vec3& d) {
                        const float den = glm::dot(d, n);
                        return std::abs(den) > 1e-6f ? o + d * (glm::dot(pivot_ - o, n) / den) : pivot_;
                    };
                    glm::vec3 t = hit(o1, d1) - hit(o0, d0);
                    if (ctrl_) {
                        // Snap in the constraint basis.
                        const glm::mat3 B = basis_();
                        glm::vec3 l = glm::transpose(B) * t;
                        t = B * glm::vec3(snap_(l.x), snap_(l.y), snap_(l.z));
                    }
                    r.translate = t;
                }
            } else if (typed_any) {
                r.translate = glm::vec3(typed_(0).value_or(0.0f), typed_(1).value_or(0.0f), typed_(2).value_or(0.0f));
            } else {
                // Free move in the view plane.
                const glm::mat4 inv = glm::inverse(vp.view);
                const glm::vec3 right = glm::normalize(glm::vec3(inv[0])), up = glm::normalize(glm::vec3(inv[1]));
                const glm::vec2 dm = m - start_mouse_;
                glm::vec3 t = (right * dm.x - up * dm.y) * wpp_;
                if (ctrl_) t = glm::vec3(snap_(t.x), snap_(t.y), snap_(t.z));
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
            float deg = typed_(0) ? *typed_(0) : accumulated_deg_ * sign;
            if (ctrl_ && !typed_(0)) deg = std::round(deg / 5.0f) * 5.0f;
            r.rotate_axis = axis;
            r.rotate_deg = deg;
            break;
        }
        case ModalKind::Scale: {
            float f = glm::distance(m, pivot_px_) / start_dist_;
            if (ctrl_) f = std::round(f * 10.0f) / 10.0f;
            glm::vec3 s(f);
            if (typed_any) {
                const float f0 = typed_(0).value_or(1.0f);
                s = tabbed_ ? glm::vec3(f0, typed_(1).value_or(1.0f), typed_(2).value_or(1.0f)) : glm::vec3(f0);
                f = f0;
            }
            if (axis_ >= 0 && !plane_) { s = glm::vec3(1.0f); s[axis_] = f; }
            else if (axis_ >= 0 && plane_) { s[axis_] = 1.0f; }
            r.scale = s;
            r.scale_basis = basis_();
            break;
        }
        case ModalKind::Inset: {
            // Moving the mouse toward the pivot thickens the inset, as in Blender.
            const float f = typed_(0) ? *typed_(0) : std::clamp(1.0f - glm::distance(m, pivot_px_) / start_dist_, 0.0f, 0.99f);
            r.amount = ctrl_ && !typed_(0) ? std::round(f * 20.0f) / 20.0f : f;
            break;
        }
        case ModalKind::Bevel: {
            const float w = typed_(0) ? *typed_(0) : std::max(0.0f, (glm::distance(m, pivot_px_) - start_dist_) * wpp_ + 0.0f);
            r.amount = ctrl_ && !typed_(0) ? std::round(w * 20.0f) / 20.0f : w;
            break;
        }
        case ModalKind::EdgeSlide: {
            // custom_axis_ spans the slide from the -1 end to the +1 end (world space).
            float t = 0.0f;
            if (typed_(0)) t = *typed_(0);
            else if (custom_axis_) {
                const auto a = vp.project(pivot_), b = vp.project(pivot_ + *custom_axis_ * 0.5f);
                const glm::vec2 sa = (a && b) ? *b - *a : glm::vec2(1, 0);
                t = glm::dot(m - start_mouse_, sa) / std::max(1e-6f, glm::dot(sa, sa));
                if (ctrl_) t = std::round(t * 10.0f) / 10.0f;
            }
            r.amount = std::clamp(t, -1.0f, 1.0f);
            break;
        }
        default: break;
    }
    return r;
}

glm::mat4 delta_matrix(const glm::vec3& translate, const glm::vec3& rot_axis, float rot_deg, const glm::vec3& scale,
                              const glm::vec3& pivot, const glm::mat3& scale_basis) {
    const glm::mat4 to = glm::translate(glm::mat4(1.0f), pivot), from = glm::translate(glm::mat4(1.0f), -pivot);
    glm::mat4 m = glm::translate(glm::mat4(1.0f), translate);
    if (rot_deg != 0.0f) m = m * to * glm::rotate(glm::mat4(1.0f), glm::radians(rot_deg), glm::normalize(rot_axis)) * from;
    if (scale != glm::vec3(1.0f)) {
        // Scale along `scale_basis`'s axes: B * S * B^T (B orthonormal).
        const glm::mat4 B(scale_basis), Bt(glm::transpose(scale_basis));
        m = m * to * B * glm::scale(glm::mat4(1.0f), scale) * Bt * from;
    }
    return m;
}

} // namespace editor
} // namespace toy
