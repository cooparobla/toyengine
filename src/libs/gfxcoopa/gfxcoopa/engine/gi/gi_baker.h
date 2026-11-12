/**
 * @file gi_baker.h
 * @brief CPU/GPU probe baking engine for SH indirect lighting with ray-scene geometry bounce.
 */

#ifndef GFXCOOPA_ENGINE_GI_GI_BAKER_H
#define GFXCOOPA_ENGINE_GI_GI_BAKER_H

#include <gfxcoopa/engine/util/sh_math.h>
#include <gfxcoopa/engine/gi/gi_data.h>
#include <gfxcoopa/engine/targets/cubemap_target.h>
#include <gfxcoopa/engine/components/renderable_ref.h>
#include <gfxcoopa/engine/components/directional_light.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/command/command_pool.h>
#include <coopa/scene/scene.h>
#include <coopa/job/parallel_for.h>

#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <cstddef>

namespace coopa {
namespace gfx {
namespace engine {
namespace gi {

using namespace util;

/// @brief One axis-aligned box of scene geometry, with the surface properties a
/// bounced ray needs. The baker's ray-scene intersection works against these.
struct SceneBox {
    glm::vec3 box_min;
    glm::vec3 box_max;
    glm::vec3 albedo;
    glm::vec3 emissive; /**< Pre-multiplied (colour * strength) -- see PBRMaterial::gpu_emissive(). */
};

/// @brief Where a traced ray struck a SceneBox, and what it struck.
struct HitInfo {
    bool hit = false;
    float t = 1e9f;
    glm::vec3 albedo = glm::vec3(0.0f);
    glm::vec3 normal = glm::vec3(0.0f, 0.0f, 1.0f);
    glm::vec3 emissive = glm::vec3(0.0f);
};

inline HitInfo intersect_box(const glm::vec3& O, const glm::vec3& D, const SceneBox& box) {
    HitInfo res{};
    glm::vec3 invD = 1.0f / (D + glm::vec3(1e-7f));
    glm::vec3 t0 = (box.box_min - O) * invD;
    glm::vec3 t1 = (box.box_max - O) * invD;
    glm::vec3 tmin = glm::min(t0, t1);
    glm::vec3 tmax = glm::max(t0, t1);

    float t_near = std::max(std::max(tmin.x, tmin.y), tmin.z);
    float t_far  = std::min(std::min(tmax.x, tmax.y), tmax.z);

    if (t_far >= std::max(t_near, 0.001f)) {
        res.hit = true;
        res.t = t_near;
        res.albedo = box.albedo;
        res.emissive = box.emissive;

        glm::vec3 hit_pos = O + D * t_near;
        glm::vec3 center = 0.5f * (box.box_min + box.box_max);
        glm::vec3 local = hit_pos - center;
        glm::vec3 half_extent = 0.5f * (box.box_max - box.box_min);
        glm::vec3 abs_local = glm::abs(local / (half_extent + glm::vec3(1e-5f)));

        if (abs_local.x >= abs_local.y && abs_local.x >= abs_local.z) {
            res.normal = glm::vec3(local.x > 0 ? 1.0f : -1.0f, 0.0f, 0.0f);
        } else if (abs_local.y >= abs_local.x && abs_local.y >= abs_local.z) {
            res.normal = glm::vec3(0.0f, local.y > 0 ? 1.0f : -1.0f, 0.0f);
        } else {
            res.normal = glm::vec3(0.0f, 0.0f, local.z > 0 ? 1.0f : -1.0f);
        }
    }
    return res;
}

/**
 * @class GiBaker
 * @brief Bakes a grid of spherical-harmonic light probes by tracing rays against
 *        the scene's geometry on the CPU.
 *
 * Projects each probe's incoming radiance onto 9 second-order SH coefficients.
 * Probes are independent, so the grid is baked in parallel over a flattened
 * index range.
 */
class GiBaker {
public:
    /// @brief JobType tag for GiBaker::bake_cpu's parallel_for dispatch (see coopa::job::JobType's doc).
    static constexpr coopa::job::JobType k_gi_bake_job_type = 0x61B00000u;

    template<typename ProbeVolume>
    static std::vector<gi::SHProbe> bake_cpu(
        const ProbeVolume& volume,
        const coopa::scene::Scene& scene)
    {
        const int Nx = volume.grid_resolution.x;
        const int Ny = volume.grid_resolution.y;
        const int total_probes = volume.total_probes();

        std::vector<gi::SHProbe> probes(total_probes);

        // Collect scene boxes and materials
        std::vector<SceneBox> boxes;
        for (const auto& ref : coopa::gfx::engine::components::gather_renderables(scene)) {
            const auto* mr = ref.renderer;
            const auto* tc = ref.transform;
            if (!mr || !tc) continue;

            glm::mat4 m = tc->get_world_matrix();
            glm::vec3 pos = glm::vec3(m[3]);
            glm::vec3 scale = glm::vec3(
                glm::length(glm::vec3(m[0])),
                glm::length(glm::vec3(m[1])),
                glm::length(glm::vec3(m[2]))
            );
            glm::vec3 box_min = pos - 0.5f * scale;
            glm::vec3 box_max = pos + 0.5f * scale;
            boxes.push_back({box_min, box_max, mr->material.albedo, glm::vec3(mr->material.gpu_emissive())});
        }

        const int SAMPLE_COUNT = 128;
        const float PI = 3.14159265358979323846f;
        const float weight = (4.0f * PI) / float(SAMPLE_COUNT);
        const float GOLDEN_ANGLE = 2.39996322972865332f;

        const auto* dir_light = scene.find_first_component<coopa::gfx::engine::components::DirectionalLightComponent>();

        // Every probe writes only its own probes[probe_idx] slot and reads nothing but the
        // shared-const boxes/dir_light/volume above, so probes are independent work items --
        // flatten the iz/iy/ix nest (probe_idx == iz*Ny*Nx + iy*Nx + ix)
        // into a single index range a parallel_for can fan out over.
        auto bake_probe = [&](int probe_idx) {
            int iz = probe_idx / (Ny * Nx);
            int rem = probe_idx % (Ny * Nx);
            int iy = rem / Nx;
            int ix = rem % Nx;
            glm::vec3 probe_pos = volume.probe_position(ix, iy, iz);

            std::array<glm::vec3, 9> sh_coeffs{};
            for (int b = 0; b < 9; ++b) sh_coeffs[b] = glm::vec3(0.0f);

            for (int k = 0; k < SAMPLE_COUNT; ++k) {
                float z = 1.0f - (2.0f * float(k) + 1.0f) / float(SAMPLE_COUNT);
                float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
                float phi = float(k) * GOLDEN_ANGLE;

                glm::vec3 d(std::cos(phi) * r, std::sin(phi) * r, z);

                HitInfo best_hit{};
                for (const auto& box : boxes) {
                    HitInfo hit = intersect_box(probe_pos, d, box);
                    if (hit.hit && hit.t < best_hit.t) {
                        best_hit = hit;
                    }
                }

                glm::vec3 radiance(0.0f);

                if (best_hit.hit) {
                    if (dir_light) {
                        float n_dot_l = std::max(glm::dot(best_hit.normal, -dir_light->direction), 0.0f);
                        glm::vec3 direct_light = dir_light->color * dir_light->intensity * n_dot_l;
                        radiance = (best_hit.albedo / PI) * direct_light;
                    } else {
                        radiance = best_hit.albedo * 0.2f;
                    }
                    // An emissive surface contributes its own radiance regardless of
                    // whether a directional light exists -- it's a light source in its
                    // own right for baked GI purposes (unlike the real-time deferred
                    // path, which never gathers emissive into direct lighting).
                    radiance += best_hit.emissive;
                } else {
                    if (dir_light) {
                        float n_dot_l = std::max(glm::dot(d, -dir_light->direction), 0.0f);
                        radiance = dir_light->color * dir_light->intensity * n_dot_l * 0.1f;
                    }
                }

                sh_project_sample(sh_coeffs, d, radiance, weight);
            }

            for (int b = 0; b < 9; ++b) {
                probes[probe_idx].bands[b] = glm::vec4(sh_coeffs[b], 0.0f);
            }
        };

        // scene.job_engine() may be nullptr (no engine installed) -- fall back to the plain
        // serial loop, byte-identical to the parallel path since each probe is independent.
        coopa::job::JobEngine* jobs = scene.job_engine();
        if (jobs && total_probes > 1) {
            jobs->parallel_for_blocking(static_cast<std::size_t>(total_probes), 0 /* auto grain */,
                [&](std::size_t begin, std::size_t end) {
                    for (std::size_t p = begin; p < end; ++p) {
                        bake_probe(static_cast<int>(p));
                    }
                },
                k_gi_bake_job_type);
        } else {
            for (int p = 0; p < total_probes; ++p) bake_probe(p);
        }

        return probes;
    }
};

} // namespace gi
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_GI_GI_BAKER_H
