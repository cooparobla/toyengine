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
 */

#ifndef TOYENGINE_CORE_MODULE_H
#define TOYENGINE_CORE_MODULE_H

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace toy {
namespace core {

class Engine;

/** @brief One registered project module. */
struct Module {
    std::string name;
    std::function<void(Engine&)> on_engine_init;
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
/// Engine in scope as `engine`.
#define TOY_MODULE(NAME)                                                                       \
    static void toy_module_init_##NAME(::toy::core::Engine& engine);                           \
    [[maybe_unused]] static const bool toy_module_registered_##NAME =                          \
        ::toy::core::register_module({#NAME, &toy_module_init_##NAME});                        \
    static void toy_module_init_##NAME([[maybe_unused]] ::toy::core::Engine& engine)

#endif // TOYENGINE_CORE_MODULE_H
