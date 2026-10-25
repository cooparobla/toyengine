#include "editor/app/ui/world_generator.h"
#include "editor/app/ui/tile_mode.h"

#include "editor/app/editor_app.h"
#include "editor/app/ui/tile_terrain_panel.h"

#include <cmath>
#include <cstdio>
#include <random>

#include <coopa/maps/map_generator.h>
#include <coopa/maps/map_presets.h>
#include <coopa/maps/map_renderer.h>
#include <coopa/maps/map_yaml.h>
#include <toyengine/world/terrain_component.h>
#include <toyengine/world/terrain_generator.h>

namespace toy {
namespace editor {

using coopa::maps::MapConfig;
using coopa::maps::MapShape;

namespace {

constexpr double k_preview_debounce = 0.3; ///< Seconds after the last edit before a preview starts.

double wgen_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

const char* const k_categories[] = {"World", "Shape", "Elevation", "Climate", "Water & Rivers", "Settlements",
                                    "Advanced", "Landforms"};
constexpr int k_category_count = static_cast<int>(sizeof(k_categories) / sizeof(k_categories[0]));
/// The list's order: Landforms (appended, so the row table's indices stay put) after Elevation.
constexpr int k_category_order[] = {0, 1, 2, 7, 3, 4, 5, 6};
constexpr int k_landforms = 7;

/**
 * @brief One editable mapcoopa setting: a label, a range, a help line, and how to reach it.
 *
 * The curated set (and the ranges) mapcoopa's own viewer offers, so the two tools describe a
 * world the same way. Values pass through double: every setting here is a number.
 */
struct Row {
    int category;
    const char* label;
    const char* help;
    bool integer;
    double lo, hi;
    double (*get)(const MapConfig&);
    void (*set)(MapConfig&, double);
};

#define WGEN_ROW(cat, label, help, integer, lo, hi, field)                                                    \
    Row {                                                                                                      \
        cat, label, help, integer, lo, hi, [](const MapConfig& c) { return static_cast<double>(c.field); },    \
            [](MapConfig& c, double v) { c.field = static_cast<decltype(c.field)>(v); }                        \
    }

const std::vector<Row>& rows() {
    static const std::vector<Row> table = {
        // World (seed and the terrain's own scale are drawn by hand above these)
        WGEN_ROW(0, "Map Cells", "Cells per side of the map. More cells, more detail -- the world keeps its size in "
                 "cells x Tiles per Cell tiles.", true, 20, 160, grid_size),
        // Shape
        WGEN_ROW(1, "Irregularity", "How far a landmass outline wanders from a circle.", false, 0.0, 0.6, shape.irregularity),
        WGEN_ROW(1, "Size Variance", "Archipelago: how much the islands' sizes vary.", false, 0.0, 0.9, shape.size_variance),
        WGEN_ROW(1, "Coast Detail", "Fine wobble of the coastline, as a fraction of a landmass's radius.", false, 0.0, 0.5,
                 shape.coast_detail),
        WGEN_ROW(1, "Land Threshold", "Noise level above which ground is water: higher makes more land, fewer lakes.", false,
                 0.0, 1.0, threshold_water),
        // Elevation
        WGEN_ROW(2, "Sea Level", "The waterline, as a fraction of the height range. Raise it to drown the coasts.", false,
                 0.05, 0.9, sea_level),
        WGEN_ROW(2, "Relief", "How much noise shapes the highlands, rather than distance from the coast alone.", false, 0.0,
                 1.5, terrain_relief),
        WGEN_ROW(2, "Roughness", "Fine fractal bumps over the land. 0 keeps terraces broad and calm.", false, 0.0, 1.0,
                 terrain_roughness),
        WGEN_ROW(2, "Smoothing", "Passes that relax the height field: more is gentler land.", true, 0, 20,
                 elevation_smoothing_iterations),
        // Climate
        WGEN_ROW(3, "Warmth", "Shifts the whole world warmer (+) or colder (-).", false, -1.0, 1.0, temperature_offset),
        WGEN_ROW(3, "Lapse Rate", "How much height cools the air -- snow on the peaks.", false, 0.0, 1.0,
                 temperature_lapse_rate),
        WGEN_ROW(3, "Polar Falloff", "Shapes the climb from the frozen edges to the warm middle.", false, 0.2, 4.0,
                 temperature_falloff),
        WGEN_ROW(3, "Polar North", "Fraction of the map at the north edge that is frozen.", false, 0.0, 0.5,
                 polar_extent_north),
        WGEN_ROW(3, "Polar South", "Fraction of the map at the south edge that is frozen.", false, 0.0, 0.5,
                 polar_extent_south),
        // Water & rivers
        WGEN_ROW(4, "Rivers", "River sources to attempt; fewer appear where the land refuses them.", true, 0, 200, river_count),
        WGEN_ROW(4, "Min River Length", "Cells a watercourse must run before it counts as a river.", true, 2, 30,
                 river_min_length),
        WGEN_ROW(4, "Source Height Lowest", "Lowest ground a river may spring from (0 the shore, 1 the summit): lower "
                 "starts rivers in the hills and sends them further.", false, 0.0, 1.0, river_source_min_elevation),
        WGEN_ROW(4, "Source Height Highest", "Highest ground a river may spring from. Narrow the window to gather every "
                 "source into one band of country.", false, 0.0, 1.0, river_source_max_elevation),
        WGEN_ROW(4, "Meander", "Passes that smooth each watercourse: more winds it, fewer leave it straight between "
                 "cells.", true, 0, 8, river_smoothing_iterations),
        WGEN_ROW(4, "River Width (m)", "Width of a small stream; rivers widen as streams join.", false, 1.0, 40.0,
                 river_width_base_m),
        WGEN_ROW(4, "Width per Tributary (m)", "Metres a river widens for each stream that has joined it: this is what "
                 "makes a trunk river broad where a headwater is a brook.", false, 0.0, 20.0, river_width_per_volume_m),
        WGEN_ROW(4, "River Depth (m)", "Depth of the water in a small stream's channel.", false, 0.2, 10.0, river_depth_m),
        WGEN_ROW(4, "Depth per Tributary (m)", "The same for depth: how much deeper a river runs per stream joined.", false,
                 0.0, 5.0, river_depth_per_volume_m),
        WGEN_ROW(4, "Channel Depth (m)", "How deep a river cuts its bed into the ground.", false, 0.0, 60.0,
                 river_channel_depth_m),
        WGEN_ROW(4, "Valley Depth (m)", "How deep a river carves the valley it runs in.", false, 0.0, 200.0, river_incision_m),
        WGEN_ROW(4, "Lake Depth (m)", "How deep a lake's bed lies under its surface, at the least.", false, 0.0, 60.0,
                 lake_depth_m),
        // Settlements & regions
        WGEN_ROW(5, "Towns", "Settlements to place (stored as regions now; towns are kept for later).", true, 0, 120,
                 towns.town_count),
        WGEN_ROW(5, "Countries", "Nations to divide the land between.", true, 1, 16, regions.country_count),
        WGEN_ROW(5, "Regions per Country", "Provinces carved out of each nation.", true, 1, 10, regions.regions_per_country),
        WGEN_ROW(5, "Landmarks", "Natural features to record: peaks, falls, canyons.", true, 0, 200, landmarks.max_natural),
        WGEN_ROW(5, "Ruins", "Abandoned works to record.", true, 0, 200, landmarks.max_abandoned),
        WGEN_ROW(5, "Caves", "Cave systems to open under the slopes.", true, 0, 60, caves.cave_count),
        // Advanced
        WGEN_ROW(6, "Island Noise Frequency", "Scale of the land/water field (at 40 cells; scaled with Map Cells).", false,
                 0.01, 0.3, noise_island.frequency),
        WGEN_ROW(6, "Island Noise Octaves", "Detail layers in the land/water field.", true, 1, 8, noise_island.octaves),
        WGEN_ROW(6, "Relief Noise Frequency", "Scale of the highland field.", false, 0.005, 0.3, noise_relief.frequency),
        WGEN_ROW(6, "Cell Jitter", "How far cells wander from a regular grid.", false, 0.0, 1.0, jitter),
        WGEN_ROW(6, "Border Width", "Width of the band of sea forced around the map edge, in cells.", false, 0.0, 6.0,
                 border_length),
        WGEN_ROW(6, "Cell Size (map m)", "Ground one cell covers in mapcoopa's metres (sets what river widths mean).", false,
                 10.0, 300.0, meters_per_grid_unit),
        WGEN_ROW(6, "Height Range (map m)", "Metres mapcoopa's full height range stands for (sets what depths mean).", false,
                 50.0, 3000.0, elevation_range_m),
        // Landforms (the natural terrain model)
        WGEN_ROW(7, "Smoothing", "How smoothly the map's heights are joined: 0 flat facets, 1 fully rounded.", false, 0.0, 1.0,
                 natural.smoothing),
        WGEN_ROW(7, "Mountains", "Height of the ridged mountain ranges on the high ground.", false, 0.0, 0.6, natural.mountains),
        WGEN_ROW(7, "Mountain Start", "How high the land must be before ranges rise (0 the shore, 1 the summit).", false, 0.0,
                 1.0, natural.mountain_start),
        WGEN_ROW(7, "Range Frequency", "How close together the ranges and their ridges are (at 40 cells).", false, 0.03, 0.6,
                 noise_ridge.frequency),
        WGEN_ROW(7, "Hills", "Height of the rolling hills over the lowland.", false, 0.0, 0.15, natural.hills),
        WGEN_ROW(7, "Hill Frequency", "How close together the hills are (at 40 cells).", false, 0.05, 1.0,
                 noise_hills.frequency),
        WGEN_ROW(7, "Plateaus", "How high flat-topped plateaus stand over the land around them.", false, 0.0, 0.25,
                 natural.plateaus),
        WGEN_ROW(7, "Plateau Coverage", "Share of the lowland raised into plateaus.", false, 0.0, 1.0,
                 natural.plateau_coverage),
        WGEN_ROW(7, "Plateau Frequency", "How large the plateaus are: lower is broader (at 40 cells).", false, 0.02, 0.4,
                 noise_plateau.frequency),
        WGEN_ROW(7, "Warp", "How far the landforms are bent, in cells: winding ridges and valleys.", false, 0.0, 4.0,
                 natural.warp),
    };
    return table;
}

#undef WGEN_ROW

const std::vector<std::string>& shape_names() {
    static const std::vector<std::string> names = {"Rectangle", "Circle", "Triangle", "Continent", "Archipelago"};
    return names;
}

/** @brief A preview of `authored`: generated at its own grid size, rendered small. Runs on a worker. */
WorldGeneratorPanel::WorldPreview make_world_preview(MapConfig authored, int layer, bool planet, int preview_px,
                                           std::uint64_t serial) {
    static coopa::debug::Logger logger("world_preview");
    // A planet's map is generated on a sphere (as the terrain does) and previewed unrolled.
    if (planet) authored.topology = coopa::maps::MapTopology::Sphere;
    MapConfig config = coopa::maps::derived_config(authored);
    coopa::maps::set_image_size(config, preview_px);
    coopa::maps::MapGenerator generator(config, logger);
    generator.generate();
    const coopa::maps::MapGraph& graph = generator.graph();
    coopa::maps::Image image;
    switch (layer) {
        case 1: image = coopa::maps::MapLayers::biomes(graph, config); break;
        case 2: image = coopa::maps::MapLayers::elevation(graph, config); break;
        case 3: image = coopa::maps::MapLayers::water(graph, config); break;
        default: image = coopa::maps::MapLayers::composite(graph, config); break;
    }
    WorldGeneratorPanel::WorldPreview out;
    out.serial = serial;
    out.width = image.width;
    out.height = image.height;
    out.rgb.resize(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 3);
    const int ch = std::max(1, image.channels);
    for (std::size_t i = 0, n = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height); i < n; ++i) {
        for (int k = 0; k < 3; ++k) out.rgb[i * 3 + static_cast<std::size_t>(k)] = image.pixels[i * static_cast<std::size_t>(ch) + static_cast<std::size_t>(std::min(k, ch - 1))];
    }
    std::size_t land = 0, counted = 0;
    for (const auto& c : graph.centers) {
        if (c.border) continue;
        ++counted;
        land += c.water ? 0 : 1;
        out.lakes += (c.water && !c.ocean) ? 1 : 0;
    }
    out.land_percent = counted > 0 ? static_cast<int>(100 * land / counted) : 0;
    out.rivers = static_cast<int>(graph.rivers.size());
    out.towns = static_cast<int>(graph.towns.size());
    return out;
}
} // namespace

void WorldGeneratorPanel::open_world_generator(ObjectId id) {
    if (id == 0) id = app_.doc_.primary();
    const int ci = app_.doc_.find_component(id, "TileTerrain");
    if (ci < 0) {
        app_.log_warn("World Generator: select a TileTerrain first");
        return;
    }
    const Node comp = app_.doc_.find(id)->at("components").as_seq()[static_cast<std::size_t>(ci)];
    wgen_object_ = id;
    wgen_config_ = toy::world::generator_config_from_node(comp);
    const toy::world::TerrainParams defaults;
    wgen_world_tiles_ = get_int(comp, "world_tiles", 0);
    if (wgen_world_tiles_ <= 0) {
        wgen_world_tiles_ = wgen_config_.grid_size * get_int(comp, "tiles_per_grid_unit", defaults.tiles_per_grid_unit);
    }
    wgen_tile_size_ = get_float(comp, "tile_size", defaults.tile_size);
    wgen_height_scale_ = get_float(comp, "height_scale", defaults.height_scale);
    wgen_water_inset_ = get_float(comp, "water_inset", defaults.water_inset);
    wgen_river_inset_ = get_float(comp, "river_inset", defaults.river_inset);
    wgen_planet_ = get_bool(comp, "planet", false);
    wgen_face_tiles_ = get_int(comp, "face_tiles", defaults.face_tiles);
    wgen_gravity_ = get_float(comp, "gravity", 9.81f);
    wgen_requested_ = wgen_started_ + 1;
    wgen_changed_at_ = 0.0;   // preview at once
    app_.pending_modal_ = "World Generator";
}

void WorldGeneratorPanel::wgen_tick_preview_() {
    // A finished preview lands.
    if (wgen_job_.valid() && wgen_job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        wgen_preview_ = wgen_job_.get();
        ++wgen_preview_landed_;
    }
    // The latest settings start one, once the edits pause -- one in flight at a time.
    if (wgen_object_ != 0 && wgen_requested_ != wgen_started_ && !wgen_job_.valid() &&
        wgen_now() - wgen_changed_at_ >= k_preview_debounce) {
        wgen_started_ = wgen_requested_;
        wgen_job_ = std::async(std::launch::async, make_world_preview, wgen_config_, wgen_layer_, wgen_planet_,
                               wgen_preview_px_, wgen_started_);
    }
    // Generate World: bake once the re-parsed terrain carries the new settings.
    if (wgen_regen_object_ != 0) {
        coopa::scene::SceneObject* live = app_.sync_.live(wgen_regen_object_);
        auto* t = live ? live->get_component<toy::world::TerrainComponent>() : nullptr;
        const bool ready = t != nullptr &&
                           fkyaml::node::serialize(toy::world::generator_node(t->generator)) == wgen_regen_expect_ &&
                           !t->baking() && !app_.tile_mode_panel_->tile_regen_data_;
        if (ready) {
            app_.tile_mode_panel_->tile_regenerate_(wgen_regen_object_);
            wgen_regen_object_ = 0;
        } else if (++wgen_regen_frames_ > 600) {
            app_.log_warn("World Generator: the terrain did not pick up the new settings; use Regenerate World");
            wgen_regen_object_ = 0;
        }
    }
}

void WorldGeneratorPanel::wgen_apply_() {
    const ObjectId id = wgen_object_;
    const int ci = app_.doc_.find_component(id, "TileTerrain");
    if (ci < 0) return;
    Node comp = app_.doc_.find(id)->at("components").as_seq()[static_cast<std::size_t>(ci)];
    for (const char* key : toy::world::k_legacy_generator_keys) erase_key(comp, key);
    wgen_config_.image_size = coopa::maps::derive_image_size(wgen_config_);
    const Node gen = toy::world::generator_node(wgen_config_);
    if (gen.get_value<Node::mapping_type>().empty()) erase_key(comp, "generator");
    else comp["generator"] = gen;
    comp["world_tiles"] = Node(static_cast<int64_t>(wgen_world_tiles_));
    if (wgen_planet_) {
        comp["planet"] = Node(true);
        comp["face_tiles"] = Node(static_cast<int64_t>(wgen_face_tiles_));
        comp["gravity"] = make_float(wgen_gravity_);
    } else {
        // A flat terrain is written exactly as before the planet mode existed.
        erase_key(comp, "planet");
        erase_key(comp, "face_tiles");
        erase_key(comp, "gravity");
    }
    comp["tile_size"] = make_float(wgen_tile_size_);
    comp["height_scale"] = make_float(wgen_height_scale_);
    comp["water_inset"] = make_float(wgen_water_inset_);
    comp["river_inset"] = make_float(wgen_river_inset_);
    app_.apply_(app_.doc_.set_component(id, ci, comp, "World Generator"));
    wgen_regen_object_ = id;
    wgen_regen_expect_ = fkyaml::node::serialize(gen);
    wgen_regen_frames_ = 0;
}

void WorldGeneratorPanel::wgen_draw_preview_(imm::Context& ctx, const imm::Box& box) {
    ctx.fill(box, ctx.style.panel_alt);
    const WorldPreview& p = wgen_preview_;
    if (p.width <= 0 || p.height <= 0) return;
    const float side = std::min(box.w, box.h);
    // One drawn block per screen pixel at most: a 1024 px preview in a 420 px pane is sampled
    // every third pixel rather than filling three rectangles per pixel. The resolution still
    // tells, because the pane is square and the map keeps its own aspect.
    const int step = std::max(1, static_cast<int>(std::ceil(static_cast<float>(std::max(p.width, p.height)) / std::max(1.0f, side))));
    const float px = side / static_cast<float>(std::max(p.width, p.height)) * static_cast<float>(step);
    const int cols = (p.width + step - 1) / step;
    const int rows = (p.height + step - 1) / step;
    const float ox = box.x + (box.w - px * static_cast<float>(cols)) * 0.5f;
    const float oy = box.y + (box.h - px * static_cast<float>(rows)) * 0.5f;
    const auto pixel = [&p, step](int col, int row) {
        const std::size_t x = static_cast<std::size_t>(std::min(col * step, p.width - 1));
        const std::size_t y = static_cast<std::size_t>(std::min(row * step, p.height - 1));
        return &p.rgb[(y * static_cast<std::size_t>(p.width) + x) * 3];
    };
    // North (+Y, the map's last row) at the top, as the terrain lies in a top view. Runs of one
    // colour in a row are one rectangle -- the sea is most of a row.
    for (int row = 0; row < rows; ++row) {
        const float y = oy + static_cast<float>(rows - 1 - row) * px;
        int start = 0;
        for (int col = 1; col <= cols; ++col) {
            const std::uint8_t* a = pixel(start, row);
            bool same = false;
            if (col < cols) {
                const std::uint8_t* b = pixel(col, row);
                same = std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) <= 6;
            }
            if (same) continue;
            ctx.fill(imm::Box{ox + static_cast<float>(start) * px, y, static_cast<float>(col - start) * px + 0.5f, px + 0.5f},
                     glm::vec4(a[0] / 255.0f, a[1] / 255.0f, a[2] / 255.0f, 1.0f));
            start = col;
        }
    }
}

void WorldGeneratorPanel::draw_world_generator_modal_(imm::Context& ctx) {
    using I = imm::Icon;
    wgen_tick_preview_();
    const glm::vec2 canvas = ctx.canvas_size();
    const glm::vec2 size{std::min(1060.0f, canvas.x - 40.0f), std::min(700.0f, canvas.y - 40.0f)};
    if (!ctx.begin_modal("World Generator", size)) {
        if (wgen_object_ != 0 && app_.pending_modal_ != "World Generator") wgen_object_ = 0;   // closed (Esc)
        return;
    }
    const auto close = [&] {
        wgen_object_ = 0;
        ctx.close_modal();
    };
    if (wgen_close_ || wgen_object_ == 0 || app_.doc_.find_component(wgen_object_, "TileTerrain") < 0) {
        wgen_close_ = false;
        close();
        ctx.end_modal();
        return;
    }
    const glm::vec4 dim = ctx.style.text_dim;
    const glm::vec2 top = ctx.cursor();
    const float w = ctx.available_width();
    const float footer_h = ctx.style.row_height * 2 + 12;
    const float box_bottom = std::floor((canvas.y - size.y) * 0.5f) + size.y;   // begin_modal() centres the box
    const float body_h = std::max(120.0f, box_bottom - ctx.style.padding - footer_h - top.y);
    const float list_w = 150, preview_w = std::min(420.0f, w * 0.42f);
    const imm::Box list{top.x, top.y, list_w, body_h};
    const imm::Box pane{top.x + list_w + 6, top.y, w - list_w - preview_w - 12, body_h};
    const imm::Box right{pane.right() + 6, top.y, preview_w, body_h};
    bool edited = false;

    // Left: the categories.
    ctx.begin_region("wgen_categories", list, false, &ctx.style.panel_alt);
    for (const int i : k_category_order) {
        if (ctx.selectable(k_categories[i], i == wgen_category_)) wgen_category_ = i;
        app_.test_rects_[std::string("world_generator:") + k_categories[i]] = ctx.last_rect();
    }
    ctx.end_region();

    // Middle: the category's settings.
    ctx.begin_region("wgen_pane", pane, true);
    MapConfig& c = wgen_config_;
    char buf[200];
    if (wgen_category_ == 0) {
        ctx.paragraph("mapcoopa generates the world once; it is stored as tiles you then edit. These settings shape the next "
                      "generation. New terrains start from mapcoopa's standard map -- what the mapcoopa tool makes on its own.",
                      &dim);
        int seed = c.seed;
        if (ctx.drag_int("Seed", &seed, 0.2f, 0, 2000000000)) { c.seed = seed; edited = true; }
        ctx.tooltip("Seed\nThe same seed and settings always make the same world");
        app_.test_rects_["world_generator:seed"] = ctx.last_rect();
        if (ctx.button("New Random Seed", -1, true, I::Restart)) {
            std::random_device rd;
            c.seed = static_cast<int>(rd() % 1000000000u);
            edited = true;
        }
    }
    if (wgen_category_ == 1) {
        int shape = static_cast<int>(c.shape.shape);
        if (ctx.combo("Shape", &shape, shape_names())) { c.shape.shape = static_cast<MapShape>(shape); edited = true; }
        ctx.tooltip("Shape\nThe outline of the land: fill the map, one round island, a triangle, one continent, or many islands");
        // One size control, for whichever dimension the shape uses (as mapcoopa's --shape-size).
        double* size_m = nullptr;
        const char* size_label = "Size (map m)";
        switch (c.shape.shape) {
            case MapShape::Circle: size_m = &c.shape.diameter_m; size_label = "Diameter (map m, 0 fills)"; break;
            case MapShape::Triangle: size_m = &c.shape.edge_length_m; size_label = "Edge Length (map m, 0 fills)"; break;
            case MapShape::Continent:
            case MapShape::Archipelago: size_m = &c.shape.continent_size_m; size_label = "Landmass Size (map m, 0 fills)"; break;
            default: break;
        }
        if (size_m != nullptr) {
            float v = static_cast<float>(*size_m);
            if (ctx.drag_float(size_label, &v, 10.0f, 0.0f, 20000.0f)) { *size_m = v; edited = true; }
        }
        if (c.shape.shape == MapShape::Archipelago) {
            int n = c.shape.continent_count;
            if (ctx.drag_int("Islands", &n, 0.05f, 1, 24)) { c.shape.continent_count = n; edited = true; }
        }
        if (c.shape.shape == MapShape::Triangle) {
            float deg = static_cast<float>(c.shape.rotation * 57.29577951308232);
            if (ctx.slider_float("Rotation (deg)", &deg, 0.0f, 360.0f, "%.0f")) { c.shape.rotation = deg / 57.29577951308232; edited = true; }
        }
    }
    const bool natural = c.terrain_model == coopa::maps::TerrainModel::Natural;
    if (wgen_category_ == k_landforms) {
        int model = natural ? 1 : 0;
        if (ctx.combo("Terrain", &model, {"Classic", "Natural"})) {
            c.terrain_model = model == 1 ? coopa::maps::TerrainModel::Natural : coopa::maps::TerrainModel::Classic;
            edited = true;
        }
        ctx.tooltip("Terrain\nNatural: smooth ground shaped into mountain ranges, hills and plateaus. Classic: mapcoopa's "
                    "flat facets between its cells -- plain, and angular on a large world");
        ctx.paragraph(natural ? "The ground is joined smoothly between the map's cells, then shaped: ridged ranges on the "
                                "high ground, rolling hills and flat-topped plateaus on the lowland, bent so nothing lines "
                                "up. Near the sea and lakes it is only ever raised, so the water stays where mapcoopa put it."
                              : "Classic terrain joins the map's cells with flat facets. Switch to Natural for landforms.",
                      &dim);
    }
    for (const Row& row : rows()) {
        if (row.category != wgen_category_) continue;
        if (row.category == k_landforms && !natural) continue;
        if (row.integer) {
            int v = static_cast<int>(row.get(c));
            if (ctx.drag_int(row.label, &v, 0.1f, static_cast<int>(row.lo), static_cast<int>(row.hi))) {
                row.set(c, v);
                edited = true;
            }
        } else {
            float v = static_cast<float>(row.get(c));
            if (ctx.slider_float(row.label, &v, static_cast<float>(row.lo), static_cast<float>(row.hi), "%.3g")) {
                row.set(c, v);
                edited = true;
            }
        }
        app_.test_rects_[std::string("world_generator:") + row.label] = ctx.last_rect();
        ctx.tooltip(std::string(row.label) + "\n" + row.help);
    }
    if (wgen_category_ == 0) {
        ctx.spacing(6);
        ctx.label("The Terrain", &dim);
        if (ctx.checkbox("Planet", &wgen_planet_)) edited = true;
        ctx.tooltip("Planet\nA small round world: the map is generated on a sphere and its tiles wrap a globe, with "
                    "gravity pulling toward the centre. Off: a flat map, as always");
        app_.test_rects_["world_generator:Planet"] = ctx.last_rect();
        if (wgen_planet_) {
            ctx.drag_int("Face Size (tiles)", &wgen_face_tiles_, 0.5f, 16, 512);
            ctx.tooltip("Face Size\nTiles along one edge of each of the six faces the globe is made of; the "
                        "circumference is four of them");
            ctx.drag_float("Gravity (m/s2)", &wgen_gravity_, 0.05f, 0.0f, 50.0f);
            ctx.tooltip("Gravity\nThe planet's pull at its surface (Earth: 9.81). 0: none");
        } else {
            if (ctx.drag_int("World Size (tiles)", &wgen_world_tiles_, 1.0f, 16, 8192)) edited = true;
            ctx.tooltip("World Size\nHow many tiles per side the map is cut into -- the world's size, whatever the map's "
                        "cell count (Map Cells sets its detail)");
        }
        app_.test_rects_["world_generator:World Size (tiles)"] = ctx.last_rect();
        ctx.drag_float("Tile Size (m)", &wgen_tile_size_, 0.01f, 0.05f, 100.0f);
        ctx.tooltip("Tile Size\nWidth of one tile in world units; with World Size, the world's extent");
        if (ctx.drag_float("Height Range (m)", &wgen_height_scale_, 0.1f, 1.0f, 256.0f)) edited = true;
        ctx.tooltip("Height Range\nWorld height of the map's full height range, from sea floor to the highest peak");
        if (wgen_planet_) {
            const toy::world::PlanetLayout layout{wgen_face_tiles_, wgen_tile_size_};
            std::snprintf(buf, sizeof(buf), "Planet: radius %.0f m, %.0f m around, 6 x %d x %d tiles, heights up to %.0f m",
                          static_cast<double>(layout.radius()), 4.0 * wgen_face_tiles_ * wgen_tile_size_, wgen_face_tiles_,
                          wgen_face_tiles_, static_cast<double>(wgen_height_scale_));
            ctx.label(buf, &dim);
        } else {
            const int tiles = wgen_world_tiles_;
            const double count = static_cast<double>(tiles) * tiles;
            const double metres = static_cast<double>(tiles) * wgen_tile_size_;
            std::snprintf(buf, sizeof(buf), "World: %d x %d tiles = %.0f x %.0f m%s, heights up to %.0f m", tiles, tiles,
                          metres, metres, count >= 1.0e6 ? " (over a million tiles)" : "",
                          static_cast<double>(wgen_height_scale_));
            ctx.label(buf, &dim);
            std::snprintf(buf, sizeof(buf), "%.1f tiles per map cell", static_cast<double>(tiles) / std::max(1, c.grid_size));
            ctx.label(buf, &dim);
        }
    }
    if (wgen_category_ == 4) {
        ctx.spacing(6);
        ctx.slider_float("Sea & Lake Inset (steps)", &wgen_water_inset_, 0.05f, 0.95f, "%.2f");
        ctx.tooltip("Sea & Lake Inset\nHow far the water surface sits under the top of the terrace it fills; the shoreline is "
                    "where the surface meets the bank. Applies without regenerating");
        ctx.slider_float("River Inset (steps)", &wgen_river_inset_, 0.0f, 0.95f, "%.2f");
        ctx.tooltip("River Inset\nThe same for rivers: lower sits them higher in their channels, nearer the bank tops");
        ctx.paragraph("Lakes stand just under the lowest point of their shore and rivers under both banks, falling all the "
                      "way to the sea -- mapcoopa decides where water is; the terrain draws it as smooth surfaces over the "
                      "beds, whatever the tile set.",
                      &dim);
    }
    ctx.end_region();

    // Right: the preview.
    ctx.begin_region("wgen_preview", right, false);
    int layer = wgen_layer_;
    if (ctx.combo("Show", &layer, {"Map", "Biomes", "Elevation", "Water"}) && layer != wgen_layer_) {
        wgen_layer_ = layer;
        ++wgen_requested_;
        wgen_changed_at_ = 0.0;
    }
    if (ctx.drag_int("Detail (px)", &wgen_preview_px_, 2.0f, 96, 1024)) {
        ++wgen_requested_;
        wgen_changed_at_ = wgen_now();
    }
    ctx.tooltip("Detail\nPixels per side the preview map is drawn at. Higher resolves rivers, coasts and biome edges "
                "the small map merges away; it costs the preview thread time, not the editor's frame");
    app_.test_rects_["world_generator:Detail (px)"] = ctx.last_rect();
    const float map_side = std::min(right.w - 8.0f, right.h - ctx.style.row_height * 5 - 16.0f);
    // A planet's preview is the globe unrolled (twice as wide as tall).
    const imm::Box map = wgen_planet_ ? ctx.next_box(map_side, map_side * 0.5f) : ctx.next_box(map_side, map_side);
    wgen_draw_preview_(ctx, map);
    app_.test_rects_["world_generator:preview"] = map;
    if (wgen_job_.valid() || wgen_requested_ != wgen_started_) {
        ctx.label("Generating the preview...", &dim);
    } else if (wgen_preview_.width > 0) {
        std::snprintf(buf, sizeof(buf), "%d%% land, %d rivers, %d lake cells, %d towns", wgen_preview_.land_percent,
                      wgen_preview_.rivers, wgen_preview_.lakes, wgen_preview_.towns);
        ctx.label(buf, &dim);
    }
    ctx.label(wgen_planet_ ? "The whole globe, unrolled: the poles along the top and bottom."
                           : "North is up, as the terrain lies from above.",
              &dim);
    ctx.end_region();

    if (edited) {
        ++wgen_requested_;
        wgen_changed_at_ = wgen_now();
    }

    // Footer.
    ctx.set_cursor_y(top.y + body_h + 6);
    const toy::world::TerrainComponent* live = app_.tile_terrain_panel_->live_tile_terrain_(wgen_object_);
    if (live && live->tiles && !live->tiles->empty()) {
        ctx.label("Generate World replaces the terrain's tiles -- your tile edits included. It can be undone.", &dim);
    } else {
        ctx.label(" ", &dim);
    }
    if (ctx.button("Reset to Defaults", 180, true, I::Restart)) {
        wgen_config_ = toy::world::default_generator();
        const toy::world::TerrainParams defaults;
        wgen_world_tiles_ = wgen_config_.grid_size * defaults.tiles_per_grid_unit;
        wgen_tile_size_ = defaults.tile_size;
        wgen_height_scale_ = defaults.height_scale;
        wgen_water_inset_ = defaults.water_inset;
        wgen_river_inset_ = defaults.river_inset;
        wgen_face_tiles_ = defaults.face_tiles;
        wgen_gravity_ = 9.81f;
        ++wgen_requested_;
        wgen_changed_at_ = 0.0;
    }
    ctx.tooltip("Reset to Defaults\nThe standard map -- what the mapcoopa tool generates with no settings -- in the natural "
                "terrain model");
    ctx.same_line();
    const bool can_generate = !app_.playing() && !app_.tile_mode_panel_->tile_regen_data_ && !(live && live->baking());
    if (ctx.button("Generate World", 170, can_generate, I::World) || (wgen_generate_ && can_generate)) {
        wgen_generate_ = false;
        wgen_apply_();
        close();
        ctx.end_modal();
        return;
    }
    app_.test_rects_["world_generator:generate"] = ctx.last_rect();
    ctx.same_line();
    if (ctx.button("Cancel", 100)) close();
    ctx.end_modal();
}

} // namespace editor
} // namespace toy
