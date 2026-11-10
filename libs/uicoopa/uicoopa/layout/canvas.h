/**
 * @file canvas.h
 * @brief Root driver for a UI tree: screen- or world-derived root rect, the
 *        explicit measure -> arrange -> emit rebuild pipeline, and (via
 *        late_update()) a fully self-driving per-frame UI pass owning its own
 *        DrawList and UiInput.
 *
 * Two render modes, mirroring Unity's Canvas.renderMode -- see
 * CanvasRenderMode. The world-space mode is deliberately confined to this file:
 * it changes where the root rect comes from and adds a canvas-pixels-to-world
 * matrix, and nothing else. Every RectTransform, Graphic, layout group and
 * widget below the canvas keeps emitting plain canvas-space rects into the
 * DrawList exactly as before, which is what lets a world canvas host the entire
 * widget library with no per-widget work at all.
 */

#ifndef UICOOPA_LAYOUT_CANVAS_H
#define UICOOPA_LAYOUT_CANVAS_H

#include <uicoopa/ui_component.h>
#include <uicoopa/layout/rect.h>
#include <uicoopa/layout/rect_transform.h>
#include <uicoopa/layout/canvas_scaler.h>
#include <uicoopa/render/draw_list.h>
#include <uicoopa/input/ui_input.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/components/transform_component.h>
#include <glm/glm.hpp>
#include <string>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cmath>
#include <optional>

namespace coopa {
namespace ui {

/**
 * @enum CanvasRenderMode
 * @brief Where a canvas's root rect lives, mirroring Unity's Canvas.renderMode.
 */
enum class CanvasRenderMode {
    /// Root rect derived from the framebuffer via CanvasScaler; drawn by UiPass
    /// straight into NDC. The original (and default) behaviour.
    ScreenSpaceOverlay,
    /// Root rect is the authored CanvasComponent::world_size, in canvas pixels,
    /// placed in the 3D world by model(); drawn by UiWorldPass through a real
    /// view/projection. CanvasScaler is ignored -- see world_size.
    WorldSpace,
};

/**
 * @enum CanvasBillboard
 * @brief How a WorldSpace canvas orients itself. Ignored in ScreenSpaceOverlay mode.
 */
enum class CanvasBillboard {
    /// Always square-on to the camera, built from the view matrix's own right/up
    /// axes. Because the resulting quad sits at a constant view-space depth, it
    /// projects to a screen-axis-aligned rectangle -- which is what lets
    /// UiWorldPass turn a Mask's clip rect into an exact scissor.
    CameraFacing,
    /// Honours the owner SceneObject's own 3D rotation (and scale), via
    /// local_right_axis/local_up_axis. True Unity WorldSpace behaviour: the
    /// canvas is a flat object in the scene and can be seen edge-on.
    Transform,
};

/**
 * @class CanvasComponent
 * @brief Screen- or world-space root of a UI tree, driving the measure/arrange/emit rebuild.
 *
 * coopa::scene::SceneObject::update() is a pre-order depth-first walk over
 * components, which happens to visit parents before children — but layout
 * groups need a bottom-up measure pass before the top-down arrange pass, an
 * ordering plain pre-order traversal cannot express. CanvasComponent works
 * around this not by avoiding SceneObject's lifecycle, but by doing its own
 * internal multi-pass recursion (measure_/arrange_/emit_ below) from
 * late_update() rather than update() — Scene::update() runs first across the
 * WHOLE scene (e.g. Button's hover/press color fade), then Scene::late_update()
 * runs second (this canvas's layout+emit), so emitted geometry always reflects
 * this frame's already-updated state instead of trailing it by one frame. See
 * coopa::scene::Component::late_update()'s doc for the general rationale.
 *
 * A loaded Scene needs nothing beyond this to be a live UI scene:
 * @code
 * scene_mgr.load_scene(path);   // Scene::start() already ran
 * // once per frame, per canvas (usually exactly one):
 * canvas->set_viewport(screen_w, screen_h);
 * canvas->set_input(window.input());
 * scene_mgr.update(dt);         // Button color fade, any other component behavior
 * scene_mgr.get_active_scene().late_update(dt);  // this canvas's layout + emit + input dispatch
 * ui_pass.draw(cmd, frame, screen_w, screen_h, canvas->scale_factor(), canvas->draw_list());
 * @endcode
 *
 * rebuild_layout()/rebuild_emit() remain public and independently callable —
 * rebuild_layout() is pure layout math (no DrawList, no GPU type), still safe
 * to exercise from a headless unit test with no window or device
 * involved at all.
 */
class CanvasComponent : public UIComponent {
public:
    CanvasComponent() = default;

    std::string type_name() const override { return "Canvas"; }

    CanvasScaler scaler;   /**< Determines how the root rect's size relates to screen pixels. ScreenSpaceOverlay only. */
    int sort_order = 0;    /**< Higher draws later (on top); see collect_canvases(). */

    /** @brief Screen-space (the default) or world-space. See CanvasRenderMode. */
    CanvasRenderMode render_mode = CanvasRenderMode::ScreenSpaceOverlay;

    /**
     * @brief Extra glyph-atlas density for text on this canvas; see effective_text_scale().
     *
     * A glyph atlas is a bitmap, so it has no resolution to spare: baked at the authored
     * font_size and then magnified on screen, it can only go soft. A ScreenSpaceOverlay canvas
     * needs nothing here -- its magnification is exactly scale_factor(), which is folded in
     * automatically. This knob is for WORLD canvases, whose on-screen size depends on how far
     * away the camera happens to be and so cannot be derived once and for all.
     *
     * Set it to roughly the magnification the canvas is usually seen at: render pixels per
     * canvas pixel, i.e. (render_height / (2 * distance * tan(fov/2))) / pixels_per_unit.
     * Round DOWN when unsure -- slight magnification is a mild blur, while minification through
     * a sampler with no mipmaps aliases, which is the worse failure.
     *
     * 1.0 (the default) reproduces the pre-supersampling behaviour exactly, down to stb's
     * oversampling choice (see FontAtlas's ctor doc).
     */
    float text_supersample = 1.0f;

    /** @brief How a WorldSpace canvas faces. See CanvasBillboard. */
    CanvasBillboard billboard = CanvasBillboard::CameraFacing;

    /**
     * @brief A WorldSpace canvas's root rect size, in CANVAS PIXELS -- not world units.
     *
     * This is the authored design size every child anchors against, exactly as the
     * framebuffer size is in screen-space mode; pixels_per_unit alone decides how big
     * that is in the world. CanvasScaler plays no part in world space: its entire job is
     * relating canvas pixels to *screen* pixels, and a world canvas has no fixed
     * relationship to the screen at all (its on-screen size is whatever perspective
     * makes it).
     */
    glm::vec2 world_size{200.0f, 50.0f};

    /**
     * @brief Canvas pixels per world unit -- the only size knob a WorldSpace canvas has.
     *
     * `world_size / pixels_per_unit` is the canvas's extent in world units, so a
     * 200x50 canvas at 100 spans 2.0 x 0.5 units. Raising this shrinks the canvas
     * without re-laying-out anything.
     */
    float pixels_per_unit = 100.0f;

    /**
     * @brief Hide fragments that fall behind opaque scene geometry.
     *
     * Off by default -- the floating-nameplate look, always legible. When on, the host's
     * render pass discards occluded UI fragments by comparing against a scene depth
     * texture it supplies (see UiWorldPass); this is a shader-side compare rather than a
     * hardware depth test, because the target a world canvas composites into generally
     * has no scene depth attached.
     */
    bool occlude = false;

    /**
     * @brief Which of the owner's LOCAL axes become canvas +X / +Y in
     *        CanvasBillboard::Transform mode.
     *
     * Defaults suit a Z-up engine: canvas right is local +X and canvas up is local +Z,
     * so the canvas normal is cross(+X, +Z) = -Y and an unrotated canvas stands
     * vertical facing world -Y. A Y-up host sets local_up_axis = {0, 1, 0} and needs no
     * code change -- uicoopa deliberately has no opinion about which way is up.
     */
    glm::vec3 local_right_axis{1.0f, 0.0f, 0.0f};
    glm::vec3 local_up_axis{0.0f, 0.0f, 1.0f};

    /** @brief The resolved root rect from the most recent rebuild_layout()/set_viewport() call. */
    const Rect& root_rect() const { return root_rect_; }

    /** @brief The scale factor `scaler` computed in the most recent rebuild_layout()/set_viewport() call. */
    float scale_factor() const { return scale_factor_; }

    /**
     * @brief How many target pixels one canvas pixel covers, for glyph-atlas baking.
     *
     * Screen space: scale_factor() is exactly that ratio already (the pass draws into a
     * screen_w x screen_h viewport while the canvas spans screen/scale_factor canvas pixels),
     * so text_supersample is a pure multiplier on top and is normally left at 1.
     *
     * World space: scale_factor_ is pinned to 1 (canvas pixels ARE the unit -- see
     * resolve_root_rect_()), and the real ratio depends on camera distance, so the author
     * supplies it via text_supersample.
     *
     * Fed to DrawList::set_text_scale() by rebuild_emit(); see Text::emit() for what reads it.
     */
    float effective_text_scale() const {
        return is_world_space() ? text_supersample : scale_factor_ * display_zoom_ * text_supersample;
    }

    /**
     * @brief Runs the bottom-up measure pass followed by the top-down arrange pass.
     *
     * After this call, every descendant RectTransform's rect(), world_matrix(),
     * and measured() are up to date for the given screen size. Pure layout math —
     * does not touch DrawList or any rendering type, so it is safe to call from a
     * headless unit test.
     *
     * In WorldSpace mode both arguments are ignored: the root rect is the authored
     * world_size and the scale factor is fixed at 1 (see resolve_root_rect_()).
     *
     * @param screen_w Framebuffer width in pixels. Unused in WorldSpace mode.
     * @param screen_h Framebuffer height in pixels. Unused in WorldSpace mode.
     */
    void rebuild_layout(uint32_t screen_w, uint32_t screen_h) {
        if (!owner) return;

        resolve_root_rect_(screen_w, screen_h);

        for (auto& child : owner->children()) {
            if (child->active()) measure_(*child);
        }
        for (auto& child : owner->children()) {
            if (child->active()) arrange_(*child, root_rect_);
        }
    }

    /**
     * @brief Runs the emit pass, appending every descendant Graphic's geometry to draw_list.
     *
     * Only forwards the reference to each UIComponent::emit() override — never
     * accesses DrawList's members itself beyond begin()/set_default_texture(),
     * so canvas.h needs only DrawList's public surface.
     *
     * @param draw_list Destination for this frame's UI geometry.
     */
    void rebuild_emit(DrawList& draw_list) {
        if (!owner) return;
        // Before the subtree emits: Text reads this off the list to pick its bake size. Set here
        // rather than in late_update() because this method is public and documented as
        // independently callable with any DrawList -- an external caller would otherwise be
        // silently left on the 1.0 default and render text at the wrong density.
        draw_list.set_text_scale(effective_text_scale());
        draw_list.set_pixel_snap(!is_world_space());
        for (auto& child : owner->children()) {
            if (child->active()) emit_(*child, draw_list, 0);
        }
        // Only reorders batch metadata (see DrawList::finalize_z_order()) so this is
        // a no-op whenever every RectTransform::z_order is left at its default 0.
        draw_list.finalize_z_order();
    }

    /**
     * @brief Sets this frame's framebuffer size, eagerly recomputing root_rect()/scale_factor().
     *
     * Call once per frame, before set_input() (which needs the current
     * root_rect()/scale_factor() to convert the cursor into canvas space) and
     * before Scene::late_update() runs.
     *
     * @param screen_w Framebuffer width in pixels.
     * @param screen_h Framebuffer height in pixels.
     */
    void set_viewport(uint32_t screen_w, uint32_t screen_h) {
        viewport_w_ = screen_w;
        viewport_h_ = screen_h;
        resolve_root_rect_(screen_w, screen_h);
    }

    /**
     * @brief Updates this canvas's own UiInput from the given per-frame input state.
     *
     * Uses the root_rect()/scale_factor() most recently set by set_viewport(),
     * so each canvas converts the same cursor position into its OWN canvas
     * space — correct even when canvases have different scale factors, unlike
     * sharing one UiInput across canvases would be.
     */
    void set_input(const coopa::input::Input& input, float cursor_scale = 1.0f) {
        if (!input_enabled_) { input_.update_idle(glm::vec2(-1.0e6f)); return; }
        input_.update(input, root_rect_, scale_factor_, cursor_scale, screen_origin_, display_zoom_);
    }

    // --- Placement inside part of the target (ScreenSpaceOverlay only) ---

    /**
     * @brief Where this canvas's top-left corner sits in the render target, in target pixels
     *        (+Y down, like the window). (0, 0) -- the default -- is a canvas covering the
     *        target from its corner, which is every game's case.
     *
     * Non-zero when a host draws the canvas into only PART of its target: an editor showing
     * a game HUD inside its viewport panel, or a UI preview frame. set_viewport() still gives
     * the canvas's own size; this only moves it. The host's render pass reads it back
     * (UiPass::draw()'s origin overload) and set_input() subtracts it from the cursor, so
     * drawing and hit testing agree on where the canvas is.
     */
    void set_screen_origin(glm::vec2 origin) { screen_origin_ = origin; }
    glm::vec2 screen_origin() const { return screen_origin_; }

    /**
     * @brief How much the placed canvas is magnified on the target: it is laid out for its
     *        set_viewport() size but drawn into viewport_size() * zoom target pixels. 1 (the
     *        default) draws pixel for pixel.
     *
     * Lets a UI designer preview a canvas at the real game resolution (1920x1080, laid out
     * exactly as the game will) inside a smaller frame -- and zoom into it -- without the
     * layout changing as the frame does. Glyph atlases bake at the magnified size, so text
     * stays sharp when zoomed in.
     */
    void set_display_zoom(float zoom) { display_zoom_ = zoom > 0.0f ? zoom : 1.0f; }
    float display_zoom() const { return display_zoom_; }

    /**
     * @brief A player-facing interface scale the HOST drives, multiplied into scale_factor()
     *        on top of whatever CanvasScaler computed. 1 (the default) changes nothing.
     *
     * Distinct from CanvasScaler::user_scale, which a UI asset authors for itself, and from
     * set_display_zoom(), which magnifies an already-laid-out canvas on the target without
     * changing its layout. This one changes the layout: the canvas spans screen / scale_factor
     * canvas pixels, so 1.25 gives a canvas 20% smaller in authored units -- every widget keeps
     * its authored size while covering 25% more screen, and anchored edges stay on their edges.
     * That is what a "HUD scale" slider means, and why text stays sharp through it
     * (effective_text_scale() folds scale_factor_ in, so glyph atlases re-bake).
     *
     * A host applies one value to every canvas it owns; a canvas that must stay pixel-exact
     * regardless -- a letterboxed cinematic frame, a debug readout -- sets ignore_ui_scale and
     * is skipped. <= 0 is ignored.
     */
    void set_ui_scale(float scale) { ui_scale_ = scale > 0.0f ? scale : 1.0f; }
    float ui_scale() const { return ui_scale_; }

    /**
     * @brief Authored opt-out from the host's set_ui_scale(). The host reads it and leaves
     *        such a canvas at 1; nothing in this class consults it.
     */
    bool ignore_ui_scale = false;


    /** @brief The size set_viewport() last gave, in target pixels (0 until it is called). */
    glm::uvec2 viewport_size() const { return {viewport_w_, viewport_h_}; }

    /**
     * @brief false: set_input() feeds an idle frame (pointer far away, nothing pressed), so
     *        the canvas draws but never reacts. true (the default) is normal input.
     */
    void set_input_enabled(bool enabled) { input_enabled_ = enabled; }
    bool input_enabled() const { return input_enabled_; }

    /**
     * @brief Layout + emit with no input dispatch and no component behaviour -- what a canvas
     *        looks like right now, for a scene that is drawn but not simulating.
     *
     * late_update() only runs while a Scene simulates (or for systems that run in edit
     * mode), so an editor's paused scene would otherwise keep a canvas's DrawList empty and
     * its UI invisible. A host calls this instead, once per frame, after set_viewport().
     */
    void preview_refresh() {
        if (!owner) return;
        rebuild_layout(viewport_w_, viewport_h_);
        update_world_transform(view_);
        draw_list_.begin(root_rect_);
        rebuild_emit(draw_list_);
    }

    /**
     * @brief Lays out but emits NOTHING, so a drawn-but-not-simulating canvas shows as empty.
     *
     * The counterpart to preview_refresh(), for a host that wants a scene's interface out of
     * the way while it is being edited. Clearing the DrawList is the whole point: simply not
     * calling preview_refresh() would leave last frame's geometry in it, and the canvas would
     * keep drawing a frozen copy of itself forever.
     *
     * Layout still runs, so root_rect() and every resolved rect stay queryable -- an editor
     * still needs to know where a widget would be in order to select it.
     */
    void clear_emitted() {
        if (!owner) return;
        rebuild_layout(viewport_w_, viewport_h_);
        update_world_transform(view_);
        draw_list_.begin(root_rect_);
    }

    /** @brief This canvas's own per-frame pointer/keyboard state, as of the last set_input(). */
    const UiInput& input() const { return input_; }

    // --- World space (no-ops / identity in ScreenSpaceOverlay mode) ---

    /** @brief True when render_mode is WorldSpace. */
    bool is_world_space() const { return render_mode == CanvasRenderMode::WorldSpace; }

    /** @brief True when this world canvas asked to be hidden behind opaque geometry. */
    bool occludes() const { return occlude; }

    /**
     * @brief Recomputes model() from the owner's resolved world transform.
     *
     * Cheap (a handful of vector ops, no matrix inverse) and idempotent, so it is safe
     * to call more than once per frame. late_update() calls it automatically; a host
     * that needs model() *before* late_update() runs -- which hit testing does, since
     * itself, right after Scene::update() has resolved the transform hierarchy.
     *
     * @param view World-to-camera matrix. Only read in CanvasBillboard::CameraFacing
     *             mode, for the camera's right/up axes; pass anything in Transform mode.
     */
    void update_world_transform(const glm::mat4& view = glm::mat4(1.0f)) {
        if (!is_world_space() || !owner) return;
        // Cached so late_update()'s own refresh can re-use the host's last view rather
        // than silently falling back to identity (which would un-billboard the canvas).
        view_ = view;

        glm::mat4 owner_world(1.0f);
        if (auto* tc = owner->get_transform()) {
            // get_world_matrix() (not world_matrix()) so this also works with no
            // TransformSystem installed at all, which is how the headless tests drive it.
            // Both are main-thread-only here: late_update() runs on the main thread, after
            // UpdatePhase::TransformResolve has already run inside Scene::update().
            owner_world = tc->transform().get_world_matrix();
        }

        if (billboard == CanvasBillboard::CameraFacing) {
            // `view` maps world -> camera, so the ROWS of its 3x3 rotation part are the
            // camera's basis vectors expressed in world space. glm is column-major
            // (m[col][row]), so transpose(mat3(view))'s COLUMNS are those rows: column 0
            // is camera right, column 1 camera up. transpose() is an exact inverse for a
            // rotation, so no inverse() is needed. normalize() is not redundant:
            // get_view_matrix() is the inverse of a world matrix, so a scaled camera
            // object leaks 1/scale into these rows.
            glm::mat3 cam_basis = glm::transpose(glm::mat3(view));
            right_ = safe_normalize_(cam_basis[0], glm::vec3(1.0f, 0.0f, 0.0f));
            up_    = safe_normalize_(cam_basis[1], glm::vec3(0.0f, 1.0f, 0.0f));
        } else {
            // Transform mode: the owner's own axes, scale included (Unity-like), so a
            // scaled canvas object scales its UI. Not normalized, deliberately.
            glm::mat3 basis(owner_world);
            right_ = basis * local_right_axis;
            up_    = basis * local_up_axis;
        }

        float s = pixels_per_unit > 0.0f ? 1.0f / pixels_per_unit : 1.0f;
        glm::vec3 u = right_ * s;   // world offset per canvas pixel along +X
        glm::vec3 v = up_    * s;   // world offset per canvas pixel along +Y
        normal_ = glm::cross(u, v);

        // Pivot at the canvas centre, so the canvas hangs centred on its owner's origin
        // -- the "health bar above the cube" case, with no offset arithmetic in the scene.
        origin_ = glm::vec3(owner_world[3])
                - u * (0.5f * world_size.x)
                - v * (0.5f * world_size.y);

        model_ = glm::mat4(1.0f);
        model_[0] = glm::vec4(u, 0.0f);
        model_[1] = glm::vec4(v, 0.0f);
        // Column 2 is never exercised (UiVertex has no z, so in_pos.z is always 0); set it
        // to the unit normal anyway so model() reads correctly in a debugger.
        model_[2] = glm::vec4(safe_normalize_(normal_, glm::vec3(0.0f, 0.0f, 1.0f)), 0.0f);
        model_[3] = glm::vec4(origin_, 1.0f);
    }

    /**
     * @brief Canvas pixels -> world space, as of the last update_world_transform().
     *
     * A render pass forms its own clip matrix as `proj * view * model()`. Deliberately
     * not premultiplied by view/proj here: the host owns the authoritative projection
     * (which it may jitter for TAA), and computing the product at draw time is what
     * keeps the UI's depth exactly consistent with the depth buffer it is compared
     * against. Identity in ScreenSpaceOverlay mode.
     */
    const glm::mat4& model() const { return model_; }

    /** @brief The canvas plane's world-space normal (unnormalized), from the last update_world_transform(). */
    const glm::vec3& world_normal() const { return normal_; }

    /**
     * @brief Intersects a world-space ray with this canvas's plane.
     *
     * The pointer-picking half of world-space UI: the host turns a cursor position into
     * a world ray, this turns the ray into a canvas-space point, which a caller hit-tests
     * against whatever it drew.
     *
     * One exact ray/plane solve serves both billboard modes -- a CameraFacing quad's
     * projection happens to be affine (constant
     * view depth) and could be inverted with a 2x2 solve, but Transform mode under
     * perspective is genuinely projective, and one exact path beats two special cases.
     *
     * @param ray_origin World-space ray origin (the eye, for a perspective camera).
     * @param ray_dir    World-space ray direction. Need not be normalized.
     * @return Canvas-space point in canvas pixels (+Y up), or nullopt when the ray is
     *         edge-on to the plane or the plane lies behind the origin. The point is NOT
     *         clamped to the canvas: a hit outside root_rect() is a real miss that
     *         the caller is responsible for rejecting.
     */
    std::optional<glm::vec2> ray_to_canvas(const glm::vec3& ray_origin,
                                           const glm::vec3& ray_dir) const {
        glm::vec3 u(model_[0]);
        glm::vec3 v(model_[1]);
        float denom = glm::dot(normal_, ray_dir);
        if (std::abs(denom) < 1.0e-8f) return std::nullopt;         // edge-on
        float t = glm::dot(normal_, origin_ - ray_origin) / denom;
        if (t < 0.0f) return std::nullopt;                           // plane behind the ray

        glm::vec3 q = ray_origin + ray_dir * t - origin_;
        float uu = glm::dot(u, u);
        float vv = glm::dot(v, v);
        if (uu <= 0.0f || vv <= 0.0f) return std::nullopt;           // degenerate canvas
        // Exact for any ORTHOGONAL u/v, unit-length or not -- which covers both billboard
        // modes and any uniformly or per-axis scaled owner. A sheared owner transform
        // would make u/v non-orthogonal and need a 2x2 Gram inverse instead; no caller
        // does that, and RectTransform layout can't express shear either.
        return glm::vec2(glm::dot(q, u) / uu, glm::dot(q, v) / vv);
    }

    /**
     * @brief Updates this canvas's UiInput from an explicit canvas-space cursor position.
     *
     * The world-space counterpart of set_input(), which can only derive the cursor from a
     * screen-space scale factor. Hosts pair this with ray_to_canvas():
     * @code
     * canvas->update_world_transform(view);
     * auto hit = canvas->ray_to_canvas(ray_origin, ray_dir);
     * // A miss must be parked far outside the canvas, NOT at (0,0) -- that is the
     * // canvas's own bottom-left corner, and would leave a widget there permanently
     * // hovered whenever the pointer is off the canvas.
     * canvas->set_world_input(input, hit ? *hit : glm::vec2(-1.0e6f));
     * @endcode
     */
    void set_world_input(const coopa::input::Input& input, glm::vec2 canvas_pos) {
        input_.update_at(input, canvas_pos);
    }

    /** @brief Sets the texture used for untextured/solid-color quads (typically a 1x1 white texel). */
    void set_default_texture(coopa::gfx::TextureView view) { draw_list_.set_default_texture(view); }

    /** @brief The geometry this canvas emitted on its most recent late_update(). */
    const DrawList& draw_list() const { return draw_list_; }

    /**
     * @brief Full per-frame UI pass: layout (using the viewport set_viewport()
     *        supplied), then emit into this canvas's own draw_list().
     *
     * Driven automatically by Scene::late_update() — see the class doc for
     * the full per-frame sequence an application drives.
     */
    void late_update(float delta_time) override {
        if (!owner) return;
        rebuild_layout(viewport_w_, viewport_h_);
        // After rebuild_layout(), so a world_size written this frame is already reflected
        // in the matrix the render pass will use. Uses the view
        // matrix the host last supplied -- a host driving a CameraFacing canvas calls
        // update_world_transform(view) itself before Scene::late_update() anyway (it needs
        // model() for the pointer ray), and this call re-uses that same cached view.
        update_world_transform(view_);
        draw_list_.begin(root_rect_);
        rebuild_emit(draw_list_);
    }

private:
    /**
     * @brief Computes root_rect_/scale_factor_ for whichever render mode is active.
     *
     * The WorldSpace branch comes FIRST for a reason beyond taste: viewport_w_/h_ default
     * to 0, and CanvasScaler's default ConstantPixelSize mode would turn that into
     * canvas_size = 0/1 = 0 -- an empty root rect, from which nothing draws at all. A
     * world canvas never needs a viewport set, so it must never reach that path.
     */
    void resolve_root_rect_(uint32_t screen_w, uint32_t screen_h) {
        if (is_world_space()) {
            scale_factor_ = 1.0f;   // canvas pixels ARE the unit here; see world_size
            root_rect_ = Rect{ glm::vec2(0.0f), world_size };
            return;
        }
        scale_factor_ = scaler.compute_scale_factor(screen_w, screen_h) * ui_scale_;
        glm::vec2 screen_size(static_cast<float>(screen_w), static_cast<float>(screen_h));
        glm::vec2 canvas_size = scale_factor_ > 0.0f ? screen_size / scale_factor_ : screen_size;
        root_rect_ = Rect{ glm::vec2(0.0f), canvas_size };
    }

    /** @brief normalize() that returns `fallback` instead of NaNs for a zero-length vector. */
    static glm::vec3 safe_normalize_(const glm::vec3& v, const glm::vec3& fallback) {
        float len2 = glm::dot(v, v);
        return len2 > 1.0e-20f ? v * (1.0f / std::sqrt(len2)) : fallback;
    }

    static void measure_(coopa::scene::SceneObject& obj) {
        for (auto& child : obj.children()) {
            if (child->active()) measure_(*child);
        }
        SizeConstraints agg{};
        for (auto& comp : obj.components()) {
            if (auto* ui = dynamic_cast<UIComponent*>(comp.get())) {
                SizeConstraints c = ui->measure();
                agg.min       = glm::max(agg.min, c.min);
                agg.preferred = glm::max(agg.preferred, c.preferred);
                agg.flexible  = glm::max(agg.flexible, c.flexible);
            }
        }
        if (auto* rt = obj.get_component<RectTransform>()) {
            rt->set_measured(agg);
        }
    }

    static void arrange_(coopa::scene::SceneObject& obj, const Rect& parent_rect) {
        auto* rt = obj.get_component<RectTransform>();
        Rect resolved = parent_rect;
        if (rt) {
            rt->resolve(parent_rect);
            resolved = rt->rect();
        }
        // Layout groups override on_rect_changed() to rewrite their children's
        // RectParams here, before those children are resolved in the recursion below.
        for (auto& comp : obj.components()) {
            if (auto* ui = dynamic_cast<UIComponent*>(comp.get())) {
                ui->on_rect_changed(resolved);
            }
        }
        for (auto& child : obj.children()) {
            if (child->active()) arrange_(*child, resolved);
        }
    }

    static void emit_(coopa::scene::SceneObject& obj, DrawList& draw_list, int z_order) {
        auto* rt = obj.get_component<RectTransform>();
        int own = rt ? rt->z_order : 0;
        int effective = z_order + own;
        // A nonzero own z_order escapes every ancestor Mask's clip for this whole
        // subtree -- see RectTransform::z_order's doc and DrawList::push_canvas_clip().
        bool escapes_clip = (own != 0);
        if (escapes_clip) draw_list.push_canvas_clip();

        draw_list.set_z_order(effective);
        for (auto& comp : obj.components()) {
            if (auto* ui = dynamic_cast<UIComponent*>(comp.get())) {
                draw_list.set_z_order(effective);
                ui->emit(draw_list);
            }
        }
        for (auto& child : obj.children()) {
            if (child->active()) emit_(*child, draw_list, effective);
        }
        // A descendant may have left the DrawList's current z_order pointing at its
        // own (deeper) effective value; restore this node's before on_children_emitted.
        draw_list.set_z_order(effective);
        for (auto& comp : obj.components()) {
            if (auto* ui = dynamic_cast<UIComponent*>(comp.get())) {
                ui->on_children_emitted(draw_list);
            }
        }

        if (escapes_clip) draw_list.pop_clip();
    }

    Rect  root_rect_{};
    float scale_factor_ = 1.0f;
    uint32_t viewport_w_ = 0;
    uint32_t viewport_h_ = 0;
    glm::vec2 screen_origin_{0.0f};   ///< See set_screen_origin().
    bool input_enabled_ = true;       ///< See set_input_enabled().
    float display_zoom_ = 1.0f;       ///< See set_display_zoom().
    float ui_scale_ = 1.0f;           ///< See set_ui_scale().

    // World-space state, all recomputed together by update_world_transform() so nothing is
    // ever derived twice or left inconsistent between the render and hit-test paths.
    glm::mat4 model_{1.0f};     ///< Canvas pixels -> world.
    glm::mat4 view_{1.0f};      ///< Last view matrix a host supplied, for late_update()'s refresh.
    glm::vec3 right_{1.0f, 0.0f, 0.0f};
    glm::vec3 up_{0.0f, 1.0f, 0.0f};
    glm::vec3 normal_{0.0f, 0.0f, 1.0f};  ///< cross(model_[0], model_[1]); unnormalized.
    glm::vec3 origin_{0.0f};              ///< World position of canvas-space (0, 0).
    UiInput     input_;
    DrawList    draw_list_;
};

/**
 * @brief Every CanvasComponent in scene, ascending by sort_order (stable sort,
 *        so equal sort_order keeps document/insertion order).
 *
 * Stateless — call fresh each frame (or after loading a scene / changing
 * which canvases exist); cheap for the realistic handful of canvases a scene
 * has. Nothing is cached, so the result can't go stale.
 */
inline std::vector<CanvasComponent*> collect_canvases(coopa::scene::Scene& scene) {
    auto canvases = scene.get_components<CanvasComponent>();
    std::stable_sort(canvases.begin(), canvases.end(),
        [](CanvasComponent* a, CanvasComponent* b) { return a->sort_order < b->sort_order; });
    return canvases;
}

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_LAYOUT_CANVAS_H
