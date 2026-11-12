/**
 * @file toon_material_test.cpp
 * @brief PBRMaterial's toon_* keys end to end on the CPU side: the YAML names parse, and
 *        gpu_toon_code() packs them into the integer the G-buffer's emissive alpha carries,
 *        decoding back to the settings the material asked for.
 *
 * No device needed. The decoder here is a literal transcription of gfx/toon.glsl's
 * gfx_toon_unpack(), which is the point: the packing and the unpacking live on opposite sides
 * of a G-buffer channel, and nothing else in the build would notice them drifting apart.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <string>

#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/components/register.h>

COOPA_TEST_SUITE("toon_material");

using coopa::gfx::engine::components::PBRMaterial;
using coopa::gfx::engine::components::ToonSpecular;

namespace {

/// gfx/toon.glsl's gfx_toon_unpack(), transcribed -- see this file's doc.
struct Decoded {
    float bands    = 0.0f;
    int   spec     = 0;
    float rim      = 0.0f;
    float softness = 0.0f;
    bool  outline  = false;
};

Decoded decode(uint32_t code) {
    static const float kSoftness[4] = {0.0f, 0.15f, 0.35f, 0.70f};
    const uint32_t bands_sel = code & 7u;
    Decoded d;
    d.bands    = bands_sel == 0u ? 0.0f : static_cast<float>(bands_sel + 1u);
    d.spec     = static_cast<int>((code >> 3) & 3u);
    d.rim      = static_cast<float>((code >> 5) & 7u) * 0.25f;
    d.softness = kSoftness[(code >> 8) & 3u];
    d.outline  = ((code >> 10) & 1u) != 0u;
    return d;
}

/// A toon code only survives the trip if the half-float G-buffer channel stores it exactly.
/// A half has 11 bits of integer precision, so this is the budget the bit layout has to fit in.
bool exact_in_half(uint32_t code) { return code <= 2048u; }

}  // namespace

/**
 * @brief An untouched material packs to exactly 0, and 0 is what the shader tests to skip the
 *        toon path. Every material in every scene that predates toon shading relies on this.
 */
COOPA_TEST(a_plain_material_packs_to_zero) {
    PBRMaterial m;
    EXPECT_FALSE(m.is_toon());
    EXPECT_EQ(m.gpu_toon_code(), 0u);

    // toon_bands 1 is "one band", i.e. no banding at all -- same as 0, and the other toon_*
    // fields must not leak into the code through it.
    m.toon_bands    = 1;
    m.toon_specular = ToonSpecular::Hard;
    m.toon_rim      = 1.0f;
    m.toon_softness = 0.35f;
    EXPECT_FALSE(m.is_toon());
    EXPECT_EQ(m.gpu_toon_code(), 0u);
}

/** @brief Every authored setting round-trips through the pack/unpack pair. */
COOPA_TEST(toon_settings_round_trip_through_the_packed_code) {
    PBRMaterial m;
    m.toon_bands    = 4;
    m.toon_specular = ToonSpecular::TwoStep;
    m.toon_rim      = 0.75f;
    m.toon_softness = 0.35f;
    m.toon_outline  = true;
    EXPECT_TRUE(m.is_toon());

    const uint32_t code = m.gpu_toon_code();
    EXPECT_TRUE(exact_in_half(code));

    const Decoded d = decode(code);
    EXPECT_EQ(d.bands, 4.0f);
    EXPECT_EQ(d.spec, static_cast<int>(ToonSpecular::TwoStep));
    EXPECT_EQ(d.rim, 0.75f);
    EXPECT_EQ(d.softness, 0.35f);
    EXPECT_TRUE(d.outline);

    // Every band count in the authored range survives, including both ends.
    for (int bands = 2; bands <= 8; ++bands) {
        PBRMaterial b;
        b.toon_bands = bands;
        EXPECT_EQ(decode(b.gpu_toon_code()).bands, static_cast<float>(bands));
    }
    // ...and every specular mode lands on its own value rather than aliasing a neighbour.
    for (int s = 0; s <= 3; ++s) {
        PBRMaterial b;
        b.toon_bands    = 3;
        b.toon_specular = static_cast<ToonSpecular>(s);
        EXPECT_EQ(decode(b.gpu_toon_code()).spec, s);
    }
}

/**
 * @brief The outline flag is independent of banding -- a plain PBR material can carry an ink
 *        line without its shading changing, which is what `outline_mode: toon` is for.
 */
COOPA_TEST(outline_is_independent_of_banding) {
    PBRMaterial m;
    m.toon_outline = true;
    EXPECT_FALSE(m.is_toon());

    const Decoded d = decode(m.gpu_toon_code());
    EXPECT_TRUE(d.outline);
    EXPECT_EQ(d.bands, 0.0f);     // still shaded as PBR
    EXPECT_NE(m.gpu_toon_code(), 0u);
}

/**
 * @brief Out-of-range authored values clamp instead of wrapping into a neighbouring field.
 *        A rim of 40 overflowing its three bits would silently reassign the softness and
 *        outline bits above it, so this is the property that makes the layout safe.
 */
COOPA_TEST(out_of_range_values_clamp_rather_than_overflow) {
    PBRMaterial hot;
    hot.toon_bands    = 99;
    hot.toon_specular = ToonSpecular::Hard;
    hot.toon_rim      = 40.0f;
    hot.toon_softness = 12.0f;
    const uint32_t code = hot.gpu_toon_code();
    EXPECT_TRUE(exact_in_half(code));

    const Decoded d = decode(code);
    EXPECT_EQ(d.bands, 8.0f);                                  // clamped to the top band count
    EXPECT_EQ(d.spec, static_cast<int>(ToonSpecular::Hard));   // NOT clobbered by the rim
    EXPECT_EQ(d.rim, 1.75f);                                   // clamped to 7 * 0.25
    EXPECT_EQ(d.softness, 0.70f);                              // clamped to the last preset
    EXPECT_FALSE(d.outline);                                   // and nothing spilled into bit 10

    PBRMaterial cold;
    cold.toon_bands    = -5;
    cold.toon_rim      = -3.0f;
    cold.toon_softness = -1.0f;
    EXPECT_EQ(cold.gpu_toon_code(), 0u);

    // The widest possible code still fits the half-float channel exactly.
    PBRMaterial widest;
    widest.toon_bands    = 8;
    widest.toon_specular = ToonSpecular::Off;
    widest.toon_rim      = 1.75f;
    widest.toon_softness = 0.70f;
    widest.toon_outline  = true;
    EXPECT_TRUE(exact_in_half(widest.gpu_toon_code()));
}

/** @brief toon_softness snaps to the nearest of the four presets the shader can decode. */
COOPA_TEST(softness_snaps_to_the_nearest_preset) {
    const float cases[][2] = {{0.0f, 0.0f},   {0.06f, 0.0f},  {0.09f, 0.15f}, {0.15f, 0.15f},
                              {0.22f, 0.15f}, {0.30f, 0.35f}, {0.35f, 0.35f}, {0.60f, 0.70f},
                              {1.0f, 0.70f}};
    for (const auto& c : cases) {
        PBRMaterial m;
        m.toon_bands    = 4;
        m.toon_softness = c[0];
        EXPECT_EQ(decode(m.gpu_toon_code()).softness, c[1]);
    }
}

/** @brief The YAML spellings a scene author writes map to the modes they name. */
COOPA_TEST(toon_specular_names_parse_to_their_modes) {
    using coopa::gfx::engine::components::parse_toon_specular_;
    EXPECT_TRUE(parse_toon_specular_("hard") == ToonSpecular::Hard);
    EXPECT_TRUE(parse_toon_specular_("twostep") == ToonSpecular::TwoStep);
    EXPECT_TRUE(parse_toon_specular_("two_step") == ToonSpecular::TwoStep);
    EXPECT_TRUE(parse_toon_specular_("off") == ToonSpecular::Off);
    EXPECT_TRUE(parse_toon_specular_("none") == ToonSpecular::Off);
    EXPECT_TRUE(parse_toon_specular_("pbr") == ToonSpecular::Pbr);
    // Unrecognised falls back to the PBR highlight, matching parse_alpha_mode_'s policy of
    // degrading to the default rather than failing the scene load.
    EXPECT_TRUE(parse_toon_specular_("") == ToonSpecular::Pbr);
    EXPECT_TRUE(parse_toon_specular_("Hard") == ToonSpecular::Pbr);  // case-sensitive by design
}
