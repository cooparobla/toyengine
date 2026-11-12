/**
 * @file skinning_test.cpp
 * @brief Vertex-group skinning (SkinnedMeshRenderer's CPU path, shared maths with the GPU
 *        pre-pass): groups become the bone palette, blends move vertices proportionally, the
 *        palette is the identity at rest under any owner transform, and the culling bounds contain
 *        every skinned vertex. GPU == CPU is rig_render's.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <fkYAML/node.hpp>
#include <gfxcoopa/engine/data/skinned_mesh_source.h>
#include <toyengine/scene/skinned_mesh_renderer.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("skinning");

/** @brief A mesh whose vertex groups name the bones skins with no joints: the palette comes
 *         from the groups (strongest four per vertex), and a vertex follows its groups' blend. */
COOPA_TEST(vertex_groups_become_the_palette_and_blend) {
    const fkyaml::node mesh = fkyaml::node::deserialize(std::string(
        "vertices: [[0, 0, 0], [1, 0, 0], [0, 1, 0]]\n"
        "normals: [[0, 0, 1], [0, 0, 1], [0, 0, 1]]\n"
        "uvs: [[0, 0], [1, 0], [0, 1]]\n"
        "faces: [[0, 1, 2]]\n"
        "weights:\n"
        "  - {Arm: 1.0}\n"
        "  - {Arm: 0.5, Hand: 0.5}\n"
        "  - {}\n"));
    const auto src = coopa::gfx::engine::data::SkinnedMeshSource::from_node(mesh);
    expect(src.groups.size() == 2 && src.groups[0] == "Arm" && src.groups[1] == "Hand",
           "skinning: the vertex groups become the bone palette, in first-use order");
    expect(src.joints.size() == 3 && src.joints[1].x >= 0 && src.joints[1].y >= 0 && src.joints[2].x == -1,
           "skinning: each corner indexes its groups (none for an unweighted vertex)");
    std::vector<coopa::gfx::engine::data::Vertex> out;
    const std::vector<glm::mat4> mats = {glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 2))};
    toy::scene::SkinnedMeshRenderer::skin(src, mats, out);
    expect_near(out[0].position.z, 0.0f, 1e-6f, "skinning: a vertex all in the still bone stays");
    expect_near(out[1].position.z, 1.0f, 1e-6f, "skinning: a vertex split half and half moves half way");
    expect_near(out[2].position.z, 0.0f, 1e-6f, "skinning: an unweighted vertex keeps its bind pose");
}

/** @brief The skinning palette maths (shared by the CPU and GPU paths): at the rest pose every
 *         skin matrix is the identity whatever the owner's transform, a moved bone's matrix moves
 *         its vertices in the owner's object space, and the palette-only culling bounds contain
 *         every skinned vertex. */
COOPA_TEST(palette_is_identity_at_rest_and_bounds_contain_the_skin) {
    using SMR = toy::scene::SkinnedMeshRenderer;
    const glm::mat4 owner = glm::translate(glm::mat4(1.0f), glm::vec3(3, -2, 1)) *
                            glm::rotate(glm::mat4(1.0f), 0.7f, glm::vec3(0, 0, 1));
    const std::vector<glm::mat4> bone_rest = {
        owner * glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 0)),
        owner * glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 1)) * glm::rotate(glm::mat4(1.0f), 0.3f, glm::vec3(1, 0, 0))};
    // Rest-pose inverse binds, as resolve_bones_() builds them: bone_rest^-1 * owner_rest.
    std::vector<glm::mat4> inv_bind;
    for (const glm::mat4& b : bone_rest) inv_bind.push_back(glm::inverse(b) * owner);
    const std::vector<uint8_t> valid = {1, 1};
    std::vector<glm::mat4> pal;
    SMR::compute_palette(owner, bone_rest, valid, inv_bind, pal);
    float err = 0.0f;
    for (const glm::mat4& m : pal)
        for (int c = 0; c < 4; ++c) err = std::max(err, glm::length(m[c] - glm::mat4(1.0f)[c]));
    expect(pal.size() == 2 && err < 1e-5f, "palette: identity at the rest pose under a moved, rotated owner");

    // Bone 1 lifts 0.5 m along the owner's z: its skin matrix is a pure object-space +z.
    std::vector<glm::mat4> posed = bone_rest;
    posed[1] = owner * glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 0.5f)) * glm::inverse(owner) * bone_rest[1];
    SMR::compute_palette(owner, posed, valid, inv_bind, pal);
    const glm::vec3 moved = glm::vec3(pal[1] * glm::vec4(0.2f, 0.1f, 1.0f, 1.0f));
    expect(glm::length(moved - glm::vec3(0.2f, 0.1f, 1.5f)) < 1e-5f, "palette: a moved bone moves its vertices in owner space");
    // An unresolved bone stays identity.
    SMR::compute_palette(owner, posed, {1, 0}, inv_bind, pal);
    expect(pal[1] == glm::mat4(1.0f), "palette: an unresolved bone is the identity");

    // Bounds from bone boxes contain every skinned vertex.
    const fkyaml::node mesh = fkyaml::node::deserialize(std::string(
        "vertices: [[0, 0, 0], [1, 0, 0], [0, 1, 2], [1, 1, 2], [5, 5, 5]]\n"
        "normals: [[0, 0, 1], [0, 0, 1], [0, 0, 1], [0, 0, 1], [0, 0, 1]]\n"
        "uvs: [[0, 0], [1, 0], [0, 1], [1, 1], [0, 0]]\n"
        "faces: [[0, 1, 2], [1, 3, 2], [2, 3, 4]]\n"
        "weights:\n  - {A: 1.0}\n  - {A: 0.5, B: 0.5}\n  - {B: 1.0}\n  - {B: 0.7, A: 0.3}\n  - {}\n"));
    const auto src = coopa::gfx::engine::data::SkinnedMeshSource::from_node(mesh);
    const glm::vec3 lo0(std::numeric_limits<float>::max()), hi0(std::numeric_limits<float>::lowest());
    std::vector<glm::vec3> bmin(2, lo0), bmax(2, hi0);
    glm::vec3 smin = lo0, smax = hi0;
    for (size_t i = 0; i < src.vertices.size(); ++i) {
        bool any = false;
        for (int c = 0; c < 4; ++c) {
            const int j = src.joints[i][c];
            if (j < 0 || src.weights[i][c] <= 0.0f) continue;
            bmin[j] = glm::min(bmin[j], src.vertices[i].position);
            bmax[j] = glm::max(bmax[j], src.vertices[i].position);
            any = true;
        }
        if (!any) { smin = glm::min(smin, src.vertices[i].position); smax = glm::max(smax, src.vertices[i].position); }
    }
    const std::vector<glm::mat4> mats = {
        glm::rotate(glm::mat4(1.0f), 0.9f, glm::vec3(0, 1, 0)),
        glm::translate(glm::mat4(1.0f), glm::vec3(-2, 1, 3)) * glm::rotate(glm::mat4(1.0f), -1.2f, glm::vec3(1, 1, 0))};
    std::vector<coopa::gfx::engine::data::Vertex> out;
    SMR::skin(src, mats, out);
    glm::vec3 lo, hi;
    SMR::palette_bounds(bmin, bmax, smin, smax, mats, lo, hi);
    bool inside = true;
    for (const auto& v : out)
        inside = inside && glm::all(glm::greaterThanEqual(v.position, lo - 1e-4f)) && glm::all(glm::lessThanEqual(v.position, hi + 1e-4f));
    expect(inside, "palette: culling bounds grown from the bone boxes contain every skinned vertex");
}

/**
 * @brief Preserve Volume (dual-quaternion skinning): the candy wrapper, and what fixes it.
 *
 * Skin half way between a still bone and one twisted 180 degrees about the limb's own axis.
 * Averaging the two matrices averages the two positions, which for a half turn is the axis
 * itself -- the limb pinches shut. Blending the rotations instead gives the quarter turn
 * between them, which keeps every vertex exactly as far from the axis as it started.
 */
COOPA_TEST(preserve_volume_holds_a_twisted_limb_open) {
    using SMR = toy::scene::SkinnedMeshRenderer;
    const fkyaml::node mesh = fkyaml::node::deserialize(std::string(
        "vertices: [[1, 0, 0.5], [0, 1, 0.5], [-1, 0, 0.5]]\n"
        "normals: [[1, 0, 0], [0, 1, 0], [-1, 0, 0]]\n"
        "uvs: [[0, 0], [1, 0], [0, 1]]\n"
        "faces: [[0, 1, 2]]\n"
        "weights:\n  - {Root: 0.5, Twist: 0.5}\n  - {Root: 0.5, Twist: 0.5}\n  - {Root: 0.5, Twist: 0.5}\n"));
    const auto src = coopa::gfx::engine::data::SkinnedMeshSource::from_node(mesh);
    const std::vector<glm::mat4> twist = {glm::mat4(1.0f),
                                          glm::rotate(glm::mat4(1.0f), 3.14159265f, glm::vec3(0, 0, 1))};

    std::vector<coopa::gfx::engine::data::Vertex> lbs, dqs;
    SMR::skin(src, twist, lbs, /*preserve_volume=*/false);
    SMR::skin(src, twist, dqs, /*preserve_volume=*/true);
    require(lbs.size() == dqs.size() && !lbs.empty(), "preserve volume: both paths skin the same vertices");

    float lbs_radius = 0.0f, dqs_radius = 1e9f;
    for (size_t i = 0; i < lbs.size(); ++i) {
        lbs_radius = std::max(lbs_radius, glm::length(glm::vec2(lbs[i].position)));
        dqs_radius = std::min(dqs_radius, glm::length(glm::vec2(dqs[i].position)));
    }
    expect(lbs_radius < 1e-4f, "preserve volume off: a half twist collapses the limb onto its axis");
    expect_near(dqs_radius, 1.0f, 1e-4f, "preserve volume on: every vertex keeps its distance from the axis");
    bool unit_normals = true;
    for (const auto& v : dqs) unit_normals &= std::fabs(glm::length(v.normal) - 1.0f) < 1e-4f;
    expect(unit_normals, "preserve volume: normals come out rotated and unit length");

    // The culling box has to hold the bulge the arc makes outside the matrix blend's straight line.
    const glm::vec3 lo0(std::numeric_limits<float>::max()), hi0(std::numeric_limits<float>::lowest());
    std::vector<glm::vec3> bmin(2, lo0), bmax(2, hi0);
    for (const auto& v : src.vertices) {
        for (int b = 0; b < 2; ++b) { bmin[b] = glm::min(bmin[b], v.position); bmax[b] = glm::max(bmax[b], v.position); }
    }
    glm::vec3 lo, hi;
    SMR::palette_bounds(bmin, bmax, lo0, hi0, twist, lo, hi, /*preserve_volume=*/true);
    bool inside = true;
    for (const auto& v : dqs) {
        inside &= glm::all(glm::greaterThanEqual(v.position, lo)) && glm::all(glm::lessThanEqual(v.position, hi));
    }
    expect(inside, "preserve volume: the grown culling bounds contain every dual-quaternion vertex");
    glm::vec3 tight_lo, tight_hi;
    SMR::palette_bounds(bmin, bmax, lo0, hi0, twist, tight_lo, tight_hi, /*preserve_volume=*/false);
    expect(glm::length(hi - lo) > glm::length(tight_hi - tight_lo),
           "preserve volume: ...which the matrix path's box is not grown to");
}

/**
 * @brief The two blends agree wherever they must: bones that only translate have no rotation to
 *        blend differently, so Preserve Volume costs nothing and changes nothing there.
 */
COOPA_TEST(preserve_volume_matches_the_matrix_blend_without_rotation) {
    using SMR = toy::scene::SkinnedMeshRenderer;
    const fkyaml::node mesh = fkyaml::node::deserialize(std::string(
        "vertices: [[0, 0, 0], [1, 0, 0], [0.5, 1, 0.25]]\n"
        "normals: [[0, 0, 1], [0, 0, 1], [0, 0, 1]]\n"
        "uvs: [[0, 0], [1, 0], [0, 1]]\n"
        "faces: [[0, 1, 2]]\n"
        "weights:\n  - {A: 1.0}\n  - {A: 0.25, B: 0.75}\n  - {A: 0.5, B: 0.5}\n"));
    const auto src = coopa::gfx::engine::data::SkinnedMeshSource::from_node(mesh);
    const std::vector<glm::mat4> shift = {glm::translate(glm::mat4(1.0f), glm::vec3(0.3f, -0.2f, 0.0f)),
                                          glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, 0.0f, 2.0f))};
    std::vector<coopa::gfx::engine::data::Vertex> lbs, dqs;
    SMR::skin(src, shift, lbs, false);
    SMR::skin(src, shift, dqs, true);
    float err = 0.0f;
    for (size_t i = 0; i < lbs.size(); ++i) err = std::max(err, glm::length(lbs[i].position - dqs[i].position));
    expect(err < 1e-5f, "preserve volume: identical to the matrix blend when the bones only translate");

    // And the packing round trips: one bone's dual quaternion transforms like its matrix.
    std::vector<glm::mat4> dq;
    SMR::compute_dq_palette({glm::translate(glm::mat4(1.0f), glm::vec3(1, 2, 3)) *
                             glm::rotate(glm::mat4(1.0f), 0.7f, glm::vec3(0.3f, 0.8f, 0.5f))}, dq);
    require(dq.size() == 1, "preserve volume: one palette entry per bone");
    expect_near(glm::length(glm::vec4(dq[0][0])), 1.0f, 1e-5f, "preserve volume: the packed rotation is a unit quaternion");
    expect_near(glm::length(glm::vec3(dq[0][2])), std::sqrt(3.0f), 1e-5f, "preserve volume: ...with unit scale pulled out of it");
}
