#include "editor/rig/rig_edit_ops.h"

#include <algorithm>
#include <cmath>

namespace toy {
namespace editor {
namespace rig_ops {

namespace {

glm::vec3 reflect(const glm::vec3& v, int axis) {
    glm::vec3 out = v;
    out[axis] = -out[axis];
    return out;
}

/** @brief Bone `i`'s rest Z in rig space -- the direction a roll aims. */
glm::vec3 rest_z(const RigModel& m, int i) { return glm::vec3(m.world_rest_matrix(i)[2]); }

} // namespace

std::string unique_bone_name(const RigModel& m, const std::string& base) {
    if (m.find_bone(base) < 0) return base;
    for (int n = 1; n < 100000; ++n) {
        const std::string candidate = base + "_" + std::to_string(n);
        if (m.find_bone(candidate) < 0) return candidate;
    }
    return base;
}

float mirrored_roll(const glm::vec3& head, const glm::vec3& tail, float roll, int axis) {
    RigModelBone probe;
    probe.head = head;
    probe.tail = tail;
    probe.roll = roll;
    RigModel one;
    one.bones.push_back(probe);
    const glm::vec3 z_mirrored = reflect(rest_z(one, 0), axis);
    return roll_to_align_z(reflect(head, axis), reflect(tail, axis), z_mirrored);
}

RigModelBone mirror_bone(const RigModelBone& b, int axis) {
    RigModelBone out = b;
    out.head = reflect(b.head, axis);
    out.tail = reflect(b.tail, axis);
    out.roll = mirrored_roll(b.head, b.tail, b.roll, axis);
    return out;
}

int subdivide_bone(RigModel& m, rig_sel::RigSelection& sel, int i, int cuts) {
    if (i < 0 || static_cast<std::size_t>(i) >= m.bones.size() || cuts < 1) return 0;
    const RigModelBone original = m.bones[static_cast<std::size_t>(i)];
    const int segments = cuts + 1;
    const glm::vec3 step = (original.tail - original.head) / static_cast<float>(segments);

    // Children of `i` must end up on the LAST segment, so remember them before the bone list
    // grows (their indices are stable: new bones are appended).
    std::vector<int> children;
    for (std::size_t j = 0; j < m.bones.size(); ++j) {
        if (m.bones[j].parent == i && static_cast<int>(j) != i) children.push_back(static_cast<int>(j));
    }

    m.bones[static_cast<std::size_t>(i)].tail = original.head + step;   // the first segment keeps name + index
    int prev = i;
    std::vector<int> created;
    for (int s = 1; s < segments; ++s) {
        RigModelBone b;
        b.name = unique_bone_name(m, original.name);
        b.parent = prev;
        b.head = original.head + step * static_cast<float>(s);
        b.tail = original.head + step * static_cast<float>(s + 1);
        b.roll = original.roll;
        b.connected = true;
        m.bones.push_back(b);
        prev = static_cast<int>(m.bones.size()) - 1;
        created.push_back(prev);
    }
    for (int c : children) {
        m.bones[static_cast<std::size_t>(c)].parent = prev;   // connected ones were already at the old tail
    }
    const std::vector<int> old_to_new = m.sort_parent_first();
    rig_sel::remap(sel, old_to_new);
    rig_sel::validate(sel, m);
    for (int c : created) {
        const int n = old_to_new[static_cast<std::size_t>(c)];
        if (n >= 0 && static_cast<std::size_t>(n) < sel.bones.size()) { sel.bones[static_cast<std::size_t>(n)].head = true; sel.bones[static_cast<std::size_t>(n)].tail = true; }
    }
    return static_cast<int>(created.size());
}

int subdivide_selected(RigModel& m, rig_sel::RigSelection& sel, int cuts) {
    // Descending: subdivide_bone re-sorts the list, so taking the highest index first keeps the
    // remaining targets findable by NAME rather than by a stale index.
    std::vector<std::string> targets;
    for (int b : rig_sel::fully_selected_bones(sel)) targets.push_back(m.bones[static_cast<std::size_t>(b)].name);
    int created = 0;
    for (const std::string& name : targets) {
        const int i = m.find_bone(name);
        if (i >= 0) created += subdivide_bone(m, sel, i, cuts);
    }
    return created;
}

int fill_between_joints(RigModel& m, rig_sel::RigSelection& sel, const std::string& name_hint) {
    const auto pts = rig_sel::selected_endpoints(m, sel);
    if (pts.size() != 2) return -1;
    if (glm::distance(pts[0].pos, pts[1].pos) < 1e-6f) return -1;

    // A tail start makes the new bone a connected continuation of that chain -- Blender's rule,
    // and what makes F usable for "keep going from here".
    const bool a_tail = pts[0].end == rig_pick::BoneEndpoint::Tail;
    const bool b_tail = pts[1].end == rig_pick::BoneEndpoint::Tail;
    int from = -1;
    glm::vec3 head = pts[0].pos, tail = pts[1].pos;
    if (a_tail != b_tail) {
        const auto& t = a_tail ? pts[0] : pts[1];
        const auto& o = a_tail ? pts[1] : pts[0];
        from = t.bone;
        head = t.pos;
        tail = o.pos;
    }
    RigModelBone b;
    b.name = unique_bone_name(m, name_hint);
    b.head = head;
    b.tail = tail;
    if (from >= 0) {
        b.parent = from;
        b.connected = true;
    } else if (sel.active >= 0 && static_cast<std::size_t>(sel.active) < m.bones.size()) {
        b.parent = m.bones[static_cast<std::size_t>(sel.active)].parent;
    }
    m.bones.push_back(b);
    const std::vector<int> old_to_new = m.sort_parent_first();
    rig_sel::remap(sel, old_to_new);
    rig_sel::validate(sel, m);
    const int created = m.find_bone(b.name);
    if (created >= 0) {
        rig_sel::select_none(sel);
        sel.bones[static_cast<std::size_t>(created)].head = true;
        sel.bones[static_cast<std::size_t>(created)].tail = true;
        sel.active = created;
    }
    return created;
}

int merge_selected_chains(RigModel& m, rig_sel::RigSelection& sel) {
    int removed = 0;
    bool merged_any = true;
    // One adjacent pair at a time, repeating until nothing merges: delete_bone shifts indices,
    // and re-running from scratch is far easier to get right than tracking a run through them.
    while (merged_any) {
        merged_any = false;
        for (std::size_t child = 0; child < m.bones.size(); ++child) {
            const int parent = m.bones[child].parent;
            if (parent < 0 || !m.bones[child].connected) continue;
            if (!rig_sel::is_fully_selected(sel, static_cast<int>(child)) || !rig_sel::is_fully_selected(sel, parent)) continue;
            // The parent absorbs the child: it keeps its own name, index and roll, and takes the
            // child's tail. delete_bone then hands the child's own children back to the parent.
            m.bones[static_cast<std::size_t>(parent)].tail = m.bones[child].tail;
            m.delete_bone(static_cast<int>(child));
            rig_sel::validate(sel, m);
            ++removed;
            merged_any = true;
            break;
        }
    }
    return removed;
}

int recalculate_roll(RigModel& m, const rig_sel::RigSelection& sel, const RollTarget& t) {
    glm::vec3 reference = t.vector;
    if (t.kind == RollTarget::Kind::ActiveBone) {
        if (sel.active < 0 || static_cast<std::size_t>(sel.active) >= m.bones.size()) return 0;
        reference = rest_z(m, sel.active);
    }
    int changed = 0;
    for (int i : rig_sel::selected_bones(sel)) {
        if (static_cast<std::size_t>(i) >= m.bones.size()) continue;
        const RigModelBone& b = m.bones[static_cast<std::size_t>(i)];
        // A Point target aims every bone at the same place, so the direction is per bone.
        const glm::vec3 up = t.kind == RollTarget::Kind::Point ? (t.vector - b.head) : reference;
        const float roll = roll_to_align_z(b.head, b.tail, up);
        if (std::abs(roll - b.roll) > 1e-5f) { m.set_roll(i, roll); ++changed; }
    }
    return changed;
}

int align_to_active(RigModel& m, const rig_sel::RigSelection& sel) {
    if (sel.active < 0 || static_cast<std::size_t>(sel.active) >= m.bones.size()) return 0;
    const auto selected = rig_sel::selected_bones(sel);
    // One bone selected: align it to its PARENT, since aligning a bone to itself is a no-op and
    // "make this follow the chain" is what the gesture means there (Blender's own fallback).
    int reference = sel.active;
    if (selected.size() <= 1) {
        reference = m.bones[static_cast<std::size_t>(sel.active)].parent;
        if (reference < 0) return 0;
    }
    const glm::vec3 up = rest_z(m, reference);
    int changed = 0;
    for (int i : selected) {
        if (i == reference || static_cast<std::size_t>(i) >= m.bones.size()) continue;
        const RigModelBone& b = m.bones[static_cast<std::size_t>(i)];
        const float roll = roll_to_align_z(b.head, b.tail, up);
        if (std::abs(roll - b.roll) > 1e-5f) { m.set_roll(i, roll); ++changed; }
    }
    return changed;
}

int symmetrize(RigModel& m, rig_sel::RigSelection& sel, SymmetrizeDirection dir, int axis, std::vector<std::string>* skipped) {
    const auto chosen = rig_sel::fully_selected_bones(sel);
    // Snapshot the source names first: the bone list grows below, and a name is a stable handle
    // across the parent-first re-sort at the end.
    std::vector<std::string> sources;
    for (int i : chosen) {
        const RigModelBone& b = m.bones[static_cast<std::size_t>(i)];
        const auto partner = mirrored_bone_name(b.name);
        if (!partner) {
            if (skipped) skipped->push_back(b.name);
            continue;
        }
        // Only copy FROM the requested side, so running it twice does not ping-pong.
        const float side = b.head[axis] + b.tail[axis];
        const bool positive = side >= 0.0f;
        if ((dir == SymmetrizeDirection::PositiveToNegative) != positive && std::abs(side) > 1e-6f) continue;
        sources.push_back(b.name);
    }

    int touched = 0;
    for (const std::string& src_name : sources) {
        const int src = m.find_bone(src_name);
        if (src < 0) continue;
        const std::string dst_name = *mirrored_bone_name(src_name);
        RigModelBone mirrored = mirror_bone(m.bones[static_cast<std::size_t>(src)], axis);
        mirrored.name = dst_name;
        // Parent to the MIRROR of the source's parent; a centre parent (`spine`) has no mirror,
        // so both sides keep hanging off the same bone, which is what a shoulder does.
        const int src_parent = m.bones[static_cast<std::size_t>(src)].parent;
        if (src_parent >= 0) {
            const std::string& pname = m.bones[static_cast<std::size_t>(src_parent)].name;
            const auto pmirror = mirrored_bone_name(pname);
            const int mapped = pmirror ? m.find_bone(*pmirror) : -1;
            mirrored.parent = mapped >= 0 ? mapped : src_parent;
        } else {
            mirrored.parent = -1;
        }
        const int existing = m.find_bone(dst_name);
        if (existing >= 0) {
            const int keep_parent = mirrored.parent == existing ? m.bones[static_cast<std::size_t>(existing)].parent : mirrored.parent;
            mirrored.parent = keep_parent;
            m.bones[static_cast<std::size_t>(existing)] = mirrored;
        } else {
            m.bones.push_back(mirrored);
        }
        ++touched;
    }
    if (touched > 0) {
        const std::vector<int> old_to_new = m.sort_parent_first();
        rig_sel::remap(sel, old_to_new);
        rig_sel::validate(sel, m);
        for (const std::string& src_name : sources) {
            const int created = m.find_bone(*mirrored_bone_name(src_name));
            if (created >= 0 && static_cast<std::size_t>(created) < sel.bones.size()) {
                sel.bones[static_cast<std::size_t>(created)].head = true;
                sel.bones[static_cast<std::size_t>(created)].tail = true;
            }
        }
    }
    return touched;
}

RigMirrorMap build_rig_mirror_map(const RigModel& m, const rig_sel::RigSelection& sel, int axis) {
    (void)axis;
    RigMirrorMap map;
    for (int i : rig_sel::selected_bones(sel)) {
        if (static_cast<std::size_t>(i) >= m.bones.size()) continue;
        const auto partner = mirrored_bone_name(m.bones[static_cast<std::size_t>(i)].name);
        if (!partner) continue;
        const int dst = m.find_bone(*partner);
        if (dst < 0 || dst == i) continue;
        if (rig_sel::is_selected(sel, dst)) continue;   // it moves on its own; mirroring too would double it
        map.pairs.push_back({i, dst});
    }
    return map;
}

void apply_rig_mirror_map(RigModel& m, const RigModel& base, const rig_sel::RigSelection& sel, const RigMirrorMap& map, int axis) {
    for (const auto& p : map.pairs) {
        if (static_cast<std::size_t>(p.src) >= m.bones.size() || static_cast<std::size_t>(p.dst) >= m.bones.size()) continue;
        if (static_cast<std::size_t>(p.src) >= sel.bones.size()) continue;
        // Only the endpoints the selection actually moved: mirroring an untouched end would drag
        // the far side's geometry around for no reason.
        if (sel.bones[static_cast<std::size_t>(p.src)].head) m.set_head(p.dst, reflect(m.bones[static_cast<std::size_t>(p.src)].head, axis));
        if (sel.bones[static_cast<std::size_t>(p.src)].tail) m.set_tail(p.dst, reflect(m.bones[static_cast<std::size_t>(p.src)].tail, axis));
        const RigModelBone& s = m.bones[static_cast<std::size_t>(p.src)];
        m.set_roll(p.dst, mirrored_roll(s.head, s.tail, s.roll, axis));
    }
    (void)base;
}

} // namespace rig_ops
} // namespace editor
} // namespace toy
