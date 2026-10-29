/**
 * @file curve.cpp
 * @brief The `Curve` scene parser.
 *
 * The YAML shape, which the editor's saver also writes:
 *
 * ```yaml
 * - type: Curve
 *   closed: false
 *   resolution: 12
 *   points:
 *     - {p: [0, 0, 0], in: [-1, 0, 0], out: [1, 0, 0], mode: aligned, tilt: 0}
 * ```
 *
 * `in`/`out` are absolute positions in the owner's local space, not offsets -- the same form the
 * pen tool drags, so what is saved is exactly what was authored.
 */

#include "toyengine/curve/curve.h"

#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>

#include <toyengine/core/yaml_read.h>

namespace toy {
namespace curve {
namespace {

/// @brief A `[x, y, z]` sequence, or a `{x:, y:, z:}` mapping; unchanged when absent.
bool read_vec3_(const fkyaml::node& node, const char* key, glm::vec3& out) {
    if (!node.is_mapping() || !node.contains(key)) return false;
    const fkyaml::node& v = node.at(key);
    if (v.is_sequence() && v.size() >= 3) {
        out.x = static_cast<float>(v[0].get_value<double>());
        out.y = static_cast<float>(v[1].get_value<double>());
        out.z = static_cast<float>(v[2].get_value<double>());
        return true;
    }
    return core::yaml::read(node, key, out);
}

HandleMode parse_mode_(const std::string& name) {
    if (name == "free") return HandleMode::Free;
    if (name == "vector") return HandleMode::Vector;
    if (name == "auto") return HandleMode::Auto;
    return HandleMode::Aligned;
}

} // namespace

const char* handle_mode_name(HandleMode mode) {
    switch (mode) {
        case HandleMode::Free:   return "free";
        case HandleMode::Vector: return "vector";
        case HandleMode::Auto:   return "auto";
        case HandleMode::Aligned:
        default:                 return "aligned";
    }
}

void register_curve_components() {
    using coopa::scene::SceneLoader;
    using coopa::scene::SceneObject;
    using core::yaml::read;

    SceneLoader::register_component_parser(
        "Curve",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* c = obj.add_component<Curve>();
            read(node, "closed", c->data.closed);
            read(node, "always_show", c->always_show);

            float resolution = static_cast<float>(c->data.resolution);
            if (read(node, "resolution", resolution)) {
                c->data.resolution = static_cast<int>(resolution);
            }

            if (!node.contains("points") || !node.at("points").is_sequence()) return;
            for (const auto& pn : node.at("points").as_seq()) {
                CurvePoint p;
                read_vec3_(pn, "p", p.position);
                // Handles default to the point itself; refresh_handles() then derives them for
                // the modes that have derived handles, so a file may list positions alone.
                p.in_handle  = p.position;
                p.out_handle = p.position;
                read_vec3_(pn, "in", p.in_handle);
                read_vec3_(pn, "out", p.out_handle);
                read(pn, "tilt", p.tilt);
                std::string mode;
                if (read(pn, "mode", mode)) p.mode = parse_mode_(mode);
                c->data.points.push_back(p);
            }
            refresh_handles(c->data);
        });
}

} // namespace curve
} // namespace toy
