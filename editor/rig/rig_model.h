/**
 * @file rig_model.h
 * @brief The editor's model of one rig asset file (coopa::rig::RigAsset's YAML schema) and the
 *        authoring operations Rig Edit mode performs on it: add/extrude/move/roll/rename/
 *        reparent/connect/delete a bone.
 *
 * Pure (no GPU, no UI, no SceneObject): Rig Edit mode (app/editor_app.cpp) edits a RigModel and
 * writes it back with to_node(); the runtime reads the same file through coopa::rig::parse_rig().
 * `parent` is an index into `bones` (matching RigAsset exactly) rather than a name, so an editing
 * op never has to resolve a name through a stale snapshot -- but to_node()/from_node() still
 * write/read `parent:` as a NAME, matching the asset's own on-disk convention.
 *
 * Bones stay parent-first (every bone's parent index < its own index), as RigAsset requires.
 * reparent_bone() enforces this by only allowing a bone to take a parent EARLIER in the list --
 * reordering the list to support an arbitrary reparent is not implemented; a reparent to a later
 * bone fails (returns false) rather than silently reordering everything underneath it.
 *
 * `connected` (a child's head pinned to its parent's tail) cascades exactly as Blender's rig
 * editing does: moving a connected bone's head actually moves the PARENT's tail (they are the
 * same point on screen), and moving a bone's tail carries its own connected children's heads
 * along with it. Disconnecting is explicit (set_connected(i, false)); it never happens as a side
 * effect of a move.
 */

#ifndef TOYENGINE_EDITOR_RIG_RIG_MODEL_H
#define TOYENGINE_EDITOR_RIG_RIG_MODEL_H

#include "../core/yaml_util.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace toy {
namespace editor {

/** @brief One bone's rest pose -- the editor-side sibling of coopa::rig::Bone. */
struct RigModelBone {
    std::string name;
    int parent = -1;              ///< Index into RigModel::bones, or -1 for a root bone.
    glm::vec3 head{0.0f, 0.0f, 0.0f};
    glm::vec3 tail{0.0f, 1.0f, 0.0f};
    float roll = 0.0f;            ///< Degrees, about the head->tail axis.
    bool connected = false;       ///< Head equals the parent's tail (enforced by RigModel's ops, not computed lazily).

    bool operator==(const RigModelBone& o) const {
        return name == o.name && parent == o.parent && head == o.head && tail == o.tail && roll == o.roll && connected == o.connected;
    }
};

struct RigModel {
    std::string name = "rig";
    std::vector<RigModelBone> bones;

    bool operator==(const RigModel& o) const { return name == o.name && bones == o.bones; }

    // --- YAML ---

    /** @brief Parses the `rig: {name, bones: [...]}` mapping -- the exact shape coopa::rig::parse_rig() reads. */
    static RigModel from_node(const Node& root);

    /** @brief Writes the same shape back, `parent:` as the parent bone's NAME. */
    Node to_node() const;

    // --- lookup ---

    int find_bone(const std::string& bone_name) const;
    float bone_length(int i) const { return glm::length(bones[static_cast<size_t>(i)].tail - bones[static_cast<size_t>(i)].head); }

    /** @brief Bone i's rest matrix in its PARENT's bone space (coopa::rig::bone_rest_matrix on this bone alone). */
    glm::mat4 local_rest_matrix(int i) const;

    /** @brief Bone i's rest matrix in rig space: its chain of local_rest_matrix()s up to the root. */
    glm::mat4 world_rest_matrix(int i) const;

    // --- authoring ops ---

    /** @brief Adds a new root (parent -1) or child bone with the given head/tail. @return Its index. */
    int add_bone(const std::string& bone_name, int parent, const glm::vec3& head, const glm::vec3& tail);

    /**
     * @brief A new CONNECTED child of `from`, starting at `from`'s tail and running to `new_tail`
     *        -- Blender's Extrude. @return The new bone's index, or -1 if `from` is out of range.
     */
    int extrude_bone(int from, const std::string& bone_name, const glm::vec3& new_tail);

    /**
     * @brief Moves bone `i`'s head. If `i` is connected, this instead moves its PARENT's tail
     *        (the same point on screen) -- see the file doc. A root, disconnected bone's head
     *        moves on its own.
     */
    void set_head(int i, const glm::vec3& head);

    /** @brief Moves bone `i`'s tail, carrying any directly-connected child's head along with it. */
    void set_tail(int i, const glm::vec3& tail);

    void set_roll(int i, float degrees);

    /**
     * @brief Renames bone `i`. Fails (returns false, no change) if `to` is empty or already taken
     *        by another bone. The caller is responsible for repointing referrers (see
     *        rig_referrers.h) -- this only touches the model itself.
     */
    bool rename_bone(int i, const std::string& to);

    /**
     * @brief Reparents bone `i` under `new_parent` (-1 for root). Fails if that would violate the
     *        parent-first order (new_parent's index must be < i, since reordering is not
     *        implemented), or create a cycle. Disconnects `i` (connected only means "pinned to
     *        THIS parent's tail"; a new parent means a new, unpinned head the caller can
     *        reconnect with set_connected() if the geometry happens to line up).
     */
    bool reparent_bone(int i, int new_parent);

    /** @brief Turning on pins `i`'s head to its parent's tail immediately (matching it once, then
     *         keeping it pinned via set_tail()'s cascade); turning off just clears the flag. */
    void set_connected(int i, bool on);

    /**
     * @brief Deletes bone `i`. Its children reparent to `i`'s own parent (Blender's rule) and, if
     *        they were connected to `i`, their head moves to `i`'s parent's tail (or stays put,
     *        disconnected, if `i` had no parent). @return The reparented children's names.
     */
    std::vector<std::string> delete_bone(int i);

    /**
     * @brief Reorders `bones` into a stable parent-first order (depth first from the roots,
     *        children keeping their current relative order) and fixes every `parent` index.
     * @return old index -> new index, for remapping anything that holds bone indices
     *         (rig_sel::remap()).
     *
     * Lets an authoring op append bones wherever it likes and normalise afterwards, instead of
     * every op having to splice at exactly the right position -- and it is the piece
     * reparent_bone()'s doc calls "not implemented", so an arbitrary reparent is now
     * reparent + sort.
     */
    std::vector<int> sort_parent_first();
};

/**
 * @brief The roll (degrees) that aims the bone (head -> tail)'s rest Z as close to `up` as the
 *        bone's own axis allows; the bone's current roll is irrelevant to the answer.
 *
 * Closed form, no search. With (x0, y, z0) the roll-0 basis coopa::rig::bone_rest_matrix builds,
 * rotating by t about y gives z(t) = cos(t)*z0 + sin(t)*x0, so aiming z at `up` (with the
 * component along y removed, since no roll can tilt out of that plane) is
 * t = atan2(dot(up_perp, x0), dot(up_perp, z0)).
 *
 * @return 0 for a degenerate bone, or when `up` is parallel to the bone axis (nothing to aim at).
 */
float roll_to_align_z(const glm::vec3& head, const glm::vec3& tail, const glm::vec3& up);

} // namespace editor
} // namespace toy

#endif // TOYENGINE_EDITOR_RIG_RIG_MODEL_H
