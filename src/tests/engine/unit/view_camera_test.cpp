/**
 * @file view_camera_test.cpp
 * @brief Which camera a camera-relative component follows: the one the host pushed, the one it
 *        names, and the fallbacks in between.
 *
 * The thing worth protecting here is the FALLBACK ORDER, because every way of getting it wrong
 * fails silently and identically -- a terrain parked at the world origin, streaming nothing.
 * Nothing in this file needs a device: resolution is pure, over a Scene built by hand.
 */

#include <coopa/testing/test.h>

#include <memory>
#include <string>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <fkYAML/node.hpp>
#include <gfxcoopa/engine/components/camera_component.h>
#include <toyengine/scene/view_camera.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("view_camera");

using coopa::gfx::engine::components::CameraComponent;
using toy::scene::CameraRef;

namespace {

/// Adds a named object carrying a camera, and returns that camera.
CameraComponent* add_camera(coopa::scene::Scene& scene, const std::string& name) {
    auto obj = std::make_unique<coopa::scene::SceneObject>(name);
    CameraComponent* cam = obj->add_component<CameraComponent>();
    scene.add_root_object(std::move(obj));
    return cam;
}

}  // namespace

/**
 * @brief The default follows what the host pushed, in preference to the global singleton.
 *
 * This is the whole point of the push: two scenes rendered in one frame each follow their own
 * camera, rather than both following whichever one claimed CameraComponent::main().
 */
COOPA_TEST(the_default_follows_the_pushed_camera) {
    coopa::scene::Scene scene("game");
    CameraComponent* pushed = add_camera(scene, "sub_cam");

    const CameraRef ref;   // defaults: use_main_camera, no name
    expect(toy::scene::resolve_view_camera(ref, scene, pushed) == pushed,
           "the pushed camera is what a default CameraRef resolves to");
    expect(toy::scene::scene_view_camera(pushed) == pushed,
           "scene_view_camera() returns the pushed camera unchanged");
}

/**
 * @brief Pushing nothing falls back to the global main camera.
 *
 * This is what keeps every single-scene game and the whole editor behaving exactly as they did
 * before the camera became a pushed value: nobody pushes one, so nothing changes.
 */
COOPA_TEST(pushing_nothing_falls_back_to_the_global_main_camera) {
    coopa::scene::Scene scene("game");
    CameraComponent* owned = add_camera(scene, "cam");
    owned->make_main();
    require(CameraComponent::main() == owned, "the scene's camera took the main-camera singleton");

    const CameraRef ref;
    expect(toy::scene::resolve_view_camera(ref, scene, nullptr) == owned,
           "with nothing pushed, resolution falls back to CameraComponent::main()");
}

/**
 * @brief Naming a camera follows that one instead, even while another is main.
 */
COOPA_TEST(a_named_camera_wins_over_the_pushed_one) {
    coopa::scene::Scene scene("game");
    CameraComponent* chase  = add_camera(scene, "chase_cam");
    CameraComponent* player = add_camera(scene, "player_cam");

    CameraRef ref;
    ref.use_main_camera = false;
    ref.camera = "chase_cam";
    expect(toy::scene::resolve_view_camera(ref, scene, player) == chase,
           "the named camera is followed rather than the pushed one");
}

/**
 * @brief A name that resolves to nothing falls back rather than yielding no camera.
 *
 * A typo in a camera name must not park a terrain at the world origin with nothing streamed in
 * and no error -- the symptom would read as a broken terrain, not as a broken name. Covered for
 * both ways a name can fail: no such object, and an object with no camera on it.
 */
COOPA_TEST(an_unresolvable_camera_name_falls_back_to_the_pushed_one) {
    coopa::scene::Scene scene("game");
    CameraComponent* pushed = add_camera(scene, "player_cam");
    scene.add_root_object(std::make_unique<coopa::scene::SceneObject>("not_a_camera"));

    CameraRef missing;
    missing.use_main_camera = false;
    missing.camera = "typo_cam";
    expect(toy::scene::resolve_view_camera(missing, scene, pushed) == pushed,
           "a name matching no object falls back to the pushed camera");

    CameraRef cameraless;
    cameraless.use_main_camera = false;
    cameraless.camera = "not_a_camera";
    expect(toy::scene::resolve_view_camera(cameraless, scene, pushed) == pushed,
           "a name matching an object with no camera falls back to the pushed camera");
}

/**
 * @brief Writing `camera:` alone is enough -- the flag follows the name.
 *
 * Requiring both keys would make `camera: chase_cam` on its own silently do nothing, which is
 * the single most likely way to author this wrong.
 */
COOPA_TEST(naming_a_camera_in_yaml_turns_off_use_main_camera) {
    CameraRef named;
    fkyaml::node only_name = fkyaml::node::deserialize(std::string("camera: chase_cam"));
    toy::scene::parse_camera_ref(only_name, named);
    expect(named.camera == "chase_cam", "the name is read");
    expect(!named.use_main_camera, "naming a camera turns the main-camera flag off by itself");

    // An author who writes both means both: the explicit flag is not overridden by the name.
    CameraRef both;
    fkyaml::node explicit_flag =
        fkyaml::node::deserialize(std::string("use_main_camera: true\ncamera: chase_cam"));
    toy::scene::parse_camera_ref(explicit_flag, both);
    expect(both.use_main_camera, "an explicit use_main_camera: true survives a name beside it");

    // Absent keys leave the reference alone, so a component's own default stands.
    CameraRef untouched;
    untouched.camera = "kept";
    fkyaml::node empty = fkyaml::node::deserialize(std::string("other_key: 1"));
    toy::scene::parse_camera_ref(empty, untouched);
    expect(untouched.use_main_camera && untouched.camera == "kept",
           "a node with neither key leaves the reference untouched");
}
