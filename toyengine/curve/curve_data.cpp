/**
 * @file curve_data.cpp
 * @brief Cubic Bezier evaluation, arc-length sampling, and the handle rules.
 */

#include "toyengine/curve/curve_data.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

namespace toy {
namespace curve {
namespace {

/// @brief The four control values of `segment`: p0, its out-handle, p1's in-handle, p1.
bool segment_controls_(const CurveData& c, std::size_t segment, glm::vec3 out[4]) {
    if (segment >= c.segment_count()) return false;
    const std::size_t a = segment;
    const std::size_t b = (segment + 1) % c.points.size();
    out[0] = c.points[a].position;
    out[1] = c.points[a].out_handle;
    out[2] = c.points[b].in_handle;
    out[3] = c.points[b].position;
    return true;
}

glm::vec3 bezier_(const glm::vec3 p[4], float t) {
    const float u = 1.0f - t;
    return u * u * u * p[0] + 3.0f * u * u * t * p[1] + 3.0f * u * t * t * p[2] + t * t * t * p[3];
}

glm::vec3 bezier_derivative_(const glm::vec3 p[4], float t) {
    const float u = 1.0f - t;
    return 3.0f * u * u * (p[1] - p[0]) + 6.0f * u * t * (p[2] - p[1]) + 3.0f * t * t * (p[3] - p[2]);
}

/// @brief The neighbours of `i`, clamped on an open curve and wrapped on a closed one.
void neighbours_(const CurveData& c, std::size_t i, glm::vec3& before, glm::vec3& after) {
    const std::size_t n = c.points.size();
    const std::size_t prev = c.closed ? (i + n - 1) % n : (i == 0 ? 0 : i - 1);
    const std::size_t next = c.closed ? (i + 1) % n : std::min(i + 1, n - 1);
    before = c.points[prev].position;
    after  = c.points[next].position;
}

} // namespace

std::size_t CurveData::segment_count() const {
    if (points.size() < 2) return 0;
    return closed ? points.size() : points.size() - 1;
}

glm::vec3 evaluate_segment(const CurveData& c, std::size_t segment, float t) {
    glm::vec3 p[4];
    if (!segment_controls_(c, segment, p)) return glm::vec3(0.0f);
    return bezier_(p, std::clamp(t, 0.0f, 1.0f));
}

glm::vec3 tangent_at(const CurveData& c, std::size_t segment, float t) {
    glm::vec3 p[4];
    if (!segment_controls_(c, segment, p)) return glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 d = bezier_derivative_(p, std::clamp(t, 0.0f, 1.0f));
    // A segment whose handles sit exactly on their points has a vanishing derivative at the
    // ends; the chord is the right answer there, and never zero unless the points coincide.
    if (glm::dot(d, d) < 1e-12f) d = p[3] - p[0];
    const float len = glm::length(d);
    return len > 1e-8f ? d / len : glm::vec3(1.0f, 0.0f, 0.0f);
}

std::vector<glm::vec3> polyline(const CurveData& c) {
    std::vector<glm::vec3> out;
    const std::size_t segments = c.segment_count();
    if (segments == 0) {
        if (c.points.size() == 1) out.push_back(c.points[0].position);
        return out;
    }
    const int res = std::max(1, c.resolution);
    out.reserve(segments * static_cast<std::size_t>(res) + 1);
    for (std::size_t s = 0; s < segments; ++s) {
        for (int i = 0; i < res; ++i) {
            out.push_back(evaluate_segment(c, s, static_cast<float>(i) / static_cast<float>(res)));
        }
    }
    // The final sample: the last control point, or back to the first when closed.
    out.push_back(c.closed ? c.points[0].position : c.points.back().position);
    return out;
}

float length(const CurveData& c) {
    const std::vector<glm::vec3> line = polyline(c);
    float total = 0.0f;
    for (std::size_t i = 1; i < line.size(); ++i) total += glm::distance(line[i - 1], line[i]);
    return total;
}

std::vector<glm::vec3> points_along(const CurveData& c, int count,
                                    std::vector<glm::vec3>* out_tangents, float from, float to) {
    std::vector<glm::vec3> out;
    if (out_tangents) out_tangents->clear();
    if (count < 2 || c.empty()) return out;
    from = std::clamp(from, 0.0f, 1.0f);
    to   = std::clamp(to, 0.0f, 1.0f);
    if (to <= from) return out;

    const std::vector<glm::vec3> line = polyline(c);
    if (line.size() < 2) return out;

    // Cumulative length along the polyline, so a position can be looked up by distance.
    std::vector<float> run(line.size(), 0.0f);
    for (std::size_t i = 1; i < line.size(); ++i) {
        run[i] = run[i - 1] + glm::distance(line[i - 1], line[i]);
    }
    const float total = run.back();
    if (total < 1e-6f) return out;

    out.reserve(static_cast<std::size_t>(count));
    if (out_tangents) out_tangents->reserve(static_cast<std::size_t>(count));

    const bool whole = from <= 0.0f && to >= 1.0f;
    const float span_start = total * from;
    const float span = total * (to - from);
    // An untrimmed closed curve's last point would land on its first, so its step spans the
    // whole loop. A trim has two distinct ends, so it spreads across them like an open curve.
    const float step = span / static_cast<float>((c.closed && whole) ? count : count - 1);
    std::size_t seg = 1;
    for (int i = 0; i < count; ++i) {
        const float want = std::min(span_start + step * static_cast<float>(i), total);
        while (seg + 1 < line.size() && run[seg] < want) ++seg;
        const float span = run[seg] - run[seg - 1];
        const float f = span > 1e-8f ? (want - run[seg - 1]) / span : 0.0f;
        out.push_back(glm::mix(line[seg - 1], line[seg], f));
        if (out_tangents) {
            const glm::vec3 d = line[seg] - line[seg - 1];
            const float dl = glm::length(d);
            out_tangents->push_back(dl > 1e-8f ? d / dl : glm::vec3(1.0f, 0.0f, 0.0f));
        }
    }
    return out;
}

glm::quat frame_from_tangent(const glm::vec3& tangent, float tilt_degrees) {
    const float len = glm::length(tangent);
    const glm::vec3 fwd = len > 1e-8f ? tangent / len : glm::vec3(1.0f, 0.0f, 0.0f);
    // Z is up in this engine; a curve running straight up has no usable reference, so fall back
    // to +X there rather than producing a degenerate basis.
    glm::vec3 up(0.0f, 0.0f, 1.0f);
    if (std::abs(glm::dot(fwd, up)) > 0.999f) up = glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 right = glm::normalize(glm::cross(up, fwd));
    const glm::vec3 real_up = glm::cross(fwd, right);
    const glm::quat base = glm::quat_cast(glm::mat3(fwd, right, real_up));
    if (std::abs(tilt_degrees) < 1e-6f) return base;
    return base * glm::angleAxis(glm::radians(tilt_degrees), glm::vec3(1.0f, 0.0f, 0.0f));
}

std::vector<glm::quat> parallel_frames(const std::vector<glm::vec3>& tangents,
                                       float tilt_degrees) {
    std::vector<glm::quat> out;
    out.reserve(tangents.size());
    if (tangents.empty()) return out;

    const glm::quat roll = std::abs(tilt_degrees) < 1e-6f
        ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
        : glm::angleAxis(glm::radians(tilt_degrees), glm::vec3(1.0f, 0.0f, 0.0f));

    // The first frame has nothing to carry from, so it picks a roll the usual way.
    glm::quat frame = frame_from_tangent(tangents.front(), 0.0f);
    out.push_back(frame * roll);

    for (std::size_t i = 1; i < tangents.size(); ++i) {
        const glm::vec3 prev = glm::length(tangents[i - 1]) > 1e-8f
            ? glm::normalize(tangents[i - 1]) : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 now = glm::length(tangents[i]) > 1e-8f
            ? glm::normalize(tangents[i]) : prev;

        const float cosine = glm::clamp(glm::dot(prev, now), -1.0f, 1.0f);
        if (cosine < 0.9999f) {
            glm::vec3 axis = glm::cross(prev, now);
            const float len = glm::length(axis);
            if (len > 1e-8f) {
                // The minimal rotation prev -> now. A reversal (cosine ~ -1) has no unique axis,
                // which is why it is taken from the cross product rather than assumed.
                frame = glm::angleAxis(std::acos(cosine), axis / len) * frame;
            }
        }
        out.push_back(frame * roll);
    }
    return out;
}

void refresh_handles(CurveData& c) {
    const std::size_t n = c.points.size();
    for (std::size_t i = 0; i < n; ++i) {
        CurvePoint& p = c.points[i];
        if (p.mode != HandleMode::Vector && p.mode != HandleMode::Auto) continue;
        glm::vec3 before, after;
        neighbours_(c, i, before, after);
        if (p.mode == HandleMode::Vector) {
            // Straight to the neighbours: a third of the way, which is the cubic that draws a
            // line when BOTH ends are Vector.
            p.in_handle  = p.position + (before - p.position) / 3.0f;
            p.out_handle = p.position + (after - p.position) / 3.0f;
        } else {
            // Catmull-Rom tangent, scaled to a third of the chord so the curve passes smoothly.
            const glm::vec3 dir = (after - before) * (1.0f / 6.0f);
            p.in_handle  = p.position - dir;
            p.out_handle = p.position + dir;
        }
    }
}

std::size_t append_point(CurveData& c, const glm::vec3& position) {
    CurvePoint p;
    p.position = position;
    // Continue the way the curve was heading, so a freshly placed point is not a corner.
    glm::vec3 dir(1.0f, 0.0f, 0.0f);
    if (!c.points.empty()) {
        const glm::vec3 d = position - c.points.back().position;
        if (glm::dot(d, d) > 1e-12f) dir = glm::normalize(d);
    }
    const float reach = c.points.empty() ? 0.0f : glm::distance(position, c.points.back().position) / 3.0f;
    p.in_handle  = position - dir * reach;
    p.out_handle = position + dir * reach;
    c.points.push_back(p);
    refresh_handles(c);
    return c.points.size() - 1;
}

std::size_t insert_point(CurveData& c, std::size_t segment, float t) {
    glm::vec3 p[4];
    if (!segment_controls_(c, segment, p)) return c.points.size();
    t = std::clamp(t, 0.0f, 1.0f);

    // De Casteljau: the split reproduces the original curve exactly, so the shape does not move.
    const glm::vec3 a0 = glm::mix(p[0], p[1], t);
    const glm::vec3 a1 = glm::mix(p[1], p[2], t);
    const glm::vec3 a2 = glm::mix(p[2], p[3], t);
    const glm::vec3 b0 = glm::mix(a0, a1, t);
    const glm::vec3 b1 = glm::mix(a1, a2, t);
    const glm::vec3 mid = glm::mix(b0, b1, t);

    const std::size_t left  = segment;
    const std::size_t right = (segment + 1) % c.points.size();

    CurvePoint inserted;
    inserted.position   = mid;
    inserted.in_handle  = b0;
    inserted.out_handle = b1;
    inserted.mode       = HandleMode::Aligned;

    // The neighbours' facing handles shorten to the split's: that is what keeps the shape.
    c.points[left].out_handle = a0;
    c.points[right].in_handle = a2;

    const std::size_t at = segment + 1;
    c.points.insert(c.points.begin() + static_cast<std::ptrdiff_t>(at), inserted);
    return at;
}

bool remove_point(CurveData& c, std::size_t index) {
    if (index >= c.points.size()) return false;
    c.points.erase(c.points.begin() + static_cast<std::ptrdiff_t>(index));
    refresh_handles(c);
    return true;
}

void move_point(CurveData& c, std::size_t index, const glm::vec3& delta) {
    if (index >= c.points.size()) return;
    CurvePoint& p = c.points[index];
    p.position   += delta;
    p.in_handle  += delta;
    p.out_handle += delta;
    // The neighbours of an Auto point derive their handles from it, so they move too.
    refresh_handles(c);
}

void set_handle_mode(CurveData& c, std::size_t index, HandleMode mode) {
    if (index >= c.points.size()) return;
    CurvePoint& p = c.points[index];
    p.mode = mode;

    if (mode == HandleMode::Vector || mode == HandleMode::Auto) {
        refresh_handles(c);
        return;
    }
    if (mode != HandleMode::Aligned) return;   // Free: handles freeze where they are.

    // Aligned: mirror one handle's direction onto the other. Whichever handle already has a
    // length is the reference and keeps it; the other takes the opposite direction. When BOTH
    // are degenerate (an endpoint never dragged), the direction instead comes from the
    // neighbours -- the same tangent Auto would compute -- and both handles take that length,
    // so the point reads as smooth immediately rather than hiding a zero-length handle.
    const float in_len  = glm::length(p.in_handle - p.position);
    const float out_len = glm::length(p.out_handle - p.position);
    if (out_len > 1e-6f) {
        const glm::vec3 fwd = (p.out_handle - p.position) / out_len;
        p.in_handle = p.position - fwd * (in_len > 1e-6f ? in_len : out_len);
    } else if (in_len > 1e-6f) {
        const glm::vec3 fwd = (p.position - p.in_handle) / in_len;
        p.out_handle = p.position + fwd * in_len;
    } else {
        glm::vec3 before, after;
        neighbours_(c, index, before, after);
        const glm::vec3 span = after - before;
        const glm::vec3 fwd = glm::dot(span, span) > 1e-12f ? glm::normalize(span)
                                                              : glm::vec3(1.0f, 0.0f, 0.0f);
        const float reach = std::min(glm::distance(p.position, before), glm::distance(p.position, after)) / 3.0f;
        p.in_handle  = p.position - fwd * reach;
        p.out_handle = p.position + fwd * reach;
    }
}

} // namespace curve
} // namespace toy
