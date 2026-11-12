/**
 * @file ui_vertex.h
 * @brief The single 2D vertex format used by every uicoopa draw call.
 */

#ifndef UICOOPA_RENDER_UI_VERTEX_H
#define UICOOPA_RENDER_UI_VERTEX_H

#include <gfxcoopa/types/vertex_layout.h>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <algorithm>

namespace coopa {
namespace ui {

/**
 * @struct UiVertex
 * @brief Position (canvas pixels), UV, and packed color for a single UI vertex.
 */
struct UiVertex {
    float    x, y;  /**< Position in canvas pixel space, origin bottom-left, +Y up. */
    float    u, v;  /**< Texture coordinates. */
    uint32_t color; /**< Packed RGBA8, Format::RGBA8_Unorm (r in the low byte). */

    /** @brief Packs four [0,1] float color channels into UiVertex::color's layout. */
    static uint32_t pack_color(float r, float g, float b, float a) {
        auto to_u8 = [](float v) {
            return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        return to_u8(r) | (to_u8(g) << 8) | (to_u8(b) << 16) | (to_u8(a) << 24);
    }

    /** @brief This vertex format's binding+attribute layout, for the sealed Pipeline ctor. */
    static coopa::gfx::VertexLayout layout() {
        return coopa::gfx::VertexLayout{}
            .binding(0, sizeof(UiVertex))
            .attribute(0, coopa::gfx::Format::RG32_Sfloat, static_cast<uint32_t>(offsetof(UiVertex, x)))
            .attribute(1, coopa::gfx::Format::RG32_Sfloat, static_cast<uint32_t>(offsetof(UiVertex, u)))
            .attribute(2, coopa::gfx::Format::RGBA8_Unorm, static_cast<uint32_t>(offsetof(UiVertex, color)));
    }
};

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_RENDER_UI_VERTEX_H
