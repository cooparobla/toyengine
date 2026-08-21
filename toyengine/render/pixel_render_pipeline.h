/**
 * @file pixel_render_pipeline.h
 * @brief Orchestrates the low-resolution deferred pixel-art frame graph.
 *
 * Owns the G-buffer and low-resolution LDR targets, the shared camera UBO
 * and descriptor sets, and every pass. Records the entire frame into a
 * single command buffer via Renderer::begin_frame's pre_pass_fn seam (see
 * gfxcoopa/presentation/renderer.h) -- no per-frame vkQueueWaitIdle, unlike
 * blendy's PbrRenderPipeline::record_offscreen_().
 *
 * Phase 6 status: G-buffer + banded PBR lighting + hard directional and
 * single-point-light-cube shadows + analytic skybox, followed by
 * PixelPostPass (outline + exposure + Bayer dither + palette quantization)
 * into a second low-res LDR target that UpscalePass then reads. Only one
 * point light can cast a shadow at a time (gfxcoopa's ShadowMapTarget holds
 * exactly one cube map, not one per light -- see shadow_map_target.h);
 * every other point light still lights the scene, just without occlusion.
 * No true HDR/ACES tonemap -- pixel_lighting.frag's banded output already
 * stays in a reasonable LDR-ish range, so PixelPostPass's "exposure" is a
 * simple multiply-and-clamp, not a filmic curve. GBufferVisualizePass is
 * retained as an unused standalone diagnostic (see its own file doc).
 */

#ifndef TOYENGINE_RENDER_PIXEL_RENDER_PIPELINE_H
#define TOYENGINE_RENDER_PIXEL_RENDER_PIPELINE_H

#include <volk/volk.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/core/swapchain.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/presentation/renderer.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>
#include <gfxcoopa/engine/targets/gbuffer_target.h>
#include <gfxcoopa/engine/targets/shadow_map_target.h>
#include <gfxcoopa/engine/passes/gbuffer_pipeline.h>
#include <gfxcoopa/engine/passes/shadow_pipeline.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/engine/data/camera_ubo.h>
#include <gfxcoopa/engine/data/light_data.h>
#include <gfxcoopa/engine/components/camera_component.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/components/directional_light.h>
#include <gfxcoopa/engine/components/point_light.h>
#include <gfxcoopa/engine/passes/skybox_pass.h>

#include <coopa/scene/scene.h>
#include <coopa/scene/components/transform_component.h>

#include <toyengine/render/pixel_render_config.h>
#include <toyengine/render/pixel_math.h>
#include <toyengine/render/instance_stream.h>
#include <toyengine/render/palette_lut.h>
#include <toyengine/render/passes/gbuffer_visualize_pass.h>
#include <toyengine/render/passes/pixel_lighting_pass.h>
#include <toyengine/render/passes/pixel_post_pass.h>
#include <toyengine/render/passes/upscale_pass.h>

namespace toy {
namespace render {

/**
 * @class PixelRenderPipeline
 * @brief Renders a scene through the low-resolution deferred pixel-art
 *        frame graph and upscales it into the swapchain.
 */
class PixelRenderPipeline {
public:
    PixelRenderPipeline(coopa::gfx::core::Device& device,
                        coopa::gfx::memory::Allocator& allocator,
                        coopa::gfx::core::Swapchain& swapchain,
                        coopa::gfx::pipeline::RenderPass& swapchain_pass,
                        coopa::gfx::command::CommandPool& cmd_pool,
                        PixelRenderConfig config)
        : device_(device), allocator_(allocator), swapchain_(swapchain), config_(std::move(config)),
          render_extent_(compute_render_extent(config_, swapchain.extent().width, swapchain.extent().height)),
          gbuffer_target_(device, allocator, render_extent_.width, render_extent_.height),
          offscreen_target_(device, allocator, render_extent_.width, render_extent_.height, VK_FORMAT_R8G8B8A8_UNORM),
          post_target_(device, allocator, render_extent_.width, render_extent_.height, VK_FORMAT_R8G8B8A8_UNORM),
          nearest_sampler_(coopa::gfx::engine::util::Sampler::nearest(device)),
          linear_sampler_(coopa::gfx::engine::util::Sampler::linear(device)),
          shadow_sampler_(device, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
          camera_ubo_(device, allocator),
          light_data_(device, allocator),
          shadow_target_(device, allocator, config_.shadow_map_resolution, config_.cube_shadow_resolution),
          palette_lut_(PaletteLut::load(device, allocator, cmd_pool, config_.palette_path)),
          instance_stream_(device, allocator)
    {
        camera_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            device, std::vector<VkDescriptorSetLayoutBinding>{make_ubo_binding_(
                0, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)});

        camera_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            device, 1,
            std::vector<VkDescriptorPoolSize>{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}});

        camera_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *camera_pool_, *camera_layout_);
        camera_set_->bind_buffer(0, camera_ubo_.buffer());

        light_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            device, std::vector<VkDescriptorSetLayoutBinding>{make_ubo_binding_(0, VK_SHADER_STAGE_FRAGMENT_BIT)});
        light_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            device, 1, std::vector<VkDescriptorPoolSize>{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}});
        light_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *light_pool_, *light_layout_);
        light_set_->bind_buffer(0, light_data_.buffer());

        std::vector<VkDescriptorSetLayoutBinding> shadow_bindings{
            make_sampler_binding_(0, VK_SHADER_STAGE_FRAGMENT_BIT),
            make_sampler_binding_(1, VK_SHADER_STAGE_FRAGMENT_BIT)
        };
        shadow_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(device, shadow_bindings);
        shadow_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            device, 1, std::vector<VkDescriptorPoolSize>{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}});
        shadow_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *shadow_pool_, *shadow_layout_);
        shadow_set_->bind_image(0, shadow_target_.dir_shadow_view(), shadow_sampler_.handle());
        shadow_set_->bind_image(1, shadow_target_.cube_shadow_view(), shadow_sampler_.handle());

        shadow_pipeline_ = std::make_unique<coopa::gfx::engine::passes::ShadowPipeline>(
            device, shadow_target_.dir_render_pass(), shadow_target_.cube_render_pass(),
            config_.shader_dir + "/shadow_depth.vert.spv",
            config_.shader_dir + "/shadow_depth.frag.spv",
            config_.shader_dir + "/shadow_cube.vert.spv",
            config_.shader_dir + "/shadow_cube.frag.spv");

        gbuffer_pipeline_ = std::make_unique<coopa::gfx::engine::passes::GBufferPipeline>(
            device, gbuffer_target_.render_pass(), camera_layout_->handle(), VK_NULL_HANDLE,
            config_.shader_dir + "/gbuffer.vert.spv",
            config_.shader_dir + "/gbuffer.frag.spv");

        gbuffer_visualize_pass_ = std::make_unique<passes::GBufferVisualizePass>(
            device, offscreen_target_.render_pass_object(),
            config_.shader_dir + "/fullscreen.vert.spv",
            config_.shader_dir + "/gbuffer_visualize.frag.spv");
        gbuffer_visualize_pass_->set_albedo_image(gbuffer_target_.g0_view(), linear_sampler_);

        pixel_lighting_pass_ = std::make_unique<passes::PixelLightingPass>(
            device, offscreen_target_.render_pass_object(), camera_layout_->handle(), light_layout_->handle(),
            shadow_layout_->handle(),
            config_.shader_dir + "/fullscreen.vert.spv",
            config_.shader_dir + "/pixel_lighting.frag.spv");
        pixel_lighting_pass_->set_gbuffer_images(
            gbuffer_target_.g0_view(), gbuffer_target_.g1_view(), gbuffer_target_.g2_view(), linear_sampler_);

        skybox_pass_ = std::make_unique<coopa::gfx::engine::passes::SkyboxPass>(
            device, offscreen_target_.render_pass_object(), camera_layout_->handle(),
            config_.shader_dir + "/fullscreen.vert.spv",
            config_.shader_dir + "/skybox.frag.spv");
        skybox_pass_->set_gbuffer_normal_image(gbuffer_target_.g1_view(), linear_sampler_);

        pixel_post_pass_ = std::make_unique<passes::PixelPostPass>(
            device, post_target_.render_pass_object(),
            config_.shader_dir + "/fullscreen.vert.spv",
            config_.shader_dir + "/pixel_post.frag.spv");
        pixel_post_pass_->set_source_images(
            offscreen_target_.color_view(), gbuffer_target_.depth_view(), gbuffer_target_.g1_view(),
            palette_lut_.view(), linear_sampler_, nearest_sampler_);

        upscale_pass_ = std::make_unique<passes::UpscalePass>(
            device, swapchain_pass,
            config_.shader_dir + "/fullscreen.vert.spv",
            config_.shader_dir + "/upscale.frag.spv");
        upscale_pass_->set_source_image(post_target_.color_view(), nearest_sampler_);
    }

    PixelRenderPipeline(const PixelRenderPipeline&) = delete;
    PixelRenderPipeline& operator=(const PixelRenderPipeline&) = delete;

    /** @brief Low-resolution render width in pixels. */
    uint32_t render_width() const { return render_extent_.width; }
    /** @brief Low-resolution render height in pixels. */
    uint32_t render_height() const { return render_extent_.height; }
    /** @brief The final low-resolution LDR color image, for pixel-accurate screenshots. */
    VkImage low_res_color_image() const { return post_target_.color_image_object()->handle(); }

    /**
     * @brief Renders one frame of the given scene.
     * @return True if the frame was presented, false if the window was minimized.
     */
    bool render(coopa::gfx::presentation::Renderer& renderer, coopa::scene::Scene& scene) {
        using coopa::gfx::engine::components::CameraComponent;
        using coopa::gfx::engine::components::CameraType;
        using coopa::gfx::engine::components::MeshRenderer;
        using coopa::gfx::engine::components::DirectionalLightComponent;
        using coopa::gfx::engine::components::PointLightComponent;

        update_lights_(scene);

        auto* cam = scene.find_first_component<CameraComponent>();
        float aspect = static_cast<float>(render_extent_.width) / static_cast<float>(render_extent_.height);

        glm::mat4 view = cam ? cam->get_view_matrix() : glm::mat4(1.0f);
        glm::mat4 proj = cam ? cam->get_projection_matrix(aspect) : glm::mat4(1.0f);
        glm::vec3 cam_pos = cam ? cam->get_world_position() : glm::vec3(0.0f);

        float pixel_density = 0.0f;
        if (cam && config_.camera_pixel_snap) {
            if (cam->type == CameraType::Orthographic) {
                pixel_density = compute_pixel_density(true, cam->orthographic_size, render_extent_.height);
            } else if (!warned_perspective_snap_) {
                std::cerr << "[toyengine] camera_pixel_snap is enabled but the active camera is "
                             "perspective; snapping only applies to orthographic cameras and is "
                             "disabled for this camera.\n";
                warned_perspective_snap_ = true;
            }
        }
        camera_ubo_.update(view, proj, cam_pos, pixel_density);

        // Gather renderables once; the instance upload, the shadow AABB fit, and
        // the draw loops below all need the same list (and world matrices) in
        // the same order.
        auto renderers = scene.get_components<MeshRenderer>();
        std::vector<glm::mat4> world_matrices(renderers.size(), glm::mat4(1.0f));
        instance_stream_.begin(renderer.current_frame());
        std::vector<uint32_t> instance_idx(renderers.size(), UINT32_MAX);
        for (size_t i = 0; i < renderers.size(); ++i) {
            if (!renderers[i]->is_ready() || !renderers[i]->owner) continue;
            auto* tc = renderers[i]->owner->get_transform();
            if (!tc) continue;
            world_matrices[i] = tc->get_world_matrix();
            instance_idx[i] = instance_stream_.add(world_matrices[i]);
        }
        instance_stream_.upload();

        auto* dir_light = scene.find_first_component<DirectionalLightComponent>();
        bool cast_dir_shadow = config_.shadows_enabled && dir_light && dir_light->cast_shadows;
        if (dir_light) {
            update_dir_shadow_matrix_(dir_light->direction, renderers, world_matrices, cam, cast_dir_shadow);
        }

        // Only the scene's first shadow-casting point light gets a real cube map
        // (see the class doc for why). Marks light_data_.point_lights[0]'s
        // cast_shadows flag so pixel_lighting.frag knows to sample it.
        auto* shadow_point = find_first_shadow_casting_point_light_(scene);
        bool cast_point_shadow = config_.shadows_enabled && shadow_point != nullptr;
        if (cast_point_shadow) {
            auto& gpu0 = light_data_.data().point_lights[0];
            gpu0.attenuation.w = 1.0f;
        }
        light_data_.upload();

        LetterboxRect letterbox = compute_letterbox(
            swapchain_.extent().width, swapchain_.extent().height,
            render_extent_.width, render_extent_.height);

        return renderer.begin_frame(
            [&](coopa::gfx::command::CommandBuffer& cmd) {
                upscale_pass_->draw(cmd, letterbox);
            },
            VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}},
            nullptr,
            [&](coopa::gfx::command::CommandBuffer& cmd) {
                record_directional_shadow_(cmd, renderers, instance_idx, cast_dir_shadow);
                record_point_shadow_(cmd, renderers, instance_idx, shadow_point, cast_point_shadow);
                record_gbuffer_(cmd, renderers, instance_idx);

                // GBufferTarget's render pass leaves the depth attachment in
                // DEPTH_STENCIL_ATTACHMENT_OPTIMAL (only G0/G1/G2 color end in
                // SHADER_READ_ONLY_OPTIMAL -- see gbuffer_target.h), but
                // pixel_post.frag samples it for the outline edge detector. blendy
                // only ever samples this depth via HiZPass, which performs the same
                // transition as an SSR side effect; toyengine has no HiZPass, so
                // this is done explicitly instead. Not transitioned back afterward:
                // GBufferTarget's render pass declares DEPTH initialLayout =
                // UNDEFINED, so next frame's begin() doesn't care what layout this
                // is left in (see pipeline/render_pass.h).
                transition_gbuffer_depth_to_shader_read_(cmd);

                // Lighting and skybox draw into the SAME open render pass instance:
                // pipeline::RenderPass always uses LOAD_OP_CLEAR on its color
                // attachment (gfxcoopa/pipeline/render_pass.h), so a target can never
                // be re-opened later to composite onto -- the skybox pass's discard
                // of already-lit pixels only works because it runs as a second draw
                // inside offscreen_target_'s single begin()/end(), not a separate pass.
                offscreen_target_.begin(cmd);

                passes::PixelLightingPass::PushConstants lighting_pc;
                lighting_pc.light_bands    = config_.light_bands;
                lighting_pc.spec_threshold = config_.spec_threshold;
                lighting_pc.rim_strength   = config_.rim_strength;
                pixel_lighting_pass_->draw(cmd, *camera_set_, *light_set_, *shadow_set_, lighting_pc,
                                          render_extent_.width, render_extent_.height);

                skybox_pass_->draw(cmd, *camera_set_, view, proj, render_extent_.width, render_extent_.height);

                offscreen_target_.end(cmd);

                post_target_.begin(cmd);
                passes::PixelPostPass::PushConstants post_pc;
                post_pc.inv_render_size  = glm::vec2(1.0f / render_extent_.width, 1.0f / render_extent_.height);
                post_pc.exposure         = config_.exposure;
                post_pc.outline_thickness = config_.outline_enabled ? config_.outline_thickness : 0.0f;
                post_pc.outline_color    = config_.outline_color;
                post_pc.depth_threshold  = config_.depth_threshold;
                post_pc.normal_threshold = config_.normal_threshold;
                post_pc.dither_strength  = config_.dither_enabled ? config_.dither_strength : 0.0f;
                post_pc.palette_count    = config_.palette_enabled ? static_cast<float>(palette_lut_.count()) : 0.0f;
                pixel_post_pass_->draw(cmd, post_pc, render_extent_.width, render_extent_.height);
                post_target_.end(cmd);
            }
        );
    }

private:
    static VkDescriptorSetLayoutBinding make_ubo_binding_(uint32_t binding, VkShaderStageFlags stages) {
        VkDescriptorSetLayoutBinding b{};
        b.binding         = binding;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags      = stages;
        return b;
    }

    static VkDescriptorSetLayoutBinding make_sampler_binding_(uint32_t binding, VkShaderStageFlags stages) {
        VkDescriptorSetLayoutBinding b{};
        b.binding         = binding;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags      = stages;
        return b;
    }

    /**
     * @brief Populates LightUBO color/intensity/count fields (not the shadow
     *        matrix or cast_shadows flags -- see update_dir_shadow_matrix_ and
     *        render()'s point-light handling, both of which run after this
     *        and share the single light_data_.upload() at the end of render()).
     */
    void update_lights_(coopa::scene::Scene& scene) {
        using coopa::gfx::engine::components::DirectionalLightComponent;
        using coopa::gfx::engine::components::PointLightComponent;

        auto& ubo = light_data_.data();

        auto* dir = scene.find_first_component<DirectionalLightComponent>();
        if (dir) {
            ubo.dir_direction = glm::vec4(dir->direction, dir->intensity);
            ubo.dir_color     = glm::vec4(dir->color, 0.0f);
            ubo.dir_ambient   = glm::vec4(dir->ambient, 0.0f);
            ubo.light_counts.x = 1;
        } else {
            ubo.light_counts.x = 0;
        }

        auto points = scene.get_components<PointLightComponent>();
        uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(points.size()), 16);
        ubo.light_counts.y = count;
        for (uint32_t i = 0; i < count; ++i) {
            auto* pl = points[i];
            auto& gpu = ubo.point_lights[i];
            gpu.position_range  = glm::vec4(pl->get_world_position(), pl->range);
            gpu.color_intensity = glm::vec4(pl->color, pl->intensity);
            gpu.attenuation     = glm::vec4(pl->attenuation_constant, pl->attenuation_linear,
                                            pl->attenuation_quadratic, 0.0f); // cast_shadows set below, light 0 only
        }
    }

    /**
     * @brief Fits an orthographic light-space box to the AABB of every ready
     *        shadow caster, with texel-snapped centering to eliminate sub-texel
     *        shadow crawl -- ported from blendy's PbrRenderPipeline::update_scene_data_
     *        (see blendy/src/blendy/render/pbr_render_pipeline.h, "Directional
     *        shadow-map projection" section). Writes light_data_.data()'s
     *        dir_light_space_matrix and dir_shadow_params (upload() happens once
     *        in render() after this and the point-light shadow flag are both set).
     */
    void update_dir_shadow_matrix_(const glm::vec3& direction,
                                   const std::vector<coopa::gfx::engine::components::MeshRenderer*>& renderers,
                                   const std::vector<glm::mat4>& world_matrices,
                                   const coopa::gfx::engine::components::CameraComponent* cam,
                                   bool cast_dir_shadow) {
        glm::vec3 light_dir = glm::normalize(direction);
        glm::vec3 up = (std::abs(light_dir.z) < 0.99f) ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        glm::mat4 light_rot = glm::lookAt(glm::vec3(0.0f), light_dir, up);

        glm::vec3 aabb_min(std::numeric_limits<float>::max());
        glm::vec3 aabb_max(std::numeric_limits<float>::lowest());
        bool any_caster = false;

        for (size_t i = 0; i < renderers.size(); ++i) {
            if (!renderers[i]->is_ready()) continue;
            const auto* mesh = renderers[i]->get_mesh().get();
            if (!mesh) continue;
            any_caster = true;

            const glm::vec3& bmin = mesh->bounds_min();
            const glm::vec3& bmax = mesh->bounds_max();
            for (int c = 0; c < 8; ++c) {
                glm::vec3 corner((c & 1) ? bmax.x : bmin.x, (c & 2) ? bmax.y : bmin.y, (c & 4) ? bmax.z : bmin.z);
                glm::vec3 ls = glm::vec3(light_rot * (world_matrices[i] * glm::vec4(corner, 1.0f)));
                aabb_min = glm::min(aabb_min, ls);
                aabb_max = glm::max(aabb_max, ls);
            }
        }

        glm::mat4 light_proj;
        if (any_caster) {
            const float pad = 1.0f;
            float half_w = 0.5f * (aabb_max.x - aabb_min.x) + pad;
            float half_h = 0.5f * (aabb_max.y - aabb_min.y) + pad;
            float extent = std::max(half_w, half_h);
            const float extent_step = 0.5f;
            extent = std::ceil(extent / extent_step) * extent_step;
            if (config_.shadow_max_extent > 0.0f) extent = std::min(extent, config_.shadow_max_extent);

            glm::vec2 center(0.5f * (aabb_min.x + aabb_max.x), 0.5f * (aabb_min.y + aabb_max.y));
            float texel = (2.0f * extent) / static_cast<float>(config_.shadow_map_resolution);
            center.x = std::floor(center.x / texel) * texel;
            center.y = std::floor(center.y / texel) * texel;

            float near_plane = -aabb_max.z - pad;
            float far_plane  = -aabb_min.z + pad;
            if (far_plane - near_plane < 0.01f) far_plane = near_plane + 0.01f;

            light_proj = glm::orthoRH_ZO(center.x - extent, center.x + extent,
                                        center.y - extent, center.y + extent,
                                        near_plane, far_plane);
        } else {
            glm::vec3 center = cam ? cam->get_world_position() : glm::vec3(0.0f);
            glm::vec3 center_ls = glm::vec3(light_rot * glm::vec4(center, 1.0f));
            float ortho_extent = 15.0f;
            light_proj = glm::orthoRH_ZO(center_ls.x - ortho_extent, center_ls.x + ortho_extent,
                                        center_ls.y - ortho_extent, center_ls.y + ortho_extent,
                                        0.1f, 60.0f);
        }
        light_proj[1][1] *= -1.0f; // Vulkan Y-flip

        auto& ubo = light_data_.data();
        ubo.dir_light_space_matrix = light_proj * light_rot;
        ubo.dir_shadow_params      = glm::vec4(config_.shadow_bias, 0.0f, cast_dir_shadow ? 1.0f : 0.0f, 0.05f);
    }

    /** @brief The first PointLightComponent with cast_shadows set, or nullptr. */
    coopa::gfx::engine::components::PointLightComponent* find_first_shadow_casting_point_light_(coopa::scene::Scene& scene) {
        for (auto* pl : scene.get_components<coopa::gfx::engine::components::PointLightComponent>()) {
            if (pl->cast_shadows) return pl;
        }
        return nullptr;
    }

    void record_directional_shadow_(coopa::gfx::command::CommandBuffer& cmd,
                                    const std::vector<coopa::gfx::engine::components::MeshRenderer*>& renderers,
                                    const std::vector<uint32_t>& instance_idx,
                                    bool cast_dir_shadow) {
        shadow_target_.begin_directional_pass(cmd);
        if (cast_dir_shadow) {
            shadow_pipeline_->bind_directional(cmd);
            cmd.bind_vertex_buffer(instance_stream_.buffer(), 0, 1);

            coopa::gfx::engine::passes::DirectionalShadowPushConstants pc{};
            pc.light_space_matrix = light_data_.data().dir_light_space_matrix;
            for (size_t i = 0; i < renderers.size(); ++i) {
                if (instance_idx[i] == UINT32_MAX) continue;
                shadow_pipeline_->push_directional(cmd, pc);
                renderers[i]->get_mesh()->bind(cmd);
                renderers[i]->get_mesh()->draw(cmd, 1, instance_idx[i]);
            }
        }
        shadow_target_.end_directional_pass(cmd);
        shadow_target_.transition_dir_to_shader_read(cmd);
    }

    void record_point_shadow_(coopa::gfx::command::CommandBuffer& cmd,
                              const std::vector<coopa::gfx::engine::components::MeshRenderer*>& renderers,
                              const std::vector<uint32_t>& instance_idx,
                              coopa::gfx::engine::components::PointLightComponent* shadow_point,
                              bool cast_point_shadow) {
        if (!cast_point_shadow) {
            shadow_target_.transition_cube_to_shader_read(cmd);
            return;
        }

        glm::vec3 light_pos = shadow_point->get_world_position();
        float range = shadow_point->range;

        for (uint32_t face = 0; face < 6; ++face) {
            shadow_target_.begin_cube_face_pass(cmd, face);
            shadow_pipeline_->bind_cube(cmd);
            cmd.bind_vertex_buffer(instance_stream_.buffer(), 0, 1);

            coopa::gfx::engine::passes::CubeShadowPushConstants pc{};
            pc.light_space_matrix = coopa::gfx::engine::targets::ShadowMapTarget::get_cube_face_matrix(face, light_pos, range);
            pc.light_pos_range    = glm::vec4(light_pos, range);

            for (size_t i = 0; i < renderers.size(); ++i) {
                if (instance_idx[i] == UINT32_MAX) continue;
                shadow_pipeline_->push_cube(cmd, pc);
                renderers[i]->get_mesh()->bind(cmd);
                renderers[i]->get_mesh()->draw(cmd, 1, instance_idx[i]);
            }
            shadow_target_.end_cube_face_pass(cmd);
        }
    }

    void transition_gbuffer_depth_to_shader_read_(coopa::gfx::command::CommandBuffer& cmd) {
        VkImageMemoryBarrier barrier{};
        barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout                       = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barrier.newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                           = gbuffer_target_.depth_image_handle();
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;
        barrier.srcAccessMask                   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask                   = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd.handle(),
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void record_gbuffer_(coopa::gfx::command::CommandBuffer& cmd,
                         const std::vector<coopa::gfx::engine::components::MeshRenderer*>& renderers,
                         const std::vector<uint32_t>& instance_idx) {
        gbuffer_target_.begin(cmd);
        gbuffer_pipeline_->bind(cmd);
        cmd.bind_descriptor_set(gbuffer_pipeline_->layout(), *camera_set_, 0);
        cmd.bind_vertex_buffer(instance_stream_.buffer(), 0, 1);

        for (size_t i = 0; i < renderers.size(); ++i) {
            if (instance_idx[i] == UINT32_MAX) continue;
            auto* mr = renderers[i];

            coopa::gfx::engine::passes::GBufferPipeline::PushConstants pc;
            pc.albedo       = glm::vec4(mr->material.albedo, mr->material.alpha);
            pc.metallic     = mr->material.metallic;
            pc.roughness    = mr->material.roughness;
            pc.ao           = mr->material.ao;
            pc.alpha_cutoff = mr->material.gpu_alpha_cutoff();
            gbuffer_pipeline_->push(cmd, pc);

            mr->get_mesh()->bind(cmd);
            mr->get_mesh()->draw(cmd, 1, instance_idx[i]);
        }

        gbuffer_target_.end(cmd);
    }

    coopa::gfx::core::Device&      device_;
    coopa::gfx::memory::Allocator& allocator_;
    coopa::gfx::core::Swapchain&   swapchain_;
    PixelRenderConfig              config_;
    RenderExtent                   render_extent_;

    coopa::gfx::engine::targets::GBufferTarget   gbuffer_target_;
    coopa::gfx::engine::targets::OffscreenTarget offscreen_target_; // lit + sky, pre-post
    coopa::gfx::engine::targets::OffscreenTarget post_target_;      // final low-res LDR, post PixelPostPass
    coopa::gfx::engine::util::Sampler            nearest_sampler_;
    coopa::gfx::engine::util::Sampler            linear_sampler_;
    coopa::gfx::engine::util::Sampler            shadow_sampler_;

    coopa::gfx::engine::data::CameraUBO camera_ubo_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> camera_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>      camera_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>       camera_set_;

    coopa::gfx::engine::data::LightData light_data_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> light_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>      light_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>       light_set_;

    coopa::gfx::engine::targets::ShadowMapTarget shadow_target_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout>   shadow_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>        shadow_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>         shadow_set_;
    std::unique_ptr<coopa::gfx::engine::passes::ShadowPipeline>  shadow_pipeline_;

    PaletteLut palette_lut_;

    std::unique_ptr<coopa::gfx::engine::passes::GBufferPipeline> gbuffer_pipeline_;
    std::unique_ptr<passes::GBufferVisualizePass>                gbuffer_visualize_pass_;
    std::unique_ptr<passes::PixelLightingPass>                   pixel_lighting_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::SkyboxPass>      skybox_pass_;
    std::unique_ptr<passes::PixelPostPass>                       pixel_post_pass_;
    std::unique_ptr<passes::UpscalePass>                         upscale_pass_;

    InstanceStream instance_stream_;
    bool           warned_perspective_snap_ = false;
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PIXEL_RENDER_PIPELINE_H
