/**
 * @file rig_asset.h
 * @brief Immutable skeleton data: a flat, parent-first list of bones with a
 *        Blender-style head/tail/roll rest pose.
 */

#ifndef TOYENGINE_RIG_RIG_ASSET_H
#define TOYENGINE_RIG_RIG_ASSET_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace toy {
namespace rig {

/**
 * @struct Bone
 * @brief One bone's rest pose, in rig space.
 *
 * `head` and `tail` define the bone's rest length and its local +Y axis
 * (`normalize(tail - head)`); `roll` turns the remaining two axes about that
 * axis, matching Blender's bone basis. `connected` locks `head` to the
 * parent's `tail` at load time (RigAsset::compute_rest) — authored for
 * convenience, baked away before any matrix math runs.
 */
struct Bone {
    std::string name;
    int         parent = -1; ///< Index into RigAsset::bones, or -1 for a root bone.
    glm::vec3   head{0.0f, 0.0f, 0.0f};
    glm::vec3   tail{0.0f, 1.0f, 0.0f};
    float       roll = 0.0f; ///< Degrees, about the head->tail axis.
    bool        connected = false; ///< Head is forced to the parent's tail (see RigAsset::compute_rest).
};

/**
 * @brief The rest-pose matrix of one bone, in rig space: its local X/Y/Z axes as columns
 *        (Y = head->tail, X/Z orthonormal and rolled by `roll` about Y) and `head` as the
 *        translation. Right-handed by construction (Z is always X cross Y).
 */
inline glm::mat4 bone_rest_matrix(const Bone& bone) {
    glm::vec3 y = bone.tail - bone.head;
    const float len = glm::length(y);
    y = len > 1e-8f ? y / len : glm::vec3(0.0f, 1.0f, 0.0f);

    // Any axis not nearly parallel to y gives a stable seed for the other two.
    const glm::vec3 seed = std::abs(glm::dot(y, glm::vec3(0.0f, 0.0f, 1.0f))) < 0.999f
                                ? glm::vec3(0.0f, 0.0f, 1.0f)
                                : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 x = glm::normalize(glm::cross(seed, y));
    glm::vec3 z = glm::cross(x, y);

    if (bone.roll != 0.0f) {
        const glm::quat r = glm::angleAxis(glm::radians(bone.roll), y);
        x = r * x;
        z = r * z;
    }

    glm::mat4 m(1.0f);
    m[0] = glm::vec4(x, 0.0f);
    m[1] = glm::vec4(y, 0.0f);
    m[2] = glm::vec4(z, 0.0f);
    m[3] = glm::vec4(bone.head, 1.0f);
    return m;
}

/**
 * @class RigAsset
 * @brief Immutable, shareable skeleton data — the payload behind AssetHandle<RigAsset>.
 *
 * `bones` is flat and parent-first: every bone's parent index is strictly
 * less than its own index, so compute_rest() (and any caller walking the
 * list in order) never needs a second pass to resolve a parent before a
 * child. Safe to decode entirely on an asset IO worker thread (see
 * rig_loader.h) — pure data, no SceneObject*, no Component*.
 */
class RigAsset {
public:
    std::string       name;
    std::vector<Bone> bones;

    /**
     * @brief Finds a bone by name.
     * @return Its index, or -1 if no bone has that name.
     */
    int find_bone(const std::string& bone_name) const {
        for (size_t i = 0; i < bones.size(); ++i) {
            if (bones[i].name == bone_name) return static_cast<int>(i);
        }
        return -1;
    }

    /** @brief Rest length of bone `i`: the distance from its head to its tail. */
    float bone_length(size_t i) const { return glm::length(bones[i].tail - bones[i].head); }

    /**
     * @brief Bakes `connected` bones' heads onto their parent's tail, then derives every
     *        bone's rest matrix in rig space (rest_model) and in its parent's bone space
     *        (rest_local). Call once after `bones` is fully populated.
     */
    void compute_rest() {
        for (auto& b : bones) {
            if (b.connected && b.parent >= 0) b.head = bones[static_cast<size_t>(b.parent)].tail;
        }
        rest_model_.assign(bones.size(), glm::mat4(1.0f));
        rest_local_.assign(bones.size(), glm::mat4(1.0f));
        for (size_t i = 0; i < bones.size(); ++i) {
            rest_model_[i] = bone_rest_matrix(bones[i]);
            rest_local_[i] = bones[i].parent >= 0
                                  ? glm::inverse(rest_model_[static_cast<size_t>(bones[i].parent)]) * rest_model_[i]
                                  : rest_model_[i];
        }
    }

    /** @brief Bone i's rest matrix in rig space (bone space -> rig space). Valid after compute_rest(). */
    const std::vector<glm::mat4>& rest_model() const { return rest_model_; }

    /** @brief Bone i's rest matrix in its parent's bone space (root: same as rest_model()[i]). */
    const std::vector<glm::mat4>& rest_local() const { return rest_local_; }

private:
    std::vector<glm::mat4> rest_model_;
    std::vector<glm::mat4> rest_local_;
};

} // namespace rig
} // namespace toy

#endif // TOYENGINE_RIG_RIG_ASSET_H
