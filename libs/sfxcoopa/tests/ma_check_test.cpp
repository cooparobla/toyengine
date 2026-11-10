/**
 * @file ma_check_test.cpp
 * @brief util/error.h: SFX_MA_CHECK turns a failed miniaudio result into an exception.
 */
#include <coopa/testing/test.h>

#include <sfxcoopa/util/error.h>

#include <stdexcept>
#include <string>

COOPA_TEST_SUITE("ma_check");

COOPA_TEST(throws_on_failure_naming_the_result_and_passes_success) {
    SFX_MA_CHECK(MA_SUCCESS);  // must not throw
    bool threw = false;
    try {
        SFX_MA_CHECK(MA_ERROR);
    } catch (const std::runtime_error& e) {
        threw = true;
        // The result code's name is what makes a device/decoder failure diagnosable.
        EXPECT_TRUE(std::string(e.what()).find("MA_ERROR") != std::string::npos);
    }
    EXPECT_TRUE(threw);
}
