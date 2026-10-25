#include "editor/ui/image_downsample.h"

#include <algorithm>
#include <cmath>

namespace toy {
namespace editor {
namespace ui {

Rgba8Image downsample_rgba8(const uint8_t* src, int width, int height, int max_side) {
    Rgba8Image out;
    if (!src || width <= 0 || height <= 0 || max_side <= 0) return out;

    const int longest = std::max(width, height);
    if (longest <= max_side) {
        out.width = width;
        out.height = height;
        out.pixels.assign(src, src + static_cast<size_t>(width) * height * 4);
        return out;
    }
    const double scale = static_cast<double>(max_side) / longest;
    out.width = std::max(1, static_cast<int>(std::lround(width * scale)));
    out.height = std::max(1, static_cast<int>(std::lround(height * scale)));
    out.pixels.assign(static_cast<size_t>(out.width) * out.height * 4, 0);

    for (int y = 0; y < out.height; ++y) {
        // The source rows this destination row covers. Rounding the edges rather than the
        // centres is what makes the boxes tile the image exactly, with no gaps or overlaps.
        const int y0 = static_cast<int>(static_cast<int64_t>(y) * height / out.height);
        const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<int64_t>(y + 1) * height / out.height));
        for (int x = 0; x < out.width; ++x) {
            const int x0 = static_cast<int>(static_cast<int64_t>(x) * width / out.width);
            const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<int64_t>(x + 1) * width / out.width));
            uint32_t sum[4] = {0, 0, 0, 0};
            uint32_t n = 0;
            for (int sy = y0; sy < y1 && sy < height; ++sy) {
                const uint8_t* row = src + (static_cast<size_t>(sy) * width + x0) * 4;
                for (int sx = x0; sx < x1 && sx < width; ++sx, row += 4) {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                    sum[3] += row[3];
                    ++n;
                }
            }
            if (n == 0) continue;
            uint8_t* dst = out.pixels.data() + (static_cast<size_t>(y) * out.width + x) * 4;
            for (int c = 0; c < 4; ++c) dst[c] = static_cast<uint8_t>((sum[c] + n / 2) / n);
        }
    }
    return out;
}

} // namespace ui
} // namespace editor
} // namespace toy
