/**
 * @file curve_math.h
 * @brief The maths behind the inspector's curve and gradient editors, with no GPU and no UI:
 *        sampling, picking, inserting, dragging and removing points.
 *
 * A curve is a list of points in the unit square, sorted by x, read as a piecewise-linear
 * function of x -- the shape a particle's size over its life, a tyre's grip against slip, or
 * a terrain brush's falloff is written with. A gradient is the same list with a colour at
 * each stop instead of a height. Both are stored in YAML as a sequence of small sequences
 * (`[[0, 0], [0.5, 1], [1, 0]]`), which is what the engine's parsers read.
 *
 * Everything here is a pure function over the point list and a rectangle, so the widget that
 * draws it stays a thin layer of imm calls and the behaviour is unit-tested without a window.
 */

#ifndef TOYEDITOR_UI_CURVE_MATH_H
#define TOYEDITOR_UI_CURVE_MATH_H

#include "../core/yaml_util.h"

#include <glm/glm.hpp>

#include <vector>

namespace toy::editor::ui {

/** @brief A rectangle in editor (imm) space: +Y DOWN from the top-left. */
struct CurveBox {
    float x = 0.0f, y = 0.0f, w = 1.0f, h = 1.0f;
};

/** @brief One stop of a gradient: a position in [0, 1] and the colour there. */
struct ColorStop {
    float t = 0.0f;
    glm::vec3 color{1.0f};
};

/** @brief Curve point (unit square, +Y up) -> a point in `box` (+Y down). */
glm::vec2 curve_to_box(const CurveBox& box, glm::vec2 p);

/** @brief A point in `box` -> a curve point, clamped into the unit square. */
glm::vec2 box_to_curve(const CurveBox& box, glm::vec2 p);

/**
 * @brief The curve's value at `x`, linearly interpolated.
 *
 * Outside the outermost points the curve holds their value rather than extrapolating, so a
 * short curve never flies off. An empty curve is 0 everywhere.
 */
float sample_curve(const std::vector<glm::vec2>& points, float x);

/** @brief The gradient's colour at `t`, linearly interpolated; black when it has no stops. */
glm::vec3 sample_gradient(const std::vector<ColorStop>& stops, float t);

/** @brief Index of the point within `radius` editor pixels of `mouse`, or -1; nearest wins. */
int pick_curve_point(const std::vector<glm::vec2>& points, const CurveBox& box, glm::vec2 mouse, float radius = 7.0f);

/** @brief Index of the stop whose handle is within `radius` pixels of `mouse.x`, or -1. */
int pick_gradient_stop(const std::vector<ColorStop>& stops, const CurveBox& box, glm::vec2 mouse, float radius = 7.0f);

/**
 * @brief Inserts a point at `p`, keeping the list sorted by x.
 * @return The index it landed at.
 */
int insert_curve_point(std::vector<glm::vec2>& points, glm::vec2 p);

/**
 * @brief Moves point `index` to `p`, clamped into the unit square and between its neighbours
 *        so the curve stays a function of x. The first and last points keep their x.
 * @return Whether anything moved.
 */
bool move_curve_point(std::vector<glm::vec2>& points, int index, glm::vec2 p);

/**
 * @brief Removes point `index`.
 *
 * A curve keeps at least two points -- one is not a curve, none would read as flat zero -- so
 * removing from a pair does nothing.
 * @return Whether a point was removed.
 */
bool remove_curve_point(std::vector<glm::vec2>& points, int index);

/** @brief Inserts a stop at `t` with the colour the gradient already has there. */
int insert_gradient_stop(std::vector<ColorStop>& stops, float t);

/** @brief Moves stop `index` to `t` and re-sorts; returns its new index. */
int move_gradient_stop(std::vector<ColorStop>& stops, int index, float t);

/** @brief Removes stop `index`, keeping at least two. */
bool remove_gradient_stop(std::vector<ColorStop>& stops, int index);

/** @brief `[[x, y], ...]` -> points. A malformed node reads as an empty curve. */
std::vector<glm::vec2> curve_from_node(const Node& n);

/** @brief Points -> `[[x, y], ...]`. */
Node curve_to_node(const std::vector<glm::vec2>& points);

/** @brief `[[t, r, g, b], ...]` -> stops. */
std::vector<ColorStop> gradient_from_node(const Node& n);

/** @brief Stops -> `[[t, r, g, b], ...]`. */
Node gradient_to_node(const std::vector<ColorStop>& stops);

} // namespace toy::editor::ui

#endif // TOYEDITOR_UI_CURVE_MATH_H
