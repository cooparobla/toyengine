#include "editor/mesh/shader_ball.h"

namespace toy {
namespace editor {
namespace detail {

void add_facing(EditMesh& m, std::vector<Corner> corners, const glm::vec3& outward, bool smooth) {
    glm::vec3 n(0.0f);   // Newell normal
    for (size_t i = 0; i < corners.size(); ++i) {
        const glm::vec3& a = m.positions[corners[i].v];
        const glm::vec3& b = m.positions[corners[(i + 1) % corners.size()].v];
        n += glm::vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
    }
    if (glm::dot(n, outward) < 0.0f) std::reverse(corners.begin(), corners.end());
    Face f;
    f.smooth = smooth;
    f.corners = std::move(corners);
    m.faces.push_back(std::move(f));
}

void append_offset(EditMesh& dst, const EditMesh& src, const glm::vec3& offset) {
    const uint32_t base = static_cast<uint32_t>(dst.positions.size());
    for (const auto& p : src.positions) dst.positions.push_back(p + offset);
    for (Face f : src.faces) {
        for (auto& c : f.corners) c.v += base;
        dst.faces.push_back(std::move(f));
    }
}

void add_chamfered_slab(EditMesh& m, float half, float z0, float z1, float chamfer) {
    const float zc = z1 - chamfer;
    const float hs[3] = {half, half, half - chamfer};
    const float zs[3] = {z0, zc, z1};
    uint32_t ring[3][4];
    static const glm::vec2 kCorner[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k) ring[r][k] = add_vertex(m, glm::vec3(kCorner[k] * hs[r], zs[r]));
    const glm::vec3 mid(0.0f, 0.0f, (z0 + z1) * 0.5f);
    auto uv = [&](uint32_t v) { const glm::vec3& p = m.positions[v]; return glm::vec2(p.x + p.y, p.z) * 1.5f; };
    for (int r = 0; r < 2; ++r) {
        for (int k = 0; k < 4; ++k) {
            const uint32_t a = ring[r][k], b = ring[r][(k + 1) % 4], c = ring[r + 1][(k + 1) % 4], d = ring[r + 1][k];
            const glm::vec3 centre = (m.positions[a] + m.positions[b] + m.positions[c] + m.positions[d]) * 0.25f;
            add_facing(m, {{a, uv(a)}, {b, uv(b)}, {c, uv(c)}, {d, uv(d)}}, centre - mid, false);
        }
    }
    auto top_uv = [&](uint32_t v) { const glm::vec3& p = m.positions[v]; return glm::vec2(p.x, p.y) + glm::vec2(0.5f); };
    add_facing(m, {{ring[2][0], top_uv(ring[2][0])}, {ring[2][1], top_uv(ring[2][1])}, {ring[2][2], top_uv(ring[2][2])},
                   {ring[2][3], top_uv(ring[2][3])}}, glm::vec3(0, 0, 1), false);
    add_facing(m, {{ring[0][0], top_uv(ring[0][0])}, {ring[0][1], top_uv(ring[0][1])}, {ring[0][2], top_uv(ring[0][2])},
                   {ring[0][3], top_uv(ring[0][3])}}, glm::vec3(0, 0, -1), false);
}

} // namespace detail
} // namespace editor
} // namespace toy

namespace toy {
namespace editor {

EditMesh make_shader_ball(float cut_azimuth_deg) {
    using detail::add_facing;
    EditMesh m;
    const float R = 0.5f, r_in = 0.43f, r_core = 0.34f;
    const int S = 64, N = 32;   // segments around, rings pole to pole
    const int cut_w = S / 4;    // a quarter turn
    const int a0 = ((static_cast<int>(std::lround(cut_azimuth_deg / 360.0f * S)) - cut_w / 2) % S + S) % S;
    auto in_cut = [&](int i, int j) { return ((i - a0 + S) % S) < cut_w && j >= N / 2; };

    // Shell vertices: outer and inner spheres on the same grid (poles are single vertices).
    auto build_sphere = [&](float rad, std::vector<uint32_t>& grid, uint32_t& south, uint32_t& north) {
        south = detail::add_vertex(m, {0, 0, -rad});
        north = detail::add_vertex(m, {0, 0, rad});
        grid.assign(static_cast<size_t>(S) * (N + 1), 0);
        for (int j = 1; j < N; ++j) {
            const float el = detail::kPi * (static_cast<float>(j) / N - 0.5f);
            for (int i = 0; i < S; ++i) {
                const float az = 2.0f * detail::kPi * i / S;
                grid[static_cast<size_t>(j) * S + i] =
                    detail::add_vertex(m, rad * glm::vec3(std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el)));
            }
        }
    };
    std::vector<uint32_t> og, ig;
    uint32_t os, on, is, in_;
    build_sphere(R, og, os, on);
    build_sphere(r_in, ig, is, in_);
    auto vid = [&](const std::vector<uint32_t>& g, uint32_t south, uint32_t north, int i, int j) {
        return j <= 0 ? south : j >= N ? north : g[static_cast<size_t>(j) * S + ((i % S) + S) % S];
    };
    auto uv = [&](int i, int j) { return glm::vec2(static_cast<float>(i) / S, static_cast<float>(j) / N); };

    // Outer (facing out) and inner (facing in) surfaces, minus the cut.
    for (int pass = 0; pass < 2; ++pass) {
        const std::vector<uint32_t>& g = pass == 0 ? og : ig;
        const uint32_t s = pass == 0 ? os : is, n = pass == 0 ? on : in_;
        const float sign = pass == 0 ? 1.0f : -1.0f;
        for (int j = 0; j < N; ++j) {
            for (int i = 0; i < S; ++i) {
                if (in_cut(i, j)) continue;
                std::vector<Corner> c;
                auto push = [&](int ii, int jj) {
                    const uint32_t v = vid(g, s, n, ii, jj);
                    for (const auto& e : c) if (e.v == v) return;   // collapsed pole corner
                    c.push_back({v, uv(ii, jj)});
                };
                push(i, j); push(i + 1, j); push(i + 1, j + 1); push(i, j + 1);
                glm::vec3 centre(0.0f);
                for (const auto& e : c) centre += m.positions[e.v];
                add_facing(m, c, centre * sign, true);
            }
        }
    }

    // Flat cut walls joining the two surfaces: two meridian walls and the equatorial floor.
    const float az0 = 2.0f * detail::kPi * a0 / S, az1 = 2.0f * detail::kPi * (a0 + cut_w) / S;
    const glm::vec3 facing0(-std::sin(az0), std::cos(az0), 0.0f), facing1(std::sin(az1), -std::cos(az1), 0.0f);
    for (int j = N / 2; j < N; ++j) {
        for (int w = 0; w < 2; ++w) {
            const int i = w == 0 ? a0 : a0 + cut_w;
            const uint32_t o0 = vid(og, os, on, i, j), o1 = vid(og, os, on, i, j + 1);
            const uint32_t i0 = vid(ig, is, in_, i, j), i1 = vid(ig, is, in_, i, j + 1);
            auto wuv = [&](uint32_t v) { const glm::vec3& p = m.positions[v]; return glm::vec2(glm::length(glm::vec2(p)), p.z) * 2.0f; };
            add_facing(m, {{o0, wuv(o0)}, {o1, wuv(o1)}, {i1, wuv(i1)}, {i0, wuv(i0)}}, w == 0 ? facing0 : facing1, false);
        }
    }
    for (int k = 0; k < cut_w; ++k) {
        const int i = a0 + k, j = N / 2;
        const uint32_t o0 = vid(og, os, on, i, j), o1 = vid(og, os, on, i + 1, j);
        const uint32_t i0 = vid(ig, is, in_, i, j), i1 = vid(ig, is, in_, i + 1, j);
        auto fuv = [&](uint32_t v) { const glm::vec3& p = m.positions[v]; return glm::vec2(p.x, p.y) * 2.0f; };
        add_facing(m, {{o0, fuv(o0)}, {o1, fuv(o1)}, {i1, fuv(i1)}, {i0, fuv(i0)}}, glm::vec3(0, 0, 1), false);
    }

    // The core ball inside, the neck, and the stepped square plinth.
    detail::append_offset(m, make_uv_sphere(r_core, 48, 24), glm::vec3(0.0f));
    const float neck_top = -R + 0.06f, plinth_top = -R - 0.12f;
    detail::append_offset(m, make_cylinder(0.13f, neck_top - plinth_top, 32, true), glm::vec3(0, 0, (neck_top + plinth_top) * 0.5f));
    detail::add_chamfered_slab(m, 0.36f, plinth_top - 0.09f, plinth_top, 0.03f);
    detail::add_chamfered_slab(m, 0.48f, plinth_top - 0.17f, plinth_top - 0.09f, 0.025f);
    return m;
}

} // namespace editor
} // namespace toy
