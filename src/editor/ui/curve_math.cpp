#include "editor/ui/curve_math.h"

#include <algorithm>
#include <cmath>

namespace toy {
namespace editor {
namespace ui {

namespace {

constexpr float kMinGap = 1e-4f;   ///< Keeps two points from sharing an x, which has no slope.

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

} // namespace

glm::vec2 curve_to_box(const CurveBox& box, glm::vec2 p) {
    return {box.x + clamp01(p.x) * box.w, box.y + (1.0f - clamp01(p.y)) * box.h};
}

glm::vec2 box_to_curve(const CurveBox& box, glm::vec2 p) {
    const float x = box.w > 0.0f ? (p.x - box.x) / box.w : 0.0f;
    const float y = box.h > 0.0f ? 1.0f - (p.y - box.y) / box.h : 0.0f;
    return {clamp01(x), clamp01(y)};
}

float sample_curve(const std::vector<glm::vec2>& points, float x) {
    if (points.empty()) return 0.0f;
    if (x <= points.front().x) return points.front().y;
    if (x >= points.back().x) return points.back().y;
    for (size_t i = 1; i < points.size(); ++i) {
        const glm::vec2& b = points[i];
        if (x > b.x) continue;
        const glm::vec2& a = points[i - 1];
        const float span = b.x - a.x;
        if (span <= 0.0f) return b.y;
        return a.y + (b.y - a.y) * ((x - a.x) / span);
    }
    return points.back().y;
}

glm::vec3 sample_gradient(const std::vector<ColorStop>& stops, float t) {
    if (stops.empty()) return glm::vec3(0.0f);
    if (t <= stops.front().t) return stops.front().color;
    if (t >= stops.back().t) return stops.back().color;
    for (size_t i = 1; i < stops.size(); ++i) {
        const ColorStop& b = stops[i];
        if (t > b.t) continue;
        const ColorStop& a = stops[i - 1];
        const float span = b.t - a.t;
        if (span <= 0.0f) return b.color;
        return a.color + (b.color - a.color) * ((t - a.t) / span);
    }
    return stops.back().color;
}

int pick_curve_point(const std::vector<glm::vec2>& points, const CurveBox& box, glm::vec2 mouse, float radius) {
    int best = -1;
    float best_d2 = radius * radius;
    for (size_t i = 0; i < points.size(); ++i) {
        const glm::vec2 at = curve_to_box(box, points[i]);
        const glm::vec2 d = at - mouse;
        const float d2 = d.x * d.x + d.y * d.y;
        if (d2 <= best_d2) {
            best_d2 = d2;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int pick_gradient_stop(const std::vector<ColorStop>& stops, const CurveBox& box, glm::vec2 mouse, float radius) {
    int best = -1;
    float best_d = radius;
    for (size_t i = 0; i < stops.size(); ++i) {
        const float x = box.x + clamp01(stops[i].t) * box.w;
        const float d = std::fabs(x - mouse.x);
        if (d <= best_d) {
            best_d = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int insert_curve_point(std::vector<glm::vec2>& points, glm::vec2 p) {
    const glm::vec2 q{clamp01(p.x), clamp01(p.y)};
    // A point dropped at the same x as an existing one goes after it, so clicking at the right
    // edge of the box gives you the new last point rather than one hidden behind the old one.
    size_t at = 0;
    while (at < points.size() && points[at].x <= q.x) ++at;
    points.insert(points.begin() + static_cast<std::ptrdiff_t>(at), q);
    return static_cast<int>(at);
}

bool move_curve_point(std::vector<glm::vec2>& points, int index, glm::vec2 p) {
    if (index < 0 || index >= static_cast<int>(points.size())) return false;
    const size_t i = static_cast<size_t>(index);
    const bool pinned = i == 0 || i + 1 == points.size();   // the ends keep their x: the curve spans [0, 1]
    float x = pinned ? points[i].x : clamp01(p.x);
    if (!pinned) {
        x = std::clamp(x, points[i - 1].x + kMinGap, points[i + 1].x - kMinGap);
    }
    const glm::vec2 moved{x, clamp01(p.y)};
    if (moved == points[i]) return false;
    points[i] = moved;
    return true;
}

bool remove_curve_point(std::vector<glm::vec2>& points, int index) {
    if (index < 0 || index >= static_cast<int>(points.size()) || points.size() <= 2) return false;
    points.erase(points.begin() + index);
    return true;
}

int insert_gradient_stop(std::vector<ColorStop>& stops, float t) {
    ColorStop s;
    s.t = clamp01(t);
    s.color = sample_gradient(stops, s.t);
    size_t at = 0;
    while (at < stops.size() && stops[at].t <= s.t) ++at;
    stops.insert(stops.begin() + static_cast<std::ptrdiff_t>(at), s);
    return static_cast<int>(at);
}

int move_gradient_stop(std::vector<ColorStop>& stops, int index, float t) {
    if (index < 0 || index >= static_cast<int>(stops.size())) return index;
    const ColorStop moved{clamp01(t), stops[static_cast<size_t>(index)].color};
    stops.erase(stops.begin() + index);
    size_t at = 0;
    while (at < stops.size() && stops[at].t <= moved.t) ++at;
    stops.insert(stops.begin() + static_cast<std::ptrdiff_t>(at), moved);
    return static_cast<int>(at);
}

bool remove_gradient_stop(std::vector<ColorStop>& stops, int index) {
    if (index < 0 || index >= static_cast<int>(stops.size()) || stops.size() <= 2) return false;
    stops.erase(stops.begin() + index);
    return true;
}

std::vector<glm::vec2> curve_from_node(const Node& n) {
    std::vector<glm::vec2> points;
    if (!n.is_sequence()) return points;
    for (const auto& e : n.as_seq()) {
        if (!e.is_sequence() || e.size() < 2) continue;
        points.push_back({clamp01(as_float(e.as_seq()[0])), clamp01(as_float(e.as_seq()[1]))});
    }
    std::stable_sort(points.begin(), points.end(), [](glm::vec2 a, glm::vec2 b) { return a.x < b.x; });
    return points;
}

Node curve_to_node(const std::vector<glm::vec2>& points) {
    Node out = Node::sequence();
    for (const glm::vec2& p : points) {
        const float v[2] = {p.x, p.y};
        out.as_seq().push_back(make_float_seq(v, 2));
    }
    return out;
}

std::vector<ColorStop> gradient_from_node(const Node& n) {
    std::vector<ColorStop> stops;
    if (!n.is_sequence()) return stops;
    for (const auto& e : n.as_seq()) {
        if (!e.is_sequence() || e.size() < 4) continue;
        ColorStop s;
        s.t = clamp01(as_float(e.as_seq()[0]));
        s.color = {as_float(e.as_seq()[1]), as_float(e.as_seq()[2]), as_float(e.as_seq()[3])};
        stops.push_back(s);
    }
    std::stable_sort(stops.begin(), stops.end(), [](const ColorStop& a, const ColorStop& b) { return a.t < b.t; });
    return stops;
}

Node gradient_to_node(const std::vector<ColorStop>& stops) {
    Node out = Node::sequence();
    for (const ColorStop& s : stops) {
        const float v[4] = {s.t, s.color.r, s.color.g, s.color.b};
        out.as_seq().push_back(make_float_seq(v, 4));
    }
    return out;
}

} // namespace ui
} // namespace editor
} // namespace toy
