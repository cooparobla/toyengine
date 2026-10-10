/**
 * @file paint_test.cpp
 * @brief Vertex Paint and Weight Paint dabs: falloff, blend modes, blur, front faces only, the
 * secondary colour, auto-normalize, the fills, and group removal renumbering.
 */

#include <coopa/testing/test.h>

#include "editor/mesh/paint.h"
#include "editor/mesh/primitives.h"
#include "editor/mesh/sculpt.h"
#include "editor/support/fixtures.h"

COOPA_TEST_SUITE("paint");

namespace toy::editor::testing {

/** @brief Vertex Paint and Weight Paint dabs: falloff, blend modes, blur, front faces only,
 *         auto-normalize, and the fills. */
COOPA_TEST(vertex_and_weight_paint_dabs) {
    EditMesh g = make_grid(16, 16, 2.0f);   // spacing 0.125, centre vertex at the origin
    SculptCache cache;
    cache.build(g);
    VertexGrid grid;
    grid.build(g, 0.5f);
    const uint32_t centre = 8 * 17 + 8, edge = 0;
    PaintSettings s;
    s.color = glm::vec4(1, 0, 0, 1);
    PaintDab d;
    d.center = glm::vec3(0.0f);
    d.radius = 0.5f;
    d.view_dir = glm::vec3(0, 0, -1);   // looking down at the +Z grid

    // Vertex Paint.
    std::vector<uint32_t> changed;
    expect(!g.has_colors, "a fresh mesh has no colour attribute");
    PaintDab back = d;
    back.view_dir = glm::vec3(0, 0, 1);
    vertex_paint_dab(g, cache, grid, s, back, changed);
    expect(changed.empty() && g.vertex_color(centre) == glm::vec4(1.0f), "front faces only: a dab from behind paints nothing");
    vertex_paint_dab(g, cache, grid, s, d, changed);
    expect(g.has_colors && !changed.empty(), "painting turns the colour attribute on");
    const glm::vec4 c0 = g.vertex_color(centre);
    expect(c0.r > 0.99f && c0.g < 0.01f, "Mix at full strength paints the centre the brush colour");
    expect(g.vertex_color(edge) == glm::vec4(1.0f), "...and leaves vertices outside the radius white");
    const uint32_t mid = 8 * 17 + 8 + 3;   // 0.375 from the centre: partly painted
    const glm::vec4 cm = g.vertex_color(mid);
    expect(cm.g > 0.01f && cm.g < 0.99f, "the falloff blends part-way toward the rim");
    s.tool = PaintTool::Blur;
    vertex_paint_dab(g, cache, grid, s, d, changed);
    expect(g.vertex_color(centre).g > c0.g, "Blur pulls the painted centre toward its neighbours");
    s.tool = PaintTool::Draw;
    s.blend = PaintBlend::Subtract;
    s.color = glm::vec4(1, 1, 1, 1);
    vertex_paint_dab(g, cache, grid, s, d, changed);
    expect(g.vertex_color(centre).r < 0.01f, "Subtract darkens");
    d.invert = true;
    s.blend = PaintBlend::Mix;
    s.secondary = glm::vec4(0, 0, 1, 1);
    vertex_paint_dab(g, cache, grid, s, d, changed);
    expect(g.vertex_color(centre).b > 0.99f, "Ctrl paints the secondary colour");
    d.invert = false;
    fill_vertex_colors(g, glm::vec4(0.25f, 0.5f, 0.75f, 1.0f));
    invert_vertex_colors(g);
    expect(glm::distance(g.vertex_color(edge), glm::vec4(0.75f, 0.5f, 0.25f, 1.0f)) < 1e-5f, "Fill then Invert");
    clear_vertex_colors(g);
    expect(!g.has_colors && !mesh_to_node(g).at("colors").as_seq().size(), "Clear removes the attribute from the file");

    // Weight Paint.
    const uint32_t a = g.group_index("Arm"), b = g.group_index("Body");
    PaintSettings w;
    w.weight = 1.0f;
    weight_paint_dab(g, cache, grid, w, a, d, changed);
    expect(g.weight(centre, a) > 0.99f && g.weight(edge, a) == 0.0f, "Draw lays weight 1 at the centre, none outside");
    d.invert = true;
    d.strength = 0.5f;   // the app fills the dab's strength from PaintSettings::strength
    weight_paint_dab(g, cache, grid, w, a, d, changed);
    expect(std::abs(g.weight(centre, a) - 0.5f) < 1e-3f, "Ctrl (Mix) takes weight away");
    d.invert = false;
    w.blend = PaintBlend::Add;
    w.weight = 0.2f;
    d.strength = 1.0f;
    weight_paint_dab(g, cache, grid, w, a, d, changed);
    expect(std::abs(g.weight(centre, a) - 0.7f) < 1e-3f, "Add adds the brush weight");
    assign_weight(g, b, 1.0f);
    w.blend = PaintBlend::Mix;
    w.weight = 0.8f;
    w.auto_normalize = true;
    weight_paint_dab(g, cache, grid, w, a, d, changed);
    expect(std::abs(g.weight(centre, a) + g.weight(centre, b) - 1.0f) < 1e-4f && g.weight(centre, a) > 0.79f,
           "Auto Normalize rescales the other groups so the weights sum to 1");
    expect(g.weight(edge, b) == 1.0f, "...only where the brush touched");
    w.auto_normalize = false;
    w.tool = PaintTool::Blur;
    const float before = g.weight(centre, a);
    weight_paint_dab(g, cache, grid, w, a, d, changed);
    expect(g.weight(centre, a) < before, "Blur softens the peak");
    normalize_all_weights(g);
    float sum = 0.0f;
    for (const auto& vw : g.weights[mid]) sum += vw.weight;
    expect(std::abs(sum - 1.0f) < 1e-4f, "Normalize All");
    g.remove_group(a);
    expect(g.groups.size() == 1 && g.groups[0] == "Body" && g.weight(centre, 0) > 0.0f, "removing a group renumbers the rest");
}

} // namespace toy::editor::testing
