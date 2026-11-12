#include <toyengine/destruct/slice.h>

#include <toyengine/destruct/fracture.h>

#include <glm/gtc/quaternion.hpp>

namespace toy {
namespace destruct {

namespace {

/** @brief Clips, frames and centres one side of the cut; false if the half is empty or too
 *         thin a sliver of `source_volume` to be worth a piece of its own. */
bool make_half(const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices,
               const glm::vec3& normal, float distance, const SliceSettings& settings,
               float source_volume, SlicePiece& out) {
    std::uint32_t cap_first = 0;
    if (!clip_mesh(vertices, indices, normal, distance, settings.cap, out.vertices, out.indices, &cap_first))
        return false;
    out.cap_first_index = cap_first;

    out.volume = mesh_volume(out.vertices, out.indices);
    if (out.volume <= 0.0f || out.volume < settings.min_volume_fraction * source_volume) return false;

    const glm::mat3 frame = best_frame(out.vertices, out.indices);
    centre_in_frame(out.vertices, frame, out.half_extent, out.centre);
    out.orientation = glm::quat_cast(frame);
    return true;
}

} // namespace

bool slice_mesh(const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices,
               const glm::vec3& normal, float distance, const SliceSettings& settings,
               SlicePiece& out_negative, SlicePiece& out_positive) {
    out_negative = SlicePiece{};
    out_positive = SlicePiece{};
    if (indices.size() < 3) return false;

    const float source_volume = mesh_volume(vertices, indices);
    if (source_volume <= 0.0f) return false;

    // Build into locals so a failure on the second half never leaves the first half's result
    // sitting in the caller's output -- a false return must mean both outputs are empty.
    SlicePiece negative, positive;
    if (!make_half(vertices, indices, normal, distance, settings, source_volume, negative)) return false;
    if (!make_half(vertices, indices, -normal, -distance, settings, source_volume, positive)) return false;
    out_negative = std::move(negative);
    out_positive = std::move(positive);
    return true;
}

} // namespace destruct
} // namespace toy
