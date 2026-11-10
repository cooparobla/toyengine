/**
 * @file imm_icons.h
 * @brief A Blender-style vector icon set for the immediate-mode layer.
 *
 * Every icon is drawn from lines, triangles and circles on a 16x16 design grid scaled to
 * the target box, so icons are crisp at any UI scale (Retina included) and need no image
 * assets. Icons are monochrome in the text colour by default, like Blender's; a few carry
 * fixed accent colours where Blender's do (light yellow, camera, object-type tints).
 *
 * Included at the end of imm.h; use through Context's icon-taking widgets
 * (icon_button(), menu_item(..., icon), tree_node(..., icon), ...) or draw_icon() directly.
 */

#ifndef UICOOPA_IMMEDIATE_IMM_ICONS_H
#define UICOOPA_IMMEDIATE_IMM_ICONS_H

#include <uicoopa/immediate/imm.h>

#include <cctype>
#include <cmath>
#include <string_view>
#include <utility>

namespace coopa {
namespace ui {
namespace imm {

namespace icon_detail {

/** @brief Maps 16x16 design-grid coordinates into a box. */
struct Pen {
    Context& ctx;
    glm::vec2 o;
    float s;
    glm::vec4 c;
    float t;

    Pen(Context& cx, const Box& b, const glm::vec4& col) : ctx(cx), c(col) {
        const float side = std::min(b.w, b.h);
        s = side / 16.0f;
        o = {b.x + (b.w - side) * 0.5f, b.y + (b.h - side) * 0.5f};
        t = std::max(1.0f, s * 1.15f);
    }
    glm::vec2 p(float x, float y) const { return o + glm::vec2(x, y) * s; }
    void l(float x0, float y0, float x1, float y1, float w = 1.0f) const { ctx.line(p(x0, y0), p(x1, y1), c, t * w); }
    void l(float x0, float y0, float x1, float y1, const glm::vec4& col, float w = 1.0f) const { ctx.line(p(x0, y0), p(x1, y1), col, t * w); }
    void tri(float x0, float y0, float x1, float y1, float x2, float y2) const { ctx.triangle(p(x0, y0), p(x1, y1), p(x2, y2), c); }
    void tri(float x0, float y0, float x1, float y1, float x2, float y2, const glm::vec4& col) const { ctx.triangle(p(x0, y0), p(x1, y1), p(x2, y2), col); }
    void rect(float x, float y, float w, float h) const { ctx.fill({p(x, y).x, p(x, y).y, w * s, h * s}, c); }
    void rect(float x, float y, float w, float h, const glm::vec4& col) const { ctx.fill({p(x, y).x, p(x, y).y, w * s, h * s}, col); }
    void box(float x, float y, float w, float h, float wt = 1.0f) const {
        l(x, y, x + w, y, wt); l(x + w, y, x + w, y + h, wt); l(x + w, y + h, x, y + h, wt); l(x, y + h, x, y, wt);
    }
    void circle(float cx, float cy, float r) const { ctx.circle(p(cx, cy), r * s, c); }
    void circle(float cx, float cy, float r, const glm::vec4& col) const { ctx.circle(p(cx, cy), r * s, col); }
    void ring(float cx, float cy, float r, float wt = 1.0f) const { ctx.ring(p(cx, cy), r * s, t * wt, c); }
    void ring(float cx, float cy, float r, const glm::vec4& col, float wt = 1.0f) const { ctx.ring(p(cx, cy), r * s, t * wt, col); }
    void arc(float cx, float cy, float r, float a0, float a1, float wt = 1.0f) const {
        const int n = 12;
        for (int i = 0; i < n; ++i) {
            const float u0 = a0 + (a1 - a0) * i / n, u1 = a0 + (a1 - a0) * (i + 1) / n;
            l(cx + r * std::cos(u0), cy + r * std::sin(u0), cx + r * std::cos(u1), cy + r * std::sin(u1), wt);
        }
    }
    void arrow_head(float x, float y, float dx, float dy, float size = 2.6f) const {
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-6f) return;
        dx /= len; dy /= len;
        const float nx = -dy, ny = dx;
        tri(x + dx * size, y + dy * size, x + nx * size * 0.7f, y + ny * size * 0.7f, x - nx * size * 0.7f, y - ny * size * 0.7f);
    }
};

inline const glm::vec4 kOrange{0.95f, 0.55f, 0.25f, 1.0f};
inline const glm::vec4 kYellow{1.0f, 0.86f, 0.35f, 1.0f};
inline const glm::vec4 kBlue{0.40f, 0.62f, 0.95f, 1.0f};
inline const glm::vec4 kGreen{0.45f, 0.80f, 0.40f, 1.0f};
inline const glm::vec4 kRed{0.92f, 0.36f, 0.36f, 1.0f};
inline const glm::vec4 kPurple{0.70f, 0.50f, 0.90f, 1.0f};

} // namespace icon_detail

/** @brief The icon's name, for tooltips and tests. */
inline const char* icon_name(Icon i) {
    switch (i) {
        case Icon::Cube: return "Mesh";
        case Icon::Camera: return "Camera";
        case Icon::Sun: return "Sun";
        default: return "";
    }
}

inline void draw_icon(Context& ctx, Icon icon, const Box& b, const glm::vec4& color) {
    using namespace icon_detail;
    if (icon == Icon::None) return;
    Pen d(ctx, b, color);
    const glm::vec4 dim = with_alpha(color, color.a * 0.45f);
    switch (icon) {
        case Icon::None: break;

        // --- object types ---
        case Icon::Cube:
            d.l(8, 2, 14, 5); d.l(14, 5, 8, 8); d.l(8, 8, 2, 5); d.l(2, 5, 8, 2);
            d.l(2, 5, 2, 11); d.l(2, 11, 8, 14); d.l(8, 14, 14, 11); d.l(14, 11, 14, 5); d.l(8, 8, 8, 14);
            break;
        case Icon::Mesh:   // Blender's mesh-data triangle with vertex dots
            d.l(3, 13, 8, 3); d.l(8, 3, 13, 13); d.l(13, 13, 3, 13);
            d.circle(3, 13, 1.6f); d.circle(8, 3, 1.6f); d.circle(13, 13, 1.6f);
            break;
        case Icon::Sphere:
            d.ring(8, 8, 6); d.arc(8, 8, 6, 0.0f, 3.14159f); d.l(2, 8, 14, 8, 0.7f);
            d.arc(8, 8.0f, 3.0f, -1.5708f, 1.5708f, 0.7f);
            break;
        case Icon::Cylinder:
            d.ring(8, 4, 1.0f, 0.0f);
            d.arc(8, 4, 5, 0, 6.2832f); d.l(3, 4, 3, 12); d.l(13, 4, 13, 12); d.arc(8, 12, 5, 0, 3.14159f);
            break;
        case Icon::Plane:
            d.l(2, 10, 8, 6); d.l(8, 6, 14, 10); d.l(14, 10, 8, 14); d.l(8, 14, 2, 10);
            break;
        case Icon::Empty:
            d.l(8, 2, 8, 14); d.l(2, 8, 14, 8); d.l(4, 12, 12, 4, 0.8f);
            break;
        case Icon::Sun:
            d.circle(8, 8, 3.0f, kYellow);
            for (int i = 0; i < 8; ++i) {
                const float a = i * 0.7854f;
                d.l(8 + 4.8f * std::cos(a), 8 + 4.8f * std::sin(a), 8 + 6.8f * std::cos(a), 8 + 6.8f * std::sin(a), kYellow);
            }
            break;
        case Icon::PointLight:
            d.circle(8, 7, 4.0f, kYellow);
            d.rect(6, 11, 4, 1.4f, color); d.rect(6.5f, 13, 3, 1.4f, color);
            break;
        case Icon::SpotLight:
            d.l(8, 2, 3, 12, kYellow); d.l(8, 2, 13, 12, kYellow); d.arc(8, 12, 5, 0.0f, 3.14159f);
            d.l(3, 12, 13, 12, kYellow, 0.6f); d.circle(8, 2.6f, 1.5f, kYellow);
            break;
        case Icon::EnvLight:
            d.arc(8, 13, 6.5f, 3.14159f, 6.2832f); d.l(1.5f, 13, 14.5f, 13);
            d.circle(8, 9, 2.0f, kYellow);
            break;
        case Icon::Camera:
            d.box(2, 5, 8, 7); d.tri(10, 8.5f, 14, 5.5f, 14, 11.5f); d.tri(4, 5, 8, 5, 6, 3);
            break;
        case Icon::Terrain:
            d.l(1, 13, 5, 7); d.l(5, 7, 8, 10); d.l(8, 10, 11, 5); d.l(11, 5, 15, 13); d.l(1, 13, 15, 13);
            break;
        case Icon::Object:
            d.rect(4, 4, 8, 8, kOrange);
            break;

        // --- data / properties tabs ---
        case Icon::Material:
            d.circle(8, 8, 6.0f, with_alpha(kRed, 0.85f));
            d.circle(6.5f, 6.5f, 2.6f, with_alpha(glm::vec4(1), 0.45f));
            d.ring(8, 8, 6, with_alpha(color, 0.6f), 0.6f);
            break;
        case Icon::Texture:
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) if ((x + y) % 2 == 0) d.rect(2 + x * 3, 2 + y * 3, 3, 3);
            d.box(2, 2, 12, 12, 0.7f);
            break;
        case Icon::World:
            d.ring(8, 8, 6); d.l(2, 8, 14, 8, 0.7f); d.arc(8, 8, 3.0f, -1.5708f, 1.5708f, 0.7f); d.arc(8, 8, 3.0f, 1.5708f, 4.7124f, 0.7f);
            d.l(3.5f, 5, 12.5f, 5, 0.6f); d.l(3.5f, 11, 12.5f, 11, 0.6f);
            break;
        case Icon::Scene:
            d.tri(8, 3, 3, 11, 13, 11, dim); d.circle(11.5f, 5, 1.8f); d.l(1, 13, 15, 13);
            d.l(3, 11, 8, 3); d.l(8, 3, 13, 11);
            break;
        case Icon::Collection:
            d.box(2, 5, 12, 8); d.l(2, 5, 4, 3); d.l(4, 3, 8, 3); d.l(8, 3, 9, 5);
            break;
        case Icon::Physics:
            d.ring(8, 8, 5.5f); d.circle(8, 8, 1.6f);
            d.arc(8, 8, 3.0f, 0.5f, 2.6f, 0.8f); d.arc(8, 8, 3.0f, 3.6f, 5.7f, 0.8f);
            break;
        case Icon::Collider:
            d.box(3, 3, 10, 10, 0.9f);
            for (int i = 0; i < 3; ++i) d.l(3 + i * 4, 13, 13, 3 + i * 4, 0.45f);
            break;
        case Icon::Rigidbody:
            d.ring(8, 6, 3.5f); d.l(8, 10, 8, 14); d.arrow_head(8, 14, 0, 1);
            break;
        case Icon::Component:   // puzzle piece (Unity component)
            d.box(3, 5, 8, 8); d.circle(7, 4.3f, 1.7f); d.circle(11.8f, 9, 1.7f);
            break;
        case Icon::Script:
            d.l(5, 4, 2, 8); d.l(2, 8, 5, 12); d.l(11, 4, 14, 8); d.l(14, 8, 11, 12); d.l(9.5f, 3, 6.5f, 13);
            break;
        case Icon::Render:   // Blender's render: camera back
            d.box(2, 4, 12, 9); d.ring(8, 8.5f, 2.8f); d.rect(3, 2.5f, 3, 1.5f);
            break;
        case Icon::Output:   // printer
            d.box(2, 6, 12, 6); d.box(4, 2, 8, 4, 0.8f); d.box(4, 10, 8, 4, 0.8f); d.circle(12, 8, 0.8f);
            break;
        case Icon::Tool:   // screwdriver
            d.l(3, 13, 9, 7, 1.6f); d.l(9, 7, 12, 4); d.rect(10.5f, 2, 3.5f, 3.5f);
            break;
        case Icon::Gear:
            d.ring(8, 8, 3.8f, 1.2f); d.circle(8, 8, 1.4f);
            for (int i = 0; i < 8; ++i) {
                const float a = i * 0.7854f;
                d.l(8 + 4.2f * std::cos(a), 8 + 4.2f * std::sin(a), 8 + 6.2f * std::cos(a), 8 + 6.2f * std::sin(a), 1.6f);
            }
            break;

        // --- visibility ---
        case Icon::Eye:
            d.arc(8, 13, 7.5f, 3.6f, 5.82f); d.arc(8, 3, 7.5f, 0.46f, 2.68f); d.circle(8, 8, 2.2f);
            break;
        case Icon::EyeClosed:
            d.arc(8, 3, 7.5f, 0.46f, 2.68f);
            d.l(4, 9.8f, 3, 12, 0.8f); d.l(8, 10.5f, 8, 13, 0.8f); d.l(12, 9.8f, 13, 12, 0.8f);
            break;
        case Icon::Monitor:
            d.box(2, 3, 12, 8); d.l(8, 11, 8, 13); d.l(5, 13.5f, 11, 13.5f);
            break;
        case Icon::MonitorOff:
            d.box(2, 3, 12, 8); d.l(8, 11, 8, 13); d.l(5, 13.5f, 11, 13.5f); d.l(2, 13, 14, 2, 1.0f);
            break;
        case Icon::Check:
            d.l(3, 8.5f, 6.5f, 12, 1.5f); d.l(6.5f, 12, 13, 4, 1.5f);
            break;
        case Icon::Lock:
            d.rect(3.5f, 7, 9, 7); d.arc(8, 7, 3.0f, 3.14159f, 6.2832f, 1.3f);
            break;

        // --- tools ---
        case Icon::SelectBox:
            for (int i = 0; i < 4; ++i) { d.l(2 + i * 3, 2, 3.5f + i * 3, 2); d.l(2 + i * 3, 11, 3.5f + i * 3, 11); }
            for (int i = 0; i < 3; ++i) { d.l(2, 2 + i * 3, 2, 3.5f + i * 3); d.l(11, 2 + i * 3, 11, 3.5f + i * 3); }
            d.tri(8, 7, 8, 15, 10.5f, 12.5f); d.tri(8, 7, 13.5f, 12, 10.5f, 12.5f);
            break;
        case Icon::Cursor:
            d.ring(8, 8, 4.5f, kRed); d.arc(8, 8, 4.5f, 0.0f, 1.57f, 1.0f); d.arc(8, 8, 4.5f, 3.14f, 4.71f, 1.0f);
            d.l(8, 1, 8, 4.5f); d.l(8, 11.5f, 8, 15); d.l(1, 8, 4.5f, 8); d.l(11.5f, 8, 15, 8);
            break;
        case Icon::Move:
            d.l(8, 2, 8, 14); d.l(2, 8, 14, 8);
            d.arrow_head(8, 2, 0, -1); d.arrow_head(8, 14, 0, 1); d.arrow_head(2, 8, -1, 0); d.arrow_head(14, 8, 1, 0);
            break;
        case Icon::Rotate:
            d.arc(8, 8, 5.5f, -2.6f, 2.2f, 1.2f); d.arrow_head(4.6f, 4.0f, -0.8f, 0.9f, 2.8f);
            break;
        case Icon::Scale:
            d.box(2, 7, 7, 7); d.l(7, 9, 13, 3); d.l(9, 3, 13, 3); d.l(13, 3, 13, 7);
            break;
        case Icon::Transform:
            d.ring(8, 8, 5.0f, 0.8f); d.l(8, 2, 8, 14, 0.8f); d.l(2, 8, 14, 8, 0.8f);
            d.arrow_head(8, 2, 0, -1, 2.2f); d.arrow_head(14, 8, 1, 0, 2.2f);
            break;
        case Icon::Extrude:
            d.l(2, 13, 14, 13); d.box(4, 7, 8, 6, 0.8f); d.l(8, 7, 8, 2); d.arrow_head(8, 2, 0, -1);
            break;
        case Icon::Inset:
            d.box(2, 2, 12, 12); d.box(5, 5, 6, 6, 0.8f);
            d.l(2, 2, 5, 5, 0.6f); d.l(14, 2, 11, 5, 0.6f); d.l(2, 14, 5, 11, 0.6f); d.l(14, 14, 11, 11, 0.6f);
            break;
        case Icon::Bevel:
            d.l(2, 14, 2, 6); d.l(2, 6, 6, 2); d.l(6, 2, 14, 2); d.l(4, 4, 7, 7, 0.6f); d.circle(4, 4, 1.0f);
            break;
        case Icon::Knife:
            d.l(2, 14, 12, 4, 1.4f); d.tri(12, 4, 14, 2, 14, 5);
            break;

        // --- edit modes / object modes ---
        case Icon::Vertex:
            d.box(3, 3, 10, 10, 0.5f); d.circle(3, 3, 2.0f); d.circle(13, 3, 1.3f, dim); d.circle(3, 13, 1.3f, dim); d.circle(13, 13, 1.3f, dim);
            break;
        case Icon::Edge:
            d.box(3, 3, 10, 10, 0.5f); d.l(3, 3, 3, 13, 1.6f);
            break;
        case Icon::Face:
            d.rect(3, 3, 10, 10, with_alpha(color, 0.55f)); d.box(3, 3, 10, 10, 0.8f);
            break;
        case Icon::ObjectMode:
            d.rect(4, 4, 8, 8, kOrange); d.box(4, 4, 8, 8, 0.8f);
            break;
        case Icon::LoopCut:   // a box cut by a yellow loop
            d.box(2, 3, 12, 10, 0.9f); d.l(2, 3, 5, 1, 0.8f); d.l(14, 3, 11, 1, 0.8f); d.l(5, 1, 11, 1, 0.8f);
            d.l(8, 3, 8, 13, kYellow, 1.2f); d.l(8, 3, 11, 1, kYellow, 1.0f);
            break;
        case Icon::SculptMode:   // a brush stroke over a bump
            d.arc(8, 13, 6, 3.14159f, 6.28318f, 1.0f); d.l(2, 13, 14, 13, 0.8f);
            d.l(9, 9, 14, 2, 1.4f); d.circle(8.6f, 9.6f, 1.6f, kOrange);
            break;
        case Icon::LocalView:   // a cube in a frame (Blender's Local View)
            d.box(5, 5, 6, 6, 0.9f);
            d.l(1, 4, 1, 1); d.l(1, 1, 4, 1); d.l(12, 1, 15, 1); d.l(15, 1, 15, 4);
            d.l(1, 12, 1, 15); d.l(1, 15, 4, 15); d.l(12, 15, 15, 15); d.l(15, 15, 15, 12);
            break;
        case Icon::BrushDraw:   // a raised stroke
            d.arc(8, 14, 6, 3.4f, 6.0f, 1.2f); d.l(1, 14, 15, 14, 0.7f); d.l(8, 3, 8, 6, kBlue, 1.0f); d.arrow_head(8, 2.5f, 0, -1, 2.2f);
            break;
        case Icon::BrushSmooth:   // a wave flattening out
            d.arc(4.5f, 9, 2.5f, 3.14159f, 6.28318f, 1.0f); d.arc(9.5f, 9, 2.5f, 0.0f, 3.14159f, 1.0f); d.l(12, 9, 15, 9);
            d.l(1, 13, 15, 13, 0.7f);
            break;
        case Icon::BrushInflate:   // a circle with outward arrows
            d.ring(8, 8, 3.5f); d.l(8, 3, 8, 1); d.l(8, 13, 8, 15); d.l(3, 8, 1, 8); d.l(13, 8, 15, 8);
            break;
        case Icon::BrushGrab:   // a hand-ish pinch: arrow pulling the surface
            d.arc(8, 15, 7, 3.6f, 5.8f, 1.0f); d.l(8, 8, 12, 3, 1.0f); d.arrow_head(12.5f, 2.5f, 0.7f, -0.7f, 2.4f); d.circle(8, 8.5f, 1.3f, kOrange);
            break;
        case Icon::BrushFlatten:   // a plane pressing down
            d.l(2, 6, 14, 6, 1.4f); d.l(8, 2, 8, 5); d.arrow_head(8, 5.5f, 0, 1, 2.0f); d.arc(8, 15, 6, 3.4f, 6.0f, 0.8f);
            break;
        case Icon::EditMode:
            d.box(3, 3, 10, 10, 0.8f); d.circle(3, 3, 1.6f); d.circle(13, 3, 1.6f); d.circle(3, 13, 1.6f); d.circle(13, 13, 1.6f);
            break;

        // --- viewport shading / overlays ---
        case Icon::ShadeWire:
            d.ring(8, 8, 6); d.arc(8, 8, 2.8f, -1.5708f, 1.5708f, 0.7f); d.arc(8, 8, 2.8f, 1.5708f, 4.7124f, 0.7f); d.l(2, 8, 14, 8, 0.7f);
            break;
        case Icon::ShadeSolid:
            d.circle(8, 8, 6.0f, color); d.circle(6.5f, 6.5f, 2.5f, with_alpha(glm::vec4(1), 0.5f));
            break;
        case Icon::ShadeMaterial:
            d.circle(8, 8, 6.0f, glm::vec4(0.75f, 0.45f, 0.35f, 1));
            d.circle(6.5f, 6.5f, 2.5f, with_alpha(glm::vec4(1), 0.55f));
            break;
        case Icon::ShadeRendered:
            d.circle(8, 8, 6.0f, glm::vec4(0.55f, 0.6f, 0.7f, 1));
            d.circle(10.2f, 10.2f, 3.4f, with_alpha(glm::vec4(0, 0, 0, 1), 0.35f));
            d.circle(6.0f, 6.0f, 2.2f, with_alpha(glm::vec4(1), 0.75f));
            break;
        case Icon::Overlays:
            d.ring(6, 8, 4.5f); d.ring(10, 8, 4.5f);
            break;
        case Icon::XRay:
            d.box(2, 2, 9, 9, 0.8f); d.rect(5, 5, 9, 9, with_alpha(color, 0.4f)); d.box(5, 5, 9, 9, 0.8f);
            break;
        case Icon::Grid:
            for (int i = 0; i < 4; ++i) { d.l(2 + i * 4, 2, 2 + i * 4, 14, 0.7f); d.l(2, 2 + i * 4, 14, 2 + i * 4, 0.7f); }
            break;
        case Icon::Snap:   // magnet
            d.arc(8, 7, 4.5f, 0.0f, 3.14159f, 2.0f); d.l(3.5f, 7, 3.5f, 3, 2.0f); d.l(12.5f, 7, 12.5f, 3, 2.0f);
            d.rect(2.4f, 2, 2.2f, 1.6f, kRed); d.rect(11.4f, 2, 2.2f, 1.6f, kBlue);
            break;
        case Icon::Pivot:
            d.circle(8, 8, 1.8f); d.ring(8, 8, 5.5f, 0.6f);
            break;
        case Icon::Orientation:
            d.l(4, 12, 13, 12); d.l(4, 12, 4, 3); d.l(4, 12, 10, 6, 0.8f);
            d.arrow_head(13, 12, 1, 0, 2.0f); d.arrow_head(4, 3, 0, -1, 2.0f);
            break;
        case Icon::ViewCamera:
            d.box(2, 5, 8, 7); d.tri(10, 8.5f, 14, 5.5f, 14, 11.5f);
            break;
        case Icon::Ortho:
            d.box(3, 3, 10, 10); d.box(5.5f, 5.5f, 5, 5, 0.6f);
            break;
        case Icon::Persp:
            d.l(1, 13, 6, 4); d.l(15, 13, 10, 4); d.l(1, 13, 15, 13); d.l(6, 4, 10, 4); d.l(3.5f, 8.5f, 12.5f, 8.5f, 0.6f);
            break;
        case Icon::Zoom:
            d.ring(6.5f, 6.5f, 4.5f); d.l(10, 10, 14, 14, 1.6f); d.l(4.5f, 6.5f, 8.5f, 6.5f); d.l(6.5f, 4.5f, 6.5f, 8.5f);
            break;
        case Icon::Hand:
            d.rect(4, 7, 8, 7); for (int i = 0; i < 4; ++i) d.rect(4 + i * 2.1f, 3, 1.6f, 5); d.rect(1.8f, 8, 2.4f, 3.5f);
            break;

        // --- playback (Unity-style) ---
        case Icon::Play: d.tri(4, 2.5f, 4, 13.5f, 13.5f, 8); break;
        case Icon::Pause: d.rect(4, 3, 3, 10); d.rect(9, 3, 3, 10); break;
        case Icon::Step: d.tri(3, 3, 3, 13, 10, 8); d.rect(11, 3, 2.4f, 10); break;
        case Icon::Stop: d.rect(3.5f, 3.5f, 9, 9); break;

        // --- files ---
        case Icon::Folder:
            d.rect(2, 5, 12, 8, with_alpha(color, 0.25f)); d.box(2, 5, 12, 8); d.l(2, 5, 3, 3); d.l(3, 3, 7, 3); d.l(7, 3, 8, 5);
            break;
        case Icon::File:
            d.l(4, 1.5f, 10, 1.5f); d.l(10, 1.5f, 13, 4.5f); d.l(13, 4.5f, 13, 14.5f); d.l(13, 14.5f, 4, 14.5f); d.l(4, 14.5f, 4, 1.5f);
            d.l(10, 1.5f, 10, 4.5f, 0.7f); d.l(10, 4.5f, 13, 4.5f, 0.7f);
            break;
        case Icon::Image:
            d.box(2, 3, 12, 10); d.tri(3, 12, 7, 7, 10, 12, dim); d.tri(7, 12, 10.5f, 8.5f, 13, 12, dim); d.circle(11, 6, 1.4f);
            break;
        case Icon::Search:
            d.ring(6.5f, 6.5f, 4.5f); d.l(10, 10, 14, 14, 1.6f);
            break;
        case Icon::Filter:
            d.tri(2, 3, 14, 3, 8, 9); d.rect(7, 9, 2, 5);
            break;
        case Icon::Asset:
            d.box(2, 2, 5, 5); d.box(9, 2, 5, 5); d.box(2, 9, 5, 5); d.rect(9, 9, 5, 5);
            break;
        case Icon::Console:
            d.box(1.5f, 2.5f, 13, 11); d.l(4, 6, 6.5f, 8); d.l(6.5f, 8, 4, 10); d.l(8, 10.5f, 12, 10.5f);
            break;
        case Icon::Save:
            d.box(2, 2, 12, 12); d.rect(4.5f, 2, 7, 4.5f, dim); d.box(4, 9, 8, 5, 0.7f);
            break;
        case Icon::Package:
            d.l(8, 2, 14, 5); d.l(14, 5, 8, 8); d.l(8, 8, 2, 5); d.l(2, 5, 8, 2); d.l(2, 5, 2, 11); d.l(2, 11, 8, 14);
            d.l(8, 14, 14, 11); d.l(14, 11, 14, 5); d.l(8, 8, 8, 14); d.l(5, 3.5f, 11, 6.5f, 0.6f);
            break;

        // --- actions ---
        case Icon::Plus: d.l(8, 3, 8, 13, 1.4f); d.l(3, 8, 13, 8, 1.4f); break;
        case Icon::X: d.l(4, 4, 12, 12, 1.3f); d.l(12, 4, 4, 12, 1.3f); break;
        case Icon::Trash:
            d.l(3, 4, 13, 4); d.l(6, 4, 6.5f, 2); d.l(6.5f, 2, 9.5f, 2); d.l(9.5f, 2, 10, 4);
            d.l(4, 4, 5, 14); d.l(5, 14, 11, 14); d.l(11, 14, 12, 4); d.l(7, 6.5f, 7, 12, 0.7f); d.l(9, 6.5f, 9, 12, 0.7f);
            break;
        case Icon::Duplicate: d.box(2, 5, 8, 8); d.box(6, 2, 8, 8, 0.8f); break;
        case Icon::Link: d.ring(5.5f, 10.5f, 3.0f); d.ring(10.5f, 5.5f, 3.0f); d.l(6.5f, 9.5f, 9.5f, 6.5f); break;
        case Icon::ArrowRight: d.tri(5.5f, 3.5f, 5.5f, 12.5f, 11.5f, 8); break;
        case Icon::ArrowDown: d.tri(3.5f, 5.5f, 12.5f, 5.5f, 8, 11.5f); break;
        case Icon::Dots: d.circle(8, 3.5f, 1.3f); d.circle(8, 8, 1.3f); d.circle(8, 12.5f, 1.3f); break;
        case Icon::Undo: d.arc(8, 9, 5, -3.1416f, 0.9f, 1.2f); d.arrow_head(3, 9, 0, 1, 2.6f); break;
        case Icon::Redo: d.arc(8, 9, 5, 2.24f, 6.28f, 1.2f); d.arrow_head(13, 9, 0, 1, 2.6f); break;
        case Icon::Restart: d.arc(8, 8, 5, 0.5f, 5.6f, 1.2f); d.arrow_head(12.5f, 5.0f, 0.4f, -1.0f, 2.6f); break;
        case Icon::Palette:   // painter's palette: a ring with paint dabs and a thumb hole
            d.arc(8, 8, 6.2f, 0.9f, 6.0f, 1.1f);
            d.l(8 + 6.2f * std::cos(0.9f), 8 + 6.2f * std::sin(0.9f), 9.5f, 11.0f, 1.1f);
            d.circle(5.0f, 6.0f, 1.3f, glm::vec4(0.95f, 0.36f, 0.33f, 1));
            d.circle(8.5f, 4.3f, 1.3f, glm::vec4(0.98f, 0.80f, 0.30f, 1));
            d.circle(11.6f, 6.4f, 1.3f, glm::vec4(0.36f, 0.66f, 0.98f, 1));
            d.circle(5.2f, 10.0f, 1.3f, glm::vec4(0.55f, 0.86f, 0.35f, 1));
            break;
        case Icon::Info: d.ring(8, 8, 6.2f); d.rect(7.2f, 7, 1.6f, 5); d.circle(8, 4.7f, 1.0f); break;
        case Icon::Warning:
            d.tri(8, 1.5f, 1, 14.5f, 15, 14.5f, kYellow); d.rect(7.2f, 5.5f, 1.6f, 5, glm::vec4(0.1f, 0.1f, 0.1f, 1));
            d.rect(7.2f, 11.6f, 1.6f, 1.6f, glm::vec4(0.1f, 0.1f, 0.1f, 1));
            break;
        case Icon::Error:
            d.circle(8, 8, 6.5f, kRed); d.l(5.2f, 5.2f, 10.8f, 10.8f, glm::vec4(1), 1.4f); d.l(10.8f, 5.2f, 5.2f, 10.8f, glm::vec4(1), 1.4f);
            break;

        // --- mouse hints (status bar) ---
        case Icon::MouseLeft:
        case Icon::MouseMiddle:
        case Icon::MouseRight:
            d.l(4.5f, 4, 4.5f, 11); d.l(11.5f, 4, 11.5f, 11); d.arc(8, 4.5f, 3.5f, 3.14159f, 6.2832f); d.arc(8, 11, 3.5f, 0.0f, 3.14159f);
            d.l(4.5f, 6.5f, 11.5f, 6.5f, 0.7f); d.l(8, 1, 8, 6.5f, 0.7f);
            if (icon == Icon::MouseLeft) d.rect(4.8f, 1.6f, 3.0f, 4.7f);
            else if (icon == Icon::MouseRight) d.rect(8.2f, 1.6f, 3.0f, 4.7f);
            else d.rect(7.2f, 2.2f, 1.6f, 3.6f);
            break;
        case Icon::Keyboard:
            d.box(1, 4, 14, 8); for (int i = 0; i < 4; ++i) d.rect(2.6f + i * 3, 5.6f, 1.8f, 1.6f); d.rect(4, 9, 8, 1.4f);
            break;
        case Icon::VertexPaint:   // a triangle with a different colour at each corner
            d.l(2, 13, 14, 13, 0.9f); d.l(14, 13, 8, 3, 0.9f); d.l(8, 3, 2, 13, 0.9f);
            d.circle(2.5f, 12.8f, 2.0f, kRed); d.circle(13.5f, 12.8f, 2.0f, kBlue); d.circle(8, 3.4f, 2.0f, kYellow);
            break;
        case Icon::WeightPaint:   // the blue-to-red weight ramp under a brush stroke
            d.rect(1, 10, 3.5f, 4, kBlue); d.rect(4.5f, 10, 3.5f, 4, kGreen); d.rect(8, 10, 3.5f, 4, kYellow);
            d.rect(11.5f, 10, 3.5f, 4, kRed); d.l(9, 7, 14, 2, 1.4f); d.circle(8.6f, 7.6f, 1.6f, kOrange);
            break;

        // --- UI design ---
        case Icon::UiCanvas:   // a screen with a HUD bar and a panel on it
            d.box(1.5f, 2.5f, 13, 11); d.rect(3, 4, 5, 1.6f, kRed); d.rect(9.5f, 8, 3.5f, 4, dim);
            break;
        case Icon::UiText:     // a serif T
            d.l(3, 3, 13, 3, 1.4f); d.l(8, 3, 8, 13, 1.4f); d.l(6, 13, 10, 13);
            break;
        case Icon::UiButton:   // a rounded pill with a label line
            d.box(1.5f, 5, 13, 6); d.l(5, 8, 11, 8, 1.2f);
            break;
        case Icon::UiLayout:   // stacked rows
            d.rect(2, 2.5f, 12, 3); d.rect(2, 6.5f, 12, 3, dim); d.rect(2, 10.5f, 12, 3);
            break;
        case Icon::UiWidget:   // a window with a title bar
            d.box(2, 3, 12, 10); d.rect(2, 3, 12, 2.6f, dim); d.l(4, 9, 12, 9, 0.8f); d.l(4, 11, 9, 11, 0.8f);
            break;
        case Icon::UiAnchor:   // four anchor petals around a centre
            d.tri(8, 2, 6.5f, 5, 9.5f, 5); d.tri(8, 14, 6.5f, 11, 9.5f, 11);
            d.tri(2, 8, 5, 6.5f, 5, 9.5f); d.tri(14, 8, 11, 6.5f, 11, 9.5f); d.box(5.5f, 5.5f, 5, 5, 0.8f);
            break;
        case Icon::Tag:   // a luggage tag pointing up-left, with its eyelet
            d.l(2, 2, 8, 2); d.l(8, 2, 14, 8); d.l(14, 8, 8, 14); d.l(8, 14, 2, 8); d.l(2, 8, 2, 2);
            d.circle(5, 5, 1.3f);
            break;
        case Icon::Proportional:   // proportional editing: a falloff ring around the selection
            d.ring(8, 8, 6.2f); d.ring(8, 8, 3.6f, dim, 0.8f); d.circle(8, 8, 1.5f);
            break;
        case Icon::Sort:   // bars shortening downward, beside a down arrow
            d.l(2, 3.5f, 9, 3.5f); d.l(2, 7, 7, 7); d.l(2, 10.5f, 5, 10.5f);
            d.l(12, 2.5f, 12, 12); d.tri(9.5f, 11, 14.5f, 11, 12, 14);
            break;
        case Icon::Grip:   // three equal bars: the drag handle of a reorderable row
            d.l(4, 4.5f, 12, 4.5f); d.l(4, 8, 12, 8); d.l(4, 11.5f, 12, 11.5f);
            break;
        case Icon::Pen:    // a nib on a shaft, over the curve it draws
            d.l(9.5f, 2.5f, 13, 6); d.l(9.5f, 2.5f, 6, 6); d.l(6, 6, 9.5f, 9.5f);
            d.l(13, 6, 9.5f, 9.5f); d.l(9.5f, 6.5f, 9.5f, 9.5f);
            d.l(2.5f, 13.5f, 6, 11.5f); d.l(6, 11.5f, 10, 13.5f); d.l(10, 13.5f, 13.5f, 11);
            break;

        // --- curve anchor modes: a square anchor is a corner, a round one is smooth, matching
        //     Illustrator/Photoshop's own convention; the handles either bend (Corner) or run
        //     straight through (Smooth/Vector), and Auto gets a curl no other mode has. ---
        case Icon::AnchorCorner:
            d.box(6.5f, 6.5f, 3, 3); d.l(8, 8, 13, 3); d.l(8, 8, 13, 12);
            d.circle(13, 3, 1.2f, dim); d.circle(13, 12, 1.2f, dim);
            break;
        case Icon::AnchorSmooth:
            d.circle(8, 8, 1.8f); d.l(2, 8, 14, 8);
            d.circle(2, 8, 1.2f, dim); d.circle(14, 8, 1.2f, dim);
            break;
        case Icon::AnchorVector:
            d.box(6.5f, 6.5f, 3, 3); d.l(2.5f, 13, 13, 2.5f);
            d.circle(2.5f, 13, 1.2f, dim); d.circle(13, 2.5f, 1.2f, dim);
            break;
        case Icon::AnchorAuto:   // an S-curve: the shape this mode derives on its own
            d.circle(8, 8, 1.8f);
            d.arc(6, 8, 3.0f, -1.5708f, 1.5708f); d.arc(10, 8, 3.0f, 1.5708f, 4.7124f);
            break;
    }
}

/**
 * @brief The Icon whose enumerator spells `name`, matched without regard to case, or
 *        Icon::None for an empty or unknown name.
 *
 * Lets data that must not depend on this header -- a schema table, a YAML file -- choose an
 * icon by name.
 */
inline Icon icon_from_name(std::string_view name) {
    static const std::pair<const char*, Icon> table[] = {
        {"Cube", Icon::Cube}, {"Mesh", Icon::Mesh}, {"Sphere", Icon::Sphere}, {"Cylinder", Icon::Cylinder},
        {"Plane", Icon::Plane}, {"Empty", Icon::Empty}, {"Sun", Icon::Sun}, {"PointLight", Icon::PointLight},
        {"SpotLight", Icon::SpotLight}, {"EnvLight", Icon::EnvLight}, {"Camera", Icon::Camera},
        {"Terrain", Icon::Terrain}, {"Object", Icon::Object}, {"Material", Icon::Material},
        {"Texture", Icon::Texture}, {"World", Icon::World}, {"Scene", Icon::Scene},
        {"Collection", Icon::Collection}, {"Physics", Icon::Physics}, {"Collider", Icon::Collider},
        {"Rigidbody", Icon::Rigidbody}, {"Component", Icon::Component}, {"Script", Icon::Script},
        {"Render", Icon::Render}, {"Output", Icon::Output}, {"Tool", Icon::Tool}, {"Gear", Icon::Gear},
        {"Eye", Icon::Eye}, {"EyeClosed", Icon::EyeClosed}, {"Monitor", Icon::Monitor},
        {"MonitorOff", Icon::MonitorOff}, {"Check", Icon::Check}, {"Lock", Icon::Lock},
        {"SelectBox", Icon::SelectBox}, {"Cursor", Icon::Cursor}, {"Move", Icon::Move}, {"Rotate", Icon::Rotate},
        {"Scale", Icon::Scale}, {"Transform", Icon::Transform}, {"Extrude", Icon::Extrude},
        {"Inset", Icon::Inset}, {"Bevel", Icon::Bevel}, {"Knife", Icon::Knife}, {"LoopCut", Icon::LoopCut},
        {"Vertex", Icon::Vertex}, {"Edge", Icon::Edge}, {"Face", Icon::Face}, {"ObjectMode", Icon::ObjectMode},
        {"EditMode", Icon::EditMode}, {"SculptMode", Icon::SculptMode}, {"LocalView", Icon::LocalView},
        {"BrushDraw", Icon::BrushDraw}, {"BrushSmooth", Icon::BrushSmooth}, {"BrushInflate", Icon::BrushInflate},
        {"BrushGrab", Icon::BrushGrab}, {"BrushFlatten", Icon::BrushFlatten}, {"ShadeWire", Icon::ShadeWire},
        {"ShadeSolid", Icon::ShadeSolid}, {"ShadeMaterial", Icon::ShadeMaterial},
        {"ShadeRendered", Icon::ShadeRendered}, {"Overlays", Icon::Overlays}, {"XRay", Icon::XRay},
        {"Grid", Icon::Grid}, {"Snap", Icon::Snap}, {"Pivot", Icon::Pivot}, {"Orientation", Icon::Orientation},
        {"ViewCamera", Icon::ViewCamera}, {"Ortho", Icon::Ortho}, {"Persp", Icon::Persp}, {"Zoom", Icon::Zoom},
        {"Hand", Icon::Hand}, {"Play", Icon::Play}, {"Pause", Icon::Pause}, {"Step", Icon::Step},
        {"Stop", Icon::Stop}, {"Folder", Icon::Folder}, {"File", Icon::File}, {"Image", Icon::Image},
        {"Search", Icon::Search}, {"Filter", Icon::Filter}, {"Asset", Icon::Asset}, {"Console", Icon::Console},
        {"Save", Icon::Save}, {"Package", Icon::Package}, {"Plus", Icon::Plus}, {"X", Icon::X},
        {"Trash", Icon::Trash}, {"Duplicate", Icon::Duplicate}, {"Link", Icon::Link},
        {"ArrowRight", Icon::ArrowRight}, {"ArrowDown", Icon::ArrowDown}, {"Dots", Icon::Dots},
        {"Undo", Icon::Undo}, {"Redo", Icon::Redo}, {"Restart", Icon::Restart}, {"Info", Icon::Info},
        {"Warning", Icon::Warning}, {"Error", Icon::Error}, {"Palette", Icon::Palette},
        {"MouseLeft", Icon::MouseLeft}, {"MouseMiddle", Icon::MouseMiddle}, {"MouseRight", Icon::MouseRight},
        {"Keyboard", Icon::Keyboard}, {"VertexPaint", Icon::VertexPaint}, {"WeightPaint", Icon::WeightPaint},
        {"UiCanvas", Icon::UiCanvas}, {"UiText", Icon::UiText}, {"UiButton", Icon::UiButton},
        {"UiLayout", Icon::UiLayout}, {"UiWidget", Icon::UiWidget}, {"UiAnchor", Icon::UiAnchor},
        {"Tag", Icon::Tag}, {"Sort", Icon::Sort}, {"Proportional", Icon::Proportional},
        {"Grip", Icon::Grip}, {"Pen", Icon::Pen},
    };
    for (const auto& [n, icon] : table) {
        std::string_view other(n);
        if (other.size() != name.size()) continue;
        bool same = true;
        for (size_t i = 0; i < name.size() && same; ++i) {
            same = std::tolower(static_cast<unsigned char>(name[i])) == std::tolower(static_cast<unsigned char>(other[i]));
        }
        if (same) return icon;
    }
    return Icon::None;
}

} // namespace imm
} // namespace ui
} // namespace coopa

#endif // UICOOPA_IMMEDIATE_IMM_ICONS_H
