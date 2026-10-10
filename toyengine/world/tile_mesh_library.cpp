#include <toyengine/world/tile_mesh_library.h>

#include <gfxcoopa/engine/data/mesh.h>
#include <toyengine/world/tile_types.h>

namespace toy {
namespace world {

glm::vec2 TileMeshLibrary::encode_uv(TileKind kind, const glm::vec2& local) {
    return glm::vec2(static_cast<float>(kind) * k_uv_cell_stride + 0.5f * k_uv_cell_stride + local.x,
                     local.y);
}

void TileMeshLibrary::bake_canonical(const SkinnedMeshSource& canonical) {
    for (std::size_t i = 0; i < k_tile_face_count; ++i) {
        bake_side(static_cast<TileFace>(i), canonical);
    }
}

void TileMeshLibrary::bake_side(TileFace face, const SkinnedMeshSource& canonical) {
    SideGeometry& out = sides_[static_cast<std::size_t>(face)];
    out.vertices.clear();
    out.indices = canonical.indices;

    const glm::mat4 transform = face_transform(face);
    // A pure rotation, so the inverse transpose is the rotation itself -- normals and
    // tangents take the same matrix as positions, with no renormalisation needed.
    const glm::mat3 rotation(transform);

    out.vertices.reserve(canonical.vertices.size());
    for (const Vertex& v : canonical.vertices) {
        Vertex rotated = v;
        rotated.position = glm::vec3(transform * glm::vec4(v.position, 1.0f));
        rotated.normal   = rotation * v.normal;
        rotated.tangent  = glm::vec4(rotation * glm::vec3(v.tangent), v.tangent.w);
        out.vertices.push_back(rotated);
    }
    analyse_mergeable_(out, face);
}

bool TileMeshLibrary::is_baked() const {
    for (const SideGeometry& side : sides_) {
        if (side.empty()) return false;
    }
    return true;
}

void TileMeshLibrary::append(TileFace face, const glm::vec3& origin, const glm::vec3& scale, TileKind kind,
            std::vector<Vertex>& out_v, std::vector<std::uint32_t>& out_i,
            bool encode_terrain_uv) const {
    const SideGeometry& side = sides_[static_cast<std::size_t>(face)];
    if (side.empty()) return;

    const std::uint32_t base = static_cast<std::uint32_t>(out_v.size());
    const glm::vec4 cell = atlas_cell(kind);
    const glm::vec3 normal_scale(1.0f / scale.x, 1.0f / scale.y, 1.0f / scale.z);

    for (const Vertex& v : side.vertices) {
        Vertex placed;
        placed.position = origin + v.position * scale;
        placed.normal   = glm::normalize(v.normal * normal_scale);

        const glm::vec3 tangent = glm::vec3(v.tangent) * scale; // a tangent scales, unlike a normal
        const float tangent_length = glm::length(tangent);
        placed.tangent = glm::vec4(tangent_length > 1e-6f ? tangent / tangent_length
                                                          : glm::vec3(1.0f, 0.0f, 0.0f),
                                   v.tangent.w);

        // encode_terrain_uv: tile-space UVs for a chunk drawn with the `terrain` shader
        // (see encode_uv()) instead of the atlas cell baked into the UV.
        placed.uv = encode_terrain_uv ? encode_uv(kind, v.uv)
                                      : glm::vec2(cell.x + v.uv.x * cell.z, cell.y + v.uv.y * cell.w);
        out_v.push_back(placed);
    }

    for (std::uint32_t index : side.indices) out_i.push_back(base + index);
}

void TileMeshLibrary::append_span(TileFace face, const glm::vec3& origin, const glm::vec3& scale,
                 const glm::vec3& span, TileKind kind,
                 std::vector<Vertex>& out_v, std::vector<std::uint32_t>& out_i) const {
    const SideGeometry& side = sides_[static_cast<std::size_t>(face)];
    const std::uint32_t base = static_cast<std::uint32_t>(out_v.size());
    for (const Vertex& v : side.vertices) {
        Vertex placed = v;   // flat and axis-aligned: normal and tangent carry over as is
        placed.position = origin + v.position * scale * span;
        const glm::vec2 local = side.uv_origin +
                                side.uv_du * (v.position[side.axis_u] * span[side.axis_u]) +
                                side.uv_dv * (v.position[side.axis_v] * span[side.axis_v]);
        placed.uv = encode_uv(kind, local);
        out_v.push_back(placed);
    }
    for (std::uint32_t index : side.indices) out_i.push_back(base + index);
}

std::uint8_t TileMeshLibrary::add_style() {
    styles_.emplace_back();
    bake_fixed_pieces_();
    return static_cast<std::uint8_t>(styles_.size() - 1);
}

std::uint8_t TileMeshLibrary::style_of(TileKind kind) const {
    const std::uint8_t s = kind_style_[static_cast<std::size_t>(kind)];
    return s < styles_.size() ? s : 0;
}

void TileMeshLibrary::bake_style_piece(std::uint8_t style, TilePiece piece, const SkinnedMeshSource& source) {
    StyleSet& set = styles_[style];
    const std::size_t p = static_cast<std::size_t>(piece);
    const std::vector<Vertex> soup = triangle_soup_(source);
    bake_oriented_(soup, set.pieces[p]);
    set.authored[p] = true;
    for (auto& plane : set.sections[p]) for (auto& g : plane) g = SideGeometry{};

    if (piece == TilePiece::WallCapContinue || piece == TilePiece::WallContinue) {
        // Straight halves are extrusions along x, so x in [0.5,1] can be remapped to [0,1]
        // and then stretched freely: a straight run of any length is one strip.
        std::vector<Vertex> unit = soup;
        for (Vertex& v : unit) v.position.x = (v.position.x - 0.5f) * 2.0f;
        Oriented turned;
        bake_oriented_(unit, turned);
        for (std::size_t q = 0; q < 4; ++q) set.straight[piece == TilePiece::WallCapContinue ? 1 : 0][q] = turned[q];
    }

    const bool taper = piece == TilePiece::WallTaperConcave;
    const bool taper_continue = piece == TilePiece::WallTaperContinue;
    const bool wall = taper || taper_continue ||
                      (piece >= TilePiece::WallCapContinue && piece <= TilePiece::WallConcave);
    if (wall) {
        const WallEnd end = taper ? WallEnd::Concave : taper_continue ? WallEnd::Continue : static_cast<WallEnd>(
            (static_cast<std::uint8_t>(piece) - static_cast<std::uint8_t>(TilePiece::WallCapContinue)) % 3u);
        // The taper pieces are cap-like: their tops are finished surface, nothing stacks on them.
        const bool cap = taper || taper_continue || piece <= TilePiece::WallCapConcave;
        const bool concave = end == WallEnd::Concave;
        for (int top = 0; top < 2; ++top) {
            if (top && cap) continue; // a cap is the top of its wall; nothing sits on it
            const float z = top ? 1.0f : 0.0f;
            const glm::vec3 corner(1.0f, 1.0f, z);
            const glm::vec3 start(0.5f, 1.0f, z);
            bake_oriented_(derive_section_(soup, glm::vec3(0.0f, 0.0f, z),
                                           glm::vec3(0.0f, 0.0f, top ? 1.0f : -1.0f),
                                           glm::vec3(0.5f, 0.5f, z), &start, concave ? &corner : nullptr),
                           set.sections[p][static_cast<std::size_t>(top ? SectionPlane::Top : SectionPlane::Bottom)]);
        }
        if (taper) {
            // Its top is finished surface over the fillet, but where the taller wall above
            // it changes shape (wraps a convex corner, say) its own footprint still needs a
            // floor: the half-footprint triangle, kept clear of the fill.
            Vertex v;
            v.normal  = glm::vec3(0.0f, 0.0f, 1.0f);
            v.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            std::vector<Vertex> tri(3, v);
            tri[0].position = glm::vec3(0.5f, 0.5f, 1.0f);
            tri[1].position = glm::vec3(1.0f, 1.0f, 1.0f);
            tri[2].position = glm::vec3(0.5f, 1.0f, 1.0f);
            bake_oriented_(tri, set.sections[p][static_cast<std::size_t>(SectionPlane::Top)]);
        }
        Oriented& end_section = set.sections[p][static_cast<std::size_t>(SectionPlane::End)];
        if (end == WallEnd::Continue) {
            const glm::vec3 a(1.0f, 0.5f, 0.0f), b(1.0f, 0.5f, 1.0f);
            bake_oriented_(derive_section_(soup, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f),
                                           glm::vec3(1.0f, 0.5f, 0.5f), &a, &b),
                           end_section);
        } else if (end == WallEnd::Concave) {
            const glm::vec3 a(1.0f, 1.0f, 0.0f), b(1.0f, 1.0f, 1.0f);
            bake_oriented_(derive_section_(soup, glm::vec3(1.0f, 1.0f, 0.0f),
                                           glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f)),
                                           glm::vec3(1.0f, 1.0f, 0.5f), &a, &b),
                           end_section);
        }
    }
}

void TileMeshLibrary::finish_styles() {
    static const std::pair<TilePiece, TilePiece> fallbacks[] = {
        {TilePiece::TopEdge, TilePiece::TopInner},       {TilePiece::TopOuter, TilePiece::TopEdge},
        {TilePiece::WallConvex, TilePiece::WallContinue}, {TilePiece::WallConcave, TilePiece::WallContinue},
        {TilePiece::WallCapContinue, TilePiece::WallContinue}, {TilePiece::WallCapConvex, TilePiece::WallConvex},
        {TilePiece::WallCapConcave, TilePiece::WallConcave},
        {TilePiece::WallTaperConcave, TilePiece::WallConcave},
        {TilePiece::WallTaperContinue, TilePiece::WallCapContinue}};
    for (StyleSet& set : styles_) {
        for (const auto& [piece, from] : fallbacks) {
            const std::size_t p = static_cast<std::size_t>(piece);
            const std::size_t f = static_cast<std::size_t>(from);
            if (set.authored[p] || set.pieces[f][0].empty()) continue;
            set.pieces[p]   = set.pieces[f];
            set.sections[p] = set.sections[f];
            if (piece == TilePiece::WallCapContinue) set.straight[1] = set.straight[0];
        }
    }
}

const TileMeshLibrary::SideGeometry& TileMeshLibrary::section(std::uint8_t style, TilePiece piece, SectionPlane plane,
                            std::uint8_t orientation) const {
    return styles_[style].sections[static_cast<std::size_t>(piece)][static_cast<std::size_t>(plane)]
                                  [orientation & 7u];
}

void TileMeshLibrary::append_styled(const SideGeometry& geometry, const glm::vec3& origin, const glm::vec3& scale,
                   float blend_code, bool top_anchored,
                   std::vector<Vertex>& out_v, std::vector<std::uint32_t>& out_i,
                   int stretch_axis, float stretch) const {
    if (geometry.empty()) return;
    const std::uint32_t base = static_cast<std::uint32_t>(out_v.size());
    const float tile   = scale.x;
    const float height = scale.z;
    const bool anchored = top_anchored && height >= 0.5f * tile;
    const float lower   = std::max((height - 0.5f * tile) / 0.5f, 1e-3f);

    // No reserve(size + n) here: called thousands of times per chunk, an exact-size reserve
    // defeats the vector's geometric growth and turns the whole mesh quadratic.
    for (std::size_t i = 0; i < geometry.vertices.size(); ++i) {
        const Vertex& v = geometry.vertices[i];
        glm::vec3 s = scale;
        float z = v.position.z * height;
        if (anchored) {
            if (v.position.z >= 0.5f) {
                z = height - (1.0f - v.position.z) * tile;
                s = glm::vec3(tile);
            } else {
                z = v.position.z * lower;
                s = glm::vec3(tile, tile, lower);
            }
        }
        Vertex placed;
        glm::vec3 local(v.position.x * tile, v.position.y * scale.y, z);
        local[stretch_axis] *= stretch;
        placed.position = origin + local;
        placed.normal   = glm::normalize(v.normal / s);
        // Any tangent orthogonal to the normal, chosen from the normal alone: nothing samples
        // a normal map here, and a deterministic tangent lets equal corners weld.
        const glm::vec3 axis = std::abs(placed.normal.z) < 0.9f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                                : glm::vec3(1.0f, 0.0f, 0.0f);
        placed.tangent = glm::vec4(glm::normalize(glm::cross(axis, placed.normal)), 1.0f);
        // One code per stamp: corners weld, and terrain_styled.frag does the rest.
        placed.uv = glm::vec2(blend_code, 0.0f);
        out_v.push_back(placed);
    }
    for (std::uint32_t index : geometry.indices) out_i.push_back(base + index);
}

std::vector<TileMeshLibrary::Vertex> TileMeshLibrary::triangle_soup_(const SkinnedMeshSource& source) {
    std::vector<Vertex> soup;
    soup.reserve(source.indices.size());
    for (std::uint32_t index : source.indices) {
        if (index < source.vertices.size()) soup.push_back(source.vertices[index]);
    }
    soup.resize(soup.size() - soup.size() % 3);
    return soup;
}

void TileMeshLibrary::bake_oriented_(const std::vector<Vertex>& soup, Oriented& out) {
    for (std::uint8_t o = 0; o < k_tile_orientation_count; ++o) {
        SideGeometry& g = out[o];
        g = SideGeometry{};
        const glm::mat4& m = variant_transform(o);
        const glm::mat3 r(m);
        const bool mirrored = orientation_mirrors(o);
        g.vertices.reserve(soup.size());
        for (std::size_t t = 0; t + 2 < soup.size(); t += 3) {
            Vertex v[3];
            for (int k = 0; k < 3; ++k) {
                v[k] = soup[t + static_cast<std::size_t>(k)];
                v[k].position = glm::vec3(m * glm::vec4(v[k].position, 1.0f));
                const glm::vec3 n = r * v[k].normal;
                v[k].normal = glm::length(n) > 1e-6f ? glm::normalize(n) : glm::vec3(0.0f, 0.0f, 1.0f);
            }
            if (mirrored) std::swap(v[1], v[2]);
            const glm::vec3 cross = glm::cross(v[1].position - v[0].position, v[2].position - v[0].position);
            const float area = glm::length(cross);
            if (area < 1e-9f) continue;
            const std::uint32_t base = static_cast<std::uint32_t>(g.vertices.size());
            for (const Vertex& vert : v) g.vertices.push_back(vert);
            g.indices.push_back(base);
            g.indices.push_back(base + 1);
            g.indices.push_back(base + 2);
        }
    }
}

std::vector<TileMeshLibrary::Vertex> TileMeshLibrary::derive_section_(const std::vector<Vertex>& soup, const glm::vec3& point,
                                           const glm::vec3& normal, const glm::vec3& ref,
                                           const glm::vec3* anchor_start, const glm::vec3* anchor_end) {
    std::vector<glm::vec3> polygon;
    if (anchor_start) polygon.push_back(*anchor_start);
    const std::vector<glm::vec3> chain = boundary_chain_(soup, point, normal, anchor_start ? *anchor_start : ref);
    polygon.insert(polygon.end(), chain.begin(), chain.end());
    if (anchor_end) polygon.push_back(*anchor_end);
    return fan_(polygon, ref, normal);
}

std::vector<glm::vec3> TileMeshLibrary::boundary_chain_(const std::vector<Vertex>& soup, const glm::vec3& point,
                                              const glm::vec3& normal, const glm::vec3& start) {
    constexpr float k_eps = 1e-3f;
    std::vector<glm::vec3> welded;
    std::map<std::array<long, 3>, std::uint32_t> ids;
    auto id_of = [&](const glm::vec3& p) {
        const std::array<long, 3> key = {std::lround(p.x * 1e4f), std::lround(p.y * 1e4f), std::lround(p.z * 1e4f)};
        auto it = ids.find(key);
        if (it != ids.end()) return it->second;
        const std::uint32_t id = static_cast<std::uint32_t>(welded.size());
        welded.push_back(p);
        ids.emplace(key, id);
        return id;
    };
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> edge_use;
    for (std::size_t t = 0; t + 2 < soup.size(); t += 3) {
        const std::uint32_t a = id_of(soup[t].position);
        const std::uint32_t b = id_of(soup[t + 1].position);
        const std::uint32_t c = id_of(soup[t + 2].position);
        for (auto [x, y] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}}) {
            if (x == y) continue;
            ++edge_use[{std::min(x, y), std::max(x, y)}];
        }
    }
    auto on_plane = [&](std::uint32_t id) { return std::abs(glm::dot(welded[id] - point, normal)) < k_eps; };
    std::map<std::uint32_t, std::vector<std::uint32_t>> adjacency;
    for (const auto& [edge, uses] : edge_use) {
        if (uses != 1 || !on_plane(edge.first) || !on_plane(edge.second)) continue;
        adjacency[edge.first].push_back(edge.second);
        adjacency[edge.second].push_back(edge.first);
    }

    // Chains: walk from each unvisited endpoint (degree 1), then any leftover loops.
    std::vector<std::vector<glm::vec3>> chains;
    std::map<std::uint32_t, bool> visited;
    auto walk = [&](std::uint32_t from) {
        std::vector<glm::vec3> chain;
        std::uint32_t current = from;
        std::uint32_t previous = UINT32_MAX;
        while (true) {
            visited[current] = true;
            chain.push_back(welded[current]);
            std::uint32_t next = UINT32_MAX;
            for (std::uint32_t n : adjacency[current]) {
                if (n != previous && !visited[n]) { next = n; break; }
            }
            if (next == UINT32_MAX) break;
            previous = current;
            current = next;
        }
        chains.push_back(std::move(chain));
    };
    for (const auto& [id, nbrs] : adjacency) if (nbrs.size() == 1 && !visited[id]) walk(id);
    for (const auto& [id, nbrs] : adjacency) if (!visited[id]) walk(id);

    // Concatenate greedily by nearest endpoint, starting from `start`.
    std::vector<glm::vec3> polygon;
    glm::vec3 cursor = start;
    std::vector<bool> used(chains.size(), false);
    for (std::size_t n = 0; n < chains.size(); ++n) {
        std::size_t best = SIZE_MAX;
        bool reversed = false;
        float best_d = 1e30f;
        for (std::size_t c = 0; c < chains.size(); ++c) {
            if (used[c]) continue;
            const float df = glm::length(chains[c].front() - cursor);
            const float db = glm::length(chains[c].back() - cursor);
            if (df < best_d) { best_d = df; best = c; reversed = false; }
            if (db < best_d) { best_d = db; best = c; reversed = true; }
        }
        used[best] = true;
        std::vector<glm::vec3>& chain = chains[best];
        if (reversed) std::reverse(chain.begin(), chain.end());
        polygon.insert(polygon.end(), chain.begin(), chain.end());
        cursor = chain.back();
    }
    return polygon;
}

std::vector<TileMeshLibrary::Vertex> TileMeshLibrary::fan_(const std::vector<glm::vec3>& polygon, const glm::vec3& ref,
                                const glm::vec3& normal) {
    std::vector<Vertex> out;
    for (std::size_t i = 0; i + 1 < polygon.size(); ++i) {
        glm::vec3 a = polygon[i];
        glm::vec3 b = polygon[i + 1];
        const glm::vec3 cross = glm::cross(a - ref, b - ref);
        if (glm::length(cross) < 1e-7f) continue;
        if (glm::dot(cross, normal) < 0.0f) std::swap(a, b);
        for (const glm::vec3& p : {ref, a, b}) {
            Vertex v;
            v.position = p;
            v.normal   = normal;
            v.uv       = glm::vec2(0.0f);
            v.tangent  = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            out.push_back(v);
        }
    }
    return out;
}

void TileMeshLibrary::bake_fixed_pieces_() {
    if (!full_top_.empty()) return;
    auto vertex = [](const glm::vec3& p) {
        Vertex v;
        v.position = p;
        v.normal   = glm::vec3(0.0f, 0.0f, 1.0f);
        v.uv       = glm::vec2(p.x, p.y);
        v.tangent  = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
        return v;
    };
    const std::vector<Vertex> quad = {vertex({0, 0, 1}), vertex({1, 0, 1}), vertex({1, 1, 1}),
                                      vertex({0, 0, 1}), vertex({1, 1, 1}), vertex({0, 1, 1})};
    Oriented quads;
    bake_oriented_(quad, quads);
    full_top_ = quads[0];
    const std::vector<Vertex> foot = {vertex({0.5f, 0.5f, 0}), vertex({1, 1, 0}), vertex({0.5f, 1, 0})};
    bake_oriented_(foot, foot_);

}

void TileMeshLibrary::analyse_mergeable_(SideGeometry& side, TileFace face) {
    side.mergeable = false;
    if (side.empty()) return;
    const glm::vec3 n = face_normal(face);
    const int axis_n = (std::abs(n.x) > 0.5f) ? 0 : (std::abs(n.y) > 0.5f) ? 1 : 2;
    side.axis_u = (axis_n == 0) ? 1 : 0;
    side.axis_v = (axis_n == 2) ? 1 : 2;
    const float eps = 1e-4f;
    auto snap = [eps](float x, int& out) {
        if (std::abs(x) < eps)        { out = 0; return true; }
        if (std::abs(x - 1.0f) < eps) { out = 1; return true; }
        return false;
    };
    const Vertex* corner[2][2] = {{nullptr, nullptr}, {nullptr, nullptr}};
    const float plane = side.vertices[0].position[axis_n];
    for (const Vertex& v : side.vertices) {
        int pu = 0, pv = 0;
        if (std::abs(v.position[axis_n] - plane) > eps) return;
        if (!snap(v.position[side.axis_u], pu) || !snap(v.position[side.axis_v], pv)) return;
        corner[pu][pv] = &v;
    }
    if (!corner[0][0] || !corner[1][0] || !corner[0][1]) return;
    side.uv_origin = corner[0][0]->uv;
    side.uv_du     = corner[1][0]->uv - side.uv_origin;
    side.uv_dv     = corner[0][1]->uv - side.uv_origin;
    for (const Vertex& v : side.vertices) {
        const glm::vec2 fit = side.uv_origin + side.uv_du * v.position[side.axis_u] +
                              side.uv_dv * v.position[side.axis_v];
        if (glm::length(fit - v.uv) > 1e-3f) return;
    }
    side.mergeable = true;
}

} // namespace world
} // namespace toy
