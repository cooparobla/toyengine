#pragma once

/**
 * @file animation.h
 * @brief Clip/rig builders shared by the animator, animation_events, root_motion and ik suites.
 */

#include <coopa/animation/animation_clip.h>
#include <coopa/animation/animator.h>
#include <coopa/animation/keyframe.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace libcoopa_test {

/** @brief A two-key linear track on `property` (channel 0 carries the value) from `from` to `to`. */
inline coopa::anim::AnimationTrack linear_track(const std::string& property, float length, float from, float to,
                                                const std::string& object_path = "") {
    using namespace coopa::anim;
    AnimationTrack t;
    t.object_path = object_path;
    t.property    = property;
    t.curve.add_key(Keyframe{0.0f, {from, 0.0f, 0.0f, 1.0f}, Interpolation::Linear});
    t.curve.add_key(Keyframe{length, {to, 0.0f, 0.0f, 1.0f}, Interpolation::Linear});
    t.curve.sort_keys();
    return t;
}

/** @brief A clip of `length` and `wrap` holding one linear `property` track from `from` to `to`. */
inline std::shared_ptr<coopa::anim::AnimationClip> linear_clip(coopa::anim::WrapMode wrap, float length,
                                                                const std::string& property, float from, float to) {
    auto clip  = std::make_shared<coopa::anim::AnimationClip>();
    clip->wrap = wrap;
    clip->set_explicit_length(length);
    clip->tracks.push_back(linear_track(property, length, from, to));
    return clip;
}

/** @brief A clip of `length` with one flat-ish track (so it has bindings) and the given events. */
inline std::shared_ptr<coopa::anim::AnimationClip> event_clip(
    float length, coopa::anim::WrapMode wrap, const std::vector<std::pair<float, std::string>>& events) {
    auto clip = linear_clip(wrap, length, "position.x", 0.0f, 1.0f);
    for (const auto& e : events) {
        coopa::anim::AnimationEvent ev;
        ev.time = e.first;
        ev.name = e.second;
        clip->events.push_back(ev);
    }
    clip->sort_events();
    return clip;
}

/** @brief Adds a root object `name` with a Transform and an Animator; returns the Animator. */
inline coopa::anim::Animator* add_animated_object(coopa::scene::Scene& scene, const std::string& name) {
    auto obj = std::make_unique<coopa::scene::SceneObject>(name);
    obj->add_component<coopa::scene::TransformComponent>();
    auto* animator = obj->add_component<coopa::anim::Animator>();
    scene.add_root_object(std::move(obj));
    return animator;
}

/** @brief Adds a child with a Transform at `pos`, linking the transforms (add_child doesn't). */
inline coopa::scene::SceneObject* add_bone(coopa::scene::SceneObject* parent, const std::string& name,
                                           const glm::vec3& pos) {
    auto obj = std::make_unique<coopa::scene::SceneObject>(name);
    auto* tc = obj->add_component<coopa::scene::TransformComponent>();
    tc->transform().set_position(pos);
    tc->set_parent_transform(&parent->get_transform()->transform());
    return parent->add_child(std::move(obj));
}

/** @brief Runs `frames` update()+late_update() pairs of `dt`. */
inline void step(coopa::scene::Scene& scene, float dt, int frames = 1) {
    for (int i = 0; i < frames; ++i) {
        scene.update(dt);
        scene.late_update(dt);
    }
}

/** @brief The object's local Transform position. */
inline glm::vec3 position_of(coopa::scene::Scene& scene, const std::string& name) {
    return scene.find_object(name)->get_transform()->transform().position();
}

} // namespace libcoopa_test
