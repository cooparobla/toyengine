/**
 * @file cloth_paint_test.cpp
 * @brief Cloth Paint's pure ops -- dab tools and limits, fill, gradient, smooth, mirror,
 *        channel materialisation -- and the `cloth:` key's round trip through
 *        mesh_to_node/mesh_from_node (weld-safe, passthrough-safe).
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>

#include "editor/mesh/cloth_paint.h"
#include "editor/mesh/edit_mesh.h"
#include "editor/mesh/primitives.h"
#include "editor/mesh/sculpt.h"
#include "editor/support/fixtures.h"

COOPA_TEST_SUITE("cloth_paint");

namespace toy::editor::testing {

/** @brief Dab mechanics on the channels: materialisation (max_distance rides along), falloff,
 *         Draw / invert-erase, Average, Blur, and the AnimDrive 0..1 ceiling vs the metre
 *         channels' open range. */
COOPA_TEST(cloth_dab_tools_and_limits) {
    EditMesh g = make_grid(16, 16, 2.0f);   // spacing 0.125, centre vertex at the origin
    SculptCache cache;
    cache.build(g);
    VertexGrid grid;
    grid.build(g, 0.5f);
    const uint32_t centre = 8 * 17 + 8, corner = 0;
    PaintSettings s;
    PaintDab d;
    d.center = glm::vec3(0.0f);
    d.radius = 0.5f;
    d.strength = 1.0f;
    d.view_dir = glm::vec3(0, 0, -1);

    expect(!g.has_cloth(), "a fresh mesh has no cloth paint");
    std::vector<uint32_t> changed;
    cloth_paint_dab(g, cache, grid, s, ClothChannel::BackstopRadius, 0.1f, d, changed);
    expect(g.has_cloth(), "painting any channel materialises the cloth block (max_distance)");
    expect(g.cloth_max_distance.size() == g.positions.size() &&
               g.cloth_backstop_radius.size() == g.positions.size(),
           "materialised channels are parallel to positions");
    expect(!changed.empty() &&
               cloth_channel_value(g, ClothChannel::BackstopRadius, centre) > 0.099f,
           "a full-strength Mix dab lays the value down at the centre");
    expect(cloth_channel_value(g, ClothChannel::BackstopRadius, corner) == 0.0f,
           "the dab's falloff leaves the far corner untouched");

    // Draw on a metre channel is NOT capped at 1.
    changed.clear();
    cloth_paint_dab(g, cache, grid, s, ClothChannel::MaxDistance, 2.5f, d, changed);
    expect(cloth_channel_value(g, ClothChannel::MaxDistance, centre) > 2.49f,
           "metre channels accept values past 1");

    // AnimDrive IS capped at 1.
    changed.clear();
    cloth_paint_dab(g, cache, grid, s, ClothChannel::AnimDrive, 2.5f, d, changed);
    expect(cloth_channel_value(g, ClothChannel::AnimDrive, centre) <= 1.0f + 1e-6f,
           "anim_drive clamps to its 0..1 range");

    // Ctrl (invert) with Mix erases toward 0.
    PaintDab inv = d;
    inv.invert = true;
    changed.clear();
    cloth_paint_dab(g, cache, grid, s, ClothChannel::MaxDistance, 2.5f, inv, changed);
    expect(cloth_channel_value(g, ClothChannel::MaxDistance, centre) < 0.01f,
           "an inverted Mix dab erases toward zero");

    // Blur pulls a lone peak toward its (zero) neighbours.
    fill_cloth_channel(g, ClothChannel::MaxDistance, 0.0f);
    cloth_channel_data(g, ClothChannel::MaxDistance)[centre] = 1.0f;
    s.tool = PaintTool::Blur;
    changed.clear();
    cloth_paint_dab(g, cache, grid, s, ClothChannel::MaxDistance, 0.0f, d, changed);
    expect(cloth_channel_value(g, ClothChannel::MaxDistance, centre) < 1.0f,
           "blur pulls a peak toward its neighbours");
}

/** @brief Fill, gradient (the waistband-to-hem shortcut), smooth and mirror. */
COOPA_TEST(cloth_fill_gradient_smooth_mirror) {
    EditMesh g = make_grid(8, 8, 2.0f);   // 9x9 vertices over [-1, 1]^2
    SculptCache cache;
    cache.build(g);

    fill_cloth_channel(g, ClothChannel::MaxDistance, 0.3f);
    expect(cloth_channel_value(g, ClothChannel::MaxDistance, 0) == 0.3f &&
               cloth_channel_value(g, ClothChannel::MaxDistance, 40) == 0.3f,
           "fill writes every vertex");

    // Gradient along +X: 0 at x = -1, 0.5 at x = +1.
    gradient_cloth_channel(g, ClothChannel::MaxDistance, glm::vec3(-1, 0, 0), glm::vec3(1, 0, 0),
                           0.0f, 0.5f);
    float left = 1e9f, right = -1e9f;
    for (uint32_t v = 0; v < g.positions.size(); ++v) {
        const float x = cloth_channel_value(g, ClothChannel::MaxDistance, v);
        if (g.positions[v].x < -0.99f) left = std::min(left, x);
        if (g.positions[v].x > 0.99f) right = std::max(right, x);
    }
    expect(left < 0.01f && right > 0.49f, "the gradient runs lo at `from` to hi at `to`");

    // Mirror X: the gradient flips.
    mirror_cloth_channel(g, ClothChannel::MaxDistance, 0);
    for (uint32_t v = 0; v < g.positions.size(); ++v) {
        if (g.positions[v].x > 0.99f && g.positions[v].y == 0.0f) {
            expect(cloth_channel_value(g, ClothChannel::MaxDistance, v) < 0.01f,
                   "mirror X moved the low edge's values to the high edge");
            break;
        }
    }

    // Smooth flattens a re-applied gradient's spread.
    gradient_cloth_channel(g, ClothChannel::MaxDistance, glm::vec3(-1, 0, 0), glm::vec3(1, 0, 0),
                           0.0f, 0.5f);
    smooth_cloth_channel(g, cache, ClothChannel::MaxDistance, 1.0f, 8);
    float lo = 1e9f, hi = -1e9f;
    for (uint32_t v = 0; v < g.positions.size(); ++v) {
        const float x = cloth_channel_value(g, ClothChannel::MaxDistance, v);
        lo = std::min(lo, x);
        hi = std::max(hi, x);
    }
    expect(hi - lo < 0.5f, "smoothing narrows the channel's spread");

    clear_cloth_channels(g);
    expect(!g.has_cloth(), "clear removes the cloth block");
}

/** @brief The `cloth:` key round-trips through export + import: per-vertex values survive the
 *         corner dedup and the weld, optional channels stay optional, and add_vertex_mix /
 *         compact keep the arrays parallel. */
COOPA_TEST(cloth_channels_round_trip_and_track_topology) {
    EditMesh g = make_grid(4, 4, 2.0f);   // 5x5 vertices
    fill_cloth_channel(g, ClothChannel::MaxDistance, 0.2f);
    cloth_channel_data(g, ClothChannel::MaxDistance)[7] = 0.45f;
    fill_cloth_channel(g, ClothChannel::AnimDrive, 0.8f);
    expect(g.cloth_backstop_distance.empty(), "unpainted channels stay absent");

    const Node node = mesh_to_node(g);
    expect(node.contains("cloth"), "export writes the cloth key");
    const EditMesh back = mesh_from_node(node);
    expect(back.has_cloth() && back.positions.size() == g.positions.size(),
           "import welds back to the same vertex count with cloth intact");
    // Match by POSITION, not by index: export dedups corners, so the file's vertex order is
    // its own and nothing promises it matches the EditMesh's.
    bool values_match = back.cloth_anim_drive.size() == back.positions.size();
    uint32_t matched = 0;
    for (uint32_t bv = 0; values_match && bv < back.positions.size(); ++bv) {
        for (uint32_t gv = 0; gv < g.positions.size(); ++gv) {
            if (glm::length(back.positions[bv] - g.positions[gv]) > 1e-5f) continue;
            if (std::abs(cloth_channel_value(back, ClothChannel::MaxDistance, bv) -
                         cloth_channel_value(g, ClothChannel::MaxDistance, gv)) > 1e-4f) {
                values_match = false;
            }
            ++matched;
            break;
        }
    }
    expect(values_match && matched == back.positions.size(),
           "per-vertex max_distance survives the round trip, at each vertex's own position");
    expect(back.cloth_backstop_distance.empty() && back.cloth_backstop_radius.empty(),
           "absent channels stay absent through the round trip");

    // Topology ops keep the arrays parallel.
    EditMesh t = make_grid(2, 2, 1.0f);
    fill_cloth_channel(t, ClothChannel::MaxDistance, 0.4f);
    const uint32_t nv = t.add_vertex_lerp(glm::vec3(0.1f, 0.1f, 0.0f), 0, 1, 0.5f);
    expect(t.cloth_max_distance.size() == t.positions.size() &&
               std::abs(t.cloth_max_distance[nv] - 0.4f) < 1e-5f,
           "add_vertex_mix blends the cloth channels onto the new vertex");
}

} // namespace toy::editor::testing
