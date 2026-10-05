/**
 * @file ui_assets.h
 * @brief Game-side helpers for UI assets (`assets/ui/*.yaml`): open a screen on demand, close
 *        it, and a component base that binds game logic to authored UI by widget name.
 *
 * A UI asset is an object asset whose root carries a Canvas -- a HUD, a pause menu, an
 * inventory. Place one in a scene file (`prefab: ui/hud`) to have it from the start, or open
 * it while the game runs:
 *
 * @code
 * coopa::ui::UiHandle pause = toy::ui::open(engine.scene(), "ui/pause_menu");
 * keep(pause.on_click("Resume", [&] { toy::ui::close(engine.scene(), pause); }));
 * @endcode
 *
 * Game logic that drives a UI usually lives in a UiController on the UI's root (or on any
 * object, pointed at the UI by name): override bind() and wire the widgets there.
 */

#ifndef TOYENGINE_UI_UI_ASSETS_H
#define TOYENGINE_UI_UI_ASSETS_H

#include <uicoopa/binding/ui_handle.h>

#include <coopa/event/signal.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>

#include <iostream>
#include <string>
#include <vector>

namespace toy::ui {

/**
 * @brief Builds the UI asset `ref` ("ui/pause_menu"; extension optional) into `scene` -- under
 *        `parent`, or as a root object -- started and ready.
 * @return The UI's root object, or null (logged) if the asset could not be loaded.
 */
inline coopa::scene::SceneObject* spawn(coopa::scene::Scene& scene, const std::string& ref,
                                        coopa::scene::SceneObject* parent = nullptr) {
    try {
        return coopa::scene::SceneLoader::spawn(scene, ref, parent);
    } catch (const std::exception& e) {
        std::cerr << "[toyengine] ui::spawn('" << ref << "') failed: " << e.what() << "\n";
        return nullptr;
    }
}

/** @brief spawn(), wrapped in a UiHandle (empty if it failed). */
inline coopa::ui::UiHandle open(coopa::scene::Scene& scene, const std::string& ref,
                                coopa::scene::SceneObject* parent = nullptr) {
    return coopa::ui::UiHandle(spawn(scene, ref, parent));
}

/** @brief Destroys a spawned UI at the end of the frame (safe from a click handler). */
inline void close(coopa::scene::Scene& scene, coopa::scene::SceneObject* root) {
    if (root) scene.commands_for(coopa::job::k_main_thread_index).destroy_object(root);
}
inline void close(coopa::scene::Scene& scene, coopa::ui::UiHandle& ui) {
    close(scene, ui.root());
    ui = coopa::ui::UiHandle();
}

/** @brief The first object named `name` in the scene, as a UiHandle (empty if none). */
inline coopa::ui::UiHandle find(coopa::scene::Scene& scene, const std::string& name) {
    return coopa::ui::UiHandle(scene.find_object(name));
}

/**
 * @class UiController
 * @brief Base for a component that drives authored UI: on start() it resolves the UI root
 *        (`ui_object`, by name in the scene; empty means its own object) and calls bind().
 *
 * Connections made through keep() last exactly as long as the component.
 *
 * @code
 * class PauseMenuLogic : public toy::ui::UiController {
 *     void bind(coopa::ui::UiHandle& ui) override {
 *         keep(ui.on_click("Resume", [this] { this->ui().hide(""); }));
 *         keep(ui.on("Volume", "value_changed", [](const auto& a) { set_volume(a.get("value", 1.0f)); }));
 *     }
 * };
 * @endcode
 */
class UiController : public coopa::scene::Component {
public:
    /** @brief Name of the UI root to bind; empty binds this component's own object. */
    std::string ui_object;

    std::string type_name() const override { return "UiController"; }

    void start() override {
        coopa::scene::SceneObject* root = owner;
        if (!ui_object.empty() && scene) root = scene->find_object(ui_object);
        handle_ = coopa::ui::UiHandle(root);
        if (handle_) bind(handle_);
    }

    coopa::ui::UiHandle& ui() { return handle_; }

protected:
    /** @brief Wire the UI here: listeners, model bindings, initial values. */
    virtual void bind(coopa::ui::UiHandle& ui) { (void)ui; }

    /** @brief Holds a connection for this component's lifetime. */
    void keep(coopa::event::Connection c) { connections_.emplace_back(std::move(c)); }

private:
    coopa::ui::UiHandle handle_;
    std::vector<coopa::event::ScopedConnection> connections_;
};

}  // namespace toy::ui

#endif  // TOYENGINE_UI_UI_ASSETS_H
