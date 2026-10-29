/**
 * @file procgraph_section_test.cpp
 * @brief The ProceduralGraph Inputs drawer, driven with a scripted mouse and no device.
 *
 * The rows this builds come from a graph file rather than from a schema, so the usual
 * component_schema tests cannot reach them. What matters here is that dragging a value, picking
 * a binding kind and redrawing the result are all survivable -- the drawer both READS the
 * interface and WRITES the component block, and getting that interleaving wrong corrupts the
 * block rather than raising anything.
 */

#include <coopa/testing/test.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include <toyengine/procgraph/scene_input.h>

#include "editor/schema/inspector_sections.h"
#include "editor/support/imm_harness.h"

COOPA_TEST_SUITE("procgraph_section");

namespace toy::editor::testing {
namespace {

namespace fs = std::filesystem;

/// @brief One input of each shape the drawer treats differently: ranged float, int, object.
const char* k_graph = R"(name: probe
inputs:
  - id: size
    type: Float
    label: Size
    default: 1.0
    min: 0.2
    max: 3.0
  - id: posts
    type: Int
    label: Posts
    default: 6
    min: 2
    max: 40
  - id: target
    type: Object
    label: Target
outputs:
  - id: object
    type: ObjectSpec
nodes: []
links: []
)";

fs::path write_graph() {
    const fs::path dir = fs::temp_directory_path() / "coopa_tests" / "procgraph_section";
    fs::create_directories(dir / "graphs");
    const fs::path p = dir / "graphs" / "probe.yaml";
    std::ofstream(p) << k_graph;
    return p;
}

/// @brief An env with just what the drawer reads: where a graph ref resolves, and object names.
InspectorEnv env_for(const fs::path& graph) {
    // The editor has these from Engine's constructor; without them every binding kind is absent
    // and the Object rows below would never be exercised at all.
    procgraph::register_builtin_input_resolvers();
    InspectorEnv env;
    env.asset_path = [graph](const std::string&) { return graph.string(); };
    env.object_names = [] { return std::vector<std::string>{"RailStart", "RailEnd"}; };
    return env;
}

Node component() {
    Node comp = Node::mapping();
    comp["type"] = Node(std::string("ProceduralGraph"));
    comp["graph"] = Node(std::string("graphs/probe"));
    return comp;
}

} // namespace

/** @brief Drawing the rows does not need the component to already carry an `inputs` block. */
COOPA_TEST(the_drawer_builds_rows_from_the_graph_not_from_the_component) {
    register_builtin_sections();
    const SectionDrawFn* fn = find_inspector_section("ProceduralGraph.inputs");
    expect(fn != nullptr, "procgraph_section: the drawer is registered");

    Node comp = component();
    InspectorEnv env = env_for(write_graph());
    std::set<std::string> drawn;
    SectionContext sc{comp, env, 0, &drawn};
    ImmHarness h;
    h.frame([&](coopa::ui::imm::Context& c) { (*fn)(c, sc); });

    expect(drawn.count("inputs") == 1, "procgraph_section: it claims the key it draws");
}

/**
 * @brief Pressing and dragging anywhere over the rows does not crash, and leaves a readable block.
 *
 * A sweep rather than one aimed click: the rows are laid out by the drawer, so their exact y is
 * an implementation detail, and a crash that only happens over one widget is exactly the kind
 * this is for. Every press is followed by a drag and a release, which is how a value row is
 * actually edited.
 */
COOPA_TEST(dragging_over_every_row_survives) {
    register_builtin_sections();
    const SectionDrawFn* fn = find_inspector_section("ProceduralGraph.inputs");
    expect(fn != nullptr, "procgraph_section: the drawer is registered");

    const fs::path graph = write_graph();
    for (float y = 4.0f; y < 420.0f; y += 9.0f) {
        for (float x : {120.0f, 400.0f, 700.0f}) {
            Node comp = component();
            InspectorEnv env = env_for(graph);
            std::set<std::string> drawn;
            SectionContext sc{comp, env, 0, &drawn};
            auto draw = [&](coopa::ui::imm::Context& c) { (*fn)(c, sc); };

            ImmHarness h;
            h.mouse = {x, y};
            h.frame(draw);
            h.down = true;
            h.frame(draw);
            h.frame(draw, {}, {}, glm::vec2(25.0f, 0.0f));   // the drag
            h.frame(draw, {}, {}, glm::vec2(25.0f, 0.0f));
            h.down = false;
            h.frame(draw);
            h.frame(draw);   // and once more, against whatever the drag wrote

            expect(comp.contains("inputs") && comp.at("inputs").is_mapping(),
                   "procgraph_section: the block is still a mapping after a drag at " +
                       std::to_string(x) + "," + std::to_string(y));
        }
    }
}

/**
 * @brief Drawing the rows writes NOTHING to the component.
 *
 * An input the author never set is absent, and absent is not an error -- the evaluator falls
 * back to the interface default. Seeding on draw would dirty a scene just by selecting an
 * object, and for a bound-only type it would write an EMPTY binding, which the next bake
 * reports as "a `children_of:` binding needs an object name".
 */
COOPA_TEST(drawing_does_not_author_anything) {
    register_builtin_sections();
    const SectionDrawFn* fn = find_inspector_section("ProceduralGraph.inputs");

    Node comp = component();
    InspectorEnv env = env_for(write_graph());
    std::set<std::string> drawn;
    SectionContext sc{comp, env, 0, &drawn};
    ImmHarness h;
    h.frame([&](coopa::ui::imm::Context& c) { (*fn)(c, sc); });

    expect(!comp.at("inputs").contains("target"),
           "procgraph_section: an untouched Object input stays absent rather than being seeded "
           "with an empty binding that fails the next bake");
    expect(!comp.at("inputs").contains("size"),
           "procgraph_section: and an untouched value input is not written either");
}


/**
 * @brief Every binding-kind name the inspector offers is snake_case.
 *
 * The dropdown mixes one editor-owned entry (`value`, for "a number typed here") with the
 * engine's registered resolver kinds (`object`, `children_of`, ...). A capitalised entry among
 * them reads as a different kind of thing than it is, and names are snake_case everywhere in the
 * engine, editor and hub. The resolver kinds are checked too, so a new one cannot break the rule
 * from the engine side either.
 */
COOPA_TEST(every_binding_kind_name_is_snake_case) {
    procgraph::register_builtin_input_resolvers();

    std::vector<std::string> names = procgraph::input_resolver_kinds();
    expect(names.size() >= 5, "procgraph_section: the built-in resolvers are registered");
    names.push_back("value");   // the editor's own entry, beside them in the same dropdown

    for (const std::string& n : names) {
        expect(!n.empty(), "procgraph_section: no empty kind name");
        for (char c : n) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            expect(ok, "procgraph_section: '" + n + "' is snake_case -- lowercase, digits and "
                       "underscores only, like every other name in the engine");
        }
    }
}
} // namespace toy::editor::testing
