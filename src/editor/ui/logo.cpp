#include "editor/ui/logo.h"

namespace toy {
namespace editor {

void draw_logo(imm::Context& ctx, const imm::Box& box) {
    const float side = std::min(box.w, box.h);
    const glm::vec2 o{box.x + (box.w - side) * 0.5f, box.y + (box.h - side) * 0.5f};
    auto at = [&](glm::vec2 u) { return o + u * side; };
    for (const auto& sh : core::logo_shapes()) {
        switch (sh.kind) {
            case core::LogoShape::Kind::RoundRect: {
                // Horizontal bands, each a trapezoid following the rounded corners, carry the gradient.
                const int bands = side > 40.0f ? 32 : 12;
                auto half_w = [&](float y) {
                    const float top = sh.min.y + sh.corner, bot = sh.max.y - sh.corner;
                    const float dy = y < top ? top - y : y > bot ? y - bot : 0.0f;
                    return (sh.max.x - sh.min.x) * 0.5f - sh.corner + std::sqrt(std::max(0.0f, sh.corner * sh.corner - dy * dy));
                };
                const float cx = (sh.min.x + sh.max.x) * 0.5f;
                for (int i = 0; i < bands; ++i) {
                    const float y0 = sh.min.y + (sh.max.y - sh.min.y) * i / bands, y1 = sh.min.y + (sh.max.y - sh.min.y) * (i + 1) / bands;
                    const float t = (i + 0.5f) / bands;
                    const glm::vec4 col = sh.color_bottom.x >= 0.0f ? glm::mix(sh.color, sh.color_bottom, t) : sh.color;
                    const float w0 = half_w(y0), w1 = half_w(y1);
                    const glm::vec2 a = at({cx - w0, y0}), b = at({cx + w0, y0}), c = at({cx + w1, y1}), d = at({cx - w1, y1});
                    ctx.triangle(a, b, c, col);
                    ctx.triangle(a, c, d, col);
                }
                break;
            }
            case core::LogoShape::Kind::Polygon:
                for (size_t i = 1; i + 1 < sh.points.size(); ++i) ctx.triangle(at(sh.points[0]), at(sh.points[i]), at(sh.points[i + 1]), sh.color);
                break;
            case core::LogoShape::Kind::Ellipse: {
                const int seg = side > 40.0f ? 40 : 16;
                for (int i = 0; i < seg; ++i) {
                    const float a0 = 6.2831853f * i / seg, a1 = 6.2831853f * (i + 1) / seg;
                    ctx.triangle(at(sh.center), at(sh.center + sh.radii * glm::vec2(std::cos(a0), std::sin(a0))),
                                 at(sh.center + sh.radii * glm::vec2(std::cos(a1), std::sin(a1))), sh.color);
                }
                break;
            }
        }
    }
}

} // namespace editor
} // namespace toy
