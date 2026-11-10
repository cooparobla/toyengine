/**
 * @file nodegraph_graph_test.cpp
 * @brief The authored `Graph`: link rules, validation, cycle detection and the YAML round trip.
 *
 * Value semantics are covered by nodegraph_value_test; what compilation does with a valid graph
 * is nodegraph_compile_test's job.
 */

#include <coopa/testing/test.h>

#include <string>

#include "support/nodegraph_fixtures.h"

#include <coopa/nodegraph/graph_yaml.h>

COOPA_TEST_SUITE("nodegraph_graph");

using namespace coopa::nodegraph;
using coopa::test::expect;

COOPA_TEST(a_valid_graph_validates_cleanly) {
    nodegraph_fixtures::setup();
    Graph g("valid");
    NodeId a = nodegraph_fixtures::add_const(g, 2.0f);
    NodeId b = nodegraph_fixtures::add_const(g, 3.0f);
    NodeId add = g.add_node("Add");
    g.add_link(SocketRef{a, "value"}, SocketRef{add, "a"});
    g.add_link(SocketRef{b, "value"}, SocketRef{add, "b"});
    nodegraph_fixtures::wire_float_output(g, "result", add, "result");

    // validate() is called on its own line: argument evaluation order within one expect() call
    // is unspecified, so building the message in the same call can read `diag` before it is filled.
    Diagnostics diag;
    const bool valid = g.validate(diag);
    expect(valid, "nodegraph: a well-formed graph with an interface validates. GroupInput and "
                  "GroupOutput are structural rather than registered node types, so validate() "
                  "must synthesize their sockets from the interface exactly as compile() does: " +
                  diag.to_string());
}

COOPA_TEST(node_ids_are_unique_and_never_reused_after_a_removal) {
    nodegraph_fixtures::setup();
    Graph g;
    NodeId a = g.add_node("Add");
    NodeId b = g.add_node("Add");
    expect(a != b && a != 0 && b != 0, "nodegraph: ids are distinct and never 0 (0 means 'no node')");

    g.remove_node(a);
    NodeId c = g.add_node("Add");
    expect(c != a,
           "nodegraph: an id is not reused after its node is removed -- a stale link holding it "
           "would otherwise silently re-point at an unrelated node");
}

COOPA_TEST(removing_a_node_removes_every_link_touching_it) {
    nodegraph_fixtures::setup();
    Graph g;
    NodeId a = nodegraph_fixtures::add_const(g, 1.0f);
    NodeId add = g.add_node("Add");
    g.add_link(SocketRef{a, "value"}, SocketRef{add, "a"});
    expect(g.links().size() == 1, "nodegraph: the link was added");

    g.remove_node(a);
    expect(g.links().empty(),
           "nodegraph: removing a node drops its links, so no link can reference a missing node");
}

COOPA_TEST(a_plain_input_takes_one_link_and_a_multi_input_accumulates) {
    nodegraph_fixtures::setup();
    Graph g;
    NodeId a = nodegraph_fixtures::add_const(g, 1.0f);
    NodeId b = nodegraph_fixtures::add_const(g, 2.0f);
    NodeId add = g.add_node("Add");

    g.add_link(SocketRef{a, "value"}, SocketRef{add, "a"});
    g.add_link(SocketRef{b, "value"}, SocketRef{add, "a"});
    expect(g.links_into(SocketRef{add, "a"}).size() == 1,
           "nodegraph: a second link onto a plain input REPLACES the first, which is what dropping "
           "a wire onto an occupied socket should do");
    expect(g.links_into(SocketRef{add, "a"}).front()->from.node == b,
           "nodegraph: the replacement is the newer link");

    NodeId sum = g.add_node("Sum");
    g.add_link(SocketRef{a, "value"}, SocketRef{sum, "values"}, true);
    g.add_link(SocketRef{b, "value"}, SocketRef{sum, "values"}, true);
    auto into = g.links_into(SocketRef{sum, "values"});
    expect(into.size() == 2, "nodegraph: a multi-input accumulates links");
    expect(into[0]->order == 0 && into[1]->order == 1,
           "nodegraph: multi-input order is assigned past every existing link and is stable");
    expect(into[0]->from.node == a && into[1]->from.node == b,
           "nodegraph: links_into returns them in order, which is the order the node reads them");
}

COOPA_TEST(a_cycle_is_rejected_and_names_the_nodes_in_it) {
    nodegraph_fixtures::setup();
    Graph g;
    NodeId x = g.add_node("Add");
    NodeId y = g.add_node("Add");
    g.add_link(SocketRef{x, "result"}, SocketRef{y, "a"});
    g.add_link(SocketRef{y, "result"}, SocketRef{x, "a"});

    Diagnostics diag;
    const bool valid = g.validate(diag);
    expect(!valid, "nodegraph: a cycle fails validation");
    const std::string text = diag.to_string();
    expect(text.find("cycle") != std::string::npos, "nodegraph: the message says 'cycle': " + text);
    expect(text.find("#" + std::to_string(x)) != std::string::npos &&
           text.find("#" + std::to_string(y)) != std::string::npos,
           "nodegraph: the message names the participating node ids, which is what an author "
           "needs to find it: " + text);
}

COOPA_TEST(a_self_link_is_refused_outright) {
    nodegraph_fixtures::setup();
    Graph g;
    NodeId a = g.add_node("Add");
    expect(!g.add_link(SocketRef{a, "result"}, SocketRef{a, "a"}),
           "nodegraph: a node cannot link to itself, so the simplest cycle never gets stored");
    expect(g.links().empty(), "nodegraph: the refused link was not recorded");
}

COOPA_TEST(an_unknown_node_type_is_an_error_naming_the_type) {
    nodegraph_fixtures::setup();
    Graph g;
    g.add_node("ThisTypeDoesNotExist");
    Diagnostics diag;
    const bool valid = g.validate(diag);
    expect(!valid, "nodegraph: an unknown node type fails validation");
    expect(diag.to_string().find("ThisTypeDoesNotExist") != std::string::npos,
           "nodegraph: the message names the missing type: " + diag.to_string());
}

COOPA_TEST(a_link_to_a_socket_that_does_not_exist_is_an_error) {
    nodegraph_fixtures::setup();
    Graph g;
    NodeId a = nodegraph_fixtures::add_const(g, 1.0f);
    NodeId add = g.add_node("Add");
    g.add_link(SocketRef{a, "value"}, SocketRef{add, "no_such_input"});

    Diagnostics diag;
    const bool valid = g.validate(diag);
    expect(!valid, "nodegraph: a link to a non-existent socket fails validation");
    expect(diag.to_string().find("no_such_input") != std::string::npos,
           "nodegraph: the message names the socket: " + diag.to_string());
}

COOPA_TEST(a_convertible_link_validates_and_an_unconvertible_one_names_both_types) {
    const auto& t = nodegraph_fixtures::setup();
    (void)t;

    // Float -> Vector is registered, so MakeVec's float input accepts a float and VecLength's
    // vector input accepts MakeVec's vector directly.
    Graph ok_graph;
    NodeId c = nodegraph_fixtures::add_const(ok_graph, 1.0f);
    NodeId mv = ok_graph.add_node("MakeVec");
    NodeId vl = ok_graph.add_node("VecLength");
    ok_graph.add_link(SocketRef{c, "value"}, SocketRef{mv, "x"});
    ok_graph.add_link(SocketRef{mv, "vector"}, SocketRef{vl, "vector"});
    Diagnostics ok_diag;
    const bool ok_valid = ok_graph.validate(ok_diag);
    expect(ok_valid, "nodegraph: matching types validate: " + ok_diag.to_string());

    // Float -> Vector IS registered, so feed a Vector into a Float socket instead: not registered.
    Graph bad_graph;
    NodeId c2 = nodegraph_fixtures::add_const(bad_graph, 1.0f);
    NodeId mv2 = bad_graph.add_node("MakeVec");
    NodeId sink = bad_graph.add_node("BigSink");
    bad_graph.add_link(SocketRef{c2, "value"}, SocketRef{mv2, "x"});
    bad_graph.add_link(SocketRef{mv2, "vector"}, SocketRef{sink, "payload"});
    Diagnostics bad_diag;
    const bool bad_valid = bad_graph.validate(bad_diag);
    expect(!bad_valid, "nodegraph: Vector -> Big has no conversion and fails");
    const std::string text = bad_diag.to_string();
    expect(text.find("Vector") != std::string::npos && text.find("Big") != std::string::npos,
           "nodegraph: the message names BOTH types, which is the message an author can act on: " + text);
}

COOPA_TEST(a_dynamic_declaration_follows_the_nodes_own_properties) {
    const auto& t = nodegraph_fixtures::setup();
    const NodeType* pt = NodeTypeRegistry::instance().find("Passthrough");
    expect(pt != nullptr, "nodegraph: the Passthrough fixture is registered");

    PropertyBag as_float;
    as_float.set("data_type", std::string("Float"));
    expect(pt->declaration_for(as_float).inputs.at(0).type == t.f,
           "nodegraph: a node's socket type follows its own props -- what a Switch node needs");

    PropertyBag as_vector;
    as_vector.set("data_type", std::string("Vector"));
    expect(pt->declaration_for(as_vector).inputs.at(0).type == t.v,
           "nodegraph: the SAME node type declares a different socket type for different props, "
           "which a static socket list could not express");
}

COOPA_TEST(a_graph_survives_a_yaml_round_trip_byte_for_byte) {
    const auto& t = nodegraph_fixtures::setup();

    Graph g("round_trip");
    InterfaceSocket in;
    in.identifier    = "length";
    in.label         = "Length";
    in.type          = t.f;
    in.default_value = nodegraph_fixtures::fval(4.0f);
    in.min_value     = nodegraph_fixtures::fval(0.5f);
    in.max_value     = nodegraph_fixtures::fval(10.0f);
    g.inputs().push_back(in);

    NodeId gi = g.add_node(k_group_input_type);
    PropertyBag props;
    props.set("operation", std::string("multiply"));
    props.set("an_unknown_future_key", std::string("preserve me"));
    NodeId mul = g.add_node("Mul", props);
    g.node(mul)->ui_position = glm::vec2(120.0f, -40.0f);
    g.add_link(SocketRef{gi, "length"}, SocketRef{mul, "a"});
    nodegraph_fixtures::wire_float_output(g, "result", mul, "result");

    const std::string first = graph_to_string(g);

    Diagnostics load_diag;
    fkyaml::node reparsed = fkyaml::node::deserialize(first);
    Graph loaded = graph_from_node(reparsed, load_diag);
    expect(load_diag.ok(), "nodegraph: the emitted graph re-parses cleanly: " + load_diag.to_string());

    const std::string second = graph_to_string(loaded);
    expect(first == second,
           "nodegraph: save -> load -> save is byte-identical, so an editor cannot churn a file "
           "just by opening it.\n--- first ---\n" + first + "--- second ---\n" + second);

    expect(loaded.name() == "round_trip", "nodegraph: the name survived");
    expect(loaded.nodes().size() == g.nodes().size(), "nodegraph: every node survived");
    expect(loaded.links().size() == g.links().size(), "nodegraph: every link survived");
    expect(loaded.inputs().size() == 1 && loaded.inputs()[0].identifier == "length",
           "nodegraph: the interface survived");
    expect(loaded.inputs()[0].default_value.as<float>(-1.0f) == 4.0f,
           "nodegraph: a socket default survived, via the type's registered YAML hook");
    expect(loaded.inputs()[0].min_value.as<float>(-1.0f) == 0.5f,
           "nodegraph: so did the UI range, which is what lets an inspector render a slider");

    const Node* loaded_mul = loaded.node(mul);
    expect(loaded_mul != nullptr, "nodegraph: node ids are written, not reassigned");
    expect(loaded_mul->props.str("an_unknown_future_key") == "preserve me",
           "nodegraph: a props key this build does not interpret is round-tripped UNTOUCHED -- a "
           "graph authored against a newer build is not silently damaged by an older one");
    expect(loaded_mul->ui_position.x == 120.0f, "nodegraph: the UI position survived");
}

COOPA_TEST(links_can_be_found_from_either_end) {
    nodegraph_fixtures::setup();
    Graph g;
    NodeId src = nodegraph_fixtures::add_const(g, 1.0f);
    NodeId h1 = g.add_node("Heavy");
    NodeId h2 = g.add_node("Heavy");
    g.add_link(SocketRef{src, "value"}, SocketRef{h1, "a"});
    g.add_link(SocketRef{src, "value"}, SocketRef{h2, "a"});

    expect(g.links_from(SocketRef{src, "value"}).size() == 2,
           "nodegraph: links_from finds both consumers of one output. Only links_into existed, so "
           "an editor redrawing wires when a node moves had to rescan every link by hand");
    expect(g.links_from(SocketRef{src, "nope"}).empty(),
           "nodegraph: and an output nothing leaves reports none");

    expect(g.links_of(src).size() == 2, "nodegraph: links_of finds a node's outgoing links");
    expect(g.links_of(h1).size() == 1, "nodegraph: and its incoming ones");
    expect(g.links_of(999).empty(), "nodegraph: a node that is not there has none");
}

COOPA_TEST(remove_link_takes_exactly_one_of_several_into_a_multi_input) {
    nodegraph_fixtures::setup();
    Graph g;
    NodeId a = nodegraph_fixtures::add_const(g, 1.0f);
    NodeId b = nodegraph_fixtures::add_const(g, 2.0f);
    NodeId sum = g.add_node("Sum");
    g.add_link(SocketRef{a, "value"}, SocketRef{sum, "values"}, true);
    g.add_link(SocketRef{b, "value"}, SocketRef{sum, "values"}, true);

    const Link first = g.links().front();
    expect(g.remove_link(first), "nodegraph: the link was found and removed");
    const auto left = g.links_into(SocketRef{sum, "values"});
    expect(left.size() == 1 && left[0]->from.node == b,
           "nodegraph: and ONLY it. remove_links_into() clears the whole socket, which is the "
           "wrong answer when a user picked one wire of several to cut");

    expect(!g.remove_link(first), "nodegraph: removing it twice reports that it was already gone");
}

COOPA_TEST(duplicating_remaps_internal_links_and_drops_boundary_ones) {
    nodegraph_fixtures::setup();
    Graph g;
    NodeId outside = nodegraph_fixtures::add_const(g, 5.0f);
    NodeId a = g.add_node("Heavy");
    NodeId b = g.add_node("Heavy");
    g.node(a)->ui_position = glm::vec2(10.0f, 20.0f);
    g.add_link(SocketRef{outside, "value"}, SocketRef{a, "a"});
    g.add_link(SocketRef{a, "result"}, SocketRef{b, "a"});

    const std::vector<NodeId> made = g.duplicate_nodes({a, b}, glm::vec2(50.0f, 0.0f));
    expect(made.size() == 2, "nodegraph: two copies were made");
    expect(g.nodes().size() == 5, "nodegraph: and the originals survive");
    expect(made[0] != a && made[1] != b, "nodegraph: the copies have fresh ids");
    expect(g.node(made[0])->ui_position == glm::vec2(60.0f, 20.0f),
           "nodegraph: offset by the requested amount, so a copy is visibly not on its original");

    const auto internal = g.links_into(SocketRef{made[1], "a"});
    expect(internal.size() == 1 && internal[0]->from.node == made[0],
           "nodegraph: a link with BOTH ends in the set is remapped onto the copies");
    expect(g.links_into(SocketRef{made[0], "a"}).empty(),
           "nodegraph: and one that crossed the boundary is dropped -- copying it would silently "
           "attach a second consumer to the original's upstream");

    expect(g.duplicate_nodes({999}, glm::vec2(0.0f)).empty(),
           "nodegraph: duplicating an id that names no node yields nothing");
}

COOPA_TEST(two_links_into_a_plain_input_are_reported) {
    nodegraph_fixtures::setup();
    Graph g;
    NodeId a = nodegraph_fixtures::add_const(g, 1.0f);
    NodeId b = nodegraph_fixtures::add_const(g, 2.0f);
    NodeId add = g.add_node("Add");

    // Pushed straight onto links(), which is exactly what graph_from_node() does when loading a
    // hand-written file -- add_link() would have replaced rather than appended.
    g.links().push_back(Link{SocketRef{a, "value"}, SocketRef{add, "a"}, 0});
    g.links().push_back(Link{SocketRef{b, "value"}, SocketRef{add, "a"}, 0});

    Diagnostics diag;
    const bool valid = g.validate(diag);
    expect(!valid,
           "nodegraph: a plain input with two links fails validation. The loader bypasses "
           "add_link(), so a hand-written file can state this and the compiler would silently use "
           "one and discard the other");
    const std::string text = diag.to_string();
    expect(text.find("multi-input") != std::string::npos && text.find("'a'") != std::string::npos,
           "nodegraph: and the message names the socket: " + text);

    int mentions = 0;
    for (const auto& e : diag.entries()) {
        if (e.message.find("multi-input") != std::string::npos) ++mentions;
    }
    expect(mentions == 1,
           "nodegraph: reported ONCE for the socket, not once per offending link -- two messages "
           "for one mistake is noise");
}

COOPA_TEST(a_multi_input_with_many_links_still_validates) {
    nodegraph_fixtures::setup();
    Graph g;
    NodeId sum = g.add_node("Sum");
    for (int i = 0; i < 3; ++i) {
        NodeId c = nodegraph_fixtures::add_const(g, static_cast<float>(i));
        g.add_link(SocketRef{c, "value"}, SocketRef{sum, "values"}, true);
    }
    nodegraph_fixtures::wire_float_output(g, "out", sum, "result");

    Diagnostics diag;
    const bool valid = g.validate(diag);
    expect(valid,
           "nodegraph: the new check is about PLAIN inputs only -- a multi-input exists precisely "
           "to take several: " + diag.to_string());
}

COOPA_TEST(a_node_added_after_a_load_cannot_collide_with_a_loaded_id) {
    nodegraph_fixtures::setup();
    Graph g;
    Node n;
    n.id   = 57;
    n.type = "Add";
    expect(g.add_node_with_id(std::move(n)), "nodegraph: a loader can place a node at a given id");
    NodeId fresh = g.add_node("Add");
    expect(fresh > 57,
           "nodegraph: add_node() after a load hands out an id above every loaded one, so a "
           "freshly added node cannot shadow one the file named");

    Node dup;
    dup.id   = 57;
    dup.type = "Add";
    expect(!g.add_node_with_id(std::move(dup)), "nodegraph: a duplicate id is refused");
    Node zero;
    zero.id  = 0;
    zero.type = "Add";
    expect(!g.add_node_with_id(std::move(zero)), "nodegraph: id 0 is refused -- it means 'no node'");
}
