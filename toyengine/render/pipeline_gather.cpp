/**
 * @file pipeline_gather.cpp
 * @brief Per-frame CPU gather: visible meshes (cull, batch, LOD, instance streams),
 *        world-space UI canvases, SDF draw items, and the scene snapshot.
 *
 * One of toy_render_pipeline.cpp's sibling translation units -- same class, split by
 * concern so a 5,000-line file does not have to be navigated as one. The declarations
 * (and the rules that govern them) are in toy_render_pipeline.h.
 */

#include <toyengine/render/toy_render_pipeline.h>
#include <glm/gtx/quaternion.hpp>

#include <coopa/job/engine.h>
#include <coopa/job/parallel_for.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/core/swapchain.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/components/sdf_renderer.h>
#include <gfxcoopa/engine/components/sdf_shape.h>
#include <gfxcoopa/engine/data/light_data.h>
#include <gfxcoopa/engine/passes/hiz_pass.h>
#include <gfxcoopa/engine/passes/ssr_pass.h>
#include <gfxcoopa/engine/passes/transparent_pass.h>
#include <gfxcoopa/engine/targets/gbuffer_target.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/presentation/renderer.h>
#include <cstdio>
#include <cstdlib>
#include <toyengine/core/runtime_paths.h>
#include <toyengine/render/frame_profile.h>
#include <toyengine/render/particle_types.h>
#include <toyengine/render/sky_state.h>
#include <toyengine/render/toy_render_config.h>
#include <toyengine/render/toy_render_math.h>
#include <toyengine/scene/camera_controller.h>
#include <uicoopa/layout/canvas.h>
#include <uicoopa/render/ui_pass.h>
#include <volk/volk.h>

namespace toy {
namespace render {

ToyRenderPipeline::MeshGather ToyRenderPipeline::gather_meshes_(uint32_t frame_slot, const glm::mat4& view, const glm::mat4& unjittered_proj,
                          bool cast_dir_shadow) {
    using coopa::gfx::engine::components::MeshRenderer;
    MeshGather out;
    // One draw item per (renderer, material part).
    for (MeshRenderer* mr : frame_scene_.mesh_renderers) {
        mr->resolve_slot_names();   // name-keyed `materials:` once the mesh has loaded
        const uint32_t parts = mr->is_ready() ? mr->get_mesh()->part_count() : 1u;
        for (uint32_t p = 0; p < parts; ++p) {
            out.renderers.push_back(mr);
            out.part.push_back(p);
        }
    }
    // Mesh-mode particle systems: one item per (batch, part), drawn from the batch's own
    // instance matrices through the same batching as every other opaque mesh.
    const size_t first_particle_item = out.renderers.size();
    std::vector<const ParticleMeshBatch*> particle_items;
    for (const ParticleMeshBatch& pb : particle_state_.meshes) {
        MeshRenderer* mr = pb.proxy;
        if (!mr || pb.count == 0 || !pb.matrices || !mr->is_ready()) continue;
        mr->resolve_slot_names();
        for (uint32_t p = 0; p < mr->get_mesh()->part_count(); ++p) {
            out.renderers.push_back(mr);
            out.part.push_back(p);
            particle_items.push_back(&pb);
        }
    }
    const size_t n = out.renderers.size();
    out.multi.assign(n, nullptr);
    out.multi_count.assign(n, 0u);
    out.own_instances.assign(n, nullptr);
    for (size_t k = 0; k < particle_items.size(); ++k) {
        out.multi[first_particle_item + k]       = particle_items[k]->matrices;
        out.multi_count[first_particle_item + k] = particle_items[k]->count;
        // Static batches (terrain scatter chunks): a persistent buffer, uploaded once.
        if (particle_items[k]->static_key != 0) out.own_instances[first_particle_item + k] = static_instances_for_(*particle_items[k]);
    }
    out.world_matrices.assign(n, glm::mat4(1.0f));
    out.prev_world_matrices.assign(n, glm::mat4(1.0f));
    out.snow_anchors.assign(n, glm::mat4(1.0f));
    out.moved.assign(n, 0);
    out.bounds.assign(n, WorldBounds{});
    out.valid.assign(n, 0);
    out.lod.assign(n, 0);
    out.ext.assign(n, glm::uvec4(0u));
    out.instance_idx.assign(n, InstanceStream::kInvalidIndex);
    instance_stream_.begin(frame_slot);

    // A world matrix counts as changed past any float churn a transform resolve can
    // produce for a resting object; a real move is orders of magnitude larger.
    auto matrix_moved = [](const glm::mat4& a, const glm::mat4& b) {
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                if (std::abs(a[c][r] - b[c][r]) > 1e-5f) return true;
            }
        }
        return false;
    };

    // world_matrix() is a pure read, safe for any number of concurrent readers (unlike
    // get_world_matrix()), because TransformSystem already resolved every dirty transform
    // earlier this frame. Each index writes only its own slot. prev_world_ is only READ
    // here (find on a map no one mutates until the serial section below), so that is
    // parallel-safe too.
    auto gather_mesh = [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            MeshRenderer* mr = out.renderers[i];
            if (out.multi[i]) {
                // A particle batch: world-space matrices already, bounds from the system.
                // No previous poses exist for its instances, so each streams prev == cur;
                // its presence alone counts as scene motion.
                const ParticleMeshBatch& pb = *particle_items[i - first_particle_item];
                out.bounds[i].center = 0.5f * (pb.bounds_min + pb.bounds_max);
                out.bounds[i].extent = 0.5f * (pb.bounds_max - pb.bounds_min);
                out.valid[i] = 1;
                out.moved[i] = 1;
                continue;
            }
            if (!mr->is_ready() || !mr->owner) continue;
            auto* tc = mr->owner->get_transform();
            if (!tc) continue;
            out.world_matrices[i] = tc->transform().world_matrix();
            const auto& mesh = mr->get_mesh();
            out.bounds[i] = world_aabb(out.world_matrices[i], mesh->bounds_min(), mesh->bounds_max());
            out.valid[i] = 1;
            const auto pit = prev_world_.find(mr);
            out.prev_world_matrices[i] = (pit != prev_world_.end()) ? pit->second : out.world_matrices[i];
            // A dynamic mesh re-uploads its vertices every frame under a fixed matrix
            // (cloth, CPU skinning): its surface moves even though prev == cur.
            const bool transform_moved = matrix_moved(out.prev_world_matrices[i], out.world_matrices[i]);
            out.moved[i] = (transform_moved || mesh->is_dynamic()) ? 1 : 0;
            // Snow anchor: frozen once the transform has moved (the pose from just before it
            // first did, so the pattern it showed at rest carries on with it).
            const auto ait = snow_anchor_.find(mr);
            out.snow_anchors[i] = ait != snow_anchor_.end() ? ait->second
                                : transform_moved           ? out.prev_world_matrices[i]
                                                            : out.world_matrices[i];
        }
    };
    if (should_parallelize_(n)) {
        // Normal help floor: the gather is the frame's critical path, so a thread waiting here
        // must not pick up a background Low job (a terrain chunk mesh) and stall the frame.
        jobs_->parallel_for_blocking(n, 0 /* auto grain */, gather_mesh, 0,
                                     coopa::job::Priority::Normal, coopa::job::Priority::Normal);
    } else {
        gather_mesh(0, n);
    }

    // Rebuild last-frame poses for the next frame (serial: the map is written here only),
    // and record whether anything in the scene moved -- the temporal freeze (see
    // record_scene_()'s stillness block) must stay off while it did.
    {
        std::unordered_map<const MeshRenderer*, glm::mat4> next_prev_world;
        std::unordered_map<const MeshRenderer*, glm::mat4> next_snow_anchor;
        std::unordered_map<const MeshRenderer*, uint32_t>  next_still;
        next_prev_world.reserve(prev_world_.size() + 16);
        next_still.reserve(still_frames_.size() + 16);
        bool any_moved = false;
        for (size_t i = 0; i < n; ++i) {
            if (!out.valid[i]) continue;
            any_moved = any_moved || out.moved[i] != 0;
            if (out.multi[i]) continue;
            MeshRenderer* mr = out.renderers[i];
            next_prev_world[mr] = out.world_matrices[i];
            // Keep a frozen anchor; freeze one the frame the transform first moves.
            if (snow_anchor_.count(mr) || matrix_moved(out.prev_world_matrices[i], out.world_matrices[i])) {
                next_snow_anchor[mr] = out.snow_anchors[i];
            }
            // Frames without moving -- what makes a caster eligible for the local-shadow
            // static cache (see the local views below).
            const auto it = still_frames_.find(mr);
            const uint32_t prev = (it != still_frames_.end()) ? it->second : 0u;
            next_still[mr] = out.moved[i] ? 0u : std::min(prev + 1u, 1u << 20);
        }
        prev_world_.swap(next_prev_world);
        snow_anchor_.swap(next_snow_anchor);
        still_frames_.swap(next_still);
        scene_moved_ = any_moved;
    }

    // --- LOD (camera), and keys. Serial: MaterialTextureCache::set_for() may allocate. ---
    std::vector<MeshBatchKey> keys(n);
    std::unordered_map<const MeshRenderer*, int> next_lod_state;
    next_lod_state.reserve(lod_state_.size());
    std::vector<LodThreshold> thresholds;
    std::hash<std::string> hash_str;
    for (size_t i = 0; i < n; ++i) {
        if (!out.valid[i]) continue;
        MeshRenderer* mr = out.renderers[i];
        const auto& mesh = *mr->get_mesh();

        const bool has_lods = mesh.lods().size() > 1 || mesh.cull_screen_size() > 0.0f;
        // Particle batches share one proxy renderer across many batches (a scatter's chunks),
        // so each picks its own level, without the per-renderer sharing / hysteresis below.
        if (out.multi[i] && has_lods && mr->lods_enabled) {
            const WorldBounds& b = out.bounds[i];
            const glm::vec3 eye = glm::vec3(glm::inverse(view)[3]);
            const glm::vec3 nearest = glm::clamp(eye, b.center - b.extent, b.center + b.extent);
            const float scale = out.multi_count[i] > 0 ? glm::length(glm::vec3(out.multi[i][0][0])) : 1.0f;
            const float radius = 0.5f * glm::length(mesh.bounds_max() - mesh.bounds_min()) * scale;
            const float size = screen_height_fraction(nearest, radius, view, unjittered_proj) * config_.mesh_lod_bias * mr->lod_bias;
            thresholds.clear();
            for (const auto& l : mesh.lods()) thresholds.push_back({l.screen_size});
            const int level = select_lod(size, thresholds, mesh.cull_screen_size(), -2, 0.1f);
            if (level < 0) { out.valid[i] = 0; continue; }
            out.lod[i] = static_cast<uint32_t>(level);
        } else if (auto done = next_lod_state.find(mr); has_lods && mr->lods_enabled && done != next_lod_state.end()) {
            // Another part of the same renderer chose already: every part shares its LOD.
            if (done->second < 0) { out.valid[i] = 0; continue; }
            out.lod[i] = static_cast<uint32_t>(done->second);
        } else if (has_lods && mr->lods_enabled) {
            thresholds.clear();
            for (const auto& l : mesh.lods()) thresholds.push_back({l.screen_size});
            const float size = screen_height_fraction(out.bounds[i].center, out.bounds[i].radius(),
                                                      view, unjittered_proj)
                             * config_.mesh_lod_bias * mr->lod_bias;
            auto it = lod_state_.find(mr);
            const int prev = (it != lod_state_.end()) ? it->second : -2;
            const int level = select_lod(size, thresholds, mesh.cull_screen_size(), prev, 0.1f);
            next_lod_state[mr] = level;
            if (level < 0) { out.valid[i] = 0; continue; }   // below cull size: no view draws it
            out.lod[i] = static_cast<uint32_t>(level);
        }

        const auto& m = out.material(i);
        MeshBatchKey& k = keys[i];
        k.shader_hash = hash_str(m.shader);
        k.flags       = (m.cull_backfaces ? 1u : 0u) | (static_cast<uint32_t>(m.alpha_mode) << 1);
        k.set         = &material_cache_->set_for(m);
        k.mesh        = &mesh;
        k.part        = out.part[i];
        k.camera.albedo_alpha = glm::vec4(m.albedo, m.alpha);
        k.camera.mra_cutoff   = glm::vec4(m.metallic, m.roughness, m.ao, m.gpu_alpha_cutoff());
        k.camera.emissive     = m.gpu_emissive();
        k.camera.params       = m.shader_params;
        k.shadow.params       = m.shader_params;
        k.shadow.alpha_cutoff = m.gpu_alpha_cutoff();
        out.ext[i]            = surface_ext_(*mr, m);
        k.camera.ext          = out.ext[i];
        k.shadow.tess_a       = out.ext[i].x;
        k.shadow.tess_b       = out.ext[i].y;
    }
    lod_state_.swap(next_lod_state);

    // A BLEND material only casts a shadow at full opacity -- the shadow passes have no
    // per-fragment alpha discard beyond the CUTOUT mask (gfx/surface/shadow_fs.glsl), so a
    // translucent object is binary: caster or not.
    const glm::vec3 eye = glm::vec3(glm::inverse(view)[3]);
    auto casts = [&](size_t i) {
        const auto& m = out.material(i);
        if (m.is_blended() && m.alpha < 1.0f) return false;
        const MeshRenderer* mr = out.renderers[i];
        if (!mr->cast_shadows) return false;
        if (mr->shadow_max_distance > 0.0f) {
            // Distance from the eye to the item's box.
            const WorldBounds& b = out.bounds[i];
            const glm::vec3 d = glm::max(glm::abs(eye - b.center) - b.extent, glm::vec3(0.0f));
            if (glm::dot(d, d) > mr->shadow_max_distance * mr->shadow_max_distance) return false;
        }
        return true;
    };
    auto opaque = [&](size_t i) { return !out.material(i).is_blended(); };
    auto blended = [&](size_t i) { return out.material(i).is_blended(); };

    // --- Static-caster eligibility and the static-scene generation hash, shared by the
    // directional cascade list cache here and the local-shadow static cache below. ---
    // Eligible: still for kStaticCasterFrames, not a dynamic mesh or a particle batch, not
    // tessellated (edge factors follow the CAMERA, so its shadow is never the same twice),
    // and not drawn with a shadow shader that may animate.
    auto static_ok = [&](size_t i) {
        if (out.multi[i]) return false;
        MeshRenderer* mr = out.renderers[i];
        if (mr->get_mesh()->is_dynamic()) return false;
        const auto it = still_frames_.find(mr);
        if (it == still_frames_.end() || it->second < kStaticCasterFrames) return false;
        if (out.ext[i].x != 0u) return false;
        return animated_shadow_shaders_.count(out.material(i).shader) == 0;
    };
    // FNV-1a over raw bytes, for the static-content hashes.
    auto mix = [](uint64_t h, const void* data, size_t len) {
        const auto* b = static_cast<const unsigned char*>(data);
        for (size_t k = 0; k < len; ++k) { h ^= b[k]; h *= 1099511628211ull; }
        return h;
    };
    std::vector<uint8_t> is_static(n, 0);
    // First item index of each renderer: cached caster lists name items by the stable identity
    // (renderer, part) -- MeshGather indices reshuffle whenever the scene's composition
    // changes -- and parts are pushed contiguously, so an identity maps back as first + part.
    std::unordered_map<const MeshRenderer*, uint32_t> first_item;
    first_item.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (!out.multi[i]) first_item.emplace(out.renderers[i], static_cast<uint32_t>(i));
    }
    // Everything a static caster contributes to a culled list: identity, pose, LOD, the
    // mesh/material batch keys, and the shadow-relevant renderer flags. Any change in the set
    // or in a member changes the hash and with it every list keyed on it.
    uint64_t static_gen = 1469598103934665603ull;
    bool static_distance_limited = false;
    for (size_t i = 0; i < n; ++i) {
        if (!out.valid[i] || !static_ok(i)) continue;
        is_static[i] = 1;
        const MeshRenderer* mr = out.renderers[i];
        const MeshBatchKey& k = keys[i];
        static_gen = mix(static_gen, &mr, sizeof(mr));
        static_gen = mix(static_gen, &out.part[i], sizeof(out.part[i]));
        static_gen = mix(static_gen, &out.world_matrices[i], sizeof(glm::mat4));
        static_gen = mix(static_gen, &out.lod[i], sizeof(out.lod[i]));
        static_gen = mix(static_gen, &k.shader_hash, sizeof(k.shader_hash));
        static_gen = mix(static_gen, &k.mesh, sizeof(k.mesh));
        static_gen = mix(static_gen, &k.shadow, sizeof(k.shadow));
        const uint8_t cast_flags = (mr->cast_shadows ? 1u : 0u) |
            ((out.material(i).is_blended() && out.material(i).alpha < 1.0f) ? 2u : 0u);
        static_gen = mix(static_gen, &cast_flags, sizeof(cast_flags));
        static_gen = mix(static_gen, &mr->shadow_max_distance, sizeof(mr->shadow_max_distance));
        if (mr->cast_shadows && mr->shadow_max_distance > 0.0f) static_distance_limited = true;
    }

    // --- Per-view culling + batching ---
    // The grid holds every valid item by its bounds; a view's cull queries the cells its
    // frustum touches (plus the always-tested large items) instead of walking the flat list.
    caster_grid_.rebuild(out.bounds, out.valid);
    uint64_t cull_items_tested = 0, cull_cells_visited = 0, dir_list_cache_hits = 0;
    uint64_t dir_levels_rendered = 0;
    std::vector<uint32_t> visible;
    visible.reserve(n);
    // Exact per-item test over the grid's candidate superset: membership -- and with it the
    // batched output -- is identical to a flat loop over every item.
    auto cull_view = [&](const glm::mat4& view_proj, bool shadow, float shadow_res,
                         const std::function<bool(size_t)>& accept) {
        visible.clear();
        const Frustum f = Frustum::from_matrix(view_proj);
        const float min_texels = shadow ? config_.shadow_min_caster_texels : 0.0f;
        cull_cells_visited += caster_grid_.for_candidates(f, view_proj_xy_rect(view_proj), [&](uint32_t i) {
            ++cull_items_tested;
            if (!out.valid[i] || !accept(i)) return;
            if (!f.intersects(out.bounds[i])) return;
            if (min_texels > 0.0f &&
                projected_texels(view_proj, out.bounds[i].center, out.bounds[i].radius(), shadow_res) < min_texels) {
                return;
            }
            visible.push_back(i);
        });
    };
    auto emit_batches = [&](bool shadow, std::vector<MeshBatch>& list) {
        list.clear();
        auto key_less = [&](uint32_t a, uint32_t b) {
            const MeshBatchKey& ka = keys[a];
            const MeshBatchKey& kb = keys[b];
            if (ka.shader_hash != kb.shader_hash) return ka.shader_hash < kb.shader_hash;
            if (ka.flags != kb.flags)             return ka.flags < kb.flags;
            if (ka.set != kb.set)                 return std::less<const void*>()(ka.set, kb.set);
            if (ka.mesh != kb.mesh)               return std::less<const void*>()(ka.mesh, kb.mesh);
            if (ka.part != kb.part)               return ka.part < kb.part;
            if (out.lod[a] != out.lod[b])         return out.lod[a] < out.lod[b];
            const int c = shadow ? std::memcmp(&ka.shadow, &kb.shadow, sizeof(ka.shadow))
                                 : std::memcmp(&ka.camera, &kb.camera, sizeof(ka.camera));
            if (c != 0) return c < 0;
            return a < b;   // stable within a batch: scene order
        };
        auto same_batch = [&](uint32_t a, uint32_t b) {
            const MeshBatchKey& ka = keys[a];
            const MeshBatchKey& kb = keys[b];
            if (out.own_instances[a] || out.own_instances[b]) return false;   // its own buffer: its own batch
            return ka.shader_hash == kb.shader_hash && ka.flags == kb.flags && ka.set == kb.set &&
                   ka.mesh == kb.mesh && ka.part == kb.part && out.lod[a] == out.lod[b] &&
                   out.material(a).shader == out.material(b).shader &&
                   (shadow ? std::memcmp(&ka.shadow, &kb.shadow, sizeof(ka.shadow)) == 0
                           : std::memcmp(&ka.camera, &kb.camera, sizeof(ka.camera)) == 0);
        };
        std::sort(visible.begin(), visible.end(), key_less);
        for (size_t r = 0; r < visible.size();) {
            size_t e = r + 1;
            while (e < visible.size() && same_batch(visible[r], visible[e])) ++e;
            MeshBatch batch;
            batch.item           = visible[r];
            batch.lod            = out.lod[visible[r]];
            if (const auto* own = out.own_instances[visible[r]]) {
                batch.own_instances  = own;
                batch.first_instance = 0;
                batch.instance_count = out.multi_count[visible[r]];
                list.push_back(batch);
                r = e;
                continue;
            }
            batch.first_instance = instance_stream_.size();
            uint32_t added = 0;
            for (size_t j = r; j < e; ++j) {
                const uint32_t it = visible[j];
                if (out.multi[it]) {
                    for (uint32_t m = 0; m < out.multi_count[it]; ++m) instance_stream_.add(out.multi[it][m]);
                    added += out.multi_count[it];
                } else {
                    instance_stream_.add(out.world_matrices[it], out.prev_world_matrices[it], out.snow_anchors[it]);
                    ++added;
                }
            }
            batch.instance_count = added;
            list.push_back(batch);
            r = e;
        }
    };
    auto build = [&](const glm::mat4& view_proj, bool shadow, float shadow_res,
                     const std::function<bool(size_t)>& accept, std::vector<MeshBatch>& list) {
        cull_view(view_proj, shadow, shadow_res, accept);
        emit_batches(shadow, list);
    };

    const glm::mat4 camera_vp = unjittered_proj * view;
    build(camera_vp, false, 0.0f, opaque, out.gbuffer);

    // The forward BLEND pass draws one renderer at a time in back-to-front order, so its
    // transforms are added singly.
    {
        const Frustum f = Frustum::from_matrix(camera_vp);
        visible.clear();
        cull_cells_visited += caster_grid_.for_candidates(f, view_proj_xy_rect(camera_vp), [&](uint32_t i) {
            ++cull_items_tested;
            if (out.valid[i] && blended(i) && f.intersects(out.bounds[i])) visible.push_back(i);
        });
        std::sort(visible.begin(), visible.end());   // ascending: keeps the stream's order stable
        for (const uint32_t i : visible) {
            out.instance_idx[i] = instance_stream_.add(out.world_matrices[i], out.prev_world_matrices[i], out.snow_anchors[i]);
            out.has_blend_mesh  = true;
        }
    }

    // A static caster appearing, vanishing, moving or changing material invalidates every
    // level's cached depth at once -- caught here, where the whole static set is hashed in
    // one O(n) pass, rather than per level. The plan next frame re-renders every live level
    // instead of waiting for the round-robin budget: a stale level here would show a missing
    // or a ghost shadow, not merely an uncovered sliver at its edge.
    dir_static_scene_changed_ = dir_cache_active_() && static_gen != dir_static_gen_;
    dir_static_gen_ = static_gen;

    if (cast_dir_shadow) {
        const uint32_t cascades = std::min<uint32_t>(
            shadow_target_.dir_cascade_count(), coopa::gfx::engine::data::MAX_DIR_SHADOW_LEVELS);
        const float tile_res = static_cast<float>(shadow_target_.dir_tile_resolution());
        const bool caching = dir_cache_active_();
        for (uint32_t c = 0; c < cascades; ++c) {
            // The PUBLISHED matrix, never the wanted one: this level's tile holds depth drawn
            // with it, and culling, drawing and sampling must all agree with that content.
            const glm::mat4 vp = current_light_data().dir_level_matrix[c];
            DirLevelState& lvl = dir_levels_[c];
            // A content change marks the level dirty even if its matrix held still, but it
            // does NOT jump the budget. Forcing every level the instant any static item
            // changes sounds safer and is in fact ruinous: a camera that moves at all flips
            // some chunk's LOD every frame, so every level would redraw every frame and the
            // cache would never once pay for itself (measured: 6.2 of 8 levels per frame on
            // habitat's game scene). The budget clears the backlog within a few frames, and
            // what shows during those frames is one chunk at its previous LOD.
            if (caching && dir_static_scene_changed_) lvl.dirty = true;

            if (caching) {
                // Static casters only on the frames this level actually re-renders them...
                if (lvl.render_static) {
                    cull_view(vp, true, tile_res, [&](size_t i) { return casts(i) && is_static[i]; });
                    emit_batches(true, out.dir_static[c]);
                    ++dir_levels_rendered;
                } else {
                    out.dir_static[c].clear();
                }
                // ...and movers every frame, drawn over the copied static depth.
                cull_view(vp, true, tile_res, [&](size_t i) { return casts(i) && !is_static[i]; });
                emit_batches(true, out.cascade[c]);
                continue;
            }

            // No cache: one list with every caster in it, exactly as before the cache existed.
            DirCasterListCache& cache = dir_caster_cache_[c];
            if (!config_.shadow_cull_cache_enabled) {
                cache.filled = false;
                build(vp, true, tile_res, casts, out.cascade[c]);
                continue;
            }
            // Everything the STATIC part of this level's list depends on. The eye enters only
            // while some static caster is distance-limited (MeshRenderer::shadow_max_distance
            // measures from the eye), so the common case survives camera translation.
            uint64_t key = static_gen;
            key = mix(key, &vp, sizeof(vp));
            key = mix(key, &tile_res, sizeof(tile_res));
            key = mix(key, &config_.shadow_min_caster_texels, sizeof(config_.shadow_min_caster_texels));
            if (static_distance_limited) key = mix(key, &eye, sizeof(eye));
            const Frustum f = Frustum::from_matrix(vp);
            if (cache.filled && cache.key == key) {
                // Static membership is reused wholesale; movers are re-culled on top of it.
                ++dir_list_cache_hits;
                visible.clear();
                for (const auto& [mr, part] : cache.items) {
                    const auto it = first_item.find(mr);
                    if (it != first_item.end()) visible.push_back(it->second + part);
                }
                cull_cells_visited += caster_grid_.for_candidates(f, view_proj_xy_rect(vp), [&](uint32_t i) {
                    ++cull_items_tested;
                    if (!out.valid[i] || is_static[i] || !casts(i)) return;
                    if (!f.intersects(out.bounds[i])) return;
                    if (config_.shadow_min_caster_texels > 0.0f &&
                        projected_texels(vp, out.bounds[i].center, out.bounds[i].radius(), tile_res) <
                            config_.shadow_min_caster_texels) {
                        return;
                    }
                    visible.push_back(i);
                });
            } else {
                cull_view(vp, true, tile_res, casts);
                cache.filled = true;
                cache.key = key;
                cache.items.clear();
                for (const uint32_t i : visible) {
                    if (is_static[i]) cache.items.emplace_back(out.renderers[i], out.part[i]);
                }
            }
            emit_batches(true, out.cascade[c]);
        }
    }
    // Point/spot shadows: every view of every local light update_local_shadows_() chose.
    // A caster must reach into the light's influence sphere as well as its view frustum
    // (light-bounds culling: a face frustum alone extends to the corners of the range cube).
    {
        uint32_t total_views = 0;
        for (const LocalShadowSlot& slot : local_slots_) total_views = std::max(total_views, slot.first_view + slot.view_count);
        out.local_static.assign(total_views, {});
        out.local_dynamic.assign(total_views, {});
        const bool caching = local_shadow_atlas_->caching();
        const auto& blk = current_light_data().local_shadows;

        for (LocalShadowSlot& slot : local_slots_) {
            const glm::vec3 c = slot.pos;
            const float     r = slot.range;
            auto in_light = [&](size_t i) {
                const WorldBounds& b = out.bounds[i];
                const glm::vec3 d = glm::max(glm::abs(c - b.center) - b.extent, glm::vec3(0.0f));
                return glm::dot(d, d) <= r * r;
            };
            const float res = static_cast<float>(slot.tile_res);
            if (!caching) {
                for (uint32_t v = 0; v < slot.view_count; ++v) {
                    build(blk.view_proj[slot.first_view + v], true, res,
                          [&](size_t i) { return casts(i) && in_light(i); },
                          out.local_dynamic[slot.first_view + v]);
                }
                slot.rerender_static = false;
                continue;
            }

            // What the cache tiles must hold: every static caster any of this light's views
            // sees, at its current pose and material, plus the views and the tile block. Any
            // change re-renders the light's cache once; otherwise the copy is enough.
            uint64_t h = 1469598103934665603ull;
            h = mix(h, &slot.block, sizeof(slot.block));
            const XyRect light_rect{glm::vec2(c.x - r, c.y - r), glm::vec2(c.x + r, c.y + r)};
            std::vector<uint32_t> in_view;
            for (uint32_t v = 0; v < slot.view_count; ++v) {
                const glm::mat4& vp = blk.view_proj[slot.first_view + v];
                h = mix(h, &vp, sizeof(vp));
                const Frustum f = Frustum::from_matrix(vp);
                in_view.clear();
                cull_cells_visited += caster_grid_.for_candidates(f, light_rect, [&](uint32_t i) {
                    ++cull_items_tested;
                    if (!is_static[i] || !casts(i) || !in_light(i)) return;
                    if (f.intersects(out.bounds[i])) in_view.push_back(i);
                });
                std::sort(in_view.begin(), in_view.end());   // the hash must not depend on grid order
                for (const uint32_t i : in_view) {
                    const MeshBatchKey& k = keys[i];
                    const void* id = out.renderers[i];
                    h = mix(h, &id, sizeof(id));
                    h = mix(h, &out.world_matrices[i], sizeof(glm::mat4));
                    h = mix(h, &out.lod[i], sizeof(out.lod[i]));
                    h = mix(h, &out.part[i], sizeof(out.part[i]));
                    h = mix(h, &k.shader_hash, sizeof(k.shader_hash));
                    h = mix(h, &k.mesh, sizeof(k.mesh));
                    h = mix(h, &k.shadow, sizeof(k.shadow));
                }
            }
            const auto it = local_cache_.find(slot.light);
            slot.rerender_static = (it == local_cache_.end() || it->second.hash != h ||
                                    !(it->second.block == slot.block));
            local_cache_[slot.light] = LocalCacheEntry{h, slot.block};

            for (uint32_t v = 0; v < slot.view_count; ++v) {
                const glm::mat4& vp = blk.view_proj[slot.first_view + v];
                if (slot.rerender_static) {
                    build(vp, true, res, [&](size_t i) { return casts(i) && in_light(i) && is_static[i]; },
                          out.local_static[slot.first_view + v]);
                }
                build(vp, true, res, [&](size_t i) { return casts(i) && in_light(i) && !is_static[i]; },
                      out.local_dynamic[slot.first_view + v]);
            }
        }
    }

    instance_stream_.upload();

    // Fold LAST frame's counts (complete now: its recording has finished) into the totals.
    if (frame_stats_.renderers > 0) {
        stats_total_.camera_draws     += frame_stats_.camera_draws;
        stats_total_.camera_instances += frame_stats_.camera_instances;
        stats_total_.camera_triangles += frame_stats_.camera_triangles;
        stats_total_.shadow_draws     += frame_stats_.shadow_draws;
        stats_total_.shadow_instances += frame_stats_.shadow_instances;
        stats_total_.shadow_triangles += frame_stats_.shadow_triangles;
        stats_total_.renderers        += frame_stats_.renderers;
        stats_total_.camera_visible   += frame_stats_.camera_visible;
        stats_total_.tess_draws       += frame_stats_.tess_draws;
        stats_total_.cull_items_tested   += frame_stats_.cull_items_tested;
        stats_total_.cull_cells_visited  += frame_stats_.cull_cells_visited;
        stats_total_.dir_list_cache_hits += frame_stats_.dir_list_cache_hits;
        stats_total_.dir_levels_rendered  += frame_stats_.dir_levels_rendered;
        ++stats_frames_;
    }
    frame_stats_ = MeshDrawStats{};
    frame_stats_.renderers = n;
    frame_stats_.streamed_instances = instance_stream_.size();
    frame_stats_.cull_items_tested   = cull_items_tested;
    frame_stats_.cull_cells_visited  = cull_cells_visited;
    frame_stats_.dir_list_cache_hits = dir_list_cache_hits;
    frame_stats_.dir_levels_rendered = dir_levels_rendered;
    for (const MeshBatch& b : out.gbuffer) frame_stats_.camera_visible += b.instance_count;
    return out;
}

void ToyRenderPipeline::gather_ui_canvases_(coopa::scene::Scene& scene, uint32_t frame_slot, const glm::mat4& view) {
    // collect_canvases() returns EVERY canvas in the scene, so the world-space filter here is
    // load-bearing: without it a screen-space canvas's canvas-pixel geometry would be fed
    // through a 3D projection. It is called ONCE and the two gathers partition its result.
    //
    // Each canvas's DrawList was emitted by CanvasComponent::late_update() during
    // Scene::late_update(), which the caller runs before render().
    std::vector<coopa::ui::CanvasComponent*> all_canvases = coopa::ui::collect_canvases(scene);
    // The overlay scene's (the editor UI's) screen canvases come last, so they draw over the
    // game's own. Its world-space canvases, if any, are ignored: it has no camera of its own.
    if (overlay_scene_) {
        for (coopa::ui::CanvasComponent* c : coopa::ui::collect_canvases(*overlay_scene_)) {
            if (!c->is_world_space()) all_canvases.push_back(c);
        }
    }
    // The engine's overlay layers (Engine::add_overlay_layer()) after that, so they draw
    // over the editor UI as well as the game's.
    for (coopa::scene::Scene* layer : overlay_layers_) {
        for (coopa::ui::CanvasComponent* c : coopa::ui::collect_canvases(*layer)) {
            if (!c->is_world_space()) all_canvases.push_back(c);
        }
    }

    world_canvases_.clear();
    if (world_ui_pass_) {
        size_t ui_verts = 0;
        size_t ui_indices = 0;
        for (coopa::ui::CanvasComponent* canvas : all_canvases) {
            if (!canvas->is_world_space()) continue;
            world_canvases_.push_back(canvas);
            world_ui_pass_->register_textures(canvas->draw_list());
            ui_verts   += canvas->draw_list().vertices().size();
            ui_indices += canvas->draw_list().indices().size();
        }
        // Painter's order, farthest first. World UI composites with the depth test off, so two
        // canvases that overlap on screen resolve purely by draw order -- without this the one
        // later in the scene file wins regardless of which is actually in front. sort_order stays
        // the primary key, so an explicit authoring choice still wins; depth only breaks ties, which
        // is the common case of everything left at 0. stable_sort keeps scene order as the final
        // tiebreak. N is the handful of canvases a scene has, so this is free.
        std::stable_sort(world_canvases_.begin(), world_canvases_.end(),
            [&view](const coopa::ui::CanvasComponent* a, const coopa::ui::CanvasComponent* b) {
                if (a->sort_order != b->sort_order) return a->sort_order < b->sort_order;
                // view maps world -> camera, which looks down -Z, so a SMALLER (more
                // negative) z is farther away and must be drawn first.
                float za = (view * a->model()[3]).z;
                float zb = (view * b->model()[3]).z;
                return za < zb;
            });

        // Size the shared geometry buffers for the WHOLE frame up front and reset the
        // append cursor: every canvas streams into one buffer pair per frame slot, and
        // growing it mid-frame would reallocate out from under geometry already uploaded.
        world_ui_pass_->begin_frame(frame_slot, ui_verts, ui_indices);
        // Glyph atlases are R8 coverage, not RGBA. Without this every world-space Text
        // batch would bind the "quad" variant and draw coverage values as colour.
        if (!world_canvases_.empty()) {
            coopa::ui::UIResourceCache::instance().mark_text_atlases(*world_ui_pass_);
        }
    }

    // Screen-space UI: the same gather and the complementary half of the filter above. Two
    // things are deliberately not shared. There is no depth sort -- collect_canvases() already
    // ordered by sort_order, which for an overlay is all "in front" means, and a screen canvas
    // has no view-space position anyway. And mark_text_atlases() must be repeated, because the
    // marked-view set is a member of each pass; an unmarked glyph atlas draws through the
    // "quad" variant as raw coverage.
    screen_canvases_.clear();
    if (screen_ui_pass_) {
        size_t ui_verts = 0;
        size_t ui_indices = 0;
        for (coopa::ui::CanvasComponent* canvas : all_canvases) {
            if (canvas->is_world_space()) continue;
            screen_canvases_.push_back(canvas);
            screen_ui_pass_->register_textures(canvas->draw_list());
            ui_verts   += canvas->draw_list().vertices().size();
            ui_indices += canvas->draw_list().indices().size();
        }
        screen_ui_pass_->begin_frame(frame_slot, ui_verts, ui_indices);
        if (!screen_canvases_.empty()) {
            coopa::ui::UIResourceCache::instance().mark_text_atlases(*screen_ui_pass_);
        }
    }
}

std::vector<SdfDrawItem> ToyRenderPipeline::gather_sdf_(coopa::scene::Scene& scene, const glm::mat4& view,
                                     const glm::mat4& proj, const glm::vec3& cam_pos,
                                     uint32_t frame_slot) {
    // --- SDF gather ---
    // Mirrors the MeshRenderer gather above: one pass over every SdfRenderer in the scene,
    // producing the SdfDrawItem list every record_*() site below shares (same role
    // renderers/world_matrices/instance_idx play for meshes). Gated on sdf_enabled alone --
    // when it's off, sdf_draws stays empty and every downstream site (all of which check
    // sdf_draws.empty() or iterate it) is naturally a no-op, so this is the ONE place that
    // needs to know about the toggle.
    std::vector<SdfDrawItem> sdf_draws;
    sdf_data_.begin(frame_slot);
    if (config_.sdf_enabled) {
        using coopa::gfx::engine::components::SdfRenderer;
        using coopa::gfx::engine::data::SdfShapeGPU;
        using coopa::gfx::engine::data::SdfRendererGPU;

        glm::mat4 view_proj = proj * view;
        const auto& sdf_renderer_comps = frame_scene_.sdf_renderers;

        // Per-SdfRenderer scratch result. Holds everything computable independently, but NOT the
        // two SSBO writes -- SdfData hands out contiguous indices, so those stay serial in the merge
        // below. This is the heaviest per-item work in the frame (an 8-corner AABB, a frustum cull,
        // then per shape a glm::inverse, three glm::length and a cbrt) and every renderer is
        // independent of every other, so it is the best parallel candidate of the three gathers.
        struct SdfGatherResult {
            bool visible = false;
            SdfRenderer* comp = nullptr;
            SdfRendererGPU rec{};
            std::vector<SdfShapeGPU> shapes;
            glm::vec3 world_min{0.0f}, world_max{0.0f};
            bool is_blend = false, cast_shadows = true;
            PixelRect px_rect{};
        };
        std::vector<SdfGatherResult> gather_results(sdf_renderer_comps.size());

        auto gather_sdf = [&](size_t begin, size_t end) {
            for (size_t ri = begin; ri < end; ++ri) {
                SdfRenderer* sr = sdf_renderer_comps[ri];
                SdfGatherResult& out = gather_results[ri];
                if (!sr->owner) continue;
                auto* tc = sr->owner->get_transform();
                if (!tc) continue;
                // world_matrix() (pure read, safe for concurrent readers once
                // TransformSystem has resolved this frame) -- see the MeshRenderer
                // gather above for the same reasoning.
                glm::mat4 world = tc->transform().world_matrix();

                // World AABB from the renderer's local bounds box.
                glm::vec3 bmin_local = sr->bounds_center - sr->bounds_extent;
                glm::vec3 bmax_local = sr->bounds_center + sr->bounds_extent;
                glm::vec3 wmin(std::numeric_limits<float>::max());
                glm::vec3 wmax(std::numeric_limits<float>::lowest());
                for (int c = 0; c < 8; ++c) {
                    glm::vec3 corner((c & 1) ? bmax_local.x : bmin_local.x,
                                     (c & 2) ? bmax_local.y : bmin_local.y,
                                     (c & 4) ? bmax_local.z : bmin_local.z);
                    glm::vec3 wc = glm::vec3(world * glm::vec4(corner, 1.0f));
                    wmin = glm::min(wmin, wc);
                    wmax = glm::max(wmax, wc);
                }

                // Screen-space (pre-upscale) rectangle -- the whole performance story: only
                // pixels inside it ever raymarch (see record_gbuffer_()/record_transparent_()'s
                // cmd.set_scissor() calls). An empty/off-screen rect means free frustum culling.
                SdfClipRect clip_rect = compute_sdf_clip_rect(view_proj, wmin, wmax);
                // Off camera, but possibly casting a shadow INTO view: keep it with an empty
                // pixel rect -- every camera pass skips those -- and let each shadow view
                // frustum-test and scissor it itself (see draw_sdf_shadow_casters_()).
                const bool casts = sr->cast_shadows && config_.sdf_shadows_enabled &&
                                   config_.shadows_enabled;
                if (!clip_rect.visible) {
                    if (!casts) continue;
                    clip_rect = SdfClipRect{glm::vec2(0.0f), glm::vec2(0.0f), false};
                }

                // Compute every shape's GPU record here; first_shape/shape_count (which
                // require sdf_data_'s shared, monotonic index allocator) are filled in by
                // the serial merge below instead of here.
                auto shapes = sr->collect_shapes();
                out.shapes.reserve(shapes.size());
                for (auto* shape : shapes) {
                    if (!shape->owner) continue;
                    auto* shape_tc = shape->owner->get_transform();
                    if (!shape_tc) continue;
                    glm::mat4 shape_world = shape_tc->transform().world_matrix();

                    // Approximate uniform scale (geometric mean of the 3 basis lengths) -- true
                    // SDFs don't support non-uniform scale exactly; this rescales the local-space
                    // distance back to world units well enough for the common case. See
                    // SdfShapeGPU::type_op_blend's doc.
                    float sx = glm::length(glm::vec3(shape_world[0]));
                    float sy = glm::length(glm::vec3(shape_world[1]));
                    float sz = glm::length(glm::vec3(shape_world[2]));
                    float uniform_scale = std::cbrt(std::max(sx * sy * sz, 1e-6f));

                    SdfShapeGPU gpu;
                    gpu.inv_world    = glm::inverse(shape_world);
                    gpu.params_round = glm::vec4(shape->params, shape->rounding);
                    float blend = shape->blend > 0.0f ? shape->blend : sr->smoothing;
                    gpu.type_op_blend = glm::vec4(static_cast<float>(static_cast<int>(shape->type)),
                                                  static_cast<float>(static_cast<int>(shape->op)),
                                                  blend, uniform_scale);
                    out.shapes.push_back(gpu);
                }
                if (out.shapes.empty()) continue; // no shapes collected -- nothing to draw

                out.rec.clip_rect    = glm::vec4(clip_rect.ndc_min, clip_rect.ndc_max);
                out.rec.bounds_min   = glm::vec4(wmin, 0.0f);
                out.rec.bounds_max   = glm::vec4(wmax, 0.0f);
                out.rec.albedo_alpha = glm::vec4(sr->material.albedo, sr->material.alpha);
                out.rec.mr_ao_cutoff = glm::vec4(sr->material.metallic, sr->material.roughness,
                                             sr->material.ao, sr->material.gpu_alpha_cutoff());
                // .w carries the toon code, which sdf_gbuffer.frag forwards into the
                // G-buffer's emissive alpha exactly as the mesh backbone does.
                out.rec.emissive     = sr->material.gpu_emissive();
                out.rec.emissive.w   = static_cast<float>(sr->material.gpu_toon_code());
                uint32_t clamped_steps = static_cast<uint32_t>(
                    std::clamp(sr->max_steps, 1, static_cast<int>(config_.sdf_max_steps)));
                out.rec.range = glm::uvec4(0, static_cast<uint32_t>(out.shapes.size()), clamped_steps, 0);
                out.rec.march = glm::vec4(sr->surface_epsilon, sr->normal_epsilon, 0.0f, 0.0f);

                out.comp         = sr;
                out.is_blend     = sr->material.is_blended();
                out.cast_shadows = sr->cast_shadows && config_.sdf_shadows_enabled;
                out.world_min    = wmin;
                out.world_max    = wmax;
                out.px_rect      = clip_rect.visible
                                 ? sdf_clip_rect_to_pixels(clip_rect.ndc_min, clip_rect.ndc_max,
                                                           render_extent_.width, render_extent_.height)
                                 : PixelRect{};
                out.visible = true;
            }
        };
        if (should_parallelize_(sdf_renderer_comps.size())) {
            jobs_->parallel_for_blocking(sdf_renderer_comps.size(), 0 /* auto grain */, gather_sdf, 0,
                                         coopa::job::Priority::Normal, coopa::job::Priority::Normal);
        } else {
            gather_sdf(0, sdf_renderer_comps.size());
        }

        // Serial merge, in gather order: sdf_data_.add_shape()/add_renderer() hand out
        // contiguous SSBO indices and must not race, so this part stays a plain loop.
        for (auto& out : gather_results) {
            if (!out.visible) continue;

            uint32_t first_shape = 0;
            for (size_t s = 0; s < out.shapes.size(); ++s) {
                uint32_t idx = sdf_data_.add_shape(out.shapes[s]);
                if (s == 0) first_shape = idx;
            }
            out.rec.range.x = first_shape;

            SdfDrawItem item;
            item.comp         = out.comp;
            item.gpu_index    = sdf_data_.add_renderer(out.rec);
            item.is_blend     = out.is_blend;
            item.cast_shadows = out.cast_shadows;
            item.world_min    = out.world_min;
            item.world_max    = out.world_max;
            item.world_center = 0.5f * (out.world_min + out.world_max);
            item.px_rect      = out.px_rect;
            sdf_draws.push_back(item);
        }

        // Per-frame globals: camera ray reconstruction + the forward pass's lighting/
        // indirect/SSR tuning, sourced from the SAME config_ fields fill_forward_globals_()
        // reads for the mesh path's own ForwardGlobals UBO (see sdf_forward.frag's doc for
        // the exact field-order mapping).
        auto& g = sdf_data_.globals();
        g.inv_view_proj = glm::inverse(view_proj);
        g.camera_pos    = glm::vec4(cam_pos, 1.0f);
        g.lighting0 = glm::vec4(config_.light_bands, config_.spec_threshold,
                                config_.soft_lighting ? 1.0f : 0.0f, config_.rim_strength);
        g.lighting1 = glm::vec4(config_.indirect.ambient_intensity, config_.indirect.sky_intensity,
                                config_.ssr_enabled ? 1.0f : 0.0f, config_.indirect.ssgi_intensity);
        g.ssr0 = glm::vec4(config_.indirect.ssgi_distance, config_.ssr_max_distance,
                          config_.ssr_bias_texels, config_.ssr_thickness);
        g.ssr1 = glm::vec4(config_.ssr_thickness_scale, config_.ssr_roughness_cutoff,
                      config_.ssr_cone_prefilter, 0.0f);
        g.ssr_steps = glm::ivec4(config_.ssr_max_iterations,
                                 static_cast<int>(hiz_pass_->max_mip_level()),
                                 config_.ssr_start_mip, config_.ssr_min_mip0_steps);
        // y: the SSR scene-colour chain (which SDF forward always reads) holds the previous
        // frame's final colour at mip 0 -- hits reproject by the G-buffer velocity.
        g.ssr_mip = glm::ivec4(static_cast<int>(scene_color_mip_pass_->max_mip_level()),
                               scene_color_history_valid_ ? 1 : 0, 0, 0);

        sdf_data_.upload();
    }
    return sdf_draws;
}

void ToyRenderPipeline::snapshot_scene_(coopa::scene::Scene& scene) {
    using namespace coopa::gfx::engine::components;
    FrameScene& fs = frame_scene_;
    fs.dir_light = nullptr;
    fs.point_lights.clear();
    fs.spot_lights.clear();
    fs.mesh_renderers.clear();
    fs.sdf_renderers.clear();
    fs.volumes.clear();
    // Depth-first, pruning at an inactive object: disabling a parent hides everything under
    // it (runtime children included -- a water body's tiles, the weather's effects).
    std::function<void(coopa::scene::SceneObject&)> visit = [&fs, &visit](coopa::scene::SceneObject& obj) {
            if (!obj.active()) return;
            bool dir = false, point = false, spot = false, mesh = false, sdf = false, vol = false;
            for (const auto& comp : obj.components()) {
                coopa::scene::Component* c = comp.get();
                if (!dir)   if (auto* x = dynamic_cast<DirectionalLightComponent*>(c)) { dir = true; if (!fs.dir_light) fs.dir_light = x; }
                if (!point) if (auto* x = dynamic_cast<PointLightComponent*>(c)) { point = true; fs.point_lights.push_back(x); }
                if (!spot)  if (auto* x = dynamic_cast<SpotLightComponent*>(c))  { spot = true;  fs.spot_lights.push_back(x); }
                if (!mesh)  if (auto* x = dynamic_cast<MeshRenderer*>(c))        { mesh = true;  fs.mesh_renderers.push_back(x); }
                if (!sdf)   if (auto* x = dynamic_cast<SdfRenderer*>(c))         { sdf = true;   fs.sdf_renderers.push_back(x); }
                if (!vol)   if (auto* x = dynamic_cast<VolumeComponent*>(c))     { vol = true;   fs.volumes.push_back(x); }
            }
            for (const auto& child : obj.children()) visit(*child);
    };
    for (const auto& root : scene.root_objects()) visit(*root);
}

const coopa::gfx::memory::Buffer* ToyRenderPipeline::static_instances_for_(const ParticleMeshBatch& pb) {
    StaticInstances& e = static_instances_[pb.static_key];
    e.last_frame = static_frame_;
    const uint32_t total = std::max(pb.static_total, pb.count);
    if (e.buffer && e.version == pb.static_version && e.count == total) return e.buffer.get();
    if (total == 0 || pb.matrices == nullptr) return nullptr;
    // A changed batch gets a NEW buffer: the old one may still be read by frames in flight.
    if (e.buffer) static_graveyard_.push_back({static_frame_, std::move(e.buffer)});
    using coopa::gfx::engine::data::InstanceData;
    std::vector<InstanceData> data(total);
    for (uint32_t k = 0; k < total; ++k) {
        data[k].model = pb.matrices[k];
        data[k].prev_model = pb.matrices[k];   // static: no motion
        data[k].snow_anchor = pb.matrices[k];
    }
    e.buffer = std::make_unique<coopa::gfx::memory::Buffer>(device_, allocator_, sizeof(InstanceData) * total,
                                                             coopa::gfx::BufferUsage::Vertex, coopa::gfx::MemoryResidency::CpuToGpu);
    e.buffer->upload(data.data(), sizeof(InstanceData) * total);
    e.version = pb.static_version;
    e.count = total;
    return e.buffer.get();
}

void ToyRenderPipeline::tick_static_instances_() {
    ++static_frame_;
    constexpr uint64_t k_retire_frames = coopa::gfx::presentation::MAX_FRAMES_IN_FLIGHT + 2;
    static_graveyard_.erase(std::remove_if(static_graveyard_.begin(), static_graveyard_.end(),
                                           [&](const auto& g) { return static_frame_ - g.first > k_retire_frames; }),
                            static_graveyard_.end());
    // Batches not drawn for a while (out of range, removed): retire their buffers.
    for (auto it = static_instances_.begin(); it != static_instances_.end();) {
        if (static_frame_ - it->second.last_frame > 30) {
            if (it->second.buffer) static_graveyard_.push_back({static_frame_, std::move(it->second.buffer)});
            it = static_instances_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace render
} // namespace toy
