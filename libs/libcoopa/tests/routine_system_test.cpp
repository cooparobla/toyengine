/**
 * @file routine_system_test.cpp
 * @brief coopa::routine::RoutineSystem on a Scene: routines resume in the Routine phase
 *        (between behaviour and animation) and stop when the system is removed, time scale
 *        pauses seconds() but not seconds_realtime(), and component helpers (RoutineScope
 *        member, start_routine / stop_routines) tie routines to their owner's lifetime.
 */
#include <coopa/testing/test.h>

#include "support/scene_fixtures.h"

#include <coopa/routine/routine.h>
#include <coopa/routine/routine_system.h>
#include <coopa/routine/runner.h>
#include <coopa/routine/yield.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

#include <memory>
#include <string>
#include <vector>

COOPA_TEST_SUITE("routine_system");

namespace cr = coopa::routine;
using libcoopa_test::RecordingSystem;

namespace {

/** @brief Counts one step per resume for `n` resumes. */
cr::Routine count_frames(int* steps, int n) {
    for (int i = 0; i < n; ++i) {
        ++*steps;
        co_yield cr::next_frame();
    }
}

/** @brief Appends `tag` to a shared log every time it resumes, forever. */
cr::Routine log_every_tick(std::vector<std::string>* log, std::string tag) {
    for (;;) {
        log->push_back(tag);
        co_yield cr::next_frame();
    }
}

cr::Routine wait_seconds_then_mark(bool* done, float duration) {
    co_yield cr::seconds(duration);
    *done = true;
}

cr::Routine wait_realtime_then_mark(bool* done, float duration) {
    co_yield cr::seconds_realtime(duration);
    *done = true;
}

/** @brief A component that owns its routines through a RoutineScope. */
class RoutineComponent : public coopa::scene::Component {
public:
    std::string type_name() const override { return "RoutineComponent"; }
    void start() override { scope.start(*this, count_frames(&steps, 1000)); }

    cr::RoutineScope scope;
    int steps = 0;
};

cr::RoutineSystem* add_routine_system(coopa::scene::Scene& scene) {
    return static_cast<cr::RoutineSystem*>(
        scene.add_system(std::make_unique<cr::RoutineSystem>(), coopa::scene::UpdatePhase::Routine));
}

} // namespace

COOPA_TEST(routines_resume_between_behaviour_and_animation_and_stop_with_the_system) {
    coopa::scene::Scene scene("RoutinePhase");
    std::vector<std::string> log;
    scene.add_system(std::make_unique<RecordingSystem>("behaviour", &log), 200);
    cr::RoutineSystem* system = add_routine_system(scene);
    scene.add_system(std::make_unique<RecordingSystem>("animation", &log), 300);

    system->runner().start(log_every_tick(&log, "routine"));
    log.clear(); // Drop the entry the immediate first resume produced.

    scene.update(0.016f);
    ASSERT_EQ(log.size(), 3u);
    EXPECT_EQ(log[0], std::string("behaviour"));
    EXPECT_EQ(log[1], std::string("routine"));
    EXPECT_EQ(log[2], std::string("animation"));

    // Removing the system stops everything it was running.
    ASSERT_TRUE(scene.remove_system("Routine"));
    log.clear();
    scene.update(0.016f);
    EXPECT_EQ(log.size(), 2u);
}

COOPA_TEST(time_scale_pauses_seconds_but_not_realtime) {
    coopa::scene::Scene scene("TimeScale");
    cr::RoutineSystem* system = add_routine_system(scene);
    system->set_time_scale(0.0f);

    bool scaled = false;
    bool realtime = false;
    system->runner().start(wait_seconds_then_mark(&scaled, 0.5f));
    system->runner().start(wait_realtime_then_mark(&realtime, 0.5f));

    for (int i = 0; i < 10; ++i) scene.update(0.125f);
    EXPECT_FALSE(scaled);  // Paused: scaled time never advanced.
    EXPECT_TRUE(realtime); // Wall clock kept running.

    system->set_time_scale(1.0f);
    for (int i = 0; i < 4; ++i) scene.update(0.125f);
    EXPECT_TRUE(scaled);
}

COOPA_TEST(component_routines_start_stop_and_die_with_their_owner) {
    coopa::scene::Scene scene("ComponentRoutines");
    cr::RoutineSystem* system = add_routine_system(scene);

    auto obj = std::make_unique<coopa::scene::SceneObject>("Actor");
    auto* component = obj->add_component<RoutineComponent>();
    coopa::scene::SceneObject* raw_obj = scene.add_root_object(std::move(obj));
    scene.start(); // Publishes Scene* onto every component, then calls start().

    // The RoutineScope member started one routine, owned by the component.
    EXPECT_EQ(system->runner().active_count(), 1u);
    EXPECT_EQ(component->steps, 1);

    scene.update(0.016f);
    EXPECT_EQ(component->steps, 2);

    // start_routine() is the free-function StartCoroutine equivalent.
    int extra_steps = 0;
    auto handle = cr::start_routine(*component, count_frames(&extra_steps, 1000));
    EXPECT_TRUE(handle.is_running());
    EXPECT_EQ(system->runner().active_count(), 2u);

    EXPECT_EQ(cr::stop_routines(*component), 2u);
    EXPECT_FALSE(handle.is_running());
    EXPECT_EQ(system->runner().active_count(), 0u);

    // Destroying the owner must not leave the scope pointing at a dead routine.
    ASSERT_TRUE(scene.remove_root_object(raw_obj));
    scene.update(0.016f);
    EXPECT_EQ(system->runner().active_count(), 0u);
}
