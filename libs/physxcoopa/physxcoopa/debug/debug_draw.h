/**
 * @file debug_draw.h
 * @brief Pure-data debug visualization: PhysicsWorld::debug_draw() (world.h) fills a DebugDraw
 *        with plain line segments; the consuming engine renders them. No Vulkan, no gfxcoopa
 *        dependency -- see the plan's toyengine-integration section for where the render side
 *        of this hooks in (a low-internal-resolution engine has no ready-made overlay seam).
 */

#ifndef PHYSXCOOPA_DEBUG_DEBUG_DRAW_H
#define PHYSXCOOPA_DEBUG_DEBUG_DRAW_H

#include <physxcoopa/geometry/aabb.h>
#include <physxcoopa/geometry/obb.h>
#include <physxcoopa/geometry/sphere.h>
#include <physxcoopa/geometry/capsule.h>
#include <physxcoopa/geometry/triangle_mesh.h>

#include <glm/glm.hpp>
#include <algorithm>
#include <glm/gtc/constants.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace coopa {
namespace physx {
namespace debug {

/**
 * @struct DebugLine
 * @brief One line segment. `color` is packed 0xRRGGBBAA.
 */
struct DebugLine {
    glm::vec3 a{0.0f};
    glm::vec3 b{0.0f};
    uint32_t color = 0xFFFFFFFFu;
};

/** @brief Which categories of debug geometry PhysicsWorld::debug_draw() should emit. */
enum class DebugDrawFlags : uint32_t {
    None = 0,
    Colliders = 1u << 0,
    BVH = 1u << 1,
    Contacts = 1u << 2,
    Joints = 1u << 3,
    All = Colliders | BVH | Contacts | Joints,
};

inline DebugDrawFlags operator|(DebugDrawFlags a, DebugDrawFlags b) {
    return static_cast<DebugDrawFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool has_flag(DebugDrawFlags flags, DebugDrawFlags flag) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
}

/** @brief Color palette debug_draw() colors state by: white awake, green sleeping, red contact
 *         normals, yellow trigger colliders (overrides the awake/sleeping color), magenta joint
 *         anchors/axes. */
namespace colors {
inline constexpr uint32_t k_awake = 0xFFFFFFFFu;
inline constexpr uint32_t k_sleeping = 0x00FF00FFu;
inline constexpr uint32_t k_trigger = 0xFFFF00FFu;
inline constexpr uint32_t k_contact_normal = 0xFF0000FFu;
inline constexpr uint32_t k_bvh_node = 0x4080FFFFu;
inline constexpr uint32_t k_joint = 0xFF00FFFFu;
} // namespace colors

/**
 * @struct DebugDraw
 * @brief Accumulates one frame's worth of debug lines.
 */
struct DebugDraw {
    std::vector<DebugLine> lines;

    void add_line(const glm::vec3& a, const glm::vec3& b, uint32_t color) { lines.push_back(DebugLine{a, b, color}); }

    /** @brief The 12 edges of a world-space AABB. */
    void add_aabb(const geometry::AABB& box, uint32_t color) {
        glm::vec3 c[8];
        for (int i = 0; i < 8; ++i) {
            c[i] = glm::vec3((i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y, (i & 4) ? box.max.z : box.min.z);
        }
        static constexpr int edges[12][2] = {{0, 1}, {0, 2}, {0, 4}, {1, 3}, {1, 5}, {2, 3},
                                              {2, 6}, {3, 7}, {4, 5}, {4, 6}, {5, 7}, {6, 7}};
        for (auto& e : edges) add_line(c[e[0]], c[e[1]], color);
    }

    /** @brief The 12 edges of an oriented box. */
    void add_obb(const geometry::OBB& box, uint32_t color) {
        glm::vec3 ax, ay, az;
        box.axes(ax, ay, az);
        glm::vec3 he = box.half_extents;
        glm::vec3 c[8];
        for (int i = 0; i < 8; ++i) {
            float sx = (i & 1) ? 1.0f : -1.0f, sy = (i & 2) ? 1.0f : -1.0f, sz = (i & 4) ? 1.0f : -1.0f;
            c[i] = box.center + ax * (sx * he.x) + ay * (sy * he.y) + az * (sz * he.z);
        }
        static constexpr int edges[12][2] = {{0, 1}, {0, 2}, {0, 4}, {1, 3}, {1, 5}, {2, 3},
                                              {2, 6}, {3, 7}, {4, 5}, {4, 6}, {5, 7}, {6, 7}};
        for (auto& e : edges) add_line(c[e[0]], c[e[1]], color);
    }

    /** @brief Three axis-aligned great circles approximating a sphere. */
    void add_sphere(const geometry::Sphere& sphere, uint32_t color, int segments = 16) {
        for (int axis = 0; axis < 3; ++axis) {
            glm::vec3 prev;
            for (int i = 0; i <= segments; ++i) {
                float t = glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(segments);
                glm::vec3 p(0.0f);
                float cx = std::cos(t) * sphere.radius, sx = std::sin(t) * sphere.radius;
                if (axis == 0) p = glm::vec3(0.0f, cx, sx);
                else if (axis == 1) p = glm::vec3(cx, 0.0f, sx);
                else p = glm::vec3(cx, sx, 0.0f);
                p += sphere.center;
                if (i > 0) add_line(prev, p, color);
                prev = p;
            }
        }
    }

    /** @brief A capsule: two end-cap circles (perpendicular to the axis) plus four side lines. */
    void add_capsule(const geometry::Capsule& cap, uint32_t color, int segments = 16) {
        glm::vec3 axis = cap.b - cap.a;
        float len = glm::length(axis);
        glm::vec3 dir = len > 1e-6f ? axis / len : glm::vec3(0.0f, 0.0f, 1.0f);
        glm::vec3 arbitrary = std::abs(dir.z) < 0.9f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        glm::vec3 u = glm::normalize(glm::cross(dir, arbitrary));
        glm::vec3 v = glm::cross(dir, u);

        auto ring = [&](const glm::vec3& center) {
            glm::vec3 prev;
            for (int i = 0; i <= segments; ++i) {
                float t = glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(segments);
                glm::vec3 p = center + (u * std::cos(t) + v * std::sin(t)) * cap.radius;
                if (i > 0) add_line(prev, p, color);
                prev = p;
            }
        };
        ring(cap.a);
        ring(cap.b);
        for (int i = 0; i < 4; ++i) {
            float t = glm::half_pi<float>() * static_cast<float>(i);
            glm::vec3 offset = (u * std::cos(t) + v * std::sin(t)) * cap.radius;
            add_line(cap.a + offset, cap.b + offset, color);
        }
    }

    /** @brief Every triangle edge of a mesh, placed at `transform`. Fine for the small
     *         collider meshes v1 targets; a large terrain mesh would want frustum culling
     *         here first -- not needed yet, so not built. */
    void add_mesh(const geometry::TriangleMesh& mesh, const glm::mat4& transform, uint32_t color) {
        if (const geometry::HeightGrid* grid = mesh.height_grid()) {
            // A heightfield has millions of triangles: draw its grid lines, thinned to ~128 a side.
            const int step_i = std::max(1, (grid->nx() - 1) / 128), step_j = std::max(1, (grid->ny() - 1) / 128);
            auto w = [&](int i, int j) { return glm::vec3(transform * glm::vec4(grid->vertex(i, j), 1.0f)); };
            for (int j = 0; j < grid->ny(); j += step_j) {
                for (int i = 0; i + step_i < grid->nx(); i += step_i) add_line(w(i, j), w(i + step_i, j), color);
            }
            for (int i = 0; i < grid->nx(); i += step_i) {
                for (int j = 0; j + step_j < grid->ny(); j += step_j) add_line(w(i, j), w(i, j + step_j), color);
            }
            return;
        }
        for (size_t tri = 0; tri < mesh.triangle_count(); ++tri) {
            glm::vec3 v0, v1, v2;
            mesh.triangle_vertices(static_cast<uint32_t>(tri), v0, v1, v2);
            glm::vec3 w0 = glm::vec3(transform * glm::vec4(v0, 1.0f));
            glm::vec3 w1 = glm::vec3(transform * glm::vec4(v1, 1.0f));
            glm::vec3 w2 = glm::vec3(transform * glm::vec4(v2, 1.0f));
            add_line(w0, w1, color);
            add_line(w1, w2, color);
            add_line(w2, w0, color);
        }
    }
};

} // namespace debug
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_DEBUG_DEBUG_DRAW_H
