/**
 * @file water_flow_bake_test.cpp
 * @brief The river flow bake (toyengine/water/water_flow_bake.h): downhill and faster where
 *        steep, white water only on rapids, UV-following on level water, and deflection plus a
 *        wake behind an obstacle.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/water/water_flow_bake.h>

#include "engine/support/checks.h"
#include "engine/support/water_fixtures.h"

COOPA_TEST_SUITE("water_flow_bake");

using namespace toy::test;

/** @brief The bake's whole premise: water runs downhill, faster and whiter where it is steep. */
COOPA_TEST(flow_runs_downhill_and_faster_where_steep) {
    std::vector<glm::vec3> pos, flow;
    std::vector<glm::vec2> uv;
    std::vector<uint32_t> idx;
    std::vector<float> turb;
    // Gentle (2%) for x < 20, steep (40%) for 20..30, gentle again after. Descends along +X.
    auto z = [](float x) {
        if (x < 20.0f) return -0.02f * x;
        if (x < 30.0f) return -0.4f - 0.4f * (x - 20.0f);
        return -4.4f - 0.02f * (x - 30.0f);
    };
    make_strip(50.0f, 4.0f, 100, 4, z, pos, uv, idx);
    toy::water::bake_flow(pos, uv, idx, {}, {}, flow, turb);

    glm::vec3 gentle = mean_flow(pos, flow, 4.0f, 16.0f);
    glm::vec3 steep  = mean_flow(pos, flow, 22.0f, 28.0f);
    expect(gentle.x > 0.3f && std::fabs(gentle.y) < 0.05f * gentle.x, "water flow: gentle reach flows downhill (+X)");
    expect(steep.x > 0.0f && steep.z < 0.0f, "water flow: steep reach flows downhill and down the slope");
    expect(glm::length(steep) > 1.8f * glm::length(gentle), "water flow: the steep reach runs much faster");

    float t_gentle = 0.0f, t_steep = 0.0f;
    for (std::size_t i = 0; i < pos.size(); ++i) {
        if (pos[i].x > 4.0f && pos[i].x < 16.0f) t_gentle = std::max(t_gentle, turb[i]);
        if (pos[i].x > 23.0f && pos[i].x < 27.0f) t_steep = std::min(t_steep == 0.0f ? 1.0f : t_steep, turb[i]);
    }
    expect(t_gentle < 0.05f, "water flow: no white water on the gentle reach");
    expect(t_steep > 0.5f, "water flow: rapids are turbulent");

    // Level water follows the UVs instead: u increasing along -X.
    make_strip(20.0f, 4.0f, 40, 4, [](float) { return 0.0f; }, pos, uv, idx);
    for (auto& u : uv) u.x = -u.x;
    toy::water::bake_flow(pos, uv, idx, {}, {}, flow, turb);
    glm::vec3 level = mean_flow(pos, flow, 4.0f, 16.0f);
    expect(level.x < -0.3f, "water flow: level water follows UV u when there is no slope to follow");
}

/** @brief A vertical wall across part of the stream: flow slides along it, a wake forms behind. */
COOPA_TEST(obstacle_deflects_flow_and_leaves_a_wake) {
    std::vector<glm::vec3> pos, flow_free, flow_obs;
    std::vector<glm::vec2> uv;
    std::vector<uint32_t> idx;
    std::vector<float> turb_free, turb_obs;
    make_strip(30.0f, 6.0f, 60, 12, [](float x) { return -0.03f * x; }, pos, uv, idx);
    // A post: an infinite vertical cylinder of radius 0.5 at (15, 0).
    toy::water::FlowRayFn post = [](const glm::vec3& o, const glm::vec3& d, float max_d, toy::water::FlowRayHit& hit) {
        glm::vec2 oc = glm::vec2(o) - glm::vec2(15.0f, 0.0f);
        glm::vec2 dh(d);
        float a = glm::dot(dh, dh);
        if (a < 1e-8f) return false;
        float b = glm::dot(oc, dh), c = glm::dot(oc, oc) - 0.25f;
        float disc = b * b - a * c;
        if (disc < 0.0f) return false;
        float t = (-b - std::sqrt(disc)) / a;
        if (t < 0.0f || t > max_d) return false;
        glm::vec2 p = glm::vec2(o) + dh * t - glm::vec2(15.0f, 0.0f);
        hit.distance = t;
        hit.normal = glm::vec3(glm::normalize(p), 0.0f);
        return true;
    };
    toy::water::FlowBakeParams fp;
    toy::water::bake_flow(pos, uv, idx, fp, {}, flow_free, turb_free);
    toy::water::bake_flow(pos, uv, idx, fp, post, flow_obs, turb_obs);

    float ahead_free = 0.0f, ahead_obs = 0.0f, wake_turb = 0.0f, far_turb = 0.0f;
    for (std::size_t i = 0; i < pos.size(); ++i) {
        const glm::vec3& p = pos[i];
        if (p.x > 13.6f && p.x < 14.4f && std::fabs(p.y) < 0.3f) {
            ahead_free += flow_free[i].x;
            ahead_obs += flow_obs[i].x;
        }
        if (p.x > 16.0f && p.x < 18.0f && std::fabs(p.y) < 0.3f) wake_turb = std::max(wake_turb, turb_obs[i]);
        if (p.x > 16.0f && p.x < 18.0f && std::fabs(p.y) > 2.5f) far_turb = std::max(far_turb, turb_obs[i]);
    }
    expect(ahead_obs < 0.8f * ahead_free, "water flow: the current into a post is turned aside");
    expect(wake_turb > 0.15f, "water flow: white water trails behind the post");
    expect(far_turb < wake_turb, "water flow: the wake stays behind the post, not across the stream");
}
