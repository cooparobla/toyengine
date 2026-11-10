/**
 * @file text_layout_test.cpp
 * @brief layout_text(): word wrap, explicit newlines, character-level breaks for over-long words,
 *        and the exact linear scaling Text::emit()'s supersampling relies on.
 *
 * Uses a synthetic advance function, so no FontAtlas/device is needed. Glyph rasterisation is not
 * covered here.
 */

#include <coopa/testing/test.h>

#include "support/ui_test_support.h"

COOPA_TEST_SUITE("text_layout");

COOPA_TEST(wraps_words_and_breaks_overlong_words) {
    // Synthetic advance function: every char is 10 units, space is 5. Lets the word-wrap
    // algorithm (layout_text) be exercised without a real baked FontAtlas/Vulkan device.
    auto advance = [](uint32_t cp) -> float { return cp == ' ' ? 5.0f : 10.0f; };

    // "hello " (5*10 + 5 = 55) fits in 60; adding 'w' (65) doesn't -> wraps before "world".
    {
        TextLayout tl = layout_text("hello world", 60.0f, advance);
        ASSERT_TRUE(tl.line_widths.size() == 2);
        ASSERT_NEAR(tl.line_widths[0], 55.0f, 1e-3f);
        ASSERT_NEAR(tl.line_widths[1], 50.0f, 1e-3f);
        ASSERT_TRUE(tl.glyphs.front().line == 0);
        ASSERT_TRUE(tl.glyphs.back().line == 1);
    }
    // Explicit '\n' always breaks, regardless of width.
    {
        TextLayout tl = layout_text("ab\ncd", -1.0f, advance);
        ASSERT_TRUE(tl.line_widths.size() == 2);
        ASSERT_NEAR(tl.line_widths[0], 20.0f, 1e-3f);
        ASSERT_NEAR(tl.line_widths[1], 20.0f, 1e-3f);
    }
    // wrap_width <= 0 disables wrapping entirely.
    {
        TextLayout tl = layout_text("hello world", -1.0f, advance);
        ASSERT_TRUE(tl.line_widths.size() == 1);
        ASSERT_NEAR(tl.line_widths[0], 105.0f, 1e-3f);
    }
    // A single word longer than wrap_width falls back to repeated character-level breaks
    // without corrupting already-placed glyphs (regression test for the word_start_pen_x
    // staleness bug: each line must be non-decreasing in glyph index and non-negative pen_x).
    {
        TextLayout tl = layout_text("x abcdefgh", 25.0f, advance);
        ASSERT_TRUE(tl.line_widths.size() == 5);
        uint32_t last_line = 0;
        for (const auto& g : tl.glyphs) {
            ASSERT_TRUE(g.line >= last_line);
            ASSERT_TRUE(g.pen_x >= 0.0f);
            last_line = g.line;
        }
        for (float w : tl.line_widths) {
            ASSERT_TRUE(w <= 25.0f + 1e-3f);
        }
    }
}

// The assumption Text::emit()'s supersampling rests on: laying out at N times the size with N
// times the wrap width produces N times the layout, exactly. That is what lets a glyph atlas be
// baked at the size it will be DRAWN at while the emitted canvas-space layout stays the authored
// one -- emit() multiplies every atlas-derived quantity by font_size/baked_px to get back.
//
// It holds because stb's advances and vertical metrics are `stbtt_ScaleForPixelHeight(f, h) *
// font_units` with scale = h / (ascent - descent), i.e. exactly linear in bake size. The synthetic
// advance function below is linear in the same way, so this pins the wrap/pen arithmetic in
// layout_text() without needing a device to bake a real atlas.
COOPA_TEST(layout_scales_linearly_with_font_size) {
    auto advance_at = [](float size) {
        return [size](uint32_t cp) -> float { return (cp == ' ' ? 5.0f : 10.0f) * size; };
    };

    const float kScale = 3.0f;
    const char* kText = "the quick brown fox jumps over the lazy dog";

    for (float wrap : {60.0f, 95.0f, 140.0f, -1.0f}) {
        TextLayout base   = layout_text(kText, wrap, advance_at(1.0f));
        TextLayout scaled = layout_text(kText, wrap > 0.0f ? wrap * kScale : -1.0f, advance_at(kScale));

        ASSERT_TRUE(base.glyphs.size() == scaled.glyphs.size());
        ASSERT_TRUE(base.line_widths.size() == scaled.line_widths.size());

        for (size_t i = 0; i < base.line_widths.size(); ++i) {
            ASSERT_NEAR(scaled.line_widths[i] / kScale, base.line_widths[i], 1e-3f);
        }
        for (size_t i = 0; i < base.glyphs.size(); ++i) {
            // Same break decisions...
            ASSERT_TRUE(base.glyphs[i].line == scaled.glyphs[i].line);
            ASSERT_TRUE(base.glyphs[i].codepoint == scaled.glyphs[i].codepoint);
            // ...and the same pen positions once divided back down, which is precisely the
            // `* inv` Text::emit() applies.
            ASSERT_NEAR(scaled.glyphs[i].pen_x / kScale, base.glyphs[i].pen_x, 1e-3f);
        }
    }
}
