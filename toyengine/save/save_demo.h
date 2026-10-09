/**
 * @file save_demo.h
 * @brief SaveDemo: the character_test save/load demo -- coin pickups and the clock kept in a
 *        save through the SaveSystem's global hooks. Demo code, written the way a game would.
 *
 * In character_test, F5 quick-saves and F9 quick-loads (config.yaml `save.quick_slot`). The
 * save then holds all three hook styles:
 *  - the player's position and facing: CharacterController is an ISaveable, and the player
 *    object carries a SaveId (`objects/player/character`);
 *  - which coins were picked up and the weather clock: this component's on_save / on_load
 *    handlers (`global/save_demo`);
 *  - the coin count as the slot's `summary`, for a slot menu (list_slots()).
 *
 * @code
 * - type: SaveDemo
 *   coin_prefix: coin_      # root objects named coin_* are coins
 *   pickup_radius: 1.0      # metres from the player's chest
 * @endcode
 */

#ifndef TOYENGINE_SAVE_SAVE_DEMO_H
#define TOYENGINE_SAVE_SAVE_DEMO_H

#include <glm/glm.hpp>

#include <set>
#include <string>
#include <vector>

#include <coopa/event/signal.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

#include <toyengine/save/save_system.h>
#include <toyengine/scene/character_controller.h>
#include <toyengine/weather/weather_system.h>

namespace toy {
namespace save {

class SaveDemo : public coopa::scene::Component {
public:
    std::string coin_prefix = "coin_";
    float       pickup_radius = 1.0f;

    std::string type_name() const override { return "SaveDemo"; }

    const std::set<std::string>& collected() const { return collected_; }

    void start() override {
        SaveSystem* saves = SaveSystem::active();
        if (!saves) return;
        save_conn_ = saves->on_save.connect_scoped([this](SaveGame& game) { on_save_(game); });
        load_conn_ = saves->on_load.connect_scoped([this](const SaveGame& game) { on_load_(game); });
    }

    /** @brief Picks up every coin within pickup_radius of the player. */
    void update(float) override {
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

private:
    std::set<std::string> collected_;
    coopa::event::ScopedConnection save_conn_;
    coopa::event::ScopedConnection load_conn_;

    std::vector<coopa::scene::SceneObject*> coins_() const {
        std::vector<coopa::scene::SceneObject*> out;
        for (auto& root : scene->root_objects()) {
            if (root->name().rfind(coin_prefix, 0) == 0) out.push_back(root.get());
        }
        return out;
    }

    void on_save_(SaveGame& game) {
        SaveNode demo = game.global().section("save_demo");
        demo.set("coins", std::vector<std::string>(collected_.begin(), collected_.end()));
        if (auto* w = scene ? weather::find(*scene) : nullptr) {
            demo.set("hour", w->time_of_day());
            demo.set("day", w->state().day);
        }
        game.summary().set("coins", static_cast<int>(collected_.size()));
    }

    void on_load_(const SaveGame& game) {
        const SaveNode demo = game.global().section("save_demo");
        const std::vector<std::string> coins = demo.get("coins", std::vector<std::string>{});
        collected_ = std::set<std::string>(coins.begin(), coins.end());
        for (coopa::scene::SceneObject* coin : coins_()) coin->set_active(!collected_.count(coin->name()));
        if (auto* w = scene ? weather::find(*scene) : nullptr) {
            if (demo.has("hour")) w->set_time(demo.get("hour", w->time_of_day()));
            if (demo.has("day")) w->set_day(demo.get("day", 0));
        }
    }
};

}  // namespace save
}  // namespace toy

#endif  // TOYENGINE_SAVE_SAVE_DEMO_H
