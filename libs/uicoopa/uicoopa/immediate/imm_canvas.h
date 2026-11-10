/**
 * @file imm_canvas.h
 * @brief Hosts an imm::Context inside a CanvasComponent: the immediate-mode UI draws during
 *        the canvas's emit pass, with the canvas's own input, into the canvas's DrawList.
 *
 * Scene shape (build_immediate_canvas() makes exactly this):
 * @code
 * "EditorUI"     CanvasComponent (screen space) + RectTransform
 *   "Immediate"  RectTransform (stretched) + ImmediateCanvas
 * @endcode
 *
 * The callback runs once per frame from Scene::late_update(), inside
 * CanvasComponent::rebuild_emit() -- after the canvas has been handed this frame's viewport
 * and input by its host (Engine::drive_ui_canvases_()) -- so declarations see current input
 * and their geometry lands in the same frame. Anything that restructures a scene should be
 * queued by the callback and applied after late_update() (the editor does exactly that).
 */

#ifndef UICOOPA_IMMEDIATE_IMM_CANVAS_H
#define UICOOPA_IMMEDIATE_IMM_CANVAS_H

#include <uicoopa/immediate/imm.h>
#include <uicoopa/layout/canvas.h>
#include <uicoopa/layout/rect_transform.h>
#include <uicoopa/ui_component.h>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>

#include <chrono>
#include <functional>
#include <memory>

namespace coopa {
namespace ui {

/**
 * @class ImmediateCanvas
 * @brief A UIComponent whose emit() runs an immediate-mode frame.
 */
class ImmediateCanvas : public UIComponent {
public:
    using DrawFn = std::function<void(imm::Context&)>;

    /** @brief The per-frame UI declaration. */
    DrawFn on_draw;

    /** @brief The Context the callback receives -- style and font live here. */
    imm::Context& context() { return ctx_; }

    /** @brief Optional: adjusts each frame's input before the Context sees it (e.g. a host
     *         withholding the mouse while something else owns it). */
    std::function<void(imm::FrameInput&)> filter_input;

    /**
     * @brief Seconds per frame fed to the Context (tooltips, caret blink).
     *
     * Driven by update() from the scene's own delta time. The initial value only covers a
     * host that emits without ticking the scene (a one-shot capture).
     */
    float frame_dt = 1.0f / 60.0f;

    std::string type_name() const override { return "ImmediateCanvas"; }

    /**
     * @brief Whether this pass owns the pointer right now -- a widget is active or hovered,
     *        or a popup is up.
     *
     * What a host asks before letting a click reach the world behind the interface. Resolved
     * from the previous frame's layout, which is the same answer the widgets themselves acted
     * on, so a click can never be claimed by one and used by the other.
     */
    bool wants_pointer() const { return ctx_.wants_mouse() || ctx_.popup_hovered(); }

    /**
     * @brief Takes this frame's delta time for the Context.
     *
     * Scene::update() runs before Scene::late_update(), which is where emit() happens, so the
     * dt the Context sees is the one for the frame it is declaring. Anything timed off
     * FrameInput::dt -- tooltip delay, caret blink, double-click -- depends on this.
     */
    void update(float delta_time) override { frame_dt = delta_time; }

    void emit(DrawList& draw_list) override {
        CanvasComponent* canvas = find_canvas_();
        if (!canvas || !on_draw) return;
        const glm::vec2 size = canvas->root_rect().size();
        imm::FrameInput in = imm::FrameInput::from_ui_input(canvas->input(), size.y, frame_dt);
        if (filter_input) filter_input(in);
        ctx_.begin_frame(draw_list, in, size);
        on_draw(ctx_);
        ctx_.end_frame();
    }

private:
    /**
     * @brief The nearest CanvasComponent strictly ABOVE this component's own object.
     *
     * Starts at the parent, not at `owner`, because a canvas never emits its own object's
     * components: CanvasComponent::rebuild_emit() walks children only, since CanvasComponent
     * is itself a UIComponent and emitting it would recurse. So an ImmediateCanvas must be a
     * DESCENDANT of its canvas -- putting both on one object would find the canvas here and
     * then never be drawn.
     */
    CanvasComponent* find_canvas_() {
        for (coopa::scene::SceneObject* o = owner ? owner->parent() : nullptr; o; o = o->parent()) {
            if (auto* c = o->get_component<CanvasComponent>()) return c;
        }
        return nullptr;
    }

    imm::Context ctx_;
};

/**
 * @brief Builds a screen-space canvas (1 canvas pixel = 1 window pixel) holding one
 *        ImmediateCanvas.
 *
 * @param sort_order Canvas draw order; higher draws on top of other canvases.
 * @param parent Object to hang the canvas under, or null for a new root object. Pass the
 *        component that owns the screen so the canvas DIES WITH IT -- a root-object canvas
 *        outlives its owner and keeps calling a destroyed on_draw. The 3D transform is
 *        deliberately left unlinked (add_child() does not link it), so a screen-space canvas
 *        stays window-relative however the parent is positioned; canvases are found by a
 *        whole-scene component scan (collect_canvases()), so nesting costs nothing.
 * @return The ImmediateCanvas, ready for on_draw and a font.
 */
inline ImmediateCanvas* build_immediate_canvas(coopa::scene::Scene& scene, const std::string& name = "ImmediateUI",
                                               int sort_order = 1000,
                                               coopa::scene::SceneObject* parent = nullptr) {
    auto root = std::make_unique<coopa::scene::SceneObject>(name);
    root->add_component<RectTransform>();
    auto* canvas = root->add_component<CanvasComponent>();
    canvas->sort_order = sort_order;
    auto child = std::make_unique<coopa::scene::SceneObject>("Immediate");
    auto* rt = child->add_component<RectTransform>();
    rt->set_anchor_min(glm::vec2(0.0f));
    rt->set_anchor_max(glm::vec2(1.0f));
    rt->set_size_delta(glm::vec2(0.0f));
    auto* imm_canvas = child->add_component<ImmediateCanvas>();
    root->add_child(std::move(child));
    coopa::scene::SceneObject* raw = parent ? parent->add_child(std::move(root))
                                            : scene.add_root_object(std::move(root));
    scene.adopt(*raw);
    raw->start();
    return imm_canvas;
}

/**
 * @brief Builds a WORLD-SPACE canvas holding one ImmediateCanvas: a panel that lives in the 3D
 *        scene (a nameplate, a sign, a wall terminal) and declares its contents per frame.
 *
 * Scene shape (two objects, because an ImmediateCanvas has to be a DESCENDANT of its canvas
 * -- see find_canvas_()):
 * @code
 * "Nameplate"     TransformComponent + CanvasComponent(WorldSpace)
 *   "Immediate"   ImmediateCanvas
 * @endcode
 *
 * Unlike the screen-space builder this takes a `parent`, because the common case is a plate
 * parented to the thing it labels -- it then follows the body with no per-frame projection to
 * keep in step, and is destroyed with it. With no parent it becomes a root object of `scene`.
 *
 * The canvas must live in the scene the camera is in: a host collects world canvases from the
 * ACTIVE scene, not from overlay layers (an overlay has no camera of its own), and it needs the
 * view matrix to billboard and to map a pointer ray through ray_to_canvas().
 *
 * @param scene            The scene to adopt the object into.
 * @param parent           Object to hang the plate under, or null for a new root object.
 * @param world_size       The authored design size in CANVAS PIXELS (not world units).
 * @param pixels_per_unit  world_size / this = the plate's extent in world units.
 * @param text_supersample Glyph-atlas density; a world canvas has no fixed screen scale, so
 *                         this is how text stays crisp when the plate is magnified.
 * @param billboard        CameraFacing (square-on, exact clip rects) or Transform (a flat
 *                         object in the scene, which can be seen edge-on).
 * @param occlude          Hide behind scene geometry, compared against the depth buffer. Off
 *                         for a nameplate that must stay readable through a hill; on for a
 *                         sign that is physically on a wall.
 * @return The ImmediateCanvas, ready for on_draw and a font.
 */
inline ImmediateCanvas* build_world_immediate_canvas(coopa::scene::Scene& scene,
                                                     coopa::scene::SceneObject* parent = nullptr,
                                                     const std::string& name = "ImmediateWorldUI",
                                                     glm::vec2 world_size = glm::vec2(320.0f, 64.0f),
                                                     float pixels_per_unit = 120.0f,
                                                     float text_supersample = 2.0f,
                                                     CanvasBillboard billboard = CanvasBillboard::CameraFacing,
                                                     bool occlude = false) {
    auto obj = std::make_unique<coopa::scene::SceneObject>(name);
    // The 3D placement the canvas reads through update_world_transform(); without it the plate
    // would resolve against an identity transform and sit at the world origin.
    obj->add_component<coopa::scene::TransformComponent>();
    auto* canvas = obj->add_component<CanvasComponent>();
    canvas->render_mode = CanvasRenderMode::WorldSpace;
    canvas->billboard = billboard;
    canvas->world_size = world_size;
    canvas->pixels_per_unit = pixels_per_unit;
    canvas->text_supersample = text_supersample;
    canvas->occlude = occlude;

    auto child = std::make_unique<coopa::scene::SceneObject>("Immediate");
    auto* imm_canvas = child->add_component<ImmediateCanvas>();
    obj->add_child(std::move(child));

    coopa::scene::SceneObject* raw = parent ? parent->add_child(std::move(obj))
                                            : scene.add_root_object(std::move(obj));
    // add_child() links the OBJECT hierarchy but not the transform one -- that is the
    // caller's job (SceneObject::add_child's own documentation says so, and every engine
    // system that parents at runtime does it). Without this the plate resolves against an
    // identity parent and sits at the world origin instead of on the thing it labels.
    if (parent) {
        if (auto* parent_tc = parent->get_transform()) {
            if (auto* tc = raw->get_transform()) tc->set_parent_transform(&parent_tc->transform());
        }
    }
    scene.adopt(*raw);
    raw->start();
    return imm_canvas;
}

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_IMMEDIATE_IMM_CANVAS_H
