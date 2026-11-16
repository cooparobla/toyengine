/**
 * @file ui_pass_test.cpp
 * @brief UiPass's pure per-frame math: the gl_FragCoord -> backdrop-UV remap that
 *        ui_glass.frag mirrors.
 *
 * Headless -- no device, no shader, no DrawList. Just the arithmetic ui_glass.frag's first
 * two lines duplicate; a divergence between this and the GLSL cannot be caught by any test,
 * which is exactly why this function exists as a named, tested thing rather than being
 * folded inline wherever a backdrop rect is set.
 */

#include <coopa/testing/test.h>

#include <uicoopa/render/ui_pass.h>

COOPA_TEST_SUITE("ui_pass");

namespace {
using coopa::ui::UiPass;
}  // namespace

COOPA_TEST(backdrop_uv_maps_the_rects_corners_to_0_1) {
    const UiPass::BackdropRemap r = UiPass::backdrop_remap({40.0f, 0.0f}, {1840.0f, 1080.0f});
    ASSERT_VEC_NEAR(UiPass::backdrop_uv({40.0f, 0.0f}, r), glm::vec2(0.0f, 0.0f), 1e-4f);
    ASSERT_VEC_NEAR(UiPass::backdrop_uv({1880.0f, 1080.0f}, r), glm::vec2(1.0f, 1.0f), 1e-4f);
    ASSERT_VEC_NEAR(UiPass::backdrop_uv({960.0f, 540.0f}, r), glm::vec2(0.5f, 0.5f), 1e-4f);
}

COOPA_TEST(backdrop_uv_clamps_outside_the_rect) {
    const UiPass::BackdropRemap r = UiPass::backdrop_remap({40.0f, 0.0f}, {1840.0f, 1080.0f});
    ASSERT_VEC_NEAR(UiPass::backdrop_uv({0.0f, -50.0f}, r), glm::vec2(0.0f, 0.0f), 1e-4f);
    ASSERT_VEC_NEAR(UiPass::backdrop_uv({5000.0f, 5000.0f}, r), glm::vec2(1.0f, 1.0f), 1e-4f);
}

COOPA_TEST(backdrop_remap_of_a_zero_size_rect_has_no_nan) {
    const UiPass::BackdropRemap r = UiPass::backdrop_remap({0.0f, 0.0f}, {0.0f, 0.0f});
    ASSERT_TRUE(r.inv_size[0] == 0.0f && r.inv_size[1] == 0.0f);
    const glm::vec2 uv = UiPass::backdrop_uv({123.0f, 456.0f}, r);
    ASSERT_VEC_NEAR(uv, glm::vec2(0.0f, 0.0f), 1e-4f);
}

COOPA_TEST(backdrop_remap_full_window_is_identity_on_the_corners) {
    const UiPass::BackdropRemap r = UiPass::backdrop_remap({0.0f, 0.0f}, {1920.0f, 1080.0f});
    ASSERT_VEC_NEAR(UiPass::backdrop_uv({0.0f, 0.0f}, r), glm::vec2(0.0f, 0.0f), 1e-4f);
    ASSERT_VEC_NEAR(UiPass::backdrop_uv({1920.0f, 1080.0f}, r), glm::vec2(1.0f, 1.0f), 1e-4f);
}
