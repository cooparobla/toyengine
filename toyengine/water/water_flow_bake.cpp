#include <toyengine/water/water_flow_bake.h>

namespace toy {
namespace water {

void weld_by_position(const std::vector<glm::vec3>& positions, const std::vector<glm::vec2>& uvs,
                             const std::vector<uint32_t>& corner_indices,
                             std::vector<glm::vec3>& out_positions, std::vector<glm::vec2>& out_uvs,
                             std::vector<uint32_t>& out_indices, float quantum) {
    struct KeyHash {
        std::size_t operator()(const glm::ivec3& k) const {
            return (static_cast<std::size_t>(k.x) * 73856093u) ^ (static_cast<std::size_t>(k.y) * 19349663u) ^
                   (static_cast<std::size_t>(k.z) * 83492791u);
        }
    };
    std::unordered_map<glm::ivec3, uint32_t, KeyHash> map;
    map.reserve(positions.size());
    std::vector<uint32_t> remap(positions.size());
    out_positions.clear();
    out_uvs.clear();
    for (std::size_t i = 0; i < positions.size(); ++i) {
        glm::ivec3 key(glm::round(positions[i] / quantum));
        auto [it, inserted] = map.emplace(key, static_cast<uint32_t>(out_positions.size()));
        if (inserted) {
            out_positions.push_back(positions[i]);
            out_uvs.push_back(i < uvs.size() ? uvs[i] : glm::vec2(0.0f));
        }
        remap[i] = it->second;
    }
    out_indices.resize(corner_indices.size());
    for (std::size_t i = 0; i < corner_indices.size(); ++i) out_indices[i] = remap[corner_indices[i]];
}

glm::vec3 uv_u_direction(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                                float ua, float ub, float uc) {
    glm::vec3 e1 = b - a, e2 = c - a;
    float du1 = ub - ua, du2 = uc - ua;
    float d11 = glm::dot(e1, e1), d22 = glm::dot(e2, e2), d12 = glm::dot(e1, e2);
    float den = d11 * d22 - d12 * d12;
    if (std::abs(den) < 1e-12f) return glm::vec3(0.0f);
    glm::vec3 g = ((du1 * d22 - du2 * d12) * e1 + (du2 * d11 - du1 * d12) * e2) / den;
    float len = glm::length(g);
    return len > 1e-8f ? g / len : glm::vec3(0.0f);
}

void bake_flow(const std::vector<glm::vec3>& positions, const std::vector<glm::vec2>& uvs,
                      const std::vector<uint32_t>& indices, const FlowBakeParams& params,
                      const FlowRayFn& ray, std::vector<glm::vec3>& out_flow,
                      std::vector<float>& out_turbulence) {
    const std::size_t n = positions.size();
    out_flow.assign(n, glm::vec3(0.0f));
    out_turbulence.assign(n, 0.0f);
    if (n == 0) return;

    // Global UV orientation: a river whose `u` was authored running UPHILL is flipped as a
    // whole (decided by the slope-weighted vote of every triangle), rather than per triangle,
    // which would tear the fallback direction apart wherever the slope vanishes.
    float uv_sign = 1.0f;
    if (!uvs.empty()) {
        float vote = 0.0f;
        for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
            uint32_t ia = indices[t], ib = indices[t + 1], ic = indices[t + 2];
            glm::vec3 along = uv_u_direction(positions[ia], positions[ib], positions[ic],
                                             uvs[ia].x, uvs[ib].x, uvs[ic].x);
            vote += -along.z; // u increasing while z decreases == u points downhill
        }
        if (vote < 0.0f) uv_sign = -1.0f;
    }

    std::vector<float> weight(n, 0.0f);
    std::vector<float> slope_turb(n, 0.0f);
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        uint32_t ia = indices[t], ib = indices[t + 1], ic = indices[t + 2];
        const glm::vec3& a = positions[ia];
        const glm::vec3& b = positions[ib];
        const glm::vec3& c = positions[ic];
        glm::vec3 cr = glm::cross(b - a, c - a);
        float area2 = glm::length(cr);
        if (area2 < 1e-10f) continue;
        glm::vec3 nrm = cr / area2;
        if (nrm.z < 0.0f) nrm = -nrm;

        // Gravity projected onto the plane: |down| == sin(slope angle).
        glm::vec3 down = glm::vec3(0.0f, 0.0f, -1.0f) + nrm * nrm.z;
        float sin_slope = glm::length(down);
        glm::vec3 downhill = sin_slope > 1e-6f ? down / sin_slope : glm::vec3(0.0f);

        glm::vec3 along = glm::vec3(0.0f);
        if (!uvs.empty()) along = uv_sign * uv_u_direction(a, b, c, uvs[ia].x, uvs[ib].x, uvs[ic].x);

        // Trust the slope once it is clearly there (~1.7 degrees+); below that, follow the UVs.
        float trust = glm::clamp((sin_slope - 0.005f) / 0.025f, 0.0f, 1.0f);
        glm::vec3 dir = glm::vec3(0.0f);
        if (glm::length(along) > 0.0f && trust < 1.0f) {
            dir = glm::mix(along, downhill, trust);
        } else {
            dir = downhill;
        }
        float dl = glm::length(dir);
        if (dl < 1e-6f) continue;
        dir /= dl;

        float speed = (params.min_speed + params.slope_gain * std::sqrt(sin_slope)) * params.speed;
        float turb  = glm::clamp((sin_slope - 0.15f) / 0.3f, 0.0f, 1.0f); // white water from ~9 degrees
        float w = area2;
        for (uint32_t i : {ia, ib, ic}) {
            out_flow[i]   += dir * speed * w;
            slope_turb[i] += turb * w;
            weight[i]     += w;
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (weight[i] > 0.0f) {
            out_flow[i] /= weight[i];
            out_turbulence[i] = slope_turb[i] / weight[i];
        }
    }

    // Obstacles: deflect around what is ahead, stir up what is behind. Rays run along the
    // CURRENT (which follows the surface slope), not horizontally -- on a descending river a
    // horizontal ray upstream would run straight into the rising bed. Only near-vertical faces
    // count as obstacles (rocks, posts, piers); a hit on the bed or a gently shelving bank,
    // which a ray along a bending slope can still graze, is ignored.
    constexpr float k_obstacle_max_nz = 0.6f;
    if (ray) {
        for (std::size_t i = 0; i < n; ++i) {
            glm::vec3 v = out_flow[i];
            float speed = glm::length(v);
            glm::vec3 vh(v.x, v.y, 0.0f);
            float sh = glm::length(vh);
            if (sh < 1e-4f || speed < 1e-4f) continue;
            glm::vec3 dir = v / speed;
            // Slightly below the surface, so a rock that only just breaks it still registers.
            glm::vec3 origin = positions[i] - glm::vec3(0.0f, 0.0f, 0.1f);
            FlowRayHit hit;
            if (params.obstacle_radius > 0.0f && ray(origin, dir, params.obstacle_radius, hit) &&
                std::abs(hit.normal.z) < k_obstacle_max_nz) {
                glm::vec3 nh(hit.normal.x, hit.normal.y, 0.0f);
                float nl = glm::length(nh);
                if (nl > 1e-4f) {
                    nh /= nl;
                    float k = 1.0f - glm::clamp(hit.distance / params.obstacle_radius, 0.0f, 1.0f);
                    float into = glm::dot(vh, nh);
                    if (into < 0.0f) {
                        // Remove the into-obstacle part (fully at contact, fading with distance)
                        // and send most of it around the side the flow already leans to; head-on
                        // there is no lean, so pick one consistently (the smoothing pass then
                        // blends neighbours on either side into a proper split).
                        glm::vec3 tang = vh - nh * into;
                        float tl = glm::length(tang);
                        glm::vec3 side = tl > 1e-3f * sh ? tang / tl : glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), nh);
                        v += -nh * into * k + side * (-into) * k * 0.7f;
                        out_turbulence[i] += 0.6f * k * glm::clamp(-into / sh, 0.0f, 1.0f);
                    }
                }
            }
            if (params.wake_length > 0.0f && ray(origin, -dir, params.wake_length, hit) &&
                std::abs(hit.normal.z) < k_obstacle_max_nz) {
                float k = 1.0f - glm::clamp(hit.distance / params.wake_length, 0.0f, 1.0f);
                glm::vec3 nh(hit.normal.x, hit.normal.y, 0.0f);
                float nl = glm::length(nh);
                // Only a surface facing back downstream casts a wake (not a bank we run beside).
                float facing = nl > 1e-4f ? glm::clamp(glm::dot(nh / nl, vh / sh), 0.0f, 1.0f) : 0.0f;
                out_turbulence[i] += 0.75f * k * facing;
                v *= 1.0f - 0.45f * k * facing;
            }
            out_flow[i] = v;
        }
    }

    // Laplacian smoothing over the mesh's adjacency.
    if (params.smooth_passes > 0) {
        std::vector<std::vector<uint32_t>> adj(n);
        for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
            uint32_t tri[3] = {indices[t], indices[t + 1], indices[t + 2]};
            for (int e = 0; e < 3; ++e) {
                uint32_t a = tri[e], b = tri[(e + 1) % 3];
                adj[a].push_back(b);
                adj[b].push_back(a);
            }
        }
        std::vector<glm::vec3> flow_tmp(n);
        std::vector<float> turb_tmp(n);
        for (int pass = 0; pass < params.smooth_passes; ++pass) {
            for (std::size_t i = 0; i < n; ++i) {
                if (adj[i].empty()) {
                    flow_tmp[i] = out_flow[i];
                    turb_tmp[i] = out_turbulence[i];
                    continue;
                }
                glm::vec3 fs(0.0f);
                float ts = 0.0f;
                for (uint32_t j : adj[i]) {
                    fs += out_flow[j];
                    ts += out_turbulence[j];
                }
                float inv = 1.0f / static_cast<float>(adj[i].size());
                flow_tmp[i] = glm::mix(out_flow[i], fs * inv, 0.5f);
                turb_tmp[i] = glm::mix(out_turbulence[i], ts * inv, 0.5f);
            }
            out_flow.swap(flow_tmp);
            out_turbulence.swap(turb_tmp);
        }
    }
    for (float& t : out_turbulence) t = glm::clamp(t, 0.0f, 1.0f);
}

} // namespace water
} // namespace toy
