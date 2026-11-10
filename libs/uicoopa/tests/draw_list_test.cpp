/**
 * @file draw_list_test.cpp
 * @brief DrawList batching: texture/clip/z_order batch breaks, finalize_z_order()'s stable
 *        reorder, and nine-slice geometry (null-texture fallback and Y-flipped UV breakpoints).
 *
 * Not covered here: GPU submission (UiPass needs a device; the windowed demos exercise it).
 */

#include <coopa/testing/test.h>

#include "support/ui_test_support.h"

COOPA_TEST_SUITE("draw_list");

COOPA_TEST(texture_and_clip_changes_break_batches) {
    DrawList dl;
    dl.begin(Rect{ glm::vec2(0.0f), glm::vec2(1000.0f, 1000.0f) });

    coopa::gfx::TextureView tex_a{0x1};
    coopa::gfx::TextureView tex_b{0x2};

    dl.set_texture(tex_a);
    dl.add_quad(Rect{ {0, 0}, {10, 10} }, Rect{ {0, 0}, {1, 1} }, 0xFFFFFFFFu);
    dl.add_quad(Rect{ {20, 20}, {30, 30} }, Rect{ {0, 0}, {1, 1} }, 0xFFFFFFFFu);
    ASSERT_TRUE(dl.batches().size() == 1);
    ASSERT_TRUE(dl.batches()[0].index_count == 12);

    dl.set_texture(tex_b);
    dl.add_quad(Rect{ {40, 40}, {50, 50} }, Rect{ {0, 0}, {1, 1} }, 0xFFFFFFFFu);
    ASSERT_TRUE(dl.batches().size() == 2);
    ASSERT_TRUE(dl.batches()[1].texture_view == tex_b);

    dl.push_clip(Rect{ {0, 0}, {100, 100} });
    dl.add_quad(Rect{ {5, 5}, {6, 6} }, Rect{ {0, 0}, {1, 1} }, 0xFFFFFFFFu);
    ASSERT_TRUE(dl.batches().size() == 3);  // clip change breaks the batch even with the same texture

    dl.pop_clip();
    ASSERT_VEC_NEAR(dl.current_clip().max, glm::vec2(1000.0f, 1000.0f), 1e-4f);

    ASSERT_TRUE(dl.vertices().size() == 16);
    ASSERT_TRUE(dl.indices().size() == 24);
}

COOPA_TEST(finalize_z_order_sorts_batches_stably) {
    DrawList dl;
    dl.begin(Rect{ glm::vec2(0.0f), glm::vec2(1000.0f, 1000.0f) });

    coopa::gfx::TextureView tex_a{0x1};  // emitted at z_order 0
    coopa::gfx::TextureView tex_b{0x2};  // emitted at z_order 1
    coopa::gfx::TextureView tex_c{0x3};  // emitted at z_order 0, after tex_b

    dl.set_texture(tex_a);
    dl.add_quad(Rect{ {0, 0}, {10, 10} }, Rect{ {0, 0}, {1, 1} }, 0xFFFFFFFFu);

    dl.set_z_order(1);
    dl.set_texture(tex_b);
    dl.add_quad(Rect{ {20, 20}, {30, 30} }, Rect{ {0, 0}, {1, 1} }, 0xFFFFFFFFu);

    dl.set_z_order(0);
    dl.set_texture(tex_c);
    dl.add_quad(Rect{ {40, 40}, {50, 50} }, Rect{ {0, 0}, {1, 1} }, 0xFFFFFFFFu);

    // Before finalize_z_order(): a z_order change breaks the batch even with an
    // unrelated texture change, same as a clip change already does.
    ASSERT_TRUE(dl.batches().size() == 3);
    ASSERT_TRUE(dl.batches()[0].texture_view == tex_a);
    ASSERT_TRUE(dl.batches()[1].texture_view == tex_b);
    ASSERT_TRUE(dl.batches()[2].texture_view == tex_c);

    dl.finalize_z_order();

    // Only reorders batch metadata: the higher layer (tex_b) sorts last (drawn on
    // top), while the two z_order=0 batches keep their original relative order.
    ASSERT_TRUE(dl.batches().size() == 3);
    ASSERT_TRUE(dl.batches()[0].texture_view == tex_a);
    ASSERT_TRUE(dl.batches()[1].texture_view == tex_c);
    ASSERT_TRUE(dl.batches()[2].texture_view == tex_b);
}

COOPA_TEST(nine_slice_without_texture_falls_back_to_one_quad) {
    DrawList dl;
    dl.begin(Rect{ glm::vec2(0.0f), glm::vec2(1000.0f, 1000.0f) });

    Sprite sprite;
    sprite.texture = nullptr;  // no_texture path exercised separately; here we force the fallback
    sprite.border = { 8.0f, 8.0f, 8.0f, 8.0f };

    // With texture == nullptr, add_nine_slice must fall back to a single quad (is_nine_sliced()
    // alone isn't enough — a real Texture* is required to convert pixel borders to UV space).
    dl.add_nine_slice(Rect{ {0, 0}, {100, 100} }, sprite, 0xFFFFFFFFu);
    ASSERT_TRUE(dl.batches().size() == 1);
    ASSERT_TRUE(dl.vertices().size() == 4);
    ASSERT_TRUE(dl.indices().size() == 6);
}

COOPA_TEST(nine_slice_flipped_uv_breakpoints_walk_inward) {
    // Regression test for draw_list.h's nine_slice_axis_breakpoints(): a Y-flipped UV
    // range (uv_hi < uv_lo, the FontAtlas/SpriteSheet convention -- see sprite_sheet.h's
    // pixel_rect_to_uv()) must still walk its border breakpoints INWARD, toward the
    // interior of the range, not outward past it.
    std::array<float, 4> pos{}, uv{};

    // Ascending UV range (uv_hi > uv_lo): border walks up from uv_lo, down from uv_hi.
    nine_slice_axis_breakpoints(0.0f, 100.0f, 10.0f, 10.0f, 0.2f, 0.8f, 0.05f, 0.05f, pos, uv);
    ASSERT_TRUE(pos[0] == 0.0f && pos[1] == 10.0f && pos[2] == 90.0f && pos[3] == 100.0f);
    ASSERT_NEAR(uv[0], 0.20f, 1e-6f);
    ASSERT_NEAR(uv[1], 0.25f, 1e-6f);  // walked inward (up) from 0.2
    ASSERT_NEAR(uv[2], 0.75f, 1e-6f);  // walked inward (down) from 0.8
    ASSERT_NEAR(uv[3], 0.80f, 1e-6f);

    // Descending UV range (uv_hi < uv_lo): border must still walk inward -- down from
    // uv_lo, up from uv_hi -- not outward past [uv_hi, uv_lo].
    nine_slice_axis_breakpoints(0.0f, 100.0f, 10.0f, 10.0f, 0.8f, 0.2f, 0.05f, 0.05f, pos, uv);
    ASSERT_NEAR(uv[0], 0.80f, 1e-6f);
    ASSERT_NEAR(uv[1], 0.75f, 1e-6f);  // walked inward (down) from 0.8
    ASSERT_NEAR(uv[2], 0.25f, 1e-6f);  // walked inward (up) from 0.2
    ASSERT_NEAR(uv[3], 0.20f, 1e-6f);
    // Every inner breakpoint stays within [min(uv_lo,uv_hi), max(uv_lo,uv_hi)].
    ASSERT_TRUE(uv[1] <= 0.80f && uv[1] >= 0.20f);
    ASSERT_TRUE(uv[2] <= 0.80f && uv[2] >= 0.20f);

    // Oversized border clamps both inner breakpoints to the midpoint rather than crossing.
    nine_slice_axis_breakpoints(0.0f, 10.0f, 8.0f, 8.0f, 0.0f, 1.0f, 0.1f, 0.1f, pos, uv);
    ASSERT_NEAR(pos[1], 5.0f, 1e-6f);
    ASSERT_NEAR(pos[2], 5.0f, 1e-6f);
}
