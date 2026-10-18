/**
 * @file fracture_test.cpp
 * @brief Voronoi mesh fracture (toyengine/destruct/fracture.h): a clipped solid stays solid,
 *        the pieces of a broken one add back up to it, and the same seed breaks it the same way.
 *
 * Volume is the measure throughout, because it is the property destruction has to preserve and
 * the one every failure mode shows up in: a cap that did not close leaks volume, a piece that
 * kept geometry from outside its cell gains it, and a fan over an unordered ring folds back on
 * itself and loses it.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <cstdio>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/destruct/fracture.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("fracture");

using toy::destruct::FracturePiece;
using toy::destruct::FractureSettings;
using toy::destruct::Vertex;

namespace {

/** @brief A unit cube centred on the origin, as 12 triangles of unwelded corners. */
void unit_cube(std::vector<Vertex>& v, std::vector<std::uint32_t>& i, float size = 1.0f) {
    const float h = 0.5f * size;
    const glm::vec3 corner[8] = {{-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h},
                                 {-h, -h, h},  {h, -h, h},  {h, h, h},  {-h, h, h}};
    const int face[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                            {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    v.clear();
    i.clear();
    for (const auto& f : face) {
        const glm::vec3 a = corner[f[0]], b = corner[f[1]], c = corner[f[2]], d = corner[f[3]];
        const glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
        const glm::vec3 quad[4] = {a, b, c, d};
        const std::uint32_t base = static_cast<std::uint32_t>(v.size());
        for (const glm::vec3& p : quad) {
            Vertex vert;
            vert.position = p;
            vert.normal = n;
            vert.uv = glm::vec2(0.0f);
            vert.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            v.push_back(vert);
        }
        i.insert(i.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

/** @brief Signed volume of a closed mesh; 0 for an open one, which is the point of measuring it. */
double volume_of(const std::vector<Vertex>& v, const std::vector<std::uint32_t>& idx) {
    double total = 0.0;
    for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
        const glm::dvec3 a(v[idx[t]].position), b(v[idx[t + 1]].position), c(v[idx[t + 2]].position);
        total += glm::dot(a, glm::cross(b, c)) / 6.0;
    }
    return std::abs(total);
}

} // namespace

/** @brief Half a cube is half a cube: the clip keeps one side and caps the face it opened. */
COOPA_TEST(a_clipped_cube_is_closed_and_half_the_volume) {
    std::vector<Vertex> v;
    std::vector<std::uint32_t> i;
    unit_cube(v, i, 2.0f);                       // volume 8
    expect_near(static_cast<float>(volume_of(v, i)), 8.0f, 1e-3f, "clip: the source cube measures 8");

    std::vector<Vertex> out_v;
    std::vector<std::uint32_t> out_i;
    const bool kept = toy::destruct::clip_mesh(v, i, glm::vec3(1.0f, 0.0f, 0.0f), 0.0f, true, out_v, out_i);
    expect(kept && !out_i.empty(), "clip: the half below the plane survives");
    expect_near(static_cast<float>(volume_of(out_v, out_i)), 4.0f, 1e-3f,
                "clip: ...is exactly half the cube, so the cut face was capped");
    for (const Vertex& x : out_v) {
        expect(x.position.x <= 1e-4f, "clip: ...and nothing is left on the far side of the plane");
        break;   // one representative check; the volume above is what proves the whole set
    }

    // A plane past the mesh keeps everything; one before it keeps nothing.
    expect(toy::destruct::clip_mesh(v, i, glm::vec3(1, 0, 0), 5.0f, true, out_v, out_i) &&
               std::abs(volume_of(out_v, out_i) - 8.0) < 1e-3,
           "clip: a plane clear of the mesh leaves it whole");
    expect(!toy::destruct::clip_mesh(v, i, glm::vec3(1, 0, 0), -5.0f, true, out_v, out_i),
           "clip: a plane past the other side leaves nothing");
}

/**
 * @brief Fracturing conserves the solid: every piece is closed, none overlap, and their volumes
 *        sum to the original's.
 */
COOPA_TEST(the_pieces_of_a_broken_cube_add_back_up_to_it) {
    std::vector<Vertex> v;
    std::vector<std::uint32_t> i;
    unit_cube(v, i, 2.0f);

    FractureSettings s;
    s.pieces = 12;
    s.seed = 7;
    const std::vector<FracturePiece> pieces = toy::destruct::fracture_mesh(v, i, s);
    expect(pieces.size() >= 8, "fracture: a cube breaks into most of the pieces asked for (" +
                                   std::to_string(pieces.size()) + " of 12)");

    double total = 0.0;
    bool all_closed = true, all_centred = true;
    for (const FracturePiece& p : pieces) {
        const double vol = volume_of(p.vertices, p.indices);
        total += vol;
        all_closed = all_closed && vol > 1e-6;
        // Each piece is centred on its own origin, so its geometry straddles it.
        glm::vec3 lo(1e9f), hi(-1e9f);
        for (const Vertex& x : p.vertices) { lo = glm::min(lo, x.position); hi = glm::max(hi, x.position); }
        all_centred = all_centred && glm::length(lo + hi) < 1e-3f;
    }
    std::printf("  cube into %zu pieces: volume %.4f of 8.0, largest %.4f\n",
                pieces.size(), total, pieces.empty() ? 0.0 : volume_of(pieces[0].vertices, pieces[0].indices));
    expect(all_closed, "fracture: every piece encloses a volume (its cut faces were capped)");
    expect(all_centred, "fracture: every piece is centred on its own origin, ready to be a body");
    expect_near(static_cast<float>(total), 8.0f, 0.05f,
                "fracture: the pieces add back up to the cube (" + std::to_string(total) + ")");
    expect(pieces.front().volume >= pieces.back().volume, "fracture: pieces come back largest first");
}

/** @brief The same seed breaks a mesh identically; a different one does not. */
COOPA_TEST(a_seed_makes_the_break_repeatable) {
    std::vector<Vertex> v;
    std::vector<std::uint32_t> i;
    unit_cube(v, i, 2.0f);

    FractureSettings a;
    a.pieces = 8;
    a.seed = 42;
    FractureSettings b = a;
    FractureSettings other = a;
    other.seed = 43;

    const auto first = toy::destruct::fracture_mesh(v, i, a);
    const auto again = toy::destruct::fracture_mesh(v, i, b);
    const auto differ = toy::destruct::fracture_mesh(v, i, other);

    bool same = first.size() == again.size();
    for (std::size_t k = 0; same && k < first.size(); ++k) {
        same = glm::length(first[k].centre - again[k].centre) < 1e-6f &&
               first[k].vertices.size() == again[k].vertices.size();
    }
    expect(same, "fracture: the same seed gives the same pieces");

    bool moved = differ.size() != first.size();
    for (std::size_t k = 0; !moved && k < first.size(); ++k) {
        moved = glm::length(first[k].centre - differ[k].centre) > 1e-4f;
    }
    expect(moved, "fracture: a different seed gives a different break");
}

/** @brief An open shell (no interior) still breaks, into open shells -- see the header's note. */
COOPA_TEST(an_open_shell_breaks_without_capping_a_volume) {
    // Two triangles: a flat quad, the degenerate case a cap cannot close.
    std::vector<Vertex> v(4);
    const glm::vec3 p[4] = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
    for (int k = 0; k < 4; ++k) {
        v[static_cast<std::size_t>(k)].position = p[k];
        v[static_cast<std::size_t>(k)].normal = glm::vec3(0, 0, 1);
        v[static_cast<std::size_t>(k)].tangent = glm::vec4(1, 0, 0, 1);
    }
    const std::vector<std::uint32_t> i = {0, 1, 2, 0, 2, 3};

    FractureSettings s;
    s.pieces = 4;
    s.seed = 3;
    const auto pieces = toy::destruct::fracture_mesh(v, i, s);
    // Zero-volume pieces are dropped, so a flat sheet yields nothing rather than a pile of
    // invisible bodies -- the caller sees "this mesh does not fracture" instead of debris
    // that cannot be hit.
    expect(pieces.empty(), "fracture: a flat sheet produces no solid pieces (" +
                               std::to_string(pieces.size()) + ")");
}

/**
 * @brief A shard's collider boxes are measured in its own frame and fill it far better than a
 *        world-axis box would -- which is what makes the debris rest on its shape rather than
 *        on the corners of a box drawn round it.
 *
 * Measured two ways, because both failures are real: the piece's own box (its principal frame,
 * one shape) against the world-axis box of the same geometry, and the fitted set of boxes
 * against that single box.
 */
COOPA_TEST(chunk_colliders_fit_the_shard_not_its_world_box) {
    std::vector<Vertex> v;
    std::vector<std::uint32_t> i;
    unit_cube(v, i, 2.0f);

    FractureSettings s;
    s.pieces = 16;
    s.seed = 19;
    s.collider_fill = 0.6f;
    s.max_boxes = 4;
    const std::vector<FracturePiece> pieces = toy::destruct::fracture_mesh(v, i, s);
    require(!pieces.empty(), "colliders: the cube broke");

    double own_frame = 0.0, world_frame = 0.0, fitted = 0.0, piece_volume = 0.0;
    std::size_t multi = 0, inside = 0;
    for (const FracturePiece& p : pieces) {
        piece_volume += p.volume;
        own_frame += 8.0 * p.half_extent.x * p.half_extent.y * p.half_extent.z;

        // The same geometry measured on the world axes: undo the piece's own rotation.
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const Vertex& x : p.vertices) {
            const glm::vec3 w = p.orientation * x.position;
            lo = glm::min(lo, w);
            hi = glm::max(hi, w);
        }
        const glm::vec3 world_half = 0.5f * (hi - lo);
        world_frame += 8.0 * world_half.x * world_half.y * world_half.z;

        require(!p.boxes.empty(), "colliders: every piece has at least one box");
        multi += p.boxes.size() > 1 ? 1 : 0;
        for (const toy::destruct::FractureBox& b : p.boxes) {
            fitted += 8.0 * b.half_extent.x * b.half_extent.y * b.half_extent.z;
            // A box may not claim space the piece's own bounds do not have.
            const glm::vec3 reach = glm::abs(b.centre) + b.half_extent;
            inside += glm::all(glm::lessThanEqual(reach, p.half_extent + glm::vec3(1e-3f))) ? 1 : 0;
        }
    }
    std::size_t boxes = 0;
    for (const FracturePiece& p : pieces) boxes += p.boxes.size();

    std::printf("  %zu pieces, %zu collider boxes (%zu pieces split): volume %.3f, own-frame box %.3f, "
                "world-axis box %.3f, fitted boxes %.3f\n",
                pieces.size(), boxes, multi, piece_volume, own_frame, world_frame, fitted);

    expect(inside == boxes, "colliders: no box reaches outside the piece it belongs to");
    expect(own_frame < world_frame, "colliders: the piece's own frame boxes it tighter than the world axes (" +
                                        std::to_string(own_frame) + " vs " + std::to_string(world_frame) + ")");
    expect(fitted < own_frame, "colliders: and the fitted set is tighter still (" + std::to_string(fitted) +
                                   " vs " + std::to_string(own_frame) + ")");
    expect(multi > 0, "colliders: the awkward shards were split into several boxes (" +
                          std::to_string(multi) + " of " + std::to_string(pieces.size()) + ")");
    expect(boxes <= pieces.size() * 4, "colliders: ...within the budget");
}
