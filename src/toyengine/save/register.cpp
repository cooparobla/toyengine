#include <toyengine/save/register.h>

#include <coopa/scene/scene_loader.h>
#include <toyengine/save/save_demo.h>
#include <toyengine/save/saveable.h>

namespace toy {
namespace save {

void register_save_components() {
    using coopa::scene::SceneLoader;
    using coopa::scene::SceneObject;

    SceneLoader::register_component_parser("SaveId",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* sid = obj.add_component<SaveId>();
            if (!node.contains("id")) return;
            const fkyaml::node& id = node.at("id");
            if (id.is_string()) sid->id = id.get_value<std::string>();
            else if (id.is_integer()) sid->id = std::to_string(id.get_value<int64_t>());
        });

    SceneLoader::register_component_parser("SaveDemo",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* d = obj.add_component<SaveDemo>();
            if (node.contains("coin_prefix")) d->coin_prefix = node.at("coin_prefix").get_value<std::string>();
            if (node.contains("pickup_radius")) d->pickup_radius = node.at("pickup_radius").get_value<float>();
        });
}

} // namespace save
} // namespace toy
