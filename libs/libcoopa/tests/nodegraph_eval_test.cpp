/**
 * @file nodegraph_eval_test.cpp
 * @brief Evaluation: the inline path, the job-parallel path, and the properties that must hold
 *        identically across both.
 *
 * The single most important claim here is that the two paths agree. Everything else -- handle
 * hygiene, slot release, error isolation, cancellation -- exists because getting any of it wrong
 * produces a wrong *result* rather than a crash, which is the kind of bug a graph system hides
 * best.
 */

#include <coopa/testing/test.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <coopa/job/engine.h>

#include "support/nodegraph_fixtures.h"

COOPA_TEST_SUITE("nodegraph_eval");

using namespace coopa::nodegraph;
using nodegraph_fixtures::BigPayload;
using nodegraph_fixtures::Tracked;
using coopa::test::expect;
using coopa::test::expect_near;

namespace {

/// @brief A graph wide enough that the parallel path really has to schedule: 4 independent
///        Heavy branches joined by a multi-input Sum.
Graph make_wide_graph(int branches = 4) {
    Graph g("wide");
    NodeId sum = g.add_node("Sum");
    for (int i = 0; i < branches; ++i) {
        NodeId c = nodegraph_fixtures::add_const(g, static_cast<float>(i + 1));
        NodeId h = g.add_node("Heavy");
        NodeId m = g.add_node("Mul");
        g.add_link(SocketRef{c, "value"}, SocketRef{h, "a"});
        g.add_link(SocketRef{h, "result"}, SocketRef{m, "a"});
        g.add_link(SocketRef{m, "result"}, SocketRef{sum, "values"}, true);
    }
    nodegraph_fixtures::wire_float_output(g, "total", sum, "result");
    return g;
}

/// @brief Evaluates `compiled` once, inline or on `jobs`.
EvalResult run(const CompiledGraph& compiled, coopa::job::JobEngine* jobs,
               std::vector<Value> inputs = {}) {
    EvalRequest request;
    request.graph    = &compiled;
    request.inputs   = inputs;
    request.jobs     = jobs;
    request.priority = coopa::job::Priority::Normal;
    return evaluate(request);
}

} // namespace

COOPA_TEST(an_inline_evaluation_computes_the_expected_value) {
    nodegraph_fixtures::setup();
    Graph g;
    NodeId a = nodegraph_fixtures::add_const(g, 2.0f);
    NodeId b = nodegraph_fixtures::add_const(g, 3.0f);
    NodeId add = g.add_node("Add");
    g.add_link(SocketRef{a, "value"}, SocketRef{add, "a"});
    g.add_link(SocketRef{b, "value"}, SocketRef{add, "b"});
    nodegraph_fixtures::wire_float_output(g, "out", add, "result");

    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: compiles: " + diag.to_string());

    EvalResult result = run(compiled, nullptr);
    expect(result.ok, "nodegraph: the evaluation succeeded: " + result.diagnostics.to_string());
    expect(result.outputs.size() == 1, "nodegraph: one interface output, one result");
    expect_near(result.outputs[0].as<float>(-1.0f), 5.0f, 1e-6f, "nodegraph: 2 + 3 = 5");
    expect(result.chains_submitted == 0, "nodegraph: the inline path submits no jobs");
}

COOPA_TEST(an_unconnected_input_falls_back_to_its_declared_default) {
    nodegraph_fixtures::setup();
    Graph g;
    // Mul's `b` defaults to 1.0 and nothing is linked to it.
    NodeId a = nodegraph_fixtures::add_const(g, 7.0f);
    NodeId mul = g.add_node("Mul");
    g.add_link(SocketRef{a, "value"}, SocketRef{mul, "a"});
    nodegraph_fixtures::wire_float_output(g, "out", mul, "result");

    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: compiles: " + diag.to_string());
    EvalResult result = run(compiled, nullptr);
    expect_near(result.outputs[0].as<float>(-1.0f), 7.0f, 1e-6f,
                "nodegraph: 7 * (default 1.0) = 7 -- an unlinked input gets a constant slot, which "
                "is why the evaluator has no 'is this linked?' branch anywhere");
}

COOPA_TEST(interface_inputs_are_bound_positionally_and_default_when_absent) {
    const auto& t = nodegraph_fixtures::setup();
    Graph g;
    InterfaceSocket in;
    in.identifier    = "x";
    in.type          = t.f;
    in.default_value = nodegraph_fixtures::fval(10.0f);
    g.inputs().push_back(in);

    NodeId gi  = g.add_node(k_group_input_type);
    NodeId two = nodegraph_fixtures::add_const(g, 2.0f);
    NodeId mul = g.add_node("Mul");
    g.add_link(SocketRef{gi, "x"}, SocketRef{mul, "a"});
    g.add_link(SocketRef{two, "value"}, SocketRef{mul, "b"});
    nodegraph_fixtures::wire_float_output(g, "out", mul, "result");

    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: compiles: " + diag.to_string());

    EvalResult defaulted = run(compiled, nullptr);
    expect_near(defaulted.outputs[0].as<float>(-1.0f), 20.0f, 1e-6f,
                "nodegraph: with no input supplied the interface default (10) is used: 10 * 2 = 20");

    std::vector<Value> given{nodegraph_fixtures::fval(4.0f)};
    EvalResult supplied = run(compiled, nullptr, given);
    expect_near(supplied.outputs[0].as<float>(-1.0f), 8.0f, 1e-6f,
                "nodegraph: a supplied input overrides the default: 4 * 2 = 8");

    // The same compiled graph, evaluated again with different inputs, must not carry state over.
    std::vector<Value> again{nodegraph_fixtures::fval(5.0f)};
    EvalResult third = run(compiled, nullptr, again);
    expect_near(third.outputs[0].as<float>(-1.0f), 10.0f, 1e-6f,
                "nodegraph: a CompiledGraph is immutable and reusable -- a second evaluation with "
                "different inputs is unaffected by the first");
}

COOPA_TEST(an_interface_input_of_a_convertible_type_is_converted_rather_than_dropped) {
    const auto& t = nodegraph_fixtures::setup();
    Graph g;
    InterfaceSocket in;
    in.identifier    = "x";
    in.type          = t.f;
    in.default_value = nodegraph_fixtures::fval(0.0f);
    g.inputs().push_back(in);
    NodeId gi = g.add_node(k_group_input_type);
    NodeId h  = g.add_node("Heavy");
    g.add_link(SocketRef{gi, "x"}, SocketRef{h, "a"});
    nodegraph_fixtures::wire_float_output(g, "out", h, "result");

    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: compiles: " + diag.to_string());

    // Hand it an Int where a Float is wanted: Int -> Float is registered.
    std::vector<Value> as_int{nodegraph_fixtures::ival(6)};
    EvalResult result = run(compiled, nullptr, as_int);
    expect_near(result.outputs[0].as<float>(-1.0f), 12.0f, 1e-6f,
                "nodegraph: an Int bound to a Float parameter is converted (6 -> 6.0, doubled = 12), "
                "because a consumer binding a scalar to a wider parameter is ordinary");

    // And one with no conversion warns and uses the default rather than reinterpreting bytes.
    std::vector<Value> as_big{Value::make(t.big, BigPayload(1))};
    EvalResult bad = run(compiled, nullptr, as_big);
    expect_near(bad.outputs[0].as<float>(-1.0f), 0.0f, 1e-6f,
                "nodegraph: an unconvertible input falls back to the declared default");
    expect(!bad.diagnostics.entries().empty(),
           "nodegraph: and says so, rather than silently substituting zero");
}

COOPA_TEST(a_multi_input_sees_every_link_in_order) {
    nodegraph_fixtures::setup();
    Graph g;
    NodeId sum = g.add_node("Sum");
    for (int i = 1; i <= 3; ++i) {
        NodeId c = nodegraph_fixtures::add_const(g, static_cast<float>(i));
        g.add_link(SocketRef{c, "value"}, SocketRef{sum, "values"}, true);
    }
    nodegraph_fixtures::wire_float_output(g, "total", sum, "result");

    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: compiles: " + diag.to_string());
    EvalResult result = run(compiled, nullptr);
    expect_near(result.outputs[0].as<float>(-1.0f), 6.0f, 1e-6f,
                "nodegraph: a multi-input summed all three links: 1 + 2 + 3 = 6");
}

COOPA_TEST(the_parallel_result_equals_the_inline_result) {
    nodegraph_fixtures::setup();
    Graph g = make_wide_graph(6);

    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: the wide graph compiles: " + diag.to_string());
    expect(compiled.chains().size() > 2,
           "nodegraph: the wide graph really does split into chains (" +
           std::to_string(compiled.chains().size()) + "), so this test is actually scheduling");

    EvalResult inline_result = run(compiled, nullptr);
    expect(inline_result.ok, "nodegraph: inline succeeded: " + inline_result.diagnostics.to_string());

    coopa::job::JobEngine engine(4);
    EvalResult parallel_result = run(compiled, &engine);
    expect(parallel_result.ok,
           "nodegraph: parallel succeeded: " + parallel_result.diagnostics.to_string());

    expect(parallel_result.outputs.size() == inline_result.outputs.size(),
           "nodegraph: both paths produce the same number of outputs");
    for (size_t i = 0; i < inline_result.outputs.size(); ++i) {
        expect(parallel_result.outputs[i].equals(inline_result.outputs[i]),
               "nodegraph: output " + std::to_string(i) +
               " is IDENTICAL between the inline and job-parallel paths. This is the property the "
               "whole two-path design exists to guarantee -- they share run_step_() and the slot "
               "arithmetic precisely so this cannot drift. inline=" +
               std::to_string(inline_result.outputs[i].as<float>(-1.0f)) + " parallel=" +
               std::to_string(parallel_result.outputs[i].as<float>(-1.0f)));
    }
    expect(parallel_result.chains_submitted == compiled.chains().size(),
           "nodegraph: the parallel path submitted one job per chain");

    engine.shutdown();
}

COOPA_TEST(repeated_parallel_evaluations_are_deterministic) {
    nodegraph_fixtures::setup();
    Graph g = make_wide_graph(5);
    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: compiles: " + diag.to_string());

    coopa::job::JobEngine engine(4);
    EvalResult first = run(compiled, &engine);
    expect(first.ok, "nodegraph: the first run succeeded: " + first.diagnostics.to_string());

    // Scheduling order varies run to run; the result must not.
    for (int i = 0; i < 25; ++i) {
        EvalResult again = run(compiled, &engine);
        expect(again.ok, "nodegraph: run " + std::to_string(i) + " succeeded");
        expect(again.outputs[0].equals(first.outputs[0]),
               "nodegraph: run " + std::to_string(i) +
               " matched the first -- a graph whose result depended on which worker got there "
               "first would be unusable for a bake");
    }
    engine.shutdown();
}

COOPA_TEST(every_job_handle_the_evaluator_creates_is_closed) {
    nodegraph_fixtures::setup();
    Graph g = make_wide_graph(4);
    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: compiles: " + diag.to_string());

    coopa::job::JobEngine engine(4);
    const uint32_t before = engine.get_counter_pool().debug_outstanding_count();

    for (int i = 0; i < 20; ++i) {
        EvalResult result = run(compiled, &engine);
        expect(result.ok, "nodegraph: run " + std::to_string(i) + " succeeded");
    }

    const uint32_t after = engine.get_counter_pool().debug_outstanding_count();
    expect(after == before,
           "nodegraph: twenty evaluations leaked NO job handles (" + std::to_string(before) +
           " before, " + std::to_string(after) + " after). The pool is a fixed 4096 slots shared "
           "process-wide, so a leak here would eventually starve every other subsystem");
    engine.shutdown();
}

COOPA_TEST(exhausting_the_handle_pool_falls_back_to_inline_rather_than_corrupting_the_order) {
    nodegraph_fixtures::setup();
    Graph g = make_wide_graph(6);
    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: compiles: " + diag.to_string());

    EvalResult reference = run(compiled, nullptr);
    expect(reference.ok, "nodegraph: the inline reference succeeded");

    // A deliberately tiny pool, then drain it so create_handle() must fail.
    coopa::job::JobEngine engine(2, 256, 8);
    std::vector<coopa::job::JobHandle> hogged;
    for (int i = 0; i < 64; ++i) {
        coopa::job::JobHandle h = engine.create_handle();
        if (!h.is_valid()) break;
        hogged.push_back(h);
    }
    expect(!hogged.empty(), "nodegraph: the test managed to take handles out of the small pool");

    EvalResult starved = run(compiled, &engine);
    expect(starved.outputs.size() == reference.outputs.size(),
           "nodegraph: a starved evaluation still produces its outputs");
    expect(starved.outputs[0].equals(reference.outputs[0]),
           "nodegraph: with the handle pool exhausted the result is STILL correct. This is the "
           "hazard the design is built around: create_handle() returns an INVALID handle when the "
           "pool is empty, and an invalid handle reports as already-complete -- so submitting "
           "against one would run a chain before its inputs existed, silently. The evaluator "
           "takes every handle before submitting anything and drops to the inline path if any "
           "one of them is invalid.");
    expect(starved.chains_submitted == 0,
           "nodegraph: and it genuinely took the inline path rather than submitting anyway");
    bool warned = false;
    for (const auto& e : starved.diagnostics.entries()) {
        if (e.message.find("handle pool") != std::string::npos) warned = true;
    }
    expect(warned,
           "nodegraph: the fallback is reported as a warning, so a slow bake is explainable "
           "instead of mysterious");

    for (const auto& h : hogged) h.close();
    engine.shutdown();
}

COOPA_TEST(a_node_that_throws_is_reported_and_does_not_take_the_evaluation_down) {
    nodegraph_fixtures::setup();
    Graph g;
    NodeId c = nodegraph_fixtures::add_const(g, 1.0f);
    NodeId thrower = g.add_node("Throw");
    NodeId add = g.add_node("Add");
    g.add_link(SocketRef{c, "value"}, SocketRef{thrower, "a"});
    g.add_link(SocketRef{thrower, "result"}, SocketRef{add, "a"});
    g.add_link(SocketRef{c, "value"}, SocketRef{add, "b"});
    nodegraph_fixtures::wire_float_output(g, "out", add, "result");

    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: a graph with a throwing node still compiles");

    EvalResult inline_result = run(compiled, nullptr);
    expect(!inline_result.ok, "nodegraph: the result reports failure");
    expect(inline_result.diagnostics.error_count() >= 1, "nodegraph: an error was recorded");
    bool named = false;
    for (const auto& e : inline_result.diagnostics.entries()) {
        if (e.node == thrower && e.message.find("threw") != std::string::npos) named = true;
    }
    expect(named,
           "nodegraph: the error is attributed to the node that threw, by id, which is what lets "
           "a tool put a marker on it: " + inline_result.diagnostics.to_string());
    expect(inline_result.outputs.size() == 1,
           "nodegraph: downstream still ran against defaults, so there is a result to display "
           "next to the message");

    // The same must hold on a worker thread, where an escaping exception would be fatal.
    coopa::job::JobEngine engine(2);
    EvalResult parallel_result = run(compiled, &engine);
    expect(!parallel_result.ok, "nodegraph: the parallel path reports the failure too");
    expect(parallel_result.diagnostics.error_count() >= 1,
           "nodegraph: a node throwing on a WORKER thread is caught at the chain boundary rather "
           "than terminating the process");
    engine.shutdown();
}

COOPA_TEST(fusion_changes_the_job_count_but_not_the_result) {
    nodegraph_fixtures::setup();

    // Four independent chains of three Trivial nodes each. Fused: 4 jobs, not 12.
    Graph g("fusable");
    NodeId sum = g.add_node("Sum");
    for (int i = 0; i < 4; ++i) {
        NodeId c = nodegraph_fixtures::add_const(g, static_cast<float>(i + 1));
        NodeId a = g.add_node("Add");
        NodeId m = g.add_node("Mul");
        g.add_link(SocketRef{c, "value"}, SocketRef{a, "a"});
        g.add_link(SocketRef{a, "result"}, SocketRef{m, "a"});
        g.add_link(SocketRef{m, "result"}, SocketRef{sum, "values"}, true);
    }
    nodegraph_fixtures::wire_float_output(g, "total", sum, "result");

    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: compiles: " + diag.to_string());

    expect(compiled.steps().size() == 13, "nodegraph: 4*3 arithmetic nodes plus the Sum = 13 steps");
    expect(compiled.chains().size() < compiled.steps().size(),
           "nodegraph: fusion produced fewer chains (" + std::to_string(compiled.chains().size()) +
           ") than steps (" + std::to_string(compiled.steps().size()) +
           ") -- otherwise a utility-heavy graph would be slower with threads than without");

    EvalResult inline_result = run(compiled, nullptr);
    coopa::job::JobEngine engine(4);
    EvalResult parallel_result = run(compiled, &engine);

    expect_near(inline_result.outputs[0].as<float>(-1.0f), 10.0f, 1e-6f,
                "nodegraph: (1+0)*1 + (2+0)*1 + (3+0)*1 + (4+0)*1 = 10");
    expect(parallel_result.outputs[0].equals(inline_result.outputs[0]),
           "nodegraph: fusion is a scheduling decision only -- the value is unchanged");
    expect(parallel_result.chains_submitted == compiled.chains().size(),
           "nodegraph: one job per CHAIN, not per step");
    engine.shutdown();
}

COOPA_TEST(a_value_read_by_two_nodes_is_not_deep_copied) {
    nodegraph_fixtures::setup();
    nodegraph_fixtures::reset_counters();

    Graph g;
    PropertyBag props;
    props.set("tag", int64_t(42));
    NodeId source = g.add_node("MakeTracked", props);
    NodeId r1 = g.add_node("ReadTracked");
    NodeId r2 = g.add_node("ReadTracked");
    NodeId r3 = g.add_node("ReadTracked");
    g.add_link(SocketRef{source, "value"}, SocketRef{r1, "value"});
    g.add_link(SocketRef{source, "value"}, SocketRef{r2, "value"});
    g.add_link(SocketRef{source, "value"}, SocketRef{r3, "value"});

    const auto& t = nodegraph_fixtures::types();
    NodeId out = g.add_node(k_group_output_type);
    InterfaceSocket os;
    os.identifier = "tag";
    os.type       = t.i;
    g.outputs().push_back(os);
    g.add_link(SocketRef{r1, "tag"}, SocketRef{out, "tag"});

    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: compiles: " + diag.to_string());

    const int before = Tracked::copies().load();
    EvalResult result = run(compiled, nullptr);
    const int made = Tracked::copies().load() - before;

    expect(result.ok, "nodegraph: the evaluation succeeded: " + result.diagnostics.to_string());
    expect(result.outputs[0].as<int32_t>(-1) == 42, "nodegraph: the value arrived intact");
    expect(made <= 1,
           "nodegraph: a payload fanned out to three consumers was deep-copied at most once (it "
           "was copied " + std::to_string(made) + " times). Fan-out is a refcount bump, which is "
           "what keeps a chain of mesh operations from duplicating a 200 MB buffer per consumer");
}

COOPA_TEST(a_slot_is_released_once_its_last_reader_has_run) {
    nodegraph_fixtures::setup();

    // BigSource -> two BigSinks. After both have read it, the payload must be gone.
    Graph g;
    PropertyBag props;
    props.set("tag", int64_t(5));
    NodeId src = g.add_node("BigSource", props);
    NodeId s1 = g.add_node("BigSink");
    NodeId s2 = g.add_node("BigSink");
    g.add_link(SocketRef{src, "payload"}, SocketRef{s1, "payload"});
    g.add_link(SocketRef{src, "payload"}, SocketRef{s2, "payload"});

    // BOTH sinks must be wired out, or pruning quite correctly deletes the unread one and the
    // slot ends up with a single reader -- which would make this test pass for the wrong reason.
    const auto& t = nodegraph_fixtures::types();
    NodeId out = g.add_node(k_group_output_type);
    InterfaceSocket first;
    first.identifier = "tag_a";
    first.type       = t.i;
    InterfaceSocket second;
    second.identifier = "tag_b";
    second.type       = t.i;
    g.outputs().push_back(first);
    g.outputs().push_back(second);
    g.add_link(SocketRef{s1, "tag"}, SocketRef{out, "tag_a"});
    g.add_link(SocketRef{s2, "tag"}, SocketRef{out, "tag_b"});

    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: compiles: " + diag.to_string());
    expect(compiled.steps().size() == 3,
           "nodegraph: all three steps survived pruning, so the slot really has two readers");

    const Step* src_step = nullptr;
    for (const auto& s : compiled.steps()) {
        if (s.type && s.type->name == "BigSource") src_step = &s;
    }
    expect(src_step != nullptr, "nodegraph: the source step survived");
    expect(compiled.slot_reader_counts().at(src_step->out_slots[0]) == 2,
           "nodegraph: the payload slot knows it has two readers");

    EvalResult result = run(compiled, nullptr);
    expect(result.ok, "nodegraph: the evaluation succeeded: " + result.diagnostics.to_string());
    expect(result.outputs[0].as<int32_t>(-1) == 5 && result.outputs[1].as<int32_t>(-1) == 5,
           "nodegraph: BOTH consumers read the payload before it was released -- an early release "
           "would have handed the second one an empty value");

    coopa::job::JobEngine engine(3);
    EvalResult parallel_result = run(compiled, &engine);
    expect(parallel_result.outputs[0].equals(result.outputs[0]),
           "nodegraph: release-by-refcount works under real concurrency too. Releasing by "
           "'last reader in topological order' would be wrong here: topological position says "
           "nothing about which of two parallel readers finishes last");
    engine.shutdown();
}

COOPA_TEST(a_cancelled_evaluation_stops_and_leaks_no_handles) {
    nodegraph_fixtures::setup();

    Graph g("slow");
    NodeId sum = g.add_node("Sum");
    for (int i = 0; i < 6; ++i) {
        PropertyBag props;
        props.set("ms", int64_t(30));
        NodeId c = nodegraph_fixtures::add_const(g, 1.0f);
        NodeId slow = g.add_node("Slow", props);
        g.add_link(SocketRef{c, "value"}, SocketRef{slow, "a"});
        g.add_link(SocketRef{slow, "result"}, SocketRef{sum, "values"}, true);
    }
    nodegraph_fixtures::wire_float_output(g, "total", sum, "result");

    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: compiles: " + diag.to_string());

    coopa::job::JobEngine engine(4);
    const uint32_t before = engine.get_counter_pool().debug_outstanding_count();

    std::atomic<bool> cancel{true};   // Cancelled before it even starts.
    EvalRequest request;
    request.graph    = &compiled;
    request.jobs     = &engine;
    request.cancel   = &cancel;
    request.priority = coopa::job::Priority::Normal;
    EvalResult result = evaluate(request);

    expect(!result.ok, "nodegraph: a cancelled evaluation reports failure rather than a half result");
    const uint32_t after = engine.get_counter_pool().debug_outstanding_count();
    expect(after == before,
           "nodegraph: cancelling leaked no handles (" + std::to_string(before) + " -> " +
           std::to_string(after) + ") -- every chain's job still completes its handle, it simply "
           "does no work, which is what keeps the pool from bleeding on an abandoned bake");
    engine.shutdown();
}

COOPA_TEST(the_parallel_path_actually_overlaps_independent_work) {
    nodegraph_fixtures::setup();

    // Six independent 40 ms sleeps. Serially that is ~240 ms; on four workers it must be well under.
    Graph g("overlap");
    NodeId sum = g.add_node("Sum");
    for (int i = 0; i < 6; ++i) {
        PropertyBag props;
        props.set("ms", int64_t(40));
        NodeId c = nodegraph_fixtures::add_const(g, 1.0f);
        NodeId slow = g.add_node("Slow", props);
        g.add_link(SocketRef{c, "value"}, SocketRef{slow, "a"});
        g.add_link(SocketRef{slow, "result"}, SocketRef{sum, "values"}, true);
    }
    nodegraph_fixtures::wire_float_output(g, "total", sum, "result");

    Diagnostics diag;
    CompiledGraph compiled = compile(g, diag);
    expect(diag.ok(), "nodegraph: compiles: " + diag.to_string());

    coopa::job::JobEngine engine(4);
    const auto start = std::chrono::steady_clock::now();
    EvalResult result = run(compiled, &engine);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start).count();

    expect(result.ok, "nodegraph: the evaluation succeeded: " + result.diagnostics.to_string());
    expect_near(result.outputs[0].as<float>(-1.0f), 12.0f, 1e-6f,
                "nodegraph: six Slow nodes each returning 1+1 summed to 12");
    // Generous: six 40 ms sleeps over four workers is ~80 ms of wall clock; serial would be ~240.
    expect(elapsed < 200,
           "nodegraph: six independent 40 ms nodes finished in " + std::to_string(elapsed) +
           " ms, so they genuinely ran concurrently rather than one after another (serial would "
           "be ~240 ms). This is what the job integration is for");
    engine.shutdown();
}

COOPA_TEST(an_empty_graph_and_a_null_request_are_handled_rather_than_crashing) {
    nodegraph_fixtures::setup();

    EvalRequest nothing;
    EvalResult null_result = evaluate(nothing);
    expect(!null_result.ok, "nodegraph: evaluating with no graph fails cleanly");
    expect(!null_result.diagnostics.entries().empty(), "nodegraph: and says why");

    Graph empty("empty");
    Diagnostics diag;
    CompiledGraph compiled = compile(empty, diag);
    expect(diag.ok(), "nodegraph: an empty graph compiles without complaint");
    EvalResult empty_result = run(compiled, nullptr);
    expect(empty_result.ok, "nodegraph: and evaluates to nothing successfully");
    expect(empty_result.outputs.empty(), "nodegraph: with no outputs");
}

COOPA_TEST(a_group_produces_the_same_answer_as_the_equivalent_flat_graph) {
    const auto& t = nodegraph_fixtures::setup();

    Graph doubler("doubler");
    InterfaceSocket in;
    in.identifier    = "x";
    in.type          = t.f;
    in.default_value = nodegraph_fixtures::fval(0.0f);
    doubler.inputs().push_back(in);
    NodeId dgi  = doubler.add_node(k_group_input_type);
    NodeId dtwo = nodegraph_fixtures::add_const(doubler, 2.0f);
    NodeId dmul = doubler.add_node("Mul");
    doubler.add_link(SocketRef{dgi, "x"}, SocketRef{dmul, "a"});
    doubler.add_link(SocketRef{dtwo, "value"}, SocketRef{dmul, "b"});
    nodegraph_fixtures::wire_float_output(doubler, "y", dmul, "result");

    auto loader = [&doubler](const std::string& ref) -> const Graph* {
        return ref == "doubler" ? &doubler : nullptr;
    };

    // Grouped: Const(5) -> [doubler] -> [doubler] -> out, so 5 * 2 * 2 = 20.
    Graph grouped;
    NodeId c  = nodegraph_fixtures::add_const(grouped, 5.0f);
    NodeId g1 = grouped.add_group_node("doubler");
    NodeId g2 = grouped.add_group_node("doubler");
    grouped.add_link(SocketRef{c, "value"}, SocketRef{g1, "x"});
    grouped.add_link(SocketRef{g1, "y"}, SocketRef{g2, "x"});
    nodegraph_fixtures::wire_float_output(grouped, "out", g2, "y");

    Diagnostics gdiag;
    CompiledGraph gcompiled = compile(grouped, loader, {}, gdiag);
    expect(gdiag.ok(), "nodegraph: the grouped graph compiles: " + gdiag.to_string());
    EvalResult gresult = run(gcompiled, nullptr);
    expect(gresult.ok, "nodegraph: it evaluates: " + gresult.diagnostics.to_string());
    expect_near(gresult.outputs[0].as<float>(-1.0f), 20.0f, 1e-6f,
                "nodegraph: two nested doublers over 5 give 20 -- a group behaves exactly as the "
                "flat equivalent, which is what makes 'a graph is reusable as a node' true rather "
                "than aspirational");

    coopa::job::JobEngine engine(3);
    EvalResult parallel_result = run(gcompiled, &engine);
    expect(parallel_result.outputs[0].equals(gresult.outputs[0]),
           "nodegraph: and an inlined group schedules correctly too");
    engine.shutdown();
}
