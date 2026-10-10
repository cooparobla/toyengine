/**
 * @file sdf_clip_rect_test.cpp
 * @brief The SDF pass's screen-space clip rect (pixel_math.h): projected bounds for visible
 *        boxes, culling of off-screen ones, the full-screen fallback when a box straddles the
 *        near plane, and the NDC -> pixel mapping with its Y flip.
 */

#include <coopa/testing/test.h>

#include <glm/gtc/matrix_transform.hpp>
#include <toyengine/render/pixel_math.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("sdf_clip_rect");

COOPA_TEST(clip_rect_bounds_visible_boxes_and_culls_the_rest) {
    {
        // Camera at the origin looking down -Z (identity view); box 5 units in front,
        // well within a 45-degree-FOV frustum at that distance (tan(22.5deg)*5 ~= 2.07).
        glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
        glm::mat4 view_proj = proj; // view = identity
        auto rect = toy::render::compute_sdf_clip_rect(view_proj, glm::vec3(-1, -1, -6), glm::vec3(1, 1, -4));
        expect(rect.visible, "sdf_clip_rect: an on-screen box is visible");
        expect(rect.ndc_min.x > -1.0f && rect.ndc_max.x < 1.0f &&
              rect.ndc_min.y > -1.0f && rect.ndc_max.y < 1.0f,
              "sdf_clip_rect: an on-screen box's rect stays strictly inside NDC bounds");
    }
    {
        glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
        // Small box, far off to the side of a 5-unit-distant frustum slice -- outside the view cone.
        auto rect = toy::render::compute_sdf_clip_rect(proj, glm::vec3(49.9f, -0.1f, -5.1f), glm::vec3(50.1f, 0.1f, -4.9f));
        expect(!rect.visible, "sdf_clip_rect: a box entirely outside the frustum is culled");
    }
    {
        // Camera-space box straddling z=0 (some corners behind the camera, w <= 0) must fall back
        // to the full-screen rect rather than compute a partial (and potentially wrong) bound.
        glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
        auto rect = toy::render::compute_sdf_clip_rect(proj, glm::vec3(-1, -1, -1), glm::vec3(1, 1, 1));
        expect(rect.visible, "sdf_clip_rect: a near-plane-straddling box stays visible (full-screen fallback)");
        expect(rect.ndc_min == glm::vec2(-1.0f) && rect.ndc_max == glm::vec2(1.0f),
              "sdf_clip_rect: a near-plane-straddling box falls back to the full [-1,1] rect");
    }
}

COOPA_TEST(clip_rect_maps_to_pixels_with_the_y_flip) {
    {
        auto px = toy::render::sdf_clip_rect_to_pixels(glm::vec2(-1.0f), glm::vec2(1.0f), 160, 90);
        expect(px.x == 0 && px.y == 0 && px.w == 160 && px.h == 90,
              "sdf_clip_rect_to_pixels: full NDC rect covers the whole render target");
    }
    {
        // NDC y in [0, 1] is "up" (this engine's OpenGL-style convention, see
        // CameraComponent::get_projection_matrix()'s doc) -- that must map to the TOP half of the
        // framebuffer (Vulkan pixel space, y = 0 at the top), i.e. the same flip the negative-height
        // viewport applies to rasterized geometry (see sdf_clip_rect_to_pixels()'s own doc).
        auto px = toy::render::sdf_clip_rect_to_pixels(glm::vec2(-1.0f, 0.0f), glm::vec2(0.0f, 1.0f), 160, 90);
        expect(px.x == 0 && px.y == 0 && px.w == 80 && px.h == 45,
              "sdf_clip_rect_to_pixels: NDC top-left quadrant maps to the pixel top-left quadrant");
    }
}
