/**
 * @file primitives_test.cpp
 * @brief The Add menu's primitives and the material viewer's shader ball are closed, consistently
 * wound and face outward -- what every mesh operation downstream assumes.
 */

#include <coopa/testing/test.h>

#include "editor/mesh/primitives.h"
#include "editor/mesh/shader_ball.h"
#include "editor/support/mesh_checks.h"

COOPA_TEST_SUITE("primitives");

namespace toy::editor::testing {

COOPA_TEST(every_primitive_is_closed_consistent_and_outward) {
    for (const auto& name : primitive_names()) {
        const EditMesh m = make_primitive(name);
        expect(!m.faces.empty(), name + " has faces");
        if (name == "Plane" || name == "Grid") {
            expect(euler(m) == 1, name + " is a disc (V-E+F = 1)");
            continue;
        }
        expect(euler(m) == 2, name + " has Euler characteristic 2 (got " + std::to_string(euler(m)) + ")");
        expect(closed_and_consistent(m), name + " is closed with consistent winding");
        // Outward normals: every face normal points away from the centroid.
        glm::vec3 lo, hi;
        m.bounds(lo, hi);
        const glm::vec3 c = (lo + hi) * 0.5f;
        bool outward = true;
        for (size_t f = 0; f < m.faces.size(); ++f) outward &= glm::dot(m.face_normal(f), m.face_center(f) - c) > 0.0f;
        expect(outward, name + " faces point outward");
    }
}

/** @brief The material viewer's shader ball: closed, consistently wound parts, facing out. */
COOPA_TEST(the_shader_ball_is_closed_and_faces_outward) {
    const EditMesh m = make_shader_ball();
    expect(closed_and_consistent(m), "every part of the shader ball is closed with consistent winding");
    // Signed volume (divergence theorem over the triangulated faces): positive = facing out.
    double vol = 0.0;
    for (const auto& f : m.faces) {
        for (size_t i = 1; i + 1 < f.corners.size(); ++i) {
            const glm::dvec3 a = m.positions[f.corners[0].v], b = m.positions[f.corners[i].v], c = m.positions[f.corners[i + 1].v];
            vol += glm::dot(a, glm::cross(b, c)) / 6.0;
        }
    }
    // A lower bound from the parts' radii: a 7/8 shell (0.5 outer, 0.43 inner) plus a 0.34 core.
    // One inside-out part subtracts its volume and drops the total below it.
    const double pi = 3.14159265358979;
    const double shell = 4.0 / 3.0 * pi * (0.125 - 0.43 * 0.43 * 0.43) * 7.0 / 8.0, core = 4.0 / 3.0 * pi * 0.34 * 0.34 * 0.34;
    expect(vol > shell + core, "the shader ball faces outward (volume " + std::to_string(vol) + ")");
}

} // namespace toy::editor::testing
