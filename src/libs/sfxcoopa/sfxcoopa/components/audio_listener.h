/**
 * @file audio_listener.h
 * @brief AudioListener: pushes its owner's world transform into the engine's
 * listener every update(dt), plus apply_listener_matrix() for hosts that
 * drive the listener from a camera when no AudioListener is in the scene.
 */

#ifndef SFXCOOPA_COMPONENTS_AUDIO_LISTENER_H
#define SFXCOOPA_COMPONENTS_AUDIO_LISTENER_H

#include <sfxcoopa/components/sfx_resources.h>
#include <sfxcoopa/core/engine.h>
#include <sfxcoopa/spatial/listener.h>

#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>

#include <glm/glm.hpp>

#include <string>

namespace coopa {
namespace sfx {
namespace components {

/**
 * @brief Builds a Listener pose from a world matrix -- same convention as
 * Listener::set_from_transform (-Z forward, +Y up, translation in column 3).
 */
inline coopa::sfx::spatial::Listener listener_from_matrix(const glm::mat4& world, const glm::vec3& velocity = glm::vec3(0.0f)) {
    coopa::sfx::spatial::Listener l;
    l.position = glm::vec3(world[3]);
    const glm::vec3 back = glm::vec3(world[2]);
    const glm::vec3 up = glm::vec3(world[1]);
    if (glm::dot(back, back) > 1e-12f) l.forward = glm::normalize(-back);
    if (glm::dot(up, up) > 1e-12f) l.up = glm::normalize(up);
    l.velocity = velocity;
    return l;
}

/** @brief Sets `engine`'s listener from a world matrix (e.g. a camera's inverse view). */
inline void apply_listener_matrix(coopa::sfx::core::AudioEngine& engine, const glm::mat4& world,
                                  const glm::vec3& velocity = glm::vec3(0.0f)) {
    engine.set_listener(listener_from_matrix(world, velocity));
}

/**
 * @class AudioListener
 * @brief Each update(dt), sets SfxResources' engine listener from the owner's
 * world transform (velocity = position delta / dt, for doppler) and marks
 * SfxResources::consume_listener_pushed(). With several active listeners the
 * last one updated in a frame wins.
 */
class AudioListener : public coopa::scene::Component {
public:
    bool track_velocity = true; /**< Derive listener velocity from frame-to-frame motion. */

    AudioListener() { SfxResources::instance().register_listener_(); }
    ~AudioListener() override { SfxResources::instance().unregister_listener_(); }
    AudioListener(const AudioListener&) = delete;
    AudioListener& operator=(const AudioListener&) = delete;

    std::string type_name() const override { return "AudioListener"; }

    void start() override { has_prev_ = false; }

    void update(float delta_time) override {
        coopa::sfx::core::AudioEngine* engine = SfxResources::instance().engine();
        if (!engine) return;
        const glm::mat4 world = world_matrix_();
        const glm::vec3 pos = glm::vec3(world[3]);
        glm::vec3 velocity(0.0f);
        if (track_velocity && has_prev_ && delta_time > 1e-6f) velocity = (pos - prev_pos_) / delta_time;
        prev_pos_ = pos;
        has_prev_ = true;
        apply_listener_matrix(*engine, world, velocity);
        SfxResources::instance().mark_listener_pushed();
    }

private:
    glm::mat4 world_matrix_() const {
        if (owner) {
            if (auto* tc = owner->get_transform()) return tc->get_world_matrix();
        }
        return glm::mat4(1.0f);
    }

    glm::vec3 prev_pos_{0.0f};
    bool has_prev_ = false;
};

}  // namespace components
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_COMPONENTS_AUDIO_LISTENER_H
