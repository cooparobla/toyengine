// game_module.cpp -- @NAME@'s C++ entry point into the engine (an example; rename or replace).
//
// Every .cpp under src/ is compiled into both build/@TARGET@ (the game) and
// build/@TARGET@_editor, so a component registered here loads in the game and in the editor's
// Play. Add it to a scene in the editor (Add Component > Gameplay > Spinner) or in YAML:
//
//   - type: Spinner
//     axis: {x: 0, y: 0, z: 1}
//     speed: 90
//
// TOY_MODULE's body runs inside every Engine constructor, after toyengine's own component
// parsers are registered and before the first scene loads (toyengine/core/module.h).

#include <toyengine/core/module.h>
#include <toyengine/scene/register.h>   // parse_vec3

#include <coopa/scene/component.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#if TOY_EDITOR
#include <editor/schema/component_schema.h>
#endif

namespace {

/** @brief Spins its object about `axis` (local space) at `speed` degrees per second. */
class Spinner : public coopa::scene::Component {
public:
    std::string type_name() const override { return "Spinner"; }

    glm::vec3 axis{0.0f, 0.0f, 1.0f};
    float speed = 90.0f;

    void update(float dt) override {
        if (!owner || dt <= 0.0f || glm::length(axis) < 1e-6f) return;
        auto* tc = owner->get_transform();
        if (!tc) return;
        coopa::util::Transform& t = tc->transform();
        const glm::quat step = glm::angleAxis(glm::radians(speed * dt), glm::normalize(axis));
        t.set_rotation_quat(glm::normalize(t.rotation_quat() * step));
    }
};

}  // namespace

TOY_MODULE(@TARGET@) {
    using coopa::scene::SceneLoader;
    SceneLoader::register_component_parser("Spinner",
        [](const fkyaml::node& node, coopa::scene::SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* s = obj.add_component<Spinner>();
            if (node.contains("axis")) s->axis = toy::scene::parse_vec3(node.at("axis"), s->axis);
            if (node.contains("speed")) s->speed = node.at("speed").get_value<float>();
        });
}

#if TOY_EDITOR
// How the inspector shows Spinner and what "Add Component" writes for it.
[[maybe_unused]] static const bool spinner_schema = toy::editor::register_component_schema(
    {"Spinner", "Gameplay", {
        toy::editor::f_vec3("axis", glm::vec3(0.0f, 0.0f, 1.0f), 0.01f, true),
        toy::editor::f_float("speed", 90.0f, 1.0f, -3600.0f, 3600.0f, true),
    }});
#endif
