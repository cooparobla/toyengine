/**
 * @file visibility_test.cpp
 * @brief toyengine/render/visibility.h: frustum culling, world AABBs, screen-size estimates and
 *        LOD selection -- the CPU half ToyRenderPipeline runs once per renderer per view.
 *        Pure math against hand-computed answers; no device.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <toyengine/render/visibility.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("visibility");

COOPA_TEST(frusta_cull_boxes_outside_their_planes) {
    {
        using namespace toy::render;
        const glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
        const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
        const Frustum f = Frustum::from_matrix(proj * view);
        auto box = [](glm::vec3 c, float e) { return WorldBounds{c, glm::vec3(e)}; };
        expect(f.intersects(box({0, 0, -10}, 1)),   "box straight ahead is visible");
        expect(!f.intersects(box({0, 0, 10}, 1)),   "box behind the camera is culled");
        expect(!f.intersects(box({0, 0, -200}, 1)), "box past the far plane is culled");
        expect(!f.intersects(box({50, 0, -10}, 1)), "box far to the right is culled");
        expect(f.intersects(box({6.5f, 0, -10}, 1)), "box straddling the right plane is visible");
        expect(f.intersects(box({0, 0, -0.1f}, 0.05f)),
               "box straddling the near plane is visible");
        expect(!f.intersects(box({0, 0, -0.03f}, 0.02f)),
               "box wholly between the camera and the near plane is culled");
    }
    {
        using namespace toy::render;
        // Ortho shadow-cascade style: a 20x20 box looking down -Z from z=50, depth 0..100.
        const glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 50), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
        const glm::mat4 proj = glm::ortho(-10.0f, 10.0f, -10.0f, 10.0f, 0.0f, 100.0f);
        const Frustum f = Frustum::from_matrix(proj * view);
        expect(f.intersects({{0, 0, 0}, glm::vec3(1)}),   "ortho: centre box visible");
        expect(!f.intersects({{15, 0, 0}, glm::vec3(1)}), "ortho: box outside the side planes culled");
        expect(!f.intersects({{0, 0, 60}, glm::vec3(1)}), "ortho: box behind the near plane culled");

        // A +X cube face (90-degree perspective).
        const glm::mat4 fview = glm::lookAt(glm::vec3(0), glm::vec3(1, 0, 0), glm::vec3(0, -1, 0));
        const glm::mat4 fproj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 20.0f);
        const Frustum cf = Frustum::from_matrix(fproj * fview);
        expect(cf.intersects({{5, 0, 0}, glm::vec3(0.5f)}),   "cube +X face sees +X");
        expect(!cf.intersects({{-5, 0, 0}, glm::vec3(0.5f)}), "cube +X face does not see -X");
        expect(!cf.intersects({{0, 5, 0}, glm::vec3(0.5f)}),  "cube +X face does not see +Y");
    }
}

COOPA_TEST(world_aabb_matches_transformed_corners) {
    using namespace toy::render;
    glm::mat4 m(1.0f);
    m = glm::translate(m, glm::vec3(3, -2, 1));
    m = glm::rotate(m, 0.7f, glm::normalize(glm::vec3(1, 2, 3)));
    m = glm::scale(m, glm::vec3(2, 0.5f, 1.5f));
    const glm::vec3 lo(-1, -2, -0.5f), hi(1, 1, 2);
    glm::vec3 mn(1e9f), mx(-1e9f);
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 c((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
        const glm::vec3 w = glm::vec3(m * glm::vec4(c, 1.0f));
        mn = glm::min(mn, w);
        mx = glm::max(mx, w);
    }
    const WorldBounds b = world_aabb(m, lo, hi);
    for (int a = 0; a < 3; ++a) {
        expect_near(b.center[a] - b.extent[a], mn[a], 1e-4f, "world_aabb min matches 8 corners");
        expect_near(b.center[a] + b.extent[a], mx[a], 1e-4f, "world_aabb max matches 8 corners");
    }
}

COOPA_TEST(screen_size_estimates_match_the_projection) {
    {
        using namespace toy::render;
        const glm::mat4 view(1.0f);   // camera at origin looking down -Z
        const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
        // fov 90 -> p11 = 1: a radius-1 sphere 10 away covers 1/10 of the half-height... as a
        // fraction of the full height: r * p11 / d.
        expect_near(screen_height_fraction({0, 0, -10}, 1.0f, view, proj), 0.1f, 1e-5f,
                    "perspective fraction = r * cot(fov/2) / depth");
        expect(screen_height_fraction({0, 0, -0.5f}, 1.0f, view, proj) > 1.0f,
               "a sphere around the camera covers the screen");
        const glm::mat4 ortho = glm::ortho(-5.0f, 5.0f, -5.0f, 5.0f, 0.0f, 50.0f);
        expect_near(screen_height_fraction({0, 0, -10}, 1.0f, view, ortho), 0.2f, 1e-5f,
                    "ortho fraction = r * 2/height, independent of depth");
    }
    {
        using namespace toy::render;
        // A 20-unit-wide ortho cascade at 2048 texels: 102.4 texels per unit, so a radius-0.5
        // sphere (1 unit across) is ~102 texels.
        const glm::mat4 vp = glm::ortho(-10.0f, 10.0f, -10.0f, 10.0f, 0.0f, 100.0f) *
                             glm::lookAt(glm::vec3(0, 0, 50), glm::vec3(0), glm::vec3(0, 1, 0));
        expect_near(projected_texels(vp, {0, 0, 0}, 0.5f, 2048.0f), 102.4f, 0.01f,
                    "ortho: 1 world unit = resolution / width texels");
        // Perspective (90-degree cube face, 512): halving the distance doubles the size.
        const glm::mat4 cf = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 50.0f) *
                             glm::lookAt(glm::vec3(0), glm::vec3(1, 0, 0), glm::vec3(0, -1, 0));
        const float far_t  = projected_texels(cf, {20, 0, 0}, 0.5f, 512.0f);
        const float near_t = projected_texels(cf, {10, 0, 0}, 0.5f, 512.0f);
        expect_near(near_t / far_t, 2.0f, 1e-3f, "perspective: size scales with 1/distance");
        expect(projected_texels(cf, {0.2f, 0, 0}, 0.5f, 512.0f) > 1.0e5f,
               "a caster around the light is never small");
    }
}

COOPA_TEST(select_lod_applies_thresholds_with_hysteresis) {
    using namespace toy::render;
    const std::vector<LodThreshold> t = {{0.0f}, {0.25f}, {0.1f}};
    expect(select_lod(0.5f,  t, 0.0f, -2, 0.1f) == 0, "large -> LOD0");
    expect(select_lod(0.2f,  t, 0.0f, -2, 0.1f) == 1, "medium -> LOD1");
    expect(select_lod(0.05f, t, 0.0f, -2, 0.1f) == 2, "small -> LOD2");
    expect(select_lod(0.005f, t, 0.01f, -2, 0.1f) == -1, "below cull size -> culled");
    // Hysteresis: at 0.24 (just under LOD1's 0.25) an object already at LOD0 stays at LOD0...
    expect(select_lod(0.24f, t, 0.0f, 0, 0.1f) == 0, "hysteresis holds LOD0 just under the threshold");
    // ...but well under it, it switches.
    expect(select_lod(0.2f, t, 0.0f, 0, 0.1f) == 1, "clearly under the threshold switches");
    // Going back finer needs to clear the threshold by the margin too.
    expect(select_lod(0.26f, t, 0.0f, 1, 0.1f) == 1, "hysteresis holds LOD1 just over the threshold");
    expect(select_lod(0.3f, t, 0.0f, 1, 0.1f) == 0, "clearly over the threshold refines");
    // A multi-level jump moves as far as the margin allows.
    expect(select_lod(0.01f, t, 0.0f, 0, 0.1f) == 2, "a big drop jumps straight to LOD2");
    const std::vector<LodThreshold> single = {{0.0f}};
    expect(select_lod(0.0001f, single, 0.0f, -2, 0.1f) == 0, "no LODs and no cull -> always LOD0");
}
