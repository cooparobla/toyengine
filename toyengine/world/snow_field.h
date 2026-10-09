/**
 * @file snow_field.h
 * @brief The snow trench field: how deep things have pressed into the lying snow around the
 *        focus (the camera), as a toroidal grid the renderer reads (render/surface_world.h,
 *        gfx_world_trench() in assets/shaders/gfx/surface/world.glsl) and gameplay can query.
 *
 * A window of n x n cells (default 512 x 512 at 0.1 m: 51.2 m a side) centred on the focus.
 * Cells are addressed by their WORLD cell index modulo n, so moving the window only clears the
 * rows / columns that scrolled in -- nothing is copied. Each cell holds the trench depth in
 * metres as unorm16 over `scale` metres, two cells per 32-bit word (the layout the shader's
 * SSBO reads).
 *
 *  - stamp(): presses a disc in (max-combine: a second footstep in the same place does not dig
 *    deeper than the deeper of the two).
 *  - refill(): new snow fills every trench a little (WeatherSystem's accumulation rate).
 *  - trench_at(): bilinear depth at a world point, as the shader sees it.
 *
 * Also here: the CPU mirror of the shader's "open to the sky" test and deep-snow height
 * (snow_open_sky(), deep_snow_depth()) so gameplay -- footstep sounds, sinking, tracks -- reads
 * the same snow the renderer draws.
 */

#ifndef TOYENGINE_WORLD_SNOW_FIELD_H
#define TOYENGINE_WORLD_SNOW_FIELD_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/particles/particle_system.h>

namespace toy {
namespace world {

class SnowField {
public:
    explicit SnowField(int n = 512, float cell = 0.1f, float scale = 1.0f)
        : n_(std::max(2, n + (n & 1))), cell_(std::max(cell, 1e-3f)), scale_(std::max(scale, 1e-3f)),
          words_(static_cast<size_t>(n_) * static_cast<size_t>(n_) / 2u, 0u) {}

    int n() const { return n_; }
    float cell() const { return cell_; }
    /** @brief Metres a full unorm16 (65535) trench is deep. */
    float scale() const { return scale_; }
    /** @brief World cell index of the window's min corner. */
    glm::ivec2 window() const { return window_; }
    const std::vector<uint32_t>& words() const { return words_; }
    /** @brief Bumped whenever words() change (the renderer re-uploads on a change). */
    uint64_t version() const { return version_; }
    bool focused() const { return focused_; }

    /** @brief Clears every trench. */
    void clear() {
        std::fill(words_.begin(), words_.end(), 0u);
        ++version_;
    }

    /**
     * @brief Centres the window on world point `xy`. Cells that scroll in start untouched; a
     *        jump further than the window clears everything.
     */
    void set_focus(const glm::vec2& xy) {
        const glm::ivec2 lo(static_cast<int>(std::floor(xy.x / cell_)) - n_ / 2,
                            static_cast<int>(std::floor(xy.y / cell_)) - n_ / 2);
        if (!focused_) { window_ = lo; focused_ = true; return; }
        if (lo == window_) return;
        const glm::ivec2 d = lo - window_;
        if (std::abs(d.x) >= n_ || std::abs(d.y) >= n_) {
            window_ = lo;
            clear();
            return;
        }
        // Columns entering on x: world cells [old_hi, new_hi) or [new_lo, old_lo).
        auto clear_col = [&](int wx) { for (int j = 0; j < n_; ++j) set_raw_(wrap_(wx), wrap_(lo.y + j), 0u); };
        auto clear_row = [&](int wy) { for (int i = 0; i < n_; ++i) set_raw_(wrap_(lo.x + i), wrap_(wy), 0u); };
        if (d.x > 0) for (int wx = window_.x + n_; wx < lo.x + n_; ++wx) clear_col(wx);
        if (d.x < 0) for (int wx = lo.x; wx < window_.x; ++wx) clear_col(wx);
        if (d.y > 0) for (int wy = window_.y + n_; wy < lo.y + n_; ++wy) clear_row(wy);
        if (d.y < 0) for (int wy = lo.y; wy < window_.y; ++wy) clear_row(wy);
        window_ = lo;
        ++version_;
    }

    /**
     * @brief Presses a disc into the snow: `depth` metres at the centre, flat out to
     *        radius * (1 - falloff), easing to nothing at `radius`. Max-combined.
     */
    void stamp(const glm::vec2& center, float radius, float depth, float falloff = 0.5f) {
        if (!focused_ || radius <= 0.0f || depth <= 0.0f) return;
        const float inner = radius * (1.0f - std::clamp(falloff, 0.0f, 1.0f));
        const int i0 = static_cast<int>(std::floor((center.x - radius) / cell_));
        const int i1 = static_cast<int>(std::floor((center.x + radius) / cell_));
        const int j0 = static_cast<int>(std::floor((center.y - radius) / cell_));
        const int j1 = static_cast<int>(std::floor((center.y + radius) / cell_));
        bool changed = false;
        for (int wj = j0; wj <= j1; ++wj) {
            for (int wi = i0; wi <= i1; ++wi) {
                if (!in_window_(wi, wj)) continue;
                const glm::vec2 c((static_cast<float>(wi) + 0.5f) * cell_, (static_cast<float>(wj) + 0.5f) * cell_);
                const float r = glm::distance(c, center);
                if (r >= radius) continue;
                float w = 1.0f;
                if (r > inner) {
                    const float t = (r - inner) / std::max(radius - inner, 1e-6f);
                    w = 1.0f - t * t * (3.0f - 2.0f * t);
                }
                const uint32_t v = encode_(depth * w);
                const int ti = wrap_(wi), tj = wrap_(wj);
                if (v > get_raw_(ti, tj)) { set_raw_(ti, tj, v); changed = true; }
            }
        }
        if (changed) ++version_;
    }

    /** @brief Fills every trench by `metres` (fresh snowfall). */
    void refill(float metres) {
        const uint32_t dv = encode_(metres);
        if (dv == 0u) return;
        bool changed = false;
        for (uint32_t& w : words_) {
            if (w == 0u) continue;
            const uint32_t lo = w & 0xFFFFu, hi = w >> 16;
            const uint32_t nlo = lo > dv ? lo - dv : 0u, nhi = hi > dv ? hi - dv : 0u;
            w = nlo | (nhi << 16);
            changed = true;
        }
        if (changed) ++version_;
    }

    /** @brief The trench depth (m) at world xy, bilinear between cell centres; 0 outside the window. */
    float trench_at(const glm::vec2& xy) const {
        if (!focused_) return 0.0f;
        const glm::vec2 g = xy / cell_ - 0.5f;
        const glm::ivec2 i0(static_cast<int>(std::floor(g.x)), static_cast<int>(std::floor(g.y)));
        const glm::vec2 f = g - glm::vec2(i0);
        auto at = [&](int wi, int wj) {
            return in_window_(wi, wj) ? decode_(get_raw_(wrap_(wi), wrap_(wj))) : 0.0f;
        };
        const float a = at(i0.x, i0.y), b = at(i0.x + 1, i0.y);
        const float c = at(i0.x, i0.y + 1), d = at(i0.x + 1, i0.y + 1);
        return glm::mix(glm::mix(a, b, f.x), glm::mix(c, d, f.x), f.y) * edge_fade(xy);
    }

    /**
     * @brief 1 inside the window, easing to 0 over its outer fifth (Chebyshev distance from the
     *        centre), so a track the window leaves behind fades instead of popping. The shader's
     *        gfx_world_trench() applies the same fade.
     */
    float edge_fade(const glm::vec2& xy) const {
        if (!focused_) return 0.0f;
        const float half_n = 0.5f * static_cast<float>(n_);
        const glm::vec2 rel = glm::abs(xy / cell_ - (glm::vec2(window_) + half_n)) / half_n;
        const float t = std::clamp((std::max(rel.x, rel.y) - 0.8f) / 0.2f, 0.0f, 1.0f);
        return 1.0f - t * t * (3.0f - 2.0f * t);
    }

private:
    int wrap_(int w) const { return ((w % n_) + n_) % n_; }
    bool in_window_(int wi, int wj) const {
        return wi >= window_.x && wj >= window_.y && wi < window_.x + n_ && wj < window_.y + n_;
    }
    uint32_t get_raw_(int ti, int tj) const {
        const size_t idx = static_cast<size_t>(tj) * static_cast<size_t>(n_) + static_cast<size_t>(ti);
        const uint32_t w = words_[idx >> 1];
        return (idx & 1u) == 0u ? (w & 0xFFFFu) : (w >> 16);
    }
    void set_raw_(int ti, int tj, uint32_t v) {
        const size_t idx = static_cast<size_t>(tj) * static_cast<size_t>(n_) + static_cast<size_t>(ti);
        uint32_t& w = words_[idx >> 1];
        w = (idx & 1u) == 0u ? ((w & 0xFFFF0000u) | v) : ((w & 0xFFFFu) | (v << 16));
    }
    uint32_t encode_(float metres) const {
        return static_cast<uint32_t>(std::clamp(metres / scale_, 0.0f, 1.0f) * 65535.0f + 0.5f);
    }
    float decode_(uint32_t v) const { return static_cast<float>(v) / 65535.0f * scale_; }

    int n_;
    float cell_;
    float scale_;
    std::vector<uint32_t> words_;
    glm::ivec2 window_{0};
    bool focused_ = false;
    uint64_t version_ = 1;
};

// ---------------------------------------------------------------------------------------------
// CPU mirrors of the shader's snow tests (gfx/surface/world.glsl, snow_height.glsl)
// ---------------------------------------------------------------------------------------------

/**
 * @brief The precipitation map's occlusion height at xy -- max of the bilinear and the nearest
 *        cell, the fallback plane outside / where nothing was found. As gfx_world_occlusion_height().
 */
/** @brief The heights the sky test reads: the moving-body-free layer when the probe made one. */
inline const std::vector<float>& snow_sky_heights(const particles::GroundField& f) {
    return f.sky_heights.size() == f.heights.size() ? f.sky_heights : f.heights;
}

inline float snow_occlusion_height(const particles::GroundField& f, const glm::vec2& xy) {
    if (f.nx <= 0 || f.ny <= 0) return -1e30f;
    const std::vector<float>& heights = snow_sky_heights(f);
    auto h = [&](int i, int j) {
        if (i < 0 || j < 0 || i >= f.nx || j >= f.ny) return f.fallback;
        const float v = heights[static_cast<size_t>(j) * static_cast<size_t>(f.nx) + static_cast<size_t>(i)];
        return std::isnan(v) ? f.fallback : v;
    };
    const glm::vec2 g = (xy - f.origin) / f.cell - 0.5f;
    const glm::ivec2 i0(static_cast<int>(std::floor(g.x)), static_cast<int>(std::floor(g.y)));
    const glm::vec2 t = g - glm::vec2(i0);
    const float bl = glm::mix(glm::mix(h(i0.x, i0.y), h(i0.x + 1, i0.y), t.x),
                              glm::mix(h(i0.x, i0.y + 1), h(i0.x + 1, i0.y + 1), t.x), t.y);
    const bool outside = g.x < -0.5f || g.y < -0.5f || g.x > static_cast<float>(f.nx) - 0.5f ||
                         g.y > static_cast<float>(f.ny) - 0.5f;
    const float nearest = outside ? f.fallback
                                  : h(std::clamp(static_cast<int>(std::floor(g.x + 0.5f)), 0, f.nx - 1),
                                      std::clamp(static_cast<int>(std::floor(g.y + 0.5f)), 0, f.ny - 1));
    return std::max(bl, nearest);
}

/** @brief 0..1: how open to the sky point p is; `soft` metres of fade. As gfx_world_open_sky(). */
inline float snow_open_sky(const particles::GroundField* f, const glm::vec3& p, float soft) {
    if (!f) return 1.0f;
    const float h = snow_occlusion_height(*f, glm::vec2(p));
    const float a = h - soft, b = h - soft * 0.25f;
    const float t = std::clamp((p.z - a) / std::max(b - a, 1e-6f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// --- Hard-edged patches: CPU mirror of gfx/surface/snow_patches.glsl (keep identical) ---

/** @brief How the lying snow's edge is drawn: the weather's snow_patch_style / snow_patch_size. */
struct SnowStyle {
    bool hard = false;    ///< Round, crisp-edged patches; false: the soft noisy cover.
    float size = 1.5f;    ///< Hard patches: metres per pattern cell (typical patch diameter).
};

namespace detail {
inline float snow_fract(float x) { return x - std::floor(x); }
/** @brief gfx_snow_hash3_(): three hashes in [0,1) per integer cell. */
inline glm::vec3 snow_hash3(const glm::vec2& c) {
    glm::vec3 p(snow_fract(c.x * 0.1031f), snow_fract(c.y * 0.1030f), snow_fract(c.x * 0.0973f));
    p += glm::dot(p, glm::vec3(p.y, p.z, p.x) + 33.33f);
    const glm::vec3 a(p.x, p.x, p.y), b(p.y, p.z, p.z), m(p.z, p.y, p.x);
    const glm::vec3 r = (a + b) * m;
    return glm::vec3(snow_fract(r.x), snow_fract(r.y), snow_fract(r.z));
}
/** @brief gfx_snow_blob_layer_(). */
inline float snow_blob_layer(const glm::vec2& xy, float size) {
    const glm::vec2 g = xy / size;
    const glm::vec2 c(std::floor(g.x), std::floor(g.y));
    float sum = 0.0f;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            const glm::vec2 cell = c + glm::vec2(static_cast<float>(i), static_cast<float>(j));
            const glm::vec3 h = snow_hash3(cell);
            const glm::vec2 centre = cell + 0.15f + 0.7f * glm::vec2(h.x, h.y);
            const float r = glm::mix(0.35f, 1.0f, h.z);
            const float d = glm::length(g - centre) / r;
            const float q = std::max(1.0f - d * d, 0.0f);
            sum += q * q;
        }
    }
    return sum;
}
} // namespace detail

/** @brief gfx_snow_blobs(): the metaball field of the hard-patch pattern. */
inline float snow_blobs(const glm::vec2& xy, float size) {
    return detail::snow_blob_layer(xy, size) + 0.3f * detail::snow_blob_layer(xy + glm::vec2(17.3f, 41.9f), size * 0.45f);
}

/** @brief gfx_snow_patch_threshold(): the field level a patch starts at for a cover 0..1. */
inline float snow_patch_threshold(float cover) {
    return 1.25f - 1.7f * cover + 1.4f * std::pow(1.0f - cover, 12.0f);
}

/** @brief gfx_snow_patch_value(): > 0 inside a hard patch. `receptive` = up-facing x open sky. */
inline float snow_patch_value(const glm::vec2& xy, float cover, float receptive, float size) {
    return snow_blobs(xy, size) - snow_patch_threshold(cover) - (1.0f - receptive) * 1.5f;
}

/** @brief gfx_snow_patch_ramp(): the patch mask with its short ramp (what the deep snow lies in). */
inline float snow_patch_mask(const glm::vec2& xy, float cover, float receptive, float size) {
    // gfx_snow_patch_saturated(): deep inside a patch everywhere -- skip the pattern.
    if (-snow_patch_threshold(cover) - (1.0f - receptive) * 1.5f >= 0.35f) return 1.0f;
    const float t = std::clamp(snow_patch_value(xy, cover, receptive, size) / 0.35f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/**
 * @brief The deep-snow height above a `snow`-shaded surface at world p (the `snow` surface
 *        shader's displacement, gfx/surface/snow_height.glsl): depth * cover * open sky (times
 *        the patch mask under the hard style), less the trench, never negative.
 */
inline float deep_snow_depth(const glm::vec3& p, float max_depth, float cover,
                             const particles::GroundField* occlusion, const SnowField* trenches,
                             const SnowStyle& style = {}) {
    if (max_depth <= 0.0f || cover <= 0.0f) return 0.0f;
    const float open = snow_open_sky(occlusion, p + glm::vec3(0.0f, 0.0f, 0.35f + 0.35f * (occlusion ? occlusion->cell : 1.0f)), 0.5f);
    const float patch_mask = style.hard ? snow_patch_mask(glm::vec2(p), cover, open, std::max(style.size, 0.1f)) : 1.0f;
    const float trench = trenches ? trenches->trench_at(glm::vec2(p)) : 0.0f;
    return std::max(0.0f, max_depth * cover * open * patch_mask - trench);
}

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_SNOW_FIELD_H
