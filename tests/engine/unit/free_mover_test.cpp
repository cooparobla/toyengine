/**
 * @file free_mover_test.cpp
 * @brief FreeMover (the focus marker's 3-axis mover): world-axis travel at move_speed with clamped
 *        diagonals, and smoothing that covers the same ground at 30 and 240 fps.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <iostream>
#include <memory>

#include <glm/glm.hpp>
#include <coopa/scene/scene.h>
#include <toyengine/scene/free_mover.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("free_mover");

using coopa::scene::Scene;
using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;

namespace {

/**
 * @brief Builds a one-object scene whose only component is a FreeMover, plus a Transform.
 *
 * No system to install, unlike make_controller_scene() above: FreeMover does its work in the
 * ordinary update() at UpdatePhase::Behaviour, because it drives nothing physical and so has no
 * reason to be hoisted ahead of the physics phase -- see free_mover.h's file doc.
 */
std::unique_ptr<Scene> make_free_mover_scene(toy::scene::FreeMover** out_fm,
                                             const glm::vec3& seed_pos) {
    auto scene = std::make_unique<Scene>("free_mover_test");
    auto obj = std::make_unique<SceneObject>("focus_marker");
    obj->add_component<TransformComponent>()->transform().set_position(seed_pos);
    *out_fm = obj->add_component<toy::scene::FreeMover>();
    scene->add_root_object(std::move(obj));
    return scene;
}

} // namespace

COOPA_TEST(travels_world_axes_at_move_speed_with_clamped_diagonals) {
    toy::scene::FreeMover* fm = nullptr;
    auto scene = make_free_mover_scene(&fm, glm::vec3(0.0f));
    fm->move_speed = 6.0f;
    fm->smoothing = 0.0f; // instant, so travelled distance is exactly speed * time
    scene->start();

    auto* tc = scene->root_objects()[0]->get_transform();

    // Vertical is the axis KinematicController deliberately does not have -- it holds its
    // authored height -- so it is the one worth checking first here.
    fm->move_input = glm::vec3(0.0f, 0.0f, 1.0f);
    for (int i = 0; i < 60; ++i) scene->update(1.0f / 60.0f);
    glm::vec3 p = tc->transform().position();
    expect_near(p.z, 6.0f, 0.05f, "free_mover: +Z input for 1 s rises move_speed units");
    expect(std::fabs(p.x) < 1e-5f && std::fabs(p.y) < 1e-5f,
           "free_mover: pure +Z input causes no horizontal drift");

    fm->move_input = glm::vec3(0.0f, 0.0f, -1.0f);
    for (int i = 0; i < 60; ++i) scene->update(1.0f / 60.0f);
    expect_near(tc->transform().position().z, 0.0f, 0.05f, "free_mover: -Z input descends again");

    // Movement is in WORLD axes, not the owner's basis: rotating the marker must not steer it.
    tc->transform().set_rotation(glm::vec3(0.0f, 0.0f, 90.0f));
    glm::vec3 before = tc->transform().position();
    fm->move_input = glm::vec3(1.0f, 0.0f, 0.0f);
    for (int i = 0; i < 60; ++i) scene->update(1.0f / 60.0f);
    glm::vec3 delta = tc->transform().position() - before;
    expect_near(delta.x, 6.0f, 0.05f, "free_mover: +X input travels world +X whatever the yaw");
    expect(std::fabs(delta.y) < 1e-4f, "free_mover: a yawed marker does not steer with its basis");

    // Diagonal input is CLAMPED, not normalized -- same contract as KinematicController, and the
    // reason a three-axis mover does not travel sqrt(3) faster on a full-deflection diagonal.
    before = tc->transform().position();
    fm->move_input = glm::vec3(1.0f, 1.0f, 1.0f);
    for (int i = 0; i < 60; ++i) scene->update(1.0f / 60.0f);
    const float diagonal = glm::length(tc->transform().position() - before);
    expect_near(diagonal, 6.0f, 0.05f,
                "free_mover: full diagonal deflection is clamped to move_speed");

    // A partial deflection must stay partial, which is what "clamp, don't normalize" buys.
    before = tc->transform().position();
    fm->move_input = glm::vec3(0.5f, 0.0f, 0.0f);
    for (int i = 0; i < 60; ++i) scene->update(1.0f / 60.0f);
    expect_near(glm::length(tc->transform().position() - before), 3.0f, 0.05f,
                "free_mover: half deflection travels half speed");
}

COOPA_TEST(smoothing_is_frame_rate_independent) {
    toy::scene::FreeMover* fm = nullptr;
    auto scene = make_free_mover_scene(&fm, glm::vec3(0.0f));
    fm->move_speed = 6.0f;
    fm->smoothing = 8.0f;
    scene->start();

    fm->move_input = glm::vec3(0.0f, 1.0f, 0.0f);
    scene->update(1.0f / 60.0f);
    expect(fm->velocity().y > 0.0f && fm->velocity().y < 6.0f,
           "free_mover: smoothing ramps velocity rather than snapping to move_speed");

    for (int i = 0; i < 300; ++i) scene->update(1.0f / 60.0f);
    expect_near(fm->velocity().y, 6.0f, 0.05f, "free_mover: smoothed velocity converges to move_speed");

    fm->move_input = glm::vec3(0.0f);
    scene->update(1.0f / 60.0f);
    expect(fm->velocity().y > 0.0f && fm->velocity().y < 6.0f,
           "free_mover: releasing input decays velocity instead of stopping dead");

    // The claim `1 - exp(-k*dt)` makes: the same wall-clock second covers the same ground
    // whatever the tick rate. A raw lerp factor would fail this badly -- which is the whole
    // reason the smoothing is written the way it is.
    toy::scene::FreeMover* fast = nullptr;
    auto fast_scene = make_free_mover_scene(&fast, glm::vec3(0.0f));
    fast->move_speed = 6.0f;
    fast->smoothing = 8.0f;
    fast_scene->start();
    fast->move_input = glm::vec3(0.0f, 1.0f, 0.0f);
    for (int i = 0; i < 240; ++i) fast_scene->update(1.0f / 240.0f); // 1 s at 240 fps

    toy::scene::FreeMover* slow = nullptr;
    auto slow_scene = make_free_mover_scene(&slow, glm::vec3(0.0f));
    slow->move_speed = 6.0f;
    slow->smoothing = 8.0f;
    slow_scene->start();
    slow->move_input = glm::vec3(0.0f, 1.0f, 0.0f);
    for (int i = 0; i < 30; ++i) slow_scene->update(1.0f / 30.0f);  // 1 s at 30 fps

    const float fast_y = fast_scene->root_objects()[0]->get_transform()->transform().position().y;
    const float slow_y = slow_scene->root_objects()[0]->get_transform()->transform().position().y;
    expect_near(slow_y, fast_y, 0.15f,
                "free_mover: one second of travel is the same at 30 and 240 fps");
    if (std::fabs(slow_y - fast_y) > 0.15f) {
        std::cerr << "         30 fps travelled " << slow_y << ", 240 fps travelled " << fast_y << "\n";
    }
}
