/**
 * @file turntable_test.cpp
 * @brief Turntable (the model viewer's spin): a drag turns the object about its pole and that
 *        way round on screen, the tilt is clamped and stays a globe's rather than a trackball's,
 *        a fling runs on and comes to a real stop, and the idle drift turns an untouched model.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <memory>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/yaml/document.h>
#include <toyengine/scene/register.h>
#include <toyengine/scene/turntable.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("turntable");

using coopa::scene::Scene;
using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;

namespace {

/** @brief A one-object scene whose only component is a Turntable, plus its Transform. */
std::unique_ptr<Scene> make_turntable_scene(toy::scene::Turntable** out_tt) {
    auto scene = std::make_unique<Scene>("turntable_test");
    auto obj = std::make_unique<SceneObject>("globe");
    obj->add_component<TransformComponent>();
    *out_tt = obj->add_component<toy::scene::Turntable>();
    scene->add_root_object(std::move(obj));
    return scene;
}

/** @brief Where a point fixed to the model ends up in world space. */
glm::vec3 on_model(const Scene& scene, const glm::vec3& local) {
    const glm::mat4 world = scene.root_objects()[0]->get_transform()->get_world_matrix();
    return glm::vec3(world * glm::vec4(local, 1.0f));
}

/** @brief Drags `pixels` across `frames` frames of 1/60 s, then releases and settles `after`. */
void drag(Scene& scene, toy::scene::Turntable& tt, const glm::vec2& pixels, int frames, int after = 0) {
    for (int i = 0; i < frames; ++i) {
        tt.dragging = true;
        tt.drag_input = pixels / static_cast<float>(frames);
        scene.update(1.0f / 60.0f);
    }
    tt.dragging = false;
    tt.drag_input = glm::vec2(0.0f);
    for (int i = 0; i < after; ++i) scene.update(1.0f / 60.0f);
}

} // namespace

/**
 * @brief A horizontal drag spins the model about its own pole, and the side facing the camera
 *        travels the way the mouse went -- the whole point of the component.
 */
COOPA_TEST(a_horizontal_drag_spins_the_model_the_way_the_mouse_went) {
    toy::scene::Turntable* tt = nullptr;
    auto scene = make_turntable_scene(&tt);
    tt->sensitivity = 0.25f;
    tt->damping = 1000.0f; // the fling is its own test below
    scene->start();

    // The camera of planet_demo: at -Y looking toward the origin, so +X is across the screen to
    // the right and the point of the model facing it is the one at -Y.
    tt->view_right = glm::vec3(1.0f, 0.0f, 0.0f);
    drag(*scene, *tt, glm::vec2(240.0f, 0.0f), 24);

    expect_near(tt->spin_deg(), 60.0f, 0.01f, "turntable: 240 px at 0.25 deg/px is 60 degrees of spin");
    expect_near(tt->tilt_deg(), 0.0f, 1e-4f, "turntable: ...and no tilt");

    const glm::vec3 facing = on_model(*scene, glm::vec3(0.0f, -1.0f, 0.0f));
    expect(facing.x > 0.5f, "turntable: the face toward the camera moved right, as the mouse did (x " +
                                std::to_string(facing.x) + ")");
    expect_near(facing.z, 0.0f, 1e-4f, "turntable: ...without leaving the equator");
}

/**
 * @brief A vertical drag tips the pole toward the viewer, clamped -- and the pole stays the pole
 *        however much the model is spun, which is what makes this a globe and not a trackball.
 */
COOPA_TEST(a_vertical_drag_tilts_the_pole_within_its_clamps) {
    toy::scene::Turntable* tt = nullptr;
    auto scene = make_turntable_scene(&tt);
    tt->sensitivity = 0.5f;
    tt->max_tilt_deg = 30.0f;
    tt->min_tilt_deg = -30.0f;
    scene->start();

    // +y is DOWN the screen (coopa::input::Input's convention), and dragging down brings the top
    // of the model toward the viewer -- i.e. toward the camera at -Y.
    drag(*scene, *tt, glm::vec2(0.0f, 40.0f), 8);
    expect_near(tt->tilt_deg(), 20.0f, 0.01f, "turntable: 40 px at 0.5 deg/px is 20 degrees of tilt");
    expect(on_model(*scene, glm::vec3(0.0f, 0.0f, 1.0f)).y < -0.3f,
           "turntable: dragging down leans the pole toward the viewer");

    drag(*scene, *tt, glm::vec2(0.0f, 400.0f), 8);
    expect_near(tt->tilt_deg(), 30.0f, 1e-4f, "turntable: a long drag stops at max_tilt_deg");
    drag(*scene, *tt, glm::vec2(0.0f, -900.0f), 8);
    expect_near(tt->tilt_deg(), -30.0f, 1e-4f, "turntable: ...and the other way at min_tilt_deg");

    // Spun a long way with the pole leaning, the pole must still be leaning the same way: a
    // trackball's composition would have swung it round the screen instead.
    const glm::vec3 before = on_model(*scene, glm::vec3(0.0f, 0.0f, 1.0f));
    drag(*scene, *tt, glm::vec2(1000.0f, 0.0f), 20);
    const glm::vec3 after = on_model(*scene, glm::vec3(0.0f, 0.0f, 1.0f));
    expect_near(glm::length(after - before), 0.0f, 1e-4f,
                "turntable: spinning does not move the pole (a globe, not a trackball)");
}

/**
 * @brief Letting go of a moving drag leaves the model spinning, and `damping` brings it to a
 *        real stop rather than to a perpetual creep.
 */
COOPA_TEST(a_fling_runs_on_and_then_stops_dead) {
    toy::scene::Turntable* tt = nullptr;
    auto scene = make_turntable_scene(&tt);
    tt->sensitivity = 0.25f;
    tt->damping = 2.0f;
    scene->start();

    drag(*scene, *tt, glm::vec2(600.0f, 0.0f), 30);
    const float released = tt->spin_deg();
    expect(tt->spin_velocity_deg_per_sec() > 50.0f,
           "turntable: letting go of a 300 deg/s drag leaves it spinning (" +
               std::to_string(tt->spin_velocity_deg_per_sec()) + " deg/s)");

    for (int i = 0; i < 30; ++i) scene->update(1.0f / 60.0f);
    expect(std::abs(tt->spin_deg() - released) > 5.0f, "turntable: ...and it carries on turning");

    for (int i = 0; i < 600; ++i) scene->update(1.0f / 60.0f);
    expect_near(tt->spin_velocity_deg_per_sec(), 0.0f, 1e-6f,
                "turntable: the fling decays to a full stop, not to a creep");
    const float resting = tt->spin_deg();
    for (int i = 0; i < 60; ++i) scene->update(1.0f / 60.0f);
    expect_near(tt->spin_deg(), resting, 1e-6f, "turntable: ...and an untouched model with no drift holds still");
}

/** @brief The idle drift turns a model nobody is dragging, at the rate it was asked for. */
COOPA_TEST(the_idle_drift_turns_an_untouched_model) {
    toy::scene::Turntable* tt = nullptr;
    auto scene = make_turntable_scene(&tt);
    tt->idle_spin_deg_per_sec = 6.0f;
    scene->start();

    for (int i = 0; i < 120; ++i) scene->update(1.0f / 60.0f);
    expect_near(tt->spin_deg(), 12.0f, 0.05f, "turntable: 6 deg/s drifts 12 degrees in two seconds");
}

/** @brief The scene parser fills the component in, under the name a scene writes. */
COOPA_TEST(a_scene_builds_one_from_its_keys) {
    toy::scene::register_scene_components();
    Scene scene = coopa::scene::SceneLoader::load_from_node(fkyaml::node::deserialize(std::string(
        "scene:\n"
        "  scene_name: turntable_yaml\n"
        "  root_objects:\n"
        "    - name: globe\n"
        "      components:\n"
        "        - { type: Transform, rotation: { x: 10.0, y: 0.0, z: 45.0 } }\n"
        "        - type: Turntable\n"
        "          sensitivity: 0.4\n"
        "          drag_button: right\n"
        "          axis: { x: 0.0, y: 1.0, z: 0.0 }\n"
        "          idle_spin_deg_per_sec: 12.0\n"
        "          damping: 1.5\n"
        "          min_tilt_deg: -45.0\n"
        "          max_tilt_deg: 45.0\n"
        "          invert_x: true\n")), "turntable_test.yaml");

    auto* tt = scene.find_first_component<toy::scene::Turntable>();
    require(tt != nullptr, "turntable: the scene parser built one");
    expect_near(tt->sensitivity, 0.4f, 1e-6f, "turntable: sensitivity parsed");
    expect(tt->drag_button == "right" && tt->invert_x, "turntable: drag_button and invert_x parsed");
    expect(tt->axis == glm::vec3(0.0f, 1.0f, 0.0f), "turntable: axis parsed");
    expect_near(tt->idle_spin_deg_per_sec, 12.0f, 1e-6f, "turntable: idle_spin_deg_per_sec parsed");
    expect_near(tt->damping, 1.5f, 1e-6f, "turntable: damping parsed");
    expect_near(tt->max_tilt_deg, 45.0f, 1e-6f, "turntable: max_tilt_deg parsed");

    // The authored rotation is the seed the component carries on from, not something it discards.
    scene.start();
    expect_near(tt->spin_deg(), 45.0f, 1e-4f, "turntable: the authored Z rotation seeds the spin");
    expect_near(tt->tilt_deg(), 10.0f, 1e-4f, "turntable: ...and the authored X rotation the tilt");
}
