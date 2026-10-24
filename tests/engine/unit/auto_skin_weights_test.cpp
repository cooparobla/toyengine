/**
 * @file auto_skin_weights_test.cpp
 * @brief Automatic skinning weights (toyengine/scene/auto_skin_weights.h): a limb's weights
 *        follow its bone chain and hand over smoothly at each joint, every vertex ends up
 *        normalised within the engine's four influences, the visibility test keeps a bone from
 *        claiming skin it is hidden from, and a mesh with no surface falls back to envelopes.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/scene/auto_skin_weights.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("auto_skin_weights");

using toy::scene::AutoWeightMethod;
using toy::scene::AutoWeights;
using toy::scene::AutoWeightSettings;
using toy::scene::BoneSegments;

namespace {

/** @brief A mesh as the generator takes it: welded positions and triangle corners. */
struct Soup {
    std::vector<glm::vec3> positions;
    std::vector<uint32_t>  triangles;
};

/**
 * @brief A closed-sided tube along +Z (no caps), `rings` rings of `sides` vertices.
 *
 * Wound so the face normals point away from the axis, which is what the visibility test's
 * inward offset relies on. `center` offsets the axis in x.
 */
Soup tube(float radius, float length, int sides, int rings, float center_x = 0.0f) {
    Soup s;
    for (int r = 0; r < rings; ++r) {
        const float z = length * static_cast<float>(r) / static_cast<float>(rings - 1);
        for (int i = 0; i < sides; ++i) {
            const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(sides);
            s.positions.push_back({center_x + radius * std::cos(a), radius * std::sin(a), z});
        }
    }
    for (int r = 0; r + 1 < rings; ++r) {
        for (int i = 0; i < sides; ++i) {
            const uint32_t a = static_cast<uint32_t>(r * sides + i);
            const uint32_t b = static_cast<uint32_t>(r * sides + (i + 1) % sides);
            const uint32_t c = static_cast<uint32_t>((r + 1) * sides + (i + 1) % sides);
            const uint32_t d = static_cast<uint32_t>((r + 1) * sides + i);
            s.triangles.insert(s.triangles.end(), {a, b, c});
            s.triangles.insert(s.triangles.end(), {a, c, d});
        }
    }
    return s;
}

/** @brief Appends `b` to `a`, offsetting its triangle indices. */
void append(Soup& a, const Soup& b) {
    const uint32_t base = static_cast<uint32_t>(a.positions.size());
    a.positions.insert(a.positions.end(), b.positions.begin(), b.positions.end());
    for (uint32_t i : b.triangles) a.triangles.push_back(base + i);
}

/** @brief One straight bone from `head` to `tail`. */
BoneSegments bone(const std::string& name, const glm::vec3& head, const glm::vec3& tail) {
    return BoneSegments{name, {{head, tail}}};
}

/** @brief Vertex `v`'s weight in group `g` (0 when it has none). */
float weight_of(const AutoWeights& w, size_t v, int g) {
    for (const auto& [group, value] : w.weights[v]) if (group == g) return value;
    return 0.0f;
}

/** @brief The group holding most of vertex `v`, or -1 when it has no weights. */
int dominant(const AutoWeights& w, size_t v) {
    return w.weights[v].empty() ? -1 : w.weights[v].front().first;
}

} // namespace

/**
 * @brief A tube over a three-bone chain weights like a limb: every vertex is normalised and
 *        within four influences, each ring's strongest bone is the one it sits on, the handover
 *        along the chain is monotone, and a ring over a joint is shared by both its bones.
 */
COOPA_TEST(heat_weights_follow_the_bone_chain) {
    const Soup mesh = tube(0.3f, 3.0f, 16, 25);
    const std::vector<BoneSegments> bones = {
        bone("Upper", {0, 0, 0}, {0, 0, 1}),
        bone("Mid",   {0, 0, 1}, {0, 0, 2}),
        bone("Lower", {0, 0, 2}, {0, 0, 3}),
    };
    const AutoWeights w = toy::scene::compute_auto_weights(mesh.positions, mesh.triangles, bones);

    require(w.groups.size() == 3 && w.weights.size() == mesh.positions.size(),
            "auto weights: one group per bone, one weight list per vertex");
    expect(w.unsolved.empty(), "auto weights: every bone's heat solve converges");
    expect(!w.fell_back_to_envelope, "auto weights: a tube is a surface heat can conduct along");

    bool normalised = true, within_four = true, all_weighted = true;
    for (size_t v = 0; v < mesh.positions.size(); ++v) {
        float sum = 0.0f;
        for (const auto& [g, value] : w.weights[v]) sum += value;
        normalised &= std::fabs(sum - 1.0f) < 1e-4f;
        within_four &= w.weights[v].size() <= 4;
        all_weighted &= !w.weights[v].empty();
    }
    expect(normalised, "auto weights: every vertex's weights sum to 1");
    expect(within_four, "auto weights: no vertex carries more influences than the engine skins");
    expect(all_weighted, "auto weights: no vertex is left behind at its bind pose");

    // The ring a vertex sits on picks its bone: ring index -> z, 25 rings over 3 metres.
    const size_t sides = 16;
    auto ring_vertex = [&](int ring) { return static_cast<size_t>(ring) * sides; };
    expect(dominant(w, ring_vertex(4)) == 0, "auto weights: skin a third of the way up follows the first bone");
    expect(dominant(w, ring_vertex(12)) == 1, "auto weights: skin over the middle bone follows it");
    expect(dominant(w, ring_vertex(20)) == 2, "auto weights: skin over the last bone follows it");

    bool monotone = true;
    for (int ring = 0; ring + 1 < 25; ++ring) {
        monotone &= weight_of(w, ring_vertex(ring + 1), 0) <= weight_of(w, ring_vertex(ring), 0) + 1e-4f;
    }
    expect(monotone, "auto weights: the first bone's hold falls off along the chain, never climbing back");

    // Ring 8 straddles the first joint (z = 1.0 of 3.0 over 25 rings).
    const size_t joint = ring_vertex(8);
    expect(weight_of(w, joint, 0) > 0.15f && weight_of(w, joint, 1) > 0.15f,
           "auto weights: skin over a joint is shared by the bones either side of it");
}

/**
 * @brief A bone the surface hides does not claim that skin. A thin tube runs alongside a fat
 *        one, close enough that its bone is NEARER to the fat tube's flank than the fat tube's
 *        own bone -- but its own wall stands in the way, so the flank stays with its own bone.
 *        With the visibility test off, the same bone takes the flank: the lever is real.
 */
COOPA_TEST(visibility_keeps_a_hidden_bone_off_the_skin) {
    Soup mesh = tube(0.5f, 2.0f, 24, 9);
    append(mesh, tube(0.2f, 2.0f, 16, 9, 0.85f));
    const std::vector<BoneSegments> bones = {
        bone("Body", {0.0f, 0, 0}, {0.0f, 0, 2}),
        bone("Rod",  {0.85f, 0, 0}, {0.85f, 0, 2}),
    };

    // Vertex 0 of each ring of the fat tube is its +x flank: 0.5 from Body, 0.35 from Rod.
    const size_t flank = 24 * 4;   // middle ring, so the ends cannot colour the result
    expect_near(glm::length(mesh.positions[flank] - glm::vec3(0.5f, 0.0f, 1.0f)), 0.0f, 1e-5f,
                "auto weights: the flank vertex is where the test assumes");

    AutoWeightSettings seeing;
    seeing.visibility = true;
    const AutoWeights with = toy::scene::compute_auto_weights(mesh.positions, mesh.triangles, bones, seeing);
    expect(dominant(with, flank) == 0,
           "auto weights: a flank keeps its own bone when the nearer one is behind another surface");

    AutoWeightSettings blind;
    blind.visibility = false;
    const AutoWeights without = toy::scene::compute_auto_weights(mesh.positions, mesh.triangles, bones, blind);
    expect(dominant(without, flank) == 1,
           "auto weights: without the visibility test that same bone takes the flank through the wall");
}

/**
 * @brief Envelope weights need no surface: they are the explicit choice for a shape heat cannot
 *        conduct over, and the automatic fallback when a mesh has no triangles at all.
 */
COOPA_TEST(envelope_weights_need_no_surface) {
    const std::vector<glm::vec3> quad = {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}};
    const std::vector<uint32_t> tris = {0, 1, 2, 0, 2, 3};
    const std::vector<BoneSegments> bones = {
        bone("Near", {0.0f, 0, 0}, {0.0f, 0, 1}),
        bone("Far",  {1.0f, 0, 0}, {1.0f, 0, 1}),
    };

    AutoWeightSettings envelope;
    envelope.method = AutoWeightMethod::Envelope;
    const AutoWeights w = toy::scene::compute_auto_weights(quad, tris, bones, envelope);
    expect(dominant(w, 0) == 0 && dominant(w, 1) == 1,
           "envelope weights: each corner follows the bone it sits on");
    expect_near(weight_of(w, 0, 0) + weight_of(w, 0, 1), 1.0f, 1e-4f, "envelope weights: normalised");

    AutoWeightSettings heat;
    const AutoWeights fallback = toy::scene::compute_auto_weights(quad, {}, bones, heat);
    expect(fallback.fell_back_to_envelope, "auto weights: no triangles to conduct along falls back to envelopes");
    expect(dominant(fallback, 1) == 1, "auto weights: the fallback still weights every vertex");
}

/**
 * @brief The limits are honoured: fewer influences than bones, the faintest weights dropped,
 *        and no vertex left unweighted by either.
 */
COOPA_TEST(influence_and_weight_limits_are_honoured) {
    const Soup mesh = tube(0.3f, 4.0f, 12, 17);
    std::vector<BoneSegments> bones;
    for (int b = 0; b < 8; ++b) {
        bones.push_back(bone("B" + std::to_string(b), {0, 0, b * 0.5f}, {0, 0, (b + 1) * 0.5f}));
    }
    AutoWeightSettings two;
    two.max_influences = 2;
    const AutoWeights w = toy::scene::compute_auto_weights(mesh.positions, mesh.triangles, bones, two);

    bool ok = true;
    for (size_t v = 0; v < mesh.positions.size(); ++v) {
        float sum = 0.0f;
        for (const auto& [g, value] : w.weights[v]) sum += value;
        ok &= !w.weights[v].empty() && w.weights[v].size() <= 2 && std::fabs(sum - 1.0f) < 1e-4f;
    }
    expect(ok, "auto weights: max_influences caps every vertex, and what is left still sums to 1");

    AutoWeightSettings loose;
    loose.weight_limit = 0.4f;   // far above what a far-away bone contributes
    const AutoWeights pruned = toy::scene::compute_auto_weights(mesh.positions, mesh.triangles, bones, loose);
    size_t pruned_influences = 0, default_influences = 0;
    const AutoWeights plain = toy::scene::compute_auto_weights(mesh.positions, mesh.triangles, bones);
    for (size_t v = 0; v < mesh.positions.size(); ++v) {
        pruned_influences += pruned.weights[v].size();
        default_influences += plain.weights[v].size();
    }
    expect(pruned_influences < default_influences,
           "auto weights: a higher weight limit drops the faintest influences");
}
