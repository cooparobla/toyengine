/**
 * @file heightmap_data_test.cpp
 * @brief The heightmap terrain's CPU data, device-free: sampling, normals, resampling and height
 *        rescale on a geometry change, chunk ranges, the edit journal several consumers follow,
 *        the surface raycast, splat normalisation and the deterministic generator.
 */

#include <coopa/testing/test.h>

#include <cmath>

#include <glm/glm.hpp>
#include <toyengine/world/heightmap_data.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("heightmap_data");

using toy::world::HeightmapData;
using toy::world::HeightmapGenerate;
using toy::world::HeightmapGeometry;
using toy::world::IRect;

namespace {

/** @brief A 9 x 9 grid over 8 x 8 m (1 m cells), 4 m chunks, heights = 0.5 * x + 0.25 * y. */
HeightmapData sloped() {
    HeightmapGeometry g;
    g.size = glm::vec2(8.0f);
    g.height = 20.0f;
    g.chunk_size = 4.0f;
    g.resolution = 9;
    g.splat_resolution = 8;
    HeightmapData d(g);
    for (int j = 0; j < d.ny(); ++j) {
        for (int i = 0; i < d.nx(); ++i) d.at(i, j) = 0.5f * static_cast<float>(i) + 0.25f * static_cast<float>(j);
    }
    d.mark_all_dirty();
    return d;
}

} // namespace

/** @brief Bilinear sampling reproduces a plane exactly, between texels too, and clamps outside. */
COOPA_TEST(sampling_reproduces_a_plane) {
    const HeightmapData d = sloped();
    expect_near(d.sample_height(glm::vec2(2.0f, 4.0f)), 2.0f, 1e-5f, "heightmap: on a texel");
    expect_near(d.sample_height(glm::vec2(2.5f, 3.5f)), 1.25f + 0.875f, 1e-5f, "heightmap: between texels");
    expect_near(d.sample_height(glm::vec2(-5.0f, 0.0f)), 0.0f, 1e-5f, "heightmap: clamped west of the grid");
    expect_near(d.sample_height(glm::vec2(8.0f, 8.0f)), 4.0f + 2.0f, 1e-5f, "heightmap: the far corner");
    const glm::vec3 n = d.normal(glm::vec2(4.0f, 4.0f));
    const glm::vec3 want = glm::normalize(glm::vec3(-0.5f, -0.25f, 1.0f));
    expect(glm::length(n - want) < 1e-4f, "heightmap: the normal of a plane is the plane's normal");
}

/** @brief A geometry change keeps the shape: corners survive a resample, heights scale with height. */
COOPA_TEST(geometry_change_resamples_and_rescales) {
    HeightmapData d = sloped();
    HeightmapGeometry g = d.geometry();
    g.resolution = 17;
    d.set_geometry(g);
    expect(d.nx() == 17 && d.ny() == 17, "heightmap: resampled to 17 x 17");
    expect_near(d.at(16, 16), 6.0f, 1e-4f, "heightmap: far corner kept");
    expect_near(d.at(8, 0), 2.0f, 1e-4f, "heightmap: an old texel's value at its new position");
    g.height = 40.0f;
    d.set_geometry(g);
    expect_near(d.at(16, 16), 12.0f, 1e-4f, "heightmap: doubling the height doubles every sample");
    g.size = glm::vec2(16.0f);
    d.set_geometry(g);
    expect_near(d.sample_height(glm::vec2(16.0f, 16.0f)), 12.0f, 1e-4f, "heightmap: a larger size stretches, same samples");
}

/** @brief Chunk ranges include their shared border texels; chunk counts round up. */
COOPA_TEST(chunk_ranges) {
    const HeightmapData d = sloped();
    expect(d.geometry().chunk_count() == glm::ivec2(2, 2), "heightmap: 8 m in 4 m chunks is 2 x 2");
    const glm::vec2 mm = d.chunk_minmax(1, 1);
    expect_near(mm.x, 0.5f * 4.0f + 0.25f * 4.0f, 1e-5f, "heightmap: chunk (1,1) starts on the shared border");
    expect_near(mm.y, 6.0f, 1e-5f, "heightmap: ...and reaches the far corner");
    HeightmapGeometry g = d.geometry();
    g.chunk_size = 3.0f;
    expect(g.chunk_count() == glm::ivec2(3, 3), "heightmap: 8 m in 3 m chunks rounds up to 3");
}

/** @brief Edits are journalled: every consumer gets the union since ITS revision; a reshape is whole. */
COOPA_TEST(edit_journal_serves_many_consumers) {
    HeightmapData d = sloped();
    const auto a = d.revision();
    d.mark_height_dirty(IRect{1, 1, 3, 3});
    const auto b = d.revision();
    d.mark_height_dirty(IRect{5, 5, 6, 7});
    expect(d.height_dirty_since(b) == IRect{1, 1, 6, 7} || d.height_dirty_since(b) == IRect{5, 5, 6, 7},
           "heightmap: since b covers the second edit");
    const IRect since_a = d.height_dirty_since(a);
    expect(since_a.x0 <= 1 && since_a.y0 <= 1 && since_a.x1 >= 6 && since_a.y1 >= 7, "heightmap: since a covers both edits");
    expect(d.height_dirty_since(d.revision()).empty(), "heightmap: nothing since now");
    expect(d.chunk_revision(0, 0) > a && d.chunk_revision(1, 1) > a, "heightmap: touched chunks bumped");
    const auto c = d.revision();
    d.mark_all_dirty();
    expect(d.height_dirty_since(c) == IRect{0, 0, 9, 9}, "heightmap: a whole-grid change is the whole grid");
    expect(d.dirty_on_disk(), "heightmap: edits are unsaved");
}

/** @brief The surface raycast finds the plane from above and misses when aimed away. */
COOPA_TEST(raycast_hits_the_surface) {
    const HeightmapData d = sloped();
    float t = 0.0f;
    const glm::vec3 o(3.3f, 2.7f, 50.0f);
    expect(d.raycast(o, glm::vec3(0.0f, 0.0f, -1.0f), 100.0f, t), "heightmap: straight down hits");
    expect_near(o.z - t, d.sample_height(glm::vec2(3.3f, 2.7f)), 1e-3f, "heightmap: ...at the surface height");
    expect(!d.raycast(o, glm::vec3(0.0f, 0.0f, 1.0f), 100.0f, t), "heightmap: straight up misses");
    const glm::vec3 slant_dir = glm::normalize(glm::vec3(1.0f, 0.5f, -1.0f));
    expect(d.raycast(glm::vec3(0.5f, 0.5f, 6.0f), slant_dir, 100.0f, t), "heightmap: a slanted ray hits");
    const glm::vec3 p = glm::vec3(0.5f, 0.5f, 6.0f) + slant_dir * t;
    expect_near(p.z, d.sample_height(glm::vec2(p)), 1e-3f, "heightmap: ...on the surface");
}

/** @brief A splat texel renormalises to 255, keeping the layer being painted. */
COOPA_TEST(splat_normalisation) {
    HeightmapData d = sloped();
    d.splat_at(0, 2, 2) = 200;
    d.splat_at(3, 2, 2) = 200;
    d.splat_at(5, 2, 2) = 100;
    d.normalize_splat_texel(2, 2, 3);
    int sum = 0;
    for (int l = 0; l < 8; ++l) sum += d.splat_at(l, 2, 2);
    expect(sum == 255, "heightmap: weights sum to 255");
    expect(d.splat_at(3, 2, 2) == 200, "heightmap: the kept layer is untouched");
    expect_near(d.layer_weight(3, glm::vec2(2.5f, 2.5f)), 200.0f / 255.0f, 0.02f, "heightmap: layer_weight reads it back");
}

/** @brief The generator is deterministic per seed and stays inside [0, height]. */
COOPA_TEST(generator_is_deterministic) {
    HeightmapGeometry g;
    g.size = glm::vec2(256.0f);
    g.resolution = 65;
    g.height = 50.0f;
    HeightmapData a(g), b(g), c(g);
    HeightmapGenerate p;
    p.seed = 7;
    a.generate(p);
    b.generate(p);
    p.seed = 8;
    c.generate(p);
    expect(a.heights() == b.heights(), "heightmap: same seed, same grid");
    expect(a.heights() != c.heights(), "heightmap: different seed, different grid");
    const glm::vec2 r = a.height_range();
    expect(r.x >= 0.0f && r.y <= 50.0f && r.y - r.x > 5.0f, "heightmap: within [0, height] with real relief");
}
