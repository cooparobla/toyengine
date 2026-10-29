/**
 * @file node_edit_test.cpp
 * @brief The graph mutations the canvas makes, and the connection rules that gate them.
 *
 * The claim worth testing hardest: a wire the canvas accepts can never fail `Graph::validate()`,
 * because both ask `TypeRegistry::can_convert()`.
 */

#include <coopa/testing/test.h>

#include "editor/support/fixtures.h"

#include <coopa/nodegraph/diagnostics.h>
#include <coopa/nodegraph/node_type.h>
#include <toyengine/procgraph/nodes.h>
#include <toyengine/procgraph/proc_types.h>

#include "editor/node/node_edit.h"

COOPA_TEST_SUITE("node_edit");

namespace toy::editor::testing {
namespace {

using namespace toy::editor::node;

void setup() { toy::procgraph::register_procgraph_nodes(); }

ng::NodeId add(ng::Graph& g, const std::string& type, glm::vec2 at = {}) {
    const ng::NodeId id = g.add_node(type);
    if (ng::Node* n = g.node(id)) n->ui_position = at;
    return id;
}

} // namespace

/** @brief A legal connection is made, and the graph then validates. */
COOPA_TEST(connecting_compatible_sockets_makes_a_link_that_validates) {
    setup();
    ng::Graph g;
    const ng::NodeId f = add(g, "Float");
    const ng::NodeId m = add(g, "Math", {300.0f, 0.0f});

    const NodeGraphEdit e = connect(g, {f, "value"}, {m, "a"});
    expect(e.changed && e.finished, "node_edit: the connection was made and closed its undo step");
    expect(g.links().size() == 1, "node_edit: exactly one link");

    ng::Diagnostics diag;
    const bool ok = g.validate(diag);
    expect(ok, "node_edit: and the graph validates. A canvas that accepted a wire validate() then "
               "rejected would be unusable: " + diag.to_string());
}

/** @brief A registered conversion makes a cross-type wire legal. */
COOPA_TEST(a_convertible_pair_connects_and_an_unconvertible_one_does_not) {
    setup();
    ng::Graph g;
    const ng::NodeId f   = add(g, "Float");
    const ng::NodeId cube = add(g, "Cube", {300.0f, 0.0f});
    const ng::NodeId sep = add(g, "SeparateXYZ", {600.0f, 0.0f});

    // Float -> Vector is registered, so Float.value may feed Cube.size.
    expect(can_connect(g, {f, "value"}, {cube, "size"}) == ConnectVerdict::Ok,
           "node_edit: Float feeds a Vector socket, because the conversion is registered");

    // Geometry -> Vector is not.
    expect(can_connect(g, {cube, "geometry"}, {sep, "vector"}) == ConnectVerdict::TypeMismatch,
           "node_edit: Geometry does not feed a Vector, and the verdict says why");
    expect(connect(g, {cube, "geometry"}, {sep, "vector"}).changed == false,
           "node_edit: and the refused connection makes no link");
    expect(g.links().empty(), "node_edit: the graph is untouched by a refusal");
}

/** @brief Every way a connection can be wrong gets its own verdict. */
COOPA_TEST(each_illegal_connection_reports_what_was_wrong) {
    setup();
    ng::Graph g;
    const ng::NodeId a = add(g, "Math");
    const ng::NodeId b = add(g, "Math", {300.0f, 0.0f});

    expect(can_connect(g, {a, "result"}, {a, "a"}) == ConnectVerdict::SameNode,
           "node_edit: a node cannot feed itself");
    expect(can_connect(g, {a, "result"}, {b, "result"}) == ConnectVerdict::NotOppositeEnds,
           "node_edit: output to output is refused as a DIRECTION problem, not a missing socket -- "
           "the message an author needs is different");
    expect(can_connect(g, {a, "no_such"}, {b, "a"}) == ConnectVerdict::MissingSocket,
           "node_edit: a socket that is not there says so");

    coopa::test::require(connect(g, {a, "result"}, {b, "a"}).changed, "node_edit: a -> b connects");
    expect(can_connect(g, {b, "result"}, {a, "a"}) == ConnectVerdict::WouldCycle,
           "node_edit: closing the loop back is refused. Graph::add_link does not check this at "
           "all, so the canvas would happily build a graph that then fails to compile");

    for (ConnectVerdict v : {ConnectVerdict::SameNode, ConnectVerdict::NotOppositeEnds,
                             ConnectVerdict::MissingSocket, ConnectVerdict::TypeMismatch,
                             ConnectVerdict::WouldCycle}) {
        expect(std::string(connect_verdict_text(v)).size() > 0,
               "node_edit: every verdict has text for the drag tooltip");
    }
}

/** @brief A plain input replaces; a multi-input appends in order. */
COOPA_TEST(a_plain_input_replaces_and_a_multi_input_appends) {
    setup();
    ng::Graph g;
    const ng::NodeId f1 = add(g, "Float");
    const ng::NodeId f2 = add(g, "Float", {0.0f, 100.0f});
    const ng::NodeId m  = add(g, "Math", {300.0f, 0.0f});

    connect(g, {f1, "value"}, {m, "a"});
    connect(g, {f2, "value"}, {m, "a"});
    expect(g.links_into({m, "a"}).size() == 1,
           "node_edit: a second wire onto a plain input REPLACES the first, which is what dropping "
           "onto an occupied socket should do");
    expect(g.links_into({m, "a"}).front()->from.node == f2, "node_edit: the newer one wins");

    // JoinGeometry's `geometry` is a multi-input.
    const ng::NodeId c1 = add(g, "Cube", {0.0f, 300.0f});
    const ng::NodeId c2 = add(g, "Cube", {0.0f, 400.0f});
    const ng::NodeId j  = add(g, "JoinGeometry", {300.0f, 350.0f});
    connect(g, {c1, "geometry"}, {j, "geometry"});
    connect(g, {c2, "geometry"}, {j, "geometry"});

    const auto into = g.links_into({j, "geometry"});
    expect(into.size() == 2,
           "node_edit: a multi-input accumulates. connect() reads multi_input() off the "
           "declaration, so no call site has to remember which add_link overload to use");
    expect(into[0]->order < into[1]->order, "node_edit: and the order is assigned increasing");
}

/** @brief Deleting takes the node's links with it. */
COOPA_TEST(deleting_a_node_removes_its_links_too) {
    setup();
    ng::Graph g;
    const ng::NodeId f = add(g, "Float");
    const ng::NodeId m = add(g, "Math", {300.0f, 0.0f});
    connect(g, {f, "value"}, {m, "a"});

    expect(delete_nodes(g, {f}).changed, "node_edit: the node was deleted");
    expect(g.node(f) == nullptr, "node_edit: and is gone");
    expect(g.links().empty(),
           "node_edit: its link went with it -- a link to a node that no longer exists is the one "
           "thing validate() cannot attribute to anything");

    expect(!delete_nodes(g, {999}).changed, "node_edit: deleting a node that is not there is a no-op");
}

/** @brief Removing one wire from a multi-input leaves the others. */
COOPA_TEST(disconnect_removes_exactly_the_wire_that_was_picked) {
    setup();
    ng::Graph g;
    const ng::NodeId c1 = add(g, "Cube");
    const ng::NodeId c2 = add(g, "Cube", {0.0f, 100.0f});
    const ng::NodeId j  = add(g, "JoinGeometry", {300.0f, 50.0f});
    connect(g, {c1, "geometry"}, {j, "geometry"});
    connect(g, {c2, "geometry"}, {j, "geometry"});

    expect(disconnect(g, 0).changed, "node_edit: the first wire was cut");
    const auto left = g.links_into({j, "geometry"});
    expect(left.size() == 1 && left[0]->from.node == c2,
           "node_edit: and ONLY it. remove_links_into() would have taken both, which is the wrong "
           "answer when a user picked one wire of several");

    expect(!disconnect(g, 7).changed, "node_edit: an out-of-range index is a no-op, not a crash");
    expect(!disconnect(g, -1).changed, "node_edit: and so is a negative one");
}

/** @brief Duplicating remaps internal links and drops the ones that led outside. */
COOPA_TEST(duplicating_remaps_only_links_whose_both_ends_were_copied) {
    setup();
    ng::Graph g;
    const ng::NodeId outside = add(g, "Float");
    const ng::NodeId a = add(g, "Math", {200.0f, 0.0f});
    const ng::NodeId b = add(g, "Math", {400.0f, 0.0f});
    connect(g, {outside, "value"}, {a, "a"});   // crosses the selection boundary
    connect(g, {a, "result"}, {b, "a"});        // internal to the selection

    std::vector<ng::NodeId> made;
    expect(duplicate(g, {a, b}, made).changed, "node_edit: the pair was duplicated");
    expect(made.size() == 2, "node_edit: two copies");
    expect(g.nodes().size() == 5, "node_edit: and the originals are still there");

    const auto copied_internal = g.links_into({made[1], "a"});
    expect(copied_internal.size() == 1 && copied_internal[0]->from.node == made[0],
           "node_edit: the internal link was remapped onto the COPIES, not left pointing at the "
           "originals");
    expect(g.links_into({made[0], "a"}).empty(),
           "node_edit: and the link that crossed the boundary was dropped rather than silently "
           "doubling a second consumer onto the original's upstream");

    expect(g.node(made[0])->ui_position != g.node(a)->ui_position,
           "node_edit: a copy is offset, so it is visibly not on top of its original");
}

/** @brief A move merges into one undo step while the drag runs, and closes on release. */
COOPA_TEST(moving_merges_while_dragging_and_closes_on_release) {
    setup();
    ng::Graph g;
    const ng::NodeId a = add(g, "Math", {0.0f, 0.0f});

    const NodeGraphEdit mid = move_nodes(g, {a}, {10.0f, 5.0f}, /*finished=*/false);
    expect(mid.changed && mid.active && !mid.finished,
           "node_edit: mid-drag the edit is active, so the undo step stays open and a drag is one "
           "entry rather than one per frame");

    const NodeGraphEdit end = move_nodes(g, {a}, {10.0f, 5.0f}, /*finished=*/true);
    expect(end.finished && !end.active, "node_edit: and release closes it");
    expect(g.node(a)->ui_position == glm::vec2(20.0f, 10.0f), "node_edit: both deltas applied");
    expect(mid.key == end.key,
           "node_edit: with one merge key throughout, which is what makes the steps collapse");

    const NodeGraphEdit still = move_nodes(g, {a}, {0.0f, 0.0f}, /*finished=*/false);
    expect(!still.changed, "node_edit: a zero delta changes nothing, so a click is not an edit");
}

/** @brief Adding places the node where it was dropped. */
COOPA_TEST(adding_a_node_puts_it_where_it_was_dropped) {
    setup();
    ng::Graph g;
    ng::NodeId id = 0;
    const NodeGraphEdit e = add_node_at(g, "Math", {123.0f, -45.0f}, id);
    expect(e.changed && id != 0, "node_edit: the node was added");
    expect(g.node(id)->ui_position == glm::vec2(123.0f, -45.0f),
           "node_edit: at the drop point, so it appears under the cursor rather than at the origin");
    expect(e.key.find("Math") != std::string::npos,
           "node_edit: and the undo label names the type: " + e.key);
}


// --- The graph's own interface ----------------------------------------------------------------

namespace {

/// @brief A graph with one Float input `size` wired from GroupInput into a Cube.
ng::Graph with_interface_() {
    ng::Graph g;
    ng::InterfaceSocket in;
    in.identifier = "size";
    in.type       = toy::procgraph::types().float_t;
    g.inputs().push_back(in);

    const ng::NodeId gi   = add(g, std::string(ng::k_group_input_type));
    const ng::NodeId cube = add(g, "Cube");
    g.add_link({gi, "size"}, {cube, "size"});
    return g;
}

} // namespace

/** @brief A wire may land on the graph's own output, whose sockets are not in any registry. */
COOPA_TEST(a_wire_may_connect_to_the_graphs_own_output) {
    setup();
    ng::Graph g;
    ng::InterfaceSocket out;
    out.identifier = "geometry";
    out.type       = toy::procgraph::types().geometry_t;
    g.outputs().push_back(out);

    const ng::NodeId cube = add(g, "Cube");
    const ng::NodeId go   = add(g, std::string(ng::k_group_output_type));

    // GroupOutput is structural, so its declaration comes from the interface and nowhere else.
    // Before that was wired up this was MissingSocket -- a graph you could not finish.
    expect(can_connect(g, {cube, "geometry"}, {go, "geometry"}) == ConnectVerdict::Ok,
           "node_edit: the graph's own output accepts a wire of its declared type");
    expect(connect(g, {cube, "geometry"}, {go, "geometry"}).changed,
           "node_edit: and the link is made");

    ng::Diagnostics diag;
    expect(g.validate(diag), "node_edit: validate agrees: " + diag.to_string());
}

/** @brief Adding gives a unique identifier and the type's default. */
COOPA_TEST(adding_an_interface_socket_never_collides) {
    setup();
    ng::Graph g;
    const ng::TypeId f = toy::procgraph::types().float_t;
    expect(add_interface_socket(g, false, f, "size").changed, "node_edit: the input was added");
    expect(add_interface_socket(g, false, f, "size").changed, "node_edit: and a second");
    expect(g.inputs().size() == 2, "node_edit: two inputs exist");
    expect(g.inputs()[0].identifier != g.inputs()[1].identifier,
           "node_edit: with distinct identifiers -- a duplicate would orphan one's links");
    expect(g.inputs()[1].identifier == "size_2",
           "node_edit: suffixed rather than renamed wholesale: " + g.inputs()[1].identifier);
    expect(g.inputs()[0].default_value.type() == f,
           "node_edit: and carries the type's default, so the row is editable at once");
}

/** @brief Renaming re-points the links that name the socket. */
COOPA_TEST(renaming_an_interface_socket_carries_its_links) {
    setup();
    ng::Graph g = with_interface_();
    expect(rename_interface_socket(g, false, 0, "edge").changed, "node_edit: the rename took");
    expect(g.inputs()[0].identifier == "edge", "node_edit: the interface shows the new name");
    expect(g.links().size() == 1 && g.links()[0].from.socket == "edge",
           "node_edit: and the link followed it rather than dangling on the old name");

    ng::Diagnostics diag;
    expect(g.validate(diag), "node_edit: the graph still validates: " + diag.to_string());
}

/** @brief A rename that would collide, or blank the name, is refused rather than applied. */
COOPA_TEST(an_interface_rename_that_would_orphan_links_is_refused) {
    setup();
    ng::Graph g = with_interface_();
    add_interface_socket(g, false, toy::procgraph::types().float_t, "other");

    expect(!rename_interface_socket(g, false, 0, "other").changed,
           "node_edit: a duplicate identifier is refused");
    expect(!rename_interface_socket(g, false, 0, "").changed, "node_edit: so is a blank one");
    expect(g.inputs()[0].identifier == "size", "node_edit: and the socket is untouched");
    expect(g.links().size() == 1 && g.links()[0].from.socket == "size",
           "node_edit: as is its link");
}

/** @brief Removing takes the links through the socket with it. */
COOPA_TEST(removing_an_interface_socket_drops_its_links) {
    setup();
    ng::Graph g = with_interface_();
    expect(remove_interface_socket(g, false, 0).changed, "node_edit: the input was removed");
    expect(g.inputs().empty(), "node_edit: the interface is empty");
    expect(g.links().empty(),
           "node_edit: and the link went too -- it named a socket that no longer exists");

    ng::Diagnostics diag;
    expect(g.validate(diag), "node_edit: so the graph still validates: " + diag.to_string());
}

/** @brief Re-typing drops only the links the new type cannot feed. */
COOPA_TEST(retyping_an_interface_socket_drops_only_links_that_stop_converting) {
    setup();
    ng::Graph g = with_interface_();
    expect(retype_interface_socket(g, false, 0, toy::procgraph::types().int_t).changed,
           "node_edit: Float -> Int took");
    expect(g.links().size() == 1,
           "node_edit: the link survives, because Int converts to the Float socket it feeds");

    expect(retype_interface_socket(g, false, 0, toy::procgraph::types().geometry_t).changed,
           "node_edit: Int -> Geometry took");
    expect(g.links().empty(),
           "node_edit: and now the link is gone -- Geometry cannot feed a Float size");

    ng::Diagnostics diag;
    expect(g.validate(diag), "node_edit: leaving a graph that validates: " + diag.to_string());
}

/** @brief Reordering changes only the order; links are by name, so they are untouched. */
COOPA_TEST(reordering_the_interface_leaves_links_alone) {
    setup();
    ng::Graph g = with_interface_();
    add_interface_socket(g, false, toy::procgraph::types().float_t, "other");

    expect(move_interface_socket(g, false, 1, -1).changed, "node_edit: the move took");
    expect(g.inputs()[0].identifier == "other" && g.inputs()[1].identifier == "size",
           "node_edit: the rows swapped");
    expect(g.links().size() == 1 && g.links()[0].from.socket == "size",
           "node_edit: and the link still names its socket, which never moved");

    expect(!move_interface_socket(g, false, 0, -1).changed,
           "node_edit: moving the first one up does nothing rather than wrapping");
    expect(!move_interface_socket(g, false, 1, 1).changed,
           "node_edit: nor the last one down");
}

}  // namespace toy::editor::testing
