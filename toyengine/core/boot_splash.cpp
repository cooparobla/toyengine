#include <toyengine/core/boot_splash.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#include <gfxcoopa/app/context.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/data/texture.h>

// Before uicoopa's widget headers: Graphic::owner_rect() calls through SceneObject.
#include <coopa/scene/scene_object.h>

// Declarations only -- STB_IMAGE_IMPLEMENTATION is compiled once elsewhere in the link.
#include <stb/stb_image.h>

#include <uicoopa/ui_resources.h>
#include <uicoopa/immediate/imm.h>
#include <uicoopa/render/ui_pass.h>
#include <uicoopa/text/font_defaults.h>
#include <uicoopa/text/text_atlas_registry.h>

#include <toyengine/core/branding.h>

namespace toy {
namespace core {

namespace {

/** @brief Display-referred sRGB to linear, matching upscale.frag's srgb_decode(). */
float to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

/** @brief Packs a colour for a DrawList vertex, decoding to linear for an sRGB attachment. */
uint32_t pack(const glm::vec3& rgb, float alpha, bool linearize) {
    const glm::vec3 c = linearize ? glm::vec3(to_linear(rgb.r), to_linear(rgb.g), to_linear(rgb.b)) : rgb;
    return coopa::ui::UiVertex::pack_color(c.r, c.g, c.b, alpha);
}

coopa::ui::Rect centered_rect(glm::vec2 center, glm::vec2 size) {
    return coopa::ui::Rect{center - size * 0.5f, center + size * 0.5f};
}

/// The bar's fill and the wordmark, from the logo's own palette (branding.h).
const glm::vec3 kToyYellow{1.0f, 0.784f, 0.271f};
const glm::vec3 kWordmark{0.953f, 0.937f, 0.988f};
const glm::vec3 kStatus{0.655f, 0.624f, 0.784f};

/** @brief The first existing `<root>/<relative>` across `roots`, or an empty path. */
std::string find_in_roots(const std::vector<std::string>& roots, const std::string& relative) {
    for (const std::string& root : roots) {
        const std::filesystem::path p = std::filesystem::path(root) / relative;
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p.string();
    }
    return {};
}

}  // namespace

void emit_boot_splash(coopa::ui::DrawList& dl, glm::vec2 canvas, const BootSplashStyle& style,
                      coopa::gfx::TextureView logo, coopa::ui::imm::TextRenderer* text,
                      const std::string& status, float progress, float time) {
    using coopa::ui::Rect;
    if (!(canvas.x > 0.0f) || !(canvas.y > 0.0f)) return;

    const bool lin = style.linearize;
    dl.begin(Rect{glm::vec2(0.0f), canvas});

    // Everything is placed in multiples of `s`, so the whole screen scales with the window
    // rather than the logo shrinking into a corner of a 4K display.
    const float s  = std::clamp(canvas.y / 720.0f, 0.55f, 3.0f);
    const float cx = canvas.x * 0.5f;
    const float cy = canvas.y * 0.5f;

    // Backdrop: the authored colour at the top, darkened toward the bottom. A full-screen quad
    // rather than the render pass's clear colour, because the gradient is the whole look.
    const uint32_t top = pack(style.backdrop, 1.0f, lin);
    const uint32_t bot = pack(style.backdrop * 0.45f, 1.0f, lin);
    dl.add_quad_gradient(glm::vec2(0.0f, canvas.y), glm::vec2(canvas.x, canvas.y),
                         glm::vec2(canvas.x, 0.0f), glm::vec2(0.0f, 0.0f),
                         top, top, bot, bot);

    // Logo, breathing slowly so a long load reads as alive rather than frozen. The wordmark is
    // placed against the UNPULSED size below, so the breathing never nudges the text.
    const float logo_size = 152.0f * s;
    const float logo_px   = logo_size * (1.0f + 0.015f * std::sin(time * 1.8f));
    const float logo_cy   = cy + 70.0f * s;
    dl.set_texture(logo);
    // uv.min pairs with pos.min (bottom-left) and the logo's first pixel row is its TOP, so the
    // V range runs 1 -> 0 up the quad -- the same flip FontAtlas's glyph UVs carry.
    dl.add_quad(centered_rect(glm::vec2(cx, logo_cy), glm::vec2(logo_px)),
                Rect{glm::vec2(0.0f, 1.0f), glm::vec2(1.0f, 0.0f)}, pack(glm::vec3(1.0f), 1.0f, lin));
    dl.set_texture(dl.default_texture());

    // Progress bar: a full-width track with the fill inside it.
    const float bar_w = 380.0f * s;
    const float bar_h = 5.0f * s;
    const float bar_y = cy - 96.0f * s;
    const Rect  track = centered_rect(glm::vec2(cx, bar_y), glm::vec2(bar_w, bar_h));
    dl.add_rounded_rect(track, bar_h * 0.5f, coopa::ui::DrawList::kRoundAll,
                        pack(glm::vec3(1.0f), 0.14f, lin));
    Rect fill = track;
    if (progress >= 0.0f) {
        fill.max.x = track.min.x + std::clamp(progress, 0.0f, 1.0f) * bar_w;
    } else {
        // Indeterminate: a short segment sliding back and forth, so a stage that cannot report
        // progress still shows motion.
        const float seg   = bar_w * 0.3f;
        const float phase = time * 0.5f - std::floor(time * 0.5f);
        const float tri   = phase < 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f;
        const float eased = tri * tri * (3.0f - 2.0f * tri);
        fill.min.x = track.min.x + eased * (bar_w - seg);
        fill.max.x = fill.min.x + seg;
    }
    if (fill.max.x - fill.min.x > bar_h) {
        dl.add_rounded_rect(fill, bar_h * 0.5f, coopa::ui::DrawList::kRoundAll,
                            pack(kToyYellow, 1.0f, lin));
    }

    if (!text) return;   // No font: the logo and the bar carry the screen on their own.
    // TextRenderer places a line by its top-left in y-DOWN space, so each baseline below is
    // canvas.y minus the canvas-space top it should sit at.
    auto line = [&](const std::string& str, float canvas_top, float size, const glm::vec3& color) {
        if (str.empty()) return;
        const float x = cx - text->width(str, size) * 0.5f;
        text->draw(dl, canvas.y, glm::vec2(x, canvas.y - canvas_top), str, size, pack(color, 1.0f, lin));
    };
    line(style.title, logo_cy - logo_size * 0.5f - 22.0f * s, 38.0f * s, kWordmark);
    line(status, bar_y - bar_h - 16.0f * s, 17.0f * s, kStatus);
}

BootSplash::BootSplash(coopa::gfx::core::Device& device,
                       coopa::gfx::memory::Allocator& allocator,
                       coopa::gfx::command::CommandPool& cmd_pool,
                       coopa::gfx::pipeline::RenderPass& pass,
                       coopa::gfx::Format pass_format,
                       const coopa::gfx::pipeline::ShaderLibrary& shaders,
                       const std::vector<std::string>& asset_roots,
                       BootSplashStyle style)
    : style_(std::move(style))
{
    style_.linearize = coopa::gfx::is_srgb(pass_format);

    // Two pipelines (quads and glyphs) over uicoopa's prebuilt shaders, and room for the handful
    // of textures this screen binds: white, the logo, and a glyph atlas per text size.
    ui_pass_ = std::make_unique<coopa::ui::UiPass>(
        device, allocator, cmd_pool, pass,
        shaders("ui.vert"), shaders("ui_quad.frag"), shaders("ui_text.frag"), /*max_textures=*/8);
    dl_.set_default_texture(ui_pass_->white_view());
    dl_.set_text_scale(1.0f);   // One canvas pixel is one target pixel; the layout scales itself.

    // Both the logo and the colours are authored display-referred, so an sRGB attachment wants
    // an sRGB texture too: the sampler's decode and the attachment's encode then cancel, exactly
    // as linearize does for the flat fills.
    const coopa::gfx::Format logo_format = style_.linearize ? coopa::gfx::Format::RGBA8_Srgb
                                                            : coopa::gfx::Format::RGBA8_Unorm;
    auto upload_logo = [&](const uint8_t* pixels, uint32_t w, uint32_t h) {
        logo_ = std::make_unique<coopa::gfx::engine::data::Texture>(
            coopa::gfx::engine::data::Texture::upload(device, allocator, cmd_pool, pixels, w, h, logo_format));
    };
    if (!style_.logo_image.empty()) {
        std::string path = style_.logo_image;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) path = find_in_roots(asset_roots, style_.logo_image);
        int w = 0, h = 0, channels = 0;
        stbi_uc* pixels = path.empty() ? nullptr : stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
        if (pixels) {
            upload_logo(pixels, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
            stbi_image_free(pixels);
        } else {
            std::cerr << "[toyengine] Splash image '" << style_.logo_image
                      << "' could not be read; showing the toyengine logo\n";
        }
    }
    if (!logo_) {
        constexpr uint32_t kLogoPx = 256;
        const std::vector<uint8_t> pixels = rasterize_logo(static_cast<int>(kLogoPx));
        upload_logo(pixels.data(), kLogoPx, kLogoPx);
    }

    // The same cache the UI itself loads fonts through, so the wordmark's font is the one the
    // editor/HUD go on to use rather than a second copy. ~Engine clears it.
    coopa::ui::UIResourceCache::instance().configure(device, allocator, cmd_pool);
    std::string font_path = find_in_roots(asset_roots, "fonts/inter_semibold.ttf");
    if (font_path.empty()) font_path = find_in_roots(asset_roots, "fonts/inter_regular.ttf");
    if (!font_path.empty()) {
        // font_for_path() adopts the first font it loads as FontDefaults::font (the fallback a
        // theme without one resolves to). The splash is not that choice, so leave it unset.
        coopa::ui::Font* const before = coopa::ui::FontDefaults::font;
        font_ = coopa::ui::UIResourceCache::instance().font_for_path(font_path);
        if (!before && coopa::ui::FontDefaults::font == font_) coopa::ui::FontDefaults::font = nullptr;
    }
    if (font_) text_ = std::make_unique<coopa::ui::imm::TextRenderer>(font_);
}

BootSplash::~BootSplash() = default;

void BootSplash::prepare(uint32_t frame_index, uint32_t width, uint32_t height,
                         const std::string& status, float progress) {
    emit_boot_splash(dl_, glm::vec2(static_cast<float>(width), static_cast<float>(height)), style_,
                     logo_->view_typed(), text_.get(), status, progress, elapsed());
    // Baking a glyph atlas is what makes a text view exist at all, so both of these run after
    // the emit: the atlas has to be marked as coverage (not colour) before it is drawn, and
    // registered as a descriptor set before the render pass opens.
    coopa::ui::TextAtlasRegistry::instance().mark_all(*ui_pass_);
    ui_pass_->register_textures(dl_);
    ui_pass_->begin_frame(frame_index, dl_.vertices().size(), dl_.indices().size());
}

void BootSplash::record(coopa::gfx::command::CommandBuffer& cmd, uint32_t frame_index,
                        uint32_t width, uint32_t height) {
    ui_pass_->draw(cmd, frame_index, width, height, /*scale_factor=*/1.0f, dl_);
}

bool BootSplash::present(coopa::gfx::app::Context& ctx, const std::string& status, float progress) {
    ctx.window().poll_events();
    const auto extent = ctx.swapchain().extent();
    if (extent.width == 0 || extent.height == 0) return false;
    const uint32_t frame = ctx.current_frame();
    // prepare() may grow this slot's geometry buffers, which destroys the ones a previous splash
    // frame could still be reading -- draw_frame()'s own wait comes too late for that (see
    // Renderer::wait_for_current_frame()'s doc).
    ctx.renderer().wait_for_current_frame();
    prepare(frame, extent.width, extent.height, status, progress);
    coopa::gfx::app::FrameCallbacks cb;
    cb.record = [this, frame, extent](coopa::gfx::command::CommandBuffer& cmd) {
        record(cmd, frame, extent.width, extent.height);
    };
    cb.clear = coopa::gfx::ClearColor{0.0f, 0.0f, 0.0f, 1.0f};   // the backdrop quad covers it
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
