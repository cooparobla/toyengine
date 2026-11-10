/**
 * @file animation_events_test.cpp
 * @brief Clip events fired by the Animator: exact counts and order across loop boundaries
 *        (including one big step over several), ping-pong endpoints and Once clips, delivery on
 *        both the on_event signal and the scene event bus, and only the crossfade target firing
 *        (with a handler that crossfades from inside the event).
 */
#include <coopa/testing/test.h>

#include "support/animation.h"

#include <coopa/animation/animation_system.h>
#include <coopa/animation/animator.h>
#include <coopa/scene/scene.h>

#include <map>
#include <string>
#include <vector>

COOPA_TEST_SUITE("animation_events");

using namespace coopa::anim;
using namespace coopa::scene;
using libcoopa_test::add_animated_object;
using libcoopa_test::event_clip;
using libcoopa_test::step;

namespace {
struct EventCounter {
    std::map<std::string, int> counts;
    std::vector<std::string>   order;
};
} // namespace

COOPA_TEST(looping_events_fire_once_per_cycle_in_order_on_signal_and_bus) {
    Scene scene("EventsLoop");
    auto* animator = add_animated_object(scene, "Rig");
    animator->add_state("s", event_clip(1.0f, WrapMode::Loop, {{0.0f, "start"}, {0.5f, "mid"}}));
    animator->auto_play = "s";

    EventCounter signal_counts, bus_counts;
    int wrong_state = 0;
    animator->on_event.connect([&](const AnimationEvent& e) {
        ++signal_counts.counts[e.name];
        signal_counts.order.push_back(e.name);
    });
    scene.events().on("Rig", "anim_event", [&](const coopa::event::EventArgs& a) {
        ++bus_counts.counts[a.get<std::string>("name", "")];
        wrong_state += a.get<std::string>("state", "") != "s";
    });

    install_animation_system(scene);
    scene.start();
    step(scene, 0.125f, 24); // t = 3.0 exactly: three full cycles

    EXPECT_EQ(signal_counts.counts["start"], 3);
    EXPECT_EQ(signal_counts.counts["mid"], 3);
    EXPECT_EQ(bus_counts.counts["start"], 3);
    EXPECT_EQ(bus_counts.counts["mid"], 3);
    EXPECT_EQ(wrong_state, 0); // the bus payload names the playing state
    ASSERT_FALSE(signal_counts.order.empty());
    EXPECT_EQ(signal_counts.order.front(), std::string("start"));

    // One big step crossing two loop boundaries fires each event once per cycle crossed, in order.
    signal_counts = EventCounter{};
    step(scene, 2.0f, 1); // 3.0 -> 5.0
    EXPECT_EQ(signal_counts.counts["start"], 2);
    EXPECT_EQ(signal_counts.counts["mid"], 2);
    ASSERT_EQ(signal_counts.order.size(), 4u);
    EXPECT_EQ(signal_counts.order[0], std::string("start"));
    EXPECT_EQ(signal_counts.order[1], std::string("mid"));
    EXPECT_EQ(signal_counts.order[2], std::string("start"));
}

COOPA_TEST(pingpong_endpoints_fire_once_per_visit_and_once_clips_fire_once) {
    {
        Scene scene("EventsPingPong");
        auto* animator = add_animated_object(scene, "Rig");
        animator->add_state("s", event_clip(1.0f, WrapMode::PingPong, {{0.0f, "lo"}, {0.5f, "mid"}, {1.0f, "hi"}}));
        animator->auto_play = "s";
        std::map<std::string, int> counts;
        animator->on_event.connect([&](const AnimationEvent& e) { ++counts[e.name]; });
        install_animation_system(scene);
        scene.start();
        step(scene, 0.125f, 32); // t = 4.0: two full there-and-back periods
        // Endpoints fire once per visit (0 at t=0,2; 1 at t=1,3); the midpoint both ways.
        EXPECT_EQ(counts["lo"], 2);
        EXPECT_EQ(counts["hi"], 2);
        EXPECT_EQ(counts["mid"], 4);
    }
    {
        Scene scene("EventsOnce");
        auto* animator = add_animated_object(scene, "Rig");
        animator->add_state("s", event_clip(1.0f, WrapMode::Once, {{0.25f, "a"}, {1.0f, "end"}}));
        animator->auto_play = "s";
        std::map<std::string, int> counts;
        int finished = 0;
        animator->on_event.connect([&](const AnimationEvent& e) { ++counts[e.name]; });
        animator->on_state_finished.connect([&](const std::string&) { ++finished; });
        install_animation_system(scene);
        scene.start();
        step(scene, 0.3f, 10); // clamps at the end; held there for 2 more seconds
        EXPECT_EQ(counts["a"], 1);
        EXPECT_EQ(counts["end"], 1);
        EXPECT_EQ(finished, 1);
    }
}

COOPA_TEST(during_a_crossfade_only_the_target_state_fires_events) {
    Scene scene("EventsCrossfade");
    auto* animator = add_animated_object(scene, "Rig");
    animator->add_state("A", event_clip(1.0f, WrapMode::Loop, {{0.5f, "a"}}));
    animator->add_state("B", event_clip(1.0f, WrapMode::Loop, {{0.25f, "b"}}));
    animator->auto_play = "A";
    std::map<std::string, int> counts;
    animator->on_event.connect([&](const AnimationEvent& e) { ++counts[e.name]; });
    install_animation_system(scene);
    scene.start();
    step(scene, 0.125f, 2); // A: 0 -> 0.25, nothing crossed
    EXPECT_EQ(counts["a"], 0);
    animator->crossfade("B", 0.5f);
    step(scene, 0.125f, 4); // A 0.25 -> 0.75 (crosses 0.5, silent), B 0 -> 0.5 (fires b)
    EXPECT_EQ(counts["a"], 0);
    EXPECT_EQ(counts["b"], 1);
    step(scene, 0.125f, 8); // B 0.5 -> 1.5: b once more
    EXPECT_EQ(counts["a"], 0);
    EXPECT_EQ(counts["b"], 2);

    // A handler may crossfade from inside the event without corrupting playback.
    bool switched = false;
    animator->on_event.connect([&](const AnimationEvent& e) {
        if (e.name == "b" && !switched) {
            switched = true;
            animator->crossfade("A", 0.25f);
        }
    });
    step(scene, 0.125f, 8);
    EXPECT_TRUE(switched);
    EXPECT_EQ(animator->current_state(), std::string("A"));
}
