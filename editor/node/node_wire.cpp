/**
 * @file node_wire.cpp
 * @brief Bezier flattening for link wires.
 */

#include "editor/node/node_wire.h"

#include <algorithm>
#include <cmath>

namespace toy {
namespace editor {
namespace node {
namespace {

void control_points_(glm::vec2 from, glm::vec2 to, glm::vec2& c0, glm::vec2& c1) {
    const float d = wire_tangent(from, to);
    c0 = from + glm::vec2(d, 0.0f);
    c1 = to - glm::vec2(d, 0.0f);
}

glm::vec2 cubic_(glm::vec2 p0, glm::vec2 c0, glm::vec2 c1, glm::vec2 p1, float t) {
    const float u = 1.0f - t;
    return u * u * u * p0 + 3.0f * u * u * t * c0 + 3.0f * u * t * t * c1 + t * t * t * p1;
}

} // namespace

float wire_tangent(glm::vec2 from, glm::vec2 to) {
    // Scaled by the horizontal gap, so near-vertical wires do not bulge sideways. The floor keeps
    // a wire between two stacked sockets from collapsing to a straight line; the ceiling stops a
    // long wire from swinging most of the way across the graph.
    const float dx = std::abs(to.x - from.x);
    return std::clamp(dx * 0.5f, 30.0f, 200.0f);
}

std::vector<glm::vec2> wire_points(glm::vec2 from, glm::vec2 to, int segments) {
    glm::vec2 c0, c1;
    control_points_(from, to, c0, c1);

    if (segments <= 0) {
        // The chord plus the control reach approximates the arc length well enough to pick a
        // segment count from, and costs no square roots beyond the one.
        const float reach = glm::length(to - from) + wire_tangent(from, to);
        segments = static_cast<int>(reach / 12.0f);
    }
    segments = std::clamp(segments, 4, 48);

    std::vector<glm::vec2> out;
    out.reserve(static_cast<size_t>(segments) + 1);
    for (int i = 0; i <= segments; ++i) {
        out.push_back(cubic_(from, c0, c1, to, static_cast<float>(i) / static_cast<float>(segments)));
    }
    return out;
}

glm::vec2 wire_point_at(glm::vec2 from, glm::vec2 to, float t) {
    glm::vec2 c0, c1;
    control_points_(from, to, c0, c1);
    return cubic_(from, c0, c1, to, std::clamp(t, 0.0f, 1.0f));
}

} // namespace node
} // namespace editor
} // namespace toy
