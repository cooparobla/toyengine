#include <toyengine/core/boot_splash.h>

#include <cmath>

#include <gfxcoopa/app/context.h>
#include <gfxcoopa/command/command_buffer.h>

#include <uicoopa/render/ui_pass.h>

namespace toy {
namespace core {

namespace {

/** @brief Display-referred sRGB to linear, matching upscale.frag's srgb_decode(). */
float to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

glm::vec3 to_linear(const glm::vec3& c) {
    return {to_linear(c.r), to_linear(c.g), to_linear(c.b)};
}

}  // namespace

void emit_boot_splash(coopa::ui::DrawList& dl, glm::vec2 canvas, const BootSplashStyle& style,
                      float time) {
    using coopa::ui::Rect;
    if (!(canvas.x > 0.0f) || !(canvas.y > 0.0f)) return;

    dl.begin(Rect{glm::vec2(0.0f), canvas});

    // The splash draws straight into the swapchain's own pass -- no tonemap/output stage ahead
    // of it the way a game frame has -- so an sRGB attachment needs its colours pre-linearized
    // here; the hardware's encode-on-write then reproduces the authored value exactly.
    LoadingConfig loading = style.loading;
    if (style.linearize) {
        loading.background = to_linear(loading.background);
        loading.spinner_color = to_linear(loading.spinner_color);
    }

    // Background: a full-screen flat fill in the loading cover's own colour, so launch and the
    // scene-load cover that follows it are one continuous screen with no seam.
    dl.add_quad(Rect{glm::vec2(0.0f), canvas}, Rect{glm::vec2(0.5f), glm::vec2(0.5f)},
               coopa::ui::UiVertex::pack_color(loading.background.r, loading.background.g,
                                               loading.background.b, 1.0f));

    emit_loading_screen(dl, canvas, loading, time);
}

BootSplash::BootSplash(coopa::gfx::core::Device& device,
                       coopa::gfx::memory::Allocator& allocator,
                       coopa::gfx::command::CommandPool& cmd_pool,
                       coopa::gfx::pipeline::RenderPass& pass,
                       coopa::gfx::Format pass_format,
                       const coopa::gfx::pipeline::ShaderLibrary& shaders,
                       BootSplashStyle style)
    : style_(std::move(style))
{
    style_.linearize = coopa::gfx::is_srgb(pass_format);

    // One pipeline (flat-coloured quads/triangles) over uicoopa's prebuilt shader, and room for
    // the one texture this screen ever binds: white.
    ui_pass_ = std::make_unique<coopa::ui::UiPass>(
        device, allocator, cmd_pool, pass,
        shaders("ui.vert"), shaders("ui_quad.frag"), shaders("ui_text.frag"), /*max_textures=*/8);
    dl_.set_default_texture(ui_pass_->white_view());
    dl_.set_text_scale(1.0f);   // One canvas pixel is one target pixel; the layout scales itself.
}

BootSplash::~BootSplash() = default;

void BootSplash::prepare(uint32_t frame_index, uint32_t width, uint32_t height) {
    emit_boot_splash(dl_, glm::vec2(static_cast<float>(width), static_cast<float>(height)), style_,
                     elapsed());
    ui_pass_->register_textures(dl_);
    ui_pass_->begin_frame(frame_index, dl_.vertices().size(), dl_.indices().size());
}

void BootSplash::record(coopa::gfx::command::CommandBuffer& cmd, uint32_t frame_index,
                        uint32_t width, uint32_t height) {
    ui_pass_->draw(cmd, frame_index, width, height, /*scale_factor=*/1.0f, dl_);
}

bool BootSplash::present(coopa::gfx::app::Context& ctx) {
    ctx.window().poll_events();
    const auto extent = ctx.swapchain().extent();
    if (extent.width == 0 || extent.height == 0) return false;
    const uint32_t frame = ctx.current_frame();
    // prepare() may grow this slot's geometry buffers, which destroys the ones a previous splash
    // frame could still be reading -- draw_frame()'s own wait comes too late for that (see
    // Renderer::wait_for_current_frame()'s doc).
    ctx.renderer().wait_for_current_frame();
    prepare(frame, extent.width, extent.height);
    coopa::gfx::app::FrameCallbacks cb;
    cb.record = [this, frame, extent](coopa::gfx::command::CommandBuffer& cmd) {
        record(cmd, frame, extent.width, extent.height);
    };
    cb.clear = coopa::gfx::ClearColor{0.0f, 0.0f, 0.0f, 1.0f};   // the background quad covers it
    last_pump_ = Clock::now();
    return ctx.frame(cb);
}

bool BootSplash::due(float min_interval) {
    const Clock::time_point now = Clock::now();
    if (last_pump_ != Clock::time_point{} &&
        std::chrono::duration<float>(now - last_pump_).count() < min_interval) {
        return false;
    }
    last_pump_ = now;
    return true;
}

float BootSplash::elapsed() const {
    return std::chrono::duration<float>(Clock::now() - start_).count();
}

}  // namespace core
}  // namespace toy
