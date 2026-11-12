/**
 * @file terrain_digger.h
 * @brief Digging a TileTerrain with the mouse: the tile under the cursor goes down (or up), and
 *        the water around it runs in behind.
 *
 * The demo-side half of runtime terrain editing. Everything it does is
 * `TerrainComponent::raycast_tiles()` + `raise_at()` (toyengine/world/terrain_component.h) --
 * it exists so a scene can be dug by hand, and so there is a worked example of driving those
 * from gameplay. A game with its own tools does not need this component at all.
 *
 * Picking goes through the terrain's own ray march, not through physics, so it works on a
 * terrain with `collision: false` -- which a planet drawn to be looked at usually has.
 *
 * Input is PUSHED in by toy::core::Engine::drive_terrain_diggers_() before Scene::update(), the
 * same contract CameraController, Turntable and FreeMover follow: no component here reads
 * coopa::input::Input, so each stays testable with no window and NO_INPUT=1 zeroes the lot.
 */

#ifndef TOYENGINE_SCENE_TERRAIN_DIGGER_H
#define TOYENGINE_SCENE_TERRAIN_DIGGER_H

#include <glm/glm.hpp>

#include <array>
#include <string>

#include <coopa/scene/component.h>

namespace toy {
namespace scene {

/**
 * @class TerrainDigger
 * @brief Lowers (or raises) the terrain under the cursor while its button is held.
 *
 * The component does not need to be on the terrain object -- it finds the first TileTerrain in
 * the scene -- so a scene can carry one anywhere, usually beside the camera.
 *
 * Holding the button digs repeatedly, at `repeat_seconds`, rather than once per click: a
 * channel is dug by dragging. The water is not this component's business at all; the terrain
 * queues it and TerrainSystem spends a bounded number of tiles a frame on it, so a hole opened
 * beside the sea fills over the next few frames.
 *
 * Example YAML:
 * @code
 * - type: TerrainDigger
 *   dig_button: middle        # "left" / "right" / "middle"
 *   raise_key: ctrl           # hold to raise instead of lower ("" for dig-only)
 *   radius: 1                 # tiles either side of the one hit (1 = a 3x3 patch)
 *   steps: 1                  # terraces per dig
 *   repeat_seconds: 0.12
 * @endcode
 */
class TerrainDigger : public coopa::scene::Component {
public:
    std::string type_name() const override { return "TerrainDigger"; }

    /** @brief Mouse button that digs: "left", "right" or "middle". */
    std::string dig_button = "middle";

    /**
     * @brief Held to raise instead of lower: "ctrl", "shift", "alt", or "" for dig-only.
     *
     * Ctrl rather than Shift by default because Shift is already spoken for: Engine binds it to
     * `move_z` down (FreeMover) and to `sprint` (CharacterController), so a scene that flies a
     * marker around -- which a digging scene tends to -- would descend every time it raised
     * ground.
     */
    std::string raise_key = "ctrl";

    /** @brief Tiles either side of the one under the cursor: 0 digs one tile, 1 a 3x3 patch. */
    int radius = 1;

    /** @brief Terraces moved per dig. */
    int steps = 1;

    /** @brief Seconds between digs while the button is held; 0 digs every frame. */
    float repeat_seconds = 0.12f;

    /** @brief How far the pick ray reaches, in world units. */
    float reach = 1.0e4f;

    // --- Per-frame input, written by Engine before Scene::update() ---

    bool      digging = false;              /**< The dig button is held this frame. */
    bool      raising = false;              /**< ...with the raise modifier held too. */
    glm::vec3 ray_origin = glm::vec3(0.0f); /**< World-space pick ray: where the cursor looks from... */
    glm::vec3 ray_direction = glm::vec3(0.0f); /**< ...and along. Zero means "no ray this frame". */

    /** @brief Draw the outline of the tile under the cursor; off hides it. */
    bool show_hover = true;

    /** @brief Outline colour while lowering, and while the raise modifier is held (RGB, 0..1). */
    glm::vec3 hover_color = glm::vec3(1.0f, 0.85f, 0.25f);
    glm::vec3 raise_color = glm::vec3(0.45f, 0.9f, 1.0f);

    /**
     * @brief Picks the tile under the cursor, digs it if the button is held, and counts down to
     *        the next repeat.
     *
     * The pick runs every frame, not only while digging: the hover outline it feeds is how a
     * player knows what a click would take, and is the first thing to look at when a dig seems
     * to do nothing -- no outline means the ray is not reaching the ground.
     */
    void update(float delta_time) override;

    /** @brief World position of the last tile dug -- for tests and for effects. */
    const glm::vec3& last_dig_point() const { return last_dig_point_; }
    /** @brief How many digs this component has applied. */
    int digs() const { return digs_; }

    /** @brief The tile the cursor is over this frame, if any. */
    bool hovering() const { return hovering_; }
    /** @brief Which tile that is (terrain tile coordinates); only meaningful while hovering(). */
    const glm::ivec2& hover_tile() const { return hover_tile_; }
    /** @brief Its top face's four world-space corners, for the outline Engine draws. */
    const std::array<glm::vec3, 4>& hover_corners() const { return hover_corners_; }

private:
    float     cooldown_ = 0.0f;
    glm::vec3 last_dig_point_{0.0f};
    int       digs_ = 0;
    bool      hovering_ = false;
    glm::ivec2 hover_tile_{0};
    std::array<glm::vec3, 4> hover_corners_{};
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_TERRAIN_DIGGER_H
