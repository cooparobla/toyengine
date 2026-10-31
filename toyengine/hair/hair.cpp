#include "toyengine/hair/hair.h"

#include <vector>

#include <coopa/asset/asset_manager.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <gfxcoopa/engine/components/register.h>

#include "toyengine/core/yaml_read.h"

namespace toy {
namespace hair {

namespace {

using coopa::scene::SceneLoader;
using coopa::scene::SceneObject;
using namespace toy::core::yaml;

/**
 * Reads a curve written either as the inspector's `[[x, y], ...]` (FieldKind::Curve's shape, and
 * what the editor writes) or as the particle modules' `[{t:, value:}, ...]`. Both spellings are
 * accepted because an author reaching for one after seeing the other should not get silence.
 */
bool read_curve(const fkyaml::node& node, const char* key, particles::FloatCurve& out) {
    if (!node.contains(key)) return false;
    const fkyaml::node& seq = node.at(key);
    if (!seq.is_sequence()) return false;

    std::vector<glm::vec2> keys;
    keys.reserve(seq.size());
    for (const fkyaml::node& entry : seq) {
        if (entry.is_sequence() && entry.size() >= 2) {
            keys.emplace_back(num(entry[0]), num(entry[1]));
        } else if (entry.is_mapping() && entry.contains("t")) {
            const float value = entry.contains("value") ? num(entry.at("value"))
                                                        : (entry.contains("y") ? num(entry.at("y")) : 1.0f);
            keys.emplace_back(num(entry.at("t")), value);
        }
    }
    if (keys.empty()) return false;
    out.set_keys(std::move(keys));
    return true;
}

} // namespace

std::string Hair::state_key() const {
    return owner != nullptr ? owner->name() : std::string("<orphan>");
}

void register_hair_components(coopa::asset::AssetManager& assets) {
    SceneLoader::register_component_parser(
        "Hair", [&assets](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* h = obj.add_component<Hair>();
            HairParams& p = h->params;

            // The scene directory, kept for HairSystem: it loads the scalp's CPU mesh and the root
            // mask, and by then there is no ParseContext to resolve against.
            h->asset_base_dir = ctx.base_dir();
            read(node, "scalp_mesh", h->scalp_mesh);

            std::string shape;
            if (read(node, "shape", shape)) p.shape = parse_hair_shape(shape);
            std::string regenerate;
            if (read(node, "regenerate", regenerate)) p.regenerate = parse_hair_regenerate(regenerate);

            read(node, "cross_section", p.cross_section);
            read(node, "sides", p.sides);
            read(node, "segments", p.segments);

            read(node, "density", p.density);
            read(node, "max_strands", p.max_strands);
            read(node, "length", p.length);
            read(node, "length_jitter", p.length_jitter);
            read(node, "thickness", p.thickness);
            read_curve(node, "thickness_over_length", p.thickness_over_length);

            read(node, "droop", p.droop);
            read(node, "clump", p.clump);
            read(node, "clump_density", p.clump_density);
            read(node, "curl", p.curl);
            read(node, "curl_radius", p.curl_radius);
            read(node, "frizz_degrees", p.frizz_degrees);

            read(node, "root_mask", p.root_mask);
            read(node, "guides", p.guides);
            read(node, "guide_influence", p.guide_influence);
            read(node, "guide_count", p.guide_count);

            read(node, "wind_flexibility", p.wind_flexibility);
            read(node, "wind_frequency", p.wind_frequency);

            read(node, "simulate", p.simulate);
            read(node, "weather_wind", h->weather_wind);
            read(node, "sim_guides", p.sim_guides);
            read(node, "sim_stiffness", p.sim_stiffness);
            read(node, "sim_stiffness_falloff", p.sim_stiffness_falloff);
            read(node, "sim_curl_hold", p.sim_curl_hold);
            read(node, "sim_damping", p.sim_damping);
            read(node, "sim_drag", p.sim_drag);
            read(node, "sim_collision_thickness", p.sim_collision_thickness);
            read(node, "sim_friction", p.sim_friction);
            read(node, "sim_gravity", p.sim_gravity);

            read(node, "lod_screen_sizes", p.lod_screen_sizes);
            read(node, "cull_screen_size", p.cull_screen_size);
            read(node, "seed", p.seed);

            p.sanitize();

            // Defaults before any authored material, so `material:` overrides them rather than
            // being overwritten by them. Two-sided because a ribbon is seen from both faces, and
            // the `hair` shader because that is where the sway and the strand highlight live.
            h->material.shader          = "hair";
            h->material.cull_backfaces  = false;
            if (node.contains("material")) {
                coopa::gfx::engine::components::parse_material_value_(node.at("material"), h->material,
                                                                      assets, ctx);
            }
        });
}

} // namespace hair
} // namespace toy
