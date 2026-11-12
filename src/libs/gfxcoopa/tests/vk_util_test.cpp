/**
 * @file vk_util_test.cpp
 * @brief gfxcoopa/util/error.h and util/format.h, no device needed: GFX_VK_CHECK turns a
 *        failed VkResult into an exception, and the format helpers' channel mapping and byte
 *        sizes -- which every upload and readback sizes its copies from -- are right.
 *
 * Not covered: vk_result_string()'s exact text (diagnostic output, not a contract).
 */
#include <coopa/testing/test.h>

#include <gfxcoopa/util/error.h>
#include <gfxcoopa/util/format.h>

#include <stdexcept>

COOPA_TEST_SUITE("vk_util");

COOPA_TEST(vk_check_throws_on_error_and_passes_success) {
    GFX_VK_CHECK(VK_SUCCESS);  // must not throw
    bool threw = false;
    try {
        GFX_VK_CHECK(VK_ERROR_INITIALIZATION_FAILED);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

COOPA_TEST(format_helpers_map_channels_and_report_byte_sizes) {
    using namespace coopa::gfx::util;
    EXPECT_EQ(format_from_channels(4, false), VK_FORMAT_R8G8B8A8_UNORM);
    EXPECT_EQ(format_from_channels(4, true), VK_FORMAT_R8G8B8A8_SRGB);
    EXPECT_EQ(format_from_channels(1, false), VK_FORMAT_R8_UNORM);
    // 3-channel images have no portable 8-bit Vulkan format: rejected, never silently mis-sized.
    ASSERT_THROWS(format_from_channels(3, false));

    EXPECT_EQ(format_byte_size(VK_FORMAT_R8G8B8A8_UNORM), 4u);
    EXPECT_EQ(format_byte_size(VK_FORMAT_R16G16B16A16_SFLOAT), 8u);
    EXPECT_EQ(format_byte_size(VK_FORMAT_R8_UNORM), 1u);

    EXPECT_TRUE(format_has_depth(VK_FORMAT_D32_SFLOAT));
    EXPECT_FALSE(format_has_depth(VK_FORMAT_R8G8B8A8_UNORM));
    EXPECT_TRUE(format_has_stencil(VK_FORMAT_D24_UNORM_S8_UINT));
    EXPECT_FALSE(format_has_stencil(VK_FORMAT_D32_SFLOAT));
}
