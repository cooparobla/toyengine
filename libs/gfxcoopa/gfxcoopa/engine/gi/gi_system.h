/**
 * @file gi_system.h
 * @brief Orchestrates Global Illumination (GI) probe baking, GPU buffer management, and descriptor binding.
 */

#ifndef GFXCOOPA_ENGINE_GI_GI_SYSTEM_H
#define GFXCOOPA_ENGINE_GI_GI_SYSTEM_H

#include <cmath>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/gi/brdf_lut.h>
#include <gfxcoopa/engine/gi/gi_data.h>
#include <gfxcoopa/engine/targets/cubemap_target.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/engine/util/instance_batcher.h>
#include <gfxcoopa/engine/util/material_texture_cache.h>
#include <gfxcoopa/engine/data/camera_ubo.h>
#include <gfxcoopa/engine/data/light_data.h>
#include <gfxcoopa/pipeline/descriptor.h>

#include <gfxcoopa/engine/gi/gi_baker.h>
#include <gfxcoopa/engine/passes/env_prefilter_pass.h>
#include <gfxcoopa/engine/passes/probe_capture_pass.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/components/renderable_ref.h>
#include <gfxcoopa/engine/components/directional_light.h>
#include <gfxcoopa/engine/components/point_light.h>
#include <gfxcoopa/engine/components/spot_light.h>
#include <gfxcoopa/engine/components/gi_probe_volume.h>
#include <gfxcoopa/engine/components/reflection_probe.h>
#include <gfxcoopa/pipeline/shader_library.h>
#include <coopa/scene/scene.h>

#include <algorithm>
#include <memory>
#include <vector>
#include <iostream>

namespace coopa {
namespace gfx {
namespace engine {
namespace gi {

using targets::CubemapTarget;
using util::Sampler;
using passes::EnvPrefilterPass;
using passes::ProbeCapturePass;
using util::MaterialTextureCache;
using components::MeshRenderer;
using components::gather_renderables;
using components::DirectionalLightComponent;
using components::PointLightComponent;
using components::SpotLightComponent;
using components::GiProbeVolumeComponent;
using components::ReflectionProbeComponent;

/**
 * @class GiSystem
 * @brief Owns the global-illumination resources a frame binds: the SH probe
 *        SSBO, the GI uniform buffer, the BRDF LUT and the reflection cubemaps.
 *
 * bake() runs once before the first frame is recorded, filling the probe grid
 * (via GiBaker) and rendering each reflection probe's cubemap. After that the
 * system is read-only and exposes one descriptor set for passes to bind.
 */
class GiSystem {
public:
    GiSystem(coopa::gfx::core::Device& device,
             coopa::gfx::memory::Allocator& allocator,
             coopa::gfx::command::CommandPool& cmd_pool,
             const coopa::gfx::pipeline::ShaderLibrary& shaders)
        : device_(device), allocator_(allocator), cmd_pool_(cmd_pool), shaders_(shaders)
    {
        // 1. Generate BRDF Integration LUT
        brdf_lut_ = std::make_unique<BRDFLUT>(
            device, allocator, cmd_pool, shaders_
        );

        // 2. Initialize GPU buffers
        gi_data_ = std::make_unique<GiData>(device, allocator);

        // 2b. Self-contained probe-capture resources (camera, light, BRDF LUT
        //     descriptor infra + the prefilter's source-cube descriptor).
        //     Deliberately NOT shared with blendy's PbrRenderPipeline's per-frame
        //     camera/light UBOs: bake() runs once, early -- before
        //     renderables_ is built, before the frame's camera/light data is
        //     written, and before any shadow pass has ever executed (shadow
        //     images are still UNDEFINED at that point). Owning private
        //     copies lets the bake stay where it is instead of restructuring
        //     the frame loop. Built once here since bake() only ever runs once.
        capture_camera_ = std::make_unique<data::CameraUBO>(device, allocator);
        capture_lights_ = std::make_unique<data::LightData>(device, allocator);

        cap_camera_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .uniform_buffer(0, coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment)
                .build(device));
        cap_light_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .uniform_buffer(0, coopa::gfx::ShaderStage::Fragment)
                .build(device));
        cap_brdf_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .combined_sampler(0, coopa::gfx::ShaderStage::Fragment)
                .build(device));
        cap_source_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .combined_sampler(0, coopa::gfx::ShaderStage::Fragment)
                .build(device));

        // Pool sized for camera+light+brdf (1 each, reused across every probe
        // bake) plus up to MAX_REFLECTION_PROBES prefilter-source sets (one
        // per cubemap, since each is bound to a specific mip0_cube_view()) --
        // sizes derived from the layouts above rather than hand-computed.
        cap_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            coopa::gfx::pipeline::DescriptorPoolBuilder()
                .add_sets(*cap_camera_layout_, 1)
                .add_sets(*cap_light_layout_, 1)
                .add_sets(*cap_brdf_layout_, 1)
                .add_sets(*cap_source_layout_, MAX_REFLECTION_PROBES)
                .build(device));

        cap_camera_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *cap_pool_, *cap_camera_layout_);
        cap_light_set_  = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *cap_pool_, *cap_light_layout_);
        cap_brdf_set_   = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *cap_pool_, *cap_brdf_layout_);

        cap_camera_set_->bind_buffer(0, capture_camera_->buffer());
        cap_light_set_->bind_buffer(0, capture_lights_->buffer());
        cap_brdf_set_->bind_image(0, brdf_lut_->view_typed(), brdf_lut_->sampler_object());

        // Pre-allocate all MAX_REFLECTION_PROBES source sets up front (fixed
        // pool capacity, matching cap_pool_'s sizing above). Each gets bound
        // to a specific cubemap's mip0_cube_view() in create_cubemap_target_().
        cap_source_sets_.resize(MAX_REFLECTION_PROBES);
        for (auto& s : cap_source_sets_) {
            s = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *cap_pool_, *cap_source_layout_);
        }

        // 2c. Material texture cache (albedo/normal/metallic-roughness/alpha-mask), shared
        //     with ToyRenderPipeline's G-buffer/shadow/transparent passes in spirit but a
        //     private instance here -- this bake runs once, early, self-contained like every
        //     other cap_*_ resource above, not per-frame.
        material_cache_ = std::make_unique<MaterialTextureCache>(device, allocator, cmd_pool);

        // 3. Initialize cubemap slot 0 (the default/no-probe-yet target) and
        //    the capture pipelines built against its render passes. Every
        //    CubemapTarget shares identical hardcoded format/attachment
        //    parameters, so their render passes are mutually compatible per
        //    Vulkan's render-pass-compatibility rules -- env_prefilter_/
        //    probe_capture_ are built ONCE here and reused to bake every
        //    subsequent probe's cubemap, not rebuilt per probe.
        create_cubemap_target_(0, 256);
        ensure_capture_pipelines_();

        cmd_pool.submit_once([&](coopa::gfx::command::CommandBuffer& cmd) {
            cubemap_targets_[0]->transition_to_shader_read(cmd, VK_IMAGE_LAYOUT_UNDEFINED);
        });

        // 4. Create Descriptor Set Layout for Set 3.
        // Bindings 3-6: reflectionMap_0..3 (Combined Image Samplers). Four
        // separately-named bindings, not a samplerCube[] array -- this device
        // does not enable shaderSampledImageArrayDynamicIndexing, so a
        // dynamically-indexed sampler array would be illegal. See
        // ibl_specular_probes_blended() in gfx/ibl.glsl for the shader side.
        coopa::gfx::pipeline::DescriptorLayoutBuilder gi_layout_builder;
        gi_layout_builder.uniform_buffer(0, coopa::gfx::ShaderStage::Fragment);   // GiUniforms (UBO)
        gi_layout_builder.storage_buffer(1, coopa::gfx::ShaderStage::Fragment);  // gi::SHProbes (SSBO)
        gi_layout_builder.combined_sampler(2, coopa::gfx::ShaderStage::Fragment); // BRDF LUT
        for (uint32_t i = 0; i < MAX_REFLECTION_PROBES; ++i) {
            gi_layout_builder.combined_sampler(3 + i, coopa::gfx::ShaderStage::Fragment);
        }
        // ReflectionProbeUBO { ReflectionProbeData probes[MAX_REFLECTION_PROBES]; }
        gi_layout_builder.uniform_buffer(7, coopa::gfx::ShaderStage::Fragment);
        gi_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(gi_layout_builder.build(device));

        // 5. Create Descriptor Pool
        gi_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            coopa::gfx::pipeline::DescriptorPoolBuilder()
                .add_sets(*gi_layout_, 1)
                .build(device));

        // 6. Allocate Descriptor Set
        gi_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *gi_pool_, *gi_layout_);

        update_descriptors_();
    }

    ~GiSystem() = default;

    GiSystem(const GiSystem&) = delete;
    GiSystem& operator=(const GiSystem&) = delete;

    /// Number of prefilter mips for a given cubemap face resolution. Stops at
    /// 8x8 -- below that a GGX lobe covers most of the face and further mips
    /// add nothing. 256 -> 6, 128 -> 5, 512 -> 7.
    static uint32_t prefilter_mip_count(uint32_t res) {
        uint32_t m = 1;
        while ((res >> m) >= 8u && m < 8u) ++m;
        return m;
    }

    void bake(coopa::scene::Scene& scene) {
        auto volumes = scene.get_components<GiProbeVolumeComponent>();
        if (!volumes.empty() && volumes[0]) {
            const auto* vol = volumes[0];
            std::cout << "[GiSystem] Baking SH light probe volume ("
                      << vol->grid_resolution.x << "x"
                      << vol->grid_resolution.y << "x"
                      << vol->grid_resolution.z << " = "
                      << vol->total_probes() << " probes)..." << std::endl;

            auto probes = GiBaker::bake_cpu(*vol, scene);

            auto& u = gi_data_->uniforms();
            active_gi_intensity_ = vol->gi_intensity;
            u.grid_origin  = glm::vec4(vol->origin, 0.0f);
            u.grid_spacing = glm::vec4(vol->spacing(), 0.0f);
            u.grid_counts  = glm::ivec4(vol->grid_resolution, vol->total_probes());
            u.gi_params.x  = vol->gi_intensity;
            // gi_params.z (max roughness mip) is derived from the cubemap's
            // real mip count in create_cubemap_(), not hardcoded here -- it
            // may not even be current yet if a reflection probe below
            // requests a different resolution.

            gi_data_->upload_uniforms();
            gi_data_->upload_probes(probes);
            gi_active_ = true;
        }

        auto reflection_probes = scene.get_components<ReflectionProbeComponent>();
        if (!reflection_probes.empty()) {
            using ReflectionProbePtr = std::remove_reference_t<decltype(reflection_probes[0])>;
            std::vector<ReflectionProbePtr> sorted_probes;
            for (auto rp : reflection_probes) {
                if (rp) {
                    sorted_probes.push_back(rp);
                }
            }

            if (!sorted_probes.empty()) {
                std::sort(sorted_probes.begin(), sorted_probes.end(),
                    [](ReflectionProbePtr a, ReflectionProbePtr b) {
                        return a->importance > b->importance;
                    });

            // Cap at MAX_REFLECTION_PROBES, keeping the highest-importance
            // ones (already sorted). Any probes beyond the cap are silently
            // dropped -- same behavior as point lights already capping at 16.
            const size_t n = std::min(sorted_probes.size(),
                                      static_cast<size_t>(MAX_REFLECTION_PROBES));

            std::vector<ReflectionProbeUniforms> ru_list(n);
            for (size_t i = 0; i < n; ++i) {
                const auto* rp = sorted_probes[i];

                // Honor the probe's authored resolution. This only ever runs
                // on the first bake (GiSystem::bake is latched to run once
                // before any frame is recorded), so nothing in flight
                // references the old cubemap.
                if (i >= cubemap_targets_.size() || !cubemap_targets_[i] ||
                    cubemap_targets_[i]->resolution() != rp->resolution) {
                    device_.wait_idle();
                    create_cubemap_target_(i, rp->resolution);
                }

                auto& ru = ru_list[i];
                ru.probe_position = glm::vec4(rp->get_world_position(), 0.0f);
                ru.box_min        = glm::vec4(rp->get_box_min(), 0.0f);
                ru.box_max        = glm::vec4(rp->get_box_max(), 0.0f);
                ru.params         = glm::vec4(rp->blend_distance, static_cast<float>(rp->importance),
                                              rp->intensity,
                                              static_cast<float>(cubemap_targets_[i]->mip_levels() - 1));

                capture_reflection_probe_(scene, i, rp->get_world_position());
            }

            active_num_reflection_probes_ = static_cast<float>(n);
            // gi_params.y/z stay written for any stray reader, but the actual
            // blend (ibl_specular_probes_blended in ibl.glsl) uses the
            // per-probe intensity/max_mip packed into each ReflectionProbeUniforms.params
            // above instead of these globals -- necessary once probes can
            // have different intensities or resolutions from each other.
            gi_data_->uniforms().gi_params.y = sorted_probes[0]->intensity;
            gi_data_->uniforms().gi_params.z = static_cast<float>(cubemap_targets_[0]->mip_levels() - 1);
            gi_data_->uniforms().gi_params.w = static_cast<float>(n); // num reflection probes
            gi_data_->upload_uniforms();
            gi_data_->upload_reflection_uniforms(ru_list);

            gi_active_ = true;
        }
        }

        update_descriptors_();
    }

    void set_gi_enabled(bool enabled) {
        if (!gi_data_) return;
        if (!enabled) {
            gi_data_->uniforms().gi_params.x = 0.0f;
            gi_data_->uniforms().gi_params.w = 0.0f;
        } else {
            gi_data_->uniforms().gi_params.x = active_gi_intensity_;
            gi_data_->uniforms().gi_params.w = active_num_reflection_probes_;
        }
        gi_data_->upload_uniforms();
    }

    void update() {
        if (gi_data_) {
            gi_data_->upload_uniforms();
        }
    }

    /// @brief Binds gi_set_ at set index 3. Relies on the caller having
    /// already called cmd.bind_pipeline() for this draw (true at every
    /// ExtraSets::bind call site -- see extra_sets.h).
    void bind(coopa::gfx::command::CommandBuffer& cmd) const {
        bind_at_set(cmd, 3);
    }

    void bind_at_set(coopa::gfx::command::CommandBuffer& cmd, uint32_t set_index) const {
        if (gi_set_) {
            cmd.bind_descriptor_set(*gi_set_, set_index);
        }
    }

    coopa::gfx::pipeline::DescriptorSetLayout& layout() { return *gi_layout_; }
    const coopa::gfx::pipeline::DescriptorSetLayout& layout() const { return *gi_layout_; }

    VkImageView brdf_lut_view() const { return brdf_lut_ ? brdf_lut_->view() : VK_NULL_HANDLE; }
    coopa::gfx::TextureView brdf_lut_view_typed() const {
        return brdf_lut_ ? brdf_lut_->view_typed() : coopa::gfx::TextureView::null();
    }
    VkSampler brdf_lut_sampler() const { return brdf_lut_ ? brdf_lut_->sampler() : VK_NULL_HANDLE; }

    bool is_active() const { return gi_active_; }

private:
    /// (Re)creates cubemap_targets_[index]'s render target and dedicated
    /// sampler, and rebinds cap_source_sets_[index] to its mip0 view. Called
    /// once per active probe slot from bake() (and once for slot 0 from the
    /// constructor, before any probe exists, so Set 3's bindings always have
    /// something valid). Resizes cubemap_targets_/cubemap_samplers_ if index
    /// is beyond their current size.
    void create_cubemap_target_(size_t index, uint32_t face_resolution) {
        if (cubemap_targets_.size() <= index) {
            cubemap_targets_.resize(index + 1);
            cubemap_samplers_.resize(index + 1);
        }

        const uint32_t mips = prefilter_mip_count(face_resolution);
        cubemap_targets_[index] = std::make_unique<CubemapTarget>(
            device_, allocator_, face_resolution, mips
        );

        // This needs its own sampler rather than the BRDF LUT's: that one has
        // max_lod = 0.0f, which would silently clamp every textureLod() in the
        // probe path back to mip 0. LINEAR mipmap mode is what makes roughness
        // blend smoothly across mip boundaries instead of banding.
        cubemap_samplers_[index] = std::make_unique<Sampler>(
            device_, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            static_cast<float>(mips), VK_SAMPLER_MIPMAP_MODE_LINEAR
        );

        // Rebind this slot's prefilter source: the mip-0 view belongs to the
        // new image. Reuses the new sampler (LINEAR/CLAMP); the mip-0-only
        // view clamps any LOD to level 0 regardless of the sampler's maxLod,
        // and env_prefilter.frag uses textureLod(..., 0.0) anyway.
        cap_source_sets_[index]->bind_image(0, cubemap_targets_[index]->mip0_cube_view(),
                                            cubemap_samplers_[index]->handle());

        update_descriptors_();
    }

    /// Builds env_prefilter_/probe_capture_ exactly once, against
    /// cubemap_targets_[0]'s render passes. Every CubemapTarget shares
    /// identical hardcoded format/attachment parameters, so their render
    /// passes are mutually compatible per Vulkan's render-pass-compatibility
    /// rules -- these pipelines are reused unchanged to bake every
    /// subsequent probe's cubemap, never rebuilt per probe.
    void ensure_capture_pipelines_() {
        if (env_prefilter_) return;

        env_prefilter_ = std::make_unique<EnvPrefilterPass>(
            device_, cubemap_targets_[0]->prefilter_render_pass(),
            shaders_("env_prefilter.vert"),
            shaders_("env_prefilter.frag"),
            *cap_source_layout_
        );

        probe_capture_ = std::make_unique<ProbeCapturePass>(
            device_, cubemap_targets_[0]->render_pass(),
            *cap_camera_layout_, *cap_light_layout_, *cap_brdf_layout_,
            shaders_, &material_cache_->layout_object()
        );
    }

    /// Renders real scene geometry into all 6 faces of
    /// cubemap_targets_[index]'s mip 0 from probe_pos, then GGX-prefilters
    /// mips 1..N-1 from that capture. Fully self-contained (own camera/light
    /// UBOs, shared across every probe index) -- see the constructor comment
    /// for why this doesn't reuse blendy's PbrRenderPipeline's per-frame ones.
    /**
     * @brief True when two materials would produce byte-identical probe-capture push constants.
     *
     * Same fields as blendy's PbrRenderPipeline's own same_material_() predicate,
     * minus alpha/alpha_cutoff: probe capture always forces albedo.a=1 and
     * cutoff=0 regardless of the source material (see capture_reflection_probe_()'s
     * "always render fully opaque" comment), so those two never actually
     * vary between any two items here. Also excludes emissive: ProbeCapturePass is a
     * forward path and its PushConstants don't carry the deferred-only `emissive` field
     * (see PBRMaterial::gpu_emissive()'s doc), so two materials differing only in
     * emissive still produce byte-identical push constants here.
     *
     * Also compares the four texture handles (raw Texture* identity, matching
     * MaterialTextureCache::set_for()'s own key) -- two materials with identical scalar
     * factors but different albedo/normal/metallic_roughness/alpha_mask textures must NOT
     * merge into one batch, or the merged batch would render every instance with only the
     * first item's textures.
     */
    static bool same_capture_material_(const components::PBRMaterial& a, const components::PBRMaterial& b) {
        return a.albedo == b.albedo && a.metallic == b.metallic
            && a.roughness == b.roughness && a.ao == b.ao
            && a.albedo_handle.get() == b.albedo_handle.get()
            && a.normal_handle.get() == b.normal_handle.get()
            && a.metallic_roughness_handle.get() == b.metallic_roughness_handle.get()
            && a.alpha_mask_handle.get() == b.alpha_mask_handle.get();
    }

    void capture_reflection_probe_(coopa::scene::Scene& scene, size_t index, const glm::vec3& probe_pos) {
        auto& target = *cubemap_targets_[index];
        auto& source_set = *cap_source_sets_[index];

        std::cout << "[GiSystem] Capturing reflection probe " << index << " geometry ("
                  << target.resolution() << "^2 x 6 faces)..." << std::endl;

        // --- (1) Lights, straight from the scene. Shadows are disabled: no
        //         shadow pass has run yet at bake time (shadow images are
        //         still UNDEFINED), and probe_capture.frag declares no
        //         shadow samplers, so probe capture is unshadowed.
        auto& lu = capture_lights_->data();
        lu = data::LightUBO{}; // reset counts / point array
        if (auto* dir_light = scene.find_first_component<DirectionalLightComponent>()) {
            lu.dir_direction     = glm::vec4(glm::normalize(dir_light->direction), dir_light->intensity);
            lu.dir_color         = glm::vec4(dir_light->color, 1.0f);
            lu.dir_shadow_params = glm::vec4(0.0f); // z = shadow_enabled = 0
            lu.light_counts.x    = 1;
        }
        uint32_t n = 0;
        for (auto* pl : scene.get_components<PointLightComponent>()) {
            if (!pl || n >= data::MAX_POINT_LIGHTS) break;
            auto& g = lu.point_lights[n];
            g.position_range  = glm::vec4(pl->get_world_position(), pl->range);
            g.color_intensity = glm::vec4(pl->color, pl->intensity);
            g.attenuation     = glm::vec4(pl->attenuation_constant, pl->attenuation_linear,
                                          pl->attenuation_quadratic, 0.0f); // w = cast_shadows = 0
            ++n;
        }
        lu.light_counts.y = n;

        uint32_t ns = 0;
        for (auto* sl : scene.get_components<SpotLightComponent>()) {
            if (!sl || ns >= data::MAX_SPOT_LIGHTS) break;
            auto& g = lu.spot_lights[ns];
            g.position_range  = glm::vec4(sl->get_world_position(), sl->range);
            g.direction_cone  = glm::vec4(sl->get_world_direction(),
                                          std::cos(glm::radians(sl->clamped_outer_angle())));
            g.color_intensity = glm::vec4(sl->color, sl->intensity);
            g.params          = glm::vec4(sl->attenuation_constant,
                                          std::cos(glm::radians(sl->clamped_inner_angle())),
                                          0.0f, 0.0f); // z = cast_shadows = 0, see (1)'s doc above
            ++ns;
        }
        lu.light_counts.z = ns;
        lu.light_counts.w = 0xFFFFFFFFu; // no shadow-casting spot at bake time either

        capture_lights_->upload();

        // --- (2) Flatten the scene once. gather_renderables() returns active
        //         objects that have a MeshRenderer; transform and readiness
        //         are ours to check.
        const auto renderables = gather_renderables(scene);
        if (renderables.empty()) return;
        struct CaptureItem {
            MeshRenderer* mr;
            glm::mat4 world;
        };
        std::vector<CaptureItem> items;
        for (const auto& ref : renderables) {
            auto* mr = ref.renderer;
            auto* tc = ref.transform;
            if (!mr || !mr->is_ready() || !mr->affects_reflection_probes) continue;
            items.push_back({ mr, tc ? tc->get_world_matrix() : glm::mat4(1.0f) });
        }

        // A local, one-shot InstanceBatcher: this is a bake (called a
        // handful of times, not per frame), so there's no reason to keep a
        // persistent instance buffer around between bakes the way
        // blendy's PbrRenderPipeline does for its per-frame geometry passes. Same
        // batch key as the G-buffer/transparent passes (mesh + material
        // equality) -- probe capture always forces albedo.a=1 and cutoff=0,
        // so those two fields are excluded from the comparison here.
        util::InstanceBatcher batcher(device_, allocator_, items.size());
        batcher.begin();
        for (size_t i = 0; i < items.size(); ++i) {
            const data::Mesh* mesh = items[i].mr->get_mesh().get();
            bool continue_batch = i > 0 &&
                mesh == items[i - 1].mr->get_mesh().get() &&
                same_capture_material_(items[i].mr->material, items[i - 1].mr->material);
            batcher.add(mesh, continue_batch, items[i].world, static_cast<uint32_t>(i));
        }
        batcher.upload();

        // --- (3) Six faces, one submit-and-wait each. data::CameraUBO::update() is
        //         a host map/memcpy/unmap, so it cannot be safely re-issued
        //         between draws inside one still-open command buffer. One
        //         begin_single_use()/end_single_use() per face (each ends in
        //         a queue wait) makes the host write safely ordered against
        //         the GPU read, at the cost of 6 small submissions -- fine
        //         for a one-time bake.
        const glm::mat4 face_proj = CubemapTarget::get_face_projection(0.05f, 200.0f);

        for (uint32_t face = 0; face < 6; ++face) {
            capture_camera_->update(
                CubemapTarget::get_face_view(face, probe_pos),
                face_proj, probe_pos, 0.0f);

            cmd_pool_.submit_once([&](coopa::gfx::command::CommandBuffer& cmd) {
                // Colour attachment loadOp is hardcoded CLEAR, so the sky must be
                // drawn INSIDE this same pass instance -- a separate pre-fill
                // pass would just be cleared away by this one.
                target.begin_face_pass(cmd, face);

                probe_capture_->draw_sky_background(cmd, face); // depth off, fills every texel

                probe_capture_->bind_geometry(cmd); // depth on, LESS, writes
                cmd.bind_descriptor_set(*cap_camera_set_, 0);
                cmd.bind_descriptor_set(*cap_light_set_,  1);
                cmd.bind_descriptor_set(*cap_brdf_set_,   2);

                batcher.bind(cmd);
                for (const auto& b : batcher.batches()) {
                    const auto& mat = items[b.item_index].mr->material;
                    ProbeCapturePass::PushConstants pc{};
                    // Probe captures always render fully opaque -- mat.alpha/alpha_mode are not
                    // forwarded here. Transparent renderers are still baked in as opaque geometry
                    // (not excluded); see the "known limitations" note in the transparency plan.
                    pc.albedo       = glm::vec4(mat.albedo, 1.0f);
                    pc.metallic     = mat.metallic;
                    pc.roughness    = mat.roughness;
                    pc.ao           = mat.ao;
                    pc.alpha_cutoff = 0.0f;
                    probe_capture_->push(cmd, pc);
                    probe_capture_->bind_material(cmd, material_cache_->set_for(mat));
                    b.mesh->bind(cmd);
                    b.mesh->draw(cmd, b.instance_count, b.first_instance);
                }

                target.end_face_pass(cmd);
            });
        }

        // --- (4) Two-stage barrier + prefilter of mips 1..N-1 from the
        //         captured mip 0.
        std::cout << "[GiSystem] Prefiltering " << target.mip_levels()
                  << " mips from captured mip 0..." << std::endl;

        const uint32_t mips = target.mip_levels();
        cmd_pool_.submit_once([&](coopa::gfx::command::CommandBuffer& cmd) {
            // mip 0 becomes the prefilter's SOURCE, so it must be readable first.
            target.transition_mip_range_to_shader_read(cmd, 0, 1);

            if (mips > 1) {
                // Legal simultaneously: mip 0 in SHADER_READ_ONLY (sampled via
                // mip0_cube_view) while mips 1..N-1 are still color attachments --
                // layouts are per-subresource and these ranges are disjoint, so
                // there is no feedback loop.
                env_prefilter_->execute(cmd, target, source_set);
                target.transition_mip_range_to_shader_read(cmd, 1, mips - 1);
            }
        });
    }

    void update_descriptors_() {
        if (!gi_set_ || cubemap_targets_.empty() || !cubemap_targets_[0] || !cubemap_samplers_[0]) return;
        gi_set_->bind_buffer(0, gi_data_->uniforms_buffer());
        gi_set_->bind_storage_buffer(1, gi_data_->probes_buffer());
        gi_set_->bind_image(2, brdf_lut_->view_typed(), brdf_lut_->sampler_object());
        // Bind every reflectionMap_0..3 slot. Slots beyond how many cubemaps
        // actually exist yet fall back to slot 0 as a harmless placeholder --
        // never sampled with nonzero weight since the shader gates on
        // gi.gi_params.w (the active probe count), but every descriptor slot
        // must have something valid bound regardless.
        for (uint32_t i = 0; i < MAX_REFLECTION_PROBES; ++i) {
            size_t src = (i < cubemap_targets_.size() && cubemap_targets_[i] && cubemap_samplers_[i]) ? i : 0;
            gi_set_->bind_image(3 + i, cubemap_targets_[src]->cubemap_view(), cubemap_samplers_[src]->handle());
        }
        gi_set_->bind_buffer(7, gi_data_->reflection_buffer());
    }

    coopa::gfx::core::Device& device_;
    coopa::gfx::memory::Allocator& allocator_;
    coopa::gfx::command::CommandPool& cmd_pool_;
    coopa::gfx::pipeline::ShaderLibrary shaders_; /**< By value: ProbeCapturePass is built lazily
                                                        in ensure_capture_pipelines_(), well after
                                                        construction, so a reference could dangle. */
    std::unique_ptr<BRDFLUT>        brdf_lut_;
    std::unique_ptr<GiData>         gi_data_;

    // Per-probe-slot GPU resources, indexed 0..(active_count-1). Sized/rebuilt
    // by create_cubemap_target_() as bake() discovers each active probe.
    std::vector<std::unique_ptr<CubemapTarget>> cubemap_targets_;
    std::vector<std::unique_ptr<Sampler>>       cubemap_samplers_;

    // Built exactly once (against cubemap_targets_[0]'s render passes) and
    // reused to bake every probe slot -- see ensure_capture_pipelines_().
    std::unique_ptr<EnvPrefilterPass>   env_prefilter_;

    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> gi_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>      gi_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>       gi_set_;

    // Self-contained resources for real reflection-probe geometry capture
    // (see capture_reflection_probe_()). Independent of blendy's PbrRenderPipeline's
    // per-frame camera/light UBOs -- built once, reused across every probe's bake.
    std::unique_ptr<data::CameraUBO> capture_camera_;
    std::unique_ptr<data::LightData> capture_lights_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> cap_camera_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> cap_light_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> cap_brdf_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> cap_source_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>      cap_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>       cap_camera_set_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>       cap_light_set_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>       cap_brdf_set_;
    // One prefilter-source set per probe slot (each bound to that slot's own
    // mip0_cube_view() in create_cubemap_target_()), pre-allocated to
    // MAX_REFLECTION_PROBES in the constructor.
    std::vector<std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>> cap_source_sets_;
    std::unique_ptr<ProbeCapturePass>                          probe_capture_;
    std::unique_ptr<MaterialTextureCache>                      material_cache_;

    bool gi_active_ = false;
    float active_gi_intensity_ = 1.0f;
    float active_num_reflection_probes_ = 0.0f;
};

} // namespace gi
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_GI_GI_SYSTEM_H
