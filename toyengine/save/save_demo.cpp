#include <toyengine/save/save_demo.h>

#include <coopa/event/signal.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <toyengine/scene/character_controller.h>
#include <toyengine/weather/weather_system.h>

namespace toy {
namespace save {

void SaveDemo::start() {
    SaveSystem* saves = SaveSystem::active();
    if (!saves) return;
    save_conn_ = saves->on_save.connect_scoped([this](SaveGame& game) { on_save_(game); });
    load_conn_ = saves->on_load.connect_scoped([this](const SaveGame& game) { on_load_(game); });
}

void SaveDemo::update(float) {
    if (!scene) return;
    auto* player = scene->find_first_component<toy::scene::CharacterController>();
    auto* ptc = player && player->owner ? player->owner->get_transform() : nullptr;
    if (!ptc) return;
    const glm::vec3 chest = glm::vec3(ptc->get_world_matrix()[3]) + glm::vec3(0.0f, 0.0f, 0.5f * player->height);
    for (coopa::scene::SceneObject* coin : coins_()) {
        auto* tc = coin->get_transform();
        if (!coin->active() || !tc) continue;
        if (glm::length(glm::vec3(tc->get_world_matrix()[3]) - chest) > pickup_radius) continue;
        coin->set_active(false);
        collected_.insert(coin->name());
    }
}

std::vector<coopa::scene::SceneObject*> SaveDemo::coins_() const {
    std::vector<coopa::scene::SceneObject*> out;
    for (auto& root : scene->root_objects()) {
        if (root->name().rfind(coin_prefix, 0) == 0) out.push_back(root.get());
    }
    return out;
}

void SaveDemo::on_save_(SaveGame& game) {
    SaveNode demo = game.global().section("save_demo");
    demo.set("coins", std::vector<std::string>(collected_.begin(), collected_.end()));
    if (auto* w = scene ? weather::find(*scene) : nullptr) {
        demo.set("hour", w->time_of_day());
        demo.set("day", w->state().day);
    }
    game.summary().set("coins", static_cast<int>(collected_.size()));
}

void SaveDemo::on_load_(const SaveGame& game) {
    const SaveNode demo = game.global().section("save_demo");
    const std::vector<std::string> coins = demo.get("coins", std::vector<std::string>{});
    collected_ = std::set<std::string>(coins.begin(), coins.end());
    for (coopa::scene::SceneObject* coin : coins_()) coin->set_active(!collected_.count(coin->name()));
    if (auto* w = scene ? weather::find(*scene) : nullptr) {
        if (demo.has("hour")) w->set_time(demo.get("hour", w->time_of_day()));
        if (demo.has("day")) w->set_day(demo.get("day", 0));
    }
}

} // namespace save
} // namespace toy
