#pragma once

/**
 * @file physics_fixtures.h
 * @brief Shared headless PhysicsWorld builders for physxcoopa's suites: ground, boxes, spheres,
 *        capsules, convex hulls, hand-built triangle meshes (with edge adjacency), and the
 *        "settled flat" check.
 *
 * Every helper builds exactly what the tests' hand-computed expectations assume (unit mass, the
 * matching closed-form inertia, identity orientation unless given), so changing one changes the
 * meaning of many tests -- keep them boring.
 */

#include <physxcoopa/world.h>
#include <physxcoopa/collision/shape.h>
#include <physxcoopa/dynamics/body.h>
#include <physxcoopa/dynamics/inertia.h>
#include <physxcoopa/geometry/convex_hull.h>
#include <physxcoopa/geometry/triangle_mesh.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace physxtest {

using namespace coopa::physx;

inline const glm::quat k_identity(1.0f, 0.0f, 0.0f, 0.0f);

/** @brief Static 8 x 8 m slab whose top face is at world z = 0. */
inline dynamics::BodyId add_static_ground(PhysicsWorld& world, dynamics::PhysicsMaterial* mat = nullptr) {
    dynamics::Body ground;
    ground.type = dynamics::BodyType::Static;
    ground.position = glm::vec3(0.0f, 0.0f, -0.1f); // top surface at world z=0
    collision::Shape shape = collision::Shape::make_box(glm::vec3(4.0f, 4.0f, 0.1f));
    shape.material = mat;
    return world.add_body(ground, shape);
}

/** @brief Unit-mass dynamic sphere. */
inline dynamics::BodyId add_dynamic_sphere(PhysicsWorld& world, const glm::vec3& pos, float radius,
                                           dynamics::PhysicsMaterial* mat = nullptr) {
    dynamics::Body body;
    body.type = dynamics::BodyType::Dynamic;
    body.position = pos;
    body.mass = 1.0f;
    body.inv_mass = 1.0f;
    body.inv_inertia_local = dynamics::sphere_inverse_inertia(radius, body.mass);
    collision::Shape shape = collision::Shape::make_sphere(radius);
    shape.material = mat;
    return world.add_body(body, shape);
}

/** @brief Unit-mass dynamic box. */
inline dynamics::BodyId add_dynamic_box(PhysicsWorld& world, const glm::vec3& pos, const glm::vec3& half_extents,
                                        const glm::quat& rot = k_identity, dynamics::PhysicsMaterial* mat = nullptr) {
    dynamics::Body body;
    body.position = pos;
    body.orientation = rot;
    body.mass = 1.0f;
    body.inv_mass = 1.0f;
    body.inv_inertia_local = dynamics::box_inverse_inertia(half_extents, body.mass);
    collision::Shape shape = collision::Shape::make_box(half_extents);
    shape.material = mat;
    return world.add_body(body, shape);
}

/** @brief Unit-mass dynamic convex hull, with the matching closed-form (diagonal) inverse
 *         inertia from hull_inverse_inertia() -- same contract as add_dynamic_box(). The caller
 *         owns `hull` and must keep it alive for as long as the body exists: Shape::hull is
 *         non-owning, same as Shape::mesh. */
inline dynamics::BodyId add_dynamic_hull(PhysicsWorld& world, const geometry::ConvexHull* hull, const glm::vec3& pos,
                                         const glm::quat& rot = k_identity, dynamics::PhysicsMaterial* mat = nullptr) {
    dynamics::Body body;
    body.position = pos;
    body.orientation = rot;
    body.mass = 1.0f;
    body.inv_mass = 1.0f;
    body.inv_inertia_local = dynamics::hull_inverse_inertia(*hull, 1.0f, body.mass);
    collision::Shape shape = collision::Shape::make_hull(hull);
    shape.material = mat;
    return world.add_body(body, shape);
}

inline dynamics::BodyId add_static_box(PhysicsWorld& world, const glm::vec3& pos, const glm::vec3& half_extents,
                                       const glm::quat& rot = k_identity, dynamics::PhysicsMaterial* mat = nullptr) {
    dynamics::Body body;
    body.type = dynamics::BodyType::Static;
    body.position = pos;
    body.orientation = rot;
    collision::Shape shape = collision::Shape::make_box(half_extents);
    shape.material = mat;
    return world.add_body(body, shape);
}

/** @brief Unit-mass dynamic capsule along local `axis` (0 = X, 1 = Y, 2 = Z), with the
 *         capsule's (perp, perp, axial) inverse inertia permuted onto that axis. */
inline dynamics::BodyId add_dynamic_capsule(PhysicsWorld& world, const glm::vec3& pos, float radius, float half_height,
                                            int axis, const glm::quat& rot = k_identity) {
    dynamics::Body body;
    body.position = pos;
    body.orientation = rot;
    body.mass = 1.0f;
    body.inv_mass = 1.0f;
    glm::vec3 raw = dynamics::capsule_inverse_inertia(radius, half_height, body.mass); // (perp, perp, axial)
    glm::vec3 local;
    if (axis == 0) local = glm::vec3(raw.z, raw.y, raw.x);      // axial -> X
    else if (axis == 1) local = glm::vec3(raw.x, raw.z, raw.y); // axial -> Y
    else local = raw;                                            // axial -> Z (already there)
    body.inv_inertia_local = local;
    return world.add_body(body, collision::Shape::make_capsule(radius, half_height, axis));
}

inline dynamics::BodyId add_static_mesh(PhysicsWorld& world, const geometry::TriangleMesh* mesh,
                                        const glm::vec3& pos = glm::vec3(0.0f)) {
    dynamics::Body body;
    body.type = dynamics::BodyType::Static;
    body.position = pos;
    return world.add_body(body, collision::Shape::make_mesh(mesh));
}

/**
 * Two coplanar triangles sharing one diagonal edge, forming a flat 4x4 floor:
 *   v3(-2,2,0) ---- v2(2,2,0)
 *      |  tri1  /     |
 *      |      /       |
 *      |    /   tri0  |
 *   v0(-2,-2,0) ---- v1(2,-2,0)
 * tri0 = (v0,v1,v2), tri1 = (v0,v2,v3); both wind to face normal +Z. The shared edge is
 * tri0's edge2 (v2->v0) / tri1's edge0 (v0->v2) -- adjacency below wires exactly that pair,
 * every other edge is a mesh boundary (default-constructed k_no_neighbor).
 */
inline geometry::TriangleMesh make_two_triangle_floor() {
    std::vector<glm::vec3> vertices = {
        glm::vec3(-2.0f, -2.0f, 0.0f), glm::vec3(2.0f, -2.0f, 0.0f),
        glm::vec3(2.0f, 2.0f, 0.0f), glm::vec3(-2.0f, 2.0f, 0.0f),
    };
    std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
    std::vector<glm::vec3> normals = {glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, 1.0f)};
    std::vector<geometry::TriangleAdjacency> adjacency(2);
    adjacency[0].neighbor[2] = 1;
    adjacency[1].neighbor[0] = 0;
    return geometry::TriangleMesh(std::move(vertices), std::move(indices), std::move(normals), std::move(adjacency));
}

/** Builds a TriangleMesh from raw vertex/index arrays, computing face normals (right-handed
 *  winding) and edge adjacency by shared vertex pairs -- what TriangleMeshLoader does after
 *  welding, for hand-built test meshes. */
inline geometry::TriangleMesh make_mesh_with_adjacency(std::vector<glm::vec3> vertices, std::vector<uint32_t> indices) {
    size_t tri_count = indices.size() / 3;
    std::vector<glm::vec3> normals(tri_count);
    std::vector<geometry::TriangleAdjacency> adjacency(tri_count);
    std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, int>> edges;
    for (size_t t = 0; t < tri_count; ++t) {
        const glm::vec3& a = vertices[indices[t * 3 + 0]];
        const glm::vec3& b = vertices[indices[t * 3 + 1]];
        const glm::vec3& c = vertices[indices[t * 3 + 2]];
        normals[t] = glm::normalize(glm::cross(b - a, c - a));
        for (int e = 0; e < 3; ++e) {
            uint32_t i0 = indices[t * 3 + e], i1 = indices[t * 3 + (e + 1) % 3];
            auto key = std::make_pair(std::min(i0, i1), std::max(i0, i1));
            auto it = edges.find(key);
            if (it == edges.end()) {
                edges.emplace(key, std::make_pair(static_cast<uint32_t>(t), e));
            } else {
                adjacency[t].neighbor[e] = it->second.first;
                adjacency[it->second.first].neighbor[it->second.second] = static_cast<uint32_t>(t);
            }
        }
    }
    return geometry::TriangleMesh(std::move(vertices), std::move(indices), std::move(normals), std::move(adjacency));
}

/** A 30-degree ramp rising along +X: z = x * tan(30deg) for x in [0, 4], y in [-2, 2]. */
inline geometry::TriangleMesh make_ramp_mesh() {
    const float h = 4.0f * std::tan(glm::radians(30.0f));
    return make_mesh_with_adjacency({glm::vec3(0.0f, -2.0f, 0.0f), glm::vec3(4.0f, -2.0f, h),
                                     glm::vec3(4.0f, 2.0f, h), glm::vec3(0.0f, 2.0f, 0.0f)},
                                    {0, 1, 2, 0, 2, 3});
}

inline void step_world(PhysicsWorld& world, int steps) {
    for (int i = 0; i < steps; ++i) world.step_fixed(util::k_default_fixed_dt);
}

/** @brief max |axis.z| over the body's three local axes: 1 when some face lies flat on a
 *         horizontal surface. > 0.999 (cos 2 degrees ~= 0.99939) is the "settled flat" bar. */
inline float best_up_alignment(const glm::quat& orientation) {
    float best = 0.0f;
    for (const glm::vec3& axis : {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)}) {
        best = std::max(best, std::abs((orientation * axis).z));
    }
    return best;
}

/** @brief At rest: asleep, or both velocities under 0.02. */
inline bool is_settled(const dynamics::Body& b) {
    return !b.awake || (glm::length(b.linear_velocity) < 0.02f && glm::length(b.angular_velocity) < 0.02f);
}

} // namespace physxtest
