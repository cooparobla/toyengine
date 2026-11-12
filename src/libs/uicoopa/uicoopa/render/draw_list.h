/**
 * @file draw_list.h
 * @brief Accumulates a frame's UI geometry into batched, indexed draw calls.
 *
 * Batches carry a TextureView rather than a resolved DescriptorSet: DrawList
 * has no notion of descriptor sets at all, so it can be filled in during
 * CanvasComponent's emit pass regardless of when/how UiPass has resolved
 * textures to descriptor sets. UiPass is the only place image-view-to-set
 * resolution happens, and only outside the render pass (see ui_pass.h) —
 * DescriptorSet::bind_image() updates immediately, which is unsafe mid-frame.
 */

#ifndef UICOOPA_RENDER_DRAW_LIST_H
#define UICOOPA_RENDER_DRAW_LIST_H

#include <uicoopa/render/ui_vertex.h>
#include <uicoopa/render/sprite.h>
#include <uicoopa/layout/rect.h>
#include <gfxcoopa/types/texture_view.h>
#include <algorithm>
#include <vector>
#include <array>
#include <cmath>
#include <cstdint>

namespace coopa {
namespace ui {

/**
 * @struct DrawBatch
 * @brief A contiguous run of indices sharing one texture and one clip rect.
 */
struct DrawBatch {
    uint32_t    first_index;  /**< Offset into DrawList::indices(). */
    uint32_t    index_count;
    coopa::gfx::TextureView texture_view; /**< Resolved to a descriptor set by UiPass at draw time. */
    Rect        clip;         /**< Canvas-space clip rect; converted to a screen-space scissor by UiPass. */
    int         z_order = 0;  /**< Effective z-order at emission time; see DrawList::finalize_z_order(). */
};

/**
 * @brief Computes the four position/UV breakpoints along one axis of a nine-slice.
 *
 * A free function (rather than a DrawList private method) so it's unit-testable with
 * plain floats -- no Sprite, no Texture, no GPU -- see DrawList::add_nine_slice()'s only
 * caller of this.
 *
 * Clamps the two inner breakpoints to the midpoint if the requested borders would
 * overlap (destination smaller than the combined border), avoiding inverted/self-
 * intersecting quads.
 *
 * `uv_border_lo`/`uv_border_hi` are always positive magnitudes (a fraction of texture
 * size); this walks them toward the *interior* of [uv_lo, uv_hi] regardless of which end
 * is numerically larger, via the sign of (uv_hi - uv_lo). That matters for any atlas
 * sub-sprite (icons, glyphs) using the FontAtlas Y-flip convention, where
 * uv.min.y > uv.max.y -- without the sign, `uv_lo + uv_border_lo` would walk outward
 * instead of inward and invert the slice.
 *
 * @param lo,hi Destination rect extent along this axis, in canvas pixels.
 * @param border_lo,border_hi Nine-slice border, in canvas pixels (same units as lo/hi).
 * @param uv_lo,uv_hi Source UV extent along this axis; uv_hi may be less than uv_lo.
 * @param uv_border_lo,uv_border_hi Nine-slice border in UV space (positive magnitudes).
 * @param pos_out Four position breakpoints: {lo, inner_lo, inner_hi, hi}.
 * @param uv_out Four UV breakpoints, signed to walk inward from uv_lo/uv_hi.
 */
inline void nine_slice_axis_breakpoints(float lo, float hi, float border_lo, float border_hi,
                                        float uv_lo, float uv_hi, float uv_border_lo, float uv_border_hi,
                                        std::array<float, 4>& pos_out, std::array<float, 4>& uv_out) {
    float inner_lo = lo + border_lo;
    float inner_hi = hi - border_hi;
    if (inner_lo > inner_hi) {
        float mid = (lo + hi) * 0.5f;
        inner_lo = inner_hi = mid;
    }
    pos_out = { lo, inner_lo, inner_hi, hi };

    float uv_dir = (uv_hi >= uv_lo) ? 1.0f : -1.0f;
    uv_out = { uv_lo, uv_lo + uv_dir * uv_border_lo, uv_hi - uv_dir * uv_border_hi, uv_hi };
}

/**
 * @class DrawList
 * @brief Batches quads and nine-slices into a single indexed vertex/index stream.
 *
 * Usage (from CanvasComponent's emit pass, via Graphic::emit()):
 * @code
 * draw_list.set_texture(sprite.texture->view_typed());
 * draw_list.add_quad(rect, sprite.uv, packed_color);
 * @endcode
 */
class DrawList {
public:
    /**
     * @brief Resets the list for a new frame and seeds the base clip rect.
     * @param canvas_bounds The canvas's root rect; the outermost clip region.
     */
    void begin(const Rect& canvas_bounds) {
        vertices_.clear();
        indices_.clear();
        batches_.clear();
        clip_stack_.clear();
        clip_stack_.push_back(canvas_bounds);
        current_texture_ = default_texture_view_;
        current_z_order_ = 0;
    }

    /**
     * @brief Sets the canvas-pixel -> target-pixel ratio this list's text should be baked at.
     *
     * Text widgets read this to bake their glyph atlas at the size it will actually be DRAWN at
     * rather than at the authored font_size, which is in canvas pixels and can be magnified
     * several times over on screen (see Text::emit()). It is a property of the canvas, so
     * CanvasComponent::rebuild_emit() sets it -- deliberately NOT reset by begin(), which clears
     * per-frame geometry.
     *
     * Defaults to 1.0: a DrawList built by hand, or by a host that never sets it, keeps exactly
     * the pre-supersampling behaviour.
     */
    void set_text_scale(float scale) { text_scale_ = scale; }

    /**
     * @brief Whether glyphs (and other detail that must stay crisp) should snap to the target's
     *        pixel grid -- true for a screen-space canvas, whose canvas pixels map to device
     *        pixels by text_scale(); false (the default) for a world-space one, which has no
     *        fixed pixel grid.
     */
    void set_pixel_snap(bool snap) { pixel_snap_ = snap; }
    bool pixel_snap() const { return pixel_snap_; }

    /** @brief The scale set by set_text_scale(); 1.0 unless a canvas supplied one. */
    float text_scale() const { return text_scale_; }

    /** @brief Sets the texture used for untextured/solid-color quads (typically a 1x1 white texel). */
    void set_default_texture(coopa::gfx::TextureView view) {
        default_texture_view_ = view;
        if (!current_texture_.valid()) current_texture_ = view;
    }

    /** @brief The texture set via set_default_texture(); widgets with no sprite should bind this explicitly. */
    coopa::gfx::TextureView default_texture() const { return default_texture_view_; }

    /** @brief Sets the texture for subsequent add_quad()/add_nine_slice() calls. */
    void set_texture(coopa::gfx::TextureView view) { current_texture_ = view; }
    coopa::gfx::TextureView current_texture() const { return current_texture_; }

    /** @brief Pushes a nested clip rect, intersected with the current one (nested masks narrow, never widen). */
    void push_clip(const Rect& r) { clip_stack_.push_back(intersect(clip_stack_.back(), r)); }

    /**
     * @brief Pushes the base canvas clip verbatim, ignoring (not intersecting with) the
     *        current top — lets a RectTransform::z_order-elevated subtree escape any
     *        ancestor Mask's clip. Must be paired with a matching pop_clip(); a nested
     *        Mask further inside the escaped subtree still intersects correctly from
     *        this new base.
     */
    void push_canvas_clip() { clip_stack_.push_back(clip_stack_.front()); }

    /** @brief Pops the most recent push_clip()/push_canvas_clip(); the base canvas clip is never popped. */
    void pop_clip() {
        if (clip_stack_.size() > 1) clip_stack_.pop_back();
    }

    const Rect& current_clip() const { return clip_stack_.back(); }

    /** @brief Sets the effective z-order for subsequent add_quad()/add_nine_slice() calls. */
    void set_z_order(int z) { current_z_order_ = z; }
    int current_z_order() const { return current_z_order_; }

    /**
     * @brief Stable-sorts batches() by z_order, ascending (higher layers draw last, on
     *        top). Only reorders batch metadata — each DrawBatch already indexes its own
     *        first_index/index_count into the untouched vertex/index buffers — so this
     *        changes draw order without UiPass needing any changes. Called automatically
     *        by CanvasComponent::rebuild_emit(); safe to call again (idempotent).
     */
    void finalize_z_order() {
        std::stable_sort(batches_.begin(), batches_.end(),
            [](const DrawBatch& a, const DrawBatch& b) { return a.z_order < b.z_order; });
    }

    /**
     * @brief Appends a single textured quad.
     * @param pos Quad position in canvas pixel space.
     * @param uv Texture coordinates; uv.min maps to pos.min, uv.max to pos.max (no implicit flip).
     * @param color Packed RGBA8 (see UiVertex::pack_color).
     */
    void add_quad(const Rect& pos, const Rect& uv, uint32_t color) {
        ensure_batch_();
        uint32_t base = static_cast<uint32_t>(vertices_.size());
        vertices_.push_back({ pos.min.x, pos.min.y, uv.min.x, uv.min.y, color });
        vertices_.push_back({ pos.max.x, pos.min.y, uv.max.x, uv.min.y, color });
        vertices_.push_back({ pos.max.x, pos.max.y, uv.max.x, uv.max.y, color });
        vertices_.push_back({ pos.min.x, pos.max.y, uv.min.x, uv.max.y, color });
        indices_.push_back(base + 0); indices_.push_back(base + 1); indices_.push_back(base + 2);
        indices_.push_back(base + 0); indices_.push_back(base + 2); indices_.push_back(base + 3);
        batches_.back().index_count += 6;
    }

    /**
     * @brief Appends one solid triangle (any winding -- UI pipelines don't cull).
     *
     * Samples the current texture at a single UV, so with the default 1x1 white texture
     * bound (the state set_default_texture() leaves) it is a flat-coloured triangle --
     * what tree-expander arrows, dropdown carets and colour-wheel cursors need.
     */
    void add_triangle(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c, uint32_t color,
                      const glm::vec2& uv = glm::vec2(0.5f)) {
        ensure_batch_();
        uint32_t base = static_cast<uint32_t>(vertices_.size());
        vertices_.push_back({ a.x, a.y, uv.x, uv.y, color });
        vertices_.push_back({ b.x, b.y, uv.x, uv.y, color });
        vertices_.push_back({ c.x, c.y, uv.x, uv.y, color });
        indices_.push_back(base + 0); indices_.push_back(base + 1); indices_.push_back(base + 2);
        batches_.back().index_count += 3;
    }

    /**
     * @brief Appends a quad with four independently placed and coloured corners (a, b, c, d in
     *        order around the quad) -- gradients (colour pickers) and rotated rectangles.
     */
    void add_quad_gradient(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c, const glm::vec2& d,
                           uint32_t ca, uint32_t cb, uint32_t cc, uint32_t cd,
                           const glm::vec2& uv = glm::vec2(0.5f)) {
        ensure_batch_();
        uint32_t base = static_cast<uint32_t>(vertices_.size());
        vertices_.push_back({ a.x, a.y, uv.x, uv.y, ca });
        vertices_.push_back({ b.x, b.y, uv.x, uv.y, cb });
        vertices_.push_back({ c.x, c.y, uv.x, uv.y, cc });
        vertices_.push_back({ d.x, d.y, uv.x, uv.y, cd });
        indices_.push_back(base + 0); indices_.push_back(base + 1); indices_.push_back(base + 2);
        indices_.push_back(base + 0); indices_.push_back(base + 2); indices_.push_back(base + 3);
        batches_.back().index_count += 6;
    }

    // --- Rounded rectangles -------------------------------------------------------------
    //
    // Canvas space is +Y up, so a rect's "top" corners are at max.y. Corner masks use the
    // usual reading order: kRoundTL | kRoundTR | kRoundBR | kRoundBL.

    static constexpr int kRoundTL = 1, kRoundTR = 2, kRoundBR = 4, kRoundBL = 8, kRoundAll = 15;

    /**
     * @brief A filled rounded rectangle with an anti-aliased edge (about one screen pixel of
     *        feather, so it stays crisp at any canvas scale). `radius` is clamped to half the
     *        rect's smaller side -- a large radius makes a pill. Uses the bound texture's
     *        centre texel, so bind the default (white) texture for a flat fill.
     */
    void add_rounded_rect(const Rect& r, float radius, int corners, uint32_t color) {
        const float aa = feather_();
        const float rad = clamp_radius_(r, radius);
        const int n = corner_segments_(rad + aa);
        // Fill inset by half the feather, then a feather ring fading out across the true edge.
        std::vector<glm::vec2> inner, outer;
        rounded_perimeter_(r, rad, corners, aa * 0.5f, n, inner);
        rounded_perimeter_(r, rad, corners, -aa * 0.5f, n, outer);
        ensure_batch_();
        const uint32_t base = static_cast<uint32_t>(vertices_.size());
        const glm::vec2 c = r.center();
        vertices_.push_back({c.x, c.y, 0.5f, 0.5f, color});
        for (const auto& p : inner) vertices_.push_back({p.x, p.y, 0.5f, 0.5f, color});
        const uint32_t count = static_cast<uint32_t>(inner.size());
        for (uint32_t i = 0; i < count; ++i) {
            indices_.push_back(base); indices_.push_back(base + 1 + i); indices_.push_back(base + 1 + (i + 1) % count);
        }
        batches_.back().index_count += count * 3;
        add_ring_(inner, outer, color, color & 0x00FFFFFFu);
    }

    /**
     * @brief A rounded outline `width` canvas pixels thick, drawn inside the rect's edge (so a
     *        border never grows the element), anti-aliased on both sides.
     */
    void add_rounded_border(const Rect& r, float radius, int corners, float width, uint32_t color) {
        if (width <= 0.0f) return;
        const float aa = feather_();
        const float rad = clamp_radius_(r, radius);
        const int n = corner_segments_(rad + aa);
        width = std::min(width, std::min(r.size().x, r.size().y) * 0.5f);
        std::vector<glm::vec2> o_feather, o_edge, i_edge, i_feather;
        rounded_perimeter_(r, rad, corners, -aa * 0.5f, n, o_feather);
        rounded_perimeter_(r, rad, corners, aa * 0.5f, n, o_edge);
        rounded_perimeter_(r, rad, corners, std::max(aa * 0.5f, width - aa * 0.5f), n, i_edge);
        rounded_perimeter_(r, rad, corners, width + aa * 0.5f, n, i_feather);
        const uint32_t clear = color & 0x00FFFFFFu;
        add_ring_(o_edge, o_feather, color, clear);
        add_ring_(i_edge, o_edge, color, color);
        add_ring_(i_edge, i_feather, color, clear);
    }

    /**
     * @brief A soft drop shadow for a rounded rect: solid under the rect, fading to clear
     *        `size` canvas pixels beyond its edge. Draw it BEFORE the rect it belongs to.
     */
    void add_rounded_shadow(const Rect& r, float radius, int corners, float size, uint32_t color) {
        if (size <= 0.0f) return;
        const float rad = clamp_radius_(r, radius);
        const int n = corner_segments_(rad + size);
        std::vector<glm::vec2> core, mid, edge;
        rounded_perimeter_(r, rad, corners, size * 0.15f, n, core);
        rounded_perimeter_(r, rad, corners, -size * 0.35f, n, mid);
        rounded_perimeter_(r, rad, corners, -size, n, edge);
        ensure_batch_();
        const uint32_t base = static_cast<uint32_t>(vertices_.size());
        const glm::vec2 c = r.center();
        vertices_.push_back({c.x, c.y, 0.5f, 0.5f, color});
        for (const auto& p : core) vertices_.push_back({p.x, p.y, 0.5f, 0.5f, color});
        const uint32_t count = static_cast<uint32_t>(core.size());
        for (uint32_t i = 0; i < count; ++i) {
            indices_.push_back(base); indices_.push_back(base + 1 + i); indices_.push_back(base + 1 + (i + 1) % count);
        }
        batches_.back().index_count += count * 3;
        // Two bands, the outer one fading faster: a cheap approximation of a gaussian falloff.
        const uint32_t a = (color >> 24) & 0xFFu;
        const uint32_t half = (color & 0x00FFFFFFu) | (static_cast<uint32_t>(a * 0.45f) << 24);
        add_ring_(core, mid, color, half);
        add_ring_(mid, edge, half, color & 0x00FFFFFFu);
    }

    /** @brief Appends a solid line segment `thickness` canvas pixels wide (an oriented quad). */
    void add_line(const glm::vec2& a, const glm::vec2& b, float thickness, uint32_t color) {
        glm::vec2 d = b - a;
        float len = std::sqrt(d.x * d.x + d.y * d.y);
        if (len < 1e-6f) return;
        glm::vec2 n = glm::vec2(-d.y, d.x) * (0.5f * thickness / len);
        add_quad_gradient(a + n, b + n, b - n, a - n, color, color, color, color);
    }

    /**
     * @brief Appends a nine-sliced quad: a 3x3 grid of quads that keeps corner
     *        regions un-stretched while the edges/center stretch to fill pos.
     *
     * Falls back to a single add_quad() if the sprite has no border.
     * @param pos Destination rect in canvas pixel space.
     * @param sprite Source texture region and border, in source texture pixels.
     * @param color Packed RGBA8.
     */
    void add_nine_slice(const Rect& pos, const Sprite& sprite, uint32_t color) {
        if (!sprite.is_nine_sliced() || !sprite.texture) {
            set_texture(sprite.texture ? sprite.texture->view_typed() : current_texture_);
            add_quad(pos, sprite.uv, color);
            return;
        }
        set_texture(sprite.texture->view_typed());

        float tex_w = static_cast<float>(sprite.texture->width());
        float tex_h = static_cast<float>(sprite.texture->height());
        float border_u_l = tex_w > 0.0f ? sprite.border.x / tex_w : 0.0f;
        float border_u_b = tex_h > 0.0f ? sprite.border.y / tex_h : 0.0f;
        float border_u_r = tex_w > 0.0f ? sprite.border.z / tex_w : 0.0f;
        float border_u_t = tex_h > 0.0f ? sprite.border.w / tex_h : 0.0f;

        std::array<float, 4> px{}, uvx{}, py{}, uvy{};
        nine_slice_axis_breakpoints(pos.min.x, pos.max.x, sprite.border.x, sprite.border.z,
                                    sprite.uv.min.x, sprite.uv.max.x, border_u_l, border_u_r, px, uvx);
        nine_slice_axis_breakpoints(pos.min.y, pos.max.y, sprite.border.y, sprite.border.w,
                                    sprite.uv.min.y, sprite.uv.max.y, border_u_b, border_u_t, py, uvy);

        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                Rect cell_pos{ { px[col], py[row] }, { px[col + 1], py[row + 1] } };
                Rect cell_uv{ { uvx[col], uvy[row] }, { uvx[col + 1], uvy[row + 1] } };
                add_quad(cell_pos, cell_uv, color);
            }
        }
    }

    const std::vector<UiVertex>&  vertices() const { return vertices_; }
    const std::vector<uint32_t>&  indices()  const { return indices_; }
    const std::vector<DrawBatch>& batches()  const { return batches_; }

private:
    /** @brief One screen pixel, in canvas pixels -- the anti-aliasing feather width. */
    float feather_() const { return 1.0f / std::max(0.25f, text_scale_); }

    static float clamp_radius_(const Rect& r, float radius) {
        return std::max(0.0f, std::min(radius, std::min(r.size().x, r.size().y) * 0.5f));
    }

    /** @brief Arc segments per rounded corner: enough to look round at this canvas's scale. */
    int corner_segments_(float radius) const {
        return std::clamp(static_cast<int>(std::ceil(radius * std::max(0.25f, text_scale_) * 0.45f)), 2, 16);
    }

    /**
     * @brief The rounded-rect outline, `inset` canvas pixels inside the rect's edge (negative:
     *        outside), as one closed loop with the SAME point count for any inset -- so two
     *        loops of one rect pair up index by index into a ring. A square corner (not in
     *        `corners`) is a zero-radius arc: inset it stays square, outset it rounds by the
     *        outset, which is what a feather or shadow wants.
     */
    void rounded_perimeter_(const Rect& r, float radius, int corners, float inset, int n, std::vector<glm::vec2>& out) const {
        out.clear();
        out.reserve(static_cast<size_t>(4 * (n + 1)));
        struct Corner { glm::vec2 k; glm::vec2 s; float a0; int bit; };
        // Counter-clockwise from bottom-left (canvas +Y up).
        const Corner cs[4] = {
            {{r.min.x, r.min.y}, {1.0f, 1.0f}, 3.14159265f, kRoundBL},
            {{r.max.x, r.min.y}, {-1.0f, 1.0f}, 4.71238898f, kRoundBR},
            {{r.max.x, r.max.y}, {-1.0f, -1.0f}, 0.0f, kRoundTR},
            {{r.min.x, r.max.y}, {1.0f, -1.0f}, 1.57079633f, kRoundTL},
        };
        for (const Corner& c : cs) {
            const float rk = (corners & c.bit) ? radius : 0.0f;
            const float rad = std::max(rk - inset, 0.0f);
            const glm::vec2 centre = c.k + c.s * std::max(rk, inset);
            for (int i = 0; i <= n; ++i) {
                const float a = c.a0 + 1.57079633f * static_cast<float>(i) / static_cast<float>(n);
                out.push_back(centre + glm::vec2(std::cos(a), std::sin(a)) * rad);
            }
        }
    }

    /** @brief A band between two matching loops (see rounded_perimeter_()), colours per loop. */
    void add_ring_(const std::vector<glm::vec2>& a, const std::vector<glm::vec2>& b, uint32_t ca, uint32_t cb) {
        if (a.size() != b.size() || a.empty()) return;
        ensure_batch_();
        const uint32_t base = static_cast<uint32_t>(vertices_.size());
        const uint32_t n = static_cast<uint32_t>(a.size());
        for (uint32_t i = 0; i < n; ++i) {
            vertices_.push_back({a[i].x, a[i].y, 0.5f, 0.5f, ca});
            vertices_.push_back({b[i].x, b[i].y, 0.5f, 0.5f, cb});
        }
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t j = (i + 1) % n;
            const uint32_t a0 = base + 2 * i, b0 = a0 + 1, a1 = base + 2 * j, b1 = a1 + 1;
            indices_.push_back(a0); indices_.push_back(b0); indices_.push_back(b1);
            indices_.push_back(a0); indices_.push_back(b1); indices_.push_back(a1);
        }
        batches_.back().index_count += n * 6;
    }

    void ensure_batch_() {
        const Rect& clip = clip_stack_.back();
        if (batches_.empty() ||
            batches_.back().texture_view != current_texture_ ||
            batches_.back().z_order != current_z_order_ ||
            !rects_equal_(batches_.back().clip, clip)) {
            DrawBatch b{};
            b.first_index   = static_cast<uint32_t>(indices_.size());
            b.index_count   = 0;
            b.texture_view  = current_texture_;
            b.clip          = clip;
            b.z_order       = current_z_order_;
            batches_.push_back(b);
        }
    }

    static bool rects_equal_(const Rect& a, const Rect& b) {
        return a.min == b.min && a.max == b.max;
    }

    std::vector<UiVertex>  vertices_;
    std::vector<uint32_t>  indices_;
    std::vector<DrawBatch> batches_;
    std::vector<Rect>      clip_stack_{ Rect{} };
    coopa::gfx::TextureView current_texture_;
    coopa::gfx::TextureView default_texture_view_;
    /// See set_text_scale(). Not touched by begin() -- it describes the canvas, not the frame.
    float                   text_scale_ = 1.0f;
    bool                    pixel_snap_ = false;   ///< See set_pixel_snap().
    int                     current_z_order_ = 0;
};

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_RENDER_DRAW_LIST_H
