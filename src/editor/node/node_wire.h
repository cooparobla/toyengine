/**
 * @file node_wire.h
 * @brief The curve a link is drawn as, flattened to points.
 *
 * A node wire is a cubic bezier with **horizontal tangents**: it leaves an output going right and
 * enters an input going left, whatever their relative positions. That is what makes a backwards
 * link (an input to the left of its output) read as a loop rather than a straight diagonal
 * through the node that produced it.
 *
 * Returned as points rather than drawn, so hit-testing walks exactly the geometry that was drawn.
 */

#ifndef TOYENGINE_EDITOR_NODE_NODE_WIRE_H
#define TOYENGINE_EDITOR_NODE_NODE_WIRE_H

#include <vector>

#include <glm/glm.hpp>

namespace toy {
namespace editor {
namespace node {

/// @brief How far the control points reach, given the endpoints' horizontal separation.
float wire_tangent(glm::vec2 from, glm::vec2 to);

/**
 * @brief The wire from an output at `from` to an input at `to`, as `segments + 1` points.
 *
 * @param segments 0 auto-picks from the endpoints' separation -- a short wire does not need 32
 *        segments and a long one looks faceted with 8. Clamped to [4, 48].
 */
std::vector<glm::vec2> wire_points(glm::vec2 from, glm::vec2 to, int segments = 0);

/// @brief The point at `t` along the same curve, without building the whole list.
glm::vec2 wire_point_at(glm::vec2 from, glm::vec2 to, float t);

} // namespace node
} // namespace editor
} // namespace toy

#endif // TOYENGINE_EDITOR_NODE_NODE_WIRE_H
