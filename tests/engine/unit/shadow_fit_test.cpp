/**
 * @file shadow_fit_test.cpp
 * @brief The directional shadow fit (toy_render_math.h): the single-map fit, cascade splits, per-slice
 *        fits, the focus (sphere) fit and the cascade atlas layout. Each check is a geometric
 *        invariant the shader relies on -- containment, texel snapping, rotation invariance,
 *        disjoint tiles -- not a tuned constant.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <toyengine/render/toy_render_math.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("shadow_fit");

namespace {

toy::render::ShadowFitCamera make_shadow_fit_camera(const glm::vec3& eye, const glm::vec3& at) {
    toy::render::ShadowFitCamera cam;
    cam.view              = glm::lookAt(eye, at, glm::vec3(0.0f, 0.0f, 1.0f));
    cam.is_perspective    = true;
    cam.fov_degrees       = 45.0f;
    cam.near_clip         = 0.1f;
    cam.far_clip          = 1000.0f;
    cam.aspect            = 16.0f / 9.0f;
    return cam;
}

} // namespace

COOPA_TEST(box_size_is_invariant_under_camera_rotation) {
    // The bounding SPHERE of the frustum has a rotation-invariant radius, so orbiting the
    // camera about its own target must not resize the box (texel size tracks the extent).
    glm::vec3 dir(0.3f, 0.4f, -1.0f);
    auto cam_a = make_shadow_fit_camera(glm::vec3(0.0f, -10.0f, 4.0f), glm::vec3(0.0f));
    auto cam_b = make_shadow_fit_camera(glm::vec3(10.0f, 0.0f, 4.0f), glm::vec3(0.0f));
    auto a = toy::render::compute_dir_shadow_fit(dir, &cam_a, 60.0f, 2048);
    auto b = toy::render::compute_dir_shadow_fit(dir, &cam_b, 60.0f, 2048);
    expect_near(a.texel_world, b.texel_world, 1e-6f,
                "dir_shadow_fit: box size is invariant under camera rotation");
}

COOPA_TEST(box_centre_snaps_to_whole_texels) {
    // The box centre must land on a whole multiple of the texel size -- that snap is what
    // removes sub-texel shadow crawl as the camera translates.
    glm::vec3 dir(0.3f, 0.4f, -1.0f);
    auto cam = make_shadow_fit_camera(glm::vec3(1.234f, -9.117f, 4.0f), glm::vec3(0.5f, 0.25f, 0.0f));
    const uint32_t resolution = 2048;
    auto fit = toy::render::compute_dir_shadow_fit(dir, &cam, 60.0f, resolution);
    // The box half-extent follows from texel_world (= 2*extent / resolution). The matrix's
    // translation column survives the light rotation (which has none), so orthoRH_ZO's
    // [3][0] = -centre_x / extent recovers the light-space centre, which must be a whole
    // number of texels.
    const float extent   = fit.texel_world * static_cast<float>(resolution) * 0.5f;
    const float centre_x = -fit.light_space_matrix[3][0] * extent;
    const float ratio    = centre_x / fit.texel_world;
    expect_near(ratio, std::round(ratio), 1e-2f, "dir_shadow_fit: box centre is snapped to a whole texel");
}

COOPA_TEST(fit_covers_a_camera_far_from_the_origin) {
    // The fit is built around the camera's own frustum, so the camera position must land
    // inside the box's clip volume no matter where in the world that camera is. Checked at
    // a terrain_demo-scale offset AND at the origin, in BOTH light-space axes: a projection
    // whose Y scale is negated without its Y translation passes at the origin (where the
    // centre is 0 and the mirror is the identity) and misses the scene entirely out here.
    glm::vec3 dir(-0.45f, -0.35f, -0.82f);
    for (const glm::vec3& target : {glm::vec3(192.0f, 192.0f, 26.0f), glm::vec3(0.0f)}) {
        auto cam = make_shadow_fit_camera(target + glm::vec3(0.0f, -52.0f, 30.0f), target);
        auto fit = toy::render::compute_dir_shadow_fit(dir, &cam, 60.0f, 2048);
        const glm::vec3 cam_pos = glm::vec3(glm::inverse(cam.view)[3]);
        const glm::vec4 clip    = fit.light_space_matrix * glm::vec4(cam_pos, 1.0f);
        const glm::vec3 ndc     = glm::vec3(clip) / clip.w;
        expect(std::abs(ndc.x) <= 1.0f && std::abs(ndc.y) <= 1.0f && ndc.z >= 0.0f && ndc.z <= 1.0f,
               "dir_shadow_fit: camera position lands inside the fitted box wherever it is");
    }
}

COOPA_TEST(zero_shadow_distance_keeps_a_usable_depth_range) {
    // A zero shadow distance collapses the frustum slice; the near/far separation floor
    // must still leave a usable (non-inverted) depth range.
    glm::vec3 dir(0.0f, 0.0f, -1.0f);
    auto cam = make_shadow_fit_camera(glm::vec3(0.0f, -8.0f, 3.0f), glm::vec3(0.0f));
    auto fit = toy::render::compute_dir_shadow_fit(dir, &cam, 0.0f, 1024);
    expect(fit.texel_world > 0.0f, "dir_shadow_fit: degenerate shadow_distance keeps a positive texel size");
    expect(std::isfinite(fit.light_space_matrix[2][2]) && fit.light_space_matrix[2][2] != 0.0f,
           "dir_shadow_fit: degenerate shadow_distance keeps a finite depth range");
}

COOPA_TEST(cascade_splits_increase_and_reach_shadow_distance) {
    auto s = toy::render::compute_cascade_splits(0.1f, 60.0f, 4, 0.75f);
    expect(s.distance[0] < s.distance[1] && s.distance[1] < s.distance[2] &&
           s.distance[2] < s.distance[3],
           "cascade_splits: boundaries strictly increase");
    expect_near(s.distance[3], 60.0f, 1e-4f,
                "cascade_splits: the last cascade reaches exactly shadow_distance");
    // The whole point of cascades: the near slice must be a small fraction of the range,
    // which is what makes its ortho box (and so its texels) small.
    expect(s.distance[0] < 60.0f * 0.2f,
           "cascade_splits: the near cascade covers a small fraction of the range");
}

COOPA_TEST(each_cascade_contains_its_own_slice) {
    // Containment-based cascade selection (gfx_csm_select) only works if a slice's frustum
    // corners really do project inside that slice's own box -- otherwise a shading point
    // would fall through to a coarser cascade, or off the end entirely.
    glm::vec3 dir(-0.45f, -0.35f, -0.82f);
    auto cam = make_shadow_fit_camera(glm::vec3(12.0f, -40.0f, 18.0f), glm::vec3(0.0f, 0.0f, 2.0f));
    auto splits = toy::render::compute_cascade_splits(cam.near_clip, 60.0f, 4, 0.75f);

    const glm::mat4 cam_to_world = glm::inverse(cam.view);
    const glm::vec3 cam_pos     = glm::vec3(cam_to_world[3]);
    const glm::vec3 cam_right   = glm::vec3(cam_to_world[0]);
    const glm::vec3 cam_up      = glm::vec3(cam_to_world[1]);
    const glm::vec3 cam_forward = -glm::vec3(cam_to_world[2]);

    bool all_inside = true;
    for (int c = 0; c < 4; ++c) {
        const float slice_near = (c == 0) ? cam.near_clip : splits.distance[c - 1];
        auto fit = toy::render::compute_dir_shadow_fit_slice(
            dir, &cam, slice_near, splits.distance[c], 60.0f, 1024);
        for (float d : {slice_near, splits.distance[c]}) {
            const float half_h = d * std::tan(glm::radians(cam.fov_degrees) * 0.5f);
            const float half_w = half_h * cam.aspect;
            for (int sy = -1; sy <= 1; sy += 2) {
                for (int sx = -1; sx <= 1; sx += 2) {
                    const glm::vec3 corner = cam_pos + cam_forward * d
                                           + cam_right * (static_cast<float>(sx) * half_w)
                                           + cam_up    * (static_cast<float>(sy) * half_h);
                    const glm::vec4 clip = fit.light_space_matrix * glm::vec4(corner, 1.0f);
                    const glm::vec3 ndc  = glm::vec3(clip) / clip.w;
                    if (std::abs(ndc.x) > 1.0f || std::abs(ndc.y) > 1.0f ||
                        ndc.z < 0.0f || ndc.z > 1.0f) {
                        all_inside = false;
                    }
                }
            }
        }
    }
    expect(all_inside, "cascade_fit: every slice's frustum corners project inside its own box");
}

COOPA_TEST(cascade_reaches_casters_above_its_slice) {
    // A cascade covering only the nearest few metres must still see a caster high above it,
    // between the ground and the sun -- the near plane is pulled back by the whole
    // shadow_distance (caster_reach), not by the slice's own depth.
    const glm::vec3 dir(0.0f, 0.0f, -1.0f); // straight down
    auto cam = make_shadow_fit_camera(glm::vec3(0.0f, -6.0f, 2.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    auto fit = toy::render::compute_dir_shadow_fit_slice(dir, &cam, 0.1f, 5.0f, 60.0f, 1024);

    const glm::vec4 clip = fit.light_space_matrix * glm::vec4(0.0f, -6.0f, 32.0f, 1.0f);
    const glm::vec3 ndc  = glm::vec3(clip) / clip.w;
    expect(ndc.z >= 0.0f && ndc.z <= 1.0f,
           "cascade_fit: a caster 30m above the near cascade is still inside its depth range");
}

COOPA_TEST(cascade_atlas_tiles_are_disjoint_and_in_bounds) {
    // The shader remaps a cascade's [0,1] light-space xy into its tile. Overlapping or
    // out-of-range tiles would silently make two cascades sample each other's depths.
    for (uint32_t count = 1; count <= 4; ++count) {
        const glm::uvec2 grid = toy::render::cascade_atlas_grid(count);
        expect(grid.x * grid.y >= count, "cascade_atlas: the grid holds every cascade");
        expect(grid.x * grid.y - count <= 1, "cascade_atlas: the grid wastes at most one tile");

        for (uint32_t a = 0; a < count; ++a) {
            auto ta = toy::render::cascade_atlas_tile(a, count);
            expect(ta.origin.x >= 0.0f && ta.origin.y >= 0.0f &&
                   ta.origin.x + ta.scale.x <= 1.0f + 1e-6f &&
                   ta.origin.y + ta.scale.y <= 1.0f + 1e-6f,
                   "cascade_atlas: every tile lies inside the atlas");
            for (uint32_t b = a + 1; b < count; ++b) {
                auto tb = toy::render::cascade_atlas_tile(b, count);
                const bool disjoint =
                    ta.origin.x + ta.scale.x <= tb.origin.x + 1e-6f ||
                    tb.origin.x + tb.scale.x <= ta.origin.x + 1e-6f ||
                    ta.origin.y + ta.scale.y <= tb.origin.y + 1e-6f ||
                    tb.origin.y + tb.scale.y <= ta.origin.y + 1e-6f;
                expect(disjoint, "cascade_atlas: tiles never overlap");
            }
        }
    }
    // One cascade is the whole atlas -- exactly the pre-cascade single map.
    auto solo = toy::render::cascade_atlas_tile(0, 1);
    expect_near(solo.scale.x, 1.0f, 1e-6f, "cascade_atlas: one cascade fills the atlas");
    expect_near(solo.scale.y, 1.0f, 1e-6f, "cascade_atlas: one cascade fills the atlas (y)");
}

COOPA_TEST(focus_cascade_contains_its_sphere_with_stable_texels) {
    // A focus cascade must contain its whole sphere (containment-based selection), and its
    // texel size must depend on the radius alone -- the property that keeps the shadows from
    // swimming as the camera turns or the focus moves.
    glm::vec3 dir(-0.45f, -0.35f, -0.82f);
    const glm::vec3 focus(37.0f, -12.5f, 6.0f);
    const float radius = 12.0f;
    auto fit = toy::render::compute_dir_shadow_fit_sphere(dir, focus, radius, 96.0f, 2048);

    bool inside = true;
    for (int i = 0; i < 26; ++i) {
        const glm::vec3 d = glm::normalize(glm::vec3((i % 3) - 1, ((i / 3) % 3) - 1, (i / 9) - 1) + glm::vec3(1e-3f));
        const glm::vec4 clip = fit.light_space_matrix * glm::vec4(focus + d * radius, 1.0f);
        inside = inside && std::abs(clip.x) <= 1.0f && std::abs(clip.y) <= 1.0f && clip.z >= 0.0f && clip.z <= 1.0f;
    }
    expect(inside, "focus_fit: the cascade box contains its whole sphere");

    auto moved = toy::render::compute_dir_shadow_fit_sphere(dir, focus + glm::vec3(3.3f, -7.1f, 0.4f), radius, 96.0f, 2048);
    expect_near(moved.texel_world, fit.texel_world, 1e-6f, "focus_fit: moving the focus keeps the texel size");
    expect_near(fit.texel_world, 2.0f * 13.0f / 2048.0f, 1e-6f,
                "focus_fit: texels are (radius + 1 m pad) * 2 / resolution across");
}

// --- Clipmap fit (shadow_fit: "clipmap") ----------------------------------------------------
//
// The cacheability contract: a clipmap level's matrix is PIECEWISE CONSTANT in the camera
// position -- bit-identical while the camera stays inside one snap cell, stepping exactly
// once when it leaves. Everything the static-caster cache does rests on that, so it is
// checked directly rather than through a tolerance.

using toy::render::compute_dir_shadow_fit_sphere;

COOPA_TEST(clipmap_radii_grow_geometrically_from_the_finest) {
    const auto radii = toy::render::compute_clipmap_level_radii(12.0f, 2.0f, 8);
    expect(radii.size() == 8, "clipmap: one radius per level");
    expect_near(radii.front(), 12.0f, 1e-4f, "clipmap: level 0 takes the configured radius");
    expect_near(radii.back(), 12.0f * 128.0f, 1e-2f, "clipmap: the outermost ring reaches r0 * ratio^7");
    for (size_t i = 1; i < radii.size(); ++i) {
        expect_near(radii[i] / radii[i - 1], 2.0f, 1e-4f, "clipmap: every ring doubles the last");
    }
    // The ratio is clamped, so a degenerate config cannot collapse the progression.
    const auto flat = toy::render::compute_clipmap_level_radii(10.0f, 1.0f, 3);
    expect(flat[1] > flat[0] && flat[2] > flat[1], "clipmap: a ratio below the clamp still grows");
}

COOPA_TEST(clipmap_matrix_holds_still_within_a_snap_cell) {
    // The cacheability contract, stated as quantization: sweeping the camera continuously
    // must make the matrix take FAR fewer distinct values than there are sample positions,
    // and each value must hold over a contiguous run. (Checking one hand-picked nudge is not
    // enough -- the snap grid lives in LIGHT space, so a small WORLD step can still cross a
    // boundary depending on where the sweep starts.)
    const glm::vec3 dir(0.3f, 0.4f, -1.0f);
    const uint32_t res = 2048, snap = 16;
    const float radius = 12.0f;
    const glm::vec3 base(100.0f, -40.0f, 6.0f);

    const auto probe = toy::render::compute_dir_shadow_fit_sphere(dir, base, radius, 1536.0f, res, snap);
    const float cell = probe.texel_world * static_cast<float>(snap);

    auto same = [](const glm::mat4& a, const glm::mat4& b) {
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                if (a[c][r] != b[c][r]) return false;   // bit-exact: the cache key compares matrices
            }
        }
        return true;
    };

    // 160 steps spanning 4 cells along a diagonal that moves all three light-space axes.
    const int steps = 160;
    const float span = cell * 4.0f;
    std::vector<glm::mat4> seq;
    seq.reserve(steps);
    for (int i = 0; i < steps; ++i) {
        const float t = span * static_cast<float>(i) / static_cast<float>(steps - 1);
        seq.push_back(compute_dir_shadow_fit_sphere(
            dir, base + glm::vec3(t, t * 0.7f, t * 0.3f), radius, 1536.0f, res, snap).light_space_matrix);
    }
    int changes = 0;
    for (size_t i = 1; i < seq.size(); ++i) {
        if (!same(seq[i - 1], seq[i])) ++changes;
    }
    // A continuous (unsnapped) fit would change on essentially every step. Snapped, the
    // matrix may only step as the sweep crosses cell boundaries on each axis.
    expect(changes > 0, "clipmap: the matrix does step as the camera crosses cells");
    expect(changes <= steps / 8,
           "clipmap: the matrix is quantized, not continuous (" + std::to_string(changes) +
               " changes over " + std::to_string(steps) + " steps)");

    // And the run containing a given position is genuinely stable: the longest identical run
    // must cover several consecutive samples, which is what a frame-to-frame cache hit is.
    int longest = 1, run = 1;
    for (size_t i = 1; i < seq.size(); ++i) {
        run = same(seq[i - 1], seq[i]) ? run + 1 : 1;
        longest = std::max(longest, run);
    }
    expect(longest >= 4, "clipmap: a snapped matrix holds over consecutive camera positions (" +
                             std::to_string(longest) + ")");
}

COOPA_TEST(clipmap_depth_origin_is_snapped_with_the_centre) {
    // Moving the camera ALONG the light direction must not disturb a snapped matrix either:
    // orthoRH_ZO builds its z row from near_plane, so an unsnapped depth origin would change
    // the matrix every frame however well xy was snapped, and no level would ever cache.
    const glm::vec3 dir = glm::normalize(glm::vec3(0.0f, 0.0f, -1.0f));
    const uint32_t res = 2048, snap = 16;
    const float radius = 12.0f;
    const glm::vec3 base(0.0f, 0.0f, 50.0f);
    const auto probe = compute_dir_shadow_fit_sphere(dir, base, radius, 1536.0f, res, snap);
    const float cell = probe.texel_world * static_cast<float>(snap);

    int changes = 0;
    glm::mat4 prev = probe.light_space_matrix;
    for (int i = 1; i <= 40; ++i) {
        // Straight down the light: this axis is pure light-space Z.
        const glm::vec3 p = base + glm::vec3(0.0f, 0.0f, -cell * 2.0f * static_cast<float>(i) / 40.0f);
        const glm::mat4 m = compute_dir_shadow_fit_sphere(dir, p, radius, 1536.0f, res, snap).light_space_matrix;
        bool differs = false;
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) differs = differs || m[c][r] != prev[c][r];
        }
        if (differs) ++changes;
        prev = m;
    }
    expect(changes <= 6, "clipmap: depth-only camera motion leaves the matrix quantized (" +
                             std::to_string(changes) + " changes over 40 steps)");
}

COOPA_TEST(clipmap_level_still_covers_its_radius_despite_the_snap) {
    // The snapped centre lags the true one, so the extent is padded to compensate. Sample the
    // sphere's own surface: every point of it must project inside the level's tile, or a
    // caster at the ring's edge would fall out of the map it is supposed to live in.
    const glm::vec3 dir(0.2f, 0.5f, -1.0f);
    const uint32_t res = 2048, snap = 32;
    const float radius = 24.0f;
    // An awkward centre, so the snap lag is near worst case on both axes.
    const glm::vec3 centre(37.137f, -11.913f, 3.5f);
    const auto fit = toy::render::compute_dir_shadow_fit_sphere(dir, centre, radius, 1536.0f, res, snap);

    bool all_inside = true;
    for (int i = 0; i < 64; ++i) {
        const float a = static_cast<float>(i) * 0.0982f;          // ~2pi/64
        const float z = -1.0f + 2.0f * (static_cast<float>(i) + 0.5f) / 64.0f;
        const float rxy = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const glm::vec3 p = centre + radius * glm::vec3(rxy * std::cos(a), rxy * std::sin(a), z);
        const glm::vec4 lsp = fit.light_space_matrix * glm::vec4(p, 1.0f);
        const glm::vec3 ndc = glm::vec3(lsp) / lsp.w;
        all_inside = all_inside && ndc.x >= -1.0f && ndc.x <= 1.0f &&
                                   ndc.y >= -1.0f && ndc.y <= 1.0f && ndc.z >= 0.0f && ndc.z <= 1.0f;
    }
    expect(all_inside, "clipmap: the padded box still contains the whole sphere after snapping");
}

COOPA_TEST(clipmap_levels_nest_so_selection_never_gaps) {
    // Containment selection walks finest-first, so level i+1 must contain level i's box --
    // a point leaving a ring has to be caught by the next one or it goes unshadowed.
    const glm::vec3 dir(0.3f, 0.4f, -1.0f);
    const uint32_t res = 2048, snap = 16;
    const glm::vec3 centre(81.3f, 22.7f, 5.0f);
    const auto radii = toy::render::compute_clipmap_level_radii(12.0f, 2.0f, 8);

    bool nested = true;
    for (size_t i = 0; i + 1 < radii.size(); ++i) {
        const auto inner = toy::render::compute_dir_shadow_fit_sphere(dir, centre, radii[i], 1536.0f, res, snap);
        const auto outer = toy::render::compute_dir_shadow_fit_sphere(dir, centre, radii[i + 1], 1536.0f, res, snap);
        // The inner level's own sphere, projected into the OUTER level, must stay in bounds.
        for (int k = 0; k < 32; ++k) {
            const float a = static_cast<float>(k) * 0.19635f;
            const glm::vec3 p = centre + radii[i] * glm::vec3(std::cos(a), std::sin(a), 0.0f);
            const glm::vec4 lsp = outer.light_space_matrix * glm::vec4(p, 1.0f);
            const glm::vec3 ndc = glm::vec3(lsp) / lsp.w;
            nested = nested && ndc.x >= -1.0f && ndc.x <= 1.0f && ndc.y >= -1.0f && ndc.y <= 1.0f;
        }
    }
    expect(nested, "clipmap: each level's sphere lies inside the next level's box");
}

COOPA_TEST(atlas_grid_packs_five_to_eight_levels_without_waste) {
    using toy::render::cascade_atlas_grid;
    // 1..4 must be exactly what they were before the level count grew.
    expect(cascade_atlas_grid(1) == glm::uvec2(1, 1), "atlas grid: 1 -> 1x1");
    expect(cascade_atlas_grid(2) == glm::uvec2(2, 1), "atlas grid: 2 -> 2x1");
    expect(cascade_atlas_grid(3) == glm::uvec2(2, 2), "atlas grid: 3 -> 2x2");
    expect(cascade_atlas_grid(4) == glm::uvec2(2, 2), "atlas grid: 4 -> 2x2");
    expect(cascade_atlas_grid(6) == glm::uvec2(3, 2), "atlas grid: 6 -> 3x2, no waste");
    expect(cascade_atlas_grid(8) == glm::uvec2(4, 2), "atlas grid: 8 -> 4x2, no waste");
    // Every level must get a tile, and no grid may hold a row that sits entirely empty.
    for (uint32_t n = 1; n <= 8; ++n) {
        const glm::uvec2 g = cascade_atlas_grid(n);
        expect(g.x * g.y >= n, "atlas grid: every level has a tile");
        expect(g.x * (g.y - 1u) < n, "atlas grid: no fully empty row");
    }
}

COOPA_TEST(eight_level_atlas_tiles_are_disjoint_and_in_bounds) {
    using toy::render::cascade_atlas_tile;
    for (uint32_t n = 5; n <= 8; ++n) {
        for (uint32_t i = 0; i < n; ++i) {
            const auto a = cascade_atlas_tile(i, n);
            expect(a.origin.x >= 0.0f && a.origin.y >= 0.0f &&
                   a.origin.x + a.scale.x <= 1.0f + 1e-5f &&
                   a.origin.y + a.scale.y <= 1.0f + 1e-5f,
                   "atlas tile: in bounds for 5..8 levels");
            for (uint32_t j = i + 1; j < n; ++j) {
                const auto b = cascade_atlas_tile(j, n);
                const bool disjoint = a.origin.x + a.scale.x <= b.origin.x + 1e-6f ||
                                      b.origin.x + b.scale.x <= a.origin.x + 1e-6f ||
                                      a.origin.y + a.scale.y <= b.origin.y + 1e-6f ||
                                      b.origin.y + b.scale.y <= a.origin.y + 1e-6f;
                expect(disjoint, "atlas tile: tiles never overlap for 5..8 levels");
            }
        }
    }
}

COOPA_TEST(snap_step_one_matches_the_historical_texel_snap) {
    // The default argument must keep every pre-existing caller bit-identical.
    const glm::vec3 dir(0.3f, 0.4f, -1.0f);
    const glm::vec3 centre(12.345f, -6.789f, 2.0f);
    const auto legacy = toy::render::compute_dir_shadow_fit_sphere(dir, centre, 20.0f, 60.0f, 2048);
    const auto explicit_one = toy::render::compute_dir_shadow_fit_sphere(dir, centre, 20.0f, 60.0f, 2048, 1u);
    bool identical = true;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            identical = identical && legacy.light_space_matrix[c][r] == explicit_one.light_space_matrix[c][r];
        }
    }
    expect(identical, "snap step 1 is the historical texel snap, bit for bit");
    expect_near(legacy.texel_world, explicit_one.texel_world, 0.0f, "snap step 1 keeps texel_world");
}
