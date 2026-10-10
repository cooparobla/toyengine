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


#include <toyengine/save/save_system.h>

namespace toy {
namespace save {

class SaveDemo : public coopa::scene::Component {
public:
    std::string coin_prefix = "coin_";
    float       pickup_radius = 1.0f;

    std::string type_name() const override { return "SaveDemo"; }

    const std::set<std::string>& collected() const { return collected_; }

    void start() override;

    /** @brief Picks up every coin within pickup_radius of the player. */
    void update(float) override;

private:
    std::set<std::string> collected_;
    coopa::event::ScopedConnection save_conn_;
    coopa::event::ScopedConnection load_conn_;

    std::vector<coopa::scene::SceneObject*> coins_() const;

    void on_save_(SaveGame& game);

    void on_load_(const SaveGame& game);
};

}  // namespace save
}  // namespace toy

#endif  // TOYENGINE_SAVE_SAVE_DEMO_H
