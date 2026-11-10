/**
 * @file nodegraph_compile_test.cpp
 * @brief Compilation: group inlining, conversion insertion, pruning, ordering and chain fusion.
 *
 * What a compiled graph then *computes* is nodegraph_eval_test's job; this suite only inspects
 * the compiled structure.
 */

#include <coopa/testing/test.h>

#include <string>
#include <unordered_map>

#include "support/nodegraph_fixtures.h"

COOPA_TEST_SUITE("nodegraph_compile");

using namespace coopa::nodegraph;
using coopa::test::expect;

namespace {

/// @brief Counts surviving steps whose node type is `type`.
size_t count_steps(const CompiledGraph& c, const std::string& type) {
    size_t n = 0;
    for (const auto& s : c.steps()) {
        if (s.type != nullptr && s.type->name == type) ++n;
    }
    return n;
}

/// @brief A sub-graph with one Float input, one Float output, doubling via Mul.
Graph make_doubler() {
    const auto& t = nodegraph_fixtures::types();
    Graph g("doubler");
    InterfaceSocket in;
    in.identifier    = "x";
    in.type          = t.f;
    in.default_value = nodegraph_fixtures::fval(0.0f);
    g.inputs().push_back(in);

    NodeId gi  = g.add_node(k_group_input_type);
    NodeId two = nodegraph_fixtures::add_const(g, 2.0f);
    NodeId mul = g.add_node("Mul");
    g.add_link(SocketRef{gi, "x"}, SocketRef{mul, "a"});
    g.add_link(SocketRef{two, "value"}, SocketRef{mul, "b"});
    nodegraph_fixtures::wire_float_output(g, "y", mul, "result");
    return g;
}

} // namespace

COOPA_TEST(a_linear_graph_compiles_to_steps_in_dependency_order) {
    nodegraph_fixtures::setup();
    Graph g;
    NodeId c   = nodegraph_fixtures::add_const(g, 1.0f);
    NodeId h1  = g.add_node("Heavy");
    NodeId h2  = g.add_node("Heavy");
    g.add_link(SocketRef{c, "value"}, SocketRef{h1, "a"});
    g.add_link(SocketRef{h1, "result"}, SocketRef{h2, "a"});
    nodegraph_fixtures::wire_float_output(g, "out", h2, "result");

    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: a linear graph compiles: " + diag.to_string());
    expect(compiled.steps().size() == 3, "nodegraph: three nodes become three steps");

    // Every dependency must point at an earlier index, which is what lets the inline evaluator
    // simply walk the list.
    for (uint32_t i = 0; i < compiled.steps().size(); ++i) {
        for (uint32_t d : compiled.steps()[i].depends_on) {
            expect(d < i,
                   "nodegraph: step " + std::to_string(i) + " depends on " + std::to_string(d) +
                   ", which must be earlier -- topological order is what makes the inline path "
                   "a plain forward walk");
        }
    }
}

COOPA_TEST(a_node_that_cannot_reach_a_requested_output_is_pruned) {
    nodegraph_fixtures::setup();
    Graph g;
    NodeId used   = nodegraph_fixtures::add_const(g, 1.0f);
    NodeId add    = g.add_node("Add");
    g.add_link(SocketRef{used, "value"}, SocketRef{add, "a"});
    nodegraph_fixtures::wire_float_output(g, "out", add, "result");

    // A whole branch nothing reads.
    NodeId orphan      = nodegraph_fixtures::add_const(g, 9.0f);
    NodeId orphan_heavy = g.add_node("Heavy");
    g.add_link(SocketRef{orphan, "value"}, SocketRef{orphan_heavy, "a"});

    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: the graph compiles: " + diag.to_string());
    expect(count_steps(compiled, "Heavy") == 0,
           "nodegraph: a Heavy node nothing reads is pruned -- the reason eager evaluation is "
           "affordable at all");
    expect(count_steps(compiled, "Const") == 1,
           "nodegraph: the orphan branch's Const went with it, and the used one stayed");
    expect(count_steps(compiled, "Add") == 1, "nodegraph: the contributing node survived");
}

COOPA_TEST(requesting_one_output_prunes_the_other_outputs_branch) {
    const auto& t = nodegraph_fixtures::setup();
    Graph g;
    NodeId c  = nodegraph_fixtures::add_const(g, 2.0f);
    NodeId h  = g.add_node("Heavy");
    NodeId m  = g.add_node("Mul");
    g.add_link(SocketRef{c, "value"}, SocketRef{h, "a"});
    g.add_link(SocketRef{c, "value"}, SocketRef{m, "a"});

    NodeId out = g.add_node(k_group_output_type);
    InterfaceSocket a;
    a.identifier = "heavy_out";
    a.type       = t.f;
    InterfaceSocket b;
    b.identifier = "cheap_out";
    b.type       = t.f;
    g.outputs().push_back(a);
    g.outputs().push_back(b);
    g.add_link(SocketRef{h, "result"}, SocketRef{out, "heavy_out"});
    g.add_link(SocketRef{m, "result"}, SocketRef{out, "cheap_out"});

    Diagnostics all_diag;
    CompiledGraph all = compile(g, GroupLoader(), {}, all_diag);
    expect(count_steps(all, "Heavy") == 1 && count_steps(all, "Mul") == 1,
           "nodegraph: asking for everything keeps both branches");

    Diagnostics one_diag;
    CompiledGraph one = compile(g, GroupLoader(), {"cheap_out"}, one_diag);
    expect(one_diag.ok(), "nodegraph: a filtered compile succeeds: " + one_diag.to_string());
    expect(count_steps(one, "Heavy") == 0,
           "nodegraph: asking only for cheap_out prunes the Heavy branch feeding the other output");
    expect(count_steps(one, "Mul") == 1, "nodegraph: the requested branch survived");
}

COOPA_TEST(an_unread_output_is_flagged_so_a_node_can_skip_computing_it) {
    nodegraph_fixtures::setup();
    Graph g;
    NodeId c   = nodegraph_fixtures::add_const(g, 1.0f);
    NodeId sum = g.add_node("Sum");
    g.add_link(SocketRef{c, "value"}, SocketRef{sum, "values"}, true);
    // Sum has two outputs: `result` and `count`. Only `result` is wired out.
    nodegraph_fixtures::wire_float_output(g, "out", sum, "result");

    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: compiles: " + diag.to_string());

    const Step* step = nullptr;
    for (const auto& s : compiled.steps()) {
        if (s.type && s.type->name == "Sum") step = &s;
    }
    expect(step != nullptr, "nodegraph: the Sum step survived");
    int result_index = step->decl.find_output("result");
    int count_index  = step->decl.find_output("count");
    expect(step->output_needed.at(result_index),
           "nodegraph: the wired output is flagged needed");
    expect(!step->output_needed.at(count_index),
           "nodegraph: the UNWIRED output is flagged not-needed, so a node whose second output is "
           "expensive can skip it -- pruning works per node, this works per socket");
}

COOPA_TEST(a_conversion_is_attached_to_the_link_that_needs_one_and_nowhere_else) {
    const auto& t = nodegraph_fixtures::setup();
    Graph g;
    // MakeVec emits Vector; VecLength wants Vector -- no conversion.
    // Const emits Float; MakeVec wants Float -- no conversion.
    NodeId c  = nodegraph_fixtures::add_const(g, 3.0f);
    NodeId mv = g.add_node("MakeVec");
    NodeId vl = g.add_node("VecLength");
    g.add_link(SocketRef{c, "value"}, SocketRef{mv, "x"});
    g.add_link(SocketRef{mv, "vector"}, SocketRef{vl, "vector"});
    nodegraph_fixtures::wire_float_output(g, "out", vl, "length");

    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: matching types compile with no conversions: " + diag.to_string());
    for (const auto& s : compiled.steps()) {
        for (const auto& links : s.inputs) {
            for (const auto& src : links) {
                expect(src.convert == nullptr,
                       "nodegraph: no conversion is attached where the types already agree");
            }
        }
    }

    // Now force one: feed a Float straight into VecLength's Vector input (Float -> Vector exists).
    Graph conv;
    NodeId c2 = nodegraph_fixtures::add_const(conv, 2.0f);
    NodeId vl2 = conv.add_node("VecLength");
    conv.add_link(SocketRef{c2, "value"}, SocketRef{vl2, "vector"});
    nodegraph_fixtures::wire_float_output(conv, "out", vl2, "length");

    Diagnostics conv_diag;
    CompiledGraph converted = compile(conv, conv_diag);
    expect(conv_diag.ok(), "nodegraph: a convertible link compiles: " + conv_diag.to_string());

    int conversions = 0;
    for (const auto& s : converted.steps()) {
        for (const auto& links : s.inputs) {
            for (const auto& src : links) {
                if (src.convert != nullptr) {
                    ++conversions;
                    expect(src.convert_to == t.v,
                           "nodegraph: the conversion targets the consuming socket's type");
                }
            }
        }
    }
    expect(conversions == 1,
           "nodegraph: exactly one conversion was attached, on the one link that needed it -- "
           "carried on the binding rather than as a synthesized node, so it costs no extra step");
}

COOPA_TEST(a_group_is_inlined_and_two_uses_become_two_independent_step_sets) {
    nodegraph_fixtures::setup();
    Graph doubler = make_doubler();
    auto loader = [&doubler](const std::string& ref) -> const Graph* {
        return ref == "doubler" ? &doubler : nullptr;
    };

    Graph g;
    NodeId c  = nodegraph_fixtures::add_const(g, 5.0f);
    NodeId g1 = g.add_group_node("doubler");
    NodeId g2 = g.add_group_node("doubler");
    NodeId add = g.add_node("Add");
    g.add_link(SocketRef{c, "value"}, SocketRef{g1, "x"});
    g.add_link(SocketRef{c, "value"}, SocketRef{g2, "x"});
    g.add_link(SocketRef{g1, "y"}, SocketRef{add, "a"});
    g.add_link(SocketRef{g2, "y"}, SocketRef{add, "b"});
    nodegraph_fixtures::wire_float_output(g, "out", add, "result");

    Diagnostics diag;
    CompiledGraph compiled = compile(g, loader, {}, diag);
    expect(diag.ok(), "nodegraph: a graph using a group compiles: " + diag.to_string());

    expect(count_steps(compiled, "Group") == 0,
           "nodegraph: no Group step survives -- the sub-graph was inlined, not called");
    expect(count_steps(compiled, "Mul") == 2,
           "nodegraph: the group used twice produced TWO independent Mul steps, so the two uses "
           "cannot share state or slots");
    // Each inlining brings its own internal Const(2.0), plus the outer Const(5.0).
    expect(count_steps(compiled, "Const") == 3,
           "nodegraph: each inlining copied the sub-graph's own nodes too");
}

COOPA_TEST(a_group_that_references_itself_is_an_error_rather_than_a_hang) {
    nodegraph_fixtures::setup();
    const auto& t = nodegraph_fixtures::types();

    Graph recursive("recursive");
    InterfaceSocket in;
    in.identifier    = "x";
    in.type          = t.f;
    in.default_value = nodegraph_fixtures::fval(0.0f);
    recursive.inputs().push_back(in);
    NodeId gi   = recursive.add_node(k_group_input_type);
    NodeId self = recursive.add_group_node("recursive");
    recursive.add_link(SocketRef{gi, "x"}, SocketRef{self, "x"});
    nodegraph_fixtures::wire_float_output(recursive, "y", self, "y");

    auto loader = [&recursive](const std::string& ref) -> const Graph* {
        return ref == "recursive" ? &recursive : nullptr;
    };

    Diagnostics diag;
    CompiledGraph compiled = compile(recursive, loader, {}, diag);
    (void)compiled;
    expect(!diag.ok(),
           "nodegraph: a self-referencing group is reported rather than recursing until the stack "
           "runs out");
    expect(diag.to_string().find("itself") != std::string::npos,
           "nodegraph: the message says what happened: " + diag.to_string());
}

COOPA_TEST(an_unresolvable_group_reference_is_reported_by_name) {
    nodegraph_fixtures::setup();
    Graph g;
    g.add_group_node("not_on_disk");
    Diagnostics diag;
    CompiledGraph compiled = compile(g, GroupLoader(), {}, diag);
    (void)compiled;
    expect(!diag.ok(), "nodegraph: an unresolvable group is an error");
    expect(diag.to_string().find("not_on_disk") != std::string::npos,
           "nodegraph: the message names the missing group: " + diag.to_string());
}

COOPA_TEST(cheap_nodes_fuse_into_one_chain_and_a_heavy_node_gets_its_own) {
    nodegraph_fixtures::setup();

    // Const -> Add -> Mul: a run of Trivial nodes, each feeding only the next.
    Graph cheap;
    NodeId c  = nodegraph_fixtures::add_const(cheap, 1.0f);
    NodeId a  = cheap.add_node("Add");
    NodeId m  = cheap.add_node("Mul");
    cheap.add_link(SocketRef{c, "value"}, SocketRef{a, "a"});
    cheap.add_link(SocketRef{a, "result"}, SocketRef{m, "a"});
    nodegraph_fixtures::wire_float_output(cheap, "out", m, "result");

    Diagnostics cheap_diag;
    CompiledGraph fused = compile(cheap, cheap_diag);
    expect(cheap_diag.ok(), "nodegraph: compiles: " + cheap_diag.to_string());
    expect(fused.steps().size() == 3, "nodegraph: still three steps");
    expect(fused.chains().size() == 1,
           "nodegraph: three single-consumer Trivial nodes collapse into ONE chain -- scheduling a "
           "job each would cost more than the arithmetic. Got " +
           std::to_string(fused.chains().size()));
    expect(fused.chains()[0].steps.size() == 3, "nodegraph: all three steps are in that chain");

    // The same shape with a Heavy node in the middle must break the chain.
    Graph heavy;
    NodeId c2 = nodegraph_fixtures::add_const(heavy, 1.0f);
    NodeId h  = heavy.add_node("Heavy");
    NodeId m2 = heavy.add_node("Mul");
    heavy.add_link(SocketRef{c2, "value"}, SocketRef{h, "a"});
    heavy.add_link(SocketRef{h, "result"}, SocketRef{m2, "a"});
    nodegraph_fixtures::wire_float_output(heavy, "out", m2, "result");

    Diagnostics heavy_diag;
    CompiledGraph split = compile(heavy, heavy_diag);
    expect(heavy_diag.ok(), "nodegraph: compiles: " + heavy_diag.to_string());
    expect(split.chains().size() > 1,
           "nodegraph: a Heavy node is not fused into its neighbours' chain, so it can be "
           "scheduled on its own thread. Got " + std::to_string(split.chains().size()));
}

COOPA_TEST(a_fan_out_breaks_a_chain_because_both_consumers_must_be_schedulable) {
    nodegraph_fixtures::setup();
    Graph g;
    NodeId c = nodegraph_fixtures::add_const(g, 1.0f);
    NodeId h1 = g.add_node("Heavy");
    NodeId h2 = g.add_node("Heavy");
    NodeId add = g.add_node("Add");
    g.add_link(SocketRef{c, "value"}, SocketRef{h1, "a"});
    g.add_link(SocketRef{c, "value"}, SocketRef{h2, "a"});
    g.add_link(SocketRef{h1, "result"}, SocketRef{add, "a"});
    g.add_link(SocketRef{h2, "result"}, SocketRef{add, "b"});
    nodegraph_fixtures::wire_float_output(g, "out", add, "result");

    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: compiles: " + diag.to_string());
    expect(compiled.chains().size() >= 3,
           "nodegraph: a node with two consumers ends its chain, so the two independent Heavy "
           "branches can run at once. Got " + std::to_string(compiled.chains().size()));

    for (uint32_t ci = 0; ci < compiled.chains().size(); ++ci) {
        for (uint32_t d : compiled.chains()[ci].depends_on) {
            expect(d < ci,
                   "nodegraph: a chain only ever depends on an earlier chain, which is what lets "
                   "the evaluator create every handle before submitting anything");
        }
    }
}

COOPA_TEST(slot_reader_counts_match_the_number_of_steps_that_read_each_slot) {
    nodegraph_fixtures::setup();
    Graph g;
    NodeId c = nodegraph_fixtures::add_const(g, 1.0f);
    NodeId h1 = g.add_node("Heavy");
    NodeId h2 = g.add_node("Heavy");
    g.add_link(SocketRef{c, "value"}, SocketRef{h1, "a"});
    g.add_link(SocketRef{c, "value"}, SocketRef{h2, "a"});
    NodeId add = g.add_node("Add");
    g.add_link(SocketRef{h1, "result"}, SocketRef{add, "a"});
    g.add_link(SocketRef{h2, "result"}, SocketRef{add, "b"});
    nodegraph_fixtures::wire_float_output(g, "out", add, "result");

    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: compiles: " + diag.to_string());

    std::unordered_map<uint32_t, uint32_t> counted;
    for (const auto& s : compiled.steps()) {
        for (uint32_t slot : s.reads) ++counted[slot];
    }
    for (const auto& [slot, n] : counted) {
        uint32_t declared = compiled.slot_reader_counts().at(slot);
        // An interface-output slot carries one extra permanent reader so it survives collection.
        expect(declared == n || declared == n + 1,
               "nodegraph: slot " + std::to_string(slot) + " is read by " + std::to_string(n) +
               " steps and its count says " + std::to_string(declared) +
               " -- the release counting is only correct if these agree");
    }

    const Step* const_step = nullptr;
    for (const auto& s : compiled.steps()) {
        if (s.type && s.type->name == "Const") const_step = &s;
    }
    expect(const_step != nullptr, "nodegraph: the Const step survived");
    expect(compiled.slot_reader_counts().at(const_step->out_slots[0]) == 2,
           "nodegraph: a value read by two steps has a reader count of 2, so neither one releasing "
           "it early can free it under the other");
}

COOPA_TEST(a_step_reads_a_shared_slot_only_once_even_when_two_of_its_inputs_use_it) {
    nodegraph_fixtures::setup();
    Graph g;
    NodeId c = nodegraph_fixtures::add_const(g, 4.0f);
    NodeId add = g.add_node("Add");
    // The SAME slot feeds both inputs of one node.
    g.add_link(SocketRef{c, "value"}, SocketRef{add, "a"});
    g.add_link(SocketRef{c, "value"}, SocketRef{add, "b"});
    nodegraph_fixtures::wire_float_output(g, "out", add, "result");

    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: compiles: " + diag.to_string());

    const Step* add_step = nullptr;
    const Step* const_step = nullptr;
    for (const auto& s : compiled.steps()) {
        if (s.type && s.type->name == "Add") add_step = &s;
        if (s.type && s.type->name == "Const") const_step = &s;
    }
    expect(add_step != nullptr && const_step != nullptr, "nodegraph: both steps survived");

    uint32_t slot = const_step->out_slots[0];
    int occurrences = 0;
    for (uint32_t r : add_step->reads) {
        if (r == slot) ++occurrences;
    }
    expect(occurrences == 1,
           "nodegraph: a slot a step reads twice is listed ONCE in `reads`, so the step's single "
           "release decrements the counter once and cannot drive it negative");
    expect(compiled.slot_reader_counts().at(slot) == 1,
           "nodegraph: and the reader count matches that single claim");
}
