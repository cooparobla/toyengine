/**
 * @file proportional_test.cpp
 * @brief Proportional editing weights: every falloff is 1 at the selection and 0 at the radius and
 * never rises, and 3D, connected and projected distances measure what they claim.
 */

#include <coopa/testing/test.h>

#include <optional>
#include <set>

#include "editor/mesh/proportional.h"
#include "editor/support/fixtures.h"

COOPA_TEST_SUITE("proportional");

namespace toy::editor::testing {

/** @brief Proportional editing weights: falloff curves and the three ways of measuring distance. */
COOPA_TEST(falloffs_and_the_three_distance_modes) {
    for (int i = 0; i < kFalloffCount; ++i) {
        const Falloff f = static_cast<Falloff>(i);
        expect(std::abs(falloff_weight(f, 0.0f) - 1.0f) < 1e-5f && falloff_weight(f, 1.0f) == 0.0f && falloff_weight(f, 2.0f) == 0.0f,
               std::string("falloff ") + falloff_name(f) + ": 1 at the selection, 0 at and past the radius");
        for (float t = 0.0f; t < 0.95f; t += 0.05f) {
            expect(falloff_weight(f, t) >= falloff_weight(f, t + 0.05f) - 1e-5f, std::string("falloff ") + falloff_name(f) + " never rises");
        }
    }
    expect(std::abs(falloff_weight(Falloff::Linear, 0.25f) - 0.75f) < 1e-5f, "linear is 1 - t");

    // A row of vertices 0..9 along X, 1 m apart (joined by edges); plus a separate pair at y = 5.
    EditMesh m;
    auto tri = [&](uint32_t a, uint32_t b) {
        Face f;
        for (uint32_t v : {a, b, a}) { Corner c; c.v = v; f.corners.push_back(c); }
        m.faces.push_back(f);
    };
    for (int i = 0; i < 10; ++i) m.positions.push_back({float(i), 0.0f, 0.0f});
    for (uint32_t i = 0; i + 1 < 10; ++i) tri(i, i + 1);
    m.positions.push_back({2.0f, 5.0f, 0.0f});
    m.positions.push_back({3.0f, 5.0f, 0.0f});
    tri(10, 11);
    ProportionalSettings ps;
    ps.enabled = true;
    ps.falloff = Falloff::Linear;
    ps.radius = 4.0f;
    ps.projected = false;
    auto weight_of = [](const std::vector<std::pair<uint32_t, float>>& w, uint32_t v) {
        for (const auto& [u, x] : w) if (u == v) return x;
        return 0.0f;
    };
    const std::set<uint32_t> sel{0};
    auto w = proportional_weights(m, sel, ps, glm::mat4(1.0f));
    expect(weight_of(w, 0) == 0.0f, "the selection itself is not in the weighted list (it moves fully)");
    expect(std::abs(weight_of(w, 1) - 0.75f) < 1e-4f && std::abs(weight_of(w, 2) - 0.5f) < 1e-4f && weight_of(w, 4) == 0.0f,
           "3D: weights fall off linearly to 0 at the radius");
    expect(weight_of(w, 10) == 0.0f, "3D: (2, 5) is 5.4 m away -- outside");
    ps.radius = 6.0f;
    w = proportional_weights(m, sel, ps, glm::mat4(1.0f));
    expect(weight_of(w, 10) > 0.0f, "3D: a bigger radius reaches the separate pair");
    ps.connected = true;
    w = proportional_weights(m, sel, ps, glm::mat4(1.0f));
    expect(weight_of(w, 10) == 0.0f && std::abs(weight_of(w, 3) - 0.5f) < 1e-4f, "connected: the separate pair stays; edge distance along the row");
    ps.connected = false;
    ps.projected = true;
    // Looking along +Y at the row: screen x = world x * 100 px; y is depth.
    auto project = [](const glm::vec3& p) { return std::optional<glm::vec2>(glm::vec2(p.x * 100.0f, -p.z * 100.0f)); };
    w = proportional_weights(m, sel, ps, glm::mat4(1.0f), project, 250.0f);
    expect(std::abs(weight_of(w, 2) - 0.2f) < 1e-4f && weight_of(w, 3) == 0.0f, "projected: pixel distance against the pixel radius");
    expect(std::abs(weight_of(w, 10) - 0.2f) < 1e-4f, "projected: depth doesn't count -- the pair 5 m behind is at x = 200 px");
}

} // namespace toy::editor::testing
