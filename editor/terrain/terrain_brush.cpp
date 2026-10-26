#include <editor/terrain/terrain_brush.h>

#include <toyengine/world/terrain_water.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace toy::editor {

using toy::world::TerrainField;
using toy::world::WaterCell;
using toy::world::HeightmapData;
using toy::world::IRect;

const char* terrain_tool_name(TerrainTool t) {
    switch (t) {
        case TerrainTool::Raise:     return "Raise / Lower";
        case TerrainTool::Smooth:    return "Smooth";
        case TerrainTool::Flatten:   return "Flatten";
        case TerrainTool::SetHeight: return "Set Height";
        case TerrainTool::Noise:     return "Noise";
        case TerrainTool::Erode:     return "Erode";
        case TerrainTool::Paint:     return "Paint";
        case TerrainTool::Water:     return "Water";
    }
    return "?";
}

float terrain_falloff(float d01, float falloff) {
    if (d01 >= 1.0f) return 0.0f;
    const float f = std::clamp(falloff, 0.0f, 1.0f);
    const float inner = 1.0f - f;   // fully weighted inside this fraction of the radius
    if (d01 <= inner) return 1.0f;
    const float t = (d01 - inner) / std::max(f, 1e-4f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

namespace {

/** @brief Height texel rectangle covering a circle (inclusive of edge texels). */
IRect circle_texels(const HeightmapData& d, glm::vec2 c, float r) {
    const glm::vec2 cell = d.cell();
    IRect rect;
    rect.x0 = static_cast<int>(std::floor((c.x - r) / cell.x));
    rect.y0 = static_cast<int>(std::floor((c.y - r) / cell.y));
    rect.x1 = static_cast<int>(std::ceil((c.x + r) / cell.x)) + 1;
    rect.y1 = static_cast<int>(std::ceil((c.y + r) / cell.y)) + 1;
    return rect.clipped(d.nx(), d.ny());
}

template <typename Fn>
void for_each_weighted(const HeightmapData& d, const IRect& r, glm::vec2 c, float radius, float strength, float falloff, Fn&& fn) {
    const glm::vec2 cell = d.cell();
    for (int j = r.y0; j < r.y1; ++j) {
        for (int i = r.x0; i < r.x1; ++i) {
            const glm::vec2 p(static_cast<float>(i) * cell.x, static_cast<float>(j) * cell.y);
            const float w = strength * terrain_falloff(glm::length(p - c) / radius, falloff);
            if (w > 0.0f) fn(i, j, w);
        }
    }
}

} // namespace

toy::world::IRect terrain_water_dab(toy::world::TerrainField& field, const TerrainBrushSettings& s,
                                    glm::vec2 centre, bool invert) {
    using namespace toy::world;
    if (field.empty()) return IRect{};
    const glm::vec2 extent = field.geometry().tile_extent();
    const float r = std::max(0.01f, s.radius);
    const IRect rect = IRect{static_cast<int>(std::floor((centre.x - r) / extent.x)),
                             static_cast<int>(std::floor((centre.y - r) / extent.y)),
                             static_cast<int>(std::ceil((centre.x + r) / extent.x)) + 1,
                             static_cast<int>(std::ceil((centre.y + r) / extent.y)) + 1}
                            .clipped(field.geometry().tiles_x, field.geometry().tiles_y);
    if (rect.empty()) return IRect{};

    // Only the tiles whose centre is inside the circle seed it; a square brush would flood
    // corners the user never painted over.
    std::vector<glm::ivec2> seeds;
    for (int y = rect.y0; y < rect.y1; ++y) {
        for (int x = rect.x0; x < rect.x1; ++x) {
            const glm::vec2 c = (glm::vec2(x, y) + 0.5f) * extent;
            if (glm::length(c - centre) <= r) seeds.push_back(glm::ivec2(x, y));
        }
    }
    if (seeds.empty()) return IRect{};
    if (invert || s.water_kind == WaterKind::None) return drain_water(field, seeds);
    return flood_water(field, seeds, s.water_level, s.water_kind);
}

IRect terrain_dab(HeightmapData& d, const TerrainBrushSettings& s, glm::vec2 centre, bool invert, float flatten_to,
                  std::uint32_t seed) {
    const float r = std::max(s.radius, 0.01f);
    const float top = d.geometry().height;
    const float strength = std::clamp(s.strength, 0.0f, 1.0f);

    if (s.tool == TerrainTool::Paint) {
        // Splat texels are texel-centred over the terrain's area.
        const glm::vec2 st = d.geometry().size / glm::vec2(static_cast<float>(d.splat_w()), static_cast<float>(d.splat_h()));
        IRect rect;
        rect.x0 = static_cast<int>(std::floor((centre.x - r) / st.x));
        rect.y0 = static_cast<int>(std::floor((centre.y - r) / st.y));
        rect.x1 = static_cast<int>(std::ceil((centre.x + r) / st.x)) + 1;
        rect.y1 = static_cast<int>(std::ceil((centre.y + r) / st.y)) + 1;
        rect = rect.clipped(d.splat_w(), d.splat_h());
        const int layer = std::clamp(s.layer, 0, toy::world::k_heightmap_layer_count - 1);
        for (int j = rect.y0; j < rect.y1; ++j) {
            for (int i = rect.x0; i < rect.x1; ++i) {
                const glm::vec2 p((static_cast<float>(i) + 0.5f) * st.x, (static_cast<float>(j) + 0.5f) * st.y);
                const float w = strength * terrain_falloff(glm::length(p - centre) / r, s.falloff);
                if (w <= 0.0f) continue;
                std::uint8_t& v = d.splat_at(layer, i, j);
                if (invert) {
                    // Erase: take some of this layer away and hand it to the others in proportion
                    // (to layer 0 -- or 1, erasing 0 -- when nothing else was there).
                    const int keep = static_cast<int>(std::lround(static_cast<float>(v) * (1.0f - w)));
                    const int removed = v - keep;
                    if (removed <= 0) continue;
                    int others = 0;
                    for (int l = 0; l < toy::world::k_heightmap_layer_count; ++l) if (l != layer) others += d.splat_at(l, i, j);
                    v = static_cast<std::uint8_t>(keep);
                    if (others == 0) {
                        d.splat_at(layer == 0 ? 1 : 0, i, j) = static_cast<std::uint8_t>(removed);
                    } else {
                        for (int l = 0; l < toy::world::k_heightmap_layer_count; ++l) {
                            if (l == layer) continue;
                            std::uint8_t& o = d.splat_at(l, i, j);
                            o = static_cast<std::uint8_t>(std::min(255L, o + std::lround(static_cast<float>(removed) * o / others)));
                        }
                    }
                    d.normalize_splat_texel(i, j, layer);
                } else {
                    v = static_cast<std::uint8_t>(std::lround(static_cast<float>(v) + (255.0f - static_cast<float>(v)) * w));
                    d.normalize_splat_texel(i, j, layer);
                }
            }
        }
        d.mark_splat_dirty(rect);
        return rect;
    }

    const IRect rect = circle_texels(d, centre, r);
    if (rect.empty()) return rect;
    // A copy of the region (plus a one-texel border) so smoothing and erosion read the
    // pre-dab heights, whatever order texels are visited in.
    const IRect src = IRect{rect.x0 - 1, rect.y0 - 1, rect.x1 + 1, rect.y1 + 1};
    std::vector<float> before(static_cast<std::size_t>(src.width()) * src.height());
    for (int j = src.y0; j < src.y1; ++j) {
        for (int i = src.x0; i < src.x1; ++i) before[static_cast<std::size_t>(j - src.y0) * src.width() + (i - src.x0)] = d.at_clamped(i, j);
    }
    auto old = [&](int i, int j) {
        i = std::clamp(i, src.x0, src.x1 - 1);
        j = std::clamp(j, src.y0, src.y1 - 1);
        return before[static_cast<std::size_t>(j - src.y0) * src.width() + (i - src.x0)];
    };

    // Raise / noise rates scale with the brush, so a big brush moves mountains and a small one
    // sculpts detail at a similar feel.
    const float rate = 0.04f * r;
    switch (s.tool) {
        case TerrainTool::Raise:
            for_each_weighted(d, rect, centre, r, strength, s.falloff, [&](int i, int j, float w) {
                d.at(i, j) = std::clamp(old(i, j) + (invert ? -1.0f : 1.0f) * w * rate, 0.0f, top);
            });
            break;
        case TerrainTool::Smooth:
            for_each_weighted(d, rect, centre, r, strength, s.falloff, [&](int i, int j, float w) {
                float sum = 0.0f;
                for (int dj = -1; dj <= 1; ++dj) {
                    for (int di = -1; di <= 1; ++di) sum += old(i + di, j + dj);
                }
                d.at(i, j) = old(i, j) + (sum / 9.0f - old(i, j)) * w;
            });
            break;
        case TerrainTool::Flatten:
            for_each_weighted(d, rect, centre, r, strength, s.falloff, [&](int i, int j, float w) {
                d.at(i, j) = old(i, j) + (std::clamp(flatten_to, 0.0f, top) - old(i, j)) * w;
            });
            break;
        case TerrainTool::SetHeight:
            for_each_weighted(d, rect, centre, r, strength, s.falloff, [&](int i, int j, float w) {
                d.at(i, j) = old(i, j) + (std::clamp(s.target_height, 0.0f, top) - old(i, j)) * w;
            });
            break;
        case TerrainTool::Noise: {
            const glm::vec2 cell = d.cell();
            const float inv = 1.0f / std::max(s.noise_scale, 0.1f);
            for_each_weighted(d, rect, centre, r, strength, s.falloff, [&](int i, int j, float w) {
                const glm::vec2 p(static_cast<float>(i) * cell.x, static_cast<float>(j) * cell.y);
                const float n = toy::world::heightmap_fbm(p.x * inv, p.y * inv, seed, 3, 0.0f);
                d.at(i, j) = std::clamp(old(i, j) + (invert ? -1.0f : 1.0f) * n * w * rate, 0.0f, top);
            });
            break;
        }
        case TerrainTool::Erode: {
            // Thermal erosion, one relaxation per dab: material above the talus slope slides to
            // the lowest neighbour, rounding crests and filling gullies.
            const glm::vec2 cell = d.cell();
            const float talus = 0.6f * std::min(cell.x, cell.y);   // ~31 degrees
            for_each_weighted(d, rect, centre, r, strength, s.falloff, [&](int i, int j, float w) {
                const float h = old(i, j);
                float lowest = h;
                for (int dj = -1; dj <= 1; ++dj) {
                    for (int di = -1; di <= 1; ++di) lowest = std::min(lowest, old(i + di, j + dj));
                }
                float highest = h;
                for (int dj = -1; dj <= 1; ++dj) {
                    for (int di = -1; di <= 1; ++di) highest = std::max(highest, old(i + di, j + dj));
                }
                float delta = 0.0f;
                if (h - lowest > talus) delta -= 0.5f * (h - lowest - talus);
                if (highest - h > talus) delta += 0.25f * (highest - h - talus);
                d.at(i, j) = std::clamp(h + delta * w, 0.0f, top);
            });
            break;
        }
        case TerrainTool::Paint:
            break;
    }
    d.mark_height_dirty(rect);
    return rect;
}

TerrainPatch capture_patch(const std::shared_ptr<HeightmapData>& data, const IRect& height_rect, const IRect& splat_rect) {
    return capture_patch_from(data, data->heights(), data->splat(0), data->splat(1), height_rect, splat_rect);
}

TerrainPatch capture_patch_from(const std::shared_ptr<HeightmapData>& data, const std::vector<float>& heights,
                                const std::vector<std::uint8_t>& splat0, const std::vector<std::uint8_t>& splat1,
                                const IRect& height_rect_in, const IRect& splat_rect_in) {
    TerrainPatch p;
    p.data = data;
    p.nx = data->nx();
    p.ny = data->ny();
    p.splat_size = data->splat_w();
    p.height_rect = height_rect_in.clipped(data->nx(), data->ny());
    p.splat_rect = splat_rect_in.clipped(data->splat_w(), data->splat_h());
    const IRect& hr = p.height_rect;
    p.heights.reserve(static_cast<std::size_t>(hr.width()) * hr.height());
    for (int j = hr.y0; j < hr.y1; ++j) {
        for (int i = hr.x0; i < hr.x1; ++i) p.heights.push_back(heights[static_cast<std::size_t>(j) * data->nx() + i]);
    }
    const IRect& sr = p.splat_rect;
    for (int j = sr.y0; j < sr.y1; ++j) {
        const std::size_t row = (static_cast<std::size_t>(j) * data->splat_w() + sr.x0) * 4;
        p.splat0.insert(p.splat0.end(), splat0.begin() + static_cast<std::ptrdiff_t>(row),
                        splat0.begin() + static_cast<std::ptrdiff_t>(row + static_cast<std::size_t>(sr.width()) * 4));
        p.splat1.insert(p.splat1.end(), splat1.begin() + static_cast<std::ptrdiff_t>(row),
                        splat1.begin() + static_cast<std::ptrdiff_t>(row + static_cast<std::size_t>(sr.width()) * 4));
    }
    return p;
}

std::vector<toy::world::WaterCell> capture_water(const TerrainField& field, const IRect& rect) {
    std::vector<toy::world::WaterCell> out;
    if (rect.empty()) return out;
    out.reserve(static_cast<std::size_t>(rect.width()) * rect.height());
    for (int y = rect.y0; y < rect.y1; ++y) {
        for (int x = rect.x0; x < rect.x1; ++x) out.push_back(field.water_at(glm::ivec2(x, y)));
    }
    return out;
}

bool apply_patch(const TerrainPatch& p, TerrainField* field) {
    std::shared_ptr<HeightmapData> data = p.data.lock();
    if (!data || data->nx() != p.nx || data->ny() != p.ny || data->splat_w() != p.splat_size) return false;
    const IRect& hr = p.height_rect;
    std::size_t k = 0;
    for (int j = hr.y0; j < hr.y1; ++j) {
        for (int i = hr.x0; i < hr.x1; ++i) data->at(i, j) = p.heights[k++];
    }
    if (!hr.empty()) data->mark_height_dirty(hr);
    const IRect& sr = p.splat_rect;
    std::size_t row = 0;
    for (int j = sr.y0; j < sr.y1; ++j) {
        const std::size_t dst = (static_cast<std::size_t>(j) * data->splat_w() + sr.x0) * 4;
        std::copy_n(p.splat0.begin() + static_cast<std::ptrdiff_t>(row), sr.width() * 4, data->splat(0).begin() + static_cast<std::ptrdiff_t>(dst));
        std::copy_n(p.splat1.begin() + static_cast<std::ptrdiff_t>(row), sr.width() * 4, data->splat(1).begin() + static_cast<std::ptrdiff_t>(dst));
        row += static_cast<std::size_t>(sr.width()) * 4;
    }
    if (!sr.empty()) data->mark_splat_dirty(sr);

    // Water, when the caller has the field to put it back into. Restored last, because
    // set_water() reads the ground under each tile and the heights above may just have moved.
    const IRect& wr = p.water_rect;
    if (field != nullptr && !wr.empty() &&
        p.water.size() == static_cast<std::size_t>(wr.width()) * wr.height()) {
        std::size_t w = 0;
        for (int y = wr.y0; y < wr.y1; ++y) {
            for (int x = wr.x0; x < wr.x1; ++x) field->set_water(glm::ivec2(x, y), p.water[w++]);
        }
        field->mark_tiles_dirty(wr);
    }
    return true;
}

} // namespace toy::editor
