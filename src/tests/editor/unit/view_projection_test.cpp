/**
 * @file view_projection_test.cpp
 * @brief Viewport picking math: project() and ray() are inverses, ray-triangle / ray-box hits, and the
 * inspector's Euler angles use Transform's convention.
 */

#include <coopa/testing/test.h>

#include "editor/mesh/mesh_bvh.h"
#include "editor/support/view_helpers.h"

COOPA_TEST_SUITE("view_projection");

namespace toy::editor::testing {

COOPA_TEST(project_and_ray_are_inverses_and_hits_agree) {
    const ViewProj vp = test_view_proj();
    auto c = vp.project(glm::vec3(0));
    expect(c && glm::distance(*c, glm::vec2(420, 230)) < 0.01f, "the look-at point projects to the rect centre");
    glm::vec3 o, d;
    vp.ray({420, 230}, o, d);
    expect(glm::dot(d, glm::vec3(0, 1, 0)) > 0.9999f, "the centre ray points along the view direction");
    vp.ray({500, 120}, o, d);
    const float t = -o.y / d.y;
    auto back = vp.project(o + d * t);
    expect(back && glm::distance(*back, glm::vec2(500, 120)) < 0.05f, "ray() and project() are inverses");
    expect(ray_triangle({0, -5, 0}, {0, 1, 0}, {-1, 0, -1}, {1, 0, -1}, {0, 0, 1}).has_value(), "ray hits a facing triangle");
    expect(!ray_triangle({5, -5, 0}, {0, 1, 0}, {-1, 0, -1}, {1, 0, -1}, {0, 0, 1}).has_value(), "and misses beside it");
    auto hit = ray_aabb({0, -5, 0}, {0, 1, 0}, glm::vec3(-1), glm::vec3(1));
    expect(hit && std::abs(*hit - 4.0f) < 1e-5f, "ray-box entry distance");
    const glm::vec3 e(10, 20, 30);
    expect(glm::distance(matrix_to_euler(euler_to_matrix(e)), e) < 1e-3f, "Euler <-> matrix round trip uses Transform's convention");
}

} // namespace toy::editor::testing
