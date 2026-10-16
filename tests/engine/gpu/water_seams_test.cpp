/**
 * @file water_seams_test.cpp
 * @brief Water bodies that share a seam must displace it identically.
 *
 * A TileTerrain chunk publishes its still water (sea, lake) and its flowing water (rivers) as
 * SEPARATE WaterBody components, and neighbouring chunks publish their own -- yet all of them
 * share the vertices along the edges where they meet (toy::world::build_chunk_water() has every
 * chunk read those corners from one ColumnPad). The vertex stage displaces a vertex from
 * `water_gerstner(gfx_params, position, time, water_depth_attenuation(depth, wavelength), ...)`,
 * so two bodies agree on a shared vertex only while they agree on the DEPTH baked into it and on
 * the wave params of the body drawing it. Where they do not, the two surfaces part and the bed
 * shows through the gap between them -- a thin wedge, widest where the disagreement is and
 * tapering to nothing where it is not.
 *
 * Surface HEIGHT may legitimately differ at a shared xy: a river steps down a terrace into a
 * lake, so the two surfaces meet at different levels and the bank's wall covers the step.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/water/water_body.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("water_seams");

using namespace toy::test;

COOPA_TEST(bodies_sharing_a_seam_agree_on_depth_and_waves) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config =
        make_test_config("assets/scenes/terrain/terrain_demo/scene.yaml", 640, 360, 640, 360);
    config.render.transparency_enabled = true;
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 600);   // the chunks stream in around the focus and their water bakes

    std::vector<toy::water::WaterBody*> bodies;
    for (auto* b : engine.scene().get_components<toy::water::WaterBody>()) {
        if (b->baked) bodies.push_back(b);
    }
    expect(bodies.size() > 20, "water seams: the streamed world baked its chunk water (" +
                                   std::to_string(bodies.size()) + " bodies)");
    if (bodies.empty()) return;

    // Every baked vertex, bucketed by its world xy in millimetres.
    struct Hit { int body; float depth; float wave_scale; };
    std::map<std::pair<long long, long long>, std::vector<Hit>> grid;
    for (int i = 0; i < static_cast<int>(bodies.size()); ++i) {
        for (const auto& v : bodies[static_cast<std::size_t>(i)]->query.vertices()) {
            grid[{llround(v.position.x * 1000.0), llround(v.position.y * 1000.0)}].push_back({i, v.depth, v.wave_scale});
        }
    }

    int shared = 0, depth_bad = 0, wave_bad = 0, scale_bad = 0;
    float worst_depth = 0.0f, worst_amp = 0.0f, worst_scale = 0.0f;
    std::string example;
    for (const auto& [key, hits] : grid) {
        bool two = false;
        for (const auto& h : hits) two = two || h.body != hits.front().body;
        if (!two) continue;
        ++shared;
        float dmin = hits.front().depth, dmax = dmin;
        float smin = hits.front().wave_scale, smax = smin;
        for (const auto& h : hits) {
            dmin = std::min(dmin, h.depth); dmax = std::max(dmax, h.depth);
            smin = std::min(smin, h.wave_scale); smax = std::max(smax, h.wave_scale);
        }
        worst_depth = std::max(worst_depth, dmax - dmin);
        if (dmax - dmin > 1e-4f) ++depth_bad;
        worst_scale = std::max(worst_scale, smax - smin);
        if (smax - smin > 1e-4f) ++scale_bad;

        const toy::water::WaveParams a = bodies[static_cast<std::size_t>(hits.front().body)]->live_waves();
        for (const auto& h : hits) {
            if (h.body == hits.front().body) continue;
            const toy::water::WaveParams b = bodies[static_cast<std::size_t>(h.body)]->live_waves();
            const float damp = std::abs(a.amplitude - b.amplitude);
            if (damp > 1e-5f || std::abs(a.wavelength - b.wavelength) > 1e-4f ||
                std::abs(a.direction - b.direction) > 1e-5f || std::abs(a.steepness - b.steepness) > 1e-5f) {
                ++wave_bad;
                worst_amp = std::max(worst_amp, damp);
                if (example.empty()) {
                    example = " e.g. (" + std::to_string(static_cast<double>(key.first) / 1000.0) + ", " +
                              std::to_string(static_cast<double>(key.second) / 1000.0) + ")";
                }
            }
            break;
        }
    }

    expect(shared > 100, "water seams: bodies share seam vertices to compare (" + std::to_string(shared) + ")");
    expect(depth_bad == 0, "water seams: a shared vertex has one baked depth (" + std::to_string(depth_bad) +
                               " disagree, worst " + std::to_string(worst_depth) + " m)");
    // A per-body wave difference pulls every shared vertex apart; the gap is up to the sum of the
    // two amplitudes, and the bed shows through it.
    // The per-corner wave scale is what lets a river stay calmer than its lake without a
    // body-wide wave difference: both meshes mean it over the same tiles, so it must match.
    expect(scale_bad == 0, "water seams: a shared vertex has one wave scale (" + std::to_string(scale_bad) +
                               " disagree, worst " + std::to_string(worst_scale) + ")");
    expect(wave_bad == 0, "water seams: bodies sharing a vertex draw it with the same waves (" +
                              std::to_string(wave_bad) + " disagree, worst amplitude gap " +
                              std::to_string(worst_amp) + " m)" + example);
}
