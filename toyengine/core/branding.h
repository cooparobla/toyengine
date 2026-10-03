/**
 * @file branding.h
 * @brief toyengine's name, version and logo -- one definition for every place that shows them.
 *
 * The logo is a toy building block: an isometric cube (yellow top, coral and teal sides) with
 * a round stud on top, on a deep-indigo rounded square. It is described once, as flat shapes in
 * a unit square (logo_shapes()), and that one description is
 *   - rasterized into RGBA pixels (rasterize_logo()) for the window / taskbar / Dock icon,
 *     which Engine sets at startup, and for the PNGs `toyengine_icon` exports; and
 *   - drawn as vector triangles by the editor (its top-left app button and About box).
 * Change a colour or a point here and every copy follows.
 */

#ifndef TOY_CORE_BRANDING_H
#define TOY_CORE_BRANDING_H

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace toy::core {

inline constexpr const char* kEngineName = "toyengine";
inline constexpr int kVersionMajor = 0;
inline constexpr int kVersionMinor = 1;
inline constexpr int kVersionPatch = 0;
inline constexpr const char* kVersionString = "0.1.0";

/** @brief One flat shape of the logo, in a unit square (x right, y down). */
struct LogoShape {
    enum class Kind { RoundRect, Polygon, Ellipse };
    Kind kind = Kind::Polygon;
    std::vector<glm::vec2> points;   ///< Polygon (convex, either winding).
    glm::vec2 center{0.5f};          ///< Ellipse centre.
    glm::vec2 radii{0.5f};           ///< Ellipse radii.
    glm::vec2 min{0.0f}, max{1.0f};  ///< RoundRect bounds.
    float corner = 0.0f;             ///< RoundRect corner radius.
    glm::vec4 color{1.0f};           ///< Straight-alpha RGBA (the top colour of a gradient).
    glm::vec4 color_bottom{-1.0f};   ///< RoundRect: a vertical gradient's bottom colour (x < 0 = solid).
};

namespace detail {
inline glm::vec4 hex_rgb(uint32_t rgb, float a = 1.0f) {
    return {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a};
}
}  // namespace detail

/** @brief The logo, back to front. */
inline const std::vector<LogoShape>& logo_shapes() {
    static const std::vector<LogoShape> shapes = [] {
        using detail::hex_rgb;
        using K = LogoShape::Kind;
        std::vector<LogoShape> s;

        // Background tile.
        LogoShape bg;
        bg.kind = K::RoundRect;
        bg.min = glm::vec2(0.04f);
        bg.max = glm::vec2(0.96f);
        bg.corner = 0.22f;
        bg.color = hex_rgb(0x3A2F6B);
        bg.color_bottom = hex_rgb(0x1B1638);
        s.push_back(bg);

        // The block: an isometric hexagon split into three faces.
        const glm::vec2 c(0.5f, 0.585f);
        const float r = 0.30f;
        const float cx = r * 0.8660254f;   // cos 30
        const glm::vec2 top = c + glm::vec2(0, -r), ur = c + glm::vec2(cx, -r * 0.5f), lr = c + glm::vec2(cx, r * 0.5f);
        const glm::vec2 bot = c + glm::vec2(0, r), ll = c + glm::vec2(-cx, r * 0.5f), ul = c + glm::vec2(-cx, -r * 0.5f);
        auto poly = [&](std::vector<glm::vec2> pts, uint32_t col) {
            LogoShape p;
            p.kind = K::Polygon;
            p.points = std::move(pts);
            p.color = hex_rgb(col);
            s.push_back(p);
        };
        poly({ul, c, bot, ll}, 0xFF6B4A);    // left face: coral
        poly({c, ur, lr, bot}, 0x2FB5C8);    // right face: teal
        poly({top, ur, c, ul}, 0xFFC845);    // top face: toy yellow

        // The stud: a short cylinder standing on the top face's centre.
        const glm::vec2 face_c = c + glm::vec2(0, -r * 0.5f);
        const float srx = r * 0.36f, sry = srx * 0.5773503f, lift = r * 0.20f;
        auto ellipse = [&](glm::vec2 at, uint32_t col) {
            LogoShape e;
            e.kind = K::Ellipse;
            e.center = at;
            e.radii = glm::vec2(srx, sry);
            e.color = hex_rgb(col);
            s.push_back(e);
        };
        ellipse(face_c, 0xD9962A);                                        // base (shadowed)
        poly({face_c + glm::vec2(-srx, 0), face_c + glm::vec2(-srx, -lift),
              face_c + glm::vec2(srx, -lift), face_c + glm::vec2(srx, 0)}, 0xE8A93A);   // side band
        ellipse(face_c + glm::vec2(0, -lift), 0xFFE08A);                  // top
        return s;
    }();
    return shapes;
}

namespace detail {
/** @brief Whether `p` (unit square) is inside shape `sh`. */
inline bool logo_inside(const LogoShape& sh, glm::vec2 p) {
    switch (sh.kind) {
        case LogoShape::Kind::RoundRect: {
            if (p.x < sh.min.x || p.y < sh.min.y || p.x > sh.max.x || p.y > sh.max.y) return false;
            const glm::vec2 inner_min = sh.min + glm::vec2(sh.corner), inner_max = sh.max - glm::vec2(sh.corner);
            const glm::vec2 q = glm::clamp(p, inner_min, inner_max);
            return glm::length(p - q) <= sh.corner;
        }
        case LogoShape::Kind::Ellipse: {
            const glm::vec2 d = (p - sh.center) / sh.radii;
            return glm::dot(d, d) <= 1.0f;
        }
        case LogoShape::Kind::Polygon: {
            int sign = 0;
            const size_t n = sh.points.size();
            for (size_t i = 0; i < n; ++i) {
                const glm::vec2 a = sh.points[i], b = sh.points[(i + 1) % n];
                const float cr = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
                if (std::abs(cr) < 1e-12f) continue;
                const int sg = cr > 0 ? 1 : -1;
                if (sign == 0) sign = sg;
                else if (sg != sign) return false;
            }
            return true;
        }
    }
    return false;
}
}  // namespace detail

/**
 * @brief The logo as `size` x `size` straight-alpha RGBA8 pixels (row 0 at the top),
 *        antialiased with 4x4 supersampling.
 */
inline std::vector<uint8_t> rasterize_logo(int size) {
    std::vector<uint8_t> out(static_cast<size_t>(size) * size * 4, 0);
    const auto& shapes = logo_shapes();
    constexpr int kSub = 4;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            glm::vec4 acc(0.0f);   // premultiplied, averaged over the subsamples
            for (int sy = 0; sy < kSub; ++sy) {
                for (int sx = 0; sx < kSub; ++sx) {
                    const glm::vec2 p((x + (sx + 0.5f) / kSub) / size, (y + (sy + 0.5f) / kSub) / size);
                    glm::vec4 c(0.0f);   // premultiplied "over" composite, back to front
                    for (const auto& sh : shapes) {
                        if (!detail::logo_inside(sh, p)) continue;
                        glm::vec4 col = sh.color;
                        if (sh.color_bottom.x >= 0.0f) {
                            const float t = std::clamp((p.y - sh.min.y) / std::max(1e-6f, sh.max.y - sh.min.y), 0.0f, 1.0f);
                            col = glm::mix(sh.color, sh.color_bottom, t);
                        }
                        const glm::vec4 pm(glm::vec3(col) * col.a, col.a);
                        c = pm + c * (1.0f - pm.a);
                    }
                    acc += c;
                }
            }
            acc /= static_cast<float>(kSub * kSub);
            const glm::vec3 rgb = acc.a > 1e-6f ? glm::vec3(acc) / acc.a : glm::vec3(0.0f);
            uint8_t* px = &out[(static_cast<size_t>(y) * size + x) * 4];
            for (int i = 0; i < 3; ++i) px[i] = static_cast<uint8_t>(std::lround(std::clamp(rgb[i], 0.0f, 1.0f) * 255.0f));
            px[3] = static_cast<uint8_t>(std::lround(std::clamp(acc.a, 0.0f, 1.0f) * 255.0f));
        }
    }
    return out;
}

}  // namespace toy::core

#endif  // TOY_CORE_BRANDING_H
