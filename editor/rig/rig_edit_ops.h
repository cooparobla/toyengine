/**
 * @file rig_edit_ops.h
 * @brief Blender's armature-edit operators as pure functions over a RigModel + RigSelection:
 *        Subdivide, Fill, Merge, Recalculate Roll, Align, Symmetrize, and the live X-mirror map.
 *
 * Nothing here touches EditorApp, undo or the viewport -- the panel supplies the selection and
 * wraps the call in one undoable edit, exactly as editor/mesh/'s operators work. The mirror
 * plane throughout is rig-space **X = 0**, matching bone_pose_edit.h's own mirror and every
 * `_l`/`_r` rig the generators produce.
 */

#ifndef TOYENGINE_EDITOR_RIG_RIG_EDIT_OPS_H
#define TOYENGINE_EDITOR_RIG_RIG_EDIT_OPS_H

#include "bone_names.h"
#include "rig_model.h"
#include "rig_selection.h"

#include <string>
#include <vector>

namespace toy {
namespace editor {
namespace rig_ops {

/** @brief A free name based on `base` ("bone", "bone_1", ...) -- every op that creates bones. */
std::string unique_bone_name(const RigModel& m, const std::string& base);

/** @brief One bone reflected across rig-space plane `axis` (0 = X): head, tail and roll. The
 *         NAME is left alone -- callers decide whether this is a new bone or an overwrite. */
RigModelBone mirror_bone(const RigModelBone& b, int axis = 0);

/**
 * @brief The roll the mirror image of this bone needs in order to LOOK mirrored.
 *
 * Computed numerically -- reflect the bone's own rest Z and ask roll_to_align_z() for the roll
 * that aims at it -- rather than as the tempting `-roll`. That shortcut is right for most bones
 * but WRONG for one running along world Z, where coopa::rig::bone_rest_matrix switches its seed
 * axis from +Z to +X (rig_asset.h) and the mirror negates that seed, making the answer
 * `180 - roll`. Deriving it from the basis is correct in both branches by construction.
 */
float mirrored_roll(const glm::vec3& head, const glm::vec3& tail, float roll, int axis = 0);

/**
 * @brief Blender's Armature > Subdivide: bone `i` becomes `cuts + 1` connected bones along its
 *        own head->tail. The first segment keeps the original's name and index; the rest are
 *        appended and the list re-sorted parent-first. Children CONNECTED to `i` follow the last
 *        segment's tail; unconnected children reparent onto the last segment.
 * @return How many bones were created (0 if `i` or `cuts` is out of range).
 */
int subdivide_bone(RigModel& m, rig_sel::RigSelection& sel, int i, int cuts);

/** @brief Subdivides every FULLY selected bone. @return How many bones were created. */
int subdivide_selected(RigModel& m, rig_sel::RigSelection& sel, int cuts);

/**
 * @brief Blender's Fill Between Joints (F): a new bone spanning the two selected endpoints.
 *
 * When exactly one of the two is a TAIL the new bone starts there, parented to and connected
 * with that bone -- which is what makes F continue a chain. Otherwise it is a free child of the
 * active bone's parent. The new bone ends up active and fully selected.
 * @return Its index, or -1 unless exactly two endpoints at distinct positions are selected.
 */
int fill_between_joints(RigModel& m, rig_sel::RigSelection& sel, const std::string& name_hint = "bone");

/**
 * @brief Blender's Merge > Within Chain (Alt+M): every maximal run of fully-selected bones
 *        forming a parent->child CONNECTED chain collapses into one bone spanning the run's
 *        first head to its last tail, keeping the first bone's name, index and roll. The run's
 *        other bones are deleted and their unselected children reparent onto the survivor.
 * @return How many bones were removed.
 */
int merge_selected_chains(RigModel& m, rig_sel::RigSelection& sel);

/** @brief What Recalculate Roll aims each bone's rest Z at. */
struct RollTarget {
    enum class Kind { Axis, Point, ActiveBone } kind = Kind::Axis;
    /** Axis: a rig-space direction (global +Z, +X...). Point: a rig-space point to aim at
     *  (the 3D cursor). ActiveBone: ignored -- the active bone's own rest Z is the reference. */
    glm::vec3 vector{0.0f, 0.0f, 1.0f};
};

/** @brief Blender's Recalculate Roll (Ctrl+N) over every selected bone. @return rolls changed. */
int recalculate_roll(RigModel& m, const rig_sel::RigSelection& sel, const RollTarget& t);

/**
 * @brief Blender's Align (Ctrl+Alt+A): rolls every selected bone so its rest Z matches the
 *        ACTIVE bone's.
 *
 * Only the roll changes -- Blender's bone_align_to_bone keeps each bone's head and tail and does
 * not re-aim it, which surprises people who expect "align" to move things. With exactly one bone
 * selected it aligns to that bone's PARENT instead, as Blender does.
 * @return How many rolls changed.
 */
int align_to_active(RigModel& m, const rig_sel::RigSelection& sel);

/** @brief Which side Symmetrize copies FROM. */
enum class SymmetrizeDirection { PositiveToNegative, NegativeToPositive };

/**
 * @brief Blender's Armature > Symmetrize: every fully-selected bone whose name carries a side
 *        gets its opposite-side partner created or overwritten as its X mirror (head, tail,
 *        mirrored_roll, connected), parented to the mirror of its own parent -- falling back to
 *        the same parent when that parent is a centre bone. The list is re-sorted parent-first.
 *
 * A selected bone with NO side suffix is skipped and reported, rather than having a side
 * invented for it behind the user's back.
 * @param skipped Filled with the names that carried no side (may be null).
 * @return How many partner bones were created or overwritten.
 */
int symmetrize(RigModel& m, rig_sel::RigSelection& sel, SymmetrizeDirection dir = SymmetrizeDirection::PositiveToNegative,
               int axis = 0, std::vector<std::string>* skipped = nullptr);

/** @brief Who mirrors whom for the transform about to run, captured BEFORE anything moves --
 *         mesh_mirror.h's MirrorMap, for bones. */
struct RigMirrorMap {
    struct Pair { int src = -1; int dst = -1; };
    std::vector<Pair> pairs;
    bool empty() const { return pairs.empty(); }
};

/** @brief Every selected bone's `_l`/`_r` partner, skipping partners that are themselves
 *         selected (those get transformed directly) and bones with no side in their name. */
RigMirrorMap build_rig_mirror_map(const RigModel& m, const rig_sel::RigSelection& sel, int axis = 0);

/**
 * @brief After the selected endpoints have moved: writes each partner's head/tail as the
 *        reflection of its source's NEW position -- head<->head, tail<->tail, since bones mirror
 *        rather than reverse -- for the endpoints the selection actually moved, plus the
 *        mirrored roll. Goes through set_head/set_tail, so the far side's `connected` cascade
 *        happens too.
 */
void apply_rig_mirror_map(RigModel& m, const RigModel& base, const rig_sel::RigSelection& sel, const RigMirrorMap& map,
                          int axis = 0);

} // namespace rig_ops
} // namespace editor
} // namespace toy

#endif // TOYENGINE_EDITOR_RIG_RIG_EDIT_OPS_H
