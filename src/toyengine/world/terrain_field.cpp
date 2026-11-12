#include <toyengine/world/terrain_field.h>

#include <algorithm>
#include <cmath>

namespace toy {
namespace world {

// ---------------------------------------------------------------------------- views

std::uint8_t TileLayerWeights::weight(int i, int j, int layer) const {
    if (!field || layer < 0 || layer >= k_terrain_layer_count) return 0;
    const TerraData* d = field->data();
    if (!d) return 0;
    if (d->has_heights()) {
        // Only the first few layers have a splat channel to carry a weight.
        if (layer >= k_heightmap_layer_count) return 0;
        // Through the texel's position across the whole plane rather than `tx * layer_sub + i`,
        // so a splat resolution that is not a whole multiple of the tile count still lands on
        // the right texel instead of drifting a texel further off with every tile.
        const int edge = std::max(1, n);
        const int tiles_x = std::max(1, d->geometry().tiles_x);
        const int tiles_y = std::max(1, d->geometry().tiles_y);
        const float u = (static_cast<float>(tx) + (static_cast<float>(i) + 0.5f) / static_cast<float>(edge)) /
                        static_cast<float>(tiles_x);
        const float v = (static_cast<float>(ty) + (static_cast<float>(j) + 0.5f) / static_cast<float>(edge)) /
                        static_cast<float>(tiles_y);
        const int si = std::clamp(static_cast<int>(u * static_cast<float>(d->layer_nx())), 0, d->layer_nx() - 1);
        const int sj = std::clamp(static_cast<int>(v * static_cast<float>(d->layer_ny())), 0, d->layer_ny() - 1);
        return d->layer_at(layer, si, sj);
    }
    const TileCell* r = field->record_at(glm::ivec2(tx, ty));
    return r && static_cast<int>(r->top_kind()) == layer ? 255 : 0;
}

int TileLayerWeights::dominant(int i, int j) const {
    if (!field) return 0;
    const TerraData* d = field->data();
    if (!d || !d->has_heights()) {
        const TileCell* r = field->record_at(glm::ivec2(tx, ty));
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

void TerrainField::reset(const TerraGeometry& geometry) {
    data_ = std::make_shared<TerraData>(geometry);
}

void TerrainField::attach(std::shared_ptr<TerraData> data) {
    data_ = std::move(data);
}

const TileCell* TerrainField::record_at(glm::ivec2 tile) const {
    if (!data_ || data_->empty()) return nullptr;
    return &data_->get(tile.x, tile.y);
}

ConstTileHeights TerrainField::tile_height_samples(int tx, int ty) const {
    if (!data_ || !data_->has_heights()) return ConstTileHeights{};
    const int sub = data_->geometry().height_sub;
    const int nx = data_->height_nx();
    if (tx < 0 || ty < 0 || (tx + 1) * sub >= nx || (ty + 1) * sub >= data_->height_ny()) return ConstTileHeights{};
    const std::size_t off = static_cast<std::size_t>(ty * sub) * nx + static_cast<std::size_t>(tx * sub);
    return ConstTileHeights{data_->heights().data() + off, nx, sub + 1};
}

TileHeights TerrainField::tile_height_samples(int tx, int ty) {
    if (!data_ || !data_->has_heights()) return TileHeights{};
    const int sub = data_->geometry().height_sub;
    const int nx = data_->height_nx();
    if (tx < 0 || ty < 0 || (tx + 1) * sub >= nx || (ty + 1) * sub >= data_->height_ny()) return TileHeights{};
    const std::size_t off = static_cast<std::size_t>(ty * sub) * nx + static_cast<std::size_t>(tx * sub);
    return TileHeights{data_->heights().data() + off, nx, sub + 1};
}

TileLayerWeights TerrainField::tile_layer_weights(int tx, int ty) const {
    if (!data_ || !data_->contains(tx, ty)) return TileLayerWeights{};
    if (!data_->has_heights()) return TileLayerWeights{this, tx, ty, 1};
    // Texels per tile edge taken from the plane the weights actually live in, so the view's
    // granularity cannot disagree with the store it reads.
    const int edge = std::max(1, data_->layer_nx() / std::max(1, data_->geometry().tiles_x));
    return TileLayerWeights{this, tx, ty, edge};
}

float TerrainField::layer_weight_at(int layer, glm::vec2 local) const {
    if (!data_ || layer < 0 || layer >= k_terrain_layer_count) return 0.0f;
    return data_->layer_weight(layer, local);
}

std::array<float, k_heightmap_layer_count> TerrainField::layer_weights_at(glm::vec2 local) const {
    if (!data_) return std::array<float, k_heightmap_layer_count>{};
    return data_->layer_weights(local);
}

int TerrainField::dominant_layer_at(glm::vec2 local) const {
    if (!data_) return 0;
    if (!data_->has_heights()) {
        const TileCell* r = record_at(data_->tile_at(local));
        return r ? static_cast<int>(r->top_kind()) : 0;
    }
    const std::array<float, k_heightmap_layer_count> w = data_->layer_weights(local);
    int best = 0;
    for (int l = 1; l < k_heightmap_layer_count; ++l) {
        if (w[static_cast<std::size_t>(l)] > w[static_cast<std::size_t>(best)]) best = l;
    }
    return best;
}

WaterCell TerrainField::water_at(glm::ivec2 tile) const {
    const TileCell* c = record_at(tile);
    if (!c || !c->wet()) return WaterCell{};
    WaterCell w;
    w.kind = static_cast<WaterKind>(c->water_kind);
    w.level = c->water_level;
    w.flow = c->flow;
    return w;
}

void TerrainField::set_water(glm::ivec2 tile, const WaterCell& water) {
    if (!data_ || !data_->contains(tile.x, tile.y)) return;
    TileCell& c = data_->at(tile.x, tile.y);
    const float ground = data_->ground_at_tile(tile);
    const bool wet = water.wet() && water.level > ground;
    c.water_kind = static_cast<std::uint8_t>(wet ? water.kind : WaterKind::None);
    c.water_level = wet ? water.level : 0.0f;
    c.flow = wet && water.kind == WaterKind::River ? water.flow : 0;
    if (!wet) c.flow_authored = 0;
}

IRect TerrainField::tiles_of_height_texels(const IRect& texels) const {
    if (texels.empty() || !data_) return IRect{};
    const int sub = std::max(1, data_->geometry().height_sub);
    // A texel sits on a tile corner, so it belongs to the tile below-left of it and the one
    // above-right; grow both ways to cover every tile the edit can have moved.
    return IRect{texels.x0 / sub - 1, texels.y0 / sub - 1, (texels.x1 + sub - 1) / sub + 1,
                 (texels.y1 + sub - 1) / sub + 1}
        .clipped(data_->geometry().tiles_x, data_->geometry().tiles_y);
}

IRect TerrainField::tiles_of_layer_texels(const IRect& texels) const {
    if (texels.empty() || !data_) return IRect{};
    const int sub = std::max(1, data_->geometry().layer_sub);
    return IRect{texels.x0 / sub, texels.y0 / sub, (texels.x1 + sub - 1) / sub, (texels.y1 + sub - 1) / sub}
        .clipped(data_->geometry().tiles_x, data_->geometry().tiles_y);
}

TerrainField borrow(TerraData& data) {
    TerrainField field;
    // A no-op deleter: the field is a handle over someone else's data, so it must not free it.
    // The doc on borrow() is what keeps the lifetime honest.
    field.attach(std::shared_ptr<TerraData>(&data, [](TerraData*) {}));
    return field;
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
