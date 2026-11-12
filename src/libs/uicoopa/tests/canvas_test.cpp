/**
 * @file canvas_test.cpp
 * @brief CanvasComponent: CanvasScaler scale factors, the measure/arrange pass driven by
 *        rebuild_layout() and by Scene::late_update(), and collect_canvases() sort order.
 *
 * Not covered here: world-space canvases (world_canvas_test.cpp).
 */

#include <coopa/testing/test.h>

#include "support/ui_test_support.h"

COOPA_TEST_SUITE("canvas");

COOPA_TEST(user_scale_and_host_ui_scale_both_shrink_the_canvas) {
    // The two multipliers a "make the interface bigger" setting is built from: one the asset
    // authors for itself, one the host drives for every canvas at once. Both multiply the
    // scale factor, and a bigger scale factor means FEWER canvas pixels across the same
    // screen -- which is what makes every authored widget cover more of it.
    CanvasScaler scaler;
    scaler.mode = ScaleMode::ScaleWithScreenSize;
    scaler.reference_resolution = { 1920.0f, 1080.0f };
    scaler.match_width_or_height = 0.0f;
    ASSERT_NEAR(scaler.compute_scale_factor(1920, 1080), 1.0f, 1e-4f);
    scaler.user_scale = 1.25f;
    ASSERT_NEAR(scaler.compute_scale_factor(1920, 1080), 1.25f, 1e-4f);
    // A nonsensical value is ignored rather than collapsing the canvas to nothing.
    scaler.user_scale = 0.0f;
    ASSERT_NEAR(scaler.compute_scale_factor(1920, 1080), 1.0f, 1e-4f);

    SceneObject canvas_obj("Canvas");
    canvas_obj.add_component<RectTransform>();
    auto* canvas = canvas_obj.add_component<CanvasComponent>();
    canvas->scaler.mode = ScaleMode::ConstantPixelSize;
    canvas->scaler.scale_factor = 1.0f;

    canvas->set_viewport(1920, 1080);
    ASSERT_NEAR(canvas->scale_factor(), 1.0f, 1e-4f);
    ASSERT_NEAR(canvas->root_rect().size().x, 1920.0f, 1e-3f);

    canvas->set_ui_scale(2.0f);
    canvas->set_viewport(1920, 1080);
    ASSERT_NEAR(canvas->scale_factor(), 2.0f, 1e-4f);
    ASSERT_NEAR(canvas->root_rect().size().x, 960.0f, 1e-3f);   // half the canvas pixels...
    ASSERT_NEAR(canvas->root_rect().size().y, 540.0f, 1e-3f);   // ...so everything looks twice as big

    // Text re-bakes rather than being magnified: the atlas density follows the scale factor.
    ASSERT_NEAR(canvas->effective_text_scale(), 2.0f, 1e-4f);

    // Composes with the asset's own multiplier, and <= 0 is ignored here too.
    canvas->scaler.user_scale = 1.5f;
    canvas->set_viewport(1920, 1080);
    ASSERT_NEAR(canvas->scale_factor(), 3.0f, 1e-4f);
    canvas->set_ui_scale(-1.0f);
    canvas->set_viewport(1920, 1080);
    ASSERT_NEAR(canvas->scale_factor(), 1.5f, 1e-4f);
}

COOPA_TEST(scaler_modes_compute_expected_scale_factor) {
    // ConstantPixelSize: always returns scale_factor, regardless of screen size.
    {
        CanvasScaler scaler;
        scaler.mode = ScaleMode::ConstantPixelSize;
        scaler.scale_factor = 2.0f;
        ASSERT_NEAR(scaler.compute_scale_factor(800, 600), 2.0f, 1e-5f);
        ASSERT_NEAR(scaler.compute_scale_factor(3840, 2160), 2.0f, 1e-5f);
    }
    // ScaleWithScreenSize, match_width_or_height = 0 (match width only).
    {
        CanvasScaler scaler;
        scaler.mode = ScaleMode::ScaleWithScreenSize;
        scaler.reference_resolution = { 1920.0f, 1080.0f };
        scaler.match_width_or_height = 0.0f;
        // Exactly at reference resolution -> scale factor 1.
        ASSERT_NEAR(scaler.compute_scale_factor(1920, 1080), 1.0f, 1e-4f);
        // Double the width -> scale factor 2 (matching width exactly).
        ASSERT_NEAR(scaler.compute_scale_factor(3840, 1080), 2.0f, 1e-4f);
    }
    // ScaleWithScreenSize, match_width_or_height = 1 (match height only).
    {
        CanvasScaler scaler;
        scaler.mode = ScaleMode::ScaleWithScreenSize;
        scaler.reference_resolution = { 1920.0f, 1080.0f };
        scaler.match_width_or_height = 1.0f;
        ASSERT_NEAR(scaler.compute_scale_factor(1280, 2160), 2.0f, 1e-4f);
    }
    // ConstantPhysicalSize: scale factor tracks DPI relative to the 96dpi reference.
    {
        CanvasScaler scaler;
        scaler.mode = ScaleMode::ConstantPhysicalSize;
        scaler.fallback_dpi = 96.0f;
        ASSERT_NEAR(scaler.compute_scale_factor(1920, 1080), 1.0f, 1e-4f);
        ASSERT_NEAR(scaler.compute_scale_factor(1920, 1080, 192.0f), 2.0f, 1e-4f);
    }
}

COOPA_TEST(rebuild_layout_resolves_nested_rects_and_tracks_resize) {
    auto canvas_obj = std::make_unique<SceneObject>("Canvas");
    auto* canvas = canvas_obj->add_component<CanvasComponent>();
    canvas->scaler.mode = ScaleMode::ConstantPixelSize;
    canvas->scaler.scale_factor = 1.0f;

    auto* panel_obj = canvas_obj->add_child(std::make_unique<SceneObject>("Panel"));
    auto* panel_rt = panel_obj->add_component<RectTransform>();
    panel_rt->anchor_preset(AnchorPreset::TopLeft);
    panel_rt->set_anchored_position({ 20.0f, -20.0f });
    panel_rt->set_size_delta({ 200.0f, 100.0f });

    auto* child_obj = panel_obj->add_child(std::make_unique<SceneObject>("Child"));
    auto* child_rt = child_obj->add_component<RectTransform>();
    child_rt->anchor_preset(AnchorPreset::StretchAll);
    child_rt->set_anchored_position({ 0.0f, 0.0f });
    child_rt->set_size_delta({ -10.0f, -10.0f });  // 5px margin on all sides

    canvas->rebuild_layout(1000, 500);

    ASSERT_VEC_NEAR(canvas->root_rect().size(), glm::vec2(1000.0f, 500.0f), 1e-4f);

    // TopLeft preset, canvas space is +Y up, so "top" is at max.y.
    const Rect& panel_rect = panel_rt->rect();
    ASSERT_NEAR(panel_rect.min.x, 20.0f, 1e-3f);
    ASSERT_NEAR(panel_rect.max.y, 500.0f - 20.0f, 1e-3f);
    ASSERT_VEC_NEAR(panel_rect.size(), glm::vec2(200.0f, 100.0f), 1e-3f);

    const Rect& child_rect = child_rt->rect();
    ASSERT_NEAR(child_rect.min.x, panel_rect.min.x + 5.0f, 1e-3f);
    ASSERT_NEAR(child_rect.max.x, panel_rect.max.x - 5.0f, 1e-3f);
    ASSERT_NEAR(child_rect.min.y, panel_rect.min.y + 5.0f, 1e-3f);
    ASSERT_NEAR(child_rect.max.y, panel_rect.max.y - 5.0f, 1e-3f);

    // Re-running at a different screen size must update, not accumulate.
    canvas->rebuild_layout(2000, 1000);
    ASSERT_VEC_NEAR(canvas->root_rect().size(), glm::vec2(2000.0f, 1000.0f), 1e-4f);
    ASSERT_NEAR(panel_rt->rect().max.y, 1000.0f - 20.0f, 1e-3f);
}

COOPA_TEST(collect_canvases_sorts_by_sort_order_with_independent_scales) {
    coopa::scene::Scene scene("SortOrderFixture");

    // Added in descending sort_order to prove collect_canvases() sorts rather
    // than just keeping insertion/document order.
    auto obj_a = std::make_unique<SceneObject>("CanvasA");
    auto* canvas_a = obj_a->add_component<CanvasComponent>();
    canvas_a->sort_order = 5;
    canvas_a->scaler.mode = ScaleMode::ConstantPixelSize;
    canvas_a->scaler.scale_factor = 2.0f;
    scene.add_root_object(std::move(obj_a));

    auto obj_b = std::make_unique<SceneObject>("CanvasB");
    auto* canvas_b = obj_b->add_component<CanvasComponent>();
    canvas_b->sort_order = 1;
    canvas_b->scaler.mode = ScaleMode::ConstantPixelSize;
    canvas_b->scaler.scale_factor = 3.0f;
    scene.add_root_object(std::move(obj_b));

    scene.start();

    auto canvases = collect_canvases(scene);
    ASSERT_TRUE(canvases.size() == 2);
    ASSERT_TRUE(canvases[0] == canvas_b);  // sort_order 1, drawn first (bottom)
    ASSERT_TRUE(canvases[1] == canvas_a);  // sort_order 5, drawn last (top)

    // Each canvas is driven directly through Scene::late_update() -- no wrapper
    // class involved -- and computes its OWN scale factor independently rather
    // than sharing one "primary" scale factor.
    for (auto* c : canvases) c->set_viewport(800, 600);
    scene.late_update(0.016f);
    ASSERT_NEAR(canvas_a->scale_factor(), 2.0f, 1e-4f);
    ASSERT_NEAR(canvas_b->scale_factor(), 3.0f, 1e-4f);
}

COOPA_TEST(late_update_alone_lays_out_and_emits_draw_batches) {
    // No wrapper class, no external rebuild_layout()/rebuild_emit() calls -- just
    // Scene::late_update(), exactly like an application drives a real frame.
    coopa::scene::Scene scene("CanvasSelfDriven");

    auto canvas_obj = std::make_unique<SceneObject>("Canvas");
    auto* canvas = canvas_obj->add_component<CanvasComponent>();
    canvas->scaler.mode = ScaleMode::ConstantPixelSize;
    canvas->scaler.scale_factor = 1.0f;

    auto* panel = canvas_obj->add_child(std::make_unique<SceneObject>("Panel"));
    panel->add_component<RectTransform>()->set_size_delta({ 50.0f, 50.0f });
    panel->add_component<Image>()->color = glm::vec4(1.0f);

    scene.add_root_object(std::move(canvas_obj));
    scene.start();

    canvas->set_viewport(800, 600);
    ASSERT_NEAR(canvas->root_rect().size().x, 800.0f, 1e-3f);

    scene.late_update(0.016f);  // measure -> arrange -> emit -> EventSystem::process, all internal to Canvas

    ASSERT_TRUE(!canvas->draw_list().batches().empty());
    ASSERT_TRUE(collect_canvases(scene).size() == 1);
    ASSERT_TRUE(collect_canvases(scene)[0] == canvas);
}
