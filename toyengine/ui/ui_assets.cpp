#include <toyengine/ui/ui_assets.h>

#include <coopa/event/signal.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>

namespace toy {
namespace ui {

coopa::scene::SceneObject* spawn(coopa::scene::Scene& scene, const std::string& ref,
                                        coopa::scene::SceneObject* parent) {
    try {
        return coopa::scene::SceneLoader::spawn(scene, ref, parent);
    } catch (const std::exception& e) {
        std::cerr << "[toyengine] ui::spawn('" << ref << "') failed: " << e.what() << "\n";
        return nullptr;
    }
}

void close(coopa::scene::Scene& scene, coopa::ui::UiHandle& ui) {
    close(scene, ui.root());
    ui = coopa::ui::UiHandle();
}

void UiController::start() {
    coopa::scene::SceneObject* root = owner;
    if (!ui_object.empty() && scene) root = scene->find_object(ui_object);
    handle_ = coopa::ui::UiHandle(root);
    if (handle_) bind(handle_);
}

} // namespace ui
} // namespace toy
