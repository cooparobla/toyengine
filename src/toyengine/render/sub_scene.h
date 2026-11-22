#pragma once

/**
 * @file sub_scene.h
 * @brief The scene-authored `SubScene` component: another scene, simulated and rendered into a
 *        texture this scene's UI can draw.
 *
 * ```yaml
 * - type: SubScene
 *   view: map                      # how UI code finds this view
 *   scene: ui/map_world_demo       # the scene asset to load and simulate
 *   camera: map_camera             # object name of the camera to render it from
 *   width: 512
 *   height: 512
 *   ortho_size: 60.0               # drives that camera's orthographic_size each frame; 0 leaves it
 *   input: hover                   # hover | never
 *   simulate_in_edit_mode: true
 * ```
 *
 * ## What this component is, and is not
 *
 * It is a DECLARATION plus a pair of published outputs. It owns no scene, no pipeline and no GPU
 * resource -- `toy::core::Engine` owns all of those, keyed off this component, which is why this
 * header pulls in nothing from `core/`. That split is also what makes the component safe to
 * destroy and rebuild: the editor rebuilds an object's components on every inspector edit, so
 * nothing with a frame-crossing lifetime may live here.
 *
 * ## Reading it from UI code
 *
 * A screen finds it through its own scene -- no globals:
 *
 * ```cpp
 * for (render::SubScene* sv : scene->get_components<render::SubScene>()) {
 *     if (sv->view_name == "map" && sv->ready) ctx.image(sv->texture, body);
 * }
 * ```
 *
 * and reports back where it drew it with `set_panel_rect()`, which is what hover-gated input
 * routing reads on the following frame.
 *
 * ## The limits worth knowing before authoring one
 *
 * - A sub-view renders geometry and lighting. Its own screen-space UI is deliberately not drawn.
 * - Its resolution is fixed at load. Every render target it owns is sized once, because the
 *   renderer's resolution keys are startup-fixed, so a resize means rebuilding the whole view.
 * - It costs a full second deferred chain per frame. One small view is affordable; several are
 *   not, and nothing here pretends otherwise.
 */

#include <cstdint>
#include <string>

#include <coopa/scene/component.h>
#include <gfxcoopa/types/texture_view.h>
#include <glm/glm.hpp>

namespace toy::render {

/**
 * @enum SubSceneInput
 * @brief Whether a sub-view's scene can be interacted with.
 */
enum class SubSceneInput {
    Never,   ///< Display only. The host keeps the pointer and the keyboard.
    Hover,   ///< While the pointer is over the view's rect, the sub-scene receives input.
};

/**
 * @brief Parses an `input:` value; unknown spellings read as `Hover`.
 *
 * @param s The authored string.
 * @return The matching mode.
 */
inline SubSceneInput parse_sub_scene_input(const std::string& s) {
    return s == "never" ? SubSceneInput::Never : SubSceneInput::Hover;
}

/**
 * @class SubScene
 * @brief Declares another scene to simulate and render into a texture.
 */
class SubScene : public coopa::scene::Component {
public:
    std::string type_name() const override { return "SubScene"; }

    // ------------------------------------------------------------------
    // Authored configuration
    // ------------------------------------------------------------------

    /// How UI code identifies this view among a scene's several. Empty matches the first lookup.
    std::string view_name;
    /// The scene asset to load, resolved like any scene path. Named `scene_ref` because
    /// `Component::scene` is already the Scene this component belongs to.
    std::string scene_ref;
    /// Object name holding the camera to render from. Empty uses the sub-scene's own declared
    /// main camera, else its first.
    std::string camera;
    uint32_t width  = 512;   ///< View width in pixels. Fixed for this view's lifetime.
    uint32_t height = 512;   ///< View height in pixels. Fixed for this view's lifetime.
    /**
     * @brief Drives the view camera's `orthographic_size` every frame; 0 leaves it alone.
     *
     * Here rather than on the sub-scene's own camera so a host's UI can zoom the view -- a map
     * panel's zoom control writes this, and the projection is rebuilt from it next frame.
     */
    float ortho_size = 0.0f;
    SubSceneInput input = SubSceneInput::Hover;   ///< Whether hovering hands it input.
    /// Keep simulating while the host is not playing (an editor in edit mode), so a panel built
    /// against this view shows something real while it is being authored.
    bool simulate_in_edit_mode = true;

    // ------------------------------------------------------------------
    // Published by the engine, read by UI code
    // ------------------------------------------------------------------

    /// The finished view. Valid only while `ready`; drawing an invalid view is a no-op in
    /// `imm::Context::image()`, so a UI need not special-case the frames before it loads.
    coopa::gfx::TextureView texture;
    /// True once the scene is loaded and its first frame has been rendered.
    bool ready = false;

    // ------------------------------------------------------------------
    // Set by UI code, read by the engine on the following frame
    // ------------------------------------------------------------------

    /**
     * @brief Reports where this view was drawn and whether the pointer is over it.
     *
     * Called by whatever draws the view, every frame it draws it. The engine reads it when
     * deciding which view owns the pointer, and to remap the cursor into this view's extent.
     *
     * Read a frame later than it is written, because the UI draws after the input pass -- so
     * hover changes take effect on the next frame. That is inherent to the frame order rather
     * than a bug; see Engine::tick().
     *
     * @param rect_px Where the view was drawn, in window pixels: x, y, width, height.
     * @param hovered Whether the pointer is inside it.
     */
    void set_panel_rect(const glm::vec4& rect_px, bool hovered) {
        panel_rect = rect_px;
        panel_hovered = hovered;
        panel_reported = true;
    }

    glm::vec4 panel_rect{0.0f};    ///< Last reported draw rect, window pixels (x, y, w, h).
    bool panel_hovered = false;    ///< Whether the pointer was over it when last reported.
    /// Whether anything has reported a rect yet. A view nobody draws never takes the pointer.
    bool panel_reported = false;
};

}  // namespace toy::render
