#include "editor/app/ui/tile_terrain_panel.h"
#include "editor/app/ui/tile_mode.h"
#include "editor/app/ui/assets.h"
#include "editor/app/ui/world_generator.h"

#include "editor/app/editor_app.h"

#include <coopa/maps/map_config.h>
#include <toyengine/world/terrain_generator.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <random>
#include <set>

#include <physxcoopa/components/mesh_collider.h>
#include <toyengine/world/terrain_component.h>
#include <toyengine/world/tile_types.h>

namespace toy {
namespace editor {

using toy::world::TerrainComponent;

namespace {

/// Surface kinds in TileKind order, as YAML spells them (tile_kind_from_name()).
const char* const k_kind_names[toy::world::k_tile_kind_count] = {
    "grass", "dirt", "stone", "sand", "snow", "rock", "water", "ice", "moss", "clay", "ash",
    "salt",  "road"};

/** @brief The schema's field for `key`, relabelled / re-explained for the panel. */
FieldDesc tfield(const std::string& key, const char* label = nullptr, const char* tip = nullptr) {
    static const ComponentSchema* schema = find_schema("TileTerrain");
    FieldDesc f;
    if (schema) {
        for (const auto& s : schema->fields) {
            if (s.key == key) { f = s; break; }
        }
    }
    if (f.key.empty()) f.key = key;
    if (label) f.label = label;
    if (tip) f.tooltip = tip;
    return f;
}

std::string str_of(const Node& n, const char* key, const std::string& def) {
    return n.contains(key) && n.at(key).is_string() ? get_string(n, key, def) : def;
}

float num_of(const Node& n, const char* key, float def) {
    if (!n.contains(key)) return def;
    const Node& v = n.at(key);
    if (v.is_float_number()) return static_cast<float>(v.get_value<double>());
    if (v.is_integer()) return static_cast<float>(v.get_value<int64_t>());
    return def;
}

/// `objects/terrain/tileset_round.yaml` -> `objects/terrain/tileset_round` (how `tile_set:` is written).
std::string tile_set_ref(std::string rel) {
    if (rel.size() > 5 && rel.compare(rel.size() - 5, 5, ".yaml") == 0) rel.resize(rel.size() - 5);
    return rel;
}

/// `objects/terrain/tileset_round` -> `tileset_round`.
std::string tile_set_label(const std::string& ref) {
    const std::size_t slash = ref.find_last_of('/');
    return slash == std::string::npos ? ref : ref.substr(slash + 1);
}

/// Index of `ref` in `sets`, or -1. A reference also matches by name: assets resolve by name
/// when their folder is not written (`objects/tileset_round` finds objects/terrain/tileset_round).
int find_tile_set(const std::vector<std::string>& sets, const std::string& ref) {
    if (ref.empty()) return -1;
    for (std::size_t i = 0; i < sets.size(); ++i) if (sets[i] == ref) return static_cast<int>(i);
    for (std::size_t i = 0; i < sets.size(); ++i) {
        if (tile_set_label(sets[i]) == tile_set_label(ref)) return static_cast<int>(i);
    }
    return -1;
}

} // namespace

TerrainComponent* TileTerrainPanel::live_tile_terrain_(ObjectId id) const {
    coopa::scene::SceneObject* live = app_.sync_.live(id);
    return live ? live->get_component<TerrainComponent>() : nullptr;
}

const std::vector<std::string>& TileTerrainPanel::tile_set_assets_() {
    // The project's own objects, plus toyengine's (the stock sets) in a game project -- offered
    // whether or not the browser is showing engine assets: they are what a terrain starts from.
    std::vector<std::string> objects = app_.project_.list("objects", ".yaml");
    if (!app_.project_.is_engine()) {
        for (auto& rel : app_.project_.list_engine("objects", ".yaml")) objects.push_back(std::move(rel));
    }
    if (objects != tile_set_cache_keys_) {
        tile_set_cache_.clear();
        std::set<std::string> seen;
        for (const std::string& rel : objects) {
            if (!app_.assets_panel_->is_tile_set_(rel)) continue;
            const std::string ref = tile_set_ref(rel);
            if (seen.insert(ref).second) tile_set_cache_.push_back(ref);
        }
        std::sort(tile_set_cache_.begin(), tile_set_cache_.end());
        tile_set_cache_keys_ = std::move(objects);
    }
    return tile_set_cache_;
}

bool TileTerrainPanel::convert_legacy_tile_terrain(Node& comp) {
    bool changed = false;
    if (comp.contains("styles") && comp.at("styles").is_mapping()) {
        const Node styles = comp.at("styles");
        const Node kinds = comp.contains("kind_styles") && comp.at("kind_styles").is_mapping() ? comp.at("kind_styles") : Node::mapping();
        auto set_of = [&styles](const std::string& name) {
            return styles.contains(name) && styles.at(name).is_string() ? styles.at(name).get_value<std::string>() : std::string();
        };
        // The default set: kind_styles.default, else the first style written.
        std::string def;
        if (kinds.contains("default") && kinds.at("default").is_string()) def = set_of(kinds.at("default").get_value<std::string>());
        if (def.empty()) {
            for (const std::string& k : keys_of(styles)) {
                def = set_of(k);
                if (!def.empty()) break;
            }
        }
        if (!def.empty()) comp["tile_set"] = Node(def);
        Node overrides = Node::mapping();
        for (const std::string& kind : keys_of(kinds)) {
            if (kind == "default" || !kinds.at(kind).is_string()) continue;
            const std::string set = set_of(kinds.at(kind).get_value<std::string>());
            if (!set.empty() && set != def) overrides[kind] = Node(set);
        }
        if (overrides.size() > 0) comp["kind_tile_sets"] = overrides;
        comp.as_map().erase("styles");
        comp.as_map().erase("kind_styles");
        changed = true;
    } else if (comp.contains("side_mesh") || comp.contains("sides")) {
        // The retired voxel mesher: its look is the voxel tile set now.
        if (!comp.contains("tile_set")) comp["tile_set"] = Node(std::string(TerrainComponent::k_default_tile_set));
        changed = true;
    }
    for (const char* key : {"side_mesh", "sides", "greedy_merge", "emit_bottom"}) {
        if (comp.contains(key)) {
            comp.as_map().erase(key);
            changed = true;
        }
    }
    return changed;
}

EditResult TileTerrainPanel::draw_tile_terrain_panel_(imm::Context& ctx, ObjectId id, Node& comp, const InspectorEnv& env) {
    using I = imm::Icon;
    EditResult r;
    std::set<std::string> placed{"type", "id"};
    const glm::vec4 dim = ctx.style.text_dim;

    // Draws the listed fields (in order) and marks them placed.
    auto fields = [&](std::initializer_list<FieldDesc> list) {
        std::vector<FieldDesc> v(list);
        for (const auto& f : v) placed.insert(f.key);
        r.absorb(draw_fields(ctx, v, comp, env, false));
    };
    auto mark = [&](std::initializer_list<const char*> keys) { for (const char* k : keys) placed.insert(k); };
    auto section = [&](const char* title, I icon, const char* what, bool open) {
        const bool o = ctx.collapsing_header(title, open, nullptr, icon);
        if (o && what) ctx.paragraph(what, &dim);
        return o;
    };
    auto changed_key = [&r](const char* key) {
        r.changed = r.finished = true;
        r.key = key;
    };
    char buf[200];

    // --- What it is ------------------------------------------------------------------------
    ctx.paragraph("A streamed tile world generated by mapcoopa: continents, rivers and biomes, cut into terraced "
                  "columns and built from a tile set's pieces in chunks around the camera.",
                  &dim);
    if (TerrainComponent* live = live_tile_terrain_(id)) {
        int chunks = 0, building = 0, colliders = 0, dormant = 0;
        for (const auto& [coord, chunk] : live->chunks()) {
            if (chunk.state == toy::world::ChunkState::Live) {
                ++chunks;
                if (chunk.object && chunk.object->get_component<coopa::physx::components::MeshCollider>()) ++colliders;
            } else if (chunk.state == toy::world::ChunkState::Dormant) {
                ++dormant;   // kept whole off the tree, ready to be re-attached
            } else if (chunk.state != toy::world::ChunkState::Retiring) {
                ++building;
            }
        }
        if (live->baking() || !live->tiles || live->tiles->empty()) {
            std::snprintf(buf, sizeof(buf), "Live: generating the world...");
        } else if (!live->library().is_baked()) {
            std::snprintf(buf, sizeof(buf), "Live: loading the tile set...");
        } else if (building > 0) {
            std::snprintf(buf, sizeof(buf), "Live: %d chunks drawn, %d building, %d with collision, %d dormant",
                          chunks, building, colliders, dormant);
        } else {
            std::snprintf(buf, sizeof(buf), "Live: %d chunks drawn, %d with collision, %d dormant", chunks,
                          colliders, dormant);
        }
        ctx.label(buf, &dim);
        if (live->tiles && !live->tiles->empty()) {
            std::snprintf(buf, sizeof(buf), "Tiles: %d x %d in %s%s", live->tiles->width(), live->tiles->height(),
                          live->tiles_file.c_str(), live->tiles->dirty_on_disk() ? " -- unsaved (Ctrl S)" : "");
            ctx.label(buf, &dim);
        }
    }
    if (!app_.playing()) {
        if (ctx.button("Edit Tiles", -1, true, I::Terrain)) app_.tile_mode_panel_->enter_tile_mode_(id);
        ctx.tooltip("Tile Mode\nRaise, flatten, paint and flood tiles with square brushes; select tiles and edit their "
                    "height, surface, biome and climate");
    }

    // --- Old keys ------------------------------------------------------------------------------
    const bool legacy = comp.contains("styles") || comp.contains("side_mesh") || comp.contains("sides") ||
                        comp.contains("greedy_merge") || comp.contains("emit_bottom");
    if (legacy) {
        ctx.label("This terrain uses the old tile keys (styles / side_mesh).", &ctx.style.warning);
        if (ctx.button("Convert to Tile Set", -1, true, I::Restart)) {
            if (convert_legacy_tile_terrain(comp)) changed_key("tile_set");
        }
        ctx.tooltip("Convert to Tile Set\nRewrites styles / kind_styles as Tile Set + per-surface overrides, and drops "
                    "the retired voxel keys -- the terrain looks the same");
    }
    mark({"styles", "kind_styles", "side_mesh", "sides", "greedy_merge", "emit_bottom"});

    // --- Tiles -----------------------------------------------------------------------------
    const std::vector<std::string>& sets = tile_set_assets_();
    if (section("Tiles", I::Terrain,
                "The tile set is the template the terrain is built from: one object whose pieces shape every top, "
                "edge, corner and wall. Change it to change the terrain's whole form -- tileset_round for soft "
                "rounded cliffs, tileset_voxel for square blocks.",
                true)) {
        const std::string cur = tile_set_ref(str_of(comp, "tile_set", legacy ? std::string() : std::string("objects/terrain/tileset_voxel")));
        std::vector<std::string> labels;
        for (const auto& set : sets) labels.push_back(tile_set_label(set));
        int index = find_tile_set(sets, cur);
        if (index < 0) {
            labels.push_back(cur.empty() ? std::string("(none)") : tile_set_label(cur) + " (missing)");
            index = static_cast<int>(labels.size()) - 1;
        }
        const int before = index;
        if (ctx.combo("Tile Set", &index, labels) && index != before && index < static_cast<int>(sets.size())) {
            comp["tile_set"] = Node(sets[static_cast<std::size_t>(index)]);
            changed_key("tile_set");
        }
        ctx.tooltip("Tile Set\nA tile-set object (Objects tab, marked tile_set: true). Its eleven pieces -- top quadrants, "
                    "wall caps and walls for straight runs, outer and inner corners -- are placed by each tile's "
                    "neighbours. " + cur);
        if (env.after_field) env.after_field(ctx, "tile_set", ctx.last_label_box());
        placed.insert("tile_set");

        const bool known = before < static_cast<int>(sets.size());
        const std::string shown = known ? sets[static_cast<std::size_t>(before)] : cur;
        if (ctx.button("Open", 90, known, I::Object)) app_.assets_panel_->open_asset(AssetType::Object, shown + ".yaml");
        ctx.tooltip("Open\nShows every piece laid out; Tab into one to reshape its mesh");
        ctx.same_line();
        if (ctx.button("Duplicate as New Tile Set", -1, known, I::Duplicate)) {
            const std::string copy = app_.assets_panel_->duplicate_tile_set(shown + ".yaml");
            if (!copy.empty()) {
                comp["tile_set"] = Node(tile_set_ref(copy));
                changed_key("tile_set");
            }
        }
        ctx.tooltip("Duplicate as New Tile Set\nCopies this tile set (and its piece meshes) into the project and points "
                    "this terrain at the copy, ready to reshape");

        // Per-surface overrides: kind -> tile set, written as `kind_tile_sets:`.
        Node overrides = comp.contains("kind_tile_sets") && comp.at("kind_tile_sets").is_mapping() ? comp.at("kind_tile_sets")
                                                                                                    : Node::mapping();
        const Node overrides_before = overrides;
        if (ctx.collapsing_header("Per-Surface Tile Sets", overrides.size() > 0, nullptr, I::Palette)) {
            ctx.paragraph("Build some surfaces from a different tile set: chunky rock for stone cliffs, square blocks for "
                          "water. A column takes its top surface's set, cliff included.",
                          &dim);
            std::vector<std::string> options{"(main tile set)"};
            for (const auto& s : sets) options.push_back(tile_set_label(s));
            for (std::size_t k = 0; k < toy::world::k_tile_kind_count; ++k) {
                const char* kind = k_kind_names[k];
                const std::string set = overrides.contains(kind) ? tile_set_ref(str_of(overrides, kind, "")) : std::string();
                std::vector<std::string> row = options;
                int pick = find_tile_set(sets, set) + 1;
                if (pick == 0 && !set.empty()) {
                    row.push_back(tile_set_label(set) + " (missing)");
                    pick = static_cast<int>(row.size()) - 1;
                }
                std::string label = kind;
                label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
                ctx.push_id(kind);
                const int was = pick;
                if (ctx.combo(label, &pick, row) && pick != was) {
                    if (pick == 0) overrides.as_map().erase(kind);
                    else if (pick <= static_cast<int>(sets.size())) overrides[kind] = Node(sets[static_cast<std::size_t>(pick - 1)]);
                }
                ctx.pop_id();
            }
        }
        if (!(overrides == overrides_before)) {
            if (overrides.size() > 0) comp["kind_tile_sets"] = overrides;
            else comp.as_map().erase("kind_tile_sets");
            changed_key("kind_tile_sets");
        }
    }
    mark({"tile_set", "kind_tile_sets"});

    // --- Material ----------------------------------------------------------------------------
    if (section("Material", I::Material,
                "Colours the surfaces: the atlas holds one colour per surface kind (grass, sand, stone...); detail is "
                "added by the terrain shader. Without a texture the stock textures/terrain_atlas.png is used.",
                false)) {
        r.absorb(draw_material_field(ctx, comp, env, [this](const std::string& rel) { app_.assets_panel_->open_asset(AssetType::Material, rel); }));
    }
    mark({"material"});

    // --- Generator -------------------------------------------------------------------------
    if (section("Generator", I::World,
                "The world is generated once by mapcoopa and stored as tiles; from then on you edit the tiles (Edit "
                "Tiles). The World Generator holds every mapcoopa setting, with a live preview of the map they make.",
                true)) {
        const coopa::maps::MapConfig gen = toy::world::generator_config_from_node(comp);
        const bool legacy_keys = std::any_of(toy::world::k_legacy_generator_keys.begin(), toy::world::k_legacy_generator_keys.end(),
                                             [&](const char* k) { return comp.contains(k); });
        const bool standard = toy::world::generator_node(gen).get_value<Node::mapping_type>().empty();
        std::snprintf(buf, sizeof(buf), "%s: seed %d, %s, %d map cells, sea level %.2f, %d rivers",
                      standard ? "Default map (natural terrain)" : "Custom map", gen.seed,
                      std::string(coopa::maps::map_shape_name(gen.shape.shape)).c_str(), gen.grid_size, gen.sea_level,
                      gen.river_count);
        ctx.label(buf, &dim);
        const float world_tiles = num_of(comp, "world_tiles", 0.0f);
        const float tiles = world_tiles > 0.0f ? world_tiles : static_cast<float>(gen.grid_size) * num_of(comp, "tiles_per_grid_unit", 4.0f);
        std::snprintf(buf, sizeof(buf), "Makes %.0f x %.0f tiles (%.0f m square), %.0f m from sea floor to peak", tiles, tiles,
                      tiles * num_of(comp, "tile_size", 1.0f), num_of(comp, "height_scale", 16.0f));
        ctx.label(buf, &dim);
        if (legacy_keys) ctx.label("Uses the old flat generator keys; the World Generator moves them into its block.", &dim);
        if (ctx.button("Open World Generator...", -1, !app_.playing(), I::World)) app_.world_generator_panel_->open_world_generator(id);
        ctx.tooltip("World Generator\nEvery mapcoopa setting -- shape, elevation, climate, rivers, settlements -- with a live "
                    "preview; Generate World re-bakes the tiles from them");
        TerrainComponent* live = live_tile_terrain_(id);
        const bool busy = live == nullptr || live->baking() || app_.tile_mode_panel_->tile_regen_data_ != nullptr;
        if (ctx.button(live && live->baking() ? "Regenerating..." : "Regenerate World", -1, !busy && !app_.playing(), I::Restart)) {
            app_.tile_mode_panel_->tile_regenerate_(id);
        }
        ctx.tooltip("Regenerate World\nReplace every tile with a fresh world from the current settings (edits included); "
                    "undoable");
    }
    mark({"generator", "height_scale", "water"});

    // --- Water -------------------------------------------------------------------------------
    if (section("Water", I::World,
                "Where the water is comes from the generator; these set how it sits and looks, and apply at once.",
                false)) {
        fields({tfield("water_inset", "Sea & Lake Inset (steps)"), tfield("river_inset", "River Inset (steps)")});
    }
    mark({"water_inset", "river_inset"});
    for (const char* k : toy::world::k_legacy_generator_keys) placed.insert(k);

    // --- Size & Terraces ---------------------------------------------------------------------
    if (section("Size & Terraces", I::Scale, "How big each tile and step is in the world, and how cliffs are drawn.", true)) {
        fields({tfield("world_tiles", "World Size (tiles)")});
        ctx.tooltip("World Size\nTiles per side the generated map is cut into (0: Map Cells x Tiles per Cell). Applies when "
                    "the world is generated (World Generator / Regenerate World)");
        if (num_of(comp, "world_tiles", 0.0f) <= 0.0f) fields({tfield("tiles_per_grid_unit", "Tiles per Cell")});
        mark({"tiles_per_grid_unit"});
        fields({tfield("tile_size", "Tile Size (m)"), tfield("height_step", "Step Height (m)")});
        if (TerrainComponent* live = live_tile_terrain_(id); live && live->tiles && !live->tiles->empty()) {
            const float metres = static_cast<float>(live->tiles->width()) * num_of(comp, "tile_size", 1.0f);
            std::snprintf(buf, sizeof(buf), "World: %d x %d tiles, %.0f m square", live->tiles->width(), live->tiles->height(), metres);
            ctx.label(buf, &dim);
        }
        fields({tfield("soil_depth_steps", "Soil Depth (steps)"), tfield("max_wall_steps", "Max Wall (steps)")});
    }
    mark({"tile_size", "height_step", "soil_depth_steps", "max_wall_steps", "tiles"});

    // --- Streaming ---------------------------------------------------------------------------
    if (section("Streaming", I::Monitor, "Chunks are built around the camera and dropped behind it -- the main cost control.",
                false)) {
        fields({tfield("chunk_size", "Chunk Size (tiles)"), tfield("view_radius", "View Radius (chunks)")});
        const float side = 2.0f * num_of(comp, "view_radius", 3.0f) + 1.0f;
        const float metres = side * num_of(comp, "chunk_size", 16.0f) * num_of(comp, "tile_size", 1.0f);
        std::snprintf(buf, sizeof(buf), "Loaded: %.0f chunks, %.0f m square around the camera", side * side, metres);
        ctx.label(buf, &dim);
        fields({tfield("max_chunk_jobs_per_frame", "Jobs per Frame"),
                tfield("max_chunks_in_flight", "Chunks in Flight")});
        fields({tfield("upload_budget_ms", "Upload Budget (ms)"), tfield("dormant_chunks", "Kept Dormant")});
        fields({tfield("shadow_distance", "Shadow Distance (m)")});
    }
    mark({"chunk_size", "view_radius", "max_chunk_jobs_per_frame", "max_chunks_in_flight",
          "upload_budget_ms", "dormant_chunks", "shadow_distance"});

    // --- Collision ---------------------------------------------------------------------------
    if (section("Collision", I::Collider, "Each chunk collides with exactly the surface it draws, whichever tile set built it.",
                false)) {
        fields({tfield("collision", "Collision"), tfield("physics_material", "Physics Material")});
        fields({tfield("collision_radius", "Collision Radius (chunks)")});
        const float radius = num_of(comp, "collision_radius", 4.0f);
        if (radius > 0.0f) {
            std::snprintf(buf, sizeof(buf), "Solid within %.0f m; ground past that is not",
                          radius * num_of(comp, "chunk_size", 16.0f) * num_of(comp, "tile_size", 1.0f));
            ctx.label(buf, &dim);
        }
    }
    mark({"collision", "physics_material", "collision_radius"});

    // Anything the panel does not place itself (new or unusual keys) stays reachable.
    std::vector<FieldDesc> rest;
    if (const ComponentSchema* schema = find_schema("TileTerrain")) {
        for (const auto& f : schema->fields) {
            if (f.kind == FieldKind::Material || placed.count(f.key)) continue;
            rest.push_back(f);
        }
    }
    bool unknown = false;
    for (const std::string& k : keys_of(comp)) unknown = unknown || (!placed.count(k) && k.rfind("__", 0) != 0);
    if ((!rest.empty() || unknown) && section("Other", I::Gear, nullptr, false)) {
        r.absorb(draw_fields(ctx, rest, comp, env, true, placed));
    }
    return r;
}

} // namespace editor
} // namespace toy
