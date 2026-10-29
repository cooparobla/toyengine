/**
 * @file node_layout_test.cpp
 * @brief Node geometry: rows from the declaration, size from the labels, bounds for frame-all.
 *
 * This is the function both the renderer and the hit-tester call, so an error here shows up as
 * wires attaching a few pixels from where they were grabbed rather than as anything obvious.
 */

#include <coopa/testing/test.h>

#include "editor/support/fixtures.h"

#include <coopa/nodegraph/node_type.h>
#include <toyengine/procgraph/nodes.h>

#include "editor/node/node_layout.h"

COOPA_TEST_SUITE("node_layout");

namespace toy::editor::testing {
namespace {

using namespace toy::editor::node;

/// @brief procgraph's real node and socket types, so the fixtures are the ones a user meets.
void setup() { toy::procgraph::register_procgraph_nodes(); }

/// @brief A stand-in for imm's text measuring: 7 px per character at zoom 1.
float measure(const std::string& s) { return static_cast<float>(s.size()) * 7.0f; }

/// @brief Lays out one node of `type` at the origin.
NodeLayout layout_of(ng::Graph& g, const std::string& type, const ng::PropertyBag& props = {}) {
    const ng::NodeId id = g.add_node(type, props);
    const ng::NodeType* t = ng::NodeTypeRegistry::instance().find(type);
    coopa::test::require(t != nullptr, "node_layout: the fixture node type '" + type + "' exists");
    return layout_node(*g.node(id), t->declaration_for(g.node(id)->props), t->display(), measure);
}

} // namespace

/** @brief Every visible socket gets a row, inputs left and outputs right. */
COOPA_TEST(sockets_become_rows_on_the_side_their_direction_says) {
    setup();
    ng::Graph g;
    // Math has inputs a, b and output result.
    const NodeLayout l = layout_of(g, "Math");

    int ins = 0, outs = 0;
    for (const auto& s : l.sockets) (s.is_input ? ins : outs)++;
    expect(ins == 2 && outs == 1, "node_layout: Math lays out 2 inputs and 1 output, got " +
                                      std::to_string(ins) + " and " + std::to_string(outs));

    const SocketLayout* a = l.find_socket("a", /*input=*/true);
    const SocketLayout* r = l.find_socket("result", /*input=*/false);
    coopa::test::require(a != nullptr && r != nullptr, "node_layout: both sockets are findable by name");
    expect(a->pos.x == l.lo.x, "node_layout: an input sits on the left edge, where a wire ends");
    expect(r->pos.x == l.hi.x, "node_layout: and an output on the right edge");
    expect(a->pos.y > l.lo.y && a->pos.y < l.hi.y, "node_layout: rows are inside the body");

    expect(l.find_socket("a", /*input=*/false) == nullptr,
           "node_layout: looking for an input on the output side finds nothing -- the side is part "
           "of a socket's identity, because a node may have the same name on both");
}

/** @brief Height follows the busier side, not the total. */
COOPA_TEST(a_node_is_as_tall_as_its_busier_side) {
    setup();
    ng::Graph g;
    const LayoutMetrics m;

    const NodeLayout math = layout_of(g, "Math");          // 2 in, 1 out -> 2 rows
    const NodeLayout sep  = layout_of(g, "SeparateXYZ");   // 1 in, 3 out -> 3 rows

    const float two_rows   = m.header_h + m.pad_y * 2.0f + 2.0f * m.row_h;
    const float three_rows = m.header_h + m.pad_y * 2.0f + 3.0f * m.row_h;
    expect(std::fabs(math.size().y - two_rows) < 0.01f,
           "node_layout: 2 inputs against 1 output is two rows tall, not three -- inputs and "
           "outputs SHARE rows");
    expect(std::fabs(sep.size().y - three_rows) < 0.01f,
           "node_layout: and 1 input against 3 outputs is three");
}

/** @brief A dynamic declaration relayouts when the props that drive it change. */
COOPA_TEST(a_node_whose_sockets_follow_its_props_relayouts_when_they_change) {
    setup();
    ng::Graph g;

    ng::PropertyBag as_float;
    as_float.set("data_type", std::string("Float"));
    const NodeLayout f = layout_of(g, "Switch", as_float);

    ng::PropertyBag as_geometry;
    as_geometry.set("data_type", std::string("Geometry"));
    const NodeLayout gm = layout_of(g, "Switch", as_geometry);

    const SocketLayout* ft = f.find_socket("true", /*input=*/true);
    const SocketLayout* gt = gm.find_socket("true", /*input=*/true);
    coopa::test::require(ft != nullptr && gt != nullptr, "node_layout: both have the socket");
    expect(ft->type != gt->type,
           "node_layout: the SAME node type lays out a different socket type for different props. "
           "Caching a layout per node TYPE rather than per node would get this wrong");
}

/** @brief Width grows with the labels and stays within its bounds. */
COOPA_TEST(width_fits_the_labels_between_the_declared_bounds) {
    setup();
    ng::Graph g;
    const LayoutMetrics m;

    const NodeLayout l = layout_of(g, "Math");
    expect(l.size().x >= m.min_width, "node_layout: a node is never narrower than the minimum");
    expect(l.size().x <= m.max_width, "node_layout: nor wider than the maximum");

    // With no measurer at all the width must still be sane rather than zero.
    const ng::NodeId id = g.add_node("Math");
    const ng::NodeType* t = ng::NodeTypeRegistry::instance().find("Math");
    const NodeLayout bare = layout_node(*g.node(id), t->declaration_for({}), "Math", nullptr);
    expect(bare.size().x >= m.min_width,
           "node_layout: with no text measurer the node falls back to the minimum width rather "
           "than collapsing -- a headless test has no font");
}

/** @brief The node is placed where its ui_position says, which is what the file stores. */
COOPA_TEST(a_node_sits_at_its_authored_position) {
    setup();
    ng::Graph g;
    const ng::NodeId id = g.add_node("Math");
    g.node(id)->ui_position = {-240.0f, 310.0f};

    const ng::NodeType* t = ng::NodeTypeRegistry::instance().find("Math");
    const NodeLayout l = layout_node(*g.node(id), t->declaration_for({}), "Math", measure);
    expect(l.lo == glm::vec2(-240.0f, 310.0f),
           "node_layout: the body's top-left IS ui_position, so dragging a node and saving writes "
           "back the same number the canvas drew at");
}

/** @brief An unregistered type still lays out, so an unfamiliar graph is not an empty one. */
COOPA_TEST(an_unknown_node_type_lays_out_as_a_bare_header) {
    setup();
    ng::Graph g;
    const ng::NodeId id = g.add_node("ANodeTypeFromTheFuture");
    g.node(id)->ui_position = {10.0f, 20.0f};

    const std::vector<NodeLayout> all = layout_graph(g, measure);
    coopa::test::require(all.size() == 1, "node_layout: the unknown node was not dropped");
    expect(all[0].sockets.empty(), "node_layout: it has no sockets, because nothing declared any");
    expect(all[0].size().y > 0.0f && all[0].size().x > 0.0f,
           "node_layout: but it still has a body -- a graph written against a newer build should "
           "look unfamiliar, not empty");
    expect(all[0].lo == glm::vec2(10.0f, 20.0f), "node_layout: and it is movable, at its position");
}

/**
 * @brief GroupInput and GroupOutput get sockets from the graph's own interface.
 *
 * They are structural, not registered node types, so nothing in the registry can describe them.
 * Without synthesizing their sockets they lay out as bare headers with nothing to connect to --
 * a canvas in which the graph's own inputs and outputs are unwirable, which is a graph you
 * cannot finish. `Graph::validate()` does the same thing for the same reason.
 */
COOPA_TEST(the_group_nodes_take_their_sockets_from_the_graph_interface) {
    setup();
    ng::Graph g;

    ng::InterfaceSocket in;
    in.identifier = "length";
    in.type       = toy::procgraph::types().float_t;
    g.inputs().push_back(in);

    ng::InterfaceSocket out;
    out.identifier = "geometry";
    out.type       = toy::procgraph::types().geometry_t;
    g.outputs().push_back(out);

    const ng::NodeId gi = g.add_node(ng::k_group_input_type);
    const ng::NodeId go = g.add_node(ng::k_group_output_type);
    g.node(go)->ui_position = {400.0f, 0.0f};

    const std::vector<NodeLayout> all = layout_graph(g, measure);
    coopa::test::require(all.size() == 2, "node_layout: both group nodes were laid out");

    const NodeLayout* input_node = nullptr;
    const NodeLayout* output_node = nullptr;
    for (const auto& l : all) {
        if (l.id == gi) input_node = &l;
        if (l.id == go) output_node = &l;
    }
    coopa::test::require(input_node != nullptr && output_node != nullptr, "node_layout: both found");

    const SocketLayout* produced = input_node->find_socket("length", /*input=*/false);
    coopa::test::require(produced != nullptr,
           "node_layout: GroupInput OUTPUTS the graph's inputs -- that is the direction that makes "
           "a graph parameter usable inside the graph");
    expect(produced->type == toy::procgraph::types().float_t,
           "node_layout: carrying the interface socket's declared type");

    const SocketLayout* consumed = output_node->find_socket("geometry", /*input=*/true);
    coopa::test::require(consumed != nullptr,
           "node_layout: and GroupOutput CONSUMES the graph's outputs, so there is somewhere to "
           "wire the result");
    expect(consumed->type == toy::procgraph::types().geometry_t,
           "node_layout: with the right type, so can_connect() can judge a wire into it");

    expect(input_node->find_socket("length", /*input=*/true) == nullptr,
           "node_layout: and not on the wrong side of either node");
}

/** @brief Bounds cover every node, and report emptiness rather than returning garbage. */
COOPA_TEST(graph_bounds_cover_every_node_and_report_an_empty_graph) {
    setup();
    ng::Graph g;
    glm::vec2 lo{0.0f}, hi{0.0f};
    expect(!graph_bounds(layout_graph(g, measure), lo, hi),
           "node_layout: an empty graph has no bounds, and says so rather than returning a box at "
           "the origin that frame-all would then fly to");

    g.node(g.add_node("Math"))->ui_position = {-100.0f, -50.0f};
    g.node(g.add_node("Math"))->ui_position = {400.0f, 300.0f};

    coopa::test::require(graph_bounds(layout_graph(g, measure), lo, hi), "node_layout: two nodes have bounds");
    expect(lo.x <= -100.0f && lo.y <= -50.0f, "node_layout: the box reaches the top-left node");
    expect(hi.x >= 400.0f && hi.y >= 300.0f,
           "node_layout: and past the bottom-right one, because bounds include its body, not just "
           "its corner");
}

}  // namespace toy::editor::testing
