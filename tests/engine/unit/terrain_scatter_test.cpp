/**
 * @file terrain_scatter_test.cpp
 * @brief Foliage scatter over a heightmap terrain, device-free: placement is deterministic per
 *        chunk whatever the build order, honours layer / slope / height masks and sits on the
 *        surface; streaming keeps only chunks near the camera; edits rebuild only touched chunks.
 */

#include <coopa/testing/test.h>

#include <cmath>

#include <glm/glm.hpp>
#include <toyengine/particles/terrain_scatter.h>
#include <toyengine/world/heightmap_data.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("terrain_scatter");

using namespace toy::particles;
using namespace toy::world;

namespace {

/** @brief 128 m square, 32 m chunks; flat west half, a 45-degree ramp in the east half; layer 1
 *         painted on the north half. */
HeightmapData field() {
    HeightmapGeometry g;
    g.size = glm::vec2(128.0f);
    g.height = 100.0f;
    g.chunk_size = 32.0f;
    g.resolution = 129;
    g.splat_resolution = 64;
    HeightmapData d(g);
    for (int j = 0; j < d.ny(); ++j) {
        for (int i = 0; i < d.nx(); ++i) d.at(i, j) = i < 64 ? 0.0f : static_cast<float>(i - 64);
    }
    for (int j = 32; j < 64; ++j) {
        for (int i = 0; i < 64; ++i) {
            d.splat_at(0, i, j) = 0;
            d.splat_at(1, i, j) = 255;
        }
    }
    d.mark_all_dirty();
    return d;
}

TerrainScatterParams params() {
    TerrainScatterParams p;
    p.shape.density = 0.25f;
    p.seed = 42;
    p.shape.sink = 0.0f;
    return p;
}

} // namespace

/** @brief The same chunk grows the same instances, built alone or after its neighbours. */
COOPA_TEST(placement_is_deterministic) {
    const HeightmapData d = field();
    const HeightmapView v{&d, glm::vec3(10.0f, 20.0f, 5.0f)};
    const auto a = TerrainScatterCache::build_chunk(v, params(), 1, 0);
    (void)TerrainScatterCache::build_chunk(v, params(), 0, 0);
    const auto b = TerrainScatterCache::build_chunk(v, params(), 1, 0);
    expect(!a.matrices.empty() && a.matrices == b.matrices, "scatter: identical matrices");
    auto p2 = params();
    p2.seed = 43;
    expect(TerrainScatterCache::build_chunk(v, p2, 1, 0).matrices != a.matrices, "scatter: another seed differs");
    for (const glm::mat4& m : a.matrices) {
        const glm::vec3 pos(m[3]);
        expect_near(pos.z, v.height(pos), 1e-3f, "scatter: instances sit on the surface");
        expect(pos.x >= a.bounds_min.x && pos.z <= a.bounds_max.z, "scatter: inside the chunk's bounds");
    }
}

/** @brief Layer, slope and height masks cut where they should. */
COOPA_TEST(masks) {
    const HeightmapData d = field();
    const HeightmapView v{&d, glm::vec3(0.0f)};
    auto p = params();
    p.layer = 1;
    p.shape.min_weight = 0.5f;
    for (int cx = 0; cx < 2; ++cx) {
        for (int cy = 0; cy < 4; ++cy) {
            const auto c = TerrainScatterCache::build_chunk(v, p, cx, cy);
            for (const glm::mat4& m : c.matrices) expect(m[3].y >= 63.0f, "scatter: layer 1 only grows in the north");
            if (cy < 2) expect(c.matrices.empty(), "scatter: the unpainted south half is bare");
        }
    }
    p = params();
    p.shape.slope_deg = glm::vec2(0.0f, 20.0f);
    expect(TerrainScatterCache::build_chunk(v, p, 3, 1).matrices.empty(), "scatter: nothing on the 45-degree ramp");
    expect(!TerrainScatterCache::build_chunk(v, p, 0, 1).matrices.empty(), "scatter: the flat ground grows");
    p = params();
    p.shape.height = glm::vec2(10.0f, 30.0f);
    for (int cy = 0; cy < 4; ++cy) {
        for (const glm::mat4& m : TerrainScatterCache::build_chunk(v, p, 2, cy).matrices) {
            expect(m[3].z >= 10.0f - 1e-3f && m[3].z <= 30.0f + 1e-3f, "scatter: within the height band");
        }
    }
}

/** @brief Only chunks near the camera are held; an edit rebuilds only the chunk it touched. */
COOPA_TEST(streaming_and_rebuilds) {
    HeightmapData d = field();
    const HeightmapView v{&d, glm::vec3(0.0f)};
    TerrainScatterCache cache;
    const glm::vec3 cam(16.0f, 16.0f, 2.0f);
    for (int k = 0; k < 20; ++k) cache.update(v, params(), cam, 40.0f, 2);
    for (const auto& [key, c] : cache.chunks()) {
        expect(key.first <= 2 && key.second <= 2, "scatter: only chunks within reach are built");
    }
    expect(cache.chunks().count({0, 0}) == 1 && cache.chunks().count({3, 3}) == 0, "scatter: near built, far not");
    const auto before01 = cache.chunks().at({0, 1}).matrices;
    const auto rev00 = cache.chunks().at({0, 0}).revision;
    d.at(5, 5) = 3.0f;
    d.mark_height_dirty(IRect{5, 5, 6, 6});
    cache.update(v, params(), cam, 40.0f, 1);
    expect(cache.chunks().at({0, 0}).revision != rev00, "scatter: the edited chunk rebuilt");
    expect(cache.chunks().at({0, 1}).matrices == before01, "scatter: an untouched chunk kept");
    cache.update(v, params(), glm::vec3(120.0f, 120.0f, 0.0f), 40.0f, 2);
    expect(cache.chunks().count({0, 0}) == 0, "scatter: chunks far behind the camera are dropped");
}

/** @brief Thinning keeps everything near, nothing at the draw distance, and less in between. */
COOPA_TEST(thinning_by_distance) {
    using TS = TerrainScatterCache;
    expect(TS::thinned_count(1000, 10.0f, 100.0f, 0.6f) == 1000, "scatter: inside the fade start, all of them");
    expect(TS::thinned_count(1000, 100.0f, 100.0f, 0.6f) == 0, "scatter: none at the draw distance");
    const auto mid = TS::thinned_count(1000, 80.0f, 100.0f, 0.6f);
    expect(mid > 0 && mid < 1000, "scatter: part of them in the fade band (" + std::to_string(mid) + ")");
    expect(TS::thinned_count(1000, 90.0f, 100.0f, 0.6f) < mid, "scatter: fewer further out");
    expect(TS::thinned_count(1000, 99.0f, 100.0f, 1.0f) == 1000, "scatter: fade_start 1 never thins");
    expect(TS::thinned_count(1000, 5000.0f, 0.0f, 0.6f) == 1000, "scatter: no draw distance, no thinning");
}

/** @brief A chunk's instances are a stable shuffle: rebuilds give new build ids, same order. */
COOPA_TEST(shuffled_chunks_are_stable) {
    const HeightmapData d = field();
    const HeightmapView v{&d, glm::vec3(0.0f)};
    const auto a = TerrainScatterCache::build_chunk(v, params(), 0, 0);
    const auto b = TerrainScatterCache::build_chunk(v, params(), 0, 0);
    expect(a.matrices == b.matrices, "scatter: the shuffle is deterministic");
    expect(a.build_id != b.build_id, "scatter: every build gets a new upload version");
    // A prefix is spread over the whole chunk, not one band of rows.
    const std::size_t half = a.matrices.size() / 4;
    float lo = 1e9f, hi = -1e9f;
    for (std::size_t k = 0; k < half; ++k) { lo = std::min(lo, a.matrices[k][3].y); hi = std::max(hi, a.matrices[k][3].y); }
    expect(hi - lo > 24.0f, "scatter: the first quarter covers the chunk (" + std::to_string(hi - lo) + " m of 32)");
}
