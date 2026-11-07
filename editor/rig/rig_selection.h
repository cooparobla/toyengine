/**
 * @file rig_selection.h
 * @brief Rig Edit mode's bone selection, and the transform that moves what it selects -- pure
 *        functions over a RigModel, no EditorApp, no UI types, no GPU.
 *
 * Blender's edit-armature selection model: a bone has THREE selectable parts (its head, its
 * tail, and its body), and the first two are independent flags rather than one "bone selected"
 * bit -- which is what makes `G` on a tail alone lengthen a bone instead of sliding it. On top
 * of that sits the ACTIVE bone: the last bone a click touched, which operators read as "the one
 * to align/parent/fill to" and which the sidebar shows. The active bone may be unselected (shift
 * clicking it off leaves it active), so it is an index, not a flag.
 *
 * The parallel-vector layout (one entry per bone, same order as RigModel::bones) rather than a
 * set of (bone, end) pairs is deliberate: every consumer asks a per-bone question -- "does this
 * bone's tail move?", "draw this one highlighted?" -- bone counts are small, and keeping the
 * selection consistent after the bone list changes is then a resize rather than a rebuild.
 */

#ifndef TOYENGINE_EDITOR_RIG_RIG_SELECTION_H
#define TOYENGINE_EDITOR_RIG_RIG_SELECTION_H

#include "bone_pick.h"
#include "rig_model.h"

#include <glm/glm.hpp>

#include <vector>

namespace toy {
namespace editor {
namespace rig_sel {

/** @brief Which of one bone's two endpoints are selected (Blender's BONE_ROOTSEL / BONE_TIPSEL). */
struct BoneEndpointSelection {
    bool head = false;
    bool tail = false;
    bool any() const { return head || tail; }
    bool both() const { return head && tail; }
    bool operator==(const BoneEndpointSelection& o) const { return head == o.head && tail == o.tail; }
};

/** @brief The whole selection: per-bone endpoint flags plus the active bone (-1 = none). */
struct RigSelection {
    std::vector<BoneEndpointSelection> bones;
    int active = -1;
};

// --- lifecycle -------------------------------------------------------------------------------

/** @brief Resizes to `m` (new bones start unselected) and drops `active` if it fell off the end.
 *         Call after EVERY model change -- add, delete, undo -- or held indices dangle. */
inline void validate(RigSelection& sel, const RigModel& m) {
    sel.bones.resize(m.bones.size());
    if (sel.active >= static_cast<int>(m.bones.size())) sel.active = -1;
}

/** @brief Moves each entry to its new index after an op reordered the bone list.
 *         `old_to_new[i] < 0` means bone `i` was deleted. */
inline void remap(RigSelection& sel, const std::vector<int>& old_to_new) {
    std::vector<BoneEndpointSelection> out(sel.bones.size());
    for (std::size_t i = 0; i < sel.bones.size() && i < old_to_new.size(); ++i) {
        const int to = old_to_new[i];
        if (to >= 0 && static_cast<std::size_t>(to) < out.size()) out[static_cast<std::size_t>(to)] = sel.bones[i];
    }
    sel.bones = std::move(out);
    if (sel.active >= 0 && static_cast<std::size_t>(sel.active) < old_to_new.size()) sel.active = old_to_new[static_cast<std::size_t>(sel.active)];
    else sel.active = -1;
}

// --- queries ---------------------------------------------------------------------------------

inline bool in_range(const RigSelection& sel, int bone) { return bone >= 0 && static_cast<std::size_t>(bone) < sel.bones.size(); }
/** @brief Either endpoint selected. */
inline bool is_selected(const RigSelection& sel, int bone) { return in_range(sel, bone) && sel.bones[static_cast<std::size_t>(bone)].any(); }
/** @brief BOTH endpoints -- Blender's "this bone is selected", what whole-bone ops act on. */
inline bool is_fully_selected(const RigSelection& sel, int bone) {
    return in_range(sel, bone) && sel.bones[static_cast<std::size_t>(bone)].both();
}
inline bool empty(const RigSelection& sel) {
    for (const auto& b : sel.bones) if (b.any()) return false;
    return true;
}
/** @brief Every bone with at least one endpoint selected, ascending. */
inline std::vector<int> selected_bones(const RigSelection& sel) {
    std::vector<int> out;
    for (std::size_t i = 0; i < sel.bones.size(); ++i) if (sel.bones[i].any()) out.push_back(static_cast<int>(i));
    return out;
}
/** @brief Every FULLY selected bone, ascending -- Subdivide / Symmetrize / Delete's input. */
inline std::vector<int> fully_selected_bones(const RigSelection& sel) {
    std::vector<int> out;
    for (std::size_t i = 0; i < sel.bones.size(); ++i) if (sel.bones[i].both()) out.push_back(static_cast<int>(i));
    return out;
}

/** @brief One selected endpoint with its rig-space position. */
struct SelectedEndpoint {
    int bone = -1;
    rig_pick::BoneEndpoint end = rig_pick::BoneEndpoint::Head;
    glm::vec3 pos{0.0f};
};

/** @brief Every selected endpoint, bones ascending, head before tail -- the write list a
 *         transform rewrites, and what the median pivot averages. */
inline std::vector<SelectedEndpoint> selected_endpoints(const RigModel& m, const RigSelection& sel) {
    std::vector<SelectedEndpoint> out;
    const std::size_t n = std::min(m.bones.size(), sel.bones.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (sel.bones[i].head) out.push_back({static_cast<int>(i), rig_pick::BoneEndpoint::Head, m.bones[i].head});
        if (sel.bones[i].tail) out.push_back({static_cast<int>(i), rig_pick::BoneEndpoint::Tail, m.bones[i].tail});
    }
    return out;
}

/**
 * @brief Blender's Median Point over the selected endpoints; (0,0,0) when nothing is selected.
 *
 * NOT deduplicated across a connected bone's head and its parent's tail: Blender holds those as
 * two points and lets the median count both, so matching its pivot means counting both too.
 */
inline glm::vec3 selection_median(const RigModel& m, const RigSelection& sel) {
    const auto pts = selected_endpoints(m, sel);
    if (pts.empty()) return glm::vec3(0.0f);
    glm::vec3 sum(0.0f);
    for (const auto& p : pts) sum += p.pos;
    return sum / static_cast<float>(pts.size());
}

// --- select all / none / invert (A, Alt+A, Ctrl+I) --------------------------------------------

inline void select_all(RigSelection& sel, const RigModel& m) {
    validate(sel, m);
    for (auto& b : sel.bones) { b.head = b.tail = true; }
}
/** @brief Leaves `active` alone -- Blender keeps an active-but-unselected bone. */
inline void select_none(RigSelection& sel) {
    for (auto& b : sel.bones) { b.head = b.tail = false; }
}
/** @brief Per ENDPOINT, not per bone: a half-selected bone inverts to its other half. */
inline void invert_selection(RigSelection& sel) {
    for (auto& b : sel.bones) { b.head = !b.head; b.tail = !b.tail; }
}

// --- click -----------------------------------------------------------------------------------

/**
 * @brief Blender's edit-armature click select.
 *
 * Plain click: clears, then selects what was hit -- an endpoint handle selects that endpoint
 * alone, a body selects BOTH -- and makes its bone active. A click on nothing clears everything.
 *
 * Shift-click TOGGLES, and only makes a bone active when it ADDS to the selection:
 *  - endpoint handle: that one flag flips;
 *  - body: an unselected or half-selected bone becomes fully selected and active, while a bone
 *    that is already fully selected AND active is deselected outright (the rule Object Mode
 *    already uses for objects);
 *  - nothing: no change at all, so a stray shift-click cannot wipe a selection.
 */
inline void click_select(RigSelection& sel, const rig_pick::BonePickResult& hit, bool shift) {
    if (!hit.hit()) {
        if (!shift) select_none(sel);
        return;
    }
    if (!in_range(sel, hit.bone)) return;
    auto& b = sel.bones[static_cast<std::size_t>(hit.bone)];
    if (!shift) {
        select_none(sel);
        if (hit.endpoint) {
            if (*hit.endpoint == rig_pick::BoneEndpoint::Head) b.head = true;
            else b.tail = true;
        } else {
            b.head = b.tail = true;
        }
        sel.active = hit.bone;
        return;
    }
    if (hit.endpoint) {
        const bool head = *hit.endpoint == rig_pick::BoneEndpoint::Head;
        bool& flag = head ? b.head : b.tail;
        flag = !flag;
        if (flag) sel.active = hit.bone;
        return;
    }
    if (b.both() && sel.active == hit.bone) {
        b.head = b.tail = false;   // already the active, fully-selected bone: shift-click drops it
        return;
    }
    b.head = b.tail = true;
    sel.active = hit.bone;
}

// --- box -------------------------------------------------------------------------------------

/** @brief One bone's endpoints projected to screen; `*_on_screen` false = behind the camera. */
struct BonePoints2D {
    glm::vec2 head{0.0f};
    glm::vec2 tail{0.0f};
    bool head_on_screen = false;
    bool tail_on_screen = false;
};

/** @brief Every bone as a world-space segment -- the one place a RigModel becomes screen-space
 *         geometry, shared by picking, box select and drawing. */
inline std::vector<rig_pick::BoneSegment> bone_segments(const RigModel& m, const glm::mat4& owner_world) {
    std::vector<rig_pick::BoneSegment> segs;
    segs.reserve(m.bones.size());
    for (std::size_t i = 0; i < m.bones.size(); ++i) {
        rig_pick::BoneSegment s;
        s.head = glm::vec3(owner_world * glm::vec4(m.bones[i].head, 1.0f));
        s.tail = glm::vec3(owner_world * glm::vec4(m.bones[i].tail, 1.0f));
        const glm::mat4 rest = owner_world * m.world_rest_matrix(static_cast<int>(i));
        s.x_hint = glm::normalize(glm::vec3(rest[0]));
        s.z_hint = glm::normalize(glm::vec3(rest[2]));
        segs.push_back(s);
    }
    return segs;
}

/** @brief Projects every segment's two endpoints. */
inline std::vector<BonePoints2D> project_endpoints(const ViewProj& vp, const std::vector<rig_pick::BoneSegment>& segs) {
    std::vector<BonePoints2D> out(segs.size());
    for (std::size_t i = 0; i < segs.size(); ++i) {
        if (const auto h = vp.project(segs[i].head)) { out[i].head = *h; out[i].head_on_screen = true; }
        if (const auto t = vp.project(segs[i].tail)) { out[i].tail = *t; out[i].tail_on_screen = true; }
    }
    return out;
}

/**
 * @brief Blender's edit-armature box select: every ENDPOINT whose projected point falls inside
 *        the box. A body merely crossing the box does not count (that is Pose mode's rule), so a
 *        bone with both ends inside comes out fully selected.
 *
 * `additive` keeps what was already selected, `subtract` removes instead of adds; with neither,
 * the selection is cleared first -- the same three-way the mesh and pose box selects take.
 */
inline void box_select(RigSelection& sel, const std::vector<BonePoints2D>& pts, glm::vec2 a, glm::vec2 b,
                       bool additive, bool subtract) {
    const glm::vec2 lo(std::min(a.x, b.x), std::min(a.y, b.y));
    const glm::vec2 hi(std::max(a.x, b.x), std::max(a.y, b.y));
    auto inside = [&](glm::vec2 p) { return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y; };
    if (!additive && !subtract) select_none(sel);
    const std::size_t n = std::min(pts.size(), sel.bones.size());
    for (std::size_t i = 0; i < n; ++i) {
        bool touched = false;
        if (pts[i].head_on_screen && inside(pts[i].head)) { sel.bones[i].head = !subtract; touched = true; }
        if (pts[i].tail_on_screen && inside(pts[i].tail)) { sel.bones[i].tail = !subtract; touched = true; }
        if (touched && !subtract) sel.active = static_cast<int>(i);
    }
}

// --- transform -------------------------------------------------------------------------------

/**
 * @brief Applies a RIG-SPACE delta to every selected endpoint, rebuilt from `base` so a modal
 *        operator can re-apply its cumulative result every frame (and cancelling is just the
 *        identity delta).
 *
 * Writes through RigModel::set_head / set_tail, so `connected` cascades exactly as rig_model.h
 * documents -- which is what makes this correct rather than a special case:
 *  - a connected bone's selected HEAD moves its parent's TAIL, because they are one joint and
 *    that is the joint Blender moves;
 *  - a selected tail carries its connected children's heads along;
 *  - both selected resolves to that same joint twice, with the same absolute value.
 * Every write is an ABSOLUTE position computed from `base`, never an increment, so writing a
 * shared joint twice is harmless and the iteration order does not matter.
 *
 * @param roll_rotation The delta's rotation/scale 3x3. A bone with BOTH endpoints selected gets
 *                      its `roll` re-derived so its rest Z follows `roll_rotation` -- otherwise
 *                      rotating a bone would slide its points while leaving its twist pointing
 *                      the old way. A one-endpoint move leaves roll alone, as Blender does.
 */
inline void transform_endpoints(RigModel& out, const RigModel& base, const RigSelection& sel,
                                const glm::mat4& rig_delta, const glm::mat3& roll_rotation) {
    const std::size_t n = std::min(base.bones.size(), sel.bones.size());
    if (out.bones.size() != base.bones.size()) out = base;
    auto moved = [&](const glm::vec3& p) { return glm::vec3(rig_delta * glm::vec4(p, 1.0f)); };
    for (std::size_t i = 0; i < n; ++i) {
        const int bi = static_cast<int>(i);
        if (sel.bones[i].head) out.set_head(bi, moved(base.bones[i].head));
        if (sel.bones[i].tail) out.set_tail(bi, moved(base.bones[i].tail));
    }
    // Roll second: it reads the bone's NEW head/tail, so every point has to be in place first.
    for (std::size_t i = 0; i < n; ++i) {
        if (!sel.bones[i].both()) continue;
        const glm::mat4 rest = base.world_rest_matrix(static_cast<int>(i));
        const glm::vec3 z_new = roll_rotation * glm::vec3(rest[2]);
        out.set_roll(static_cast<int>(i), roll_to_align_z(out.bones[i].head, out.bones[i].tail, z_new));
    }
}

} // namespace rig_sel
} // namespace editor
} // namespace toy

#endif // TOYENGINE_EDITOR_RIG_RIG_SELECTION_H
