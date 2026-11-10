/**
 * @file ik.h
 * @brief Inverse-kinematics math: an analytic two-bone solver with a pole vector and a soft
 *        reach limit, a FABRIK solver for chains of any length, and a clamped look-at. Pure
 *        glm -- no scene types -- so it is testable on its own; ik_components.h wraps it as
 *        TwoBoneIK / ChainIK / LookAtIK components.
 *
 * Everything here works on WORLD-space joint positions and returns WORLD-space rotation deltas
 * (pre-multiplied onto a bone's world rotation). to_local_delta() turns one into the new local
 * rotation of a bone whose parent has a given world rotation.
 */

#ifndef COOPA_ANIMATION_IK_H
#define COOPA_ANIMATION_IK_H

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace coopa {
namespace anim {
namespace ik {

/** @brief Shortest-arc rotation taking unit vector `from` onto unit vector `to` (any axis
 *         perpendicular to `from` when they are opposite). */
inline glm::quat rotation_between(const glm::vec3& from, const glm::vec3& to) {
    const float d = glm::dot(from, to);
    if (d >= 1.0f - 1e-7f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (d <= -1.0f + 1e-7f) {
        glm::vec3 axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), from);
        if (glm::dot(axis, axis) < 1e-8f) axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), from);
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    const glm::vec3 c = glm::cross(from, to);
    return glm::normalize(glm::quat(1.0f + d, c.x, c.y, c.z));
}

/** @brief Rotation part of a (possibly scaled) affine matrix. */
inline glm::quat rotation_of(const glm::mat4& m) {
    glm::mat3 r(m);
    for (int i = 0; i < 3; ++i) {
        const float len = glm::length(r[i]);
        if (len > 1e-8f) r[i] /= len;
    }
    return glm::normalize(glm::quat_cast(r));
}

/** @brief The new local rotation for a bone whose world rotation is pre-multiplied by
 *         `world_delta`, given its parent's world rotation (identity for a root). */
inline glm::quat to_local_delta(const glm::quat& parent_world, const glm::quat& local, const glm::quat& world_delta) {
    return glm::normalize(glm::inverse(parent_world) * world_delta * parent_world * local);
}

/**
 * @brief Softened reach distance: past `(1 - softness)` of the chain's full length the reach
 *        eases asymptotically toward full extension instead of snapping straight -- the knee
 *        or elbow never pops when the target drifts just out of range.
 * @param dist Distance from the chain root to the target.
 * @param chain Full chain length (upper + lower).
 * @param softness Fraction of `chain` the ease spans; 0 = a hard clamp.
 */
inline float soft_reach(float dist, float chain, float softness) {
    const float s = std::clamp(softness, 0.0f, 0.99f) * chain;
    const float knee = chain - s;
    if (s <= 1e-6f) return std::min(dist, chain * 0.99999f);
    if (dist <= knee) return dist;
    return knee + s * (1.0f - std::exp(-(dist - knee) / s));
}

/**
 * @struct TwoBoneResult
 * @brief World-space rotation deltas for the upper and lower bones, plus the solved joint
 *        positions (handy for tests and debug draw).
 */
struct TwoBoneResult {
    glm::quat upper_delta{1.0f, 0.0f, 0.0f, 0.0f};  ///< Pre-multiply onto the upper bone's world rotation.
    glm::quat lower_delta{1.0f, 0.0f, 0.0f, 0.0f};  ///< Pre-multiply onto the lower bone's world rotation
                                                    ///< AFTER the upper delta has been applied.
    glm::vec3 mid{0.0f};                            ///< Solved middle-joint (knee/elbow) position.
    glm::vec3 end{0.0f};                            ///< Solved end-effector position.
    bool      reached = false;                      ///< The end landed on the target (not clamped by reach).
};

/**
 * @brief Analytic two-bone IK (law of cosines).
 *
 * @param a Upper joint (hip / shoulder) world position.
 * @param b Middle joint (knee / elbow) world position.
 * @param c End effector (ankle / wrist) world position.
 * @param target Where the end effector should go.
 * @param pole World point the middle joint bends toward; ignored when !has_pole (the chain's
 *        current bend direction is kept instead).
 * @param softness See soft_reach().
 *
 * The middle joint is placed in the plane through a, target and the pole, on the pole's side.
 * Each bone then swings by the shortest arc onto its solved direction, so twist about the bone
 * axis is whatever the input pose had.
 */
inline TwoBoneResult solve_two_bone(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                                    const glm::vec3& target, const glm::vec3& pole, bool has_pole,
                                    float softness = 0.0f) {
    TwoBoneResult r;
    const float lab = glm::length(b - a);
    const float lbc = glm::length(c - b);
    r.mid = b;
    r.end = c;
    if (lab < 1e-6f || lbc < 1e-6f) return r;

    glm::vec3 to_t = target - a;
    float dist = glm::length(to_t);
    glm::vec3 dir;
    if (dist < 1e-6f) {
        dir = glm::length(c - a) > 1e-6f ? glm::normalize(c - a) : glm::vec3(0.0f, 0.0f, -1.0f);
        dist = 0.0f;
    } else {
        dir = to_t / dist;
    }
    const float chain = lab + lbc;
    const float min_reach = std::abs(lab - lbc) + 1e-4f;
    float reach = soft_reach(dist, chain, softness);
    reach = std::clamp(reach, min_reach, chain * 0.99999f);
    r.reached = std::abs(reach - dist) < 1e-4f;

    // Bend direction: the pole (or the current bend) made perpendicular to the reach line.
    glm::vec3 bend = (has_pole ? pole : b) - a;
    bend -= dir * glm::dot(bend, dir);
    if (glm::dot(bend, bend) < 1e-10f) {
        // Pole on the reach line (or a straight chain with no pole): any perpendicular.
        bend = glm::cross(dir, glm::vec3(1.0f, 0.0f, 0.0f));
        if (glm::dot(bend, bend) < 1e-8f) bend = glm::cross(dir, glm::vec3(0.0f, 1.0f, 0.0f));
    }
    bend = glm::normalize(bend);

    const float cos_a = std::clamp((lab * lab + reach * reach - lbc * lbc) / (2.0f * lab * reach), -1.0f, 1.0f);
    const float sin_a = std::sqrt(std::max(0.0f, 1.0f - cos_a * cos_a));
    r.mid = a + dir * (lab * cos_a) + bend * (lab * sin_a);
    r.end = a + dir * reach;

    r.upper_delta = rotation_between(glm::normalize(b - a), glm::normalize(r.mid - a));
    const glm::vec3 c_after = a + r.upper_delta * (c - a);   // the end, carried by the upper swing
    r.lower_delta = rotation_between(glm::normalize(c_after - r.mid), glm::normalize(r.end - r.mid));
    return r;
}

/**
 * @brief FABRIK (forward-and-backward reaching IK) over a chain of any length.
 *
 * @param joints World positions of the chain's joints, root first, the end effector last. On
 *        return, the solved positions: the root stays where it was, every segment keeps its
 *        length, and the end lies on the target -- or, out of reach, the chain points straight
 *        at it.
 * @param target Where the end effector should go.
 * @param iterations Forward/backward passes at most.
 * @param tolerance Stop once the end is this close to the target (world units).
 * @return Whether the end reached the target within `tolerance`.
 *
 * Starting from the input pose, FABRIK moves each joint the least it can, so a chain keeps the
 * bend it was animated with wherever the target leaves a choice -- a spine that leans forward
 * stays leaning forward rather than snapping to some canonical shape.
 */
inline bool solve_chain(std::vector<glm::vec3>& joints, const glm::vec3& target, int iterations = 10,
                        float tolerance = 1e-3f) {
    const size_t n = joints.size();
    if (n < 2) return false;
    std::vector<float> len(n - 1);
    float total = 0.0f;
    for (size_t i = 0; i + 1 < n; ++i) {
        len[i] = glm::length(joints[i + 1] - joints[i]);
        total += len[i];
    }
    const glm::vec3 root = joints[0];
    if (glm::length(target - root) >= total) {
        // Out of reach: straighten toward the target.
        glm::vec3 dir = target - root;
        dir = glm::dot(dir, dir) > 1e-12f ? glm::normalize(dir) : glm::vec3(0.0f, 0.0f, 1.0f);
        for (size_t i = 0; i + 1 < n; ++i) joints[i + 1] = joints[i] + dir * len[i];
        return false;
    }
    auto place = [](const glm::vec3& from, const glm::vec3& toward, float l) {
        const glm::vec3 d = toward - from;
        const float dl = glm::length(d);
        return dl > 1e-9f ? from + d * (l / dl) : from;
    };
    for (int it = 0; it < std::max(iterations, 1); ++it) {
        if (glm::length(joints[n - 1] - target) <= tolerance) return true;
        joints[n - 1] = target;                                       // backward: end to root
        for (size_t i = n - 1; i-- > 0;) joints[i] = place(joints[i + 1], joints[i], len[i]);
        joints[0] = root;                                             // forward: root to end
        for (size_t i = 0; i + 1 < n; ++i) joints[i + 1] = place(joints[i], joints[i + 1], len[i]);
    }
    return glm::length(joints[n - 1] - target) <= tolerance;
}

/**
 * @brief Rotates `from` toward `to` by at most `max_angle_rad` (both unit vectors).
 * @return The (unit) clamped direction.
 */
inline glm::vec3 clamp_direction(const glm::vec3& from, const glm::vec3& to, float max_angle_rad,
                                 const glm::vec3& fallback_axis = glm::vec3(0.0f, 0.0f, 1.0f)) {
    const float angle = std::acos(std::clamp(glm::dot(from, to), -1.0f, 1.0f));
    if (angle <= max_angle_rad) return to;
    glm::vec3 axis = glm::cross(from, to);
    if (glm::dot(axis, axis) < 1e-10f) {
        axis = fallback_axis - from * glm::dot(fallback_axis, from);
        if (glm::dot(axis, axis) < 1e-10f) axis = glm::cross(from, glm::vec3(1.0f, 0.0f, 0.0f));
        axis = glm::cross(from, glm::normalize(axis));   // turn toward the fallback axis
    }
    return glm::normalize(glm::angleAxis(max_angle_rad, glm::normalize(axis)) * from);
}

/**
 * @brief The world rotation delta that turns a bone's `forward` (world, unit) to look along
 *        `desired` (world, unit), at most `max_angle_rad` away from `forward`, while keeping the
 *        bone's `up` (world, unit) as close as possible to where it was -- so swinging the gaze
 *        diagonally doesn't roll the head.
 */
inline glm::quat look_at_delta(const glm::vec3& forward, const glm::vec3& up, const glm::vec3& desired,
                               float max_angle_rad) {
    const glm::vec3 d = clamp_direction(forward, desired, max_angle_rad, up);
    const glm::quat swing = rotation_between(forward, d);
    // Re-roll about d so the swung up axis lines up with the old up (projected off d).
    glm::vec3 want_up = up - d * glm::dot(up, d);
    glm::vec3 have_up = swing * up;
    have_up -= d * glm::dot(have_up, d);
    if (glm::dot(want_up, want_up) < 1e-8f || glm::dot(have_up, have_up) < 1e-8f) return swing;
    want_up = glm::normalize(want_up);
    have_up = glm::normalize(have_up);
    const float ang = std::atan2(glm::dot(glm::cross(have_up, want_up), d), glm::dot(have_up, want_up));
    return glm::normalize(glm::angleAxis(ang, d) * swing);
}

} // namespace ik
} // namespace anim
} // namespace coopa

#endif // COOPA_ANIMATION_IK_H
