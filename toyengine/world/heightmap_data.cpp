#include <toyengine/world/heightmap_data.h>

#include <atomic>
#include <cmath>
#include <limits>

namespace toy {
namespace world {

namespace {

std::uint32_t hash_u32(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/** @brief Pseudo-random gradient direction at an integer lattice point. */
glm::vec2 lattice_gradient(int ix, int iy, std::uint32_t seed) {
    const std::uint32_t h = hash_u32(static_cast<std::uint32_t>(ix) * 0x8da6b343U ^
                                     static_cast<std::uint32_t>(iy) * 0xd8163841U ^ seed * 0xcb1ab31fU);
    const float a = static_cast<float>(h) * (6.28318530718f / 4294967296.0f);
    return glm::vec2(std::cos(a), std::sin(a));
}

/** @brief 2D gradient noise in about [-0.7, 0.7]. */
float gradient_noise(float x, float y, std::uint32_t seed) {
    const float fx = std::floor(x), fy = std::floor(y);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
    const float tx = x - fx, ty = y - fy;
    auto dot_at = [&](int ox, int oy) {
        const glm::vec2 g = lattice_gradient(ix + ox, iy + oy, seed);
        return g.x * (tx - static_cast<float>(ox)) + g.y * (ty - static_cast<float>(oy));
    };
    auto fade = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
    const float u = fade(tx), v = fade(ty);
    const float a = dot_at(0, 0) + (dot_at(1, 0) - dot_at(0, 0)) * u;
    const float b = dot_at(0, 1) + (dot_at(1, 1) - dot_at(0, 1)) * u;
    return a + (b - a) * v;
}

/** @brief Bilinear resample of a single-channel float grid. */
std::vector<float> resample_grid(const std::vector<float>& src, int sw, int sh, int dw, int dh) {
    std::vector<float> out(static_cast<std::size_t>(dw) * dh);
    for (int j = 0; j < dh; ++j) {
        const float v = dh > 1 ? static_cast<float>(j) * static_cast<float>(sh - 1) / static_cast<float>(dh - 1) : 0.0f;
        const int j0 = std::min(static_cast<int>(v), sh - 1);
        const int j1 = std::min(j0 + 1, sh - 1);
        const float fy = v - static_cast<float>(j0);
        for (int i = 0; i < dw; ++i) {
            const float u = dw > 1 ? static_cast<float>(i) * static_cast<float>(sw - 1) / static_cast<float>(dw - 1) : 0.0f;
            const int i0 = std::min(static_cast<int>(u), sw - 1);
            const int i1 = std::min(i0 + 1, sw - 1);
            const float fx = u - static_cast<float>(i0);
            const float a = src[static_cast<std::size_t>(j0) * sw + i0] * (1 - fx) + src[static_cast<std::size_t>(j0) * sw + i1] * fx;
            const float b = src[static_cast<std::size_t>(j1) * sw + i0] * (1 - fx) + src[static_cast<std::size_t>(j1) * sw + i1] * fx;
            out[static_cast<std::size_t>(j) * dw + i] = a + (b - a) * fy;
        }
    }
    return out;
}

/** @brief Bilinear resample of an RGBA8 image, texel-centred (splats cover areas, not points). */
std::vector<std::uint8_t> resample_rgba(const std::vector<std::uint8_t>& src, int sw, int sh, int dw, int dh) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(dw) * dh * 4);
    for (int j = 0; j < dh; ++j) {
        const float v = std::clamp((static_cast<float>(j) + 0.5f) * static_cast<float>(sh) / static_cast<float>(dh) - 0.5f,
                                   0.0f, static_cast<float>(sh - 1));
        const int j0 = static_cast<int>(v), j1 = std::min(j0 + 1, sh - 1);
        const float fy = v - static_cast<float>(j0);
        for (int i = 0; i < dw; ++i) {
            const float u = std::clamp((static_cast<float>(i) + 0.5f) * static_cast<float>(sw) / static_cast<float>(dw) - 0.5f,
                                       0.0f, static_cast<float>(sw - 1));
            const int i0 = static_cast<int>(u), i1 = std::min(i0 + 1, sw - 1);
            const float fx = u - static_cast<float>(i0);
            for (int c = 0; c < 4; ++c) {
                auto s = [&](int x, int y) {
                    return static_cast<float>(src[(static_cast<std::size_t>(y) * sw + x) * 4 + c]);
                };
                const float a = s(i0, j0) * (1 - fx) + s(i1, j0) * fx;
                const float b = s(i0, j1) * (1 - fx) + s(i1, j1) * fx;
                out[(static_cast<std::size_t>(j) * dw + i) * 4 + c] =
                    static_cast<std::uint8_t>(std::clamp(a + (b - a) * fy + 0.5f, 0.0f, 255.0f));
            }
        }
    }
    return out;
}

} // namespace

float heightmap_fbm(float x, float y, std::uint32_t seed, int octaves, float ridges) {
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    float fx = x, fy = y;
    for (int o = 0; o < std::max(1, octaves); ++o) {
        const float n = gradient_noise(fx, fy, seed + static_cast<std::uint32_t>(o) * 1013u) * 1.4f;
        const float ridged = 1.0f - 2.0f * std::abs(n); // peaks along the zero crossings
        sum += amp * (n + (ridged - n) * ridges);
        norm += amp;
        amp *= 0.5f;
        // Rotate a little each octave so lattice artefacts don't line up.
        const float rx = fx * 1.6f - fy * 1.2f, ry = fx * 1.2f + fy * 1.6f;
        fx = rx + 17.3f; fy = ry - 9.1f;
    }
    return sum / norm;
}

std::uint64_t HeightmapData::next_uid_() {
    static std::atomic<std::uint64_t> counter{1};
    return counter.fetch_add(1);
}

HeightmapGeometry HeightmapGeometry::sanitized() const {
    HeightmapGeometry g = *this;
    g.size.x = std::max(g.size.x, 1.0f);
    g.size.y = std::max(g.size.y, 1.0f);
    g.height = std::max(g.height, 0.01f);
    g.chunk_size = std::clamp(g.chunk_size, 4.0f, std::max(g.size.x, g.size.y));
    g.resolution = std::clamp(g.resolution, 3, 8193);
    g.splat_resolution = std::clamp(g.splat_resolution, 4, 8192);
    return g;
}

glm::ivec2 HeightmapGeometry::chunk_count() const {
    return glm::ivec2(std::max(1, static_cast<int>(std::ceil(size.x / chunk_size - 1e-4f))),
                      std::max(1, static_cast<int>(std::ceil(size.y / chunk_size - 1e-4f))));
}

void HeightmapData::reset(const HeightmapGeometry& geometry) {
    geometry_ = geometry.sanitized();
    heights_.assign(static_cast<std::size_t>(nx()) * ny(), 0.0f);
    for (auto& s : splat_) s.assign(static_cast<std::size_t>(splat_w()) * splat_h() * 4, 0);
    const glm::ivec2 cc = geometry_.chunk_count();
    chunk_revisions_.assign(static_cast<std::size_t>(cc.x) * cc.y, 0);
    fill_layer(0);
    mark_all_dirty();
}

void HeightmapData::set_geometry(const HeightmapGeometry& geometry_in) {
    const HeightmapGeometry geometry = geometry_in.sanitized();
    if (geometry == geometry_) return;
    const HeightmapGeometry old = geometry_;

    if (geometry.resolution != old.resolution) {
        heights_ = resample_grid(heights_, old.resolution, old.resolution, geometry.resolution, geometry.resolution);
    }
    if (geometry.height != old.height) {
        const float k = geometry.height / old.height;
        for (float& h : heights_) h *= k;
    }
    if (geometry.splat_resolution != old.splat_resolution) {
        for (auto& s : splat_) {
            s = resample_rgba(s, old.splat_resolution, old.splat_resolution, geometry.splat_resolution,
                              geometry.splat_resolution);
        }
    }
    geometry_ = geometry;
    const glm::ivec2 cc = geometry_.chunk_count();
    chunk_revisions_.assign(static_cast<std::size_t>(cc.x) * cc.y, 0);
    mark_all_dirty();
}

void HeightmapData::assign_content(const HeightmapData& src) {
    const std::uint64_t rev = std::max(revision_, src.revision_);
    const std::uint64_t hrev = std::max(height_revision_, src.height_revision_);
    geometry_ = src.geometry_;
    heights_ = src.heights_;
    splat_ = src.splat_;
    revision_ = rev;
    height_revision_ = hrev;
    const glm::ivec2 cc = geometry_.chunk_count();
    chunk_revisions_.assign(static_cast<std::size_t>(cc.x) * cc.y, 0);
    mark_all_dirty();
}

float HeightmapData::sample_height(glm::vec2 local) const {
    const glm::vec2 t = glm::clamp(to_texel(local), glm::vec2(0.0f),
                                   glm::vec2(static_cast<float>(nx() - 1), static_cast<float>(ny() - 1)));
    const int i0 = std::min(static_cast<int>(t.x), nx() - 2);
    const int j0 = std::min(static_cast<int>(t.y), ny() - 2);
    const float fx = t.x - static_cast<float>(i0), fy = t.y - static_cast<float>(j0);
    const float a = at(i0, j0) + (at(i0 + 1, j0) - at(i0, j0)) * fx;
    const float b = at(i0, j0 + 1) + (at(i0 + 1, j0 + 1) - at(i0, j0 + 1)) * fx;
    return a + (b - a) * fy;
}

glm::vec3 HeightmapData::normal(glm::vec2 local) const {
    const glm::vec2 c = cell();
    const float hl = sample_height(local - glm::vec2(c.x, 0.0f));
    const float hr = sample_height(local + glm::vec2(c.x, 0.0f));
    const float hd = sample_height(local - glm::vec2(0.0f, c.y));
    const float hu = sample_height(local + glm::vec2(0.0f, c.y));
    return glm::normalize(glm::vec3((hl - hr) / (2.0f * c.x), (hd - hu) / (2.0f * c.y), 1.0f));
}

std::array<float, k_heightmap_layer_count> HeightmapData::layer_weights(glm::vec2 local) const {
    std::array<float, k_heightmap_layer_count> w{};
    const glm::vec2 uv = local / geometry_.size;
    const float u = std::clamp(uv.x * static_cast<float>(splat_w()) - 0.5f, 0.0f, static_cast<float>(splat_w() - 1));
    const float v = std::clamp(uv.y * static_cast<float>(splat_h()) - 0.5f, 0.0f, static_cast<float>(splat_h() - 1));
    const int i0 = static_cast<int>(u), j0 = static_cast<int>(v);
    const int i1 = std::min(i0 + 1, splat_w() - 1), j1 = std::min(j0 + 1, splat_h() - 1);
    const float fx = u - static_cast<float>(i0), fy = v - static_cast<float>(j0);
    float total = 0.0f;
    for (int l = 0; l < k_heightmap_layer_count; ++l) {
        const float a = splat_at(l, i0, j0) * (1 - fx) + splat_at(l, i1, j0) * fx;
        const float b = splat_at(l, i0, j1) * (1 - fx) + splat_at(l, i1, j1) * fx;
        w[static_cast<std::size_t>(l)] = a + (b - a) * fy;
        total += w[static_cast<std::size_t>(l)];
    }
    if (total <= 0.0f) {
        w[0] = 1.0f;
        return w;
    }
    for (float& x : w) x /= total;
    return w;
}

float HeightmapData::layer_weight(int layer, glm::vec2 local) const {
    if (layer < 0 || layer >= k_heightmap_layer_count) return 0.0f;
    return layer_weights(local)[static_cast<std::size_t>(layer)];
}

glm::vec2 HeightmapData::minmax(const IRect& r_in) const {
    const IRect r = IRect{r_in.x0, r_in.y0, r_in.x1, r_in.y1}.clipped(nx(), ny());
    if (r.empty()) return glm::vec2(0.0f);
    float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
    for (int j = r.y0; j < r.y1; ++j) {
        for (int i = r.x0; i < r.x1; ++i) {
            const float h = at(i, j);
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
    }
    return glm::vec2(lo, hi);
}

IRect HeightmapData::chunk_texels(int cx, int cy) const {
    const glm::vec2 c = cell();
    const float cs = geometry_.chunk_size;
    IRect r;
    r.x0 = static_cast<int>(std::floor(static_cast<float>(cx) * cs / c.x));
    r.y0 = static_cast<int>(std::floor(static_cast<float>(cy) * cs / c.y));
    r.x1 = static_cast<int>(std::ceil(static_cast<float>(cx + 1) * cs / c.x)) + 1;
    r.y1 = static_cast<int>(std::ceil(static_cast<float>(cy + 1) * cs / c.y)) + 1;
    return r.clipped(nx(), ny());
}

glm::vec2 HeightmapData::chunk_minmax(int cx, int cy) const { return minmax(chunk_texels(cx, cy)); }

glm::vec2 HeightmapData::height_range() const {
    if (range_revision_ != height_revision_) {
        range_ = minmax(IRect{0, 0, nx(), ny()});
        range_revision_ = height_revision_;
    }
    return range_;
}

bool HeightmapData::raycast(glm::vec3 origin, glm::vec3 dir, float max_t, float& t_out) const {
    // Clip the ray to the terrain's box first, so a camera far outside doesn't march empty air.
    const glm::vec2 range = height_range();
    const glm::vec3 lo(0.0f, 0.0f, range.x - 0.01f), hi(geometry_.size, range.y + 0.01f);
    float t0 = 0.0f, t1 = max_t;
    for (int a = 0; a < 3; ++a) {
        if (std::abs(dir[a]) < 1e-12f) {
            if (origin[a] < lo[a] || origin[a] > hi[a]) return false;
            continue;
        }
        float ta = (lo[a] - origin[a]) / dir[a], tb = (hi[a] - origin[a]) / dir[a];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    const float len = glm::length(dir);
    if (len <= 0.0f) return false;
    const float step = 0.5f * std::min(cell().x, cell().y) / len;
    auto above = [&](float t) {
        const glm::vec3 p = origin + dir * t;
        return p.z - sample_height(glm::vec2(p)) ;
    };
    float prev_t = t0;
    float prev = above(t0);
    if (prev < 0.0f) { t_out = t0; return true; }
    for (float t = t0 + step;; t += step) {
        const bool last = t >= t1;
        if (last) t = t1;
        const float d = above(t);
        if (d < 0.0f) {
            float a = prev_t, b = t;
            for (int k = 0; k < 24; ++k) {
                const float m = 0.5f * (a + b);
                if (above(m) < 0.0f) b = m; else a = m;
            }
            t_out = 0.5f * (a + b);
            return true;
        }
        prev_t = t;
        prev = d;
        if (last) break;
    }
    return false;
}

void HeightmapData::bump_chunks_(const glm::vec2& lo, const glm::vec2& hi) {
    ++revision_;
    unsaved_ = true;
    const glm::ivec2 cc = geometry_.chunk_count();
    const float cs = geometry_.chunk_size;
    const int cx0 = std::clamp(static_cast<int>(std::floor(lo.x / cs)), 0, cc.x - 1);
    const int cy0 = std::clamp(static_cast<int>(std::floor(lo.y / cs)), 0, cc.y - 1);
    const int cx1 = std::clamp(static_cast<int>(std::floor(hi.x / cs)), 0, cc.x - 1);
    const int cy1 = std::clamp(static_cast<int>(std::floor(hi.y / cs)), 0, cc.y - 1);
    for (int cy = cy0; cy <= cy1; ++cy) {
        for (int cx = cx0; cx <= cx1; ++cx) chunk_revisions_[static_cast<std::size_t>(cy) * cc.x + cx] = revision_;
    }
}

void HeightmapData::journal_(std::vector<JournalEntry>& journal, std::uint64_t& floor, const IRect& rect) {
    // Consecutive dabs of one stroke usually overlap: merge into the last entry while it stays
    // small, so a long stroke is a handful of entries rather than thousands.
    if (!journal.empty()) {
        IRect merged = journal.back().rect;
        merged.include(rect);
        if (static_cast<long long>(merged.width()) * merged.height() <= 512LL * 512LL) {
            journal.back() = {revision_, merged};
            return;
        }
    }
    journal.push_back({revision_, rect});
    constexpr std::size_t k_max_entries = 256;
    if (journal.size() > k_max_entries) {
        floor = journal.front().revision;
        journal.erase(journal.begin());
    }
}

IRect HeightmapData::dirty_since_(const std::vector<JournalEntry>& journal, std::uint64_t floor, std::uint64_t since,
                                  const IRect& full) {
    if (since < floor) return full;
    IRect r;
    for (auto it = journal.rbegin(); it != journal.rend() && it->revision > since; ++it) r.include(it->rect);
    return r;
}

IRect HeightmapData::height_dirty_since(std::uint64_t since) const {
    return dirty_since_(height_journal_, height_floor_, since, IRect{0, 0, nx(), ny()});
}

IRect HeightmapData::splat_dirty_since(std::uint64_t since) const {
    return dirty_since_(splat_journal_, splat_floor_, since, IRect{0, 0, splat_w(), splat_h()});
}

void HeightmapData::mark_height_dirty(const IRect& texels) {
    const IRect r = texels.clipped(nx(), ny());
    if (r.empty()) return;
    const glm::vec2 c = cell();
    // A texel on a chunk border belongs to both chunks: widen by one so both bump.
    bump_chunks_(glm::vec2(static_cast<float>(r.x0 - 1), static_cast<float>(r.y0 - 1)) * c,
                 glm::vec2(static_cast<float>(r.x1), static_cast<float>(r.y1)) * c);
    ++height_revision_;
    journal_(height_journal_, height_floor_, r);
}

void HeightmapData::mark_splat_dirty(const IRect& texels) {
    const IRect r = texels.clipped(splat_w(), splat_h());
    if (r.empty()) return;
    const glm::vec2 st = geometry_.size / glm::vec2(static_cast<float>(splat_w()), static_cast<float>(splat_h()));
    bump_chunks_(glm::vec2(static_cast<float>(r.x0 - 1), static_cast<float>(r.y0 - 1)) * st,
                 glm::vec2(static_cast<float>(r.x1 + 1), static_cast<float>(r.y1 + 1)) * st);
    journal_(splat_journal_, splat_floor_, r);
}

void HeightmapData::mark_all_dirty() {
    bump_chunks_(glm::vec2(0.0f), geometry_.size);
    ++height_revision_;
    // Everything changed (or the shape did): no consumer can patch from an older revision.
    height_journal_.clear();
    splat_journal_.clear();
    height_floor_ = revision_;
    splat_floor_ = revision_;
}

std::uint64_t HeightmapData::chunk_revision(int cx, int cy) const {
    const glm::ivec2 cc = geometry_.chunk_count();
    if (cx < 0 || cy < 0 || cx >= cc.x || cy >= cc.y) return 0;
    return chunk_revisions_[static_cast<std::size_t>(cy) * cc.x + cx];
}

void HeightmapData::generate(const HeightmapGenerate& p) {
    const float inv_feature = 1.0f / std::max(p.feature_size, 1.0f);
    const glm::vec2 c = cell();
    for (int j = 0; j < ny(); ++j) {
        for (int i = 0; i < nx(); ++i) {
            const glm::vec2 local(static_cast<float>(i) * c.x, static_cast<float>(j) * c.y);
            const float n = heightmap_fbm(local.x * inv_feature, local.y * inv_feature, p.seed, p.octaves, p.ridges);
            float h = p.base + p.amplitude * (0.5f + 0.5f * n);
            if (p.island) {
                const glm::vec2 d = (local / geometry_.size) * 2.0f - 1.0f;
                const float r = std::min(1.0f, glm::length(d));
                const float fade = 1.0f - r * r * (3.0f - 2.0f * r);
                h = p.base * 0.25f + (h - p.base * 0.25f) * fade;
            }
            at(i, j) = std::clamp(h, 0.0f, 1.0f) * geometry_.height;
        }
    }
    mark_all_dirty();
}

void HeightmapData::fill_layer(int layer) {
    layer = std::clamp(layer, 0, k_heightmap_layer_count - 1);
    for (auto& s : splat_) std::fill(s.begin(), s.end(), std::uint8_t{0});
    for (int j = 0; j < splat_h(); ++j) {
        for (int i = 0; i < splat_w(); ++i) splat_at(layer, i, j) = 255;
    }
    mark_splat_dirty(IRect{0, 0, splat_w(), splat_h()});
}

void HeightmapData::normalize_splat_texel(int i, int j, int keep) {
    int total = 0;
    for (int l = 0; l < k_heightmap_layer_count; ++l) total += splat_at(l, i, j);
    if (total == 255) return;
    if (total == 0) {
        splat_at(keep >= 0 ? keep : 0, i, j) = 255;
        return;
    }
    // Scale the others to fit around `keep`, then hand the rounding remainder to the largest.
    const int fixed = keep >= 0 ? std::min<int>(255, splat_at(keep, i, j)) : 0;
    const int others = total - fixed;
    const int room = 255 - fixed;
    int sum = fixed;
    int largest = keep >= 0 ? keep : 0;
    for (int l = 0; l < k_heightmap_layer_count; ++l) {
        if (l == keep) continue;
        int v = others > 0 ? (splat_at(l, i, j) * room + others / 2) / others : 0;
        splat_at(l, i, j) = static_cast<std::uint8_t>(v);
        sum += v;
        if (splat_at(l, i, j) > splat_at(largest, i, j)) largest = l;
    }
    if (others == 0 && keep >= 0) {
        splat_at(keep, i, j) = 255;
        return;
    }
    const int fix = 255 - sum;
    splat_at(largest, i, j) = static_cast<std::uint8_t>(std::clamp(splat_at(largest, i, j) + fix, 0, 255));
}

} // namespace world
} // namespace toy
