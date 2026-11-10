/**
 * @file scene_manager_test.cpp
 * @brief coopa::scene::SceneManager: concurrent multi-scene processing on a JobEngine (guarded
 *        by COOPA_SCENE_THREAD_CHECKS, on for this binary) and the serial no-engine path with
 *        active flags and removal.
 */
#include <coopa/testing/test.h>

#include "support/scene_fixtures.h"

#include <coopa/job/engine.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_manager.h>
#include <coopa/scene/scene_object.h>

#include <memory>
#include <string>
#include <vector>

COOPA_TEST_SUITE("scene_manager");

using libcoopa_test::CountingComponent;

COOPA_TEST(concurrent_scenes_update_exactly_like_serial) {
    coopa::job::JobEngine engine(4);
    coopa::scene::SceneManager mgr;
    mgr.set_job_engine(&engine);

    std::vector<CountingComponent*> counters;
    for (int s = 0; s < 6; ++s) {
        auto scene = std::make_unique<coopa::scene::Scene>("S" + std::to_string(s));
        for (int i = 0; i < 20; ++i) {
            auto obj = std::make_unique<coopa::scene::SceneObject>("Obj");
            counters.push_back(obj->add_component<CountingComponent>());
            scene->add_root_object(std::move(obj));
        }
        mgr.add_scene(std::move(scene));
    }

    // Six scenes concurrently processed across four workers, three frames --
    // if two of Scene's methods ever ran concurrently on the SAME Scene,
    // COOPA_SCENE_THREAD_CHECKS (on for this test binary) would abort.
    for (int frame = 0; frame < 3; ++frame) {
        mgr.begin_frame();
        mgr.update(0.016f);
        mgr.late_update(0.016f);
        mgr.end_frame();
    }

    int wrong = 0;
    for (auto* c : counters) wrong += c->update_count != 3;
    EXPECT_EQ(wrong, 0);
    engine.shutdown();
}

COOPA_TEST(serial_path_honours_active_flags_and_removal) {
    coopa::scene::SceneManager mgr; // no engine installed -- serial fallback path
    std::vector<CountingComponent*> counters;
    std::vector<coopa::scene::Scene*> raws;
    for (int s = 0; s < 3; ++s) {
        auto scene = std::make_unique<coopa::scene::Scene>("S" + std::to_string(s));
        auto obj = std::make_unique<coopa::scene::SceneObject>("Obj");
        counters.push_back(obj->add_component<CountingComponent>());
        scene->add_root_object(std::move(obj));
        raws.push_back(mgr.add_scene(std::move(scene)));
    }
    ASSERT_TRUE(mgr.has_scene());
    EXPECT_TRUE(&mgr.get_active_scene() == raws[0]); // first scene added becomes active
    EXPECT_EQ(mgr.scenes().size(), 3u);

    mgr.update(0.016f);
    mgr.late_update(0.016f);
    for (auto* c : counters) EXPECT_EQ(c->update_count, 1);

    mgr.set_scene_active(raws[1], false);
    mgr.update(0.016f);
    mgr.late_update(0.016f);
    EXPECT_EQ(counters[0]->update_count, 2);
    EXPECT_EQ(counters[1]->update_count, 1); // unchanged -- inactive
    EXPECT_EQ(counters[2]->update_count, 2);

    EXPECT_TRUE(mgr.remove_scene(raws[1]));
    EXPECT_EQ(mgr.scenes().size(), 2u);
    EXPECT_FALSE(mgr.remove_scene(raws[1])); // already removed
}
