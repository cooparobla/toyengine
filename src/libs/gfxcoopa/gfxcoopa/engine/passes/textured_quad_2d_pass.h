/**
 * @file textured_quad_2d_pass.h
 * @brief Shared 2D textured-quad pass: descriptor layout/pool, the TextureView -> descriptor
 * set cache, per-frame-in-flight streaming geometry buffers, and a named-pipeline-variant
 * map -- the parts uicoopa's UiPass and pixengine's SpritePass have in common.
 *
 * What stays OUT of this class, because it genuinely differs per consumer:
 *  - Viewport/scissor policy (UiPass: full framebuffer, per-batch clip-derived scissor.
 *    SpritePass: a caller-supplied best-fit sub-rect, one scissor per frame).
 *  - The push-constant struct's own shape and when it's pushed (per-batch vs per-frame).
 *  - Batching (DrawList vs SpriteDrawList: clip/z-order stack vs none) -- this class only
 *    ever sees "a TextureView and an index range", not how those groupings were formed.
 *
 * Every consumer's vertex struct is layout-identical in spirit (position, uv, color) but
 * physically distinct types (UiVertex, SpriteVertex) with their own VertexLayout/pack_color
 * conventions -- this class stores vertex STRIDE (bytes), not a type, so it never needs to
 * know either concrete struct.
 *
 * Reuses the exact add_variant()/bind(name) pipeline-variant shape already proven on the 3D
 * side (GBufferPipeline, TransparentPass, ...): a variant shares this pass's descriptor
 * layout and push-constant range, differing only in which two shader modules are bound --
 * see gfx/surface2d/quad_vs.glsl for the shared vertex backbone this pairs with.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_TEXTURED_QUAD_2D_PASS_H
#define GFXCOOPA_ENGINE_PASSES_TEXTURED_QUAD_2D_PASS_H

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/passes/extra_sets.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/engine/data/texture.h>
#include <gfxcoopa/presentation/renderer.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/sampler_desc.h>
#include <gfxcoopa/types/texture_view.h>
#include <gfxcoopa/types/vertex_layout.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/**
 * @struct TexturedQuad2DDesc
 * @brief Everything a caller must supply that this pass can't infer -- its own vertex
 *        layout/stride, blend convention, push-constant size, and sampler/fallback pixel.
 */
struct TexturedQuad2DDesc {
    coopa::gfx::VertexLayout vertex;      ///< Caller's vertex struct layout (position/uv/color).
    size_t   vertex_stride      = 0;      ///< sizeof(the caller's vertex struct); required.
    coopa::gfx::pipeline::BlendMode blend_mode = coopa::gfx::pipeline::BlendMode::Alpha;
    uint32_t push_constant_size = 0;      ///< sizeof(the caller's own push-constant struct); required.
    coopa::gfx::SamplerDesc sampler_desc = coopa::gfx::SamplerDesc::linear_repeat();
    /// The single pixel bound in place of any texture never registered before a draw --
    /// loud on purpose (a caller bug shows up as a solid-colored quad, not a crash). White
    /// for UI (an unregistered texture reads as "no image"); pixengine uses opaque magenta
    /// so a missing atlas is unmistakable against pixel art.
    std::array<uint8_t, 4> fallback_pixel = {255, 255, 255, 255};
    uint32_t initial_max_verts   = 4096;
    uint32_t initial_max_indices = 6144;
    uint32_t max_textures        = 256;
    /**
     * Optional app-supplied descriptor sets, appended after this pass's own texture set
     * (set 0) -- e.g. uicoopa's UiWorldPass taking a scene depth texture at set 1 so it
     * can discard world-space UI fragments behind opaque geometry. Default-constructed
     * ExtraSets is empty(), in which case no extra set is declared in the pipeline
     * layout at all.
     *
     * Appended to EVERY pipeline this pass builds -- the stock one and every
     * add_variant() -- deliberately, not per variant: draw() implementations here push
     * their constants ONCE and then rebind pipelines per batch, which is only valid while
     * all variants stay layout-compatible. A per-variant set would silently invalidate
     * push constants on the first variant switch.
     */
    ExtraSets extra;
};

/**
 * @class TexturedQuad2DPass
 * @brief The shared machinery behind a 2D textured-quad pass -- see file doc for what's
 *        deliberately left to the caller.
 */
class TexturedQuad2DPass {
public:
    static constexpr uint32_t kFrames = coopa::gfx::presentation::MAX_FRAMES_IN_FLIGHT;

    /**
     * @param device       Logical device.
     * @param allocator    VMA allocator.
     * @param cmd_pool     Command pool for the fallback texture's one-shot upload.
     * @param target_pass  The render pass this pass draws into -- a guest in someone else's
     *                     pass (UI/sprites composite into an already-open swapchain pass;
     *                     this class never clears or owns a render pass of its own).
     * @param vert_spv     Stock vertex shader (resolved .spv path).
     * @param frag_spv     Stock fragment shader (resolved .spv path).
     * @param desc         Everything else -- see TexturedQuad2DDesc.
     */
    TexturedQuad2DPass(coopa::gfx::core::Device& device,
                       coopa::gfx::memory::Allocator& allocator,
                       coopa::gfx::command::CommandPool& cmd_pool,
                       coopa::gfx::pipeline::RenderPass& target_pass,
                       const std::string& vert_spv,
                       const std::string& frag_spv,
                       TexturedQuad2DDesc desc)
        : device_(device), allocator_(&allocator), target_pass_(target_pass), desc_(std::move(desc))
    {
        if (desc_.vertex_stride == 0 || desc_.push_constant_size == 0) {
            throw std::runtime_error("[gfxcoopa] TexturedQuad2DPass: vertex_stride and "
                                     "push_constant_size are required");
        }
        desc_.extra.validate("TexturedQuad2DPass");

        desc_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .combined_sampler(0, coopa::gfx::ShaderStage::Fragment)
                .build(device));

        desc_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            coopa::gfx::pipeline::DescriptorPoolBuilder().add_sets(*desc_layout_, desc_.max_textures).build(device));

        vert_shader_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, vert_spv, coopa::gfx::ShaderStage::Vertex);
        frag_shader_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, frag_spv, coopa::gfx::ShaderStage::Fragment);
        pipeline_ = create_pipeline_(*vert_shader_, *frag_shader_);

        sampler_ = std::make_unique<coopa::gfx::engine::util::Sampler>(device, desc_.sampler_desc);

        fallback_texture_ = std::make_unique<coopa::gfx::engine::data::Texture>(
            coopa::gfx::engine::data::Texture::upload(
                device, allocator, cmd_pool, desc_.fallback_pixel.data(), 1, 1,
                coopa::gfx::Format::RGBA8_Unorm, desc_.sampler_desc));
        register_view_(fallback_texture_->view_typed());

        for (uint32_t i = 0; i < kFrames; ++i) {
            vbo_[i] = std::make_unique<coopa::gfx::memory::Buffer>(
                coopa::gfx::memory::Buffer::vertex(device, allocator, desc_.initial_max_verts * desc_.vertex_stride));
            ibo_[i] = std::make_unique<coopa::gfx::memory::Buffer>(
                coopa::gfx::memory::Buffer::index(device, allocator, desc_.initial_max_indices * sizeof(uint32_t)));
            vbo_capacity_[i] = desc_.initial_max_verts;
            ibo_capacity_[i] = desc_.initial_max_indices;
        }
    }

    /**
     * @brief Registers a derived shader's own vertex/fragment pair as a named variant,
     *        reusing this pass's descriptor layout and push-constant range -- e.g. uicoopa's
     *        "text" variant (R8 coverage sampling) alongside the stock RGBA quad shader.
     */
    void add_variant(const std::string& name, const std::string& vert_spv, const std::string& frag_spv) {
        Variant v;
        v.vert_shader = std::make_unique<coopa::gfx::pipeline::Shader>(device_, vert_spv, coopa::gfx::ShaderStage::Vertex);
        v.frag_shader = std::make_unique<coopa::gfx::pipeline::Shader>(device_, frag_spv, coopa::gfx::ShaderStage::Fragment);
        v.pipeline    = create_pipeline_(*v.vert_shader, *v.frag_shader);
        variants_.emplace(name, std::move(v));
    }

    /** @brief True if a variant named `name` was registered via add_variant(). */
    bool has_variant(const std::string& name) const { return variants_.find(name) != variants_.end(); }

    /** @brief Binds the stock pipeline. */
    void bind(coopa::gfx::command::CommandBuffer& cmd) const { cmd.bind_pipeline(*pipeline_); }

    /** @brief Binds a named variant's pipeline, or the stock pipeline if `name` is empty or unregistered. */
    void bind(coopa::gfx::command::CommandBuffer& cmd, const std::string& name) const {
        auto it = variants_.find(name);
        cmd.bind_pipeline(it != variants_.end() ? *it->second.pipeline : *pipeline_);
    }

    /**
     * @brief Binds the caller's ExtraSets (if any), starting at set index 1 -- immediately
     *        after this pass's own texture set.
     *
     * No-op when desc.extra was left empty. Must be called AFTER bind()/bind(name), which
     * is what caches the pipeline layout on the CommandBuffer the sealed
     * bind_descriptor_set() overload needs -- see extra_sets.h.
     */
    void bind_extra(coopa::gfx::command::CommandBuffer& cmd) const {
        if (desc_.extra.bind) desc_.extra.bind(cmd, 1u);
    }

    /** @brief The 1x1 fallback texture's view -- callers seed their draw-list's default texture with this. */
    coopa::gfx::TextureView fallback_view() const { return fallback_texture_->view_typed(); }

    /**
     * @brief Resolves `view` into a cached descriptor set, allocating (and immediately
     *        writing) one the first time this view is seen.
     *
     * Must be called before the render pass this pass draws into begins -- bind_image()
     * updates the descriptor set immediately, which is unsafe once recording has started.
     * Already-registered views are skipped, so calling every frame is cheap.
     */
    void register_view(coopa::gfx::TextureView view) { register_view_(view); }

    /**
     * @brief Forgets `view`'s cached descriptor set, so a later view that lands on the same
     *        handle value gets a freshly bound one.
     *
     * Not an optimisation -- omitting it is a correctness bug. The cache is keyed by
     * TextureView, which is the raw VkImageView handle value (see detail/vk_convert.h's
     * wrap()), and register_view_() early-returns on a key hit. Vulkan drivers routinely
     * hand a freed handle straight back to the next vkCreateImageView, so a consumer that
     * destroys one texture and creates another gets a new view that compares EQUAL to the
     * dead one, skips registration, and is then drawn through a descriptor set still
     * describing the destroyed image.
     *
     * It fails quietly, which is the worst part: descriptor_set_for() only falls back to
     * the fallback texture when a key is ABSENT, so a stale key produces a plausible-looking
     * wrong image rather than an obvious white quad or a crash.
     *
     * Eviction also bounds the cache. Without it every texture a consumer ever creates
     * permanently consumes one of desc_.max_textures descriptor sets, and a long-running
     * app that swaps textures eventually fails to allocate one at all.
     *
     * Call before destroying the texture that owns `view`, and only once no in-flight frame
     * still references it -- wait on the device, or on that frame's fence.
     *
     * @param view The view to forget. An unknown view is a no-op.
     */
    void unregister_view(coopa::gfx::TextureView view) { descriptor_cache_.erase(view); }

    /**
     * @brief Looks up the descriptor set for `view`, falling back to the fallback texture's
     *        set if it was never registered (a caller bug shows up as the fallback pixel,
     *        never a crash).
     */
    const coopa::gfx::pipeline::DescriptorSet& descriptor_set_for(coopa::gfx::TextureView view) const {
        auto it = descriptor_cache_.find(view);
        if (it == descriptor_cache_.end()) {
            it = descriptor_cache_.find(fallback_texture_->view_typed());
        }
        return *it->second;
    }

    /**
     * @brief Grows this frame-slot's vertex/index buffers if `needed_verts`/`needed_indices`
     *        exceed current capacity, doubling (or matching the need, if larger) -- callers
     *        always re-upload the full stream every frame, so growth never needs to preserve
     *        existing contents.
     */
    void ensure_capacity(uint32_t frame_index, size_t needed_verts, size_t needed_indices) {
        if (needed_verts > vbo_capacity_[frame_index]) {
            size_t new_capacity = std::max(needed_verts, static_cast<size_t>(vbo_capacity_[frame_index]) * 2);
            vbo_[frame_index].reset(); // must be destroyed before the allocator creates the replacement
            vbo_[frame_index] = std::make_unique<coopa::gfx::memory::Buffer>(
                coopa::gfx::memory::Buffer::vertex(device_, allocator_ref_(), new_capacity * desc_.vertex_stride));
            vbo_capacity_[frame_index] = static_cast<uint32_t>(new_capacity);
        }
        if (needed_indices > ibo_capacity_[frame_index]) {
            size_t new_capacity = std::max(needed_indices, static_cast<size_t>(ibo_capacity_[frame_index]) * 2);
            ibo_[frame_index].reset();
            ibo_[frame_index] = std::make_unique<coopa::gfx::memory::Buffer>(
                coopa::gfx::memory::Buffer::index(device_, allocator_ref_(), new_capacity * sizeof(uint32_t)));
            ibo_capacity_[frame_index] = static_cast<uint32_t>(new_capacity);
        }
    }

    /** @brief This frame-slot's vertex buffer -- upload into it, then bind_vertex_buffer(). */
    coopa::gfx::memory::Buffer& vertex_buffer(uint32_t frame_index) const { return *vbo_[frame_index]; }

    /** @brief This frame-slot's index buffer -- upload into it, then bind_index_buffer(). */
    coopa::gfx::memory::Buffer& index_buffer(uint32_t frame_index) const { return *ibo_[frame_index]; }

private:
    struct Variant {
        std::unique_ptr<coopa::gfx::pipeline::Shader> vert_shader;
        std::unique_ptr<coopa::gfx::pipeline::Shader> frag_shader;
        std::unique_ptr<coopa::gfx::pipeline::Pipeline> pipeline;
    };

    /// Builds one Pipeline against this pass's shared descriptor layout, vertex layout,
    /// push-constant size, and blend/depth/raster state -- the stock pipeline and every
    /// add_variant() call route through here so the only thing that can differ is the two
    /// shader modules.
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> create_pipeline_(
        coopa::gfx::pipeline::Shader& vert, coopa::gfx::pipeline::Shader& frag) {
        coopa::gfx::pipeline::PipelineDesc pd;
        pd.shaders = {&vert, &frag};
        pd.vertex  = desc_.vertex;
        pd.descriptor_layouts = {desc_layout_.get()};
        for (const coopa::gfx::pipeline::DescriptorSetLayout* extra : desc_.extra.layouts) {
            pd.descriptor_layouts.push_back(extra);
        }
        pd.push_constants = {{coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment,
                              0, desc_.push_constant_size}};
        pd.raster.cull  = coopa::gfx::CullMode::None;
        pd.depth.test   = false;
        pd.depth.write  = false;
        pd.blend.mode   = desc_.blend_mode;
        return std::make_unique<coopa::gfx::pipeline::Pipeline>(device_, target_pass_, pd);
    }

    void register_view_(coopa::gfx::TextureView view) {
        if (descriptor_cache_.count(view)) return;
        auto set = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device_, *desc_pool_, *desc_layout_);
        set->bind_image(0, view, *sampler_);
        descriptor_cache_[view] = std::move(set);
    }

    coopa::gfx::memory::Allocator& allocator_ref_() {
        if (!allocator_) {
            throw std::runtime_error("[gfxcoopa] TexturedQuad2DPass geometry buffer grew before an Allocator was captured.");
        }
        return *allocator_;
    }

    coopa::gfx::core::Device&      device_;
    coopa::gfx::memory::Allocator* allocator_;
    coopa::gfx::pipeline::RenderPass& target_pass_;
    TexturedQuad2DDesc desc_;

    std::unique_ptr<coopa::gfx::pipeline::Shader>              vert_shader_;
    std::unique_ptr<coopa::gfx::pipeline::Shader>              frag_shader_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> desc_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>      desc_pool_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline>            pipeline_;
    std::unique_ptr<coopa::gfx::engine::util::Sampler>         sampler_;
    std::unique_ptr<coopa::gfx::engine::data::Texture>         fallback_texture_;

    std::map<std::string, Variant> variants_;

    std::unordered_map<coopa::gfx::TextureView, std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>> descriptor_cache_;

    std::unique_ptr<coopa::gfx::memory::Buffer> vbo_[kFrames];
    std::unique_ptr<coopa::gfx::memory::Buffer> ibo_[kFrames];
    uint32_t vbo_capacity_[kFrames] = {};
    uint32_t ibo_capacity_[kFrames] = {};
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_TEXTURED_QUAD_2D_PASS_H
