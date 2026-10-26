#include <toyengine/world/terrain_field.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace toy {
namespace world {
namespace {

/** @brief The WaterKind a record's flags describe. */
WaterKind kind_of_record(const TileRecord& r) {
    if (!r.water()) return WaterKind::None;
    if (r.has(TileRecord::Flowing)) return WaterKind::River;
    return r.has(TileRecord::Ocean) ? WaterKind::Ocean : WaterKind::Lake;
}

} // namespace

// ---------------------------------------------------------------------------- geometry

glm::ivec2 TerrainFieldGeometry::chunk_count() const {
    const int c = std::max(1, chunk_tiles);
    return glm::ivec2(std::max(1, (tiles_x + c - 1) / c), std::max(1, (tiles_y + c - 1) / c));
}

TerrainFieldGeometry TerrainFieldGeometry::sanitized() const {
    TerrainFieldGeometry g = *this;
    g.tiles_x = std::max(0, g.tiles_x);
    g.tiles_y = std::max(0, g.tiles_y);
    g.chunk_tiles = std::clamp(g.chunk_tiles, 1, 512);
    g.layer_sub = std::clamp(g.layer_sub, 1, 64);
    g.height = std::max(0.01f, g.height);
    g.height_step = std::max(0.001f, g.height_step);

    if (g.mode == TerrainMode::Tiles) {
        // One step per tile is what makes a tile a tile; the height plane does not exist.
        g.height_sub = 1;
        // Square tiles, so the covered area follows the tile count.
        const float t = g.size.x > 0.0f && g.tiles_x > 0 ? g.size.x / static_cast<float>(g.tiles_x) : 1.0f;
        g.size = glm::vec2(static_cast<float>(g.tiles_x), static_cast<float>(g.tiles_y)) * t;
    } else {
        g.height_sub = std::clamp(g.height_sub, 1, 64);
        // The height plane is square (HeightmapData holds one resolution), so the tile COUNT is
        // square too. A rectangular `size` then gives rectangular tiles, which is what
        // tile_extent() and cell() report.
        g.tiles_y = g.tiles_x;
        g.size = glm::max(g.size, glm::vec2(0.01f));
    }
    return g;
}

HeightmapGeometry TerrainFieldGeometry::heightmap_geometry() const {
    HeightmapGeometry h;
    h.size = size;
    h.height = height;
    h.chunk_size = static_cast<float>(std::max(1, chunk_tiles)) * tile_size();
    h.resolution = height_nx();
    h.splat_resolution = layer_nx();
    return h.sanitized();
}

bool TerrainFieldGeometry::operator==(const TerrainFieldGeometry& o) const {
    return mode == o.mode && tiles_x == o.tiles_x && tiles_y == o.tiles_y && size == o.size &&
           height_sub == o.height_sub && layer_sub == o.layer_sub && height == o.height &&
           height_step == o.height_step && chunk_tiles == o.chunk_tiles;
}

TerrainField borrow_tiles(TileMapData& tiles, float height_step, float tile_size) {
    TerrainFieldGeometry g;
    g.mode = TerrainMode::Tiles;
    g.tiles_x = tiles.width();
    g.tiles_y = tiles.height();
    g.size = glm::vec2(static_cast<float>(g.tiles_x), static_cast<float>(g.tiles_y)) * tile_size;
    g.height_step = height_step;
    TerrainField field;
    // A no-op deleter: the field is a handle over someone else's grid, so it must not free it.
    // The doc on borrow_tiles() is what keeps the lifetime honest.
    field.adopt(std::shared_ptr<TileMapData>(&tiles, [](TileMapData*) {}), nullptr, g);
    return field;
}

// ---------------------------------------------------------------------------- views

std::uint8_t TileLayerWeights::weight(int i, int j, int layer) const {
    if (!field || layer < 0 || layer >= k_terrain_layer_count) return 0;
    if (const HeightmapData* h = field->heights()) {
        // Only the first few layers have a splat channel to carry a weight.
        if (layer >= k_heightmap_layer_count) return 0;
        // Through the texel's position across the whole plane rather than `tx * layer_sub + i`,
        // so a splat resolution that is not a whole multiple of the tile count still lands on
        // the right texel instead of drifting a texel further off with every tile.
        const TerrainFieldGeometry& g = field->geometry();
        const int edge = std::max(1, n);
        const float u = (static_cast<float>(tx) + (static_cast<float>(i) + 0.5f) / static_cast<float>(edge)) /
                        static_cast<float>(std::max(1, g.tiles_x));
        const float v = (static_cast<float>(ty) + (static_cast<float>(j) + 0.5f) / static_cast<float>(edge)) /
                        static_cast<float>(std::max(1, g.tiles_y));
        const int si = std::clamp(static_cast<int>(u * static_cast<float>(h->splat_w())), 0, h->splat_w() - 1);
        const int sj = std::clamp(static_cast<int>(v * static_cast<float>(h->splat_h())), 0, h->splat_h() - 1);
        return h->splat_at(layer, si, sj);
    }
    const TileRecord* r = field->record_at(glm::ivec2(tx, ty));
    return r && static_cast<int>(r->top_kind()) == layer ? 255 : 0;
}

int TileLayerWeights::dominant(int i, int j) const {
    if (!field) return 0;
    if (!field->heights()) {
        const TileRecord* r = field->record_at(glm::ivec2(tx, ty));
        return r ? static_cast<int>(r->top_kind()) : 0;
    }
    int best = 0;
    std::uint8_t best_w = 0;
    for (int l = 0; l < k_heightmap_layer_count; ++l) {
        const std::uint8_t w = weight(i, j, l);
        if (w > best_w) { best_w = w; best = l; }
    }
    return best;
}

// ---------------------------------------------------------------------------- field

std::uint64_t TerrainField::uid() const { return tiles_ ? tiles_->uid() : 0; }

void TerrainField::reset(const TerrainFieldGeometry& geometry) {
    geometry_ = geometry.sanitized();
    tiles_ = std::make_shared<TileMapData>();
    tiles_->resize(geometry_.tiles_x, geometry_.tiles_y);
    if (geometry_.mode == TerrainMode::Heightmap) {
        heights_ = std::make_shared<HeightmapData>(geometry_.heightmap_geometry());
        water_level_.assign(static_cast<std::size_t>(geometry_.tiles_x) * geometry_.tiles_y, 0.0f);
    } else {
        heights_.reset();
        water_level_.clear();
    }
}

void TerrainField::adopt(std::shared_ptr<TileMapData> tiles, std::shared_ptr<HeightmapData> heights,
                         const TerrainFieldGeometry& geometry) {
    geometry_ = geometry.sanitized();
    tiles_ = std::move(tiles);
    heights_ = geometry_.mode == TerrainMode::Heightmap ? std::move(heights) : nullptr;
    const std::size_t n = tiles_ ? static_cast<std::size_t>(tiles_->width()) * tiles_->height() : 0;
    if (geometry_.mode == TerrainMode::Heightmap) {
        water_level_.resize(n, 0.0f);
    } else {
        water_level_.clear();
    }
}

bool TerrainField::contains(glm::vec2 local) const {
    return local.x >= 0.0f && local.y >= 0.0f && local.x <= geometry_.size.x && local.y <= geometry_.size.y;
}

glm::ivec2 TerrainField::tile_at(glm::vec2 local) const {
    const glm::vec2 e = geometry_.tile_extent();
    return glm::ivec2(static_cast<int>(std::floor(local.x / e.x)), static_cast<int>(std::floor(local.y / e.y)));
}

glm::vec2 TerrainField::tile_centre(glm::ivec2 tile) const {
    return (glm::vec2(tile) + 0.5f) * geometry_.tile_extent();
}

const TileRecord* TerrainField::record_at(glm::ivec2 tile) const {
    if (!tiles_ || tiles_->empty()) return nullptr;
    return &tiles_->get(tile.x, tile.y);
}

float TerrainField::ground_at(glm::vec2 local) const {
    if (heights_) return heights_->sample_height(local);
    const TileRecord* r = record_at(tile_at(local));
    return r ? static_cast<float>(r->steps) * geometry_.height_step : 0.0f;
}

float TerrainField::ground_at_tile(glm::ivec2 tile) const {
    if (!tiles_ || tiles_->empty()) return 0.0f;
    if (heights_) {
        const glm::ivec2 t = glm::clamp(tile, glm::ivec2(0), glm::ivec2(tiles_->width() - 1, tiles_->height() - 1));
        return heights_->sample_height(tile_centre(t));
    }
    return static_cast<float>(tiles_->get(tile.x, tile.y).steps) * geometry_.height_step;
}

glm::vec3 TerrainField::normal_at(glm::vec2 local) const {
    if (heights_) return heights_->normal(local);
    // A tile column's top is flat; its walls are the mesher's, not the field's.
    return glm::vec3(0.0f, 0.0f, 1.0f);
}

glm::vec2 TerrainField::chunk_height_range(int cx, int cy) const {
    if (heights_) return heights_->chunk_minmax(cx, cy);
    if (!tiles_ || tiles_->empty()) return glm::vec2(0.0f);
    const int c = std::max(1, geometry_.chunk_tiles);
    const IRect r = IRect{cx * c, cy * c, (cx + 1) * c + 1, (cy + 1) * c + 1}
                        .clipped(tiles_->width(), tiles_->height());
    if (r.empty()) return glm::vec2(0.0f);
    float lo = std::numeric_limits<float>::max(), hi = -std::numeric_limits<float>::max();
    for (int y = r.y0; y < r.y1; ++y) {
        for (int x = r.x0; x < r.x1; ++x) {
            const float h = static_cast<float>(tiles_->at(x, y).steps) * geometry_.height_step;
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
    }
    return glm::vec2(lo, hi);
}

ConstTileHeights TerrainField::tile_height_samples(int tx, int ty) const {
    if (!heights_) return ConstTileHeights{};
    const int sub = geometry_.height_sub;
    const int nx = heights_->nx();
    if (tx < 0 || ty < 0 || (tx + 1) * sub >= nx || (ty + 1) * sub >= heights_->ny()) return ConstTileHeights{};
    const std::size_t off = static_cast<std::size_t>(ty * sub) * nx + static_cast<std::size_t>(tx * sub);
    return ConstTileHeights{heights_->heights().data() + off, nx, sub + 1};
}

TileHeights TerrainField::tile_height_samples(int tx, int ty) {
    if (!heights_) return TileHeights{};
    const int sub = geometry_.height_sub;
    const int nx = heights_->nx();
    if (tx < 0 || ty < 0 || (tx + 1) * sub >= nx || (ty + 1) * sub >= heights_->ny()) return TileHeights{};
    const std::size_t off = static_cast<std::size_t>(ty * sub) * nx + static_cast<std::size_t>(tx * sub);
    return TileHeights{heights_->heights().data() + off, nx, sub + 1};
}

TileLayerWeights TerrainField::tile_layer_weights(int tx, int ty) const {
    if (!tiles_ || !tiles_->contains(tx, ty)) return TileLayerWeights{};
    if (!heights_) return TileLayerWeights{this, tx, ty, 1};
    // Texels per tile edge taken from the plane the weights actually live in, so the view's
    // granularity cannot disagree with the store it reads.
    const int edge = std::max(1, heights_->splat_w() / std::max(1, geometry_.tiles_x));
    return TileLayerWeights{this, tx, ty, edge};
}

float TerrainField::layer_weight_at(int layer, glm::vec2 local) const {
    if (layer < 0 || layer >= k_terrain_layer_count) return 0.0f;
    if (heights_) return layer < k_heightmap_layer_count ? heights_->layer_weight(layer, local) : 0.0f;
    const TileRecord* r = record_at(tile_at(local));
    return r && static_cast<int>(r->top_kind()) == layer ? 1.0f : 0.0f;
}

std::array<float, k_heightmap_layer_count> TerrainField::layer_weights_at(glm::vec2 local) const {
    if (heights_) return heights_->layer_weights(local);
    std::array<float, k_heightmap_layer_count> w{};
    const TileRecord* r = record_at(tile_at(local));
    const int layer = r ? static_cast<int>(r->top_kind()) : 0;
    if (layer < k_heightmap_layer_count) w[static_cast<std::size_t>(layer)] = 1.0f;
    return w;
}

int TerrainField::dominant_layer_at(glm::vec2 local) const {
    if (!heights_) {
        const TileRecord* r = record_at(tile_at(local));
        return r ? static_cast<int>(r->top_kind()) : 0;
    }
    const std::array<float, k_heightmap_layer_count> w = layer_weights_at(local);
    int best = 0;
    for (int l = 1; l < k_heightmap_layer_count; ++l) {
        if (w[static_cast<std::size_t>(l)] > w[static_cast<std::size_t>(best)]) best = l;
    }
    return best;
}

WaterCell TerrainField::water_at(glm::ivec2 tile) const {
    const TileRecord* r = record_at(tile);
    if (!r) return WaterCell{};
    const WaterKind kind = kind_of_record(*r);
    if (kind == WaterKind::None) return WaterCell{};
    WaterCell c;
    c.kind = kind;
    c.flow = r->flow;
    if (!water_level_.empty() && tiles_->contains(tile.x, tile.y)) {
        c.level = water_level_[static_cast<std::size_t>(tile.y) * tiles_->width() + tile.x];
    } else {
        c.level = static_cast<float>(r->water_level()) * geometry_.height_step;
    }
    return c;
}

WaterCell TerrainField::water_at(glm::vec2 local) const { return water_at(tile_at(local)); }

void TerrainField::set_water(glm::ivec2 tile, const WaterCell& water) {
    if (!tiles_ || !tiles_->contains(tile.x, tile.y)) return;
    TileRecord& r = tiles_->at(tile.x, tile.y);
    const bool flowing = water.kind == WaterKind::River;

    if (heights_) {
        // The height plane is the ground; the record's terrace mirrors it so the flags, the
        // climate and the ids stay one record per tile in both modes.
        const float ground = ground_at(tile_centre(tile));
        const bool wet = water.wet() && water.level > ground;
        r.steps = static_cast<std::int16_t>(std::lround(ground / geometry_.height_step));
        // A sentinel, not a depth: the level plane is the authority here, and water() reads
        // this byte. The real depth is `level - ground`, which water_at() reports.
        r.water_depth = wet ? 1 : 0;
        r.set(TileRecord::Water, wet);
        r.set(TileRecord::Ocean, wet && water.kind == WaterKind::Ocean);
        r.set(TileRecord::Flowing, wet && flowing);
        r.flow = wet && flowing ? water.flow : 0;
        if (!water_level_.empty()) {
            water_level_[static_cast<std::size_t>(tile.y) * tiles_->width() + tile.x] = wet ? water.level : 0.0f;
        }
        return;
    }

    const int level = static_cast<int>(std::floor(water.level / geometry_.height_step + 1e-4f));
    r.set_water(water.wet() ? level : r.steps, water.kind == WaterKind::Ocean, flowing);
    // Lake and Ocean differ only by the Ocean bit; set_water() clears Flowing for both.
    if (r.water() && flowing) r.flow = water.flow;
}

IRect TerrainField::tiles_of_height_texels(const IRect& texels) const {
    if (texels.empty()) return IRect{};
    const int sub = std::max(1, geometry_.height_sub);
    // A texel sits on a tile corner, so it belongs to the tile below-left of it and the one
    // above-right; grow both ways to cover every tile the edit can have moved.
    return IRect{texels.x0 / sub - 1, texels.y0 / sub - 1, (texels.x1 + sub - 1) / sub + 1,
                 (texels.y1 + sub - 1) / sub + 1}
        .clipped(geometry_.tiles_x, geometry_.tiles_y);
}

IRect TerrainField::tiles_of_layer_texels(const IRect& texels) const {
    if (texels.empty()) return IRect{};
    const int sub = std::max(1, geometry_.layer_sub);
    return IRect{texels.x0 / sub, texels.y0 / sub, (texels.x1 + sub - 1) / sub, (texels.y1 + sub - 1) / sub}
        .clipped(geometry_.tiles_x, geometry_.tiles_y);
}

void TerrainField::mark_tiles_dirty(const IRect& tiles) {
    if (tiles.empty()) return;
    if (tiles_) tiles_->mark_dirty(tiles);
    if (heights_) {
        const int hsub = std::max(1, geometry_.height_sub);
        const int lsub = std::max(1, geometry_.layer_sub);
        heights_->mark_height_dirty(IRect{tiles.x0 * hsub, tiles.y0 * hsub, tiles.x1 * hsub + 1,
                                          tiles.y1 * hsub + 1});
        heights_->mark_splat_dirty(IRect{tiles.x0 * lsub, tiles.y0 * lsub, tiles.x1 * lsub, tiles.y1 * lsub});
    }
}

void TerrainField::mark_all_dirty() {
    if (tiles_) tiles_->mark_all_dirty();
    if (heights_) heights_->mark_all_dirty();
}

TerrainFieldCursor TerrainField::cursor() const {
    TerrainFieldCursor c;
    c.tiles = tiles_ ? tiles_->revision() : 0;
    c.heights = heights_ ? heights_->revision() : 0;
    return c;
}

std::uint64_t TerrainField::chunk_revision(int cx, int cy) const {
    if (heights_) return heights_->chunk_revision(cx, cy);
    return tiles_ ? tiles_->revision() : 0;
}

IRect TerrainField::dirty_since(const TerrainFieldCursor& since) const {
    IRect out = tiles_ ? tiles_->dirty_since(since.tiles) : IRect{};
    if (heights_) {
        out.include(tiles_of_height_texels(heights_->height_dirty_since(since.heights)));
        out.include(tiles_of_layer_texels(heights_->splat_dirty_since(since.heights)));
    }
    return out;
}

// ---------------------------------------------------------------------------- view

WaterCell TerrainFieldView::water(glm::vec3 world) const {
    if (!field) return WaterCell{};
    WaterCell c = field->water_at(local(world));
    if (c.wet()) c.level += origin.z;
    return c;
}

} // namespace world
} // namespace toy
