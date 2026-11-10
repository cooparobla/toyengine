/**
 * @file stat_test.cpp
 * @brief coopa::stat::Resource and StatBlock: damage/heal clamping, set_max with and without
 *        keeping the ratio, delayed regeneration, edge-triggered on_depleted / on_changed, and
 *        StatBlock handing out pointer-stable resources as it grows.
 */
#include <coopa/testing/test.h>

#include <coopa/stat/resource.h>
#include <coopa/stat/stat_block.h>

#include <string>

COOPA_TEST_SUITE("stat");

using namespace coopa::stat;

COOPA_TEST(damage_and_heal_clamp_to_zero_and_max) {
    Resource r(100.0f);
    r.damage(30.0f);
    EXPECT_NEAR(r.current, 70.0f, 1e-4f);

    r.damage(1000.0f);
    EXPECT_EQ(r.current, 0.0f);
    EXPECT_TRUE(r.is_depleted());

    r.heal(1000.0f);
    EXPECT_EQ(r.current, r.max);
    EXPECT_TRUE(r.is_full());
}

COOPA_TEST(set_max_keeps_ratio_or_clamps_current) {
    Resource r(100.0f);
    r.damage(50.0f); // current = 50, normalized 0.5
    r.set_max(50.0f, /*keep_ratio=*/true);
    EXPECT_NEAR(r.max, 50.0f, 1e-4f);
    EXPECT_NEAR(r.current, 25.0f, 1e-4f); // 0.5 * 50

    Resource r2(100.0f);
    r2.damage(10.0f); // current = 90
    r2.set_max(50.0f, /*keep_ratio=*/false);
    EXPECT_NEAR(r2.current, 50.0f, 1e-4f); // clamped down to the new max
}

COOPA_TEST(regeneration_waits_out_the_post_damage_delay_then_stops_at_max) {
    Resource r(100.0f, /*regen=*/10.0f, /*delay=*/1.0f);
    r.damage(50.0f);

    r.tick(0.5f); // still inside the post-damage delay window
    EXPECT_NEAR(r.current, 50.0f, 1e-4f);

    r.tick(0.6f); // crosses the delay -- regen resumes
    EXPECT_GT(r.current, 50.0f);
    const float after_first_regen_tick = r.current;

    r.tick(0.5f);
    EXPECT_GT(r.current, after_first_regen_tick);

    r.tick(1000.0f); // a long tick clamps at max, doesn't overshoot
    EXPECT_TRUE(r.is_full());
}

COOPA_TEST(on_depleted_fires_once_per_crossing_to_zero) {
    Resource r(100.0f);
    int depleted_count = 0;
    auto conn = r.on_depleted.connect([&]() { depleted_count++; });

    r.damage(50.0f);
    EXPECT_EQ(depleted_count, 0);
    r.damage(50.0f); // crosses to 0
    EXPECT_EQ(depleted_count, 1);
    r.damage(10.0f); // already at 0 -- no further edge
    EXPECT_EQ(depleted_count, 1);
    r.heal(20.0f);
    r.damage(20.0f); // crosses to 0 again -- a fresh edge
    EXPECT_EQ(depleted_count, 2);
}

COOPA_TEST(on_changed_is_silent_when_nothing_changes) {
    Resource r(100.0f); // regen_per_second = 0 by default
    int changed_count = 0;
    auto conn = r.on_changed.connect([&](float, float) { changed_count++; });

    r.tick(1.0f); // already full, no regen configured -- silent
    EXPECT_EQ(changed_count, 0);
    r.damage(0.0f); // non-positive amount -- no-op
    EXPECT_EQ(changed_count, 0);
    r.damage(10.0f); // a real change
    EXPECT_EQ(changed_count, 1);
}

COOPA_TEST(stat_block_resources_stay_pointer_stable_as_it_grows) {
    StatBlock block;
    Resource* health = &block.resource("health");
    health->current = 42.0f;

    for (int i = 0; i < 50; ++i) block.resource("stat_" + std::to_string(i));

    EXPECT_NEAR(health->current, 42.0f, 1e-4f); // would read freed memory if storage relocated
    EXPECT_EQ(block.size(), 51u);
}
