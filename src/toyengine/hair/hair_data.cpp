#include "toyengine/hair/hair_data.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include "toyengine/curve/curve_data.h"
#include "toyengine/world/png16.h"

namespace toy {
namespace hair {

namespace {

/**
 * One deterministic random draw for (strand, purpose).
 *
 * Stateless and keyed on nothing but its arguments, which is what makes a groom identical across
 * runs, threads and machines -- the same contract procgraph's bakes keep. `stream` decorrelates
 * the purposes, so changing how many draws one step makes cannot shift another step's values.
 */
inline float rnd(std::uint32_t seed, std::uint32_t index, std::uint32_t stream) {
    const std::uint32_t a = particles::hash_u32(seed + 0x9e3779b9u * (stream + 1u));
    return particles::hash01(a ^ particles::hash_u32(index + 0x85ebca6bu));
}

/// Any unit vector perpendicular to `n`. Picks the axis `n` leans on least, so it never degenerates.
inline glm::vec3 any_perpendicular(const glm::vec3& n) {
    const glm::vec3 axis = (std::fabs(n.z) < 0.9f) ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    return glm::normalize(glm::cross(axis, n));
}

/**
 * Carries a frame along a strand: rotates `u` and `v` by the smallest rotation taking `from` to
 * `to` (parallel transport).
 *
 * Deriving each frame independently from world up instead -- which is what
 * curve::frame_from_tangent() does on its own -- makes the ROLL about the tangent jump as the
 * tangent turns, and on a ribbon that shows as a strand that twists for no reason. Same problem
 * curve::parallel_frames() solves for instanced geometry; this is that maths without the
 * per-strand vector allocations, since a groom runs it thousands of times.
 */
inline void transport(glm::vec3& u, glm::vec3& v, const glm::vec3& from, const glm::vec3& to) {
    const glm::vec3 axis = glm::cross(from, to);
    const float     s    = glm::length(axis);
    if (s < 1e-6f) return;  // parallel (or exactly opposed): nothing smaller to rotate by
    const float     angle = std::atan2(s, glm::dot(from, to));
    const glm::quat q     = glm::angleAxis(angle, axis / s);
    u = q * u;
    v = q * v;
}

} // namespace

// --- ScalpMesh / MaskImage ---------------------------------------------------------------------

ScalpMesh ScalpMesh::from_cpu_data(const MeshCpuData& data) {
    ScalpMesh out;
    out.vertices     = data.vertices.data();
    out.vertex_count = data.vertices.size();
    out.indices      = data.indices.data();
    // LOD 0 is always the full mesh; the coarser levels live behind it in the same buffer and
    // would grow hair twice over the same surface.
    out.index_count = data.lods.empty() ? data.indices.size() : data.lods[0].index_count;
    return out;
}

bool MaskImage::valid() const {
    return width > 0 && height > 0 &&
           rgba.size() >= static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
}

glm::vec4 MaskImage::sample(glm::vec2 uv) const {
    if (!valid()) return glm::vec4(1.0f);

    // Wrap, so a UV layout that runs slightly outside [0,1] still samples something sensible.
    const float fx = (uv.x - std::floor(uv.x)) * static_cast<float>(width) - 0.5f;
    const float fy = (uv.y - std::floor(uv.y)) * static_cast<float>(height) - 0.5f;
    const int   x0 = static_cast<int>(std::floor(fx));
    const int   y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);

    const auto texel = [&](int x, int y) {
        const int cx = ((x % width) + width) % width;
        const int cy = ((y % height) + height) % height;
        const std::size_t i = (static_cast<std::size_t>(cy) * static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(cx)) * 4u;
        return glm::vec4(rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]) * (1.0f / 255.0f);
    };

    const glm::vec4 a = glm::mix(texel(x0, y0), texel(x0 + 1, y0), tx);
    const glm::vec4 b = glm::mix(texel(x0, y0 + 1), texel(x0 + 1, y0 + 1), tx);
    return glm::mix(a, b, ty);
}

bool load_mask_image(const std::string& path, MaskImage& out) {
    // read_png_rgba8() is the heightmap/splat reader; hair needs exactly the same thing, so there
    // is no second image path in the tree.
    return world::read_png_rgba8(path, out.width, out.height, out.rgba);
}

HairGuide make_hair_guide(const toy::curve::CurveData& curve, int points) {
    HairGuide guide;
    if (curve.empty() || points < 2) return guide;

    // The shared sampling: points_along() is what the viewport draws and what a procedural graph
    // instances on, so a guide bends hair exactly where the drawn line is.
    const std::vector<glm::vec3> sampled = curve::points_along(curve, points);
    if (sampled.size() < 2) return guide;

    guide.root = sampled.front();
    glm::vec3 first = sampled[1] - sampled[0];
    if (glm::length(first) < 1e-7f) return guide;
    guide.direction = glm::normalize(first);
    guide.length    = curve::length(curve);
    if (guide.length <= 1e-6f) return guide;

    // Into the guide's own root frame, so the shape can be replanted on a root facing elsewhere.
    const glm::quat frame = glm::inverse(curve::frame_from_tangent(guide.direction, 0.0f));
    guide.offsets.resize(sampled.size());
    for (std::size_t k = 0; k < sampled.size(); ++k) {
        guide.offsets[k] = frame * (sampled[k] - guide.root);
    }
    return guide;
}

// --- Generation --------------------------------------------------------------------------------

float scalp_area(const ScalpMesh& scalp) {
    if (!scalp.valid()) return 0.0f;
    float area = 0.0f;
    for (std::size_t t = 0; t + 2 < scalp.index_count; t += 3) {
        const glm::vec3& a = scalp.vertices[scalp.indices[t]].position;
        const glm::vec3& b = scalp.vertices[scalp.indices[t + 1]].position;
        const glm::vec3& c = scalp.vertices[scalp.indices[t + 2]].position;
        area += 0.5f * glm::length(glm::cross(b - a, c - a));
    }
    return area;
}

std::size_t strand_count_for(const ScalpMesh& scalp, const HairParams& params) {
    if (!scalp.valid() || params.density <= 0.0f || params.max_strands <= 0) return 0;
    const double wanted = static_cast<double>(scalp_area(scalp)) * static_cast<double>(params.density);
    const auto   capped = std::min<double>(wanted, static_cast<double>(params.max_strands));
    return static_cast<std::size_t>(std::max(0.0, std::floor(capped)));
}

StrandSet scatter_roots(const ScalpMesh& scalp, const HairParams& params, const MaskImage* mask) {
    StrandSet set;
    const std::size_t wanted = strand_count_for(scalp, params);
    if (wanted == 0) return set;

    const std::size_t triangles = scalp.triangle_count();

    // Cumulative triangle area, so a draw is one binary search. Area-weighted and not uniform
    // over triangles: density must mean the same thing wherever the mesh happens to be subdivided.
    std::vector<float> cumulative(triangles);
    float running = 0.0f;
    for (std::size_t t = 0; t < triangles; ++t) {
        const std::uint32_t* tri = scalp.indices + t * 3;
        const glm::vec3&     a   = scalp.vertices[tri[0]].position;
        const glm::vec3&     b   = scalp.vertices[tri[1]].position;
        const glm::vec3&     c   = scalp.vertices[tri[2]].position;
        running += 0.5f * glm::length(glm::cross(b - a, c - a));
        cumulative[t] = running;
    }
    if (running <= 0.0f) return set;

    const bool  has_mask = mask != nullptr && mask->valid();
    const float frizz    = std::tan(glm::radians(params.frizz_degrees));

    set.roots.reserve(wanted);
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(wanted); ++i) {
        // Which triangle.
        const float target = rnd(params.seed, i, 0) * running;
        std::size_t t = static_cast<std::size_t>(
            std::lower_bound(cumulative.begin(), cumulative.end(), target) - cumulative.begin());
        if (t >= triangles) t = triangles - 1;

        // Where in it. sqrt on the first weight is what makes the point uniform over the triangle
        // rather than bunched toward one corner.
        const float s  = std::sqrt(rnd(params.seed, i, 1));
        const float b2 = rnd(params.seed, i, 2);
        const glm::vec3 w(1.0f - s, s * (1.0f - b2), s * b2);

        const std::uint32_t* tri = scalp.indices + t * 3;
        const Vertex&        v0  = scalp.vertices[tri[0]];
        const Vertex&        v1  = scalp.vertices[tri[1]];
        const Vertex&        v2  = scalp.vertices[tri[2]];

        StrandRoot root;
        root.position = v0.position * w.x + v1.position * w.y + v2.position * w.z;
        root.uv       = v0.uv * w.x + v1.uv * w.y + v2.uv * w.z;
        root.triangle = static_cast<std::uint32_t>(t);
        root.bary     = w;

        const glm::vec3 n = v0.normal * w.x + v1.normal * w.y + v2.normal * w.z;
        const float     n_len = glm::length(n);
        if (n_len < 1e-6f) {
            // A degenerate or missing vertex normal: fall back to the triangle's own face normal
            // rather than dropping the strand, which would thin the groom for a mesh-authoring bug.
            const glm::vec3 face = glm::cross(v1.position - v0.position, v2.position - v0.position);
            const float     f_len = glm::length(face);
            if (f_len < 1e-12f) continue;
            root.normal = face / f_len;
        } else {
            root.normal = n / n_len;
        }

        float length_scale = 1.0f;
        if (has_mask) {
            const glm::vec4 m = mask->sample(root.uv);
            // Rejection, not a density scale: dropping a strand whose own draw lands above the
            // mask thins the groom evenly, where scaling a per-triangle count would bias toward
            // wherever the mask happens to be bright.
            if (rnd(params.seed, i, 3) > m.r) continue;
            length_scale = m.g;
        }

        root.random = rnd(params.seed, i, 4);
        root.length = params.length * length_scale *
                      (1.0f - params.length_jitter * rnd(params.seed, i, 5));
        if (root.length <= 1e-5f) continue;

        // Frizz: tilt off the normal by a random direction within frizz_degrees.
        const glm::vec3 t1    = any_perpendicular(root.normal);
        const glm::vec3 t2    = glm::cross(root.normal, t1);
        const float     angle = rnd(params.seed, i, 6) * glm::two_pi<float>();
        const float     reach = frizz * std::sqrt(rnd(params.seed, i, 7));
        root.direction = glm::normalize(root.normal +
                                        (t1 * std::cos(angle) + t2 * std::sin(angle)) * reach);

        set.roots.push_back(root);
    }

    // Clumps by spatial CELL, not by a random draw per strand: a lock is strands that are near
    // each other, so an unrelated strand on the far side of the head must not be pulled in.
    if (params.clump > 0.0f && !set.roots.empty()) {
        const float clump_count = std::max(1.0f, static_cast<float>(set.roots.size()) * params.clump_density);
        const float cell = std::sqrt(std::max(running, 1e-8f) / clump_count);
        std::unordered_map<std::uint64_t, std::uint32_t> cell_to_clump;
        cell_to_clump.reserve(set.roots.size());
        for (StrandRoot& root : set.roots) {
            const auto cx = static_cast<std::int32_t>(std::floor(root.position.x / cell));
            const auto cy = static_cast<std::int32_t>(std::floor(root.position.y / cell));
            const auto cz = static_cast<std::int32_t>(std::floor(root.position.z / cell));
            const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 42) ^
                                      (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cy)) << 21) ^
                                       static_cast<std::uint64_t>(static_cast<std::uint32_t>(cz));
            const auto it = cell_to_clump.emplace(key, static_cast<std::uint32_t>(cell_to_clump.size()));
            root.clump = it.first->second;
        }
    }

    set.points_per_strand = params.segments + 1;
    set.points.assign(set.roots.size() * static_cast<std::size_t>(set.points_per_strand), glm::vec3(0.0f));
    return set;
}

void grow_strands(StrandSet& set, const HairParams& params, const glm::vec3& down_local,
                  const std::vector<HairGuide>& guides) {
    if (set.roots.empty() || set.points_per_strand < 2) return;

    const int   steps = set.points_per_strand - 1;
    const float inv   = 1.0f / static_cast<float>(steps);
    const glm::vec3 down = glm::length(down_local) > 1e-6f ? glm::normalize(down_local)
                                                           : glm::vec3(0.0f, 0.0f, -1.0f);

    // Pass 1: the shape of each strand on its own.
    for (std::size_t i = 0; i < set.roots.size(); ++i) {
        const StrandRoot& root = set.roots[i];
        glm::vec3*        p    = set.strand(i);

        const glm::vec3 t1 = any_perpendicular(root.direction);
        const glm::vec3 t2 = glm::cross(root.direction, t1);
        const float     phase = root.random * glm::two_pi<float>();

        for (int k = 0; k <= steps; ++k) {
            const float t = static_cast<float>(k) * inv;
            glm::vec3 q = root.position + root.direction * (root.length * t);
            // Droop as t^2: the root stays planted and the tip falls furthest.
            q += down * (root.length * params.droop * t * t);
            if (params.curl > 0.0f && params.curl_radius > 0.0f) {
                const float a = phase + glm::two_pi<float>() * params.curl * t;
                // Radius grows with t so the helix opens out of the scalp instead of starting
                // already displaced, which would lift every root off the surface.
                q += (t1 * std::cos(a) + t2 * std::sin(a)) * (params.curl_radius * t);
            }
            p[k] = q;
        }
    }

    // Pass 1b: the guides. Each strand takes the SHAPE of its nearest ones -- their offsets
    // replanted in its own root frame and scaled to its own length -- rather than their
    // positions, which would drag every strand toward one guide's tip.
    if (!guides.empty() && params.guide_influence > 0.0f) {
        std::vector<const HairGuide*> usable;
        usable.reserve(guides.size());
        for (const HairGuide& guide : guides) {
            if (guide.valid() && static_cast<int>(guide.offsets.size()) >= set.points_per_strand) {
                usable.push_back(&guide);
            }
        }
        if (!usable.empty()) {
            const int wanted = std::min(params.guide_count, static_cast<int>(usable.size()));
            // Small fixed arrays rather than a sort: `wanted` is at most 8, and this runs once
            // per strand over a groom of thousands.
            std::vector<std::pair<float, const HairGuide*>> nearest;
            nearest.reserve(static_cast<std::size_t>(wanted));

            for (std::size_t i = 0; i < set.roots.size(); ++i) {
                const StrandRoot& root = set.roots[i];
                nearest.clear();
                for (const HairGuide* guide : usable) {
                    const glm::vec3 delta = guide->root - root.position;
                    const float     d2    = glm::dot(delta, delta);
                    if (static_cast<int>(nearest.size()) < wanted) {
                        nearest.emplace_back(d2, guide);
                        std::push_heap(nearest.begin(), nearest.end(),
                                       [](const auto& a, const auto& b) { return a.first < b.first; });
                    } else if (d2 < nearest.front().first) {
                        std::pop_heap(nearest.begin(), nearest.end(),
                                      [](const auto& a, const auto& b) { return a.first < b.first; });
                        nearest.back() = {d2, guide};
                        std::push_heap(nearest.begin(), nearest.end(),
                                       [](const auto& a, const auto& b) { return a.first < b.first; });
                    }
                }

                // Inverse-square weights: a strand sitting on a guide follows it almost exactly,
                // and influence falls away smoothly between guides rather than switching.
                float total = 0.0f;
                for (const auto& [d2, guide] : nearest) total += 1.0f / (d2 + 1e-4f);
                if (total <= 0.0f) continue;

                float blended_length = 0.0f;
                for (const auto& [d2, guide] : nearest) {
                    blended_length += guide->length * (1.0f / (d2 + 1e-4f)) / total;
                }
                if (blended_length <= 1e-6f) continue;

                // The guide's shape, replanted on THIS strand's root and scaled to its length --
                // so a long guide drawn beside short hair sweeps it without lengthening it.
                const glm::quat frame = curve::frame_from_tangent(root.direction, 0.0f);
                const float     scale = root.length / blended_length;
                glm::vec3*      p     = set.strand(i);
                for (int k = 1; k <= steps; ++k) {
                    glm::vec3 offset(0.0f);
                    for (const auto& [d2, guide] : nearest) {
                        offset += guide->offsets[static_cast<std::size_t>(k)] *
                                  ((1.0f / (d2 + 1e-4f)) / total);
                    }
                    const glm::vec3 guided = root.position + frame * (offset * scale);
                    p[k] = glm::mix(p[k], guided, params.guide_influence);
                }
            }
        }
    }

    // Pass 2: gather into locks. Each strand leans toward the first strand of its cell, more so
    // toward the tip -- which is how hair actually separates: roots stay spread, tips meet.
    if (params.clump > 0.0f) {
        std::vector<std::uint32_t> representative;
        for (std::size_t i = 0; i < set.roots.size(); ++i) {
            const std::uint32_t c = set.roots[i].clump;
            if (c >= representative.size()) representative.resize(c + 1, 0xFFFFFFFFu);
            if (representative[c] == 0xFFFFFFFFu) representative[c] = static_cast<std::uint32_t>(i);
        }
        for (std::size_t i = 0; i < set.roots.size(); ++i) {
            const std::uint32_t rep = representative[set.roots[i].clump];
            if (rep == static_cast<std::uint32_t>(i)) continue;
            glm::vec3*       p = set.strand(i);
            const glm::vec3* r = set.strand(rep);
            for (int k = 1; k <= steps; ++k) {
                const float t = static_cast<float>(k) * inv;
                p[k] = glm::mix(p[k], r[k], params.clump * t * t);
            }
        }
    }

    // Pass 3: re-space to exactly `length`. Hair is inextensible, and droop plus curl have just
    // stretched the polyline; walking its direction field with uniform steps restores the length
    // while keeping the shape, and hands the solver uniform rest lengths.
    for (std::size_t i = 0; i < set.roots.size(); ++i) {
        glm::vec3*  p    = set.strand(i);
        const float step = set.roots[i].length * inv;
        glm::vec3   prev = p[0];
        glm::vec3   dir  = set.roots[i].direction;
        for (int k = 1; k <= steps; ++k) {
            const glm::vec3 raw = p[k] - prev;
            const float     len = glm::length(raw);
            if (len > 1e-7f) dir = raw / len;  // otherwise keep the previous direction
            prev = p[k];
            p[k] = p[k - 1] + dir * step;
        }
    }
}

// --- Geometry ----------------------------------------------------------------------------------

std::size_t vertices_per_strand(const HairParams& params) {
    const auto points = static_cast<std::size_t>(params.segments) + 1u;
    if (params.shape == HairShape::Tube) {
        // sides + 1 so the UV seam is a duplicated column rather than a wrap.
        return points * (static_cast<std::size_t>(params.sides) + 1u);
    }
    return points * 2u * static_cast<std::size_t>(params.cross_section);
}

std::size_t indices_per_strand(const HairParams& params) {
    const auto steps = static_cast<std::size_t>(params.segments);
    if (params.shape == HairShape::Tube) return steps * static_cast<std::size_t>(params.sides) * 6u;
    return steps * 6u * static_cast<std::size_t>(params.cross_section);
}

std::vector<std::uint32_t> strand_lod_order(std::size_t count) {
    std::vector<std::uint32_t> order;
    if (count == 0) return order;
    order.reserve(count);

    // Bits needed to cover count, then visit indices by reversed bit pattern. The result is
    // 0, n/2, n/4, 3n/4, ... -- every prefix spread over the whole range.
    int bits = 0;
    while ((std::size_t{1} << bits) < count) ++bits;
    const std::size_t span = std::size_t{1} << bits;
    for (std::size_t i = 0; i < span; ++i) {
        std::size_t reversed = 0;
        for (int b = 0; b < bits; ++b) {
            if (i & (std::size_t{1} << b)) reversed |= std::size_t{1} << (bits - 1 - b);
        }
        if (reversed < count) order.push_back(static_cast<std::uint32_t>(reversed));
    }
    return order;
}

namespace {

/// Writes one strand's vertices at `base`. Shared by build_geometry() and
/// write_strand_vertices() so the two can never disagree about the layout.
void emit_strand_vertices(const StrandSet& set, const HairParams& params, std::size_t strand_index,
                          Vertex* out) {
    const StrandRoot& root   = set.roots[strand_index];
    const glm::vec3*  p      = set.strand(strand_index);
    const int         points = set.points_per_strand;
    const int         steps  = points - 1;
    const float       inv    = 1.0f / static_cast<float>(steps);

    // The frame, transported along the strand. The first one comes from curve::frame_from_tangent
    // so a strand's roll convention matches everything else built on a curve (+X on the tangent).
    glm::vec3 tangent = glm::normalize(p[1] - p[0]);
    if (!(glm::length(p[1] - p[0]) > 1e-7f)) tangent = root.direction;
    const glm::quat q0 = curve::frame_from_tangent(tangent, 0.0f);
    glm::vec3 u = q0 * glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 v = q0 * glm::vec3(0.0f, 0.0f, 1.0f);

    std::size_t w = 0;
    for (int k = 0; k < points; ++k) {
        const float t = static_cast<float>(k) * inv;

        if (k > 0) {
            const glm::vec3 raw = p[k] - p[k - 1];
            const float     len = glm::length(raw);
            if (len > 1e-7f) {
                const glm::vec3 next = raw / len;
                transport(u, v, tangent, next);
                tangent = next;
            }
        }

        const float half = 0.5f * params.thickness * params.thickness_over_length.evaluate(t);
        const glm::vec4 tan4(tangent, 1.0f);  // w is handedness: B = cross(N, T) * w

        if (params.shape == HairShape::Tube) {
            const int sides = params.sides;
            for (int s = 0; s <= sides; ++s) {
                const float a = glm::two_pi<float>() * static_cast<float>(s % sides) /
                                static_cast<float>(sides);
                const glm::vec3 radial = u * std::cos(a) + v * std::sin(a);
                Vertex& vert = out[w++];
                vert.position = p[k] + radial * half;
                vert.normal   = radial;
                vert.uv       = glm::vec2(t, root.random);
                vert.tangent  = tan4;
            }
        } else {
            for (int plane = 0; plane < params.cross_section; ++plane) {
                const float a = glm::pi<float>() * static_cast<float>(plane) /
                                static_cast<float>(params.cross_section);
                const glm::vec3 side     = u * std::cos(a) + v * std::sin(a);
                const glm::vec3 plane_n  = -u * std::sin(a) + v * std::cos(a);
                for (int edge = 0; edge < 2; ++edge) {
                    const float sign = edge == 0 ? -1.0f : 1.0f;
                    Vertex& vert = out[w++];
                    vert.position = p[k] + side * (half * sign);
                    // Leaned outward from the strand axis, not flat on the plane: a crossed card
                    // whose plane points at the light goes black otherwise -- what the tree's leaf
                    // cards hit, and what their outward-blown normals fixed.
                    vert.normal  = glm::normalize(plane_n * 0.45f + side * (sign * 0.55f));
                    vert.uv      = glm::vec2(t, root.random);
                    vert.tangent = tan4;
                }
            }
        }
    }
}

} // namespace

void build_geometry(const StrandSet& set, const HairParams& params, MeshCpuData& out) {
    out = MeshCpuData{};
    if (set.empty()) return;

    const std::size_t strands = set.strand_count();
    const std::size_t vps     = vertices_per_strand(params);
    const std::size_t ips     = indices_per_strand(params);
    const int         points  = set.points_per_strand;
    const int         steps   = points - 1;

    // Vertices in natural strand order, so the simulated path can rewrite strand i's block at
    // i * vps and touch nothing else.
    out.vertices.resize(strands * vps);
    for (std::size_t i = 0; i < strands; ++i) {
        emit_strand_vertices(set, params, i, out.vertices.data() + i * vps);
    }

    // Indices in bit-reversed strand order, so any prefix is an even thinning of the groom.
    const std::vector<std::uint32_t> order = strand_lod_order(strands);
    out.indices.reserve(strands * ips);
    for (const std::uint32_t strand : order) {
        const auto base = static_cast<std::uint32_t>(strand * vps);
        if (params.shape == HairShape::Tube) {
            const std::uint32_t ring = static_cast<std::uint32_t>(params.sides) + 1u;
            for (int k = 0; k < steps; ++k) {
                for (int s = 0; s < params.sides; ++s) {
                    const std::uint32_t a = base + static_cast<std::uint32_t>(k) * ring + static_cast<std::uint32_t>(s);
                    const std::uint32_t b = a + 1u;
                    const std::uint32_t c = a + ring;
                    const std::uint32_t d = c + 1u;
                    out.indices.insert(out.indices.end(), {a, c, b, b, c, d});
                }
            }
        } else {
            const std::uint32_t stride = 2u * static_cast<std::uint32_t>(params.cross_section);
            for (int plane = 0; plane < params.cross_section; ++plane) {
                const auto off = base + 2u * static_cast<std::uint32_t>(plane);
                for (int k = 0; k < steps; ++k) {
                    const std::uint32_t a = off + static_cast<std::uint32_t>(k) * stride;
                    const std::uint32_t b = a + 1u;
                    const std::uint32_t c = a + stride;
                    const std::uint32_t d = c + 1u;
                    out.indices.insert(out.indices.end(), {a, c, b, b, c, d});
                }
            }
        }
    }

    out.bounds_min = glm::vec3(std::numeric_limits<float>::max());
    out.bounds_max = glm::vec3(std::numeric_limits<float>::lowest());
    for (const Vertex& vert : out.vertices) {
        out.bounds_min = glm::min(out.bounds_min, vert.position);
        out.bounds_max = glm::max(out.bounds_max, vert.position);
    }

    // LOD 0 is the whole groom; each further level halves the strand count by taking a shorter
    // prefix of the same index buffer, so the levels cost no memory at all.
    out.lods.push_back({0u, static_cast<std::uint32_t>(out.indices.size()), 0, 0.0f, {}});
    const float thresholds[2] = {params.lod_screen_sizes.x, params.lod_screen_sizes.y};
    std::size_t level_strands = strands;
    for (const float threshold : thresholds) {
        if (threshold <= 0.0f) break;
        level_strands = std::max<std::size_t>(1u, level_strands / 2u);
        out.lods.push_back({0u, static_cast<std::uint32_t>(level_strands * ips), 0, threshold, {}});
        if (level_strands <= 1u) break;
    }
    out.cull_screen_size = params.cull_screen_size;
}

void write_strand_vertices(const StrandSet& set, const HairParams& params,
                           std::size_t strand_index, std::vector<Vertex>& vertices) {
    if (strand_index >= set.strand_count()) return;
    const std::size_t vps  = vertices_per_strand(params);
    const std::size_t base = strand_index * vps;
    if (vertices.size() < base + vps) return;
    emit_strand_vertices(set, params, strand_index, vertices.data() + base);
}

} // namespace hair
} // namespace toy
