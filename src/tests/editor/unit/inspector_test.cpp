/**
 * @file inspector_test.cpp
 * @brief Inspector field drawing: int-coded enums show their labels but store the int, and the material
 * block draws its Shader dropdown once (a schema field skipped by draw_fields() is the caller's).
 */

#include <coopa/testing/test.h>

#include <string>
#include <vector>

#include "editor/schema/inspector.h"
#include "editor/schema/item_list.h"
#include "editor/schema/settings_schema.h"
#include "editor/support/imm_harness.h"

COOPA_TEST_SUITE("inspector");

namespace toy::editor::testing {

using imm_box_t = coopa::ui::imm::Box;

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

// ---------------------------------------------------------------------------------------
// FieldKind::ItemList: a per-item foldout with a reorder grip, and an Add that defaults.
// ---------------------------------------------------------------------------------------

namespace {

/** @brief A two-key item schema, shaped like Vehicle `wheels` (named by a text field). */
FieldDesc wheels_list() {
    return with_item_noun(f_items("wheels", {f_string("object"), f_float("radius", 0.35f, 0.01f, 0.0f, 10.0f)}),
                          "Wheel");
}

Node list_of(const std::vector<std::string>& objects) {
    Node seq = Node::sequence();
    for (const auto& o : objects) {
        Node m = Node::mapping();
        m["object"] = Node(o);
        m["radius"] = 0.35f;
        seq.as_seq().push_back(m);
    }
    return seq;
}

std::string nth_object(const Node& block, size_t i) {
    return block.at("wheels").as_seq()[i].at("object").get_value<std::string>();
}

} // namespace

COOPA_TEST(a_collapsed_item_list_draws_one_header_per_item_and_no_rows) {
    ImmHarness h;
    const FieldDesc list = collapsed_items(wheels_list());
    Node block = Node::mapping();
    block["wheels"] = list_of({"wheel_fl", "wheel_fr"});
    InspectorEnv env;
    float used = 0.0f;
    auto ui = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 400, 400}, false);
        draw_field(c, list, block, env);
        used = c.cursor().y;
        c.end_region();
    };
    h.frame(ui);
    const float collapsed_h = used;

    const FieldDesc open_list = wheels_list();   // the same list, items open
    float open_h = 0.0f;
    auto ui2 = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r2", {0, 0, 400, 400}, false);
        draw_field(c, open_list, block, env);
        open_h = c.cursor().y;
        c.end_region();
    };
    h.frame(ui2);
    expect(open_h > collapsed_h + 20.0f,
           "closed items hide their rows (" + std::to_string(collapsed_h) + " vs " + std::to_string(open_h) + ")");
    expect(nth_object(block, 0) == "wheel_fl" && nth_object(block, 1) == "wheel_fr",
           "and drawing changed nothing");
}

/**
 * @brief The same grip drag on two lists: one reorderable, one fixed_order.
 *
 * Written as one differential case on purpose. Asserting only that a fixed_order list does
 * not move proves nothing on its own -- a press that misses the grip entirely would pass it
 * too -- so the gesture has to be shown working on the list that should move.
 */
COOPA_TEST(a_grip_drag_reorders_a_list_and_a_fixed_order_list_ignores_it) {
    InspectorEnv env;

    // Where the drawer puts the second item's grip: the region insets by `padding`, the
    // list's caption takes one row, and each item header is row_height + 3 plus the gap.
    auto gesture = [&](const FieldDesc& f) {
        ImmHarness h;
        Node block = Node::mapping();
        block["wheels"] = list_of({"a", "b", "c"});
        auto ui = [&](coopa::ui::imm::Context& c) {
            c.begin_region("r", {0, 0, 400, 400}, false);
            draw_field(c, f, block, env);
            c.end_region();
        };
        h.frame(ui);
        const coopa::ui::imm::Style& st = h.ctx.style;
        const float pad = st.padding;
        const float header_h = st.row_height + 3.0f;
        const float pitch = header_h + st.spacing;
        const float caption = st.row_height + st.spacing;
        const float second_top = pad + caption + pitch;

        h.mouse = {pad + 2.0f + (st.row_height - 2.0f) * 0.5f, second_top + header_h * 0.5f};
        h.frame(ui);
        h.down = true;
        h.frame(ui);
        h.mouse.y -= pitch;             // one item of travel, upwards
        h.frame(ui);
        h.down = false;
        h.frame(ui);
        return nth_object(block, 0) + nth_object(block, 1) + nth_object(block, 2);
    };

    expect(gesture(collapsed_items(wheels_list())) == "bac",
           "dragging the second item's grip up one item swaps it with the first (got " +
               gesture(collapsed_items(wheels_list())) + ")");
    expect(gesture(fixed_order(collapsed_items(wheels_list()))) == "abc",
           "the same drag on a fixed_order list moves nothing (got " +
               gesture(fixed_order(collapsed_items(wheels_list()))) + ")");
}

COOPA_TEST(add_appends_an_item_at_its_defaults_and_stops_at_the_cap) {
    ImmHarness h;
    const FieldDesc list = capped(collapsed_items(wheels_list()), 2);
    Node block = Node::mapping();
    block["wheels"] = list_of({"a"});
    InspectorEnv env;
    imm_box_t add{};
    auto ui = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 400, 400}, false);
        draw_field(c, list, block, env);
        add = c.last_rect();
        c.end_region();
    };
    h.frame(ui);
    h.click(add.center(), ui);
    expect(block.at("wheels").size() == 2, "Add appends one item");
    const Node& fresh = block.at("wheels").as_seq()[1];
    expect(fresh.at("radius").get_value<double>() > 0.34 && fresh.at("radius").get_value<double>() < 0.36,
           "the new item's float is at its schema default, not 0");

    h.frame(ui);
    h.click(add.center(), ui);
    expect(block.at("wheels").size() == 2, "and Add does nothing once the cap is reached");
}

COOPA_TEST(an_empty_item_list_says_so_once) {
    ImmHarness h;
    const FieldDesc list = wheels_list();
    Node block = Node::mapping();
    InspectorEnv env;
    auto ui = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 400, 400}, false);
        draw_field(c, list, block, env);
        c.end_region();
    };
    h.frame(ui);
    expect(block.contains("wheels") && block.at("wheels").is_sequence() && block.at("wheels").size() == 0,
           "an absent list is created empty, not filled");
    expect(item_list_empty_text(list) == "No wheels yet.", "and the empty wording is the shared one");
}

COOPA_TEST(a_list_item_marks_its_override_with_the_lists_own_key) {
    ImmHarness h;
    const FieldDesc list = wheels_list();   // items open, so sub-rows draw
    Node block = Node::mapping();
    block["wheels"] = list_of({"a"});
    InspectorEnv env;
    std::vector<std::string> marked;
    env.after_field = [&](coopa::ui::imm::Context&, const std::string& key, const imm_box_t&) {
        marked.push_back(key);
    };
    auto ui = [&](coopa::ui::imm::Context& c) {
        marked.clear();
        c.begin_region("r", {0, 0, 400, 400}, false);
        draw_field(c, list, block, env);
        c.end_region();
    };
    h.frame(ui);
    expect(!marked.empty(), "a field inside a list reaches after_field at all");
    for (const std::string& k : marked) {
        expect(k == "wheels", "and is marked with the list's key, not the item's (got '" + k + "')");
    }
}

} // namespace toy::editor::testing
