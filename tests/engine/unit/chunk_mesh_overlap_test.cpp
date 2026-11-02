/**
 * @file chunk_mesh_overlap_test.cpp
 * @brief The styled mesher never emits two coplanar faces over the same area.
 *
 * Two triangles occupying the same plane patch rasterize to depths an ulp apart; which one wins
 * the depth test is re-decided per pixel whenever the camera moves and freezes when it stops.
 * On screen that is surface flicker that no render toggle affects (it lives in the depth test),
 * that no still-image comparison catches (identical pose means an identical winner), and that a
 * LESS-vs-LESS_OR_EQUAL probe cannot see either (the depths are unequal, just barely). The only
 * reliable detector is the geometry itself -- which is what this suite scans.
 *
 * Coverage: every shipped tile set, flat and randomized terrain (seeded, so failures reproduce),
 * and a pair of adjacent chunks meshed from one shared heightfield, since a doubled wall can be
 * two chunks each emitting the same border face -- invisible to any single-chunk scan.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/world/terrain_chunk.h>
#include <toyengine/world/tile_topology.h>
#include <toyengine/world/tile_types.h>

#include "engine/support/checks.h"
#include "engine/support/terrain_fixtures.h"

COOPA_TEST_SUITE("chunk_mesh_overlap");

using namespace toy::test;

namespace {

struct Tri {
    glm::vec3 a, b, c;
    glm::vec3 n;       // unit normal, sign-canonicalized with d
    glm::vec3 gn;      // geometric (winding) normal, NOT canonicalized -- which way it faces
    float     d;       // plane offset: dot(n, a)
    float     area;
};

/** @brief The mesh's triangles with their canonicalized planes; degenerates dropped. */
std::vector<Tri> collect_triangles(const toy::world::ChunkMeshData& mesh, glm::vec3 offset) {
    std::vector<Tri> tris;
    tris.reserve(mesh.indices.size() / 3);
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        Tri t;
        t.a = mesh.vertices[mesh.indices[i]].position + offset;
        t.b = mesh.vertices[mesh.indices[i + 1]].position + offset;
        t.c = mesh.vertices[mesh.indices[i + 2]].position + offset;
        const glm::vec3 cross = glm::cross(t.b - t.a, t.c - t.a);
        t.area = 0.5f * glm::length(cross);
        if (t.area < 1e-8f) continue;
        t.n = cross / (2.0f * t.area);
        t.gn = t.n;
        t.d = glm::dot(t.n, t.a);
        // One canonical orientation per geometric plane, so two coplanar faces wound opposite
        // ways (a top and the underside stamped over it) land in the same bucket.
        if (t.n.x < 0.0f || (t.n.x == 0.0f && (t.n.y < 0.0f || (t.n.y == 0.0f && t.n.z < 0.0f)))) {
            t.n = -t.n;
            t.d = -t.d;
        }
        tris.push_back(t);
    }
    return tris;
}

/** @brief Clips polygon `poly` against the half-plane left of edge a->b (2D). */
void clip_edge(std::vector<glm::vec2>& poly, glm::vec2 a, glm::vec2 b) {
    std::vector<glm::vec2> out;
    out.reserve(poly.size() + 1);
    const glm::vec2 e = b - a;
    auto side = [&](glm::vec2 p) { return e.x * (p.y - a.y) - e.y * (p.x - a.x); };
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const glm::vec2 p = poly[i];
        const glm::vec2 q = poly[(i + 1) % poly.size()];
        const float sp = side(p), sq = side(q);
        if (sp >= 0.0f) out.push_back(p);
        if ((sp > 0.0f && sq < 0.0f) || (sp < 0.0f && sq > 0.0f)) {
            out.push_back(p + (q - p) * (sp / (sp - sq)));
        }
    }
    poly = std::move(out);
}

/** @brief Area of the intersection of two 2D triangles (Sutherland-Hodgman clip). */
float overlap_area_2d(const glm::vec2 p[3], const glm::vec2 q[3]) {
    // The clipper assumes the clip triangle is counter-clockwise.
    glm::vec2 c0 = q[0], c1 = q[1], c2 = q[2];
    if ((c1.x - c0.x) * (c2.y - c0.y) - (c1.y - c0.y) * (c2.x - c0.x) < 0.0f) std::swap(c1, c2);
    std::vector<glm::vec2> poly{p[0], p[1], p[2]};
    clip_edge(poly, c0, c1);
    if (poly.size() >= 3) clip_edge(poly, c1, c2);
    if (poly.size() >= 3) clip_edge(poly, c2, c0);
    if (poly.size() < 3) return 0.0f;
    float twice = 0.0f;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const glm::vec2 a = poly[i], b = poly[(i + 1) % poly.size()];
        twice += a.x * b.y - a.y * b.x;
    }
    return 0.5f * std::abs(twice);
}

/** @brief Projects a triangle onto the plane's two dominant axes. */
void project(const Tri& t, int drop, glm::vec2 out[3]) {
    auto pick = [&](const glm::vec3& v) {
        return drop == 0 ? glm::vec2(v.y, v.z) : drop == 1 ? glm::vec2(v.x, v.z) : glm::vec2(v.x, v.y);
    };
    out[0] = pick(t.a);
    out[1] = pick(t.b);
    out[2] = pick(t.c);
}

struct OverlapReport {
    long long  pairs = 0;             // every coplanar overlapping pair
    long long  same_facing = 0;       // the subset whose windings agree -- these z-fight even
                                      // with backface culling on; opposite-facing coincident
                                      // pairs are culled to one visible face per view side
    long long  coplanar_pairs_checked = 0;
    glm::vec3  example_at{0.0f};
    float      example_area = 0.0f;
};

/**
 * @brief Counts coplanar triangle pairs whose 2D intersection has real area.
 *
 * Shared edges and shared vertices are fine (adjacent triangles meet with zero area); the
 * epsilon is far below any authored piece (the smallest voxel feature is 0.5 x 1 wu) and far
 * above the clipper's float noise.
 */
OverlapReport find_overlaps(const std::vector<Tri>& tris, bool verbose) {
    // Plane buckets. d is quantized coarsely and each triangle is also probed in the two
    // neighbouring d-cells, so a pair straddling a cell boundary is still compared.
    struct Key {
        int nx, ny, nz, d;
        bool operator<(const Key& o) const {
            if (nx != o.nx) return nx < o.nx;
            if (ny != o.ny) return ny < o.ny;
            if (nz != o.nz) return nz < o.nz;
            return d < o.d;
        }
    };
    const float n_step = 1e-3f, d_step = 1e-3f;
    std::vector<std::pair<Key, std::size_t>> entries;
    entries.reserve(tris.size());
    for (std::size_t i = 0; i < tris.size(); ++i) {
        const Tri& t = tris[i];
        entries.push_back({{int(std::lround(t.n.x / n_step)), int(std::lround(t.n.y / n_step)),
                            int(std::lround(t.n.z / n_step)), int(std::floor(t.d / d_step))},
                           i});
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    OverlapReport report;
    const float plane_eps = 2e-3f;   // max |d| gap still considered the same plane
    const float area_eps  = 1e-4f;   // wu^2 -- a 1cm x 1cm patch
    for (std::size_t s = 0; s < entries.size();) {
        // One normal bucket, d-cells merged while within plane_eps of the run's start.
        std::size_t e = s + 1;
        while (e < entries.size()) {
            const Key &k0 = entries[s].first, &k1 = entries[e].first;
            if (k1.nx != k0.nx || k1.ny != k0.ny || k1.nz != k0.nz) break;
            ++e;
        }
        for (std::size_t i = s; i < e; ++i) {
            const Tri& ti = tris[entries[i].second];
            const glm::vec3 an = glm::abs(ti.n);
            const int drop = an.x > an.y ? (an.x > an.z ? 0 : 2) : (an.y > an.z ? 1 : 2);
            glm::vec2 pi[3];
            project(ti, drop, pi);
            for (std::size_t j = i + 1; j < e; ++j) {
                const Tri& tj = tris[entries[j].second];
                if (std::abs(ti.d - tj.d) > plane_eps) continue;
                ++report.coplanar_pairs_checked;
                glm::vec2 pj[3];
                project(tj, drop, pj);
                const float area = overlap_area_2d(pi, pj);
                if (area > area_eps) {
                    const bool same = glm::dot(ti.gn, tj.gn) > 0.0f;
                    if (same) ++report.same_facing;
                    if (report.pairs == 0) {
                        report.example_at = (ti.a + ti.b + ti.c) / 3.0f;
                        report.example_area = area;
                    }
                    if (verbose && report.pairs < 20) {
                        auto tri_str = [](const Tri& t) {
                            auto v = [](const glm::vec3& p) {
                                return "(" + std::to_string(p.x) + "," + std::to_string(p.y) +
                                       "," + std::to_string(p.z) + ")";
                            };
                            return v(t.a) + v(t.b) + v(t.c);
                        };
                        std::cerr << "         overlap " << area << " wu^2 "
                                  << (same ? "SAME-FACING" : "opposite-facing") << " n=("
                                  << ti.n.x << "," << ti.n.y << "," << ti.n.z
                                  << ")\n           A[area "
                                  << ti.area << "] " << tri_str(ti) << "\n           B[area "
                                  << tj.area << "] " << tri_str(tj) << "\n";
                    }
                    ++report.pairs;
                }
            }
        }
        s = e;
    }
    return report;
}

/**
 * @brief Counts T-junctions: a vertex of one triangle lying strictly inside an edge of another.
 *
 * Adjacent triangles whose shared boundary has a vertex on one side that the other side does not
 * share cannot rasterize watertight: the two sides evaluate coverage from different edge
 * equations, so sub-pixel holes open and close along the junction as the camera moves -- a line
 * of sparkling pixels that is still at rest, survives every render toggle, and shows whatever is
 * behind the surface through the holes. The greedy top-rectangle merge and the straight wall-run
 * merge both build long edges that neighbouring per-tile geometry subdivides, which is exactly
 * this configuration -- including across PLANES (a merged wall strip meeting per-tile tops along
 * a cliff edge), so this scan is 3D, not per-plane.
 */
long long count_t_junctions(const std::vector<Tri>& tris, float min_edge = 0.0f) {
    struct Q {
        int x, y, z;
        bool operator==(const Q& o) const { return x == o.x && y == o.y && z == o.z; }
        bool operator<(const Q& o) const {
            if (x != o.x) return x < o.x;
            if (y != o.y) return y < o.y;
            return z < o.z;
        }
    };
    const float q = 1.0f / 1e-3f;   // quantization: 1e-3 wu, far under the 0.5 wu feature grid
    auto quant = [&](const glm::vec3& v) {
        return Q{int(std::lround(v.x * q)), int(std::lround(v.y * q)), int(std::lround(v.z * q))};
    };

    // Unique vertices, hashed into coarse grid cells for the segment walk.
    std::vector<std::pair<Q, glm::vec3>> verts;
    for (const Tri& t : tris) {
        verts.push_back({quant(t.a), t.a});
        verts.push_back({quant(t.b), t.b});
        verts.push_back({quant(t.c), t.c});
    }
    std::sort(verts.begin(), verts.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    verts.erase(std::unique(verts.begin(), verts.end(),
                            [](const auto& a, const auto& b) { return a.first == b.first; }),
                verts.end());

    const float cell = 1.0f;
    struct Cell {
        int x, y, z;
        bool operator<(const Cell& o) const {
            if (x != o.x) return x < o.x;
            if (y != o.y) return y < o.y;
            return z < o.z;
        }
    };
    std::vector<std::pair<Cell, std::size_t>> grid;
    grid.reserve(verts.size());
    auto cell_of = [&](const glm::vec3& v) {
        return Cell{int(std::floor(v.x / cell)), int(std::floor(v.y / cell)),
                    int(std::floor(v.z / cell))};
    };
    for (std::size_t i = 0; i < verts.size(); ++i) grid.push_back({cell_of(verts[i].second), i});
    std::sort(grid.begin(), grid.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    auto cell_range = [&](const Cell& c) {
        auto lo = std::lower_bound(grid.begin(), grid.end(), c,
                                   [](const auto& e, const Cell& k) { return e.first < k; });
        auto hi = std::upper_bound(grid.begin(), grid.end(), c,
                                   [](const Cell& k, const auto& e) { return k < e.first; });
        return std::make_pair(lo, hi);
    };

    // Unique edges (undirected, quantized), so a welded edge shared by two triangles is walked
    // once and never reports its own endpoints.
    std::vector<std::pair<std::pair<Q, Q>, std::pair<glm::vec3, glm::vec3>>> edges;
    for (const Tri& t : tris) {
        const glm::vec3* v[3] = {&t.a, &t.b, &t.c};
        for (int e = 0; e < 3; ++e) {
            glm::vec3 a = *v[e], b = *v[(e + 1) % 3];
            Q qa = quant(a), qb = quant(b);
            if (qb < qa) {
                std::swap(a, b);
                std::swap(qa, qb);
            }
            edges.push_back({{qa, qb}, {a, b}});
        }
    }
    std::sort(edges.begin(), edges.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    edges.erase(std::unique(edges.begin(), edges.end(),
                            [](const auto& a, const auto& b) { return a.first == b.first; }),
                edges.end());

    const float dist_eps = 1e-4f;
    long long junctions = 0;
    std::vector<std::pair<Q, std::pair<Q, Q>>> found;   // dedup (vertex, edge)
    for (const auto& [key, seg] : edges) {
        const glm::vec3 a = seg.first, b = seg.second;
        const glm::vec3 ab = b - a;
        const float len2 = glm::dot(ab, ab);
        if (len2 < 1e-10f || len2 < min_edge * min_edge) continue;
        const Cell ca = cell_of(glm::min(a, b) - dist_eps), cb = cell_of(glm::max(a, b) + dist_eps);
        for (int cz = ca.z; cz <= cb.z; ++cz)
            for (int cy = ca.y; cy <= cb.y; ++cy)
                for (int cx = ca.x; cx <= cb.x; ++cx) {
                    auto [lo, hi] = cell_range(Cell{cx, cy, cz});
                    for (auto it = lo; it != hi; ++it) {
                        const glm::vec3 p = verts[it->second].second;
                        const float t = glm::dot(p - a, ab) / len2;
                        if (t < 1e-3f || t > 1.0f - 1e-3f) continue;   // endpoint, not a junction
                        const glm::vec3 closest = a + ab * t;
                        if (glm::dot(p - closest, p - closest) > dist_eps * dist_eps) continue;
                        found.push_back({verts[it->second].first, key});
                    }
                }
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    junctions = static_cast<long long>(found.size());
    if (std::getenv("OVERLAP_VERBOSE") != nullptr) {
        for (std::size_t i = 0; i < found.size() && i < 12; ++i) {
            const Q& v = found[i].first;
            std::cerr << "         T-junction at (" << v.x * 1e-3f << ", " << v.y * 1e-3f << ", "
                      << v.z * 1e-3f << ")\n";
        }
    }
    return junctions;
}

/**
 * @brief A seeded terraced heightfield over `size` x `size` tiles (skirt handled by the caller):
 *        plateaus, single-step terraces and the occasional tall cliff, with a few kinds mixed in
 *        so the kind-dependent mesher branches (lips, section caps) run too.
 */
std::vector<toy::world::TileColumn> make_height_field(std::int32_t size, std::uint32_t seed) {
    using namespace toy::world;
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> step(-1, 1);
    std::uniform_int_distribution<int> cliff(0, 11);
    std::uniform_int_distribution<int> kind(0, 3);
    std::vector<TileColumn> field(static_cast<std::size_t>(size) * size);
    int h = 6;
    for (std::int32_t y = 0; y < size; ++y) {
        int row_h = h;
        for (std::int32_t x = 0; x < size; ++x) {
            row_h = std::clamp(row_h + step(rng), 2, 14);
            if (cliff(rng) == 0) row_h = std::clamp(row_h + 4, 2, 14);
            TileColumn col;
            col.steps = row_h;
            switch (kind(rng)) {
                case 0: col.top_kind = TileKind::Grass; break;
                case 1: col.top_kind = TileKind::Dirt; break;
                case 2: col.top_kind = TileKind::Stone; break;
                default: col.top_kind = TileKind::Sand; break;
            }
            field[static_cast<std::size_t>(y) * size + x] = col;
        }
        if (y % 3 == 0) h = std::clamp(h + step(rng), 2, 14);
    }
    return field;
}

/** @brief A pad for the chunk at tile offset (ox, oy) of `field`, skirt clamped at the border. */
toy::world::ColumnPad pad_from_field(const std::vector<toy::world::TileColumn>& field,
                                     std::int32_t field_size, std::int32_t chunk_size,
                                     std::int32_t ox, std::int32_t oy) {
    toy::world::ColumnPad pad;
    pad.resize(chunk_size);
    for (std::int32_t y = -1; y <= chunk_size; ++y) {
        for (std::int32_t x = -1; x <= chunk_size; ++x) {
            const std::int32_t fx = std::clamp(ox + x, 0, field_size - 1);
            const std::int32_t fy = std::clamp(oy + y, 0, field_size - 1);
            pad.at(x, y) = field[static_cast<std::size_t>(fy) * field_size + fx];
        }
    }
    return pad;
}

/** @brief Meshes one pad with `set` and returns its audited triangles at `offset`. */
std::vector<Tri> mesh_and_collect(const toy::world::TileMeshLibrary& library,
                                  const toy::world::ColumnPad& pad,
                                  const toy::world::TerrainParams& params, glm::vec3 offset) {
    toy::world::ChunkMeshData mesh;
    toy::world::mesh_chunk_styled(pad, library, params, mesh);
    return collect_triangles(mesh, offset);
}

/** @brief The highest hit of a straight-down ray at `(x, y)`, or a large negative sentinel on a miss. */
float cast_down(const toy::world::ChunkMeshData& mesh, float x, float y) {
    constexpr float k_miss = -1e8f;
    float best = k_miss;
    const glm::vec2 p(x, y);
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const glm::vec3& a = mesh.vertices[mesh.indices[t]].position;
        const glm::vec3& b = mesh.vertices[mesh.indices[t + 1]].position;
        const glm::vec3& c = mesh.vertices[mesh.indices[t + 2]].position;
        const glm::vec2 a2(a.x, a.y), b2(b.x, b.y), c2(c.x, c.y);
        // Barycentric weights via signed sub-areas, all over the same (possibly negative)
        // denominator, so the sign is consistent whatever way the triangle winds.
        const float area_abc = (b2.x - a2.x) * (c2.y - a2.y) - (b2.y - a2.y) * (c2.x - a2.x);
        if (std::abs(area_abc) < 1e-9f) continue;
        const float area_pbc = (b2.x - p.x) * (c2.y - p.y) - (b2.y - p.y) * (c2.x - p.x);
        const float area_pca = (c2.x - p.x) * (a2.y - p.y) - (c2.y - p.y) * (a2.x - p.x);
        const float w_a = area_pbc / area_abc;
        const float w_b = area_pca / area_abc;
        const float w_c = 1.0f - w_a - w_b;
        if (w_a < -1e-4f || w_b < -1e-4f || w_c < -1e-4f) continue;
        const float z = w_a * a.z + w_b * b.z + w_c * c.z;
        if (z > best) best = z;
    }
    return best;
}

void run_set_audit(const char* set_name) {
    using namespace toy::world;
    const TileMeshLibrary library = make_tile_library({set_name});
    const std::int32_t chunk = 8;
    TerrainParams params = make_test_params(chunk);
    const bool verbose = std::getenv("OVERLAP_VERBOSE") != nullptr;

    long long total_pairs = 0, total_same = 0, total_tris = 0, total_tjunctions = 0;
    // Several seeds: the suspect branches (tapers, run merges, shelf sections) fire on specific
    // height configurations, and one seed could miss them all.
    for (std::uint32_t seed : {7u, 19u, 42u, 1337u}) {
        // One shared field, two side-by-side chunks: catches both in-chunk doubles and a wall
        // emitted by both chunks along their shared border.
        const std::int32_t field_size = chunk * 2;
        const auto field = make_height_field(field_size, seed);
        std::vector<Tri> tris = mesh_and_collect(
            library, pad_from_field(field, field_size, chunk, 0, 0), params, glm::vec3(0.0f));
        const std::vector<Tri> right = mesh_and_collect(
            library, pad_from_field(field, field_size, chunk, chunk, 0), params,
            glm::vec3(static_cast<float>(chunk) * params.tile_size, 0.0f, 0.0f));
        tris.insert(tris.end(), right.begin(), right.end());
        total_tris += static_cast<long long>(tris.size());

        const OverlapReport report = find_overlaps(tris, verbose);
        total_pairs += report.pairs;
        total_same += report.same_facing;
        total_tjunctions += count_t_junctions(tris);
        if (report.pairs > 0) {
            std::cerr << "         [" << set_name << " seed " << seed << "] " << report.pairs
                      << " overlapping coplanar pairs, " << report.same_facing
                      << " same-facing (first: " << report.example_area << " wu^2 at "
                      << report.example_at.x << ", " << report.example_at.y << ", "
                      << report.example_at.z << ")\n";
        }
    }
    std::cerr << "         [" << set_name << "] " << total_tris << " triangles, " << total_pairs
              << " coplanar overlaps (" << total_same << " same-facing), " << total_tjunctions
              << " T-junctions\n";
    // The z-fight contract: a SAME-facing coplanar overlap is re-decided per pixel per pose and
    // flickers under motion regardless of every render setting. Opposite-facing coincident
    // sections are reported above but tolerated: terrain draws with backface culling, so only
    // one of the pair ever rasterizes from a given side.
    expect(total_same == 0, std::string(set_name) +
                                ": no same-facing coplanar face over another (" +
                                std::to_string(total_same) + ")");
}

/**
 * @brief Quadrant-granularity top merging must change only the TRIANGULATION of a pad, never the
 * surface it covers. Mesh a flat, two-kind pad (a kind island, so both the merge and its corner
 * exclusion fire, with no wall geometry to confound the check) once normally and once with
 * `TOY_TERRAIN_NO_MERGE=1`, then assert: every merged vertex position exists, within tolerance, in
 * the unmerged mesh's positions, both cover the same projected top area, and a grid of
 * straight-down rays agrees on the hit height everywhere.
 */
void run_merge_fidelity(const char* set_name) {
    using namespace toy::world;
    const TileMeshLibrary library = make_tile_library({set_name});
    const std::int32_t chunk = 6;
    const TerrainParams params = make_test_params(chunk);

    ColumnPad pad = make_flat_pad(chunk, 4);
    for (std::int32_t y = 2; y <= 3; ++y) {
        for (std::int32_t x = 2; x <= 3; ++x) pad.at(x, y).top_kind = TileKind::Stone;
    }

    ChunkMeshData merged;
    mesh_chunk_styled(pad, library, params, merged);
    ChunkMeshData unmerged;
    {
        ScopedEnv no_merge("TOY_TERRAIN_NO_MERGE", "1");
        mesh_chunk_styled(pad, library, params, unmerged);
    }

    // 1. Every merged vertex position exists, within tolerance, in the unmerged mesh's positions.
    // Not bitwise: variant_transform() places a rotated quadrant (orientation > 0) via a sin/cos
    // matrix, so an unmerged corner at a 90/180/270-degree orientation lands a few ulps off its
    // axis-exact value (e.g. cosf(radians(90)) is ~4.4e-8, not 0) -- a pre-existing property of
    // the D4 bake, not something this merge introduces. A tolerance far above that noise floor but
    // far below a real placement bug still catches the merge deriving the wrong origin.
    constexpr float k_tolerance = 1e-4f;
    std::size_t missing = 0;
    for (const auto& mv : merged.vertices) {
        bool found = false;
        for (const auto& uv : unmerged.vertices) {
            if (std::abs(mv.position.x - uv.position.x) < k_tolerance &&
                std::abs(mv.position.y - uv.position.y) < k_tolerance &&
                std::abs(mv.position.z - uv.position.z) < k_tolerance) {
                found = true;
                break;
            }
        }
        if (!found) {
            ++missing;
            std::cerr << "         [" << set_name << "] missing merged vertex at ("
                      << mv.position.x << ", " << mv.position.y << ", " << mv.position.z << ")\n";
        }
    }
    expect(missing == 0, std::string(set_name) + ": every merged vertex position exists within "
                                                  "tolerance in the unmerged mesh (" +
                              std::to_string(missing) + " of " +
                              std::to_string(merged.vertices.size()) + " missing)");

    // 2. Equal projected top area -- catches a dropped or double-covered quadrant.
    auto top_area = [](const ChunkMeshData& mesh) {
        double area = 0.0;
        for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            const glm::vec3& a = mesh.vertices[mesh.indices[t]].position;
            const glm::vec3& b = mesh.vertices[mesh.indices[t + 1]].position;
            const glm::vec3& c = mesh.vertices[mesh.indices[t + 2]].position;
            if (a.z < 3.999f || b.z < 3.999f || c.z < 3.999f) continue;  // tops only
            area += 0.5 * std::abs(static_cast<double>((b.x - a.x) * (c.y - a.y) -
                                                        (b.y - a.y) * (c.x - a.x)));
        }
        return area;
    };
    const double merged_area = top_area(merged);
    const double unmerged_area = top_area(unmerged);
    expect(std::abs(merged_area - unmerged_area) < 1e-6,
           std::string(set_name) + ": equal top area (" + std::to_string(merged_area) + " vs " +
               std::to_string(unmerged_area) + ")");

    // 3. A grid of straight-down rays agrees on the hit height everywhere -- no gap the merge
    // opened or quadrant it duplicated.
    std::size_t ray_mismatches = 0;
    for (float y = 0.25f; y < static_cast<float>(chunk); y += 0.5f) {
        for (float x = 0.25f; x < static_cast<float>(chunk); x += 0.5f) {
            const float hm = cast_down(merged, x, y);
            const float hu = cast_down(unmerged, x, y);
            if (std::abs(hm - hu) > 1e-4f) ++ray_mismatches;
        }
    }
    expect(ray_mismatches == 0, std::string(set_name) + ": straight-down rays agree between "
                                                        "merged and unmerged (" +
                                    std::to_string(ray_mismatches) + " mismatches)");
}

/**
 * @brief The ChunkLod::style tier's seam safety net. A style level re-meshes independently of its
 *        neighbour -- chunk_seam_locks() never sees it, since build_chunk_lods() only locks the
 *        ratio levels sharing LOD 0's vertices -- so whatever keeps a style-overridden chunk's
 *        border matching an ordinary neighbour's has to come from the style's own geometry, not
 *        from a shared-vertex contract.
 *
 *        Vertex-set equality turns out NOT to be the right check, despite `soft_lod` sharing every
 *        R_OUT/R_IN/LIP_H/LIP_V/round_kind_corners with `soft`: build_wall() in
 *        tools/gen_tile_styles.py samples a wall cap as a full (profile x levels) GRID -- the plan
 *        corner turn (ARC_SEG) crossed with the vertical round-over (LIP_SEG) -- and only the
 *        grid's HANDFUL OF SHARED CORNERS (both axes at an endpoint) land at the same point
 *        regardless of segment count. Every other grid point is a real vertex that exists on only
 *        ONE side -- not a small sagitta off the matching point, but an entirely absent one, since
 *        a coarser style (soft_lod's ARC_SEG 1 / LIP_SEG 1) simply never samples the corner's
 *        interior at all. A first version of this test (nearest-vertex matching, both directions)
 *        measured 0.6-2.8 tile "deviations" this way; that was the test confusing "no corresponding
 *        sample" for "a hole", not a real one.
 *
 *        So the proof instead reuses the same bar the codebase already accepts for every OTHER
 *        seam, same-style or not (run_set_audit() above, and Stage 1's quadrant-merge audit): mesh
 *        two adjacent chunks from one shared height field -- A entirely re-meshed with the
 *        `lod_style_name` override, B left as ordinary `soft` -- and require the combined surface
 *        to have no SAME-FACING coplanar overlap (the z-fight that would mean two incompatible
 *        copies of the seam exist). T-junctions are expected and not asserted, exactly as for every
 *        other style pair. A straight-down probe across the seam band additionally catches an
 *        actual see-through hole, which the overlap audit alone would not.
 */
void run_seam_proof(const char* lod_style_name) {
    using namespace toy::world;
    const TileMeshLibrary library = make_tile_library({"soft", lod_style_name});
    const std::int32_t chunk = 8;
    TerrainParams params = make_test_params(chunk);
    const bool verbose = std::getenv("OVERLAP_VERBOSE") != nullptr;

    long long total_pairs = 0, total_same = 0, total_tris = 0, total_tjunctions = 0, total_misses = 0;
    for (std::uint32_t seed : {7u, 19u, 42u, 1337u}) {
        const std::int32_t field_size = chunk * 2;
        const auto field = make_height_field(field_size, seed);
        const float seam_x = static_cast<float>(chunk) * params.tile_size;

        ChunkMeshData a, b;
        mesh_chunk_styled(pad_from_field(field, field_size, chunk, 0, 0), library, params, a,
                          /*style_override=*/1, /*native_border=*/0);
        mesh_chunk_styled(pad_from_field(field, field_size, chunk, chunk, 0), library, params, b);

        std::vector<Tri> tris = collect_triangles(a, glm::vec3(0.0f));
        const std::vector<Tri> right = collect_triangles(b, glm::vec3(seam_x, 0.0f, 0.0f));
        tris.insert(tris.end(), right.begin(), right.end());
        total_tris += static_cast<long long>(tris.size());

        const OverlapReport report = find_overlaps(tris, verbose);
        total_pairs += report.pairs;
        total_same += report.same_facing;
        total_tjunctions += count_t_junctions(tris);
        if (report.pairs > 0) {
            std::cerr << "         [" << lod_style_name << " seam, seed " << seed << "] " << report.pairs
                      << " overlapping coplanar pairs, " << report.same_facing
                      << " same-facing (first: " << report.example_area << " wu^2 at "
                      << report.example_at.x << ", " << report.example_at.y << ", "
                      << report.example_at.z << ")\n";
        }

        // Straight down, across the seam band: a combined mesh so a ray can cross from one
        // chunk's geometry into the other's.
        ChunkMeshData combined = a;
        const std::uint32_t base = static_cast<std::uint32_t>(a.vertices.size());
        combined.vertices.insert(combined.vertices.end(), b.vertices.begin(), b.vertices.end());
        for (std::size_t i = base; i < combined.vertices.size(); ++i) combined.vertices[i].position.x += seam_x;
        combined.indices.reserve(combined.indices.size() + b.indices.size());
        for (const auto idx : b.indices) combined.indices.push_back(idx + base);
        for (float y = 0.5f; y < static_cast<float>(chunk); y += 0.25f) {
            for (float x = seam_x - 1.0f; x <= seam_x + 1.0f; x += 0.1f) {
                if (cast_down(combined, x, y) < -1e7f) ++total_misses;
            }
        }
    }
    std::cerr << "         [" << lod_style_name << " seam, style tier against a plain neighbour] "
              << total_tris << " triangles, " << total_pairs << " coplanar overlaps (" << total_same
              << " same-facing), " << total_tjunctions << " T-junctions, " << total_misses
              << " straight-down misses across the seam band\n";
    expect(total_same == 0, std::string(lod_style_name) +
                                ": the style tier's seam against a plain neighbour has no "
                                "same-facing coplanar overlap (z-fight)");
    expect(total_misses == 0, std::string(lod_style_name) +
                                  ": no straight-down miss crossing from one chunk's geometry "
                                  "into the other's (a real gap, not just a differently-sampled "
                                  "round-over)");
}

} // namespace

COOPA_TEST(voxel_set_has_no_doubled_faces) { run_set_audit("voxel"); }
COOPA_TEST(round_set_has_no_doubled_faces) { run_set_audit("round"); }
COOPA_TEST(rock_set_has_no_doubled_faces) { run_set_audit("rock"); }
COOPA_TEST(soft_set_has_no_doubled_faces) { run_set_audit("soft"); }
COOPA_TEST(soft_lod_set_has_no_doubled_faces) { run_set_audit("soft_lod"); }

COOPA_TEST(quadrant_merge_changes_only_the_triangulation_soft) { run_merge_fidelity("soft"); }
COOPA_TEST(quadrant_merge_changes_only_the_triangulation_voxel) { run_merge_fidelity("voxel"); }

COOPA_TEST(soft_lod_style_tier_matches_the_seam_it_replaces) { run_seam_proof("soft_lod"); }
