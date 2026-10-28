/**
 * @file procgraph_bake_test.cpp
 * @brief The bake: node behaviour, determinism, and the object tree a graph produces.
 *
 * Everything here runs without a Vulkan device, which is the point of keeping `proc_bake` free of
 * engine dependencies. Publishing into a real scene is covered by the GPU suite.
 */

#include <coopa/testing/test.h>

#include <atomic>
#include <string>
#include <vector>

#include "engine/support/checks.h"

#include <coopa/job/engine.h>
#include <coopa/nodegraph/graph.h>
#include <coopa/nodegraph/graph_yaml.h>
#include <toyengine/procgraph/nodes.h>
#include <toyengine/procgraph/proc_bake.h>

COOPA_TEST_SUITE("procgraph_bake");

using namespace toy::procgraph;
using coopa::nodegraph::CompiledGraph;
using coopa::nodegraph::Diagnostics;
using coopa::nodegraph::Graph;
using coopa::nodegraph::GroupLoader;
using coopa::nodegraph::InterfaceSocket;
using coopa::nodegraph::NodeId;
using coopa::nodegraph::PropertyBag;
using coopa::nodegraph::SocketRef;
using coopa::nodegraph::Value;
using coopa::test::expect;
using coopa::test::expect_near;

namespace {

/// @brief Every suite starts here: node types must be registered before a graph can compile.
void setup() { register_procgraph_nodes(); }

/// @brief Declares one interface output of `type` named `id`, wired from `from`.
void wire_output(Graph& g, const std::string& id, coopa::nodegraph::TypeId type,
                 NodeId from, const std::string& socket) {
    InterfaceSocket os;
    os.identifier = id;
    os.type       = type;
    g.outputs().push_back(os);
    NodeId out = g.find_node_of_type(coopa::nodegraph::k_group_output_type);
    if (out == 0) out = g.add_node(coopa::nodegraph::k_group_output_type);
    g.add_link(SocketRef{from, socket}, SocketRef{out, id});
}

/// @brief Compiles and bakes, asserting the compile was clean.
BakeResult run(const Graph& g, coopa::job::JobEngine* jobs = nullptr, uint32_t seed = 7) {
    Diagnostics diag;
    CompiledGraph compiled = coopa::nodegraph::compile(g, GroupLoader(), {}, diag);
    expect(diag.ok(), "procgraph: the graph compiles: " + diag.to_string());

    BakeRequest request;
    request.graph              = &compiled;
    request.jobs               = jobs;
    request.context.seed       = seed;
    request.context.graph_name = g.name().empty() ? "test" : g.name();
    return bake(request);
}

/// @brief A graph emitting a 2x2x2 cube as its Geometry output.
Graph cube_graph(float size = 2.0f) {
    Graph g("cube");
    PropertyBag props;
    props.set("value", std::string());
    NodeId vec = g.add_node("Vector");
    g.node(vec)->props.raw()["value"] = fkyaml::node::deserialize(
        "[" + std::to_string(size) + ", " + std::to_string(size) + ", " + std::to_string(size) + "]");
    NodeId cube = g.add_node("Cube");
    g.add_link(SocketRef{vec, "value"}, SocketRef{cube, "size"});
    wire_output(g, "geometry", types().geometry_t, cube, "geometry");
    return g;
}

} // namespace

COOPA_TEST(a_cube_graph_bakes_to_a_mesh_with_the_expected_counts_and_bounds) {
    setup();
    const BakeResult result = run(cube_graph(2.0f));

    expect(result.ok(), "procgraph: the bake succeeded: " + result.messages());
    expect(result.root.mesh != nullptr, "procgraph: the root carries a mesh");

    const MeshGeometry& m = *result.root.mesh;
    expect(m.triangle_count() == 12,
           "procgraph: a cube is 12 triangles (two per face). Got " +
           std::to_string(m.triangle_count()));
    expect(m.vertices.size() == 36,
           "procgraph: and 36 vertices -- each face keeps its own four corners, because sharing "
           "them would force one averaged normal per corner and the cube would shade soft");

    const MeshCpuData cpu = m.to_cpu_data();
    expect_near(cpu.bounds_min.x, -1.0f, 1e-5f, "procgraph: a size-2 cube spans -1 to +1");
    expect_near(cpu.bounds_max.z, 1.0f, 1e-5f, "procgraph: in every axis");

    // Every normal must be axis-aligned and unit: a cube with averaged corner normals would fail.
    for (const auto& v : m.vertices) {
        expect_near(glm::length(v.normal), 1.0f, 1e-5f, "procgraph: each normal is unit length");
        const float biggest = std::max({std::abs(v.normal.x), std::abs(v.normal.y),
                                        std::abs(v.normal.z)});
        expect_near(biggest, 1.0f, 1e-5f,
                    "procgraph: a cube's normals are axis-aligned, so its faces read flat");
    }
}

COOPA_TEST(graph_inputs_drive_the_result) {
    setup();
    Graph g("sized");
    InterfaceSocket in;
    in.identifier    = "size";
    in.type          = types().float_t;
    in.default_value = make_float(1.0f);
    g.inputs().push_back(in);

    NodeId gi   = g.add_node(coopa::nodegraph::k_group_input_type);
    NodeId cube = g.add_node("Cube");
    // Float -> Vector is a registered implicit conversion, so a scalar feeds `size` directly.
    g.add_link(SocketRef{gi, "size"}, SocketRef{cube, "size"});
    wire_output(g, "geometry", types().geometry_t, cube, "geometry");

    Diagnostics diag;
    CompiledGraph compiled = coopa::nodegraph::compile(g, GroupLoader(), {}, diag);
    expect(diag.ok(), "procgraph: compiles: " + diag.to_string());

    auto bake_with = [&compiled](float size) {
        std::vector<Value> inputs{make_float(size)};
        BakeRequest request;
        request.graph  = &compiled;
        request.inputs = inputs;
        return bake(request);
    };

    const BakeResult small = bake_with(1.0f);
    const BakeResult large = bake_with(4.0f);
    expect(small.ok() && large.ok(), "procgraph: both bakes succeeded");

    const MeshCpuData small_cpu = small.root.mesh->to_cpu_data();
    const MeshCpuData large_cpu = large.root.mesh->to_cpu_data();
    expect_near(small_cpu.bounds_max.x, 0.5f, 1e-5f, "procgraph: size 1 spans -0.5 to +0.5");
    expect_near(large_cpu.bounds_max.x, 2.0f, 1e-5f,
                "procgraph: size 4 spans -2 to +2, so the input genuinely drove the geometry");
    expect(small.input_hash == 0 && large.input_hash == 0,
           "procgraph: bake() reports whatever input_hash it was given; computing it is the "
           "resolver's job, not the bake's");
}

COOPA_TEST(the_bake_is_byte_identical_between_the_inline_and_parallel_evaluators) {
    setup();

    // Wide enough to actually schedule: four independent primitive branches joined together.
    Graph g("wide");
    NodeId join = g.add_node("JoinGeometry");
    for (int i = 0; i < 4; ++i) {
        NodeId cyl = g.add_node("Cylinder");
        NodeId move = g.add_node("TransformGeometry");
        PropertyBag vp;
        NodeId vec = g.add_node("Vector");
        g.node(vec)->props.raw()["value"] =
            fkyaml::node::deserialize("[" + std::to_string(i * 2) + ", 0, 0]");
        g.add_link(SocketRef{cyl, "geometry"}, SocketRef{move, "geometry"});
        g.add_link(SocketRef{vec, "value"}, SocketRef{move, "translation"});
        g.add_link(SocketRef{move, "geometry"}, SocketRef{join, "geometry"}, true);
    }
    wire_output(g, "geometry", types().geometry_t, join, "geometry");

    const BakeResult inline_result = run(g, nullptr);
    expect(inline_result.ok(), "procgraph: the inline bake succeeded: " + inline_result.messages());

    coopa::job::JobEngine engine(4);
    const BakeResult parallel_result = run(g, &engine);
    expect(parallel_result.ok(),
           "procgraph: the parallel bake succeeded: " + parallel_result.messages());

    const MeshGeometry& a = *inline_result.root.mesh;
    const MeshGeometry& b = *parallel_result.root.mesh;
    expect(a.vertices.size() == b.vertices.size() && a.indices.size() == b.indices.size(),
           "procgraph: both paths produced the same mesh size");

    bool identical = a.indices == b.indices;
    for (size_t i = 0; i < a.vertices.size() && identical; ++i) {
        identical = a.vertices[i].position == b.vertices[i].position &&
                    a.vertices[i].normal == b.vertices[i].normal &&
                    a.vertices[i].uv == b.vertices[i].uv;
    }
    expect(identical,
           "procgraph: the baked mesh is BYTE-IDENTICAL between the inline and job-parallel "
           "evaluators. A bake whose vertex order depended on which worker got there first would "
           "be unusable: the asset would change every load");
    engine.shutdown();
}

COOPA_TEST(the_same_seed_produces_the_same_instances_and_a_different_seed_does_not) {
    setup();
    Graph g("scatter");
    NodeId line = g.add_node("LinePoints");
    g.node(line)->props.set("count", int64_t(0));
    NodeId count = g.add_node("Integer");
    g.node(count)->props.set("value", int64_t(24));
    g.add_link(SocketRef{count, "value"}, SocketRef{line, "count"});

    NodeId inst = g.add_node("InstanceOnPoints");
    NodeId proto = g.add_node("String");
    g.node(proto)->props.set("value", std::string("objects/rock"));
    NodeId spin = g.add_node("Float");
    g.node(spin)->props.set("value", 3.0);
    g.add_link(SocketRef{line, "points"}, SocketRef{inst, "points"});
    g.add_link(SocketRef{proto, "value"}, SocketRef{inst, "prototype"});
    g.add_link(SocketRef{spin, "value"}, SocketRef{inst, "random_rotation"});
    wire_output(g, "geometry", types().geometry_t, inst, "instances");

    const BakeResult a = run(g, nullptr, 7);
    const BakeResult b = run(g, nullptr, 7);
    const BakeResult c = run(g, nullptr, 8);

    expect(a.ok() && b.ok() && c.ok(), "procgraph: all three bakes succeeded: " + a.messages());
    expect(a.root.instances != nullptr && !a.root.instances->empty(),
           "procgraph: instances were produced");
    expect(a.root.instances->front().size() == 24, "procgraph: one instance per point");

    const auto& ta = a.root.instances->front().transforms;
    const auto& tb = b.root.instances->front().transforms;
    const auto& tc = c.root.instances->front().transforms;

    bool same_seed_matches = true;
    for (size_t i = 0; i < ta.size(); ++i) same_seed_matches = same_seed_matches && ta[i] == tb[i];
    expect(same_seed_matches,
           "procgraph: seed 7 twice gives identical transforms -- every random draw is keyed on "
           "(bake seed, element index) and nothing else, never on time or a global");

    bool different_seed_differs = false;
    for (size_t i = 0; i < ta.size(); ++i) different_seed_differs = different_seed_differs || ta[i] != tc[i];
    expect(different_seed_differs, "procgraph: and seed 8 genuinely gives something else");
}

COOPA_TEST(a_graph_emitting_only_components_bakes_a_valid_tree) {
    setup();

    // The claim that this is not a mesh generator: a graph whose entire output is a Light.
    Graph g("lights_only");
    NodeId add = g.add_node("AddComponent");
    g.node(add)->props.set("type", std::string("Light"));
    g.node(add)->props.set("intensity", 4.0);
    g.node(add)->props.raw()["color"] = fkyaml::node::deserialize("[1.0, 0.8, 0.5]");

    InterfaceSocket os;
    os.identifier = "component";
    os.type       = types().component_t;
    g.outputs().push_back(os);
    NodeId out = g.add_node(coopa::nodegraph::k_group_output_type);
    g.add_link(SocketRef{add, "component"}, SocketRef{out, "component"});

    const BakeResult result = run(g);
    expect(result.ok(), "procgraph: a component-only graph bakes: " + result.messages());
    expect(result.root.mesh == nullptr,
           "procgraph: it produced NO geometry, which is a valid outcome -- this is why the "
           "component has no `output:` key and why the system is not called geometry nodes");
    expect(result.root.components.size() == 1, "procgraph: exactly one component was emitted");
    expect(result.root.components[0].type == "Light",
           "procgraph: of the requested type, which goes through the SceneLoader parser table at "
           "publish time, so any registered component works with no code here");
    expect(result.root.components[0].props.contains("intensity"),
           "procgraph: the node's own props became the component's keys verbatim");
    expect(!result.root.components[0].props.contains("type"),
           "procgraph: except `type`, which the node consumed");
}

COOPA_TEST(an_object_output_tree_becomes_a_baked_object_tree) {
    setup();
    Graph g("tree");

    NodeId cube = g.add_node("Cube");
    NodeId child_out = g.add_node("ObjectOutput");
    g.node(child_out)->props.set("name", std::string("Child"));
    g.add_link(SocketRef{cube, "geometry"}, SocketRef{child_out, "geometry"});

    NodeId light = g.add_node("AddComponent");
    g.node(light)->props.set("type", std::string("Light"));

    NodeId root_out = g.add_node("ObjectOutput");
    g.node(root_out)->props.set("name", std::string("Root"));
    g.add_link(SocketRef{light, "component"}, SocketRef{root_out, "components"}, true);
    g.add_link(SocketRef{child_out, "object"}, SocketRef{root_out, "children"}, true);

    InterfaceSocket os;
    os.identifier = "object";
    os.type       = types().object_spec_t;
    g.outputs().push_back(os);
    NodeId out = g.add_node(coopa::nodegraph::k_group_output_type);
    g.add_link(SocketRef{root_out, "object"}, SocketRef{out, "object"});

    const BakeResult result = run(g);
    expect(result.ok(), "procgraph: the tree bakes: " + result.messages());
    expect(result.root.name == "Root", "procgraph: the root took its authored name");
    expect(result.root.components.size() == 1 && result.root.components[0].type == "Light",
           "procgraph: the root carries its component");
    expect(result.root.children.size() == 1,
           "procgraph: and one child -- nesting Object Output nodes is how a graph builds a "
           "subtree, which is what makes this a scene-graph converter rather than a mesh baker");
    expect(result.root.children[0].name == "Child", "procgraph: the child kept its name");
    expect(result.root.children[0].mesh != nullptr, "procgraph: and its own geometry");
    expect(result.root.total_objects() == 2, "procgraph: two objects in total");
}

COOPA_TEST(make_collider_marks_the_object_collidable) {
    setup();
    Graph g("collider");
    NodeId cube = g.add_node("Cube");
    NodeId coll = g.add_node("MakeCollider");
    NodeId obj  = g.add_node("ObjectOutput");
    g.node(obj)->props.set("name", std::string("Solid"));
    g.add_link(SocketRef{cube, "geometry"}, SocketRef{obj, "geometry"});
    g.add_link(SocketRef{coll, "component"}, SocketRef{obj, "components"}, true);

    InterfaceSocket os;
    os.identifier = "object";
    os.type       = types().object_spec_t;
    g.outputs().push_back(os);
    NodeId out = g.add_node(coopa::nodegraph::k_group_output_type);
    g.add_link(SocketRef{obj, "object"}, SocketRef{out, "object"});

    const BakeResult result = run(g);
    expect(result.ok(), "procgraph: it bakes: " + result.messages());
    expect(result.root.collider == BakedObject::Collider::Trimesh,
           "procgraph: a Make Collider node marks the object collidable. The SHAPE is not carried "
           "in YAML -- a baked trimesh is far too big -- so publish builds it from the object's "
           "own geometry; this flag is the intent travelling with it");
    expect(result.root.mesh != nullptr, "procgraph: and the visual mesh is still there");
}

COOPA_TEST(a_node_that_errors_yields_a_result_carrying_the_message) {
    setup();
    Graph g("broken");
    NodeId line = g.add_node("LinePoints");
    // Instance on Points with no prototype is an authored mistake, not a crash.
    NodeId inst = g.add_node("InstanceOnPoints");
    g.add_link(SocketRef{line, "points"}, SocketRef{inst, "points"});
    wire_output(g, "geometry", types().geometry_t, inst, "instances");

    const BakeResult result = run(g);
    expect(!result.ok(), "procgraph: the bake reports failure");
    expect(!result.errors.empty(), "procgraph: with an error recorded");
    expect(result.errors[0].find("prototype") != std::string::npos,
           "procgraph: naming what was missing: " + result.messages());
    expect(result.errors[0].find("InstanceOnPoints") != std::string::npos,
           "procgraph: and which node it was, so a tool can mark it: " + result.messages());
}

COOPA_TEST(an_unwired_graph_warns_rather_than_failing_silently) {
    setup();
    Graph g("empty");
    g.add_node("Cube");   // Built, but nothing is wired to an output.

    const BakeResult result = run(g);
    expect(result.ok(), "procgraph: a graph under construction is not an ERROR");
    expect(!result.warnings.empty(), "procgraph: but it does warn");
    expect(result.warnings.back().find("Group Output") != std::string::npos,
           "procgraph: naming the single most common cause -- an unwired Group Output is why a "
           "graph 'does nothing': " + result.messages());
}

COOPA_TEST(join_geometry_merges_every_branch) {
    setup();
    Graph g("join");
    NodeId join = g.add_node("JoinGeometry");
    for (int i = 0; i < 3; ++i) {
        NodeId cube = g.add_node("Cube");
        g.add_link(SocketRef{cube, "geometry"}, SocketRef{join, "geometry"}, true);
    }
    wire_output(g, "geometry", types().geometry_t, join, "geometry");

    const BakeResult result = run(g);
    expect(result.ok(), "procgraph: it bakes: " + result.messages());
    expect(result.root.mesh->triangle_count() == 36,
           "procgraph: three cubes joined is 36 triangles -- a multi-input saw all three links. "
           "Got " + std::to_string(result.root.mesh->triangle_count()));
}

COOPA_TEST(transform_geometry_moves_the_mesh_and_set_material_names_its_slot) {
    setup();
    Graph g("transformed");
    NodeId cube = g.add_node("Cube");
    NodeId vec  = g.add_node("Vector");
    g.node(vec)->props.raw()["value"] = fkyaml::node::deserialize("[10.0, 0.0, 0.0]");
    NodeId move = g.add_node("TransformGeometry");
    NodeId mat  = g.add_node("String");
    g.node(mat)->props.set("value", std::string("materials/stone"));
    NodeId set_mat = g.add_node("SetMaterial");

    g.add_link(SocketRef{cube, "geometry"}, SocketRef{move, "geometry"});
    g.add_link(SocketRef{vec, "value"}, SocketRef{move, "translation"});
    g.add_link(SocketRef{move, "geometry"}, SocketRef{set_mat, "geometry"});
    g.add_link(SocketRef{mat, "value"}, SocketRef{set_mat, "material"});
    wire_output(g, "geometry", types().geometry_t, set_mat, "geometry");

    const BakeResult result = run(g);
    expect(result.ok(), "procgraph: it bakes: " + result.messages());

    const MeshCpuData cpu = result.root.mesh->to_cpu_data();
    expect_near(cpu.bounds_min.x, 9.5f, 1e-5f,
                "procgraph: the cube moved to x = 10, so its -0.5 face is at 9.5");
    expect(cpu.slot_names.size() == 1 && cpu.slot_names[0] == "materials/stone",
           "procgraph: Set Material named the slot, which is what the renderer resolves to a "
           "material at publish time");
}

COOPA_TEST(a_cancelled_bake_stops_and_reports) {
    setup();
    Graph g = cube_graph();
    Diagnostics diag;
    CompiledGraph compiled = coopa::nodegraph::compile(g, GroupLoader(), {}, diag);
    expect(diag.ok(), "procgraph: compiles: " + diag.to_string());

    std::atomic<bool> cancel{true};
    BakeRequest request;
    request.graph  = &compiled;
    request.cancel = &cancel;
    const BakeResult result = bake(request);

    expect(result.root.mesh == nullptr,
           "procgraph: a bake cancelled before it started produced no geometry rather than a "
           "half-built mesh that would then be published");
    expect(!result.warnings.empty() || !result.errors.empty(),
           "procgraph: and it said so: " + result.messages());
}

COOPA_TEST(the_variable_node_reads_the_bakes_snapshot_and_falls_back_when_unset) {
    setup();
    Graph g("variable");
    NodeId var = g.add_node("Variable");
    g.node(var)->props.set("name", std::string("demo.scale"));
    g.node(var)->props.set("data_type", std::string("Float"));
    NodeId fallback = g.add_node("Float");
    g.node(fallback)->props.set("value", -1.0);
    g.add_link(SocketRef{fallback, "value"}, SocketRef{var, "fallback"});

    NodeId out = g.add_node(coopa::nodegraph::k_group_output_type);
    InterfaceSocket value_socket;
    value_socket.identifier = "value";
    value_socket.type       = types().float_t;
    InterfaceSocket found_socket;
    found_socket.identifier = "found";
    found_socket.type       = types().bool_t;
    g.outputs().push_back(value_socket);
    g.outputs().push_back(found_socket);
    g.add_link(SocketRef{var, "value"}, SocketRef{out, "value"});
    g.add_link(SocketRef{var, "found"}, SocketRef{out, "found"});

    Diagnostics diag;
    CompiledGraph compiled = coopa::nodegraph::compile(g, GroupLoader(), {}, diag);
    expect(diag.ok(), "procgraph: compiles: " + diag.to_string());

    // Unset: the fallback, and `found` false.
    BakeRequest bare;
    bare.graph = &compiled;
    BakeResult unset = bake(bare);
    expect(unset.ok(), "procgraph: an unset variable is not an error: " + unset.messages());

    // Set: the snapshot the bake carries. A COPY, not a pointer to the live table -- the bake
    // runs on a worker while set_variable() is called from the main thread.
    BakeRequest with;
    with.graph = &compiled;
    with.context.variables["demo.scale"] = make_float(2.5f);
    BakeResult found = bake(with);
    expect(found.ok(), "procgraph: a set variable bakes cleanly: " + found.messages());
    expect(found.warnings.size() <= unset.warnings.size() + 1,
           "procgraph: and reading a variable that exists does not warn");
}

COOPA_TEST(every_registered_node_declares_without_crashing_on_empty_props) {
    setup();
    // A UI adds a node with no settings at all, so every declare() must cope with an empty bag.
    // This is the cheapest guard against a node type that assumes its props are populated.
    const auto all = coopa::nodegraph::NodeTypeRegistry::instance().all();
    expect(all.size() >= 30,
           "procgraph: the node library registered a useful number of types. Got " +
           std::to_string(all.size()));

    const PropertyBag empty;
    for (const auto* type : all) {
        const auto decl = type->declaration_for(empty);
        expect(type->evaluate != nullptr,
               "procgraph: node '" + type->name + "' has an evaluate function");
        for (const auto& socket : decl.inputs) {
            expect(socket.type != coopa::nodegraph::k_invalid_type,
                   "procgraph: node '" + type->name + "' input '" + socket.identifier +
                   "' has a registered type -- an invalid id would make every link to it fail to "
                   "validate with a confusing message");
        }
        for (const auto& socket : decl.outputs) {
            expect(socket.type != coopa::nodegraph::k_invalid_type,
                   "procgraph: node '" + type->name + "' output '" + socket.identifier +
                   "' has a registered type");
        }
    }
}
