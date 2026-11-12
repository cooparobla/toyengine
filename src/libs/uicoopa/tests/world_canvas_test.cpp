/**
 * @file world_canvas_test.cpp
 * @brief World-space canvases (CanvasRenderMode::WorldSpace): a root rect that ignores the screen,
 *        the canvas-pixels-to-world model matrix for both billboard modes, the ray/plane inverse
 *        pointer picking depends on, and the UiInput::update_at() seam it feeds.
 *
 * All pure math -- no device, no window. Rendering through UiWorldPass is not covered here.
 */

#include <coopa/testing/test.h>

#include "support/ui_test_support.h"

COOPA_TEST_SUITE("world_canvas");

/** @brief The negative-height viewport (0, h, w, -h) UiWorldPass draws through: framebuffer
 *         row 0 is ndc_y = +1, the OPPOSITE sign of the usual Vulkan relation. Both helpers
 *         below deliberately spell it out rather than sharing engine code, so a silent flip
 *         in the real convention shows up here as a failure. */
static glm::vec2 wc_ndc_to_fb(glm::vec2 ndc, float w, float h) {
    return { (ndc.x * 0.5f + 0.5f) * w, (0.5f - ndc.y * 0.5f) * h };
}
static glm::vec2 wc_fb_to_ndc(glm::vec2 fb, float w, float h) {
    return { 2.0f * fb.x / w - 1.0f, 1.0f - 2.0f * fb.y / h };
}

/** @brief A Z-up scene viewed from -Y, matching toyengine's world_canvas_test. */
static glm::mat4 wc_view() {
    return glm::lookAt(glm::vec3(0.0f, -6.0f, 1.5f), glm::vec3(0.0f, 0.0f, 0.6f),
                       glm::vec3(0.0f, 0.0f, 1.0f));
}
static glm::mat4 wc_proj(float w, float h) {
    return glm::perspective(glm::radians(50.0f), w / h, 0.1f, 100.0f);
}

/** @brief A world canvas at `pos` with the scene's own defaults; caller sets billboard. */
static CanvasComponent* wc_make(SceneObject& obj, glm::vec3 pos) {
    auto* tc = obj.add_component<coopa::scene::TransformComponent>();
    tc->transform().set_position(pos);
    auto* c = obj.add_component<CanvasComponent>();
    c->render_mode = CanvasRenderMode::WorldSpace;
    c->world_size = glm::vec2(140.0f, 34.0f);
    c->pixels_per_unit = 90.0f;
    return c;
}

COOPA_TEST(root_rect_ignores_screen_size) {
    SceneObject obj("WorldCanvas");
    CanvasComponent* c = wc_make(obj, glm::vec3(0.0f, 0.0f, 1.1f));

    // A screen-space canvas would resolve 1920x1080 here. A world one must not: its root rect
    // is the authored design size, and its scale factor is fixed at 1.
    c->rebuild_layout(1920, 1080);
    ASSERT_VEC_NEAR(c->root_rect().size(), glm::vec2(140.0f, 34.0f), 1e-5f);
    ASSERT_NEAR(c->scale_factor(), 1.0f, 1e-6f);

    // set_viewport() must take the same branch. If only rebuild_layout() did,
    // late_update()'s unconditional rebuild would clobber it back every frame.
    c->set_viewport(640, 360);
    ASSERT_VEC_NEAR(c->root_rect().size(), glm::vec2(140.0f, 34.0f), 1e-5f);

    // And with no viewport ever set at all -- the realistic case, since a world canvas has no
    // reason to receive one. CanvasScaler's default ConstantPixelSize mode would turn the
    // 0x0 default into an EMPTY root rect, from which nothing draws.
    SceneObject fresh("Fresh");
    CanvasComponent* f = wc_make(fresh, glm::vec3(0.0f));
    f->rebuild_layout(0, 0);
    ASSERT_VEC_NEAR(f->root_rect().size(), glm::vec2(140.0f, 34.0f), 1e-5f);
}

COOPA_TEST(model_centres_on_owner_and_scales_by_pixels_per_unit) {
    const glm::vec3 pos(0.0f, 0.0f, 1.1f);
    for (int mode = 0; mode < 2; ++mode) {
        SceneObject obj("WorldCanvas");
        CanvasComponent* c = wc_make(obj, pos);
        c->billboard = mode == 0 ? CanvasBillboard::CameraFacing : CanvasBillboard::Transform;
        c->update_world_transform(wc_view());

        // The pivot is the canvas centre, so a bar "above the cube" needs no offset maths.
        glm::vec3 centre = glm::vec3(c->model() * glm::vec4(70.0f, 17.0f, 0.0f, 1.0f));
        ASSERT_NEAR(glm::length(centre - pos), 0.0f, 1e-5f);

        // world_size is in canvas PIXELS; pixels_per_unit alone sets the world extent.
        glm::vec3 bl = glm::vec3(c->model() * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        glm::vec3 br = glm::vec3(c->model() * glm::vec4(140.0f, 0.0f, 0.0f, 1.0f));
        glm::vec3 tl = glm::vec3(c->model() * glm::vec4(0.0f, 34.0f, 0.0f, 1.0f));
        ASSERT_NEAR(glm::length(br - bl), 140.0f / 90.0f, 1e-5f);
        ASSERT_NEAR(glm::length(tl - bl), 34.0f / 90.0f, 1e-5f);
    }
}

COOPA_TEST(transform_mode_default_axes_face_minus_y) {
    // The Z-up defaults (canvas right = local +X, canvas up = local +Z) must put an unrotated
    // Transform-mode canvas's normal along world -Y, which is where a -Y camera sits.
    SceneObject obj("WallCanvas");
    CanvasComponent* c = wc_make(obj, glm::vec3(0.0f));
    c->billboard = CanvasBillboard::Transform;
    c->update_world_transform(wc_view());
    ASSERT_VEC_NEAR(glm::vec2(glm::normalize(c->world_normal())), glm::vec2(0.0f, -1.0f), 1e-5f);
    ASSERT_NEAR(glm::normalize(c->world_normal()).z, 0.0f, 1e-5f);

    // A Y-up host re-aims it with the two axis fields and no code change.
    c->local_up_axis = glm::vec3(0.0f, 1.0f, 0.0f);
    c->update_world_transform(wc_view());
    ASSERT_NEAR(glm::normalize(c->world_normal()).z, 1.0f, 1e-5f);
}

COOPA_TEST(camera_facing_quad_projects_axis_aligned) {
    // UiWorldPass turns a Mask's clip rect into an EXACT scissor by projecting its corners and
    // taking their AABB. That is only exact if a CameraFacing quad -- which sits at a constant
    // view depth -- projects to a screen-axis-aligned rectangle. ProgressBar::start() adds a
    // Mask unconditionally, so this premise is on the critical path, not a nicety.
    const float W = 480.0f, H = 270.0f;
    SceneObject obj("WorldCanvas");
    CanvasComponent* c = wc_make(obj, glm::vec3(0.3f, -0.4f, 1.1f));
    c->billboard = CanvasBillboard::CameraFacing;
    c->update_world_transform(wc_view());

    glm::mat4 clip = wc_proj(W, H) * wc_view() * c->model();
    auto fb = [&](glm::vec2 p) {
        glm::vec4 h = clip * glm::vec4(p, 0.0f, 1.0f);
        return wc_ndc_to_fb(glm::vec2(h) / h.w, W, H);
    };
    glm::vec2 p00 = fb({0.0f, 0.0f}), p10 = fb({140.0f, 0.0f});
    glm::vec2 p01 = fb({0.0f, 34.0f}), p11 = fb({140.0f, 34.0f});
    ASSERT_NEAR(p00.y, p10.y, 1e-3f);   // bottom edge horizontal
    ASSERT_NEAR(p01.y, p11.y, 1e-3f);   // top edge horizontal
    ASSERT_NEAR(p00.x, p01.x, 1e-3f);   // left edge vertical
    ASSERT_NEAR(p10.x, p11.x, 1e-3f);   // right edge vertical
}

COOPA_TEST(ray_to_canvas_round_trips_both_billboard_modes) {
    // The pointer-picking inverse: project a known canvas point to framebuffer pixels, rebuild
    // the ray the host would build from that pixel, and land back on the same canvas point.
    // Both billboard modes -- CameraFacing's projection happens to be affine, Transform's is
    // genuinely projective, and one ray/plane path has to serve both.
    const float W = 480.0f, H = 270.0f;
    const glm::mat4 view = wc_view();
    const glm::mat4 proj = wc_proj(W, H);
    const glm::mat4 inv_vp = glm::inverse(proj * view);

    for (int mode = 0; mode < 2; ++mode) {
        SceneObject obj("WorldCanvas");
        CanvasComponent* c = wc_make(obj, glm::vec3(0.0f, 0.0f, 1.1f));
        c->billboard = mode == 0 ? CanvasBillboard::CameraFacing : CanvasBillboard::Transform;
        c->update_world_transform(view);
        glm::mat4 clip = proj * view * c->model();

        const glm::vec2 probes[5] = {{0.0f, 0.0f}, {140.0f, 0.0f}, {140.0f, 34.0f},
                                     {0.0f, 34.0f}, {70.0f, 17.0f}};
        for (glm::vec2 want : probes) {
            glm::vec4 h = clip * glm::vec4(want, 0.0f, 1.0f);
            glm::vec2 ndc = wc_fb_to_ndc(wc_ndc_to_fb(glm::vec2(h) / h.w, W, H), W, H);
            glm::vec4 a4 = inv_vp * glm::vec4(ndc, 0.0f, 1.0f);
            glm::vec4 b4 = inv_vp * glm::vec4(ndc, 1.0f, 1.0f);
            glm::vec3 ro = glm::vec3(a4) / a4.w;
            glm::vec3 rd = glm::vec3(b4) / b4.w - ro;

            std::optional<glm::vec2> got = c->ray_to_canvas(ro, rd);
            ASSERT_TRUE(got.has_value());
            ASSERT_VEC_NEAR(*got, want, 0.01f);
        }
    }
}

COOPA_TEST(ray_misses_behind_or_edge_on_are_real_misses) {
    SceneObject obj("WorldCanvas");
    CanvasComponent* c = wc_make(obj, glm::vec3(0.0f, 0.0f, 1.1f));
    c->billboard = CanvasBillboard::CameraFacing;
    c->update_world_transform(wc_view());

    // Pointing away from the plane: the intersection is BEHIND the ray origin, which is a miss,
    // not a hit at negative t. A canvas that reported this as a hit would respond to a pointer
    // aimed in the opposite direction.
    ASSERT_TRUE(!c->ray_to_canvas(glm::vec3(0.0f, -6.0f, 1.5f), glm::vec3(0.0f, -1.0f, 0.0f)).has_value());

    // Edge-on: the denominator is ~0 and must not be divided by.
    glm::vec3 in_plane = glm::vec3(c->model()[0]);
    ASSERT_TRUE(!c->ray_to_canvas(glm::vec3(0.0f, 0.0f, 1.1f) - in_plane * 10.0f, in_plane).has_value());

    // A hit well outside the rect is still a HIT (the plane is infinite) -- rejecting it is
    // Raycaster's job, via root_rect(). This keeps the two responsibilities separate.
    std::optional<glm::vec2> far_hit =
        c->ray_to_canvas(glm::vec3(40.0f, -6.0f, 1.1f), glm::vec3(0.0f, 1.0f, 0.0f));
    ASSERT_TRUE(far_hit.has_value());
    ASSERT_TRUE(!contains(c->root_rect(), *far_hit));
}

COOPA_TEST(screen_space_canvases_are_unaffected) {
    // The whole feature has to be inert unless asked for.
    SceneObject obj("ScreenCanvas");
    auto* c = obj.add_component<CanvasComponent>();
    ASSERT_TRUE(c->render_mode == CanvasRenderMode::ScreenSpaceOverlay);
    ASSERT_TRUE(!c->is_world_space());
    c->set_viewport(640, 360);
    ASSERT_VEC_NEAR(c->root_rect().size(), glm::vec2(640.0f, 360.0f), 1e-5f);
    ASSERT_NEAR(c->scale_factor(), 1.0f, 1e-6f);
    // model() stays identity, and update_world_transform() is a no-op on a screen canvas.
    c->update_world_transform(wc_view());
    ASSERT_TRUE(c->model() == glm::mat4(1.0f));
}

COOPA_TEST(ui_input_update_at_sets_canvas_position_directly) {
    // update_at() is the seam world canvases feed their ray/plane result through; update()
    // is defined in terms of it, so this also covers the screen-space path's copying.
    coopa::input::Input input;
    UiInput ui;
    ui.update_at(input, glm::vec2(12.0f, 7.0f));
    ASSERT_VEC_NEAR(ui.position(), glm::vec2(12.0f, 7.0f), 1e-5f);
    ui.update_at(input, glm::vec2(20.0f, 7.0f));
    ASSERT_VEC_NEAR(ui.position(), glm::vec2(20.0f, 7.0f), 1e-5f);
    ASSERT_VEC_NEAR(ui.delta(), glm::vec2(8.0f, 0.0f), 1e-5f);
}
