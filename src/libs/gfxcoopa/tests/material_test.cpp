/**
 * @file material_test.cpp
 * @brief engine/components/register.h + mesh_renderer.h, no device needed: alpha_mode string
 *        parsing (CUTOUT/MASK/CLIP all mean alpha-tested) and PBRMaterial::gpu_alpha_cutoff(),
 *        the value both the G-buffer and shadow shaders read to arm their alpha-mask discard.
 */
#include <coopa/testing/test.h>

#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/components/register.h>

COOPA_TEST_SUITE("material");

using coopa::gfx::engine::components::AlphaMode;

COOPA_TEST(alpha_mode_names_parse_to_their_modes) {
    using coopa::gfx::engine::components::parse_alpha_mode_;
    EXPECT_TRUE(parse_alpha_mode_("BLEND") == AlphaMode::Blend);
    EXPECT_TRUE(parse_alpha_mode_("MASK") == AlphaMode::Mask);
    EXPECT_TRUE(parse_alpha_mode_("CLIP") == AlphaMode::Mask);
    // CUTOUT is the Unity-facing name for the same alpha-tested behaviour MASK/CLIP (the glTF
    // names) already select -- all three must collapse onto AlphaMode::Mask.
    EXPECT_TRUE(parse_alpha_mode_("CUTOUT") == AlphaMode::Mask);
    // Unrecognised (or misspelled) strings silently fall back to Opaque, matching this parser's
    // existing behaviour for every other unrecognised enum-like value.
    EXPECT_TRUE(parse_alpha_mode_("") == AlphaMode::Opaque);
    EXPECT_TRUE(parse_alpha_mode_("OPAQUE") == AlphaMode::Opaque);
    EXPECT_TRUE(parse_alpha_mode_("cutout") == AlphaMode::Opaque);  // case-sensitive by design
}

COOPA_TEST(gpu_alpha_cutoff_arms_only_for_masked_materials) {
    using coopa::gfx::engine::components::PBRMaterial;

    PBRMaterial opaque;
    opaque.alpha_mode = AlphaMode::Opaque;
    opaque.alpha_cutoff = 0.7f;
    EXPECT_EQ(opaque.gpu_alpha_cutoff(), 0.0f);  // OPAQUE never arms the discard

    PBRMaterial blended;
    blended.alpha_mode = AlphaMode::Blend;
    blended.alpha_cutoff = 0.7f;
    EXPECT_EQ(blended.gpu_alpha_cutoff(), 0.0f);  // BLEND never arms the discard either

    PBRMaterial masked;
    masked.alpha_mode = AlphaMode::Mask;
    masked.alpha_cutoff = 0.7f;
    EXPECT_EQ(masked.gpu_alpha_cutoff(), 0.7f);

    // Clamped to (0, 1] -- 0.0 would otherwise collide with the "disabled" sentinel itself.
    PBRMaterial masked_zero;
    masked_zero.alpha_mode = AlphaMode::Mask;
    masked_zero.alpha_cutoff = 0.0f;
    EXPECT_GT(masked_zero.gpu_alpha_cutoff(), 0.0f);

    // has_alpha_mask() requires BOTH AlphaMode::Mask AND a loaded texture -- an unset
    // alpha_mask_handle (the default for every material, masked or not) must read false, so
    // MaterialTextureCache::set_for() knows to bind the white fallback instead.
    EXPECT_FALSE(masked.has_alpha_mask());
    EXPECT_FALSE(opaque.has_alpha_mask());
}
