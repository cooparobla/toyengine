/**
 * @file image.h
 * @brief A plain 8-bit interleaved pixel buffer, and the rasterisation
 *        primitives the map renderers draw into it with.
 */

#ifndef COOPA_MAPS_IMAGE_H
#define COOPA_MAPS_IMAGE_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/maps/map_data.h>

namespace coopa {
namespace maps {

/**
 * @struct Image
 * @brief An 8-bit-per-channel image with interleaved components, origin top-left.
 *
 * Deliberately not a texture: libcoopa carries no graphics dependency, so this
 * is a buffer a consumer can upload, encode or discard as it likes.
 */
struct Image {
    int width = 0;    /**< @brief Width in pixels. */
    int height = 0;   /**< @brief Height in pixels. */
    int channels = 3; /**< @brief Components per pixel; 3 for RGB. */
    std::vector<unsigned char> pixels; /**< @brief `width * height * channels` bytes. */

    /**
     * @brief Allocates the buffer and clears it to a colour.
     * @param w Width in pixels.
     * @param h Height in pixels.
     * @param c Components per pixel.
     * @param color Clear colour, in 0-255 component range.
     */
    void reset(int w, int h, int c, const glm::vec3& color) {
        reset(w, h, c, glm::vec4(color, 255.0f));
    }

    /**
     * @brief Allocates the buffer and clears it to a colour, alpha included.
     * @param w Width in pixels.
     * @param h Height in pixels.
     * @param c Components per pixel; 4 for RGBA.
     * @param color Clear colour, in 0-255 component range. An alpha of 0 on a
     *              4-channel image gives a fully transparent layer to draw onto.
     */
    void reset(int w, int h, int c, const glm::vec4& color) {
        width = w;
        height = h;
        channels = c;
        pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h)
                          * static_cast<std::size_t>(c), 0);
        fill(color);
    }

    /**
     * @brief Overwrites every pixel with one colour.
     * @param color The colour to fill with, in 0-255 component range.
     */
    void fill(const glm::vec3& color) {
        fill(glm::vec4(color, 255.0f));
    }

    /**
     * @brief Overwrites every pixel with one colour, alpha included.
     * @param color The colour to fill with, in 0-255 component range.
     */
    void fill(const glm::vec4& color) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                set_pixel(x, y, color);
            }
        }
    }

    /**
     * @brief Writes one pixel, ignoring coordinates outside the image.
     * @param x Column.
     * @param y Row.
     * @param color The colour to write, in 0-255 component range; clamped.
     */
    void set_pixel(int x, int y, const glm::vec3& color) {
        set_pixel(x, y, glm::vec4(color, 255.0f));
    }

    /**
     * @brief Writes one pixel including its alpha, ignoring coordinates outside the image.
     *
     * The overload that makes a 4-channel image usable at all. `reset()` zeroes
     * the buffer, so an RGBA layer starts fully transparent and only the marks
     * actually drawn onto it become opaque -- which is what lets the road,
     * structure and landmark layers be stacked over the terrain without carrying
     * a background of their own.
     *
     * @param x Column.
     * @param y Row.
     * @param color The colour to write, in 0-255 component range; clamped.
     *              The alpha component is dropped on a 3-channel image.
     */
    void set_pixel(int x, int y, const glm::vec4& color) {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return;
        }
        const std::size_t index =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
             + static_cast<std::size_t>(x)) * static_cast<std::size_t>(channels);
        pixels[index] = to_byte(color.r);
        if (channels > 1) pixels[index + 1] = to_byte(color.g);
        if (channels > 2) pixels[index + 2] = to_byte(color.b);
        if (channels > 3) pixels[index + 3] = to_byte(color.a);
    }

    /**
     * @brief Reads one pixel's alpha; 255 where the image has no alpha channel.
     * @param x Column.
     * @param y Row.
     * @return The alpha byte, or 0 for a coordinate outside the image.
     */
    unsigned char alpha_at(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return 0;
        }
        if (channels < 4) {
            return 255;
        }
        return pixels[((static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
                        + static_cast<std::size_t>(x)) * static_cast<std::size_t>(channels)) + 3];
    }

    /**
     * @brief Reads one pixel's colour, ignoring alpha.
     * @param x Column.
     * @param y Row.
     * @return The colour in 0-255 component range; black outside the image.
     */
    glm::vec3 color_at(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return glm::vec3(0.0f);
        }
        const std::size_t index =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
             + static_cast<std::size_t>(x)) * static_cast<std::size_t>(channels);
        return glm::vec3(static_cast<float>(pixels[index]),
                         channels > 1 ? static_cast<float>(pixels[index + 1]) : 0.0f,
                         channels > 2 ? static_cast<float>(pixels[index + 2]) : 0.0f);
    }

    /**
     * @brief Converts a 0-255 float component to a byte.
     *
     * Clamps rather than casting straight through: a palette entry outside the
     * range would otherwise wrap and produce a pixel of an unrelated colour.
     *
     * @param value The component value.
     * @return The clamped byte.
     */
    static unsigned char to_byte(float value) {
        return static_cast<unsigned char>(std::clamp(value, 0.0f, 255.0f));
    }
};

/**
 * @struct RowBand
 * @brief A range of image rows a draw call is confined to.
 *
 * What makes rendering one image on several threads safe *and* reproducible.
 * Splitting the work by shape instead would race: adjacent cells deliberately
 * share their boundary pixels, so two threads filling neighbouring cells write
 * the same pixel and which one lands last is down to the scheduler. Splitting by
 * rows gives every thread a disjoint slice of the buffer, and because each slice
 * replays the *same* draw sequence clipped to its own rows, the result is
 * identical to having drawn the whole thing on one thread.
 *
 * Defaulted everywhere, so an unthreaded call site never mentions it.
 */
struct RowBand {
    int begin = 0;                                  /**< @brief First row, inclusive. */
    int end = std::numeric_limits<int>::max();      /**< @brief Last row, exclusive. */

    /** @brief Clamps a row range to this band and to an image height. */
    void clamp(int& min_y, int& max_y, int height) const {
        min_y = std::max(min_y, std::max(0, begin));
        max_y = std::min(max_y, std::min(height - 1, end - 1));
    }

    /** @brief Whether a row range overlaps this band at all. */
    bool intersects(int min_y, int max_y) const { return max_y >= begin && min_y < end; }
};

/**
 * @brief Fills a triangle using half-space edge functions.
 *
 * Accepts either winding by testing the sign of the triangle's own area, and
 * includes pixels exactly on an edge so adjacent triangles of a fan leave no
 * seam between them.
 *
 * @param image The target buffer.
 * @param a First vertex, in pixel coordinates.
 * @param b Second vertex.
 * @param c Third vertex.
 * @tparam Color `glm::vec3` or `glm::vec4`; whatever `Image::set_pixel()` accepts.
 * @param color Fill colour, in 0-255 component range.
 * @param band Rows this call may touch; defaults to the whole image.
 */
template <typename Color>
inline void fill_triangle(Image& image, const MapPoint& a, const MapPoint& b, const MapPoint& c,
                          const Color& color, const RowBand& band = RowBand{}) {
    const auto edge_function = [](const MapPoint& p, const MapPoint& q, const MapPoint& r) {
        return (r.x - p.x) * (q.y - p.y) - (r.y - p.y) * (q.x - p.x);
    };

    const int min_x = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
    const int max_x = std::min(image.width - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
    int min_y = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
    int max_y = std::min(image.height - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
    band.clamp(min_y, max_y, image.height);

    const double area = edge_function(a, b, c);
    const bool positive_area = area > 0.0;

    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const MapPoint point{static_cast<double>(x), static_cast<double>(y)};
            const double w0 = edge_function(b, c, point);
            const double w1 = edge_function(c, a, point);
            const double w2 = edge_function(a, b, point);

            const bool on_edge = w0 == 0.0 || w1 == 0.0 || w2 == 0.0;
            const bool inside = positive_area ? (w0 > 0.0 && w1 > 0.0 && w2 > 0.0)
                                              : (w0 < 0.0 && w1 < 0.0 && w2 < 0.0);
            if (on_edge || inside) {
                image.set_pixel(x, y, color);
            }
        }
    }
}

/**
 * @brief Fills a convex polygon by fanning triangles out from its centroid.
 * @param image The target buffer.
 * @param vertices The polygon outline, already ordered, in pixel coordinates.
 * @param color Fill colour, in 0-255 component range.
 */
template <typename Color>
inline void fill_polygon(Image& image, const std::vector<MapPoint>& vertices,
                         const Color& color, const RowBand& band = RowBand{}) {
    if (vertices.size() < 3) {
        return;
    }
    double sum_x = 0.0;
    double sum_y = 0.0;
    for (const MapPoint& vertex : vertices) {
        sum_x += vertex.x;
        sum_y += vertex.y;
    }
    const double count = static_cast<double>(vertices.size());
    const MapPoint centroid{sum_x / count, sum_y / count};

    for (std::size_t i = 0; i < vertices.size(); ++i) {
        fill_triangle(image, centroid, vertices[i], vertices[(i + 1) % vertices.size()], color,
                      band);
    }
}

/**
 * @brief Fills a triangle, choosing each pixel's colour from a shader.
 * @tparam Shader Callable of `(double x, double y) -> glm::vec3`, in pixel coordinates.
 * @param image The target buffer.
 * @param a First vertex, in pixel coordinates.
 * @param b Second vertex.
 * @param c Third vertex.
 * @param shader Supplies the colour for each covered pixel.
 * @param band Rows this call may touch; defaults to the whole image.
 */
template <typename Shader>
inline void fill_triangle_shaded(Image& image, const MapPoint& a, const MapPoint& b,
                                 const MapPoint& c, Shader&& shader,
                                 const RowBand& band = RowBand{}) {
    const auto edge_function = [](const MapPoint& p, const MapPoint& q, const MapPoint& r) {
        return (r.x - p.x) * (q.y - p.y) - (r.y - p.y) * (q.x - p.x);
    };

    const int min_x = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
    const int max_x = std::min(image.width - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
    int min_y = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
    int max_y = std::min(image.height - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
    band.clamp(min_y, max_y, image.height);

    const double area = edge_function(a, b, c);
    const bool positive_area = area > 0.0;

    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const MapPoint point{static_cast<double>(x), static_cast<double>(y)};
            const double w0 = edge_function(b, c, point);
            const double w1 = edge_function(c, a, point);
            const double w2 = edge_function(a, b, point);

            const bool on_edge = w0 == 0.0 || w1 == 0.0 || w2 == 0.0;
            const bool inside = positive_area ? (w0 > 0.0 && w1 > 0.0 && w2 > 0.0)
                                              : (w0 < 0.0 && w1 < 0.0 && w2 < 0.0);
            if (on_edge || inside) {
                image.set_pixel(x, y, shader(point.x, point.y));
            }
        }
    }
}

/**
 * @brief Fills a convex polygon with a per-pixel shaded colour.
 *
 * The gradient counterpart to `fill_polygon()`. A heightmap flat-shaded per cell
 * shows the Voronoi tessellation rather than the terrain; sampling a height
 * function per pixel is what makes a continuous surface out of the same data.
 *
 * @tparam Shader Callable of `(double x, double y) -> glm::vec3`, in pixel coordinates.
 * @param image The target buffer.
 * @param vertices The polygon outline, already ordered, in pixel coordinates.
 * @param shader Supplies the colour for each covered pixel.
 */
template <typename Shader>
inline void fill_polygon_shaded(Image& image, const std::vector<MapPoint>& vertices,
                                Shader&& shader, const RowBand& band = RowBand{}) {
    if (vertices.size() < 3) {
        return;
    }
    double sum_x = 0.0;
    double sum_y = 0.0;
    for (const MapPoint& vertex : vertices) {
        sum_x += vertex.x;
        sum_y += vertex.y;
    }
    const double count = static_cast<double>(vertices.size());
    const MapPoint centroid{sum_x / count, sum_y / count};

    for (std::size_t i = 0; i < vertices.size(); ++i) {
        fill_triangle_shaded(image, centroid, vertices[i], vertices[(i + 1) % vertices.size()],
                             shader, band);
    }
}

/**
 * @brief Draws a line of a given half-width, by distance to the segment.
 *
 * Every pixel whose centre lies within `half_width` of the segment is painted.
 * Not a brush stamped along a rasterised path, because both kinds of brush get
 * the width wrong:
 *
 * - A **square** brush widens a line by up to sqrt(2) as it turns off the axes,
 *   so a diagonal 6 m road draws 8 m wide.
 * - A round brush stamped along an 8-connected path fixes that but introduces
 *   the opposite error: the path advances sqrt(2) of ground per step, so a
 *   diagonal covers sqrt(2) fewer pixels per unit length and draws 0.707 of the
 *   width it was asked for.
 *
 * A distance test has neither problem, because it never rasterises a path at
 * all -- the painted set is defined by the geometry. It also takes a *fractional*
 * half-width, so a 6 m road at one pixel to the metre is 6 pixels and not the
 * nearest odd number, which is what a pixel-centred brush is limited to.
 *
 * @tparam Color `glm::vec3` or `glm::vec4`; whatever `Image::set_pixel()` accepts.
 * @param image The target buffer.
 * @param x0 Start column.
 * @param y0 Start row.
 * @param x1 End column.
 * @param y1 End row.
 * @param half_width Half the stroke width in pixels; 0 draws a single-pixel line.
 * @param color Stroke colour, in 0-255 component range.
 * @param band Rows this call may touch; defaults to the whole image.
 */
template <typename Color>
inline void draw_line(Image& image, double x0, double y0, double x1, double y1, double half_width,
                      const Color& color, const RowBand& band = RowBand{}) {
    // A stroke of width W is exactly the ground within W/2 of its centreline --
    // the geometric definition, and the only one that is the same in every
    // direction. Rasterisation then takes any pixel whose *centre* falls in that
    // band, which for an axis-aligned stroke can include one extra row, since
    // pixel centres sit on integers and an even width cannot be centred on one.
    // Insetting by half a pixel to hide that instead makes the band itself a
    // pixel narrow, and then only axis-aligned strokes look right.
    const double reach = std::max(half_width, 0.0);
    const int min_x = std::max(0, static_cast<int>(std::floor(std::min(x0, x1) - reach)));
    const int max_x = std::min(image.width - 1, static_cast<int>(std::ceil(std::max(x0, x1) + reach)));
    int min_y = std::max(0, static_cast<int>(std::floor(std::min(y0, y1) - reach)));
    int max_y = std::min(image.height - 1, static_cast<int>(std::ceil(std::max(y0, y1) + reach)));
    band.clamp(min_y, max_y, image.height);

    const double dx = x1 - x0;
    const double dy = y1 - y0;
    const double length_squared = dx * dx + dy * dy;

    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            // Nearest point on the segment, clamped to its ends so the stroke
            // gets rounded caps rather than running on past them.
            double t = 0.0;
            if (length_squared > 0.0) {
                t = ((x - x0) * dx + (y - y0) * dy) / length_squared;
                t = std::clamp(t, 0.0, 1.0);
            }
            const double nearest_x = x0 + dx * t;
            const double nearest_y = y0 + dy * t;
            const double offset_x = x - nearest_x;
            const double offset_y = y - nearest_y;
            if (offset_x * offset_x + offset_y * offset_y <= reach * reach) {
                image.set_pixel(x, y, color);
            }
        }
    }
}

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_IMAGE_H
