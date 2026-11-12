/**
 * @file sprite.h
 * @brief A named sub-region of a Texture, with optional 9-slice borders.
 */

#ifndef UICOOPA_RENDER_SPRITE_H
#define UICOOPA_RENDER_SPRITE_H

#include <gfxcoopa/engine/data/texture.h>
#include <uicoopa/layout/rect.h>
#include <glm/glm.hpp>

namespace coopa {
namespace ui {

/**
 * @struct Sprite
 * @brief A texture region plus optional nine-slice border metadata.
 */
struct Sprite {
    coopa::gfx::engine::data::Texture* texture = nullptr;         /**< Non-owning; caller owns the Texture's lifetime. */
    Rect      uv{ glm::vec2(0.0f), glm::vec2(1.0f) };              /**< Sub-region within the texture, in [0,1] UV space. */
    glm::vec4 border{0.0f};                                        /**< Nine-slice border in source texture pixels: (left, bottom, right, top). */

    /** @brief Whether this sprite defines a non-degenerate nine-slice border. */
    bool is_nine_sliced() const {
        return border.x > 0.0f || border.y > 0.0f || border.z > 0.0f || border.w > 0.0f;
    }
};

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_RENDER_SPRITE_H
