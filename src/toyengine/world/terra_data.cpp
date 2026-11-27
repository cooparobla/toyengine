#include <toyengine/world/terra_data.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

namespace toy {
namespace world {

namespace {

std::atomic<std::uint64_t> g_next_uid{1};

/// Journal bound: a long session of single-tile edits merges into few entries, and a consumer
/// further behind than this re-reads the whole plane. Same bound TileMapData/HeightmapData used.
constexpr std::size_t k_journal_cap = 256;

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

/** @brief Bilinear resample of an RGBA8 image, texel-centred (layers cover areas, not points). */
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

// ---------------------------------------------------------------------------- geometry

glm::ivec2 TerraGeometry::chunk_count() const {
    const int c = std::max(1, chunk_tiles);
    return glm::ivec2(std::max(1, (tiles_x + c - 1) / c), std::max(1, (tiles_y + c - 1) / c));
}

TerraGeometry TerraGeometry::sanitized() const {
    TerraGeometry g = *this;
    g.tiles_x = std::max(0, g.tiles_x);
    g.tiles_y = std::max(0, g.tiles_y);
    g.tile_size = std::max(0.01f, g.tile_size);
    g.height_sub = std::clamp(g.height_sub, 1, 64);
    g.layer_sub = std::clamp(g.layer_sub, 1, 64);
    g.height_step = std::max(0.001f, g.height_step);
    g.height = std::max(0.01f, g.height);
    g.chunk_tiles = std::clamp(g.chunk_tiles, 1, 512);
    return g;
}

// ---------------------------------------------------------------------------- TileCell

bool TileCell::operator==(const TileCell& o) const { return std::memcmp(this, &o, sizeof(TileCell)) == 0; }

// ---------------------------------------------------------------------------- construction / shape

std::uint64_t TerraData::next_uid_() { return g_next_uid.fetch_add(1); }

TerraData::TerraData() : uid_(next_uid_()) { reset(TerraGeometry{}); }
TerraData::TerraData(const TerraGeometry& geometry) : uid_(next_uid_()) { reset(geometry); }

void TerraData::reset(const TerraGeometry& geometry) {
    geometry_ = geometry.sanitized();
    tiles_.assign(static_cast<std::size_t>(geometry_.tiles_x) * geometry_.tiles_y, TileCell{});
    spill_offsets_.clear();
    spill_.clear();
    if (geometry_.has_height_plane()) {
        heights_.assign(static_cast<std::size_t>(height_nx()) * height_ny(), 0.0f);
    } else {
        heights_.clear();
    }
    for (auto& l : layers_) l.assign(static_cast<std::size_t>(layer_nx()) * layer_ny() * 4, 0);
    const glm::ivec2 cc = geometry_.chunk_count();
    chunk_revisions_.assign(static_cast<std::size_t>(cc.x) * cc.y, 0);
    fill_layer(0);
    needs_bake_ = true;
    mark_all_dirty();
}

void TerraData::reshape(const TerraGeometry& geometry_in) {
    const TerraGeometry geometry = geometry_in.sanitized();
    if (geometry == geometry_) return;
    const TerraGeometry old = geometry_;

    // The spill list indexes the OLD grid, and a reshape renumbers every tile: rather than
    // remap it, drop it and clear the flag that points at it, so a reshaped world is a correct
    // world with one roof per column rather than one whose indices point at the wrong rock.
    // Reshaping a world with caves in it is re-baking it anyway.
    if (!spill_.empty()) {
        for (TileCell& cell : tiles_) cell.set(TileCell::Spilled, false);
        spill_offsets_.clear();
        spill_.clear();
    }

    // Tiles: kept where both grids still have the cell, defaulted past whichever shrank.
    std::vector<TileCell> new_tiles(static_cast<std::size_t>(geometry.tiles_x) * geometry.tiles_y, TileCell{});
    const int kx = std::min(old.tiles_x, geometry.tiles_x), ky = std::min(old.tiles_y, geometry.tiles_y);
    for (int y = 0; y < ky; ++y) {
        for (int x = 0; x < kx; ++x) {
            new_tiles[static_cast<std::size_t>(y) * geometry.tiles_x + x] = tiles_[static_cast<std::size_t>(y) * old.tiles_x + x];
        }
    }
    tiles_ = std::move(new_tiles);

    // Height: resample to the new sample counts, then rescale if the vertical scale changed.
    const int old_hnx = old.height_nx(), old_hny = old.height_ny();
    const int new_hnx = geometry.height_nx(), new_hny = geometry.height_ny();
    if (!old.has_height_plane() && geometry.has_height_plane()) {
        // Raising height_sub from 1: there is no plane to resample from yet, so expand every
        // tile flat at its own steps -- the lossless direction (a flat tile has no cliff to
        // reconcile; see terrain_field_test.cpp's Stage 0 history for why a cliff is not).
        heights_.assign(static_cast<std::size_t>(new_hnx) * new_hny, 0.0f);
        for (int ty = 0; ty < geometry.tiles_y; ++ty) {
            for (int tx = 0; tx < geometry.tiles_x; ++tx) {
                const float v = (tx < kx && ty < ky ? tiles_[static_cast<std::size_t>(ty) * geometry.tiles_x + tx].steps : 0) *
                               geometry.height_step;
                for (int dy = 0; dy <= geometry.height_sub; ++dy) {
                    for (int dx = 0; dx <= geometry.height_sub; ++dx) {
                        heights_[static_cast<std::size_t>(ty * geometry.height_sub + dy) * new_hnx + (tx * geometry.height_sub + dx)] = v;
                    }
                }
            }
        }
    } else if (old.has_height_plane() && !geometry.has_height_plane()) {
        heights_.clear();
    } else if (old.has_height_plane()) {
        if (new_hnx != old_hnx || new_hny != old_hny) heights_ = resample_grid(heights_, old_hnx, old_hny, new_hnx, new_hny);
        if (geometry.height != old.height) {
            const float k = geometry.height / old.height;
            for (float& h : heights_) h *= k;
        }
    }

    // Layers: resample to the new texel counts.
    const int old_lnx = old.layer_nx(), old_lny = old.layer_ny();
    const int new_lnx = geometry.layer_nx(), new_lny = geometry.layer_ny();
    if (new_lnx != old_lnx || new_lny != old_lny) {
        for (auto& l : layers_) l = resample_rgba(l, old_lnx, old_lny, new_lnx, new_lny);
    }

    geometry_ = geometry;
    const glm::ivec2 cc = geometry_.chunk_count();
    chunk_revisions_.assign(static_cast<std::size_t>(cc.x) * cc.y, 0);
    if (geometry_.has_height_plane()) refresh_steps_(IRect{0, 0, geometry_.tiles_x, geometry_.tiles_y});
    mark_all_dirty();
}

void TerraData::assign_content(const TerraData& src) {
    const std::uint64_t tr = std::max(tiles_revision_, src.tiles_revision_);
    const std::uint64_t hr = std::max(height_revision_, src.height_revision_);
    const std::uint64_t lr = std::max(layer_revision_, src.layer_revision_);
    geometry_ = src.geometry_;
    tiles_ = src.tiles_;
    heights_ = src.heights_;
    layers_ = src.layers_;
    // The spill list travels with the tile plane it indexes. Forgetting it here leaves a loaded
    // world with its hills intact and every column's lower storeys gone -- which looks exactly
    // like a world that never had them.
    spill_offsets_ = src.spill_offsets_;
    spill_ = src.spill_;
    tiles_revision_ = tr;
    height_revision_ = hr;
    layer_revision_ = lr;
    // The bake handshake travels with the content, not with this object's identity: a restored
    // shape that was never baked must still say so, and one loaded from a file must not.
    needs_bake_ = src.needs_bake_;
    bake_requests_ = src.bake_requests_;
    bakes_done_ = src.bakes_done_;
    const glm::ivec2 cc = geometry_.chunk_count();
    chunk_revisions_.assign(static_cast<std::size_t>(cc.x) * cc.y, 0);
    mark_all_dirty();
}

bool TerraData::contains(glm::vec2 local) const {
    const glm::vec2 s = geometry_.size();
    return local.x >= 0.0f && local.y >= 0.0f && local.x <= s.x && local.y <= s.y;
}

glm::ivec2 TerraData::tile_at(glm::vec2 local) const {
    return glm::ivec2(static_cast<int>(std::floor(local.x / geometry_.tile_size)),
                      static_cast<int>(std::floor(local.y / geometry_.tile_size)));
}

glm::vec2 TerraData::tile_centre(glm::ivec2 tile) const {
    return (glm::vec2(tile) + 0.5f) * geometry_.tile_size;
}

// ---------------------------------------------------------------------------- tile plane

const TileCell& TerraData::get(int tx, int ty) const {
    static const TileCell k_empty{};
    if (empty()) return k_empty;
    return tiles_[index_(std::clamp(tx, 0, geometry_.tiles_x - 1), std::clamp(ty, 0, geometry_.tiles_y - 1))];
}

std::vector<TileCell> TerraData::capture(const IRect& rect) const {
    std::vector<TileCell> out;
    const IRect r = rect.clipped(geometry_.tiles_x, geometry_.tiles_y);
    out.reserve(static_cast<std::size_t>(r.width()) * static_cast<std::size_t>(r.height()));
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) out.push_back(at(x, y));
    return out;
}

void TerraData::restore(const IRect& rect, const std::vector<TileCell>& cells) {
    const IRect r = rect.clipped(geometry_.tiles_x, geometry_.tiles_y);
    if (r.empty() || cells.size() != static_cast<std::size_t>(r.width()) * static_cast<std::size_t>(r.height())) return;
    std::size_t i = 0;
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) at(x, y) = cells[i++];
    mark_tiles_dirty(r);
}

// ---------------------------------------------------------------------------- ground

float TerraData::bilinear_height_(glm::vec2 local) const {
    const int nx = height_nx(), ny = height_ny();
    const glm::vec2 t = glm::clamp(to_texel(local), glm::vec2(0.0f),
                                   glm::vec2(static_cast<float>(nx - 1), static_cast<float>(ny - 1)));
    const int i0 = std::min(static_cast<int>(t.x), nx - 2);
    const int j0 = std::min(static_cast<int>(t.y), ny - 2);
    const float fx = t.x - static_cast<float>(i0), fy = t.y - static_cast<float>(j0);
    const float a = height_at(i0, j0) + (height_at(i0 + 1, j0) - height_at(i0, j0)) * fx;
    const float b = height_at(i0, j0 + 1) + (height_at(i0 + 1, j0 + 1) - height_at(i0, j0 + 1)) * fx;
    return a + (b - a) * fy;
}

float TerraData::ground_at(glm::vec2 local) const {
    if (has_heights()) return bilinear_height_(local);
    const glm::ivec2 tile = glm::clamp(tile_at(local), glm::ivec2(0), glm::ivec2(geometry_.tiles_x - 1, geometry_.tiles_y - 1));
    return empty() ? 0.0f : get(tile.x, tile.y).outer_ground_steps() * geometry_.height_step;
}

float TerraData::ground_at_tile(glm::ivec2 tile) const {
    if (empty()) return 0.0f;
    if (has_heights()) {
        const glm::ivec2 t = glm::clamp(tile, glm::ivec2(0), glm::ivec2(geometry_.tiles_x - 1, geometry_.tiles_y - 1));
        return bilinear_height_(tile_centre(t));
    }
    // The OUTERMOST surface. Every caller of this wants the surface the world SHOWS -- the
    // streamer framing a chunk, a brush working on a hillside, a water sheet, foliage scatter --
    // and on a column with a roof over it `ground_steps()` is the floor of the open span
    // underneath, tens of steps down. Something standing INSIDE a cave asks `ground_under()`
    // instead, which takes the height it is at; that question is opt-in precisely so no existing
    // caller changes answer by accident.
    return get(tile.x, tile.y).outer_ground_steps() * geometry_.height_step;
}

glm::vec3 TerraData::normal_at(glm::vec2 local) const {
    if (!has_heights()) return glm::vec3(0.0f, 0.0f, 1.0f);
    const float c = cell();
    const float hl = bilinear_height_(local - glm::vec2(c, 0.0f));
    const float hr = bilinear_height_(local + glm::vec2(c, 0.0f));
    const float hd = bilinear_height_(local - glm::vec2(0.0f, c));
    const float hu = bilinear_height_(local + glm::vec2(0.0f, c));
    return glm::normalize(glm::vec3((hl - hr) / (2.0f * c), (hd - hu) / (2.0f * c), 1.0f));
}

glm::vec2 TerraData::minmax(const IRect& r_in) const {
    const IRect r = IRect{r_in.x0, r_in.y0, r_in.x1, r_in.y1}.clipped(height_nx(), height_ny());
    if (r.empty() || !has_heights()) return glm::vec2(0.0f);
    float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
    for (int j = r.y0; j < r.y1; ++j) {
        for (int i = r.x0; i < r.x1; ++i) {
            const float h = height_at(i, j);
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
    }
    return glm::vec2(lo, hi);
}

glm::vec2 TerraData::chunk_minmax(int cx, int cy) const {
    if (!has_heights()) {
        if (empty()) return glm::vec2(0.0f);
        const int c = std::max(1, geometry_.chunk_tiles);
        const IRect r = IRect{cx * c, cy * c, (cx + 1) * c, (cy + 1) * c}.clipped(geometry_.tiles_x, geometry_.tiles_y);
        if (r.empty()) return glm::vec2(0.0f);
        float lo = std::numeric_limits<float>::max(), hi = -std::numeric_limits<float>::max();
        for (int y = r.y0; y < r.y1; ++y) {
            for (int x = r.x0; x < r.x1; ++x) {
                const float h = at(x, y).ground_steps() * geometry_.height_step;
                lo = std::min(lo, h);
                hi = std::max(hi, h);
            }
        }
        return glm::vec2(lo, hi);
    }
    const int c = std::max(1, geometry_.chunk_tiles), sub = geometry_.height_sub;
    // The shared +1 corner row/column is the chunk's border with its neighbour.
    const IRect texels{cx * c * sub, cy * c * sub, (cx + 1) * c * sub + 1, (cy + 1) * c * sub + 1};
    return minmax(texels);
}

glm::vec2 TerraData::height_range() const {
    if (height_range_revision_ != height_revision_) {
        if (has_heights()) {
            height_range_ = minmax(IRect{0, 0, height_nx(), height_ny()});
        } else if (!empty()) {
            // No float height plane: `steps` IS the height (the terraced read), the same case
            // chunk_minmax() already handles per chunk -- this is that scan over the whole
            // plane, not one call per region even for a multi-region grid: a shaped world's
            // faces are stored as one contiguous TerraData.
            float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
            for (int y = 0; y < geometry_.tiles_y; ++y) {
                for (int x = 0; x < geometry_.tiles_x; ++x) {
                    const float h = at(x, y).ground_steps() * geometry_.height_step;
                    lo = std::min(lo, h);
                    hi = std::max(hi, h);
                }
            }
            height_range_ = glm::vec2(lo, hi);
        } else {
            height_range_ = glm::vec2(0.0f);
        }
        height_range_revision_ = height_revision_;
    }
    return height_range_;
}

// ---------------------------------------------------------------------------- layers

float TerraData::layer_weight(int layer, glm::vec2 local) const {
    if (layer < 0 || layer >= k_terrain_layer_count) return 0.0f;
    if (!has_heights()) {
        const glm::ivec2 tile = glm::clamp(tile_at(local), glm::ivec2(0), glm::ivec2(geometry_.tiles_x - 1, geometry_.tiles_y - 1));
        return !empty() && static_cast<int>(get(tile.x, tile.y).top_kind()) == layer ? 1.0f : 0.0f;
    }
    return layer < k_heightmap_layer_count ? layer_weights(local)[static_cast<std::size_t>(layer)] : 0.0f;
}

std::array<float, k_heightmap_layer_count> TerraData::layer_weights(glm::vec2 local) const {
    std::array<float, k_heightmap_layer_count> w{};
    if (!has_heights()) {
        const glm::ivec2 tile = glm::clamp(tile_at(local), glm::ivec2(0), glm::ivec2(geometry_.tiles_x - 1, geometry_.tiles_y - 1));
        const int layer = empty() ? 0 : static_cast<int>(get(tile.x, tile.y).top_kind());
        if (layer >= 0 && layer < k_heightmap_layer_count) w[static_cast<std::size_t>(layer)] = 1.0f;
        return w;
    }
    const int lnx = layer_nx(), lny = layer_ny();
    const glm::vec2 uv = local / geometry_.size();
    const float u = std::clamp(uv.x * static_cast<float>(lnx) - 0.5f, 0.0f, static_cast<float>(lnx - 1));
    const float v = std::clamp(uv.y * static_cast<float>(lny) - 0.5f, 0.0f, static_cast<float>(lny - 1));
    const int i0 = static_cast<int>(u), j0 = static_cast<int>(v);
    const int i1 = std::min(i0 + 1, lnx - 1), j1 = std::min(j0 + 1, lny - 1);
    const float fx = u - static_cast<float>(i0), fy = v - static_cast<float>(j0);
    float total = 0.0f;
    for (int l = 0; l < k_heightmap_layer_count; ++l) {
        const float a = layer_at(l, i0, j0) * (1 - fx) + layer_at(l, i1, j0) * fx;
        const float b = layer_at(l, i0, j1) * (1 - fx) + layer_at(l, i1, j1) * fx;
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

void TerraData::fill_layer(int layer) {
    layer = std::clamp(layer, 0, k_heightmap_layer_count - 1);
    for (auto& l : layers_) std::fill(l.begin(), l.end(), std::uint8_t{0});
    for (int j = 0; j < layer_ny(); ++j)
        for (int i = 0; i < layer_nx(); ++i) layer_at(layer, i, j) = 255;
    mark_layer_dirty(IRect{0, 0, layer_nx(), layer_ny()});
}

void TerraData::normalize_layer_texel(int i, int j, int keep) {
    int total = 0;
    for (int l = 0; l < k_heightmap_layer_count; ++l) total += layer_at(l, i, j);
    if (total == 255) return;
    if (total == 0) {
        layer_at(keep >= 0 ? keep : 0, i, j) = 255;
        return;
    }
    const int fixed = keep >= 0 ? std::min<int>(255, layer_at(keep, i, j)) : 0;
    const int others = total - fixed;
    const int room = 255 - fixed;
    int sum = fixed;
    int largest = keep >= 0 ? keep : 0;
    for (int l = 0; l < k_heightmap_layer_count; ++l) {
        if (l == keep) continue;
        int v = others > 0 ? (layer_at(l, i, j) * room + others / 2) / others : 0;
        layer_at(l, i, j) = static_cast<std::uint8_t>(v);
        sum += v;
        if (layer_at(l, i, j) > layer_at(largest, i, j)) largest = l;
    }
    if (others == 0 && keep >= 0) {
        layer_at(keep, i, j) = 255;
        return;
    }
    const int fix = 255 - sum;
    layer_at(largest, i, j) = static_cast<std::uint8_t>(std::clamp(layer_at(largest, i, j) + fix, 0, 255));
}

// ---------------------------------------------------------------------------- steps cache

std::int16_t TerraData::derive_steps_(int tx, int ty) const {
    const int sub = geometry_.height_sub;
    const int nx = height_nx();
    double sum = 0.0;
    int n = 0;
    for (int dy = 0; dy <= sub; ++dy) {
        for (int dx = 0; dx <= sub; ++dx) {
            sum += static_cast<double>(heights_[static_cast<std::size_t>(ty * sub + dy) * nx + (tx * sub + dx)]);
            ++n;
        }
    }
    return static_cast<std::int16_t>(std::lround((sum / n) / geometry_.height_step));
}

void TerraData::refresh_steps_(const IRect& tiles) {
    const IRect r = tiles.clipped(geometry_.tiles_x, geometry_.tiles_y);
    for (int y = r.y0; y < r.y1; ++y) {
        for (int x = r.x0; x < r.x1; ++x) {
            TileCell& c = tiles_[index_(x, y)];
            c.steps = derive_steps_(x, y);
            c.ground_sub = 0;   // the float plane IS the height here; there is no remainder to keep
        }
    }
}

// ---------------------------------------------------------------------------- change tracking

void TerraData::bump_render_chunks_(const IRect& tile_rect) {
    ++revision_;
    unsaved_ = true;
    if (tile_rect.empty() || chunk_revisions_.empty()) return;
    const glm::ivec2 cc = geometry_.chunk_count();
    const int c = std::max(1, geometry_.chunk_tiles);
    const int cx0 = std::clamp(tile_rect.x0 / c, 0, cc.x - 1);
    const int cy0 = std::clamp(tile_rect.y0 / c, 0, cc.y - 1);
    const int cx1 = std::clamp((tile_rect.x1 - 1) / c, 0, cc.x - 1);
    const int cy1 = std::clamp((tile_rect.y1 - 1) / c, 0, cc.y - 1);
    for (int cy = cy0; cy <= cy1; ++cy)
        for (int cx = cx0; cx <= cx1; ++cx) chunk_revisions_[static_cast<std::size_t>(cy) * cc.x + cx] = revision_;
}

void TerraData::journal_(std::vector<JournalEntry>& journal, std::uint64_t& floor, std::uint64_t revision,
                         const IRect& rect) {
    if (!journal.empty()) {
        const IRect& last = journal.back().rect;
        const bool touch = rect.x0 <= last.x1 && last.x0 <= rect.x1 && rect.y0 <= last.y1 && last.y0 <= rect.y1;
        IRect merged = last;
        merged.include(rect);
        if (touch && static_cast<long long>(merged.width()) * merged.height() <= 64LL * 64LL) {
            journal.back() = {revision, merged};
            return;
        }
    }
    journal.push_back({revision, rect});
    if (journal.size() > k_journal_cap) {
        floor = journal.front().revision;
        journal.erase(journal.begin());
    }
}

IRect TerraData::dirty_since_(const std::vector<JournalEntry>& journal, std::uint64_t floor, std::uint64_t since,
                              const IRect& full) {
    if (since < floor) return full;
    IRect r;
    for (auto it = journal.rbegin(); it != journal.rend() && it->revision > since; ++it) r.include(it->rect);
    return r;
}

void TerraData::for_each_dirty_since_(const std::vector<JournalEntry>& journal, std::uint64_t floor,
                                      std::uint64_t since, const IRect& full,
                                      const std::function<void(const IRect&)>& fn) {
    if (since >= floor && journal.empty()) return;
    if (since < floor) {
        fn(full);
        return;
    }
    for (const JournalEntry& e : journal) {
        if (e.revision > since) fn(e.rect);
    }
}

void TerraData::mark_tiles_dirty(const IRect& tiles) {
    const IRect r = tiles.clipped(geometry_.tiles_x, geometry_.tiles_y);
    if (r.empty()) return;
    ++tiles_revision_;
    journal_(tiles_journal_, tiles_floor_, tiles_revision_, r);
    bump_render_chunks_(r);
}

void TerraData::mark_height_dirty(const IRect& samples) {
    if (!has_heights()) return;
    const IRect r = samples.clipped(height_nx(), height_ny());
    if (r.empty()) return;
    ++height_revision_;
    journal_(height_journal_, height_floor_, height_revision_, r);
    // A sample on a tile corner belongs to the tile below-left and the one above-right.
    const int sub = std::max(1, geometry_.height_sub);
    const IRect tiles = IRect{r.x0 / sub - 1, r.y0 / sub - 1, (r.x1 + sub - 1) / sub + 1, (r.y1 + sub - 1) / sub + 1}
                            .clipped(geometry_.tiles_x, geometry_.tiles_y);
    // `steps` is a maintained cache of this plane (see the file comment): every height write ends
    // here, so this is the one place it is ever recomputed -- no edit path re-derives it itself.
    refresh_steps_(tiles);
    mark_tiles_dirty(tiles);
}

void TerraData::mark_layer_dirty(const IRect& texels) {
    const IRect r = texels.clipped(layer_nx(), layer_ny());
    if (r.empty()) return;
    ++layer_revision_;
    journal_(layer_journal_, layer_floor_, layer_revision_, r);
    const int sub = std::max(1, geometry_.layer_sub);
    const IRect tiles{r.x0 / sub, r.y0 / sub, (r.x1 + sub - 1) / sub, (r.y1 + sub - 1) / sub};
    bump_render_chunks_(tiles.clipped(geometry_.tiles_x, geometry_.tiles_y));
}

void TerraData::mark_all_dirty() {
    ++tiles_revision_;
    ++height_revision_;
    ++layer_revision_;
    ++revision_;
    unsaved_ = true;
    tiles_journal_.clear();
    height_journal_.clear();
    layer_journal_.clear();
    tiles_floor_ = tiles_revision_;
    height_floor_ = height_revision_;
    layer_floor_ = layer_revision_;
    std::fill(chunk_revisions_.begin(), chunk_revisions_.end(), revision_);
}

TerraCursor TerraData::cursor() const { return TerraCursor{tiles_revision_, height_revision_, layer_revision_}; }

std::uint64_t TerraData::chunk_revision(int cx, int cy) const {
    const glm::ivec2 cc = geometry_.chunk_count();
    if (cx < 0 || cy < 0 || cx >= cc.x || cy >= cc.y || chunk_revisions_.empty()) return 0;
    return chunk_revisions_[static_cast<std::size_t>(cy) * cc.x + cx];
}

namespace {
/** @brief A height- or layer-texel rect, mapped onto the tiles it covers. */
IRect tiles_of_height_texels(const TerraGeometry& g, const IRect& texels) {
    if (texels.empty()) return IRect{};
    const int sub = std::max(1, g.height_sub);
    return IRect{texels.x0 / sub - 1, texels.y0 / sub - 1, (texels.x1 + sub - 1) / sub + 1, (texels.y1 + sub - 1) / sub + 1}
        .clipped(g.tiles_x, g.tiles_y);
}
IRect tiles_of_layer_texels(const TerraGeometry& g, const IRect& texels) {
    if (texels.empty()) return IRect{};
    const int sub = std::max(1, g.layer_sub);
    return IRect{texels.x0 / sub, texels.y0 / sub, (texels.x1 + sub - 1) / sub, (texels.y1 + sub - 1) / sub}
        .clipped(g.tiles_x, g.tiles_y);
}
} // namespace

IRect TerraData::dirty_since(const TerraCursor& since) const {
    IRect out = dirty_since_(tiles_journal_, tiles_floor_, since.tiles, IRect{0, 0, geometry_.tiles_x, geometry_.tiles_y});
    if (has_heights()) {
        out.include(tiles_of_height_texels(geometry_, height_dirty_since(since.heights)));
    }
    out.include(tiles_of_layer_texels(geometry_, layer_dirty_since(since.layers)));
    return out;
}

void TerraData::for_each_dirty_since(const TerraCursor& since, const std::function<void(const IRect&)>& fn) const {
    for_each_dirty_since_(tiles_journal_, tiles_floor_, since.tiles, IRect{0, 0, geometry_.tiles_x, geometry_.tiles_y}, fn);
    if (has_heights()) {
        for_each_dirty_since_(height_journal_, height_floor_, since.heights, IRect{0, 0, height_nx(), height_ny()},
                              [&](const IRect& r) { fn(tiles_of_height_texels(geometry_, r)); });
    }
    for_each_dirty_since_(layer_journal_, layer_floor_, since.layers, IRect{0, 0, layer_nx(), layer_ny()},
                          [&](const IRect& r) { fn(tiles_of_layer_texels(geometry_, r)); });
}

IRect TerraData::height_dirty_since(std::uint64_t since) const {
    return dirty_since_(height_journal_, height_floor_, since, IRect{0, 0, height_nx(), height_ny()});
}

IRect TerraData::layer_dirty_since(std::uint64_t since) const {
    return dirty_since_(layer_journal_, layer_floor_, since, IRect{0, 0, layer_nx(), layer_ny()});
}

// ---------------------------------------------------------------------------- baking

void TerraData::assign_baked(TerraData&& baked) {
    geometry_ = baked.geometry_;
    tiles_ = std::move(baked.tiles_);
    heights_ = std::move(baked.heights_);
    layers_ = std::move(baked.layers_);
    spill_offsets_ = std::move(baked.spill_offsets_);   // see assign_content()
    spill_ = std::move(baked.spill_);
    const glm::ivec2 cc = geometry_.chunk_count();
    chunk_revisions_.assign(static_cast<std::size_t>(cc.x) * cc.y, 0);
    if (geometry_.has_height_plane()) refresh_steps_(IRect{0, 0, geometry_.tiles_x, geometry_.tiles_y});
    mark_all_dirty();
    unsaved_ = true;
    needs_bake_ = false;
    ++bakes_done_;
}

// ---------------------------------------------------------------------------- queries

bool TerraData::raycast_columns(const glm::vec3& origin, const glm::vec3& dir, float max_t, TileHit& hit) const {
    if (empty() || geometry_.tile_size <= 0.0f) return false;
    const int width = geometry_.tiles_x, height = geometry_.tiles_y;
    const float tile_size = geometry_.tile_size, height_step = geometry_.height_step;
    float t0 = 0.0f, t1 = max_t;
    const glm::vec2 lo(0.0f), hi(static_cast<float>(width) * tile_size, static_cast<float>(height) * tile_size);
    for (int a = 0; a < 2; ++a) {
        if (std::abs(dir[a]) < 1e-12f) {
            if (origin[a] < lo[a] || origin[a] > hi[a]) return false;
            continue;
        }
        float ta = (lo[a] - origin[a]) / dir[a], tb = (hi[a] - origin[a]) / dir[a];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
    }
    if (t0 > t1) return false;

    const glm::vec3 start = origin + dir * (t0 + 1e-5f);
    int ix = std::clamp(static_cast<int>(std::floor(start.x / tile_size)), 0, width - 1);
    int iy = std::clamp(static_cast<int>(std::floor(start.y / tile_size)), 0, height - 1);
    const int step_x = dir.x > 0.0f ? 1 : -1, step_y = dir.y > 0.0f ? 1 : -1;
    const float inf = std::numeric_limits<float>::infinity();
    float t_max_x = std::abs(dir.x) > 1e-12f ? ((static_cast<float>(ix + (dir.x > 0.0f ? 1 : 0)) * tile_size) - origin.x) / dir.x : inf;
    float t_max_y = std::abs(dir.y) > 1e-12f ? ((static_cast<float>(iy + (dir.y > 0.0f ? 1 : 0)) * tile_size) - origin.y) / dir.y : inf;
    const float t_delta_x = std::abs(dir.x) > 1e-12f ? tile_size / std::abs(dir.x) : inf;
    const float t_delta_y = std::abs(dir.y) > 1e-12f ? tile_size / std::abs(dir.y) : inf;

    float t_enter = t0;
    glm::vec3 entry_normal(0.0f);
    bool first = true;
    while (ix >= 0 && iy >= 0 && ix < width && iy < height && t_enter <= t1) {
        const float t_exit = std::min({t_max_x, t_max_y, t1});
        const TileCell& cell = at(ix, iy);
        const float top = (cell.steps < 0 ? 0.0f : cell.ground_steps()) * height_step;
        const float z_enter = origin.z + dir.z * t_enter;
        if (z_enter <= top) {
            hit.tile = glm::ivec2(ix, iy);
            hit.t = t_enter;
            hit.point = origin + dir * t_enter;
            hit.normal = first ? glm::vec3(0.0f, 0.0f, 1.0f) : entry_normal;
            return true;
        }
        // Every solid span of this column, not the ground alone. A column with a roof over it is
        // solid again from `roof_steps` to `roof_top` -- so a ray crossing a hill with a cave in
        // it meets the hillside first, and one fired inside the cave meets its ceiling. Tested
        // against the ground's own top above, which is unchanged, so a column with no roof takes
        // exactly the path it always did.
        if (cell.roofed()) {
            const auto [roofs, roof_count] = spans_at(ix, iy);
            const TileSpanRecord one{cell.roof_steps, cell.roof_top, cell.roof_kinds, 0, 0};
            const TileSpanRecord* list = roof_count > 0 ? roofs : &one;
            const std::size_t n = roof_count > 0 ? roof_count : 1;
            float best_t = std::numeric_limits<float>::infinity();
            glm::vec3 best_n(0.0f);
            for (std::size_t i = 0; i < n; ++i) {
                const float under = static_cast<float>(list[i].under) * height_step;
                const float over = static_cast<float>(list[i].top) * height_step;
                if (z_enter >= under && z_enter <= over) {   // started inside the rock
                    best_t = t_enter;
                    best_n = first ? glm::vec3(0.0f, 0.0f, 1.0f) : entry_normal;
                    break;
                }
                // The face the ray reaches first: a roof's top coming down onto it, or its
                // underside -- a ceiling -- coming up.
                const float face = dir.z < 0.0f ? over : under;
                if (std::abs(dir.z) < 1e-12f) continue;
                const float t_face = (face - origin.z) / dir.z;
                if (t_face < t_enter || t_face > t_exit || t_face >= best_t) continue;
                best_t = t_face;
                best_n = glm::vec3(0.0f, 0.0f, dir.z < 0.0f ? 1.0f : -1.0f);
            }
            if (best_t <= t_exit) {
                hit.tile = glm::ivec2(ix, iy);
                hit.t = best_t;
                hit.point = origin + dir * best_t;
                hit.normal = best_n;
                return true;
            }
        }
        if (dir.z < 0.0f) {
            const float t_top = (top - origin.z) / dir.z;
            if (t_top >= t_enter && t_top <= t_exit) {
                hit.tile = glm::ivec2(ix, iy);
                hit.t = t_top;
                hit.point = origin + dir * t_top;
                hit.normal = glm::vec3(0.0f, 0.0f, 1.0f);
                return true;
            }
        }
        first = false;
        if (t_max_x < t_max_y) {
            ix += step_x;
            t_enter = t_max_x;
            t_max_x += t_delta_x;
            entry_normal = glm::vec3(static_cast<float>(-step_x), 0.0f, 0.0f);
        } else {
            iy += step_y;
            t_enter = t_max_y;
            t_max_y += t_delta_y;
            entry_normal = glm::vec3(0.0f, static_cast<float>(-step_y), 0.0f);
        }
    }
    return false;
}

bool TerraData::raycast_surface(glm::vec3 origin, glm::vec3 dir, float max_t, float& t_out) const {
    if (!has_heights()) return false;
    const glm::vec2 range = height_range();
    const glm::vec3 lo(0.0f, 0.0f, range.x - 0.01f), hi(geometry_.size(), range.y + 0.01f);
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
    const float step = 0.5f * cell() / len;
    auto above = [&](float t) {
        const glm::vec3 p = origin + dir * t;
        return p.z - bilinear_height_(glm::vec2(p));
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


std::pair<const TileSpanRecord*, std::size_t> TerraData::spans_at(int tx, int ty) const {
    if (spill_offsets_.empty() || tx < 0 || ty < 0 || tx >= geometry_.tiles_x ||
        ty >= geometry_.tiles_y) {
        return {nullptr, 0};
    }
    const std::uint32_t word =
        spill_offsets_[static_cast<std::size_t>(ty) * geometry_.tiles_x + tx];
    const std::size_t count = word & 0xFFu;
    const std::size_t first = word >> 8;
    if (count == 0 || first + count > spill_.size()) return {nullptr, 0};
    return {spill_.data() + first, count};
}

void TerraData::set_spans(int tx, int ty, const TileSpanRecord* spans, std::size_t count) {
    if (tx < 0 || ty < 0 || tx >= geometry_.tiles_x || ty >= geometry_.tiles_y) return;
    const std::size_t idx = static_cast<std::size_t>(ty) * geometry_.tiles_x + tx;
    TileCell& cell = tiles_[idx];

    if (count == 0) {
        cell.set(TileCell::Roofed, false);
        cell.set(TileCell::Spilled, false);
        if (!spill_offsets_.empty()) spill_offsets_[idx] = 0;
        return;
    }

    // The outermost roof always lives on the cell itself, so a reader that knows nothing about
    // these planes still sees a correct hill with one cave under it.
    const TileSpanRecord& outer = spans[count - 1];
    cell.roof_steps = outer.under;
    cell.roof_top = outer.top;
    cell.roof_kinds = outer.kinds;
    cell.set(TileCell::Roofed, true);

    // One dry roof fits entirely on the cell. One roof with water under it does not: the band's
    // own table lives on the span record, so it spills even though there is a single roof.
    if (count == 1 && spans[0].water_kind == 0) {
        cell.set(TileCell::Spilled, false);
        if (!spill_offsets_.empty()) spill_offsets_[idx] = 0;
        return;
    }
    if (count > k_max_column_roofs) count = k_max_column_roofs;

    if (spill_offsets_.empty()) {
        spill_offsets_.assign(static_cast<std::size_t>(geometry_.tiles_x) * geometry_.tiles_y, 0);
    }
    const std::size_t first = spill_.size();
    spill_.insert(spill_.end(), spans, spans + count);
    spill_offsets_[idx] = static_cast<std::uint32_t>((first << 8) | count);
    cell.set(TileCell::Spilled, true);
}

void TerraData::sync_outer_spans() {
    if (spill_offsets_.empty()) return;
    for (std::size_t i = 0; i < spill_offsets_.size(); ++i) {
        const std::size_t count = spill_offsets_[i] & 0xFFu;
        const std::size_t first = spill_offsets_[i] >> 8;
        if (count == 0 || first + count > spill_.size()) continue;
        const TileCell& cell = tiles_[i];
        if (!cell.roofed()) continue;
        TileSpanRecord& outer = spill_[first + count - 1];
        outer.under = cell.roof_steps;
        outer.top = cell.roof_top;
        outer.kinds = cell.roof_kinds;
    }
}

void TerraData::compact_spans() {
    if (spill_offsets_.empty()) return;
    std::vector<TileSpanRecord> packed;
    packed.reserve(spill_.size());
    for (std::uint32_t& word : spill_offsets_) {
        const std::size_t count = word & 0xFFu;
        const std::size_t first = word >> 8;
        if (count == 0 || first + count > spill_.size()) {
            word = 0;
            continue;
        }
        const std::size_t at = packed.size();
        packed.insert(packed.end(), spill_.begin() + static_cast<std::ptrdiff_t>(first),
                      spill_.begin() + static_cast<std::ptrdiff_t>(first + count));
        word = static_cast<std::uint32_t>((at << 8) | count);
    }
    spill_ = std::move(packed);
    if (spill_.empty()) spill_offsets_.clear();
}

void TerraData::adopt_spans(std::vector<std::uint32_t> index, std::vector<TileSpanRecord> spans) {
    const std::size_t want = static_cast<std::size_t>(geometry_.tiles_x) * geometry_.tiles_y;
    if (spans.empty() || index.size() != want) {
        spill_offsets_.clear();
        spill_.clear();
        return;
    }
    spill_offsets_ = std::move(index);
    spill_ = std::move(spans);
}

TerraData::GroundLevel TerraData::ground_under_tile(glm::ivec2 tile, float z) const {
    GroundLevel out;
    if (tile.x < 0 || tile.y < 0 || tile.x >= geometry_.tiles_x || tile.y >= geometry_.tiles_y) {
        return out;
    }
    const TileCell& cell = at(tile.x, tile.y);
    const float hs = geometry_.height_step;
    const float ground = (cell.steps < 0 ? 0.0f : cell.ground_steps()) * hs;
    if (ground <= z) {
        out = {ground, 0.0f, 0, true};
    }
    if (!cell.roofed()) return out;

    // Bottom-up, so the LAST roof at or below `z` wins: a body standing on the second storey of
    // a cave is on that storey's floor, not on the ground tens of steps under it.
    const auto [roofs, roof_count] = spans_at(tile.x, tile.y);
    const TileSpanRecord one{cell.roof_steps, cell.roof_top, cell.roof_kinds, 0, 0};
    const TileSpanRecord* list = roof_count > 0 ? roofs : &one;
    const std::size_t n = roof_count > 0 ? roof_count : 1;
    for (std::size_t i = 0; i < n; ++i) {
        const float top = static_cast<float>(list[i].top) * hs;
        if (top > z) break;
        out = {top, static_cast<float>(list[i].under) * hs, static_cast<int>(i) + 1, true};
    }
    return out;
}

TerraData::GroundLevel TerraData::ground_under(glm::vec2 local, float z) const {
    return ground_under_tile(tile_at(local), z);
}

TerraData::GroundLevel TerraData::ceiling_over_tile(glm::ivec2 tile, float z) const {
    GroundLevel out;
    if (tile.x < 0 || tile.y < 0 || tile.x >= geometry_.tiles_x || tile.y >= geometry_.tiles_y) {
        return out;
    }
    const TileCell& cell = at(tile.x, tile.y);
    if (!cell.roofed()) return out;   // open to the sky
    const float hs = geometry_.height_step;
    const auto [roofs, roof_count] = spans_at(tile.x, tile.y);
    const TileSpanRecord one{cell.roof_steps, cell.roof_top, cell.roof_kinds, 0, 0};
    const TileSpanRecord* list = roof_count > 0 ? roofs : &one;
    const std::size_t n = roof_count > 0 ? roof_count : 1;
    for (std::size_t i = 0; i < n; ++i) {
        const float under = static_cast<float>(list[i].under) * hs;
        if (under < z) continue;
        return {under, under, static_cast<int>(i) + 1, true};
    }
    return out;
}

} // namespace world
} // namespace toy
