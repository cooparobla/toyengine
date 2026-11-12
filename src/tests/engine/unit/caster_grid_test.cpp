/**
 * @file caster_grid_test.cpp
 * @brief toyengine/render/caster_grid.h: the XY culling grid every per-view cull queries.
 *        The contract under test is the superset guarantee -- for any frustum, candidates
 *        plus the caller's exact per-item test select exactly the items a flat loop over
 *        everything would. Randomized bounds against a brute-force reference; no device.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <toyengine/render/caster_grid.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("caster_grid");

namespace {

using namespace toy::render;

/// Deterministic LCG so a failure reproduces; returns floats in [lo, hi).
struct Lcg {
    uint64_t state = 0x853c49e6748fea9bull;
    float next(float lo, float hi) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        const float u = static_cast<float>((state >> 33) & 0xFFFFFF) / static_cast<float>(0x1000000);
        return lo + u * (hi - lo);
    }
};

/// A spread of item sizes: chunk-sized boxes, props, and a few cell-spanning "large" items.
void make_items(Lcg& rng, size_t count, std::vector<WorldBounds>& bounds,
                std::vector<uint8_t>& valid) {
    bounds.clear();
    valid.clear();
    for (size_t i = 0; i < count; ++i) {
        WorldBounds b;
        b.center = glm::vec3(rng.next(-500.0f, 500.0f), rng.next(-500.0f, 500.0f),
                             rng.next(-20.0f, 60.0f));
        const float kind = rng.next(0.0f, 1.0f);
        const float e = kind > 0.95f ? rng.next(40.0f, 200.0f)   // large: terrain plane scale
                      : kind > 0.6f  ? rng.next(8.0f, 16.0f)     // chunk scale
                                     : rng.next(0.2f, 3.0f);     // prop scale
        b.extent = glm::vec3(e, e * rng.next(0.5f, 1.0f), rng.next(0.5f, e));
        bounds.push_back(b);
        valid.push_back(rng.next(0.0f, 1.0f) > 0.1f ? 1 : 0);   // some LOD-culled items
    }
}

/// The exact membership a flat loop produces for frustum f.
std::vector<uint32_t> brute_force(const Frustum& f, const std::vector<WorldBounds>& bounds,
                                  const std::vector<uint8_t>& valid) {
    std::vector<uint32_t> hits;
    for (size_t i = 0; i < bounds.size(); ++i) {
        if (valid[i] && f.intersects(bounds[i])) hits.push_back(static_cast<uint32_t>(i));
    }
    return hits;
}

/// Grid candidates filtered by the same exact test, sorted for comparison.
std::vector<uint32_t> via_grid(const CasterGrid& grid, const Frustum& f, const XyRect& rect,
                               const std::vector<WorldBounds>& bounds,
                               const std::vector<uint8_t>& valid) {
    std::vector<uint32_t> hits;
    grid.for_candidates(f, rect, [&](uint32_t i) {
        if (valid[i] && f.intersects(bounds[i])) hits.push_back(i);
    });
    std::sort(hits.begin(), hits.end());
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
    return hits;
}

} // namespace

COOPA_TEST(grid_membership_matches_brute_force_for_random_views) {
    Lcg rng;
    std::vector<WorldBounds> bounds;
    std::vector<uint8_t> valid;
    make_items(rng, 800, bounds, valid);
    CasterGrid grid;
    grid.rebuild(bounds, valid);

    for (int trial = 0; trial < 40; ++trial) {
        const glm::vec3 eye(rng.next(-400.0f, 400.0f), rng.next(-400.0f, 400.0f),
                            rng.next(5.0f, 80.0f));
        const glm::vec3 at = eye + glm::vec3(rng.next(-1.0f, 1.0f), rng.next(-1.0f, 1.0f),
                                             rng.next(-1.0f, -0.2f));
        const glm::mat4 view = glm::lookAt(eye, at, glm::vec3(0, 0, 1));
        glm::mat4 proj;
        if (trial % 2 == 0) {
            proj = glm::perspective(glm::radians(rng.next(35.0f, 90.0f)), 16.0f / 9.0f, 0.1f,
                                    rng.next(50.0f, 1200.0f));
        } else {
            const float ext = rng.next(10.0f, 300.0f);   // ortho: a shadow-cascade-style box
            proj = glm::ortho(-ext, ext, -ext, ext, 0.0f, rng.next(100.0f, 2000.0f));
        }
        const glm::mat4 vp = proj * view;
        const Frustum f = Frustum::from_matrix(vp);
        const auto expected = brute_force(f, bounds, valid);
        const auto actual   = via_grid(grid, f, view_proj_xy_rect(vp), bounds, valid);
        expect(actual == expected, "grid + exact test selects what the flat loop selects");
    }
}

COOPA_TEST(grid_handles_degenerate_and_unbounded_footprints) {
    Lcg rng;
    std::vector<WorldBounds> bounds;
    std::vector<uint8_t> valid;
    make_items(rng, 200, bounds, valid);
    CasterGrid grid;
    grid.rebuild(bounds, valid);

    // A singular matrix footprint is unbounded: the query degrades to every occupied cell and
    // must still return the full superset rather than guessing.
    const glm::mat4 singular(0.0f);
    const XyRect rect = view_proj_xy_rect(singular);
    expect(rect.min.x < -1.0e30f && rect.max.x > 1.0e30f, "singular view-proj reads as unbounded");

    const glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 40), glm::vec3(0), glm::vec3(0, 1, 0));
    const glm::mat4 proj = glm::ortho(-600.0f, 600.0f, -600.0f, 600.0f, 0.0f, 200.0f);
    const Frustum f = Frustum::from_matrix(proj * view);
    const auto expected = brute_force(f, bounds, valid);
    const auto actual   = via_grid(grid, f, rect, bounds, valid);
    expect(actual == expected, "unbounded footprint still yields exact membership");
}

COOPA_TEST(grid_files_every_valid_item_exactly_once) {
    Lcg rng;
    std::vector<WorldBounds> bounds;
    std::vector<uint8_t> valid;
    make_items(rng, 300, bounds, valid);
    CasterGrid grid;
    grid.rebuild(bounds, valid);

    // An all-encompassing ortho view must surface every valid item exactly once (centre-filed
    // cells and the large list partition the set).
    const glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 5000), glm::vec3(0), glm::vec3(0, 1, 0));
    const glm::mat4 proj = glm::ortho(-5000.0f, 5000.0f, -5000.0f, 5000.0f, 0.0f, 10000.0f);
    const glm::mat4 vp = proj * view;
    std::vector<uint32_t> seen;
    grid.for_candidates(Frustum::from_matrix(vp), view_proj_xy_rect(vp),
                        [&](uint32_t i) { seen.push_back(i); });
    std::sort(seen.begin(), seen.end());
    expect(std::adjacent_find(seen.begin(), seen.end()) == seen.end(),
           "no item is filed in two places");
    size_t valid_count = 0;
    for (const uint8_t v : valid) valid_count += v;
    expect(seen.size() == valid_count, "every valid item is a candidate; invalid ones never are");
}
