/**
 * @file node_layout.h
 * @brief Where a node's body and each of its sockets sit, in graph space.
 *
 * **One function decides this, and both drawing and hit-testing call it.** Computing a socket's
 * position twice -- once to draw the dot, once to test the click -- is how a node editor ends up
 * with wires that attach a few pixels from where you grabbed them, and the drift is invisible
 * until someone complains. So `layout_node()` is the only place that knows the geometry.
 *
 * Everything here is in **graph units**, not pixels: a node is the same size in the document at
 * every zoom, and `NodeView` converts once at draw time. The one exception is the text measuring
 * used for the width, which needs a pixel-space measurer -- passed in as a callback so this stays
 * free of `imm` and unit-testable without a font.
 */

#ifndef TOYENGINE_EDITOR_NODE_NODE_LAYOUT_H
#define TOYENGINE_EDITOR_NODE_NODE_LAYOUT_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/nodegraph/declaration.h>
#include <coopa/nodegraph/graph.h>

namespace toy {
namespace editor {
namespace node {

namespace ng = coopa::nodegraph;

/**
 * @struct LayoutMetrics
 * @brief The constants a node's shape is built from, in graph units.
 */
struct LayoutMetrics {
    float header_h   = 22.0f;   ///< Title bar.
    float row_h      = 18.0f;   ///< One socket row.
    float pad_y      = 6.0f;    ///< Above the first row and below the last.
    float min_width  = 120.0f;
    float max_width  = 360.0f;
    float label_pad  = 28.0f;   ///< Space for the socket dot plus a gap, per side.
    float socket_r   = 4.5f;    ///< Socket dot radius.
};

/**
 * @struct SocketLayout
 * @brief One socket's place on a node.
 */
struct SocketLayout {
    std::string identifier;   ///< The stable key links use.
    bool        is_input = false;
    ng::TypeId  type = ng::k_invalid_type;
    bool        multi_input = false;
    glm::vec2   pos{0.0f};    ///< Graph space: the centre of the dot, on the node's edge.
    float       row_y = 0.0f; ///< Graph space: the row's vertical centre, for the label.
};

/**
 * @struct NodeLayout
 * @brief A node's full geometry.
 *
 * `lo`/`hi` are the body's graph-space corners. Inputs sit on the left edge, outputs on the right,
 * interleaved by row so a node with 2 inputs and 3 outputs is 3 rows tall -- which is what makes
 * a node's height a function of `max(inputs, outputs)` rather than their sum.
 */
struct NodeLayout {
    ng::NodeId                id = 0;
    glm::vec2                 lo{0.0f};
    glm::vec2                 hi{0.0f};
    std::vector<SocketLayout> sockets;

    glm::vec2 size() const { return hi - lo; }
    /// @brief The title bar's box.
    glm::vec2 header_lo() const { return lo; }
    glm::vec2 header_hi(const LayoutMetrics& m) const { return {hi.x, lo.y + m.header_h}; }

    /// @brief The socket named `id` on the given side, or nullptr.
    const SocketLayout* find_socket(const std::string& socket_id, bool input) const {
        for (const auto& s : sockets) {
            if (s.is_input == input && s.identifier == socket_id) return &s;
        }
        return nullptr;
    }
};

/**
 * @brief The declaration a GroupInput / GroupOutput node carries, built from the graph interface.
 *
 * Forwards to `ng::Graph::interface_declaration()`, which is also what `validate()` and
 * `compile()` consult -- so a socket the canvas draws is one the library agrees exists.
 * `as_outputs` is true for GroupInput: the graph's INPUTS are what it hands out.
 */
inline ng::NodeDeclaration interface_declaration(const ng::Graph& graph, bool as_outputs) {
    return graph.interface_declaration(as_outputs);
}

/// @brief Measures a label's width in graph units at zoom 1. `imm::Context::text_width` in the panel.
using TextMeasure = std::function<float(const std::string&)>;

/**
 * @brief Lays out one node from its declaration.
 *
 * @param node    Supplies `ui_position` and the id.
 * @param decl    The node's sockets, from `NodeType::declaration_for(node.props)`. Sockets flagged
 *                `Hide` take no row -- they are authored-only and have nothing to click.
 * @param title   The display string for the header, for width measurement.
 * @param measure Label widths. May be null, in which case `min_width` is used.
 */
NodeLayout layout_node(const ng::Node& node, const ng::NodeDeclaration& decl,
                       const std::string& title, const TextMeasure& measure,
                       const LayoutMetrics& m = {});

/**
 * @brief Lays out every node in `graph`, resolving each type through the registry.
 *
 * A node whose type is not registered still gets a layout -- a header-only box -- so an unknown
 * node is visible and movable rather than invisible. An editor that dropped it would make a graph
 * authored against a newer build look empty.
 */
std::vector<NodeLayout> layout_graph(const ng::Graph& graph, const TextMeasure& measure,
                                     const LayoutMetrics& m = {});

/// @brief The graph-space bounds of every node, for frame-all. False when the graph is empty.
bool graph_bounds(const std::vector<NodeLayout>& layouts, glm::vec2& out_lo, glm::vec2& out_hi);

} // namespace node
} // namespace editor
} // namespace toy

#endif // TOYENGINE_EDITOR_NODE_NODE_LAYOUT_H
