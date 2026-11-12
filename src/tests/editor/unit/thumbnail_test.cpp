/**
 * @file thumbnail_test.cpp
 * @brief The CPU shrink behind the editor's texture thumbnails: it averages rather than
 * samples, keeps the aspect ratio, and leaves a small image alone.
 *
 * Not here: the GPU cache around it (ThumbnailCache needs a device).
 */

#include <coopa/testing/test.h>

#include <cstdint>
#include <vector>

#include "editor/support/fixtures.h"
#include "editor/ui/image_downsample.h"

COOPA_TEST_SUITE("thumbnail");

namespace toy::editor::testing {

namespace {

using namespace toy::editor::ui;

/** @brief A w x h image whose every pixel is `v` in all four channels. */
std::vector<uint8_t> flat(int w, int h, uint8_t v) { return std::vector<uint8_t>(static_cast<size_t>(w) * h * 4, v); }

uint8_t at(const Rgba8Image& img, int x, int y, int c) {
    return img.pixels[(static_cast<size_t>(y) * img.width + x) * 4 + c];
}

} // namespace

/** @brief An image already small enough comes back untouched. */
COOPA_TEST(an_image_within_the_limit_is_returned_as_is) {
    const std::vector<uint8_t> src = flat(64, 32, 200);
    const Rgba8Image out = downsample_rgba8(src.data(), 64, 32, 128);
    expect(out.width == 64 && out.height == 32, "the size is unchanged");
    expect(out.pixels == src, "and so are the pixels");
}

/** @brief Shrinking keeps the aspect ratio and lands on the long side exactly. */
COOPA_TEST(shrinking_keeps_the_aspect_ratio) {
    const std::vector<uint8_t> src = flat(1024, 256, 128);
    const Rgba8Image out = downsample_rgba8(src.data(), 1024, 256, 128);
    expect(out.width == 128, "the long side becomes the limit (" + std::to_string(out.width) + ")");
    expect(out.height == 32, "the short side follows it (" + std::to_string(out.height) + ")");
    expect(out.pixels.size() == static_cast<size_t>(out.width) * out.height * 4, "the buffer matches the size");
    expect(at(out, 10, 10, 0) == 128, "a flat image stays flat");
}

/**
 * @brief Every source pixel is averaged in, which is the whole point: a one-pixel line must
 *        darken the thumbnail rather than vanish between samples.
 */
COOPA_TEST(every_source_pixel_is_averaged_in) {
    // 4x1 black, except one white texel: shrunk to 1 wide, the result is a quarter of white.
    std::vector<uint8_t> src = flat(4, 1, 0);
    for (int c = 0; c < 4; ++c) src[c] = 255;   // the leftmost pixel
    const Rgba8Image out = downsample_rgba8(src.data(), 4, 1, 1);
    expect(out.width == 1 && out.height == 1, "it shrinks to one pixel");
    expect(at(out, 0, 0, 0) == 64, "which is the average of 255, 0, 0, 0 (" + std::to_string(at(out, 0, 0, 0)) + ")");

    // A checkerboard halves to mid grey everywhere, not to one of its two colours.
    std::vector<uint8_t> board = flat(4, 4, 0);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            if ((x + y) % 2) continue;
            for (int c = 0; c < 4; ++c) board[(static_cast<size_t>(y) * 4 + x) * 4 + c] = 255;
        }
    }
    const Rgba8Image half = downsample_rgba8(board.data(), 4, 4, 2);
    expect(half.width == 2 && half.height == 2, "4x4 halves to 2x2");
    expect(at(half, 0, 0, 0) == 128 && at(half, 1, 1, 0) == 128,
           "and every cell is the average of its two black and two white texels");
}

/** @brief Odd sizes, 1x1 and nonsense inputs are all handled without a crash. */
COOPA_TEST(odd_sizes_and_empty_inputs_are_safe) {
    const std::vector<uint8_t> odd = flat(333, 7, 90);
    const Rgba8Image out = downsample_rgba8(odd.data(), 333, 7, 128);
    expect(out.width == 128 && out.height >= 1, "an odd width shrinks, and the short side never rounds to 0");
    expect(!out.empty(), "...and the result has pixels");

    const std::vector<uint8_t> one = flat(1, 1, 7);
    const Rgba8Image tiny = downsample_rgba8(one.data(), 1, 1, 128);
    expect(tiny.width == 1 && tiny.height == 1 && at(tiny, 0, 0, 0) == 7, "a 1x1 image survives");

    expect(downsample_rgba8(nullptr, 10, 10, 10).empty(), "no pixels means no image");
    expect(downsample_rgba8(one.data(), 0, 5, 10).empty(), "nor does a zero dimension");
    expect(downsample_rgba8(one.data(), 1, 1, 0).empty(), "nor a zero limit");
}

} // namespace toy::editor::testing
