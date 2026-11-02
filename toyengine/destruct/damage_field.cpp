#include <toyengine/destruct/damage_field.h>

#include <toyengine/destruct/mesh_solid.h>

#include <algorithm>
#include <limits>

namespace toy {
namespace destruct {

void DamageField::reset(const glm::vec3& bounds_min, const glm::vec3& bounds_max, int longest_axis_cells) {
    const glm::vec3 extent = glm::max(bounds_max - bounds_min, glm::vec3(1e-4f));
    const float longest = std::max({extent.x, extent.y, extent.z});
    cell_ = longest / static_cast<float>(std::max(1, longest_axis_cells));
    dims_ = glm::max(glm::ivec3(glm::ceil(extent / cell_)), glm::ivec3(1));
    origin_ = bounds_min;
    cells_.assign(static_cast<std::size_t>(dims_.x) * static_cast<std::size_t>(dims_.y) *
                      static_cast<std::size_t>(dims_.z),
                  0.0f);
}

bool DamageField::cell_of_(const glm::vec3& local_point, glm::ivec3& out) const {
    if (cells_.empty()) return false;
    const glm::vec3 rel = (local_point - origin_) / cell_;
    out = glm::ivec3(glm::floor(rel));
    return out.x >= 0 && out.y >= 0 && out.z >= 0 && out.x < dims_.x && out.y < dims_.y && out.z < dims_.z;
}

float DamageField::deposit(const glm::vec3& local_point, const glm::vec3& /*local_dir*/, const glm::vec3& local_edge,
                           const coopa::physx::damage::ResolvedDamage& dmg) {
    if (cells_.empty()) return 0.0f;
    const float radius = std::max(cell_ * 0.5f, dmg.radius);
    const float half_length = std::max(0.0f, dmg.along_edge);
    const glm::vec3 edge_dir = half_length > 0.0f && glm::length(local_edge) > 1e-6f
                                   ? glm::normalize(local_edge)
                                   : glm::vec3(0.0f);

    // Cells whose centre could possibly fall within the footprint: the point's AABB, expanded
    // by the capsule's half-length along the edge and by radius in every direction.
    const glm::vec3 reach(radius + half_length * std::abs(edge_dir.x),
                          radius + half_length * std::abs(edge_dir.y),
                          radius + half_length * std::abs(edge_dir.z));
    glm::ivec3 lo, hi;
    cell_of_(local_point - reach, lo);
    cell_of_(local_point + reach, hi);
    lo = glm::clamp(lo, glm::ivec3(0), dims_ - glm::ivec3(1));
    hi = glm::clamp(hi, glm::ivec3(0), dims_ - glm::ivec3(1));

    for (int z = lo.z; z <= hi.z; ++z) {
        for (int y = lo.y; y <= hi.y; ++y) {
            for (int x = lo.x; x <= hi.x; ++x) {
                const glm::vec3 centre = cell_centre_(x, y, z);
                const glm::vec3 to_centre = centre - local_point;
                float dist;
                if (half_length > 0.0f) {
                    // Closest point on the segment [-half_length, +half_length] along edge_dir.
                    const float t = glm::clamp(glm::dot(to_centre, edge_dir), -half_length, half_length);
                    dist = glm::length(to_centre - edge_dir * t);
                } else {
                    dist = glm::length(to_centre);
                }
                if (dist > radius) continue;
                const float falloff = 1.0f - dist / radius;
                float& cell = cells_[index_(x, y, z)];
                cell = std::min(1.0f, cell + falloff);
            }
        }
    }
    return total();
}

float DamageField::at(const glm::vec3& local_point) const {
    glm::ivec3 c;
    if (!cell_of_(local_point, c)) return 0.0f;
    return cells_[index_(c.x, c.y, c.z)];
}

float DamageField::total() const {
    if (cells_.empty()) return 0.0f;
    double sum = 0.0;
    for (float c : cells_) sum += c;
    return static_cast<float>(sum / static_cast<double>(cells_.size()));
}

int DamageField::saturated_cells() const {
    int n = 0;
    for (float c : cells_) {
        if (c >= 0.999f) ++n;
    }
    return n;
}

bool DamageField::saturated_plane(glm::vec3& out_normal, float& out_distance, float& out_flatness) const {
    std::vector<glm::vec3> points;
    for (int z = 0; z < dims_.z; ++z) {
        for (int y = 0; y < dims_.y; ++y) {
            for (int x = 0; x < dims_.x; ++x) {
                if (cells_[index_(x, y, z)] >= 0.999f) points.push_back(cell_centre_(x, y, z));
            }
        }
    }
    if (points.size() < 4) return false;

    glm::vec3 mean(0.0f);
    for (const glm::vec3& p : points) mean += p;
    mean /= static_cast<float>(points.size());

    glm::mat3 cov(0.0f);
    for (const glm::vec3& p : points) {
        const glm::vec3 d = p - mean;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) cov[i][j] += d[i] * d[j];
    }
    cov /= static_cast<float>(points.size());

    const glm::mat3 axes = jacobi_eigenvectors(cov);
    glm::vec3 eigenvalues;
    for (int i = 0; i < 3; ++i) eigenvalues[i] = glm::dot(glm::vec3(axes[i]), cov * glm::vec3(axes[i]));

    int thin = 0;
    for (int i = 1; i < 3; ++i) {
        if (eigenvalues[i] < eigenvalues[thin]) thin = i;
    }
    out_normal = glm::normalize(glm::vec3(axes[thin]));
    out_distance = glm::dot(out_normal, mean);

    const float largest = std::max({eigenvalues.x, eigenvalues.y, eigenvalues.z});
    const float smallest = eigenvalues[thin];
    out_flatness = largest > 1e-8f ? std::clamp(1.0f - smallest / largest, 0.0f, 1.0f) : 0.0f;
    return true;
}

bool DamageField::saturated_bounds(glm::vec3& out_min, glm::vec3& out_max) const {
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
    bool any = false;
    for (int z = 0; z < dims_.z; ++z) {
        for (int y = 0; y < dims_.y; ++y) {
            for (int x = 0; x < dims_.x; ++x) {
                if (cells_[index_(x, y, z)] < 0.999f) continue;
                any = true;
                const glm::vec3 centre = cell_centre_(x, y, z);
                lo = glm::min(lo, centre - 0.5f * cell_);
                hi = glm::max(hi, centre + 0.5f * cell_);
            }
        }
    }
    if (!any) return false;
    out_min = lo;
    out_max = hi;
    return true;
}

} // namespace destruct
} // namespace toy
