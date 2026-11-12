/**
 * @file inspector_test.cpp
 * @brief Inspector field drawing: int-coded enums show their labels but store the int, and the material
 * block draws its Shader dropdown once (a schema field skipped by draw_fields() is the caller's).
 */

#include <coopa/testing/test.h>

#include "editor/schema/inspector.h"
#include "editor/schema/settings_schema.h"
#include "editor/support/imm_harness.h"

COOPA_TEST_SUITE("inspector");

namespace toy::editor::testing {

/** @brief Int-coded modes (fog_mode) show names, and picking one still writes the int. */
COOPA_TEST(int_enums_show_labels_but_store_ints) {
    const FieldDesc* fog = nullptr;
    for (const auto& g : render_settings_groups()) for (const auto& f : g.fields) if (f.key == "fog_mode") fog = &f;
    expect(fog && fog->kind == FieldKind::Int && fog->options.size() == 2 && fog->option_tips.size() == 2,
           "fog_mode is a labelled int enum");
    expect(fog && fog->options[1] == "Exponential", "fog_mode 1 reads as Exponential");

    ImmHarness h;
    Node block = Node::mapping();
    block["fog_mode"] = Node(static_cast<int64_t>(1));
    InspectorEnv env;
    auto ui = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 400, 300}, false);
        draw_field(c, *fog, block, env);
        c.end_region();
    };
    h.frame(ui);
    expect(get_int(block, "fog_mode") == 1, "drawing does not change the value");
    float row_y = -1.0f;
    for (float y = 2.0f; y < 60.0f && row_y < 0.0f; y += 4.0f) {
        h.click({330, y}, ui);
        h.frame(ui);
        if (h.ctx.any_popup_open()) row_y = y;
    }
    expect(row_y >= 0.0f, "the fog mode row is a dropdown");
    // The list opens below the row with "Linear" first; pick it.
    for (float y = row_y + 4.0f; y < row_y + 120.0f && get_int(block, "fog_mode") == 1; y += 4.0f) {
        if (!h.ctx.any_popup_open()) { h.click({330, row_y}, ui); h.frame(ui); }
        h.click({330, y}, ui);
        h.frame(ui);
    }
    expect(block.at("fog_mode").is_scalar() && get_int(block, "fog_mode") == 0, "picking a mode writes its index");
    expect(get_string(block, "fog_mode") != "Linear", "the file still stores an int, not the label");
}

/**
 * @brief The material panel draws its Shader dropdown once, at the top.
 *
 * Two things could draw it twice: draw_fields() ignoring the caller's skip list, and a
 * material section listing `shader` among its own keys. Both are checked, because a second
 * row would not just be untidy -- it would write over what the first one wrote.
 */
COOPA_TEST(the_material_block_draws_its_shader_dropdown_once) {
    ImmHarness h;
    InspectorEnv env;
    Node m = Node::mapping();
    m["shader"] = Node(std::string("triplanar"));
    const float params[4] = {1.0f, 4.0f, 0.0f, 1.0f};
    m["shader_params"] = make_float_seq(params, 4);
    m["albedo"] = make_color(glm::vec3(0.8f));

    std::vector<FieldDesc> shader_fields;
    for (const auto& f : material_fields()) if (f.key == "shader" || f.key == "shader_params") shader_fields.push_back(f);
    expect(shader_fields.size() == 2, "the material schema lists shader and shader_params");

    float moved = 0.0f;
    h.frame([&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 400, 2000}, false);
        const float y0 = c.cursor().y;
        draw_fields(c, shader_fields, m, env, false, {"shader", "shader_params"});
        moved = c.cursor().y - y0;
        c.end_region();
    });
    expect(moved == 0.0f, "draw_fields() draws no row for a skipped schema field (" + std::to_string(moved) + ")");

    for (const auto& s : material_sections()) {
        for (const auto& k : s.keys) {
            expect(k != "shader" && k != "shader_params" && k != "shader_params_ext",
                   "no material section claims a shader key (" + s.title + "." + k + ")");
        }
    }
}

/** @brief Every material key is placed, once, so none ends up in "More" by accident. */
COOPA_TEST(the_material_sections_place_every_key_but_the_shader_ones) {
    std::set<std::string> placed;
    for (const auto& s : material_sections()) {
        for (const auto& k : s.keys) {
            expect(placed.insert(k).second, "material." + k + " is in exactly one section");
        }
        if (!s.toggle.empty()) placed.insert(s.toggle);
    }
    // The three the Shader section owns, and the colour-space map drawn under Maps.
    const std::set<std::string> drawn_elsewhere{"shader", "shader_params", "shader_params_ext"};
    for (const auto& f : material_fields()) {
        if (drawn_elsewhere.count(f.key)) continue;
        expect(placed.count(f.key) == 1, "material." + f.key + " has a section");
    }
    expect(material_schema().field("albedo") != nullptr, "the material schema finds its fields");
}

/**
 * @brief A sectioned component draws its sections, hides a row whose condition fails, and
 *        shows it the moment the condition holds -- writing the same key either way.
 */
COOPA_TEST(a_conditional_row_appears_only_when_its_condition_holds) {
    register_component_schema({"TestConditionalRows", "Gameplay",
        {
            f_enum("mode", {"planar", "flowing"}, true),
            f_float("wave_amplitude", 0.15f, 0.005f, 0.0f, 20.0f),
            shown_when(f_float("flow_speed", 1.0f, 0.01f, 0.0f, 50.0f), "mode", {"flowing"}),
        },
        true, true,
        {sec("Shape", "Mesh").says("What kind of water this is.").keys({"mode"}).done(),
         sec("Motion", "Play").keys({"wave_amplitude", "flow_speed"}).done()}});
    const ComponentSchema* schema = find_schema("TestConditionalRows");
    expect(schema && schema->sections.size() == 2, "the test schema has its layout");

    ImmHarness h;
    InspectorEnv env;
    Node comp = default_component("TestConditionalRows");
    auto height = [&] {
        float y0 = 0.0f, y1 = 0.0f;
        h.frame([&](coopa::ui::imm::Context& c) {
            c.begin_region("r", {0, 0, 400, 2000}, false);
            y0 = c.cursor().y;
            draw_component(c, comp, env);
            y1 = c.cursor().y;
            c.end_region();
        });
        return y1 - y0;
    };
    const float planar = height();
    expect(planar > 0.0f, "a sectioned component draws something");

    comp["mode"] = Node(std::string("flowing"));
    const float flowing = height();
    expect(flowing > planar, "switching mode reveals the flow row (" + std::to_string(planar) + " -> " +
                             std::to_string(flowing) + ")");

    comp["mode"] = Node(std::string("planar"));
    expect(height() == planar, "and switching back hides it again");
    expect(comp.contains("mode"), "the component still holds every key it was given");
}

/**
 * @brief A key no section lists is still shown: the trailing More block carries it, so adding
 *        a key to a schema and forgetting the layout cannot make it vanish.
 */
COOPA_TEST(a_key_no_section_lists_still_shows_under_more) {
    const std::vector<FieldDesc> fields = {f_float("placed", 1.0f, 0.01f, 0.0f, 10.0f, true),
                                           f_float("forgotten", 2.0f, 0.01f, 0.0f, 10.0f, true)};
    register_component_schema({"TestAllKeysPlaced", "Gameplay", fields, true, true,
                               {sec("Main").keys({"placed", "forgotten"}).done()}});
    register_component_schema({"TestUnplacedKey", "Gameplay", fields, true, true,
                               {sec("Main").keys({"placed"}).done()}});

    ImmHarness h;
    InspectorEnv env;
    auto height_of = [&](const char* type) {
        Node comp = default_component(type);
        float y0 = 0.0f, y1 = 0.0f;
        h.frame([&](coopa::ui::imm::Context& c) {
            c.begin_region("r", {0, 0, 400, 2000}, false);
            y0 = c.cursor().y;
            draw_component(c, comp, env);
            y1 = c.cursor().y;
            c.end_region();
        });
        return y1 - y0;
    };
    const float all_placed = height_of("TestAllKeysPlaced");
    const float one_loose = height_of("TestUnplacedKey");
    expect(all_placed > 0.0f, "a sectioned component draws its section");
    expect(one_loose > all_placed, "the unplaced key adds a More header (" + std::to_string(all_placed) + " -> " +
                                   std::to_string(one_loose) + ")");
}

} // namespace toy::editor::testing
