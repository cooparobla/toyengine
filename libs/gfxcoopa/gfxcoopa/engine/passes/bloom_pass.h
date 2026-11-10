/**
 * @file bloom_pass.h
 * @brief Additive HDR bloom: bright-pass threshold -> progressive multi-tap
 *        downsample -> progressive tent-filter upsample+combine.
 *
 * Storage is a chain of owned OffscreenTargets, each stage reading the previous
 * one's output, with no manual barriers (see execute()'s doc) -- not a single
 * mip-mapped image with per-mip views, framebuffers and barriers the way
 * SceneColorMipPass does it. Bloom's consumer (StylizePass) reads exactly
 * ONE final image at one resolution, so the mip-mapped layout's advantage (one
 * allocation, one maxLod sampler) buys nothing, and a dual-filter pyramid needs
 * a parallel "up" chain either way (see up_targets_'s own doc).
 *
 * Every descriptor this pass owns is bound once, at construction (see
 * bind_static_descriptors_()) -- there is no per-frame update_descriptors() the
 * way SceneColorMipPass/HiZPass have, and therefore (unlike those two) this pass
 * needs no per-frame device_.wait_idle() to stay valid under this pipeline's
 * overlapped-frame-in-flight model.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_BLOOM_PASS_H
#define GFXCOOPA_ENGINE_PASSES_BLOOM_PASS_H

#include <volk/volk.h>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/engine/passes/fullscreen_stage.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/types/texture_view.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/**
 * @class BloomPass
 * @brief Builds an additive HDR bloom image from a scene colour target.
 *
 * Owns the whole pyramid: one OffscreenTarget per level in each direction, and
 * three pipelines (prefilter, downsample, upsample). The caller supplies the
 * source HDR view once at construction or via recreate(), runs execute() inside
 * its frame, and samples result_view_typed() afterwards.
 */
class BloomPass {
public:
    /// Unity's default is 6 iterations from a half-resolution start. At this
    /// engine's 720x480 internal resolution the half-res base is 360x240 and 6
    /// levels bottoms out at 11x7 -- a screen-wide glow. A 7th level would be 5x3,
    /// where the 13-tap kernel's own footprint (4x4 source texels) exceeds the
    /// image and every tap clamps to the same handful of edge texels: cost with
    /// no added spread. recreate()'s loop stops early on smaller render targets.
    static constexpr uint32_t kMaxLevels = 6;

    /// Per-frame tunables. All are push constants, so unlike this pass's
    /// descriptor bindings (fixed at construction -- see bind_static_descriptors_)
    /// these are free to change every frame with no rebuild.
    struct Params {
        float threshold = 1.0f;   ///< Brightness (max3 of RGB) below which nothing glows.
        float soft_knee = 0.5f;   ///< 0..1 fraction of `threshold` the quadratic knee spans; 0 = hard cutoff.
        float clamp_max = 20.0f;  ///< Per-tap ceiling applied before thresholding (firefly guard).
        float radius    = 1.0f;   ///< Tent width multiplier on the upsample; > 1 widens the halo.
        float scatter   = 0.7f;   ///< Per-level blend toward the coarser tent; Unity URP's Bloom "Scatter".
    };

    struct PrefilterPush {
        glm::vec2 src_texel;      // offset  0, 8 bytes
        float     threshold;      // offset  8
        float     soft_knee;      // offset 12
        float     clamp_max;      // offset 16
    };
    static_assert(sizeof(PrefilterPush) == 20,
                 "PrefilterPush must match bloom_prefilter.frag's BloomPrefilterPush byte-for-byte");

    struct DownsamplePush {
        glm::vec2 src_texel;      // offset 0, 8 bytes
    };
    static_assert(sizeof(DownsamplePush) == 8,
                 "DownsamplePush must match bloom_downsample.frag's BloomDownsamplePush byte-for-byte");

    struct UpsamplePush {
        glm::vec2 low_texel;      // offset  0, 8 bytes
        float     radius;         // offset  8
        float     scatter;        // offset 12
    };
    static_assert(sizeof(UpsamplePush) == 16,
                 "UpsamplePush must match bloom_upsample.frag's BloomUpsamplePush byte-for-byte");

    BloomPass(coopa::gfx::core::Device& device,
              coopa::gfx::memory::Allocator& allocator,
              uint32_t full_width, uint32_t full_height,
              coopa::gfx::TextureView source_hdr,
              const coopa::gfx::engine::util::Sampler& linear_sampler,
              const std::string& vert_spv,
              const std::string& prefilter_frag_spv,
              const std::string& downsample_frag_spv,
              const std::string& upsample_frag_spv)
        : device_(device), allocator_(allocator),
          vert_spv_(vert_spv), prefilter_frag_spv_(prefilter_frag_spv),
          downsample_frag_spv_(downsample_frag_spv), upsample_frag_spv_(upsample_frag_spv)
    {

        recreate(full_width, full_height, source_hdr, linear_sampler);
    }

    BloomPass(const BloomPass&) = delete;
    BloomPass& operator=(const BloomPass&) = delete;

    /// @brief Rebuilds the whole pyramid at a new resolution and/or source image.
    ///
    /// Destroys and recreates every owned target, pipeline and descriptor set --
    /// safe to call between frames (matches OffscreenTarget::recreate()'s own
    /// contract: after a device_.wait_idle(), not concurrently with an in-flight
    /// command buffer touching this pass's resources).
    void recreate(uint32_t full_width, uint32_t full_height,
                  coopa::gfx::TextureView source_hdr,
                  const coopa::gfx::engine::util::Sampler& linear_sampler) {
        full_width_  = full_width;
        full_height_ = full_height;
        base_width_  = std::max(1u, full_width_  / 2u);
        base_height_ = std::max(1u, full_height_ / 2u);

        // Stop before any level's smaller dimension drops under 4 texels: the 13-tap
        // kernel's own footprint is 4x4 source texels, so below that every tap
        // clamps to the same handful of edge texels and the level adds a render
        // pass for no additional spread. At 360x240 this yields exactly kMaxLevels
        // (the smallest level is 11x7); a much smaller render target degrades
        // gracefully to fewer levels instead of asserting or clamping garbage.
        level_count_ = 1;
        while (level_count_ < kMaxLevels &&
               (base_width_  >> level_count_) >= 4u &&
               (base_height_ >> level_count_) >= 4u) {
            ++level_count_;
        }

        down_targets_.clear();
        up_targets_.clear();
        prefilter_.reset();
        downsample_.reset();
        upsample_.reset();

        // down_targets_[0] is the prefilter output at (base_width_, base_height_);
        // down_targets_[i] is that halved i times.
        down_targets_.reserve(level_count_);
        for (uint32_t i = 0; i < level_count_; ++i) {
            uint32_t w = std::max(1u, base_width_  >> i);
            uint32_t h = std::max(1u, base_height_ >> i);
            down_targets_.push_back(std::make_unique<targets::OffscreenTarget>(
                device_, allocator_, w, h, coopa::gfx::Format::RGBA16_Sfloat, targets::kColorOnly));
        }

        // up_targets_[i] is the same size as down_targets_[i], for i in
        // [0, level_count_ - 1). A parallel chain is mandatory, not an
        // optimisation opportunity: the upsample at level i READS down_targets_[i]
        // (as its same-resolution "high" input) and pipeline::RenderPass hardcodes
        // LOAD_OP_CLEAR, so that combined result cannot be written back into
        // down_targets_[i] -- it needs a target of its own.
        if (level_count_ > 1) {
            up_targets_.reserve(level_count_ - 1);
            for (uint32_t i = 0; i + 1 < level_count_; ++i) {
                up_targets_.push_back(std::make_unique<targets::OffscreenTarget>(
                    device_, allocator_, down_targets_[i]->width(), down_targets_[i]->height(),
                    coopa::gfx::Format::RGBA16_Sfloat, targets::kColorOnly));
            }
        }

        // All bloom targets share identical format/depth-format/final-layouts/
        // sample-count, so their render passes are Vulkan render-pass-compatible
        // regardless of resolution (resolution is not part of the compatibility
        // rule, and Pipeline always enables dynamic viewport/scissor -- see
        // pipeline.h). One Pipeline per stage type therefore suffices; only the
        // framebuffer/viewport changes per level in execute()'s loop.
        // One FullscreenStage per stage, rebuilt here rather than in the constructor
        // because the chain length -- and so the number of descriptor sets each stage
        // needs -- follows the render resolution.
        //
        // Blend is left at BlendMode::None: the upsample combine happens in the fragment
        // shader (two sampler reads summed via mix()), not in hardware -- see
        // bloom_upsample.frag's own doc for why BlendMode::Additive cannot help under
        // LOAD_OP_CLEAR.
        using coopa::gfx::DescriptorType;
        using coopa::gfx::ShaderStage;

        auto stage_desc = [&](const std::string& frag, uint32_t sampled, uint32_t push_size,
                              uint32_t instances) {
            FullscreenStageDesc d;
            d.vert_spv = vert_spv_;
            d.frag_spv = frag;
            d.owned_sets.emplace_back();
            for (uint32_t i = 0; i < sampled; ++i) {
                d.owned_sets[0].push_back({i, DescriptorType::CombinedImageSampler,
                                           ShaderStage::Fragment, 1});
            }
            d.push_constants = {{ShaderStage::Fragment, 0, push_size}};
            d.instances = instances;
            return d;
        };

        prefilter_ = std::make_unique<FullscreenStage>(
            device_, down_targets_[0]->render_pass_object(),
            stage_desc(prefilter_frag_spv_, 1, sizeof(PrefilterPush), 1));

        // One set per level. Index 0 is allocated but never bound, so set indices line
        // up 1:1 with down_targets_.
        downsample_ = std::make_unique<FullscreenStage>(
            device_, down_targets_[0]->render_pass_object(),
            stage_desc(downsample_frag_spv_, 1, sizeof(DownsamplePush), level_count_));

        if (!up_targets_.empty()) {
            upsample_ = std::make_unique<FullscreenStage>(
                device_, up_targets_[0]->render_pass_object(),
                stage_desc(upsample_frag_spv_, 2, sizeof(UpsamplePush),
                           static_cast<uint32_t>(up_targets_.size())));
        }

        bind_static_descriptors_(source_hdr, linear_sampler);
    }

    /// @brief Runs the full pyramid: prefilter -> progressive downsample -> progressive
    /// tent upsample+combine. Leaves the finished result in result_view_typed().
    ///
    /// No manual VkImageMemoryBarrier anywhere in this method, and none is missing.
    /// Every target here is an OffscreenTarget, whose pipeline::RenderPass is built
    /// with color_final_layout = SHADER_READ_ONLY_OPTIMAL, which makes
    /// pipeline::RenderPass emit an EXIT subpass dependency (COLOR_ATTACHMENT_OUTPUT /
    /// COLOR_ATTACHMENT_WRITE -> FRAGMENT_SHADER / SHADER_READ; see render_pass.h's
    /// needs_exit_dependency block) -- precisely the write-then-read hazard between
    /// consecutive pyramid stages. SmaaPass chains three OffscreenTargets the same
    /// way with no manual barriers; this pass follows that precedent rather than
    /// SceneColorMipPass's explicit per-mip barrier, which restates that same
    /// exit dependency.
    void execute(coopa::gfx::command::CommandBuffer& cmd, const Params& params) {
        static constexpr VkClearColorValue kBlack{{0.0f, 0.0f, 0.0f, 1.0f}};

        // --- Stage 1: bright-pass + first halving, full-res HDR -> down_targets_[0] ---
        begin_stage_(cmd, *down_targets_[0], kBlack, *prefilter_, 0);
        PrefilterPush pf{};
        pf.src_texel = glm::vec2(1.0f / static_cast<float>(full_width_), 1.0f / static_cast<float>(full_height_));
        pf.threshold = params.threshold;
        pf.soft_knee = params.soft_knee;
        pf.clamp_max = params.clamp_max;
        cmd.push_constants(coopa::gfx::ShaderStage::Fragment, pf);
        prefilter_->draw(cmd);
        down_targets_[0]->end(cmd);

        // --- Stage 2: progressive downsample ---
        for (uint32_t i = 1; i < level_count_; ++i) {
            const auto& src = *down_targets_[i - 1];
            begin_stage_(cmd, *down_targets_[i], kBlack, *downsample_, i);
            DownsamplePush ds{};
            ds.src_texel = glm::vec2(1.0f / static_cast<float>(src.width()), 1.0f / static_cast<float>(src.height()));
            cmd.push_constants(coopa::gfx::ShaderStage::Fragment, ds);
            downsample_->draw(cmd);
            down_targets_[i]->end(cmd);
        }

        // --- Stage 3: progressive tent upsample + same-resolution combine ---
        for (int i = static_cast<int>(level_count_) - 2; i >= 0; --i) {
            const uint32_t ui = static_cast<uint32_t>(i);
            const auto& low = (ui + 2 == level_count_) ? *down_targets_[level_count_ - 1] : *up_targets_[ui + 1];
            begin_stage_(cmd, *up_targets_[ui], kBlack, *upsample_, static_cast<uint32_t>(ui));
            UpsamplePush us{};
            us.low_texel = glm::vec2(1.0f / static_cast<float>(low.width()), 1.0f / static_cast<float>(low.height()));
            us.radius    = params.radius;
            us.scatter   = params.scatter;
            cmd.push_constants(coopa::gfx::ShaderStage::Fragment, us);
            upsample_->draw(cmd);
            up_targets_[ui]->end(cmd);
        }
    }

    /// @brief The finished, fully blurred bloom image, at HALF the source resolution.
    /// Composite it with a plain `texture()` through a LINEAR sampler -- the
    /// hardware bilinear upsample to full resolution is exactly right for an
    /// intentionally soft effect, and costs nothing extra.
    coopa::gfx::TextureView result_view_typed() const {
        // level_count_ == 1 (a render target too small for even one halving) leaves
        // up_targets_ empty: the prefilter output IS the finished bloom.
        return up_targets_.empty() ? down_targets_[0]->color_view_typed()
                                   : up_targets_[0]->color_view_typed();
    }

    uint32_t level_count() const { return level_count_; }
    uint32_t base_width()  const { return base_width_; }
    uint32_t base_height() const { return base_height_; }

private:
    /// begin() + bind + POSITIVE-height viewport. The positive height is not
    /// redundant: OffscreenTarget::begin() sets a NEGATIVE-height viewport (its
    /// Vulkan-NDC Y-flip for geometry passes, offscreen_target.h), which would flip
    /// a fullscreen-triangle pass's output vertically. This pyramid has an odd
    /// number of stages (1 + N + (N-1)), so relying on begin()'s viewport would ship
    /// an upside-down bloom overlay. Every other fullscreen pass in this engine
    /// overrides it the same way -- see SmaaPass::draw() and StylizePass::draw().
    void begin_stage_(coopa::gfx::command::CommandBuffer& cmd, targets::OffscreenTarget& target,
                      VkClearColorValue clear, const FullscreenStage& stage,
                      uint32_t instance) const {
        target.begin(cmd, clear);
        stage.bind(cmd, target.width(), target.height(), instance);
    }

    /// Binds every descriptor this pass ever reads, ONCE. Never called again outside
    /// recreate() -- see the file doc's "no per-frame update_descriptors()" note.
    void bind_static_descriptors_(coopa::gfx::TextureView source_hdr,
                                  const coopa::gfx::engine::util::Sampler& linear_sampler) {
        prefilter_->set().bind_image(0, source_hdr, linear_sampler);

        for (uint32_t i = 1; i < level_count_; ++i) {
            downsample_->set(0, i).bind_image(0, down_targets_[i - 1]->color_view_typed(), linear_sampler);
        }

        for (uint32_t i = 0; i + 1 < level_count_; ++i) {
            // The coarsest upsample step (i == level_count_ - 2) has no up_targets_
            // entry above it yet, so it seeds from the bottom of the DOWN chain
            // instead of a nonexistent up_targets_[i + 1].
            coopa::gfx::TextureView low = (i + 2 == level_count_)
                ? down_targets_[level_count_ - 1]->color_view_typed()
                : up_targets_[i + 1]->color_view_typed();
            upsample_->set(0, static_cast<uint32_t>(i)).bind_image(0, low, linear_sampler);
            upsample_->set(0, static_cast<uint32_t>(i)).bind_image(1, down_targets_[i]->color_view_typed(), linear_sampler);
        }
    }

    coopa::gfx::core::Device&      device_;
    coopa::gfx::memory::Allocator& allocator_;

    uint32_t full_width_  = 0;   ///< Source HDR image size (e.g. 720x480).
    uint32_t full_height_ = 0;
    uint32_t base_width_  = 0;   ///< Pyramid level 0 = half the source (e.g. 360x240).
    uint32_t base_height_ = 0;
    uint32_t level_count_ = 0;

    /// down_targets_[0] is the prefilter output; down_targets_[i] is that halved i times.
    std::vector<std::unique_ptr<targets::OffscreenTarget>> down_targets_;
    /// up_targets_[i] is the same size as down_targets_[i], for i in [0, level_count_ - 1).
    std::vector<std::unique_ptr<targets::OffscreenTarget>> up_targets_;

    std::string vert_spv_, prefilter_frag_spv_, downsample_frag_spv_, upsample_frag_spv_;

    std::unique_ptr<FullscreenStage> prefilter_;   ///< Bright-pass threshold into down_targets_[0].
    std::unique_ptr<FullscreenStage> downsample_;  ///< One set per level; index 0 unused.
    std::unique_ptr<FullscreenStage> upsample_;    ///< One set per up-chain level; null when level_count_ == 1.
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_BLOOM_PASS_H
