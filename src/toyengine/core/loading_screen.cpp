#include <toyengine/core/loading_screen.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace toy {
namespace core {

namespace {

using coopa::ui::UiVertex;

/// The block's hexagon, in branding.h's own unit square (x right, y down) -- the same centre
/// and radius logo_shapes() uses, so changing the brand there and here stays in agreement.
const glm::vec2 kCenter(0.5f, 0.585f);
const float kRadius = 0.30f;
const float kCx = kRadius * 0.8660254f;   // cos 30

/// One face's four corners (branding units) and where in the cycle (0..1) it peaks.
struct Face {
    glm::vec2 a, b, c, d;
    float peak;
};

const std::vector<Face>& faces() {
    static const std::vector<Face> fs = [] {
        const glm::vec2 top = kCenter + glm::vec2(0.0f, -kRadius);
        const glm::vec2 ur  = kCenter + glm::vec2(kCx, -kRadius * 0.5f);
        const glm::vec2 lr  = kCenter + glm::vec2(kCx, kRadius * 0.5f);
        const glm::vec2 bot = kCenter + glm::vec2(0.0f, kRadius);
        const glm::vec2 ll  = kCenter + glm::vec2(-kCx, kRadius * 0.5f);
        const glm::vec2 ul  = kCenter + glm::vec2(-kCx, -kRadius * 0.5f);
        return std::vector<Face>{
            {top, ur, kCenter, ul, 0.0f},         // top face peaks first
            {kCenter, ur, lr, bot, 1.0f / 3.0f},  // then the right face
            {ul, kCenter, bot, ll, 2.0f / 3.0f},  // then the left face
        };
    }();
    return fs;
}

/// 0..1, peaking at `peak` and floored so a resting face is dim, never fully dark.
float face_brightness(float peak, float t) {
    constexpr float kFloor = 0.32f;
    constexpr float kTau = 6.2831853f;
    const float x = std::cos(kTau * (t - peak));
    const float pulse = x > 0.0f ? x * x : 0.0f;
    return kFloor + (1.0f - kFloor) * pulse;
}

}  // namespace

void emit_loading_screen(coopa::ui::DrawList& dl, glm::vec2 canvas, const LoadingConfig& style,
                         float time) {
    if (!style.spinner) return;
    if (!(canvas.x > 0.0f) || !(canvas.y > 0.0f)) return;

    // Scales with the window, like the boot splash -- a mark pinned to a raw pixel count
    // would shrink into nothing on a 4K display and swamp a small one.
    const float s = std::clamp(canvas.y / 720.0f, 0.5f, 3.0f);
    const float size = 64.0f * s;
    const float margin = 28.0f * s;

    // Placed in top-left, +Y-down space -- branding.h's own convention -- and flipped to
    // DrawList's +Y-up canvas space once at the end, rather than un-deriving every point.
    glm::vec2 origin;
    switch (style.corner) {
        case LoadingCorner::BottomLeft: origin = {margin, canvas.y - margin - size}; break;
        case LoadingCorner::TopRight:   origin = {canvas.x - margin - size, margin}; break;
        case LoadingCorner::TopLeft:    origin = {margin, margin}; break;
        case LoadingCorner::BottomRight:
        default:
            origin = {canvas.x - margin - size, canvas.y - margin - size};
            break;
    }
    const auto at = [&](glm::vec2 u) { return origin + u * size; };
    const auto to_canvas = [&](glm::vec2 p) { return glm::vec2(p.x, canvas.y - p.y); };

    constexpr float kPeriod = 1.1f;   // seconds per revolution of the highlight
    const float t = std::fmod(std::max(time, 0.0f), kPeriod) / kPeriod;

    for (const Face& f : faces()) {
        const float bright = face_brightness(f.peak, t);
        const glm::vec3 tint = style.spinner_color * bright;
        const uint32_t col = UiVertex::pack_color(tint.r, tint.g, tint.b, 1.0f);
        const glm::vec2 a = to_canvas(at(f.a));
        const glm::vec2 b = to_canvas(at(f.b));
        const glm::vec2 c = to_canvas(at(f.c));
        const glm::vec2 d = to_canvas(at(f.d));
        dl.add_triangle(a, b, c, col);
        dl.add_triangle(a, c, d, col);
    }
}

}  // namespace core
}  // namespace toy
