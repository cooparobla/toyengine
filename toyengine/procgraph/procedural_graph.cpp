/**
 * @file procedural_graph.cpp
 * @brief The component's YAML parser and its state key.
 */

#include <toyengine/procgraph/procedural_graph.h>

#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>

#include <toyengine/core/yaml_read.h>

#include <iostream>
#include <set>

namespace toy {
namespace procgraph {
namespace {

/// @brief One line per distinct message, so a parse warning cannot spam a scene load.
void warn_once_(const std::string& line) {
    static std::set<std::string> seen;
    if (!seen.insert(line).second) return;
    std::cerr << line << "\n";
}

} // namespace

bool parse_bake_mode(const std::string& name, BakeMode& out) {
    if (name == "on_load") { out = BakeMode::OnLoad; return true; }
    if (name == "on_change") { out = BakeMode::OnChange; return true; }
    if (name == "manual") { out = BakeMode::Manual; return true; }
    return false;
}

const char* bake_mode_name(BakeMode mode) {
    switch (mode) {
        case BakeMode::OnChange: return "on_change";
        case BakeMode::Manual:   return "manual";
        case BakeMode::OnLoad:   break;
    }
    return "on_load";
}

std::string ProceduralGraph::state_key() const {
    // The owner's name plus the graph path. Deliberately NOT the component's own address: the
    // editor recreates the component on every inspector keystroke, so an address-keyed entry
    // would be orphaned on each one and the bake would be lost (or worse, kept against a dangling
    // pointer). Two objects sharing both a name and a graph would share state, which is a real if
    // unlikely limitation -- the alternative needs a persisted per-object id the engine has not got.
    const std::string object_name = owner ? owner->name() : std::string("<unowned>");
    return object_name + "|" + graph;
}

void register_procgraph_components() {
    using coopa::scene::SceneLoader;
    using coopa::scene::SceneObject;
    using core::yaml::read;

    SceneLoader::register_component_parser(
        "ProceduralGraph",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* pg = obj.add_component<ProceduralGraph>();
            read(node, "graph", pg->graph);
            read(node, "refresh_on_asset_change", pg->refresh_on_asset_change);

            float seed = static_cast<float>(pg->seed);
            if (read(node, "seed", seed)) pg->seed = static_cast<int32_t>(seed);

            std::string mode;
            if (read(node, "bake", mode) && !parse_bake_mode(mode, pg->bake)) {
                // Said once rather than silently ignored: a typo'd `bake:` would otherwise look
                // exactly like a graph that refuses to re-bake. Matches the de-duplicated cerr
                // line InstancePainterSystem::log_once_ uses.
                warn_once_("[ProceduralGraph] unknown bake mode '" + mode + "'; using on_load");
            }

            if (node.is_mapping() && node.contains("inputs") && node.at("inputs").is_mapping()) {
                const fkyaml::node& inputs = node.at("inputs");
                for (auto it = inputs.begin(); it != inputs.end(); ++it) {
                    const std::string key = it.key().get_value<std::string>();
                    pg->inputs[key] = parse_input_binding(it.value());
                }
            }
        });
}

} // namespace procgraph
} // namespace toy
