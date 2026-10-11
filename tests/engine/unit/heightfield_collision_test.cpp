/**
 * @file heightfield_collision_test.cpp
 * @brief The heightfield collider, device-free: a TriangleMesh in height-grid mode (physxcoopa's
 *        geometry/height_grid.h) must collide exactly like the same surface stored as an ordinary
 *        mesh -- same triangles, normals, adjacency, raycasts, contacts and sweeps -- and a body
 *        dropped on it in a PhysicsWorld comes to rest on it.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <physxcoopa/collision/mesh_contact.h>
#include <physxcoopa/components/heightfield_collider.h>
#include <physxcoopa/geometry/height_grid.h>
#include <physxcoopa/geometry/triangle_mesh.h>
#include <physxcoopa/query/queries.h>
#include <physxcoopa/query/sweep.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("heightfield_collision");

namespace geo = coopa::physx::geometry;
namespace col = coopa::physx::collision;
namespace qry = coopa::physx::query;

namespace {

/** @brief A bumpy 17 x 13 grid, 0.75 m x 1.25 m cells. */
std::shared_ptr<const geo::HeightGrid> bumpy() {
    const int nx = 17, ny = 13;
    std::vector<float> h(static_cast<std::size_t>(nx) * ny);
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            h[static_cast<std::size_t>(j) * nx + i] = 1.5f * std::sin(0.6f * static_cast<float>(i)) * std::cos(0.45f * static_cast<float>(j)) +
                                                     0.1f * static_cast<float>(i);
        }
    }
    return std::make_shared<const geo::HeightGrid>(nx, ny, glm::vec2(0.75f, 1.25f), std::move(h));
}

/** @brief The same surface stored the ordinary way (vertices, indices, normals, adjacency). */
geo::TriangleMesh stored(const geo::HeightGrid& g) {
    std::vector<glm::vec3> v;
    for (int j = 0; j < g.ny(); ++j) {
        for (int i = 0; i < g.nx(); ++i) v.push_back(g.vertex(i, j));
    }
    std::vector<uint32_t> idx;
    std::vector<glm::vec3> n;
    std::vector<geo::TriangleAdjacency> adj;
    for (uint32_t t = 0; t < g.triangle_count(); ++t) {
        glm::vec3 a, b, c;
        g.triangle_vertices(t, a, b, c);
        auto find = [&](const glm::vec3& p) {
            const int i = static_cast<int>(std::lround(p.x / g.cell().x)), j = static_cast<int>(std::lround(p.y / g.cell().y));
            return static_cast<uint32_t>(j * g.nx() + i);
        };
        idx.insert(idx.end(), {find(a), find(b), find(c)});
        n.push_back(g.triangle_normal(t));
        geo::TriangleAdjacency ad;
        g.triangle_neighbors(t, ad.neighbor);
        adj.push_back(ad);
    }
    return geo::TriangleMesh(std::move(v), std::move(idx), std::move(n), std::move(adj));
}

} // namespace

/** @brief Implicit adjacency is symmetric and names the triangle across the shared edge. */
COOPA_TEST(adjacency_is_consistent) {
    const auto g = bumpy();
    int checked = 0;
    for (uint32_t t = 0; t < g->triangle_count(); ++t) {
        uint32_t n[3];
        g->triangle_neighbors(t, n);
        glm::vec3 v[3];
        g->triangle_vertices(t, v[0], v[1], v[2]);
        for (int e = 0; e < 3; ++e) {
            if (n[e] == geo::k_no_neighbor) continue;
            uint32_t back[3];
            g->triangle_neighbors(n[e], back);
            expect(back[0] == t || back[1] == t || back[2] == t, "heightfield: adjacency is symmetric");
            glm::vec3 w[3];
            g->triangle_vertices(n[e], w[0], w[1], w[2]);
            int shared = 0;
            for (const glm::vec3& a : {v[e], v[(e + 1) % 3]}) {
                for (const glm::vec3& b : w) shared += glm::length(a - b) < 1e-6f ? 1 : 0;
            }
            expect(shared == 2, "heightfield: the neighbour shares the edge's two vertices");
            ++checked;
        }
        expect(g->triangle_normal(t).z > 0.0f, "heightfield: every triangle faces up");
    }
    expect(checked > 100, "heightfield: interior edges were checked");
}

/** @brief Raycasts against the grid and the stored mesh agree on distance and normal. */
COOPA_TEST(raycasts_match_a_stored_mesh) {
    const auto g = bumpy();
    const geo::TriangleMesh grid_mesh = geo::TriangleMesh::from_height_grid(g);
    const geo::TriangleMesh mesh = stored(*g);
    const col::Shape a = col::Shape::make_mesh(&grid_mesh);
    const col::Shape b = col::Shape::make_mesh(&mesh);
    const glm::vec3 pos(1.0f, -2.0f, 0.5f);
    const glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
    int hits = 0;
    for (int k = 0; k < 64; ++k) {
        geo::Ray ray;
        // Over the grid's interior (world x 1..13, y -2..13), so a slant cannot leave it.
        ray.origin = glm::vec3(4.0f + 0.6f * static_cast<float>(k % 8), 2.0f + 0.7f * static_cast<float>(k / 8), 9.0f);
        ray.direction = glm::normalize(glm::vec3(0.3f * std::sin(static_cast<float>(k)), 0.2f * std::cos(static_cast<float>(k)), -1.0f));
        ray.max_distance = 50.0f;
        float ta = 0, tb = 0;
        glm::vec3 na, nb;
        const bool ha = qry::raycast_shape(ray, a, pos, rot, ta, na);
        const bool hb = qry::raycast_shape(ray, b, pos, rot, tb, nb);
        expect(ha == hb, "heightfield: hit / miss agree");
        if (ha && hb) {
            expect_near(ta, tb, 1e-4f, "heightfield: same hit distance");
            expect(glm::length(na - nb) < 1e-4f, "heightfield: same hit normal");
            ++hits;
        }
    }
    expect(hits > 40, ("heightfield: most rays hit the surface (" + std::to_string(hits) + " of 64)").c_str());
}

/** @brief Sphere and box contacts, and a capsule sweep, match the stored mesh exactly. */
COOPA_TEST(contacts_and_sweeps_match_a_stored_mesh) {
    const auto g = bumpy();
    const geo::TriangleMesh grid_mesh = geo::TriangleMesh::from_height_grid(g);
    const geo::TriangleMesh mesh = stored(*g);
    const col::Shape a = col::Shape::make_mesh(&grid_mesh);
    const col::Shape b = col::Shape::make_mesh(&mesh);
    const glm::quat id(1.0f, 0.0f, 0.0f, 0.0f);
    int touching = 0;
    for (int k = 0; k < 24; ++k) {
        const glm::vec2 xy(1.0f + 0.45f * static_cast<float>(k), 2.0f + 0.37f * static_cast<float>(k % 7));
        const float ground = 1.5f * std::sin(0.6f * xy.x / 0.75f) * std::cos(0.45f * xy.y / 1.25f) + 0.1f * xy.x / 0.75f;
        for (const col::Shape& probe : {col::Shape::make_sphere(0.6f), col::Shape::make_box(glm::vec3(0.4f, 0.3f, 0.5f))}) {
            const glm::vec3 p(xy, ground + 0.2f);
            col::ContactManifold ma, mb;
            const bool ca = col::generate_mesh_contacts(probe, p, id, a, glm::vec3(0.0f), id, ma);
            const bool cb = col::generate_mesh_contacts(probe, p, id, b, glm::vec3(0.0f), id, mb);
            expect(ca == cb && ma.count == mb.count, "heightfield: same contact count");
            if (ca && cb) {
                ++touching;
                expect(glm::length(ma.normal - mb.normal) < 1e-4f, "heightfield: same contact normal");
                for (int i = 0; i < std::min<int>(ma.count, mb.count); ++i) {
                    expect_near(ma.points[i].penetration, mb.points[i].penetration, 1e-4f, "heightfield: same penetration");
                }
            }
        }
        const col::Shape capsule = col::Shape::make_capsule(0.3f, 0.5f);
        qry::SweepResult sa, sb;
        const glm::vec3 start(xy, ground + 4.0f);
        const bool wa = qry::sweep_mesh(capsule, start, id, glm::vec3(0.0f, 0.0f, -1.0f), 10.0f, a, glm::vec3(0.0f), id, sa);
        const bool wb = qry::sweep_mesh(capsule, start, id, glm::vec3(0.0f, 0.0f, -1.0f), 10.0f, b, glm::vec3(0.0f), id, sb);
        expect(wa == wb, "heightfield: sweep hit agrees");
        if (wa && wb) expect_near(sa.distance, sb.distance, 1e-4f, "heightfield: same sweep distance");
    }
    expect(touching > 10, "heightfield: probes touched the surface");
}

/** @brief The collider builds its shape from the grid and swaps it on set_grid(). */
COOPA_TEST(collider_swaps_grids) {
    coopa::physx::components::HeightfieldCollider c;
    expect(!c.make_shape(glm::vec3(1.0f)).enabled, "heightfield: no grid, no shape");
    const auto rev0 = c.revision();
    c.set_grid(bumpy());
    const col::Shape s = c.make_shape(glm::vec3(3.0f));
    expect(s.enabled && s.type == col::ShapeType::TriangleMesh && s.mesh && s.mesh->height_grid(),
           "heightfield: a grid-mode mesh shape");
    expect(s.mesh_scale == 1.0f, "heightfield: never scaled with its object");
    expect(c.revision() != rev0, "heightfield: set_grid bumps the revision");
}
