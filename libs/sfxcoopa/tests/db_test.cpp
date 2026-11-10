/**
 * @file db_test.cpp
 * @brief util/db.h: decibel <-> linear gain conversion, including the silence floor.
 */
#include <coopa/testing/test.h>

#include <sfxcoopa/util/db.h>

COOPA_TEST_SUITE("db");

COOPA_TEST(converts_known_points_round_trips_and_floors_silence) {
    using namespace coopa::sfx::util;
    EXPECT_NEAR(db_to_linear(0.0f), 1.0f, 1e-6f);
    EXPECT_NEAR(db_to_linear(20.0f), 10.0f, 1e-4f);
    EXPECT_NEAR(db_to_linear(-20.0f), 0.1f, 1e-5f);
    EXPECT_NEAR(linear_to_db(1.0f), 0.0f, 1e-4f);
    EXPECT_NEAR(linear_to_db(10.0f), 20.0f, 1e-3f);
    for (float db = -60.0f; db <= 12.0f; db += 6.0f) {
        EXPECT_NEAR(linear_to_db(db_to_linear(db)), db, 1e-3f);
    }
    // Zero (and nonsense negative) gain map to the finite silence floor, never -inf/NaN, so
    // meters and fades downstream never see a non-finite level.
    EXPECT_EQ(linear_to_db(0.0f), k_silence_db);
    EXPECT_EQ(linear_to_db(-1.0f), k_silence_db);
    EXPECT_LT(k_silence_db, 0.0f);
}
