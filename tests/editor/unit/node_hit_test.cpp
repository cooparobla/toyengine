/**
 * @file node_hit_test.cpp
 * @brief Picking on the node canvas, and the priority order it resolves in.
 *
 * `imm` keeps no hit-test tree, so this is the whole of "what did I just click".
 */

#include <coopa/testing/test.h>

#include "editor/support/fixtures.h"

#include <coopa/nodegraph/node_type.h>
#include <toyengine/procgraph/nodes.h>

#include "editor/node/node_hit.h"
#include "editor/node/node_wire.h"

COOPA_TEST_SUITE("node_hit");

namespace toy::editor::testing {
namespace {

using namespace toy::editor::node;

void setup() { toy::procgraph::register_procgraph_nodes(); }
float measure(const std::string& s) { return static_cast<float>(s.size()) * 7.0f; }

/// @brief Two Math nodes well apart, the first's `result` feeding the second's `a`.
struct Fixture {
    ng::Graph               graph;
    ng::NodeId              a = 0;
    ng::NodeId              b = 0;
    std::vector<NodeLayout> layouts;
    LayoutMetrics           metrics;

    Fixture() {
        setup();
        a = graph.add_node("Math");
        b = graph.add_node("Math");
        graph.node(a)->ui_position = {0.0f, 0.0f};
        graph.node(b)->ui_position = {400.0f, 120.0f};
        graph.add_link(ng::SocketRef{a, "result"}, ng::SocketRef{b, "a"});
        layouts = layout_graph(graph, measure);
    }

    const NodeLayout& layout(ng::NodeId id) const {
        for (const auto& l : layouts) {
            if (l.id == id) return l;
        }
        coopa::test::require(false, "node_hit: the fixture node was laid out");
        return layouts.front();
    }

    Hit pick(glm::vec2 p) const { return hit_test(graph, layouts, metrics, p, 8.0f, 6.0f); }
};

} // namespace

/** @brief Clicking a body, a header and empty space each report what they are. */
COOPA_TEST(a_body_a_header_and_the_background_are_distinguished) {
    Fixture f;
    const NodeLayout& la = f.layout(f.a);

    const Hit header = f.pick({la.lo.x + la.size().x * 0.5f, la.lo.y + f.metrics.header_h * 0.5f});
    expect(header.kind == HitKind::Header && header.node == f.a,
           "node_hit: the title bar reports Header -- dragging a node by its title is the usual "
           "gesture, and the body is where per-socket widgets will go");

    const Hit body = f.pick({la.lo.x + la.size().x * 0.5f, la.hi.y - 2.0f});
    expect(body.kind == HitKind::Node && body.node == f.a, "node_hit: below the header is the body");

    const Hit nothing = f.pick({-500.0f, -500.0f});
    expect(!nothing && nothing.kind == HitKind::None,
           "node_hit: empty space is a miss, not the nearest node");
}

/** @brief A socket wins over the node it sits on. */
COOPA_TEST(a_socket_beats_the_node_underneath_it) {
    Fixture f;
    const NodeLayout& la = f.layout(f.a);
    const SocketLayout* out = la.find_socket("result", /*input=*/false);
    coopa::test::require(out != nullptr, "node_hit: the output socket exists");

    const Hit on_socket = f.pick(out->pos);
    expect(on_socket.kind == HitKind::Socket && on_socket.node == f.a && on_socket.socket == "result",
           "node_hit: a socket sits ON its node's edge, so testing the node first would make every "
           "socket unclickable");
    expect(!on_socket.is_input, "node_hit: and it is reported as an output");

    // Just inside the tolerance, and just outside it.
    expect(f.pick(out->pos + glm::vec2(5.0f, 0.0f)).kind == HitKind::Socket,
           "node_hit: near enough still counts -- a 9 px dot needs a forgiving radius");
    const Hit far = f.pick(out->pos + glm::vec2(40.0f, 0.0f));
    expect(far.kind != HitKind::Socket, "node_hit: far enough does not");
}

/** @brief A wire is pickable along its whole curve, not only near its ends. */
COOPA_TEST(a_wire_is_pickable_along_its_curve) {
    Fixture f;
    glm::vec2 from{}, to{};
    coopa::test::require(link_endpoints(f.graph.links()[0], f.layouts, from, to),
                         "node_hit: the fixture link has endpoints");

    // Sample the curve itself; the midpoint of a bezier is nowhere near the chord's midpoint.
    for (float t : {0.25f, 0.5f, 0.75f}) {
        const glm::vec2 on_curve = wire_point_at(from, to, t);
        const int idx = hit_link(f.graph, f.layouts, on_curve, 6.0f);
        expect(idx == 0, "node_hit: the wire is hit at t=" + std::to_string(t) +
                             " -- picking against the chord instead of the curve would miss here");
    }

    // Sampled at a quarter, NOT the midpoint: with symmetric horizontal tangents the two control
    // offsets cancel exactly at t = 0.5, so a bezier wire's midpoint IS its chord's midpoint. A
    // deviation check there would always read zero and quietly prove nothing.
    const glm::vec2 chord_q = from + (to - from) * 0.25f;
    const glm::vec2 curve_q = wire_point_at(from, to, 0.25f);
    expect(glm::distance(chord_q, curve_q) > 1.0f,
           "node_hit: and the test means something, because the curve really does leave the chord "
           "away from its ends and its midpoint");
    expect(glm::distance((from + to) * 0.5f, wire_point_at(from, to, 0.5f)) < 1e-3f,
           "node_hit: while at the midpoint it provably does not -- worth pinning, because it is "
           "what makes a straight-looking wire still pick correctly there");

    expect(hit_link(f.graph, f.layouts, {from.x, from.y - 300.0f}, 6.0f) < 0,
           "node_hit: well off the wire is a miss");
}

/** @brief A link naming a socket that no longer exists is simply not drawn or picked. */
COOPA_TEST(a_link_to_a_vanished_socket_is_skipped_rather_than_crashing) {
    Fixture f;
    // A node's sockets follow its props, so changing one can orphan a link until it is rewired.
    f.graph.links()[0].to.socket = "a_socket_that_went_away";

    glm::vec2 from{}, to{};
    expect(!link_endpoints(f.graph.links()[0], f.layouts, from, to),
           "node_hit: an orphaned link has no endpoints to draw between");
    expect(hit_link(f.graph, f.layouts, {200.0f, 60.0f}, 6.0f) < 0,
           "node_hit: and picking anywhere near where it used to run finds nothing, rather than "
           "dereferencing a socket that is gone");
}

/** @brief Rubber band takes partial overlap, like every other box-select in the editor. */
COOPA_TEST(a_band_selects_on_overlap_not_containment) {
    Fixture f;
    const NodeLayout& la = f.layout(f.a);

    const std::vector<ng::NodeId> all = nodes_in_box(f.layouts, {-50.0f, -50.0f}, {600.0f, 400.0f});
    expect(all.size() == 2, "node_hit: a band over both nodes selects both");

    const std::vector<ng::NodeId> clipped =
        nodes_in_box(f.layouts, la.lo - glm::vec2(10.0f), la.lo + glm::vec2(5.0f));
    expect(clipped.size() == 1 && clipped[0] == f.a,
           "node_hit: a band clipping only a node's CORNER still selects it -- containment would "
           "mean you had to enclose a node entirely, which no other tool here requires");

    expect(nodes_in_box(f.layouts, {-500.0f, -500.0f}, {-400.0f, -400.0f}).empty(),
           "node_hit: a band over nothing selects nothing");
    expect(nodes_in_box(f.layouts, la.lo, la.lo).size() == 1,
           "node_hit: a zero-size band on a node still counts, so a click-drag of one pixel is "
           "not a surprise miss");
}

/** @brief The topmost node wins an overlap. */
COOPA_TEST(the_last_drawn_node_wins_an_overlap) {
    setup();
    ng::Graph g;
    const ng::NodeId under = g.add_node("Math");
    const ng::NodeId over  = g.add_node("Math");
    g.node(under)->ui_position = {0.0f, 0.0f};
    g.node(over)->ui_position  = {10.0f, 10.0f};   // Deliberately on top of the first.

    const std::vector<NodeLayout> layouts = layout_graph(g, measure);
    const LayoutMetrics m;
    const Hit h = hit_test(g, layouts, m, {40.0f, 40.0f}, 8.0f, 6.0f);
    expect(h.node == over,
           "node_hit: where two nodes overlap the one drawn LAST is picked, which is the one the "
           "user can see");
}

/** @brief An empty graph picks nothing and does not reach into an empty list. */
COOPA_TEST(an_empty_graph_picks_nothing) {
    setup();
    ng::Graph g;
    const std::vector<NodeLayout> layouts = layout_graph(g, measure);
    const LayoutMetrics m;
    expect(!hit_test(g, layouts, m, {0.0f, 0.0f}, 8.0f, 6.0f), "node_hit: nothing to hit");
    expect(hit_link(g, layouts, {0.0f, 0.0f}, 6.0f) < 0, "node_hit: and no wires either");
    expect(nodes_in_box(layouts, {-10.0f, -10.0f}, {10.0f, 10.0f}).empty(), "node_hit: nor to band");
}

}  // namespace toy::editor::testing
