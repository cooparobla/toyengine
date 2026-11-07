#include <toyengine/world/terra_image.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

#include <toyengine/world/png16.h>
#include <toyengine/world/terra_data.h>

namespace toy {
namespace world {

namespace {

/** @brief Bilinear sample of a `sw x sh` row-major grid at destination texel (dx, dy) of `dw x dh`. */
template <int Channels, typename Fetch>
void resample(int sw, int sh, Fetch fetch, int dw, int dh, const std::function<void(int, int, const float*)>& write) {
    // Source texel centres map onto the destination the same way TerraData's own planes already
    // relate a tile's corners to its centre: texel i of dw covers the same normalised span as
    // texel i of sw, so index by the ratio of extents, not by a +0.5 half-texel offset -- both
    // planes already agree their sample (0,0) is the same corner.
    const float sx = sw > 1 ? static_cast<float>(sw - 1) / static_cast<float>(std::max(1, dw - 1)) : 0.0f;
    const float sy = sh > 1 ? static_cast<float>(sh - 1) / static_cast<float>(std::max(1, dh - 1)) : 0.0f;
    std::vector<float> out(Channels);
    for (int y = 0; y < dh; ++y) {
        const float fy = std::clamp(static_cast<float>(y) * sy, 0.0f, static_cast<float>(sh - 1));
        const int y0 = static_cast<int>(fy), y1 = std::min(y0 + 1, sh - 1);
        const float ty = fy - static_cast<float>(y0);
        for (int x = 0; x < dw; ++x) {
            const float fx = std::clamp(static_cast<float>(x) * sx, 0.0f, static_cast<float>(sw - 1));
            const int x0 = static_cast<int>(fx), x1 = std::min(x0 + 1, sw - 1);
            const float tx = fx - static_cast<float>(x0);
            for (int c = 0; c < Channels; ++c) {
                const float v00 = fetch(x0, y0, c), v10 = fetch(x1, y0, c);
                const float v01 = fetch(x0, y1, c), v11 = fetch(x1, y1, c);
                const float top = v00 + (v10 - v00) * tx;
                const float bot = v01 + (v11 - v01) * tx;
                out[static_cast<std::size_t>(c)] = top + (bot - top) * ty;
            }
            write(x, y, out.data());
        }
    }
}

} // namespace

bool import_height_png(const std::string& path, TerraData& data) {
    if (!data.has_heights()) return false;
    int w = 0, h = 0;
    std::vector<std::uint16_t> px;
    if (!read_png16_grey(path, w, h, px) || w <= 0 || h <= 0) return false;

    const int dw = data.height_nx(), dh = data.height_ny();
    const float scale = data.geometry().height / 65535.0f;
    const auto fetch = [&](int x, int y, int) -> float {
        return static_cast<float>(px[static_cast<std::size_t>(y) * w + x]);
    };
    resample<1>(w, h, fetch, dw, dh, [&](int x, int y, const float* v) {
        data.height_at(x, y) = v[0] * scale;
    });
    data.mark_height_dirty(IRect{0, 0, dw, dh});
    return true;
}

bool export_height_png(const std::string& path, const TerraData& data) {
    if (!data.has_heights()) return false;
    const int w = data.height_nx(), h = data.height_ny();
    const float scale = data.geometry().height > 0.0f ? 65535.0f / data.geometry().height : 0.0f;
    std::vector<std::uint16_t> px(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float v = std::clamp(data.height_at(x, y) * scale, 0.0f, 65535.0f);
            px[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint16_t>(v + 0.5f);
        }
    }
    return write_png16_grey(path, w, h, px);
}

bool import_layer_png(const std::string& path, TerraData& data, int which) {
    if (which != 0 && which != 1) return false;
    int w = 0, h = 0;
    std::vector<std::uint8_t> px;
    if (!read_png_rgba8(path, w, h, px) || w <= 0 || h <= 0) return false;

    const int dw = data.layer_nx(), dh = data.layer_ny();
    const auto fetch = [&](int x, int y, int c) -> float {
        return static_cast<float>(px[(static_cast<std::size_t>(y) * w + x) * 4 + c]);
    };
    std::vector<std::uint8_t>& plane = data.layer_plane(which);
    resample<4>(w, h, fetch, dw, dh, [&](int x, int y, const float* v) {
        for (int c = 0; c < 4; ++c) {
            plane[(static_cast<std::size_t>(y) * dw + x) * 4 + c] =
                static_cast<std::uint8_t>(std::clamp(v[c] + 0.5f, 0.0f, 255.0f));
        }
    });
    data.mark_layer_dirty(IRect{0, 0, dw, dh});
    return true;
}

bool export_layer_png(const std::string& path, const TerraData& data, int which) {
    if (which != 0 && which != 1) return false;
    return write_png_rgba8(path, data.layer_nx(), data.layer_ny(), data.layer_plane(which));
}

} // namespace world
} // namespace toy
