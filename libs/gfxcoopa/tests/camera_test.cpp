/**
 * @file camera_test.cpp
 * @brief engine/components/camera_component.h, no device needed: the CameraComponent::main()
 *        singleton (declared main beats start order, first started wins when none is declared,
 *        a declared main takes over from a runtime override, removal clears it rather than
 *        dangling) and switching between orthographic and perspective projection.
 *
 * Not covered: the "multiple cameras marked main" warning text (log output, not behaviour).
 */
#include <coopa/testing/test.h>

#include <gfxcoopa/engine/components/camera_component.h>

#include <coopa/scene/scene_object.h>

COOPA_TEST_SUITE("camera");

using coopa::gfx::engine::components::CameraComponent;
using coopa::scene::SceneObject;

COOPA_TEST(main_camera_is_the_declared_one_else_the_first_started) {
    {
        SceneObject a("cam_a"), b("cam_b");
        auto* cam_a = a.add_component<CameraComponent>();
        auto* cam_b = b.add_component<CameraComponent>();
        cam_b->is_main = true;
        // b starts first despite a existing first -- is_main should still win regardless of order.
        cam_b->start();
        cam_a->start();
        EXPECT_TRUE(CameraComponent::main() == cam_b);
    }
    {
        SceneObject a("cam_a"), b("cam_b");
        auto* cam_a = a.add_component<CameraComponent>();
        auto* cam_b = b.add_component<CameraComponent>();
        // Neither declares is_main -- the first to start() claims the singleton,
        // so a loaded scene always has a main camera.
        cam_a->start();
        cam_b->start();
        EXPECT_TRUE(CameraComponent::main() == cam_a);
    }
}

COOPA_TEST(declared_main_takes_over_from_a_runtime_override) {
    // A runtime override (an editor viewport camera) holds main; a scene's `main: true` camera
    // starting afterwards takes over. A second declared main started later wins (with a warning).
    SceneObject viewer("viewer"), game("game"), other("other");
    auto* cam_viewer = viewer.add_component<CameraComponent>();
    auto* cam_game = game.add_component<CameraComponent>();
    auto* cam_other = other.add_component<CameraComponent>();
    cam_viewer->make_main();
    cam_game->is_main = true;
    cam_game->start();
    EXPECT_TRUE(CameraComponent::main() == cam_game);

    cam_other->is_main = true;
    cam_other->start();
    EXPECT_TRUE(CameraComponent::main() == cam_other);
}

COOPA_TEST(removing_the_main_camera_clears_the_singleton) {
    // Lifetime: main() must never hand back a destroyed component.
    SceneObject holder("cam_temp");
    auto* cam = holder.add_component<CameraComponent>();
    cam->make_main();
    ASSERT_TRUE(CameraComponent::main() == cam);

    holder.remove_component<CameraComponent>();
    EXPECT_TRUE(CameraComponent::main() == nullptr);
}

COOPA_TEST(projection_switches_between_orthographic_and_perspective) {
    using coopa::gfx::engine::components::CameraType;
    SceneObject obj("cam");
    auto* cam = obj.add_component<CameraComponent>();

    cam->set_orthographic(5.0f);
    EXPECT_TRUE(cam->type == CameraType::Orthographic);
    EXPECT_EQ(cam->get_projection_matrix(1.0f)[3][3], 1.0f);  // orthographic: w stays 1

    cam->set_perspective(60.0f);
    EXPECT_TRUE(cam->type == CameraType::Perspective);
    EXPECT_EQ(cam->get_projection_matrix(1.0f)[3][3], 0.0f);  // perspective: w comes from -z
}
