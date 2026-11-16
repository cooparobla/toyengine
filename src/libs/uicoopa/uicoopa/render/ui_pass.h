/**
 * @file ui_pass.h
 * @brief Composites a DrawList on top of the existing swapchain render pass.
 *
 * Thin wrapper around gfxcoopa's TexturedQuad2DPass (engine/passes/textured_quad_2d_pass.h)
 * -- the descriptor layout/pool, TextureView->DescriptorSet cache, streaming vertex/index
 * buffers, and pipeline-variant machinery are all shared with pixengine's SpritePass now;
 * this file only owns what's genuinely UI-specific: per-batch clip-rect scissoring, the
 * canvas-space push constant, and dispatching "is this batch a text atlas?" to the "text"
 * pipeline variant (see ui_quad.frag/ui_text.frag) instead of the runtime branch this used
 * to be.
 *
 * Modeled directly on gfxcoopa/engine/passes/present_pass.h. gfxcoopa's
 * RenderPass always clears its color attachment on load,
 * so a second render pass over the swapchain image would erase the 3D scene —
 * UiPass therefore draws inside the *same* render pass the 3D pipeline already
 * opened, appended to its record lambda after the 3D present blit.
 *
 * Texture registration is split from drawing: DescriptorSet::bind_image()
 * updates descriptor sets immediately, which is unsafe once a render
 * pass has begun. register_textures() must run before Renderer::begin_frame();
 * draw() only ever looks up already-registered descriptor sets.
 *
 * ## The "glass" variant
 *
 * A third pipeline variant, alongside "" (quad) and "text", for Context::glass()/glass_panel():
 * it samples the host's blurred-backdrop image (set_backdrop_view()) by gl_FragCoord instead of
 * the quad's own uv, so a frosted UI pane does not scroll with its contents. Selected the exact
 * same way "text" is -- a side-table of TextureViews (glass_views_, mirroring text_views_), not
 * a vertex flag or a push-constant branch, because UiVertex has no spare attribute to carry one
 * and a glass quad already needs a unique texture key to batch correctly.
 *
 * The backdrop is bound as an ORDINARY set-0 texture, not through ExtraSets: a glass quad's
 * texture key IS the backdrop view, so it gets its own batch and its own pipeline bind for free
 * through the same machinery that already separates font-atlas batches from sprite batches --
 * no second descriptor set, no new layout, no ordering question with the rest of the pipeline
 * (compare how adding a wrongly-ordered ExtraSets set once rendered a plausible-looking but
 * unlit frame with no Vulkan error, in an unrelated part of the engine). The cost: a glass batch
 * cannot also sample an authored texture (a noise map for "wobbly" glass, say). If that is ever
 * needed, the delta is small and worth stating here rather than guessing at migration time: add
 * an `ExtraSets` parameter to the ctor (forwarded to `desc.extra`, as world_ui_pass_ already
 * does), call `pass_->bind_extra(cmd)` right after the per-batch pipeline bind in draw() below,
 * and move `u_backdrop` in ui_glass.frag to `set = 1, binding = 0`. Everything else -- the
 * sentinel-view batching, set_backdrop_view()'s refusal when no "glass" variant exists, the
 * push-constant remap -- stays exactly as it is.
 */

#ifndef UICOOPA_RENDER_UI_PASS_H
#define UICOOPA_RENDER_UI_PASS_H

#include <memory>
#include <string>
#include <unordered_set>
#include <algorithm>
#include <cmath>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/passes/textured_quad_2d_pass.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/sampler_desc.h>
#include <gfxcoopa/types/texture_view.h>

#include <uicoopa/render/ui_vertex.h>
#include <uicoopa/render/draw_list.h>

namespace coopa {
namespace ui {

/**
 * @struct UiPushConstants
 * @brief Matches the `Push` block declared in ui.vert exactly -- see
 *        gfx/surface2d/quad_vs.glsl's doc for the scale/offset convention. Text vs quad
 *        vs glass is a pipeline variant, not a push-constant flag (see ui_quad.frag/
 *        ui_text.frag/ui_glass.frag).
 *
 * The trailing pair is fragment-only (ui_glass.frag's gl_FragCoord -> backdrop-UV remap,
 * see backdrop_remap()) but declared in ui.vert's block too, since one push range covers
 * both stages -- a block that differs in size per stage is the kind of thing validation
 * layers flag. 32 bytes, well inside the 128 Vulkan guarantees, and still pushed ONCE PER
 * FRAME (see draw()): the backdrop rect is a property of the frame's letterbox, not of any
 * one batch.
 */
struct UiPushConstants {
    float scale[2];
    float offset[2];
    float backdrop_origin[2];    ///< Target pixels, top-left of the backdrop rect.
    float backdrop_inv_size[2];  ///< 1 / that rect's size in target pixels; 0 if the rect is empty.
};
static_assert(sizeof(UiPushConstants) == 32, "must match ui.vert byte-for-byte");

/**
 * @class UiPass
 * @brief Owns the UI graphics pipeline (via TexturedQuad2DPass), per-frame streaming
 *        geometry buffers, and per-batch clip-rect scissoring.
 *
 * Usage, mirroring blendy's PbrRenderPipeline::render():
 * @code
 * ui_pass.register_textures(draw_list);         // before begin_frame — safe to update descriptors
 * renderer.begin_frame([&](CommandBuffer& cmd) {
 *     present_pass.draw(cmd, sw, sh);
 *     ui_pass.draw(cmd, renderer.current_frame(), sw, sh, canvas.scale_factor(), draw_list);
 * });
 * @endcode
 */
class UiPass {
public:
    /**
     * @param device          Logical device.
     * @param allocator       VMA allocator.
     * @param cmd_pool        Command pool for the default white texture's one-shot upload.
     * @param swapchain_pass  The SAME render pass Renderer draws the 3D scene into.
     * @param vert_spv        Path to ui.vert.spv -- shared by both pipeline variants below.
     * @param quad_frag_spv   Path to ui_quad.frag.spv -- the stock RGBA-sampling variant.
     * @param text_frag_spv   Path to ui_text.frag.spv -- the R8-coverage variant, bound for
     *                        batches whose texture was mark_as_text_atlas()'d.
     * @param glass_frag_spv  Path to ui_glass.frag.spv -- samples the view passed to
     *                        set_backdrop_view() by gl_FragCoord instead of the quad's own
     *                        uv. Empty (the default) registers no "glass" variant at all, so
     *                        a host that never built a backdrop pays nothing for it, and
     *                        set_backdrop_view() refuses any view it is given.
     * @param max_textures    Upper bound on distinct textures drawn in a single frame
     *                        (sprites + font atlases); sizes the descriptor pool.
     */
    UiPass(coopa::gfx::core::Device& device,
           coopa::gfx::memory::Allocator& allocator,
           coopa::gfx::command::CommandPool& cmd_pool,
           coopa::gfx::pipeline::RenderPass& swapchain_pass,
           const std::string& vert_spv,
           const std::string& quad_frag_spv,
           const std::string& text_frag_spv,
           const std::string& glass_frag_spv = "",
           uint32_t max_textures = 256)
    {
        using namespace coopa::gfx;
        using namespace coopa::gfx::engine::passes;

        TexturedQuad2DDesc desc;
        desc.vertex             = UiVertex::layout();
        desc.vertex_stride      = sizeof(UiVertex);
        // AlphaOver, not Alpha: the two differ only in what they leave in the destination's
        // ALPHA channel, and they are identical over the opaque targets this pass normally
        // draws into. Matching UiWorldPass (which genuinely needs the accumulating variant --
        // see its own comment) keeps the two UI passes' blend state from silently diverging.
        desc.blend_mode         = pipeline::BlendMode::AlphaOver;
        desc.push_constant_size = sizeof(UiPushConstants);
        // Bilinear + clamp-to-edge, not linear_repeat(): this sampler is shared by every
        // texture UiPass binds (see TexturedQuad2DPass::register_view()), including
        // sprite-sheet sub-rects whose UVs never reach 0/1 -- REPEAT risks sampling a
        // neighboring packed icon/glyph at the seam under bilinear filtering, while
        // CLAMP_TO_EDGE is a no-op for the full-[0,1] quads (white texture, unsliced
        // whole-texture sprites) every other caller relies on.
        desc.sampler_desc = coopa::gfx::SamplerDesc::linear_repeat();
        desc.sampler_desc.address = coopa::gfx::AddressMode::ClampToEdge;
        desc.fallback_pixel = {255, 255, 255, 255}; // white -- an untextured Graphic reads as "no image"
        desc.initial_max_verts   = kInitialMaxVerts;
        desc.initial_max_indices = kInitialMaxIndices;
        desc.max_textures        = max_textures;

        pass_ = std::make_unique<TexturedQuad2DPass>(
            device, allocator, cmd_pool, swapchain_pass, vert_spv, quad_frag_spv, desc);
        pass_->add_variant("text", vert_spv, text_frag_spv);
        if (!glass_frag_spv.empty()) pass_->add_variant("glass", vert_spv, glass_frag_spv);
    }

    /** @brief Returns the default white texture's view, for callers that need to seed DrawList::set_default_texture(). */
    coopa::gfx::TextureView white_view() const { return pass_->fallback_view(); }

    /**
     * @brief Resolves every texture referenced by draw_list's batches into a descriptor set.
     *
     * Must be called before Renderer::begin_frame() — never while a render pass is open.
     * Already-registered textures (by TextureView) are skipped, so this is cheap to call
     * every frame even when the texture set is mostly stable.
     */
    void register_textures(const DrawList& draw_list) {
        for (const DrawBatch& batch : draw_list.batches()) {
            pass_->register_view(batch.texture_view);
        }
    }

    /**
     * @brief Sizes this frame's shared geometry buffers and resets the append cursor.
     *
     * Must be called once per frame, before that frame's draw() calls, and -- like
     * register_textures() -- before the host's Renderer::begin_frame(), since growing a
     * buffer destroys and recreates it.
     *
     * The totals are the SUM over every canvas this frame will draw. Passing a short total
     * is not a soft failure: draw() would have nowhere to append the overflow, and the
     * recorded draw commands of earlier canvases would read whatever landed there instead.
     *
     * @param frame_index     Host's current frame-in-flight index.
     * @param total_vertices  Sum of draw_list().vertices().size() over this frame's canvases.
     * @param total_indices   Sum of draw_list().indices().size() over this frame's canvases.
     */
    void begin_frame(uint32_t frame_index, size_t total_vertices, size_t total_indices) {
        pass_->ensure_capacity(frame_index, total_vertices, total_indices);
        vertex_cursor_ = 0;
        index_cursor_  = 0;
    }

    /**
     * @brief Records the UI draw calls into an already-open render pass.
     *
     * Every texture referenced by draw_list must already have been registered via
     * register_textures() earlier this frame, before Renderer::begin_frame() was called.
     *
     * @param cmd           Command buffer, mid-recording inside the swapchain render pass.
     * @param frame_index   renderer.current_frame() — selects this frame's geometry buffers.
     * @param screen_w      Framebuffer width in pixels.
     * @param screen_h      Framebuffer height in pixels.
     * @param scale_factor  Canvas's scale_factor() (screen pixels per canvas pixel); used to
     *                      convert canvas-space clip rects to screen-space scissors.
     * @param draw_list     This frame's batched UI geometry.
     */
    void draw(coopa::gfx::command::CommandBuffer& cmd,
              uint32_t frame_index,
              uint32_t screen_w, uint32_t screen_h,
              float scale_factor,
              const DrawList& draw_list) {
        draw(cmd, frame_index, screen_w, screen_h, glm::vec2(0.0f), screen_w, screen_h, scale_factor, draw_list);
    }

    /**
     * @brief draw(), for a canvas placed inside PART of the target: its screen_w x screen_h
     *        rect has its top-left at `origin` (target pixels, +Y down) -- see
     *        CanvasComponent::set_screen_origin(). Every batch's scissor is clamped to that
     *        rect as well as to the target, so nothing the canvas emits can spill outside it.
     *
     * @param target_w/target_h The bound render target's size, for clamping.
     */
    void draw(coopa::gfx::command::CommandBuffer& cmd,
              uint32_t frame_index,
              uint32_t target_w, uint32_t target_h,
              glm::vec2 origin,
              uint32_t screen_w, uint32_t screen_h,
              float scale_factor,
              const DrawList& draw_list) {
        if (draw_list.indices().empty()) return;

        // Append at this frame's running cursor -- see begin_frame(). Every canvas the host
        // draws shares one buffer pair per frame slot, so uploading at offset 0 here would
        // make the LAST canvas's geometry the only geometry, and every earlier canvas's
        // already-recorded draw commands would read it by mistake. Capacity is the caller's
        // responsibility via begin_frame(), because growing a buffer mid-frame would
        // invalidate exactly the geometry those recorded commands point at.
        const uint32_t first_vertex = vertex_cursor_;
        const uint32_t first_index  = index_cursor_;
        pass_->vertex_buffer(frame_index).upload(
            draw_list.vertices().data(), draw_list.vertices().size() * sizeof(UiVertex),
            static_cast<size_t>(first_vertex) * sizeof(UiVertex));
        pass_->index_buffer(frame_index).upload(
            draw_list.indices().data(), draw_list.indices().size() * sizeof(uint32_t),
            static_cast<size_t>(first_index) * sizeof(uint32_t));
        vertex_cursor_ += static_cast<uint32_t>(draw_list.vertices().size());
        index_cursor_  += static_cast<uint32_t>(draw_list.indices().size());

        pass_->bind(cmd); // stock ("quad") -- rebound to "text" per batch below as needed
        cmd.set_viewport(origin.x, origin.y, static_cast<float>(screen_w), static_cast<float>(screen_h));
        cmd.bind_vertex_buffer(pass_->vertex_buffer(frame_index));
        cmd.bind_index_buffer(pass_->index_buffer(frame_index));

        float canvas_w = scale_factor > 0.0f ? static_cast<float>(screen_w) / scale_factor : static_cast<float>(screen_w);
        float canvas_h = scale_factor > 0.0f ? static_cast<float>(screen_h) / scale_factor : static_cast<float>(screen_h);

        UiPushConstants push{};
        push.scale[0]  = canvas_w > 0.0f ? (1.0f / canvas_w) * 2.0f : 0.0f;
        push.scale[1]  = canvas_h > 0.0f ? (1.0f / canvas_h) * 2.0f : 0.0f;
        push.offset[0] = -1.0f;
        push.offset[1] = -1.0f;
        const BackdropRemap remap = backdrop_remap(backdrop_origin_, backdrop_size_);
        push.backdrop_origin[0] = remap.origin[0]; push.backdrop_origin[1] = remap.origin[1];
        push.backdrop_inv_size[0] = remap.inv_size[0]; push.backdrop_inv_size[1] = remap.inv_size[1];
        // Scale/offset/backdrop are per-FRAME (canvas size, letterbox rect), not per-batch --
        // pushed once. The pipeline bind below (quad/text/glass variant) is what varies per batch.
        cmd.push_constants(coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment, push);

        enum class Variant { Quad, Text, Glass };
        Variant last_variant = Variant::Quad; // "quad" bound just above
        bool have_bound = true;

        for (const DrawBatch& batch : draw_list.batches()) {
            if (batch.index_count == 0) continue;

            // Glass checked first: a view can only ever be in one of these two sets, but
            // ordering the check makes that explicit rather than depending on it.
            const Variant want = is_glass_view_(batch.texture_view) ? Variant::Glass
                                : is_text_view_(batch.texture_view)  ? Variant::Text : Variant::Quad;
            if (!have_bound || want != last_variant) {
                pass_->bind(cmd, want == Variant::Glass ? "glass" : want == Variant::Text ? "text" : "");
                last_variant = want;
                have_bound = true;
            }

            cmd.bind_descriptor_set(pass_->descriptor_set_for(batch.texture_view));

            ScreenScissor scissor = place_scissor(to_screen_scissor_(batch.clip, screen_w, screen_h, scale_factor),
                                                  origin, target_w, target_h);
            if (scissor.w == 0 || scissor.h == 0) continue;
            cmd.set_scissor(scissor.x, scissor.y, scissor.w, scissor.h);

            // first_index shifts by this canvas's slice start; vertex_offset re-bases the
            // indices, which DrawList wrote relative to this canvas's own vertex array.
            cmd.draw_indexed(batch.index_count,
                             first_index + batch.first_index,
                             static_cast<int32_t>(first_vertex),
                             1);
        }
    }

    /**
     * @brief Marks a texture view as an R8 coverage atlas (glyph atlas), so draw() binds
     *        the "text" pipeline variant for batches using it rather than sampling it as a
     *        full RGBA color.
     *
     * Called by FontAtlas when it registers its texture; unmarked views are treated as
     * ordinary RGBA sprites.
     */
    void mark_as_text_atlas(coopa::gfx::TextureView view) { text_views_.insert(view); }

    /**
     * @brief Sets the host's blurred-backdrop image, for Context::glass()/glass_panel() batches.
     *
     * Registers the view and marks it so draw() binds the "glass" pipeline variant for any
     * batch that uses it as its texture (a glass quad's texture key IS the backdrop view --
     * see DrawList::set_backdrop_texture()). Refuses an invalid view, or any view at all when
     * no "glass" variant was built (glass_frag_spv was empty at construction): the degrade
     * path is a flat fill, drawn by the host via Context::glass()'s own fallback, not a quad
     * variant sampling an unset backdrop at a fixed uv.
     *
     * Must run before Renderer::begin_frame(), same as register_textures() -- it calls
     * register_view() when the view is accepted.
     */
    void set_backdrop_view(coopa::gfx::TextureView view) {
        if (!pass_->has_variant("glass")) return;
        if (!view.valid()) { backdrop_view_ = {}; return; }
        backdrop_view_ = view;
        pass_->register_view(view);
        glass_views_.insert(view);
    }

    /** @brief The view set by set_backdrop_view(), or an invalid view if none was accepted. */
    coopa::gfx::TextureView backdrop_view() const { return backdrop_view_; }

    /**
     * @brief Where the backdrop image sits in the TARGET this pass draws into, in target
     *        pixels (the letterbox rect) -- ui_glass.frag's gl_FragCoord fetch is relative to
     *        the whole framebuffer, so this is what maps a fragment back onto the backdrop.
     *
     * Pure per-frame CPU state (no descriptor work), so safe to call any time before this
     * frame's draw(), including mid-render-pass.
     */
    void set_backdrop_rect(glm::vec2 origin, glm::vec2 size) { backdrop_origin_ = origin; backdrop_size_ = size; }

    /** @brief The gl_FragCoord -> backdrop-UV remap, as the push constants carry it. */
    struct BackdropRemap { float origin[2]; float inv_size[2]; };

    /**
     * @brief Computes the remap from a backdrop rect -- public and pure so a CPU test can
     *        check this exact arithmetic with no device. ui_glass.frag's first lines mirror
     *        this function (and backdrop_uv() below); the duplication is real and a future
     *        edit to either one must keep both in step, since no test can catch a C++/GLSL
     *        divergence directly.
     */
    static BackdropRemap backdrop_remap(glm::vec2 origin, glm::vec2 size) {
        BackdropRemap r{};
        r.origin[0] = origin.x; r.origin[1] = origin.y;
        r.inv_size[0] = size.x > 0.0f ? 1.0f / size.x : 0.0f;
        r.inv_size[1] = size.y > 0.0f ? 1.0f / size.y : 0.0f;
        return r;
    }

    /** @brief frag_coord (framebuffer pixels) -> backdrop uv in [0,1], clamped. Mirrors ui_glass.frag. */
    static glm::vec2 backdrop_uv(glm::vec2 frag_coord, const BackdropRemap& r) {
        const float u = (frag_coord.x - r.origin[0]) * r.inv_size[0];
        const float v = (frag_coord.y - r.origin[1]) * r.inv_size[1];
        return glm::vec2(std::clamp(u, 0.0f, 1.0f), std::clamp(v, 0.0f, 1.0f));
    }

    /**
     * @brief Forgets everything this pass knows about `view` -- call before destroying the
     *        Texture that owns it.
     *
     * Mandatory, not hygiene. TextureView is the raw VkImageView handle value, and drivers
     * reuse a freed handle for the next view created; both caches below are keyed by it, so
     * a stale entry silently captures the next texture that lands on the same handle. See
     * TexturedQuad2DPass::unregister_view() for the descriptor-set half.
     *
     * The text_views_ half is the nastier of the two and is why this wrapper exists rather
     * than a bare forward: a glyph atlas is an R8 coverage texture drawn through the "text"
     * pipeline variant. Destroy a Font, upload an ordinary RGBA texture that inherits the
     * atlas's handle, and is_text_view_() still says true -- so the new texture is sampled
     * as single-channel coverage and draws as a flat tinted block.
     *
     * Safe to call for a view that was never registered or never marked.
     *
     * @param view The view to forget, before its Texture is destroyed and while no
     *             in-flight frame still references it.
     */
    void unregister_texture(coopa::gfx::TextureView view) {
        pass_->unregister_view(view);
        text_views_.erase(view);
        glass_views_.erase(view);
        if (backdrop_view_ == view) backdrop_view_ = {};
    }

    /** @brief A clamped, screen-pixel scissor rect. */
    struct ScreenScissor { int32_t x, y; uint32_t w, h; };

    /**
     * @brief Moves a canvas-local scissor (already clamped to the canvas's own rect) by the
     *        canvas's `origin` and clamps it to the target. Public and pure so a host's tests
     *        can check placement without a device.
     */
    static ScreenScissor place_scissor(ScreenScissor s, glm::vec2 origin, uint32_t target_w, uint32_t target_h) {
        const int64_t ox = static_cast<int64_t>(std::lround(origin.x));
        const int64_t oy = static_cast<int64_t>(std::lround(origin.y));
        const int64_t x0 = std::clamp<int64_t>(s.x + ox, 0, target_w);
        const int64_t y0 = std::clamp<int64_t>(s.y + oy, 0, target_h);
        const int64_t x1 = std::clamp<int64_t>(s.x + ox + static_cast<int64_t>(s.w), 0, target_w);
        const int64_t y1 = std::clamp<int64_t>(s.y + oy + static_cast<int64_t>(s.h), 0, target_h);
        return ScreenScissor{static_cast<int32_t>(x0), static_cast<int32_t>(y0),
                             static_cast<uint32_t>(std::max<int64_t>(0, x1 - x0)),
                             static_cast<uint32_t>(std::max<int64_t>(0, y1 - y0))};
    }

    /** @brief Converts a canvas-space clip rect to a scissor clamped to the canvas's own rect. */
    static ScreenScissor to_screen_scissor_(const Rect& clip, uint32_t screen_w, uint32_t screen_h, float scale_factor) {
        float x0 = clip.min.x * scale_factor;
        float x1 = clip.max.x * scale_factor;
        float y0 = static_cast<float>(screen_h) - clip.max.y * scale_factor;  // top edge
        float y1 = static_cast<float>(screen_h) - clip.min.y * scale_factor;  // bottom edge

        x0 = std::clamp(x0, 0.0f, static_cast<float>(screen_w));
        x1 = std::clamp(x1, 0.0f, static_cast<float>(screen_w));
        y0 = std::clamp(y0, 0.0f, static_cast<float>(screen_h));
        y1 = std::clamp(y1, 0.0f, static_cast<float>(screen_h));

        return ScreenScissor{
            static_cast<int32_t>(std::round(x0)), static_cast<int32_t>(std::round(y0)),
            static_cast<uint32_t>(std::max(0.0f, std::round(x1 - x0))),
            static_cast<uint32_t>(std::max(0.0f, std::round(y1 - y0)))
        };
    }

private:
    static constexpr uint32_t kInitialMaxVerts   = 8192;
    static constexpr uint32_t kInitialMaxIndices = 12288;

    /// Running append offsets into this frame's shared buffers; reset by begin_frame().
    uint32_t vertex_cursor_ = 0;
    uint32_t index_cursor_  = 0;

    bool is_text_view_(coopa::gfx::TextureView view) const {
        return view != pass_->fallback_view() && text_views_.count(view) != 0;
    }
    bool is_glass_view_(coopa::gfx::TextureView view) const {
        return view != pass_->fallback_view() && glass_views_.count(view) != 0;
    }

    std::unique_ptr<coopa::gfx::engine::passes::TexturedQuad2DPass> pass_;
    std::unordered_set<coopa::gfx::TextureView> text_views_;
    std::unordered_set<coopa::gfx::TextureView> glass_views_;
    coopa::gfx::TextureView backdrop_view_;
    glm::vec2 backdrop_origin_{0.0f};
    glm::vec2 backdrop_size_{0.0f};
};

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_RENDER_UI_PASS_H
