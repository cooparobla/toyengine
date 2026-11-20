#include <toyengine/cloth/character_cloth.h>

#include <algorithm>

#include <coopa/scene/scene_object.h>

namespace toy {
namespace cloth {

std::string CharacterCloth::state_key() const {
    return owner != nullptr ? owner->name() : std::string("<orphan>");
}

glm::vec2 auto_bone_capsule(float length, float scale) {
    const float s = std::max(scale, 0.0f);
    if (length < k_auto_sphere_bone_length) {
        return glm::vec2(0.08f * s, 0.0f);   // a stub bone: a sphere, no axis worth stretching
    }
    const float radius = std::clamp(length * 0.22f, 0.03f, 0.2f) * s;
    // The cylinder spans the bone, so the capsule's TOTAL length is bone + two caps. Half the
    // bone is the cylindrical half-height physxcoopa wants (it excludes the caps).
    return glm::vec2(radius, length * 0.5f);
}

} // namespace cloth
} // namespace toy
