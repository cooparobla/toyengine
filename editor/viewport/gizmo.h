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
    std::optional<glm::vec2> project(const glm::vec3& p) const;

    /** @brief Window pixels -> world ray (origin, unit direction). */
    void ray(glm::vec2 px, glm::vec3& o, glm::vec3& d) const;

    /** @brief World units per window pixel at `p` (for constant-screen-size handles). */
    float world_per_pixel(const glm::vec3& p) const;
};

// --- picking primitives ---

/** @brief Möller–Trumbore. @return Distance along the ray, or nullopt. Two-sided. */
std::optional<float> ray_triangle(const glm::vec3& o, const glm::vec3& d,
                                         const glm::vec3& a, const glm::vec3& b, const glm::vec3& c);

/** @brief Slab test. @return Entry distance (0 if inside), or nullopt. */
std::optional<float> ray_aabb(const glm::vec3& o, const glm::vec3& d, const glm::vec3& lo, const glm::vec3& hi);

/** @brief Distance from p to segment ab (2D). */
float point_segment_distance(glm::vec2 p, glm::vec2 a, glm::vec2 b);

/** @brief Parameter along line (p, u) of its closest approach to ray (o, d). */
float closest_on_line_to_ray(const glm::vec3& p, const glm::vec3& u, const glm::vec3& o, const glm::vec3& d);

/** @brief Rotation-degrees (Transform's XYZ Euler, applied ZYX) <-> matrix. */
inline glm::mat4 euler_to_matrix(const glm::vec3& deg) {
    return glm::eulerAngleZYX(glm::radians(deg.z), glm::radians(deg.y), glm::radians(deg.x));
}
glm::vec3 matrix_to_euler(const glm::mat4& m);

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
                      bool pressed, bool down, bool released, bool snap, bool allow_hover);

    /** @brief Draws the handles into the UI layer. */
    glm::vec4 free_color{0.85f, 0.85f, 0.85f, 0.8f};   ///< the view-plane handle (theme: viewport.gizmo_free)

    void draw(imm::Context& ctx, const ViewProj& vp, const glm::vec3& pivot, const glm::mat3& basis) const;

private:
    int hit_(const ViewProj& vp, const glm::vec3& pivot, const glm::mat3& basis, float L, glm::vec2 mouse) const;

    /** @brief Centre offset of the plane handle perpendicular to axis k. */
    static glm::vec3 plane_offset_(const glm::mat3& basis, int k, float L);
    /** @brief The plane handle's screen quad; false if behind the camera or seen edge-on. */
    static bool plane_quad_(const ViewProj& vp, const glm::vec3& pivot, const glm::mat3& basis, float L, int k, glm::vec2 q[4]);
    static bool in_quad_(glm::vec2 p, const glm::vec2 q[4]);

    glm::vec2 drag_param_(const ViewProj& vp, const glm::vec3& o, const glm::vec3& d);

    static void ring_basis_(const glm::vec3& axis, glm::vec3& u, glm::vec3& v);
    float ring_distance_(const ViewProj& vp, const glm::vec3& p, const glm::vec3& axis, float r, glm::vec2 mouse) const;
    static void draw_ring_(imm::Context& ctx, const ViewProj& vp, const glm::vec3& p, const glm::vec3& axis, float r,
                           const glm::vec4& c, float t);

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
