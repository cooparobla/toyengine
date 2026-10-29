/**
 * @file inspector_sections_test.cpp
 * @brief The inspector's section layer: when a conditional row shows, that every section names
 * keys its schema really has and names each one once, that section icons resolve, and that the
 * custom-drawer registry hands back what was registered.
 *
 * Not here: how a section looks once drawn (inspector_test), nor whether a key the engine
 * parses exists at all (gpu engine_contract).
 */

#include <coopa/testing/test.h>

#include <set>
#include <string>
#include <vector>

#include "editor/schema/inspector_sections.h"
#include "editor/support/imm_harness.h"

COOPA_TEST_SUITE("inspector_sections");

namespace toy::editor::testing {

namespace {

ShowWhen when(std::string key, std::vector<std::string> values, bool negate = false) {
    ShowWhen w;
    w.key = std::move(key);
    w.equals = std::move(values);
    w.negate = negate;
    return w;
}

} // namespace

/** @brief A condition reads the block's value, and the schema default when the key is absent. */
COOPA_TEST(conditions_read_the_value_or_the_schema_default) {
    const std::vector<FieldDesc> fields = {
        f_enum("mode", {"planar", "flowing"}),
        f_bool("collide", true),
        f_int_enum("fog_mode", {"Linear", "Exponential"}, 1),
        f_float("amount", 0.25f),
    };
    Node block = Node::mapping();

    // Nothing written yet: every value is the schema's own default.
    expect(condition_value(block, "mode", fields) == "planar", "an enum defaults to its first option");
    expect(condition_value(block, "collide", fields) == "true", "a bool default reads as true/false");
    expect(condition_value(block, "fog_mode", fields) == "1", "an int-coded enum reads as its index");
    expect(condition_value(block, "amount", fields) == "0.25", "a float reads as its number");
    expect(condition_value(block, "nothing", fields).empty(), "an unknown key reads as empty");

    block["mode"] = Node(std::string("flowing"));
    block["collide"] = Node(false);
    block["fog_mode"] = Node(static_cast<int64_t>(0));
    expect(condition_value(block, "mode", fields) == "flowing", "a written enum wins over the default");
    expect(condition_value(block, "collide", fields) == "false", "...and a written bool");
    expect(condition_value(block, "fog_mode", fields) == "0", "...and a written int");
}

/** @brief equals / negate / always, and the case-insensitive match enums need. */
COOPA_TEST(show_when_decides_whether_a_row_is_drawn) {
    const std::vector<FieldDesc> fields = {f_enum("mode", {"planar", "flowing"}), f_bool("refraction", false)};
    Node block = Node::mapping();

    expect(show_when_holds(block, ShowWhen{}, fields), "an empty condition always holds");
    expect(show_when_holds(block, when("mode", {"planar"}), fields), "the default value satisfies a condition on it");
    expect(!show_when_holds(block, when("mode", {"flowing"}), fields), "...and fails one it does not match");
    expect(show_when_holds(block, when("mode", {"flowing"}, true), fields), "negate inverts the match");
    expect(show_when_holds(block, when("mode", {"flowing", "planar"}), fields), "any of the listed values holds");
    expect(show_when_holds(block, when("mode", {"PLANAR"}), fields), "an enum matches whatever its case");
    expect(!show_when_holds(block, when("refraction", {"true"}), fields), "a bool off hides its rows");
    block["refraction"] = Node(true);
    expect(show_when_holds(block, when("refraction", {"true"}), fields), "...and switching it on shows them");
}

/**
 * @brief Every section must name keys its own schema has, each at most once.
 *
 * A typo'd key is silently skipped when drawn -- the row just never appears -- so it has to be
 * caught here. A key in two sections would draw twice and write the second one's value.
 */
COOPA_TEST(every_section_names_real_keys_exactly_once) {
    size_t sectioned = 0;
    for (const auto& [type, schema] : schemas()) {
        if (schema.sections.empty()) continue;
        ++sectioned;
        std::set<std::string> seen;
        for (const auto& s : schema.sections) {
            expect(!s.title.empty(), type + " has a section with no title");
            if (!s.icon.empty()) {
                expect(coopa::ui::imm::icon_from_name(s.icon) != coopa::ui::imm::Icon::None,
                       type + " section '" + s.title + "' names a real icon (" + s.icon + ")");
            }
            if (!s.toggle.empty()) {
                const FieldDesc* t = schema.field(s.toggle);
                expect(t && t->kind == FieldKind::Bool,
                       type + " section '" + s.title + "' toggles a bool field (" + s.toggle + ")");
            }
            if (!s.show.key.empty()) {
                expect(schema.field(s.show.key) != nullptr,
                       type + " section '" + s.title + "' is conditional on a real key (" + s.show.key + ")");
            }
            for (const auto& h : s.subheads) {
                expect(h.index <= s.keys.size(), type + " section '" + s.title + "' sub-heading is in range");
                if (!h.show.key.empty()) {
                    expect(schema.field(h.show.key) != nullptr,
                           type + " sub-heading '" + h.title + "' is conditional on a real key");
                }
            }
            for (const auto& k : s.keys) {
                expect(schema.field(k) != nullptr, type + " section '" + s.title + "' names a real key (" + k + ")");
                expect(seen.insert(k).second, type + "." + k + " is in exactly one section");
            }
        }
        for (const auto& f : schema.fields) {
            if (f.show.key.empty()) continue;
            expect(schema.field(f.show.key) != nullptr, type + "." + f.key + " is conditional on a real key");
        }
    }
    expect(sectioned > 0, "at least one component has an inspector layout");
}

/** @brief A drawer is found by the id it was registered under, and re-registering replaces it. */
COOPA_TEST(the_registry_hands_back_the_drawer_registered_for_an_id) {
    expect(find_inspector_section("TestSectionNobodyRegistered") == nullptr, "an unregistered id has no drawer");
    expect(find_inspector_section("") == nullptr, "a section that names no drawer has none");

    int calls = 0;
    register_inspector_section("TestSection.rows", [&calls](imm::Context&, SectionContext& sc) {
        ++calls;
        sc.handled("speed");
        EditResult r;
        r.key = "speed";
        return r;
    });
    const SectionDrawFn* fn = find_inspector_section("TestSection.rows");
    expect(fn != nullptr, "a registered drawer is found by its id");

    Node comp = Node::mapping();
    std::set<std::string> drawn;
    InspectorEnv env;
    SectionContext sc{comp, env, 0, &drawn};
    ImmHarness h;
    EditResult r;
    h.frame([&](coopa::ui::imm::Context& c) { r = (*fn)(c, sc); });
    expect(calls == 1 && r.key == "speed", "...and it runs");
    expect(drawn.count("speed") == 1, "handled() tells the section not to draw the key again");

    register_inspector_section("TestSection.rows", [](imm::Context&, SectionContext&) { return EditResult{}; });
    expect(find_inspector_section("TestSection.rows") != nullptr, "re-registering an id replaces its drawer");
}


/**
 * @brief The ProceduralGraph Inputs section finds its drawer, and the drawer claims `inputs`.
 *
 * Two mistakes this catches, both of which leave an Inputs section that is simply EMPTY with no
 * error anywhere: registering the drawer under an id the section does not name, and a section
 * whose `custom` id does not match the registration -- `draw_section()` looks up `s.custom`
 * verbatim, so the id in `component_sections.cpp` has to be the full "Type.section" spelling.
 * When the drawer does not run, `inputs` also stays unclaimed and reappears under *More*.
 */
COOPA_TEST(the_procedural_graph_inputs_section_is_wired_to_its_drawer) {
    register_builtin_sections();

    const SectionDrawFn* fn = find_inspector_section("ProceduralGraph.inputs");
    expect(fn != nullptr,
           "the ProceduralGraph inputs drawer is registered under the id its section names");

    const ComponentSchema* schema = find_schema("ProceduralGraph");
    expect(schema != nullptr, "ProceduralGraph has a schema");
    bool named = false;
    for (const auto& sec : schema->sections) {
        if (sec.custom == "ProceduralGraph.inputs") named = true;
    }
    expect(named,
           "and a section names that exact id -- draw_section() looks it up verbatim, so a bare "
           "\"inputs\" would find nothing and draw an empty section");

    // With no graph chosen there are no rows to draw, but the key must still be claimed, or the
    // raw `inputs` mapping shows up again under the generic More block.
    Node comp = Node::mapping();
    std::set<std::string> drawn;
    InspectorEnv env;
    SectionContext sc{comp, env, 0, &drawn};
    ImmHarness h;
    h.frame([&](coopa::ui::imm::Context& c) { (*fn)(c, sc); });
    expect(drawn.count("inputs") == 1,
           "the drawer claims `inputs`, so it is not listed twice");
}

} // namespace toy::editor::testing
