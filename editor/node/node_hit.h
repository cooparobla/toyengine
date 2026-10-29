/**
 * @file node_hit.h
 * @brief What is under the pointer: a socket, a node, a wire, or nothing.
 *
 * `imm` keeps no hit-test tree, so the canvas resolves picks itself. The order is fixed and
 * matters: **socket, then node, then wire, then background.** A socket sits on its node's edge and
 * a wire ends at a socket, so testing the other way round would make sockets unclickable wherever
 * they overlap something larger.
 *
 * Everything takes graph-space coordinates and a `tolerance` already converted to graph units, so
 * a pick stays the same size in *screen* pixels at every zoom -- a socket that got harder to click
 * as you zoomed out would be the wrong behaviour.
 */

#ifndef TOYENGINE_EDITOR_NODE_NODE_HIT_H
#define TOYENGINE_EDITOR_NODE_NODE_HIT_H

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/nodegraph/graph.h>
#include "editor/node/node_layout.h"

namespace toy {
namespace editor {
namespace node {

/// @brief What a pick found. `None` means the background.
enum class HitKind : uint8_t { None, Node, Header, Socket, Link };

/**
 * @struct Hit
 * @brief One pick result.
 */
struct Hit {
    HitKind     kind = HitKind::None;
    ng::NodeId  node = 0;          ///< Node / Header / Socket.
    std::string socket;            ///< Socket only.
    bool        is_input = false;  ///< Socket only.
    int         link = -1;         ///< Link only: an index into `graph.links()`.

    explicit operator bool() const { return kind != HitKind::None; }
};

/// @brief Whether `p` is inside the node's body.
bool node_contains(const NodeLayout& layout, glm::vec2 p);

/**
 * @brief The topmost thing at `p`.
 *
 * `layouts` is searched back to front, so the last-drawn node wins an overlap -- which is the one
 * the user sees on top.
 *
 * @param socket_tolerance Graph units around a socket dot that still count as hitting it.
 * @param link_tolerance   Graph units from a wire's curve that still count as hitting it.
 */
Hit hit_test(const ng::Graph& graph, const std::vector<NodeLayout>& layouts,
             const LayoutMetrics& metrics, glm::vec2 p,
             float socket_tolerance, float link_tolerance);

/// @brief The socket at `p`, or an empty hit. Exposed so a wire drag can test drop targets alone.
Hit hit_socket(const std::vector<NodeLayout>& layouts, glm::vec2 p, float tolerance);

/// @brief The index into `graph.links()` of the wire at `p`, or -1.
int hit_link(const ng::Graph& graph, const std::vector<NodeLayout>& layouts, glm::vec2 p,
             float tolerance);

/// @brief Every node whose body overlaps the graph-space rectangle, for a rubber band.
std::vector<ng::NodeId> nodes_in_box(const std::vector<NodeLayout>& layouts,
                                     glm::vec2 a, glm::vec2 b);

/// @brief The two endpoints a link is drawn between, from the laid-out sockets. False if either is gone.
bool link_endpoints(const ng::Link& link, const std::vector<NodeLayout>& layouts,
                    glm::vec2& out_from, glm::vec2& out_to);

} // namespace node
} // namespace editor
} // namespace toy

#endif // TOYENGINE_EDITOR_NODE_NODE_HIT_H
