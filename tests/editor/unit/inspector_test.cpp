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

/** @brief The material panel draws its Shader dropdown once, at the top: draw_fields() leaves
 *         a skipped schema field to its caller instead of drawing it again further down. */
COOPA_TEST(the_material_block_draws_its_shader_dropdown_once) {
    ImmHarness h;
    InspectorEnv env;
    Node m = Node::mapping();
    m["shader"] = Node(std::string("triplanar"));
    const float params[4] = {1.0f, 4.0f, 0.0f, 1.0f};
    m["shader_params"] = make_float_seq(params, 4);
    m["albedo"] = make_color(glm::vec3(0.8f));
    auto height_of = [&](const std::function<void(coopa::ui::imm::Context&)>& draw) {
        float y0 = 0.0f, y1 = 0.0f;
        h.frame([&](coopa::ui::imm::Context& c) {
            c.begin_region("r", {0, 0, 400, 2000}, false);
            y0 = c.cursor().y;
            draw(c);
            y1 = c.cursor().y;
            c.end_region();
        });
        return y1 - y0;
    };
    const float empty = height_of([](auto&) {});   // the region's own offset, not a row
    auto rows = [&](const std::function<void(coopa::ui::imm::Context&)>& draw) { return height_of(draw) - empty; };
    std::vector<FieldDesc> shader_fields, rest;
    for (const auto& f : material_fields()) (f.key == "shader" || f.key == "shader_params" ? shader_fields : rest).push_back(f);
    expect(shader_fields.size() == 2, "the material schema lists shader and shader_params");

    const float skipped = rows([&](auto& c) { draw_fields(c, shader_fields, m, env, false, {"shader", "shader_params"}); });
    expect(skipped == 0.0f, "draw_fields() draws no row for a skipped schema field (" + std::to_string(skipped) + ")");

    const float section = rows([&](auto& c) { draw_shader_section(c, m); });
    const float space = rows([&](auto& c) { c.spacing(); });
    const float fields = rows([&](auto& c) { draw_fields(c, rest, m, env, false, {"base"}); });
    const float block = rows([&](auto& c) { draw_material_block(c, m, env); });
    expect(section > 0.0f && block == section + space + fields,
           "the material block is the Shader section plus the other fields, nothing more (" +
           std::to_string(block) + " vs " + std::to_string(section + space + fields) + ")");
}

} // namespace toy::editor::testing
