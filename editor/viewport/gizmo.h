/**
 * @file gizmo.h
 * @brief Viewport math (projection, rays, picking) and the translate/rotate/scale gizmo.
 *
 * The gizmo is drawn in the immediate-mode UI layer, at full window resolution on top of
 * the (low-resolution, pixel-art) scene image, and hit-tested in window pixels -- the space
 * the user actually aims in. It reports CUMULATIVE deltas since the drag began, so the
 * caller applies them to the values captured at drag start and never accumulates rounding.
 */

#ifndef TOYEDITOR_VIEWPORT_GIZMO_H
#define TOYEDITOR_VIEWPORT_GIZMO_H

#include <uicoopa/immediate/imm.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/euler_angles.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace toy::editor {

namespace imm = coopa::ui::imm;

/** @brief A camera's view/projection plus the window-pixel rect it is displayed in. */
struct ViewProj {
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    imm::Box  rect;          ///< Window pixels (the letterboxed scene image).
    bool      ortho = false;

    glm::vec3 camera_pos() const { return glm::vec3(glm::inverse(view)[3]); }
    glm::vec3 camera_forward() const { return -glm::normalize(glm::vec3(glm::inverse(view)[2])); }

    /** @brief World -> window pixels; nullopt behind the camera. */
    std::optional<glm::vec2> project(const glm::vec3& p) const {
        const glm::vec4 c = proj * view * glm::vec4(p, 1.0f);
        if (c.w <= 1e-6f) return std::nullopt;
        const glm::vec2 ndc = glm::vec2(c) / c.w;
        return glm::vec2(rect.x + (ndc.x * 0.5f + 0.5f) * rect.w, rect.y + (0.5f - ndc.y * 0.5f) * rect.h);
    }

    /** @brief Window pixels -> world ray (origin, unit direction). */
    void ray(glm::vec2 px, glm::vec3& o, glm::vec3& d) const {
        const glm::vec2 ndc(2.0f * (px.x - rect.x) / std::max(1.0f, rect.w) - 1.0f,
                            1.0f - 2.0f * (px.y - rect.y) / std::max(1.0f, rect.h));
        const glm::mat4 inv = glm::inverse(proj * view);
        glm::vec4 a = inv * glm::vec4(ndc, 0.0f, 1.0f);
        glm::vec4 b = inv * glm::vec4(ndc, 1.0f, 1.0f);
        o = glm::vec3(a) / a.w;
        d = glm::normalize(glm::vec3(b) / b.w - o);
    }

    /** @brief World units per window pixel at `p` (for constant-screen-size handles). */
    float world_per_pixel(const glm::vec3& p) const {
        const auto a = project(p);
        const glm::vec3 up = glm::normalize(glm::vec3(glm::inverse(view)[1]));
        const auto b = project(p + up);
        if (!a || !b) return 0.01f;
        const float px = glm::distance(*a, *b);
        return px > 1e-4f ? 1.0f / px : 0.01f;
    }
};

// --- picking primitives ---

/** @brief Möller–Trumbore. @return Distance along the ray, or nullopt. Two-sided. */
inline std::optional<float> ray_triangle(const glm::vec3& o, const glm::vec3& d,
                                         const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 e1 = b - a, e2 = c - a;
    const glm::vec3 p = glm::cross(d, e2);
    const float det = glm::dot(e1, p);
    if (std::abs(det) < 1e-10f) return std::nullopt;
    const float inv = 1.0f / det;
    const glm::vec3 s = o - a;
    const float u = glm::dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) return std::nullopt;
    const glm::vec3 q = glm::cross(s, e1);
    const float v = glm::dot(d, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return std::nullopt;
    const float t = glm::dot(e2, q) * inv;
    return t > 1e-5f ? std::optional<float>(t) : std::nullopt;
}

/** @brief Slab test. @return Entry distance (0 if inside), or nullopt. */
inline std::optional<float> ray_aabb(const glm::vec3& o, const glm::vec3& d, const glm::vec3& lo, const glm::vec3& hi) {
    float t0 = 0.0f, t1 = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-12f) {
            if (o[i] < lo[i] || o[i] > hi[i]) return std::nullopt;
            continue;
        }
        float a = (lo[i] - o[i]) / d[i], b = (hi[i] - o[i]) / d[i];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1) return std::nullopt;
    }
    return t0;
}

/** @brief Distance from p to segment ab (2D). */
inline float point_segment_distance(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
    const glm::vec2 ab = b - a;
    const float l2 = glm::dot(ab, ab);
    const float t = l2 > 1e-9f ? std::clamp(glm::dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
    return glm::distance(p, a + ab * t);
}

/** @brief Parameter along line (p, u) of its closest approach to ray (o, d). */
inline float closest_on_line_to_ray(const glm::vec3& p, const glm::vec3& u, const glm::vec3& o, const glm::vec3& d) {
    const glm::vec3 w = p - o;
    const float a = glm::dot(u, u), b = glm::dot(u, d), c = glm::dot(d, d);
    const float dd = glm::dot(u, w), e = glm::dot(d, w);
    const float den = a * c - b * b;
    if (std::abs(den) < 1e-9f) return 0.0f;
    return (b * e - c * dd) / den;
}

/** @brief Rotation-degrees (Transform's XYZ Euler, applied ZYX) <-> matrix. */
inline glm::mat4 euler_to_matrix(const glm::vec3& deg) {
    return glm::eulerAngleZYX(glm::radians(deg.z), glm::radians(deg.y), glm::radians(deg.x));
}
inline glm::vec3 matrix_to_euler(const glm::mat4& m) {
    float z = 0, y = 0, x = 0;
    glm::extractEulerAngleZYX(m, z, y, x);
    return glm::degrees(glm::vec3(x, y, z));
}

enum class GizmoMode { Translate, Rotate, Scale };

/** @brief What a gizmo drag produced, cumulative since the drag started. */
struct GizmoDelta {
    bool active = false;     ///< A drag is in progress.
    bool started = false;    ///< It began this frame (capture start values now).
    bool finished = false;   ///< It ended this frame (close the undo step).
    glm::vec3 translate{0.0f};
    glm::vec3 rotate_axis{0, 0, 1};
    float rotate_deg = 0.0f;
    glm::vec3 scale{1.0f};
};

class Gizmo {
public:
    GizmoMode mode = GizmoMode::Translate;
    bool local = false;            ///< Axes follow the object's rotation.
    float translate_snap = 0.25f;  ///< Applied while Ctrl is held.
    float rotate_snap = 15.0f;
    float scale_snap = 0.1f;
    float size_px = 90.0f;

    bool dragging() const { return active_axis_ >= 0; }
    int hot_axis() const { return hot_axis_; }

    /**
     * @param basis  Columns are the gizmo's axes (world, or the object's rotation if local).
     * @param snap   Snapping modifier held.
     * @param allow_hover False when the mouse belongs to the UI (a popup, another widget).
     */
    GizmoDelta update(const ViewProj& vp, const glm::vec3& pivot, const glm::mat3& basis, glm::vec2 mouse,
                      bool pressed, bool down, bool released, bool snap, bool allow_hover) {
        GizmoDelta out;
        const float L = size_px * vp.world_per_pixel(pivot);
        if (active_axis_ < 0) {
            hot_axis_ = allow_hover ? hit_(vp, pivot, basis, L, mouse) : -1;
            if (pressed && hot_axis_ >= 0) {
                active_axis_ = hot_axis_;
                start_pivot_ = pivot;
                start_basis_ = basis;
                start_mouse_ = mouse;
                start_L_ = L;
                glm::vec3 o, d;
                vp.ray(mouse, o, d);
                start_param_ = drag_param_(vp, o, d);
                const auto c = vp.project(pivot);
                start_angle_ = c ? std::atan2(mouse.y - c->y, mouse.x - c->x) : 0.0f;
                out.started = true;
            }
        }
        if (active_axis_ >= 0) {
            out.active = true;
            glm::vec3 o, d;
            vp.ray(mouse, o, d);
            const int ax = active_axis_;
            if (mode == GizmoMode::Translate) {
                if (ax < 3) {
                    const glm::vec3 axis = start_basis_[ax];
                    float t = closest_on_line_to_ray(start_pivot_, axis, o, d) - start_param_.x;
                    if (snap && translate_snap > 0) t = std::round(t / translate_snap) * translate_snap;
                    out.translate = axis * t;
                } else {
                    const glm::vec3 n = vp.camera_forward();
                    const float den = glm::dot(d, n);
                    if (std::abs(den) > 1e-6f) {
                        const glm::vec3 hit = o + d * (glm::dot(start_pivot_ - o, n) / den);
                        glm::vec3 t = hit - start_hit_;
                        if (snap && translate_snap > 0) t = glm::round(t / translate_snap) * translate_snap;
                        out.translate = t;
                    }
                }
            } else if (mode == GizmoMode::Rotate) {
                const auto c = vp.project(start_pivot_);
                const float ang = c ? std::atan2(mouse.y - c->y, mouse.x - c->x) : start_angle_;
                float deg = glm::degrees(ang - start_angle_);
                while (deg > 180.0f) deg -= 360.0f;
                while (deg < -180.0f) deg += 360.0f;
                accumulated_deg_ += deg - last_deg_;
                last_deg_ = deg;
                const glm::vec3 axis = ax < 3 ? start_basis_[ax] : -vp.camera_forward();
                // Screen angles grow clockwise (y down); flip so dragging follows the ring.
                const float sign = glm::dot(axis, vp.camera_forward()) > 0.0f ? 1.0f : -1.0f;
                float total = accumulated_deg_ * sign;
                if (snap && rotate_snap > 0) total = std::round(total / rotate_snap) * rotate_snap;
                out.rotate_axis = axis;
                out.rotate_deg = total;
            } else {
                const auto c = vp.project(start_pivot_);
                if (ax < 3 && c) {
                    const auto tip = vp.project(start_pivot_ + start_basis_[ax] * start_L_);
                    glm::vec2 sa = tip ? *tip - *c : glm::vec2(1, 0);
                    const float len = std::max(1.0f, glm::length(sa));
                    sa /= len;
                    float f = 1.0f + glm::dot(mouse - start_mouse_, sa) / len;
                    if (snap && scale_snap > 0) f = std::round(f / scale_snap) * scale_snap;
                    out.scale = glm::vec3(1.0f);
                    out.scale[ax] = f;
                } else {
                    float f = 1.0f + (mouse.x - start_mouse_.x) / 120.0f;
                    if (snap && scale_snap > 0) f = std::round(f / scale_snap) * scale_snap;
                    out.scale = glm::vec3(f);
                }
            }
            if (released || !down) {
                out.finished = true;
                active_axis_ = -1;
                accumulated_deg_ = last_deg_ = 0.0f;
            }
        }
        return out;
    }

    /** @brief Draws the handles into the UI layer. */
    void draw(imm::Context& ctx, const ViewProj& vp, const glm::vec3& pivot, const glm::mat3& basis) const {
        const float L = size_px * vp.world_per_pixel(pivot);
        const auto c = vp.project(pivot);
        if (!c) return;
        const glm::vec4 cols[3] = {ctx.style.axis_x, ctx.style.axis_y, ctx.style.axis_z};
        const glm::vec4 hot{1.0f, 0.95f, 0.45f, 1.0f};
        auto col = [&](int i) { return (active_axis_ == i || (active_axis_ < 0 && hot_axis_ == i)) ? hot : cols[i]; };
        if (mode == GizmoMode::Rotate) {
            for (int i = 0; i < 3; ++i) draw_ring_(ctx, vp, pivot, basis[i], L, col(i), 2.0f);
            draw_ring_(ctx, vp, pivot, -vp.camera_forward(), L * 1.15f,
                       (active_axis_ == 3 || (active_axis_ < 0 && hot_axis_ == 3)) ? hot : glm::vec4(0.85f, 0.85f, 0.85f, 0.8f), 1.5f);
            return;
        }
        for (int i = 0; i < 3; ++i) {
            const auto tip = vp.project(pivot + basis[i] * L);
            if (!tip) continue;
            ctx.line(*c, *tip, col(i), 2.5f);
            if (mode == GizmoMode::Translate) {
                // Arrow head.
                glm::vec2 dir = *tip - *c;
                const float len = glm::length(dir);
                if (len > 1) {
                    dir /= len;
                    const glm::vec2 n(-dir.y, dir.x);
                    ctx.triangle(*tip + dir * 12.0f, *tip + n * 5.0f, *tip - n * 5.0f, col(i));
                }
            } else {
                ctx.fill(imm::Box{tip->x - 5, tip->y - 5, 10, 10}, col(i));
            }
        }
        const glm::vec4 cc = (active_axis_ == 3 || (active_axis_ < 0 && hot_axis_ == 3)) ? hot : glm::vec4(0.9f, 0.9f, 0.9f, 0.9f);
        ctx.outline(imm::Box{c->x - 6, c->y - 6, 12, 12}, cc, 2.0f);
    }

private:
    int hit_(const ViewProj& vp, const glm::vec3& pivot, const glm::mat3& basis, float L, glm::vec2 mouse) const {
        const auto c = vp.project(pivot);
        if (!c) return -1;
        if (glm::distance(mouse, *c) < 9.0f) return 3;
        int best = -1;
        float best_d = 9.0f;
        if (mode == GizmoMode::Rotate) {
            for (int i = 0; i < 4; ++i) {
                const glm::vec3 axis = i < 3 ? basis[i] : -vp.camera_forward();
                const float r = i < 3 ? L : L * 1.15f;
                const float dist = ring_distance_(vp, pivot, axis, r, mouse);
                if (dist < best_d) { best_d = dist; best = i; }
            }
            return best;
        }
        for (int i = 0; i < 3; ++i) {
            const auto tip = vp.project(pivot + basis[i] * L);
            if (!tip) continue;
            const float dist = point_segment_distance(mouse, *c, *tip + glm::normalize(*tip - *c + glm::vec2(1e-6f)) * 10.0f);
            if (dist < best_d) { best_d = dist; best = i; }
        }
        return best;
    }

    glm::vec2 drag_param_(const ViewProj& vp, const glm::vec3& o, const glm::vec3& d) {
        if (mode == GizmoMode::Translate && active_axis_ < 3 && active_axis_ >= 0) {
            return {closest_on_line_to_ray(start_pivot_, start_basis_[active_axis_], o, d), 0.0f};
        }
        const glm::vec3 n = vp.camera_forward();
        const float den = glm::dot(d, n);
        start_hit_ = std::abs(den) > 1e-6f ? o + d * (glm::dot(start_pivot_ - o, n) / den) : start_pivot_;
        return {0.0f, 0.0f};
    }

    static void ring_basis_(const glm::vec3& axis, glm::vec3& u, glm::vec3& v) {
        const glm::vec3 a = glm::normalize(axis);
        u = glm::normalize(glm::cross(a, std::abs(a.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0)));
        v = glm::cross(a, u);
    }
    float ring_distance_(const ViewProj& vp, const glm::vec3& p, const glm::vec3& axis, float r, glm::vec2 mouse) const {
        glm::vec3 u, v;
        ring_basis_(axis, u, v);
        float best = 1e9f;
        std::optional<glm::vec2> prev;
        for (int i = 0; i <= 48; ++i) {
            const float a = 6.2831853f * i / 48.0f;
            const auto q = vp.project(p + (u * std::cos(a) + v * std::sin(a)) * r);
            if (q && prev) best = std::min(best, point_segment_distance(mouse, *prev, *q));
            prev = q;
        }
        return best;
    }
    static void draw_ring_(imm::Context& ctx, const ViewProj& vp, const glm::vec3& p, const glm::vec3& axis, float r,
                           const glm::vec4& c, float t) {
        glm::vec3 u, v;
        ring_basis_(axis, u, v);
        std::optional<glm::vec2> prev;
        for (int i = 0; i <= 64; ++i) {
            const float a = 6.2831853f * i / 64.0f;
            const auto q = vp.project(p + (u * std::cos(a) + v * std::sin(a)) * r);
            if (q && prev) ctx.line(*prev, *q, c, t);
            prev = q;
        }
    }

    int hot_axis_ = -1;
    int active_axis_ = -1;
    glm::vec3 start_pivot_{0.0f};
    glm::mat3 start_basis_{1.0f};
    glm::vec2 start_mouse_{0.0f};
    glm::vec2 start_param_{0.0f};
    glm::vec3 start_hit_{0.0f};
    float start_L_ = 1.0f;
    float start_angle_ = 0.0f;
    float accumulated_deg_ = 0.0f;
    float last_deg_ = 0.0f;
};

} // namespace toy::editor

#endif // TOYEDITOR_VIEWPORT_GIZMO_H
