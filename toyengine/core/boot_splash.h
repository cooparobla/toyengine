/**
 * @file boot_splash.h
 * @brief The branded screen every toyengine host shows while it starts up.
 *
 * A window that the window system maps before anything has been presented into it shows
 * whatever was on that part of the screen before, for as long as startup takes -- pipeline
 * creation, asset loaders, then a whole scene read, built and drained. BootSplash fills that
 * gap: it draws the logo, the wordmark, a progress bar and a status line, and needs nothing but
 * what gfx::app::Context already owns (device, allocator, command pool, a render pass), so it is
 * up and presenting before ToyRenderPipeline exists. Engine builds one in its constructor, pumps
 * it at each startup milestone (Engine::boot_progress()) and drops it after the first frame the
 * app itself renders.
 *
 * The drawing is uicoopa's: a UiPass over the swapchain render pass, with the quads and glyphs
 * emitted by emit_boot_splash() -- a pure function of the canvas size and the status, so the
 * layout is unit-testable without a device.
 */

#ifndef TOYENGINE_CORE_BOOT_SPLASH_H
#define TOYENGINE_CORE_BOOT_SPLASH_H

#include <glm/glm.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <gfxcoopa/pipeline/shader_library.h>
#include <gfxcoopa/types/format.h>
#include <gfxcoopa/types/texture_view.h>
#include <uicoopa/ui_resources.h>
#include <uicoopa/render/draw_list.h>

namespace coopa::gfx {
namespace core { class Device; }
namespace memory { class Allocator; }
namespace command { class CommandPool; class CommandBuffer; }
namespace pipeline { class RenderPass; }
namespace app { class Context; }
namespace engine::data { class Texture; }
}  // namespace coopa::gfx

namespace coopa::ui {
class UiPass;
class Font;
namespace imm { class TextRenderer; }
}  // namespace coopa::ui

namespace toy::core {

/**
 * @struct BootSplashStyle
 * @brief What the splash shows: its backdrop, its wordmark and (optionally) a project's own logo.
 *
 * The defaults are toyengine's own branding (branding.h). A game project overrides them through
 * the `window.splash_color` / `splash_title` / `splash_image` config keys; the editor and the hub
 * always keep the defaults, since they are toyengine rather than the project they opened.
 */
struct BootSplashStyle {
    /// Backdrop base colour, display-referred RGB; the gradient darkens it toward the bottom.
    glm::vec3   backdrop{0.227f, 0.184f, 0.420f};
    /// Wordmark under the logo. Empty draws no wordmark.
    std::string title = "toyengine";
    /// A PNG to draw in place of the toyengine block logo -- absolute, or relative to an asset root.
    std::string logo_image;
    /**
     * True decodes every colour to linear before packing it, so the hardware's encode-on-write
     * into an sRGB attachment reproduces the authored value exactly -- the same cancellation
     * upscale.frag does for the game's own image. BootSplash sets it from its target's format;
     * colours here (and in `window.splash_color`) are always authored display-referred.
     */
    bool linearize = false;
};

/**
 * @brief Emits one splash frame's geometry into `dl` -- backdrop, logo, wordmark, bar, status.
 *
 * Pure: no device, no file access, no state of its own. Canvas space is uicoopa's (origin
 * bottom-left, +Y up) and one canvas pixel is one target pixel, so `canvas` is the framebuffer
 * size. Everything scales with the canvas height, so the same layout reads the same on a 720p
 * window and a 4K one.
 *
 * @param dl       Target draw list. begin() is called here; the caller supplies the white
 *                 default texture (DrawList::set_default_texture()) that the flat fills sample.
 * @param canvas   Target size in pixels.
 * @param style    Backdrop colour, wordmark and logo override.
 * @param logo     The logo texture's view.
 * @param text     Glyph emitter, or null to draw no text at all (a missing font is not fatal).
 * @param status   The status line under the bar, e.g. "Loading scene...". May be empty.
 * @param progress 0..1 fills the bar; negative draws an indeterminate sweep driven by `time`.
 * @param time     Seconds since the splash appeared, for the sweep and the logo's pulse.
 */
void emit_boot_splash(coopa::ui::DrawList& dl, glm::vec2 canvas, const BootSplashStyle& style,
                      coopa::gfx::TextureView logo, coopa::ui::imm::TextRenderer* text,
                      const std::string& status, float progress, float time);

/**
 * @class BootSplash
 * @brief Owns the splash's GPU resources and presents it into a Context's swapchain.
 *
 * Construction loads the UI pipelines (uicoopa's prebuilt `ui.vert` / `ui_quad.frag` /
 * `ui_text.frag`), rasterizes the logo and loads the wordmark font -- a few tens of
 * milliseconds, deliberately all of it before the window is mapped.
 */
class BootSplash {
public:
    /**
     * @brief Builds the pass, the logo texture and the font.
     *
     * @param device      Logical device.
     * @param allocator   VMA allocator.
     * @param cmd_pool    Command pool for the logo/font uploads.
     * @param pass        The render pass the splash records into (Context's swapchain pass, or
     *                    an offscreen target's in a test).
     * @param pass_format The colour format of `pass`'s attachment -- decides whether the
     *                    colours are decoded to linear first (see BootSplashStyle::linearize).
     * @param shaders     Resolves `ui.vert` / `ui_quad.frag` / `ui_text.frag`.
     * @param asset_roots Asset search roots, for the wordmark font and a project logo PNG.
     * @param style       See BootSplashStyle.
     * @throws std::runtime_error if the UI shaders cannot be resolved.
     */
    BootSplash(coopa::gfx::core::Device& device,
               coopa::gfx::memory::Allocator& allocator,
               coopa::gfx::command::CommandPool& cmd_pool,
               coopa::gfx::pipeline::RenderPass& pass,
               coopa::gfx::Format pass_format,
               const coopa::gfx::pipeline::ShaderLibrary& shaders,
               const std::vector<std::string>& asset_roots,
               BootSplashStyle style = {});
    ~BootSplash();

    BootSplash(const BootSplash&) = delete;
    BootSplash& operator=(const BootSplash&) = delete;

    /**
     * @brief Builds this frame's geometry and resolves its textures.
     *
     * Must run before the render pass is opened -- UiPass resolves descriptor sets and may grow
     * its geometry buffers here, neither of which is safe mid-pass (see ui_pass.h).
     */
    void prepare(uint32_t frame_index, uint32_t width, uint32_t height,
                 const std::string& status, float progress);

    /** @brief Records the prepared frame into an already-open render pass. */
    void record(coopa::gfx::command::CommandBuffer& cmd, uint32_t frame_index,
                uint32_t width, uint32_t height);

    /**
     * @brief Presents one splash frame: OS events, prepare(), and the Context's own frame.
     *
     * Pumps `poll_events()` rather than Context::poll() on purpose: a boot frame must not
     * advance the frame timer or clear the input state the app's first real frame reads.
     *
     * @return False if the frame was skipped (a zero-sized/minimized window).
     */
    bool present(coopa::gfx::app::Context& ctx, const std::string& status, float progress);

    /**
     * @brief True at most once per `min_interval` seconds -- the throttle for a pump inside a
     *        tight loop (the asset drain), so presenting does not pace the work it reports on.
     *        Updates the stamp when it returns true.
     */
    bool due(float min_interval);

    /** @brief Seconds since construction. */
    float elapsed() const;

    const BootSplashStyle& style() const { return style_; }

private:
    using Clock = std::chrono::steady_clock;

    BootSplashStyle style_;
    std::unique_ptr<coopa::ui::UiPass> ui_pass_;
    std::unique_ptr<coopa::gfx::engine::data::Texture> logo_;
    coopa::ui::Font* font_ = nullptr;   ///< Owned by UIResourceCache (cleared with the Engine).
    std::unique_ptr<coopa::ui::imm::TextRenderer> text_;
    coopa::ui::DrawList dl_;
    Clock::time_point start_ = Clock::now();
    Clock::time_point last_pump_ = Clock::time_point{};
};

}  // namespace toy::core

#endif  // TOYENGINE_CORE_BOOT_SPLASH_H
