/**
 * @file procgraph_scene_input_test.cpp
 * @brief Scene bindings: the resolvers, the variable table, change detection, and the
 *        self-reference guard.
 *
 * No Vulkan device: a Scene with plain objects is enough, which is the point of resolving to
 * snapshot value types rather than to live pointers.
 */

#include <coopa/testing/test.h>

#include <string>
#include <unordered_map>

#include "engine/support/checks.h"

#include <coopa/nodegraph/compile.h>
#include <coopa/nodegraph/graph.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

#include <toyengine/procgraph/nodes.h>
#include <toyengine/procgraph/scene_input.h>

COOPA_TEST_SUITE("procgraph_scene_input");

using namespace toy::procgraph;
using coopa::nodegraph::CompiledGraph;
using coopa::nodegraph::Diagnostics;
using coopa::nodegraph::Graph;
using coopa::nodegraph::GroupLoader;
using coopa::nodegraph::InterfaceSocket;
using coopa::nodegraph::Value;
using coopa::scene::Scene;
using coopa::scene::SceneObject;
using coopa::test::expect;
using coopa::test::expect_near;

namespace {

void setup() {
    register_procgraph_nodes();
    register_builtin_input_resolvers();
}

/// @brief A graph whose interface has one input of `type` named `id`, and nothing else.
Graph graph_with_input(const std::string& id, coopa::nodegraph::TypeId type) {
    Graph g("bound");
    InterfaceSocket in;
    in.identifier    = id;
    in.type          = type;
    in.default_value = Value::make_default(type);
    g.inputs().push_back(in);
    g.add_node(coopa::nodegraph::k_group_input_type);
    return g;
}

/**
 * @brief Adds a named object at a position.
 *
 * The TransformComponent is added explicitly: a bare SceneObject has none (the scene loader adds
 * one for authored objects, via its `auto_transform` key), and a helper that silently skipped the
 * position would make every transform assertion below pass for the wrong reason.
 */
SceneObject* add_object(Scene& scene, const std::string& name, const glm::vec3& position) {
    auto obj = std::make_unique<SceneObject>(name);
    auto* tc = obj->add_component<coopa::scene::TransformComponent>();
    tc->transform().set_position(position);
    SceneObject* raw = scene.add_root_object(std::move(obj));
    scene.adopt(*raw);
    return raw;
}

/// @brief Adds a child with a transform, for the children_of and subtree tests.
SceneObject* add_child(SceneObject& parent, const std::string& name, const glm::vec3& position) {
    auto child = std::make_unique<SceneObject>(name);
    auto* tc = child->add_component<coopa::scene::TransformComponent>();
    tc->transform().set_position(position);
    SceneObject* raw = child.get();
    parent.add_child(std::move(child));
    return raw;
}

/// @brief Parses a binding from YAML text, as the component parser would.
InputBinding binding(const std::string& yaml) {
    return parse_input_binding(fkyaml::node::deserialize(yaml));
}

} // namespace

COOPA_TEST(the_built_in_resolver_kinds_are_registered_and_a_typo_is_detectable) {
    setup();
    const auto kinds = input_resolver_kinds();
    for (const char* expected : {"object", "children_of", "name_like", "component", "var"}) {
        bool found = false;
        for (const auto& k : kinds) {
            if (k == expected) found = true;
        }
        expect(found, std::string("procgraph: the '") + expected + "' resolver is registered");
    }
    expect(!has_input_resolver("tag"),
           "procgraph: there is deliberately NO `tag` resolver -- this engine's SceneObject has no "
           "tag concept, so a tag binding would need one invented first. `name_like` covers the "
           "same authoring need, and a parser can reject the typo rather than silently ignoring it");
}

COOPA_TEST(a_mapping_naming_no_resolver_is_a_literal_not_an_error) {
    setup();
    expect(binding("3.5").is_literal(), "procgraph: a scalar is a literal");
    expect(binding("[1, 2, 3]").is_literal(), "procgraph: a sequence is a literal");
    expect(binding("{ object: Rock }").kind == "object", "procgraph: a known key selects a resolver");
    expect(binding("{ children_of: Spawns }").kind == "children_of", "procgraph: ...for each kind");
    expect(binding("{ something_else: 4 }").is_literal(),
           "procgraph: a mapping naming no resolver is treated as a literal rather than rejected -- "
           "it is authored data, and a future resolver name should not be a hard error today");
    // Key order inside the mapping must not matter.
    expect(binding("{ geometry: true, object: Rock }").kind == "object",
           "procgraph: the resolver key is found wherever it sits in the mapping");
}

COOPA_TEST(an_object_binding_snapshots_the_transform_and_reports_a_missing_object) {
    setup();
    Scene scene;
    add_object(scene, "Marker", glm::vec3(3.0f, 4.0f, 5.0f));

    Graph g = graph_with_input("anchor", types().object_t);
    Diagnostics compile_diag;
    CompiledGraph compiled = coopa::nodegraph::compile(g, GroupLoader(), {}, compile_diag);
    expect(compile_diag.ok(), "procgraph: compiles: " + compile_diag.to_string());

    Diagnostics diag;
    ResolveContext ctx;
    ctx.scene = &scene;
    ctx.out   = &diag;

    std::unordered_map<std::string, InputBinding> bindings;
    bindings["anchor"] = binding("{ object: Marker }");

    uint64_t hash = 0;
    const auto values = resolve_inputs(bindings, compiled, ctx, hash);
    expect(values.size() == 1, "procgraph: one interface input, one resolved value");

    static const ObjectRef none;
    const ObjectRef ref = values[0].as<ObjectRef>(none);
    expect(ref.valid, "procgraph: the object was found");
    expect(ref.name == "Marker", "procgraph: and named");
    expect_near(ref.position.y, 4.0f, 1e-5f,
                "procgraph: its world position was snapshotted -- a VALUE, not a SceneObject*, "
                "which is what makes it safe to hand to a worker thread");

    // A missing object is a warning and an invalid ref, not a hard failure: a graph that places
    // something only when a marker exists is ordinary authoring.
    bindings["anchor"] = binding("{ object: NoSuchThing }");
    Diagnostics missing_diag;
    ctx.out = &missing_diag;
    const auto missing = resolve_inputs(bindings, compiled, ctx, hash);
    const ObjectRef bad = missing[0].as<ObjectRef>(none);
    expect(!bad.valid,
           "procgraph: a missing object resolves to an INVALID ref, so a graph can branch on "
           "`exists` rather than the whole bake failing");
    expect(missing_diag.ok(), "procgraph: and it is a warning, not an error");
}

COOPA_TEST(children_of_and_name_like_build_object_sets) {
    setup();
    Scene scene;
    SceneObject* parent = add_object(scene, "Spawns", glm::vec3(0.0f));
    for (int i = 0; i < 3; ++i) {
        add_child(*parent, "Spawn_" + std::to_string(i),
                  glm::vec3(static_cast<float>(i), 0.0f, 0.0f));
    }
    scene.adopt(*parent);
    add_object(scene, "Rock_a", glm::vec3(10.0f, 0.0f, 0.0f));
    add_object(scene, "Rock_b", glm::vec3(11.0f, 0.0f, 0.0f));
    add_object(scene, "Tree_a", glm::vec3(12.0f, 0.0f, 0.0f));

    Graph g = graph_with_input("set", types().object_set_t);
    Diagnostics compile_diag;
    CompiledGraph compiled = coopa::nodegraph::compile(g, GroupLoader(), {}, compile_diag);
    expect(compile_diag.ok(), "procgraph: compiles: " + compile_diag.to_string());

    Diagnostics diag;
    ResolveContext ctx;
    ctx.scene = &scene;
    ctx.out   = &diag;
    uint64_t hash = 0;
    static const ObjectSet none;

    std::unordered_map<std::string, InputBinding> bindings;
    bindings["set"] = binding("{ children_of: Spawns }");
    const ObjectSet children = resolve_inputs(bindings, compiled, ctx, hash)[0].as<ObjectSet>(none);
    expect(children.size() == 3,
           "procgraph: children_of captured all three children -- how you hand-author placement "
           "and let the graph do the construction. Got " + std::to_string(children.size()));

    bindings["set"] = binding("{ name_like: \"Rock_*\" }");
    const ObjectSet rocks = resolve_inputs(bindings, compiled, ctx, hash)[0].as<ObjectSet>(none);
    expect(rocks.size() == 2, "procgraph: the glob matched exactly the two rocks");
    expect(rocks.at(0).name == "Rock_a" && rocks.at(1).name == "Rock_b",
           "procgraph: and the set is sorted by name, so a graph instancing over it cannot "
           "reshuffle just because the scene's internal object order changed");
}

COOPA_TEST(glob_matching_handles_the_usual_patterns) {
    expect(glob_match("*", "anything"), "procgraph: bare star matches everything");
    expect(glob_match("Rock_*", "Rock_01"), "procgraph: a trailing star matches a prefix");
    expect(!glob_match("Rock_*", "Tree_01"), "procgraph: and rejects a non-match");
    expect(glob_match("*_01", "Rock_01"), "procgraph: a leading star matches a suffix");
    expect(glob_match("Rock_??", "Rock_ab"), "procgraph: ? matches exactly one character");
    expect(!glob_match("Rock_??", "Rock_abc"), "procgraph: and not two");
    expect(glob_match("a*b*c", "axxbyyc"), "procgraph: several stars backtrack correctly");
    expect(!glob_match("a*b*c", "axxbyy"), "procgraph: and still reject a non-match");
    expect(glob_match("exact", "exact"), "procgraph: a pattern with no wildcard is an exact match");
    expect(!glob_match("exact", "exactly"), "procgraph: which does not match a longer name");
    expect(glob_match("*", ""), "procgraph: a star matches an empty name");
}

COOPA_TEST(a_variable_binding_reads_the_table_and_falls_back_cleanly) {
    setup();
    Scene scene;
    Graph g = graph_with_input("season", types().float_t);
    g.inputs()[0].default_value = make_float(99.0f);
    Diagnostics compile_diag;
    CompiledGraph compiled = coopa::nodegraph::compile(g, GroupLoader(), {}, compile_diag);
    expect(compile_diag.ok(), "procgraph: compiles: " + compile_diag.to_string());

    VariableTable variables;
    variables.set("world.season", make_float(2.0f));

    Diagnostics diag;
    ResolveContext ctx;
    ctx.scene     = &scene;
    ctx.variables = &variables;
    ctx.out       = &diag;
    uint64_t hash = 0;

    std::unordered_map<std::string, InputBinding> bindings;
    bindings["season"] = binding("{ var: world.season }");
    const auto values = resolve_inputs(bindings, compiled, ctx, hash);
    expect_near(values[0].as<float>(-1.0f), 2.0f, 1e-5f,
                "procgraph: the variable was read -- the general escape hatch for anything a "
                "scene query cannot express");

    bindings["season"] = binding("{ var: not.set }");
    Diagnostics missing_diag;
    ctx.out = &missing_diag;
    const auto fallback = resolve_inputs(bindings, compiled, ctx, hash);
    expect(fallback[0].empty(),
           "procgraph: an unset variable resolves to nothing, so the evaluator uses the socket's "
           "declared default rather than a zero that looks like a real value");
    expect(missing_diag.ok(), "procgraph: and it warns rather than erroring");
}

COOPA_TEST(a_variable_of_a_convertible_type_is_converted_to_the_sockets_type) {
    setup();
    Scene scene;
    Graph g = graph_with_input("count", types().float_t);
    Diagnostics compile_diag;
    CompiledGraph compiled = coopa::nodegraph::compile(g, GroupLoader(), {}, compile_diag);
    expect(compile_diag.ok(), "procgraph: compiles: " + compile_diag.to_string());

    VariableTable variables;
    variables.set("n", make_int(5));   // An Int bound to a Float parameter.

    Diagnostics diag;
    ResolveContext ctx;
    ctx.scene     = &scene;
    ctx.variables = &variables;
    ctx.out       = &diag;
    uint64_t hash = 0;

    std::unordered_map<std::string, InputBinding> bindings;
    bindings["count"] = binding("{ var: n }");
    const auto values = resolve_inputs(bindings, compiled, ctx, hash);
    expect(values[0].type() == types().float_t,
           "procgraph: the resolved value was converted to the socket's type");
    expect_near(values[0].as<float>(-1.0f), 5.0f, 1e-5f,
                "procgraph: Int 5 became Float 5 -- binding a scalar to a wider parameter is "
                "ordinary and should not need an explicit conversion node");
}

COOPA_TEST(the_variable_table_reads_scalars_from_a_settings_mapping) {
    setup();
    VariableTable variables;
    variables.load_from_node(fkyaml::node::deserialize(
        "{ a_float: 1.5, an_int: 7, a_bool: true, a_string: hello, a_vector: [1, 2, 3], "
        "a_mapping: { nested: 1 } }"));

    expect_near(variables.get("a_float").as<float>(-1.0f), 1.5f, 1e-5f, "procgraph: a float");
    expect(variables.get("an_int").as<int32_t>(-1) == 7, "procgraph: an int");
    expect(variables.get("a_bool").as<bool>(false), "procgraph: a bool");
    expect(variables.get("a_string").as<std::string>("") == "hello", "procgraph: a string");
    expect_near(variables.get("a_vector").as<glm::vec3>(glm::vec3(0.0f)).z, 3.0f, 1e-5f,
                "procgraph: a three-element sequence becomes a Vector");
    expect(!variables.contains("a_mapping"),
           "procgraph: a nested mapping is skipped -- a variable is a scalar by design, and a "
           "structured one would need a type the graph could name");
}

COOPA_TEST(the_input_hash_moves_when_a_bound_object_moves_and_not_otherwise) {
    setup();
    Scene scene;
    SceneObject* marker = add_object(scene, "Marker", glm::vec3(0.0f));
    add_object(scene, "Unrelated", glm::vec3(0.0f));

    Graph g = graph_with_input("anchor", types().object_t);
    Diagnostics compile_diag;
    CompiledGraph compiled = coopa::nodegraph::compile(g, GroupLoader(), {}, compile_diag);
    expect(compile_diag.ok(), "procgraph: compiles: " + compile_diag.to_string());

    Diagnostics diag;
    ResolveContext ctx;
    ctx.scene = &scene;
    ctx.out   = &diag;

    std::unordered_map<std::string, InputBinding> bindings;
    bindings["anchor"] = binding("{ object: Marker }");

    uint64_t first = 0;
    resolve_inputs(bindings, compiled, ctx, first);

    uint64_t again = 0;
    resolve_inputs(bindings, compiled, ctx, again);
    expect(first == again,
           "procgraph: re-resolving an unchanged scene gives the SAME hash. This is what makes "
           "`bake: on_change` affordable -- without it every frame would re-bake");

    scene.find_object("Unrelated")->get_transform()->transform().set_position(
        glm::vec3(50.0f, 0.0f, 0.0f));
    uint64_t after_unrelated = 0;
    resolve_inputs(bindings, compiled, ctx, after_unrelated);
    expect(after_unrelated == first,
           "procgraph: moving an object the graph does NOT bind to leaves the hash alone, so an "
           "unrelated edit cannot rebuild a 200k-vertex mesh");

    marker->get_transform()->transform().set_position(glm::vec3(0.0f, 0.0f, 9.0f));
    uint64_t after_move = 0;
    resolve_inputs(bindings, compiled, ctx, after_move);
    expect(after_move != first,
           "procgraph: moving the BOUND object does move the hash, which is what triggers the "
           "re-bake");
}

COOPA_TEST(an_object_set_hash_follows_its_members_not_its_pointer) {
    setup();
    Scene scene;
    SceneObject* parent = add_object(scene, "Spawns", glm::vec3(0.0f));
    SceneObject* child_raw = add_child(*parent, "Spawn_0", glm::vec3(0.0f));
    scene.adopt(*parent);

    Graph g = graph_with_input("set", types().object_set_t);
    Diagnostics compile_diag;
    CompiledGraph compiled = coopa::nodegraph::compile(g, GroupLoader(), {}, compile_diag);
    expect(compile_diag.ok(), "procgraph: compiles: " + compile_diag.to_string());

    Diagnostics diag;
    ResolveContext ctx;
    ctx.scene = &scene;
    ctx.out   = &diag;
    std::unordered_map<std::string, InputBinding> bindings;
    bindings["set"] = binding("{ children_of: Spawns }");

    uint64_t first = 0;
    resolve_inputs(bindings, compiled, ctx, first);
    uint64_t again = 0;
    resolve_inputs(bindings, compiled, ctx, again);
    expect(first == again,
           "procgraph: an ObjectSet hashes its MEMBERS, not the pointer to the vector it was "
           "freshly built into. Hashing the pointer would report 'changed' on every resolve and "
           "an `on_change` graph would re-bake forever");

    child_raw->get_transform()->transform().set_position(glm::vec3(1.0f, 0.0f, 0.0f));
    uint64_t moved = 0;
    resolve_inputs(bindings, compiled, ctx, moved);
    expect(moved != first, "procgraph: and a member moving does change it");
}

COOPA_TEST(a_binding_into_the_graphs_own_output_is_detected) {
    setup();
    Scene scene;
    SceneObject* owner = add_object(scene, "Generator", glm::vec3(0.0f));
    SceneObject* output_raw = add_child(*owner, "Generated", glm::vec3(0.0f));
    add_child(*output_raw, "GeneratedChild", glm::vec3(0.0f));
    scene.adopt(*owner);

    std::unordered_map<std::string, InputBinding> safe;
    safe["a"] = binding("{ object: Generator }");
    expect(!bindings_reference_subtree(safe, scene, *output_raw),
           "procgraph: binding to the OWNER is fine -- it is not part of the output");

    std::unordered_map<std::string, InputBinding> cyclic;
    cyclic["a"] = binding("{ object: Generated }");
    expect(bindings_reference_subtree(cyclic, scene, *output_raw),
           "procgraph: binding to the graph's own output root is detected. A graph that reads what "
           "it writes would dirty itself every bake and never settle, so the system refuses it as "
           "a diagnostic rather than looping forever");

    std::unordered_map<std::string, InputBinding> nested_binding;
    nested_binding["a"] = binding("{ object: GeneratedChild }");
    expect(bindings_reference_subtree(nested_binding, scene, *output_raw),
           "procgraph: and so is a binding to anything DEEPER in the output subtree");

    std::unordered_map<std::string, InputBinding> glob;
    glob["a"] = binding("{ name_like: \"Generated*\" }");
    expect(bindings_reference_subtree(glob, scene, *output_raw),
           "procgraph: a glob that would sweep the output up is caught too, even though it names "
           "no object directly");
}

COOPA_TEST(an_unbound_interface_input_resolves_to_nothing_so_the_default_is_used) {
    setup();
    Scene scene;
    Graph g = graph_with_input("size", types().float_t);
    Diagnostics compile_diag;
    CompiledGraph compiled = coopa::nodegraph::compile(g, GroupLoader(), {}, compile_diag);
    expect(compile_diag.ok(), "procgraph: compiles: " + compile_diag.to_string());

    Diagnostics diag;
    ResolveContext ctx;
    ctx.scene = &scene;
    ctx.out   = &diag;
    uint64_t hash = 0;

    const std::unordered_map<std::string, InputBinding> nothing;
    const auto values = resolve_inputs(nothing, compiled, ctx, hash);
    expect(values.size() == 1, "procgraph: one value per interface input, bound or not");
    expect(values[0].empty(),
           "procgraph: an unbound input is empty, which the evaluator reads as 'use the declared "
           "default'");
    expect(diag.ok(), "procgraph: and an unbound input is not a complaint");
}

COOPA_TEST(a_literal_binding_is_parsed_through_the_sockets_own_yaml_hook) {
    setup();
    Scene scene;
    Graph g = graph_with_input("size", types().vector_t);
    Diagnostics compile_diag;
    CompiledGraph compiled = coopa::nodegraph::compile(g, GroupLoader(), {}, compile_diag);
    expect(compile_diag.ok(), "procgraph: compiles: " + compile_diag.to_string());

    Diagnostics diag;
    ResolveContext ctx;
    ctx.scene = &scene;
    ctx.out   = &diag;
    uint64_t hash = 0;

    std::unordered_map<std::string, InputBinding> bindings;
    bindings["size"] = binding("[2, 4, 6]");
    const auto values = resolve_inputs(bindings, compiled, ctx, hash);
    expect_near(values[0].as<glm::vec3>(glm::vec3(0.0f)).y, 4.0f, 1e-5f,
                "procgraph: a sequence literal was read by the Vector type's registered YAML hook");

    // A scalar broadcasts, matching the Float -> Vector implicit conversion.
    bindings["size"] = binding("3");
    const auto broadcast = resolve_inputs(bindings, compiled, ctx, hash);
    expect_near(broadcast[0].as<glm::vec3>(glm::vec3(0.0f)).z, 3.0f, 1e-5f,
                "procgraph: and a scalar broadcasts to every component, so `size: 3` works");
}
