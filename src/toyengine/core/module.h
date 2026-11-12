/**
 * @file module.h
 * @brief Project modules: how a game project's src/ code plugs into the engine.
 *
 * A project registers a module from any .cpp under its src/ with TOY_MODULE(); the body runs
 * inside every Engine constructor, after all of toyengine's own component parsers are
 * registered and before the default scene loads -- in the game, and in the editor (whose Play
 * runs in-process on the same Engine). That is the place to register component parsers:
 * @code
 * #include <toyengine/core/module.h>
 *
 * TOY_MODULE(my_game) {
 *     coopa::scene::SceneLoader::register_component_parser("Spinner", ...);
 * }
 * @endcode
 * The body sees the constructing `toy::core::Engine& engine`.
 *
 * Registration is a static initializer, which is safe here because toyengine_add_project()
 * (cmake/ToyProject.cmake) compiles project sources straight into each executable -- a static
 * library would let the linker drop a TU nothing references.
 *
 * ## The other two hooks, and why TOY_MODULE() cannot carry them
 *
 * `on_engine_init` runs late in the constructor, which is the right place for a parser but too
 * late for two things a project may need:
 *
 * - **`on_prepare_scene`** runs at the end of `Engine::prepare_scene_()`, once per scene, after
 *   every built-in system is installed and before the scene first ticks. A project installs its
 *   own scene systems here -- `scene.add_system(..., order)` -- and may `scene.remove_system()`
 *   a built-in it replaces, since nothing has executed yet and no state exists to orphan.
 *   `on_engine_init` cannot do this: no scene exists when the constructor runs.
 * - **`on_render_config`** runs at the end of `Engine::make_render_config_()`, which is
 *   `static` and evaluated in the member-init list -- so it runs BEFORE `on_engine_init`, and
 *   before the pipeline is built from the config it returns. A project adds a surface shader
 *   here (`rc.surface_shaders.add({...})`), which is the only moment early enough for the
 *   pipeline to build a variant for it. It runs once per config DERIVATION rather than once
 *   per Engine -- a scene carrying render overrides makes another one -- so it must be safe to
 *   repeat. It is: each derivation starts from a fresh `ToyRenderConfig`, so a registry add
 *   never sees its own previous entry (which would throw, the names being unique).
 *
 * Because `on_render_config` runs before any module body does, a module that needs it must be
 * registered from a static initializer rather than set from inside `on_engine_init`. TOY_MODULE()
 * only fills `on_engine_init`, so register a second module literal directly:
 * @code
 * static const bool k_my_render = toy::core::register_module({
 *     "my_game_render",
 *     nullptr,                                        // on_engine_init
 *     nullptr,                                        // on_prepare_scene
 *     [](toy::render::ToyRenderConfig& rc) { rc.surface_shaders.add({...}); },
 * });
 * @endcode
 *
 * Every hook is optional; a null one is skipped. Modules run in registration order, which is
 * static-init order -- so a project must not depend on two of its own modules' relative order.
 */

#ifndef TOYENGINE_CORE_MODULE_H
#define TOYENGINE_CORE_MODULE_H

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace coopa {
namespace scene {
class Scene;
}
}

namespace toy {

namespace render {
struct ToyRenderConfig;
}

namespace core {

class Engine;

/** @brief One registered project module. Every hook is optional. */
struct Module {
    std::string name;

    /** @brief Late in the Engine constructor: component parsers, asset loaders, editor schemas. */
    std::function<void(Engine&)> on_engine_init;

    /**
     * @brief End of prepare_scene_(), once per scene: the project's own scene systems.
     *
     * Every built-in system is installed by now and the scene has not ticked, so this is where
     * to `add_system()` -- and where `remove_system()` is safe for a built-in being replaced.
     */
    std::function<void(Engine&, coopa::scene::Scene&)> on_prepare_scene;

    /**
     * @brief End of make_render_config_(), before the pipeline is built: surface shaders.
     *
     * Runs ahead of `on_engine_init` (see the file doc), so a module using only this hook has
     * to be registered with register_module() rather than through TOY_MODULE().
     */
    std::function<void(render::ToyRenderConfig&)> on_render_config;
};

/** @brief Every registered module, in registration order. */
inline std::vector<Module>& modules() {
    static std::vector<Module> list;
    return list;
}

/** @brief Adds a module; returns true so it can initialise a static. */
inline bool register_module(Module m) {
    modules().push_back(std::move(m));
    return true;
}

} // namespace core
} // namespace toy

/// Defines and registers a module; the braces that follow are its on_engine_init body, with the
/// Engine in scope as `engine`. For the other hooks, see this file's doc.
#define TOY_MODULE(NAME)                                                                       \
    static void toy_module_init_##NAME(::toy::core::Engine& engine);                           \
    [[maybe_unused]] static const bool toy_module_registered_##NAME =                          \
        ::toy::core::register_module({#NAME, &toy_module_init_##NAME});                        \
    static void toy_module_init_##NAME([[maybe_unused]] ::toy::core::Engine& engine)

#endif // TOYENGINE_CORE_MODULE_H
