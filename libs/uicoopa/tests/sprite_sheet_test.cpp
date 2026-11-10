/**
 * @file sprite_sheet_test.cpp
 * @brief Sprite sheets: descriptor YAML parsing and its errors, the pixel-rect -> UV Y-flip
 *        convention, build_sprite_table(), and the integrity of the shipped icon/cursor/prompt
 *        sheet descriptors (in bounds, non-overlapping, and carrying the names code looks up).
 *
 * Exact sheet dimensions and sprite counts are deliberately not asserted -- regenerating a sheet
 * with tools/gen_*.py may change them without breaking anything.
 */

#include <coopa/testing/test.h>

#include "support/ui_test_support.h"
#include <fstream>
#include <sstream>

COOPA_TEST_SUITE("sprite_sheet");

COOPA_TEST(descriptor_parses_and_rejects_missing_image_or_duplicates) {
    std::string yaml =
        "image: icons.png\n"
        "width: 64\n"
        "height: 32\n"
        "sprites:\n"
        "  - { name: a, x: 0, y: 0, w: 32, h: 32 }\n"
        "  - { name: b, x: 32, y: 0, w: 32, h: 32,\n"
        "      border: { left: 4, bottom: 4, right: 4, top: 4 } }\n";
    SpriteSheetDesc desc = parse_sprite_sheet_desc(yaml);
    ASSERT_TRUE(desc.image == "icons.png");
    ASSERT_TRUE(desc.width == 64 && desc.height == 32);
    ASSERT_TRUE(desc.sprites.size() == 2);
    ASSERT_TRUE(desc.sprites[0].name == "a" && desc.sprites[0].w == 32 && desc.sprites[0].h == 32);
    ASSERT_TRUE(desc.sprites[1].name == "b" && desc.sprites[1].x == 32);
    ASSERT_VEC_NEAR(glm::vec2(desc.sprites[1].border.x, desc.sprites[1].border.y), glm::vec2(4.0f, 4.0f), 1e-6f);
    ASSERT_TRUE(desc.sprites[0].border == glm::vec4(0.0f)); // no border block -> defaults to zero

    // Missing 'image' key throws.
    bool threw = false;
    try { parse_sprite_sheet_desc("sprites:\n  - { name: a, x: 0, y: 0, w: 1, h: 1 }\n"); }
    catch (const std::runtime_error&) { threw = true; }
    ASSERT_TRUE(threw);

    // Duplicate sprite name throws.
    threw = false;
    try {
        parse_sprite_sheet_desc(
            "image: x.png\nsprites:\n"
            "  - { name: a, x: 0, y: 0, w: 1, h: 1 }\n"
            "  - { name: a, x: 1, y: 1, w: 1, h: 1 }\n");
    } catch (const std::runtime_error&) { threw = true; }
    ASSERT_TRUE(threw);
}

COOPA_TEST(pixel_rect_to_uv_flips_y_and_insets_half_texel) {
    // Full-texture rect, no inset: exactly {{0,1},{1,0}} -- the FontAtlas/SpriteSheet
    // Y-flip convention (uv.min pairs with pos.min, canvas-bottom, i.e. the image's
    // BOTTOM row, which is the LARGER v coordinate since image space is +Y down).
    Rect full = pixel_rect_to_uv(0, 0, 256, 256, 256, 256);
    ASSERT_VEC_NEAR(full.min, glm::vec2(0.0f, 1.0f), 1e-6f);
    ASSERT_VEC_NEAR(full.max, glm::vec2(1.0f, 0.0f), 1e-6f);
    ASSERT_TRUE(full.min.y > full.max.y);

    // One 32px cell in a 256x256 sheet at (0,0): min.y (bottom edge) > max.y (top edge).
    Rect cell = pixel_rect_to_uv(0, 0, 32, 32, 256, 256);
    ASSERT_VEC_NEAR(cell.min, glm::vec2(0.0f, 0.125f), 1e-6f);
    ASSERT_VEC_NEAR(cell.max, glm::vec2(0.125f, 0.0f), 1e-6f);

    // Half-texel inset shrinks every edge toward the rect's interior.
    Rect inset = pixel_rect_to_uv(0, 0, 32, 32, 256, 256, /*half_texel_inset=*/true);
    float half_texel = 0.5f / 256.0f;
    ASSERT_NEAR(inset.min.x, 0.0f + half_texel, 1e-6f);
    ASSERT_NEAR(inset.max.x, 0.125f - half_texel, 1e-6f);
    ASSERT_NEAR(inset.min.y, 0.125f - half_texel, 1e-6f); // min.y shrinks DOWN (toward max.y)
    ASSERT_NEAR(inset.max.y, 0.0f + half_texel, 1e-6f);   // max.y shrinks UP (toward min.y)
}

COOPA_TEST(build_sprite_table_without_texture_is_stable) {
    SpriteSheetDesc desc = parse_sprite_sheet_desc(
        "image: icons.png\nwidth: 64\nheight: 32\nsprites:\n"
        "  - { name: a, x: 0, y: 0, w: 32, h: 32 }\n"
        "  - { name: b, x: 32, y: 0, w: 32, h: 32 }\n");
    auto table = build_sprite_table(desc, /*texture=*/nullptr, 64, 32);
    ASSERT_TRUE(table.size() == 2);
    ASSERT_TRUE(table.count("a") == 1 && table.count("b") == 1);
    ASSERT_TRUE(table.at("a").texture == nullptr);
    ASSERT_VEC_NEAR(table.at("a").uv.min, glm::vec2(0.0f, 1.0f), 1e-6f);

    // Pointer stability: taking an address before more lookups stays valid (unordered_map
    // node-based storage never invalidates references on further lookups/inserts).
    const Sprite* a_ptr = &table.at("a");
    (void)table.at("b");
    ASSERT_TRUE(a_ptr == &table.at("a"));

    // whole_image_desc() -- the "bare PNG, no sidecar YAML" case.
    SpriteSheetDesc whole = whole_image_desc("logo", 128, 64);
    ASSERT_TRUE(whole.sprites.size() == 1 && whole.sprites[0].name == "logo");
    auto whole_table = build_sprite_table(whole, nullptr, 128, 64);
    ASSERT_VEC_NEAR(whole_table.at("logo").uv.min, glm::vec2(0.0f, 1.0f), 1e-6f);
    ASSERT_VEC_NEAR(whole_table.at("logo").uv.max, glm::vec2(1.0f, 0.0f), 1e-6f);
}

/** @brief The three shipped sheets (icons/cursors/prompts) parse, every cell lies inside its
 *         image, no two cells overlap, and every name the builder/cursor/prompt code asks for
 *         exists. */
COOPA_TEST(shipped_sheet_descriptors_are_in_bounds_and_disjoint) {
    {
        std::string path = std::string(ROOT_DIR) + "/assets/icons/icons.yaml";
        std::ifstream f(path);
        ASSERT_TRUE(static_cast<bool>(f));
        std::stringstream buf;
        buf << f.rdbuf();

        SpriteSheetDesc desc = parse_sprite_sheet_desc(buf.str());
        ASSERT_TRUE(desc.image == "icons.png");
        ASSERT_TRUE(desc.width > 0 && desc.height > 0);

        bool has_arrow_left = false, has_check = false, has_plus = false, has_minus = false,
             has_star = false, has_gear = false, has_chevron_down = false;
        for (const auto& e : desc.sprites) {
            ASSERT_TRUE(e.x + e.w <= desc.width);
            ASSERT_TRUE(e.y + e.h <= desc.height);
            if (e.name == "arrow_left")    has_arrow_left = true;
            if (e.name == "check")         has_check = true;
            if (e.name == "plus")          has_plus = true;
            if (e.name == "minus")         has_minus = true;
            if (e.name == "star")          has_star = true;
            if (e.name == "gear")          has_gear = true;
            if (e.name == "chevron_down")  has_chevron_down = true;
        }
        ASSERT_TRUE(has_arrow_left && has_check && has_plus && has_minus &&
                   has_star && has_gear && has_chevron_down);

        // No two cells overlap (they're laid out on a grid, but this holds regardless of layout).
        for (size_t i = 0; i < desc.sprites.size(); ++i) {
            for (size_t j = i + 1; j < desc.sprites.size(); ++j) {
                const auto& a = desc.sprites[i];
                const auto& b = desc.sprites[j];
                bool disjoint = a.x + a.w <= b.x || b.x + b.w <= a.x ||
                               a.y + a.h <= b.y || b.y + b.h <= a.y;
                ASSERT_TRUE(disjoint);
            }
        }
    }

    /** @brief Sibling of the icon-sheet block above for the separate
     *         cursor sheet (assets/icons/cursors.png, generated by
     *         tools/gen_default_cursors.py) -- kept as its own file/descriptor so cursor
     *         changes never touch icons.png/icons.yaml's own byte-for-byte reproducibility. */
    {
        std::string path = std::string(ROOT_DIR) + "/assets/icons/cursors.yaml";
        std::ifstream f(path);
        ASSERT_TRUE(static_cast<bool>(f));
        std::stringstream buf;
        buf << f.rdbuf();

        SpriteSheetDesc desc = parse_sprite_sheet_desc(buf.str());
        ASSERT_TRUE(desc.image == "cursors.png");
        ASSERT_TRUE(desc.width > 0 && desc.height > 0);

        bool has_default = false, has_pointer = false, has_text = false, has_disabled = false;
        for (const auto& e : desc.sprites) {
            ASSERT_TRUE(e.x + e.w <= desc.width);
            ASSERT_TRUE(e.y + e.h <= desc.height);
            if (e.name == "cursor_default")  has_default = true;
            if (e.name == "cursor_pointer")  has_pointer = true;
            if (e.name == "cursor_text")     has_text = true;
            if (e.name == "cursor_disabled") has_disabled = true;
        }
        ASSERT_TRUE(has_default && has_pointer && has_text && has_disabled);

        for (size_t i = 0; i < desc.sprites.size(); ++i) {
            for (size_t j = i + 1; j < desc.sprites.size(); ++j) {
                const auto& a = desc.sprites[i];
                const auto& b = desc.sprites[j];
                bool disjoint = a.x + a.w <= b.x || b.x + b.w <= a.x ||
                               a.y + a.h <= b.y || b.y + b.h <= a.y;
                ASSERT_TRUE(disjoint);
            }
        }
    }

    /** @brief Sibling of the cursor-sheet block above for the gamepad
     *         button-prompt sheet (assets/icons/prompts.png, generated by
     *         tools/gen_button_prompts.py) -- its own file/descriptor so prompt-glyph
     *         changes never touch icons.png/cursors.png's own reproducibility. */
    {
        std::string path = std::string(ROOT_DIR) + "/assets/icons/prompts.yaml";
        std::ifstream f(path);
        ASSERT_TRUE(static_cast<bool>(f));
        std::stringstream buf;
        buf << f.rdbuf();

        SpriteSheetDesc desc = parse_sprite_sheet_desc(buf.str());
        ASSERT_TRUE(desc.image == "prompts.png");
        ASSERT_TRUE(desc.width > 0 && desc.height > 0);

        static const std::vector<std::string> kExpectedNames = {
            "prompt_a", "prompt_b", "prompt_x", "prompt_y", "prompt_l", "prompt_r",
            "prompt_dpad", "prompt_dpad_h", "prompt_dpad_v", "prompt_start", "prompt_select", "prompt_stick",
        };
        for (const auto& expected : kExpectedNames) {
            bool found = false;
            for (const auto& e : desc.sprites) if (e.name == expected) { found = true; break; }
            ASSERT_TRUE(found);
        }

        for (const auto& e : desc.sprites) {
            ASSERT_TRUE(e.x + e.w <= desc.width);
            ASSERT_TRUE(e.y + e.h <= desc.height);
        }
        for (size_t i = 0; i < desc.sprites.size(); ++i) {
            for (size_t j = i + 1; j < desc.sprites.size(); ++j) {
                const auto& a = desc.sprites[i];
                const auto& b = desc.sprites[j];
                bool disjoint = a.x + a.w <= b.x || b.x + b.w <= a.x ||
                               a.y + a.h <= b.y || b.y + b.h <= a.y;
                ASSERT_TRUE(disjoint);
            }
        }
    }
}
