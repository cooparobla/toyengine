/**
 * @file geometry_set.h
 * @brief `GeometrySet` -- what flows along a Geometry socket.
 *
 * A bundle of optional parts (a mesh, a point cloud, instance groups) behind a single
 * `shared_ptr`, which is the whole trick: a `GeometrySet` is **16 bytes**, so it fits inside a
 * `coopa::nodegraph::Value`'s inline buffer and copying one along a link copies a pointer and
 * bumps a refcount. Fanning a 200k-vertex mesh out to four consumers costs four refcount bumps.
 *
 * Writes go through `mutable_data()`, which clones first if anyone else is holding the same
 * payload -- ordinary copy-on-write. So a node that modifies geometry pays for exactly one copy,
 * and only when the input is genuinely shared; a node at the end of a linear chain (the common
 * case, since the compiler fuses those) usually holds the only reference and modifies in place.
 *
 * ### Why a mesh here is not a `MeshCpuData`
 *
 * `MeshCpuData` expresses material slots as index *ranges* (`MeshLod::parts`), which means every
 * operation that adds or reorders triangles has to maintain sorted-by-slot index order. Inside a
 * graph that is the wrong representation: a node wants to append a triangle and say which slot it
 * is in. So `MeshGeometry` carries a per-triangle slot id, and `to_cpu_data()` does the sort into
 * contiguous parts once, at the end. Same reason `gfx`'s own YAML loader has a `sort_by_slot_`
 * step rather than requiring authored files to be pre-sorted.
 *
 * Pure CPU: no Vulkan objects, no `Scene`, no `AssetManager`. That is what lets it be unit-tested
 * without a device, as `env_field.h` and `destruct/fracture.h` are.
 */

#ifndef TOYENGINE_PROCGRAPH_GEOMETRY_SET_H
#define TOYENGINE_PROCGRAPH_GEOMETRY_SET_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <gfxcoopa/engine/data/mesh.h>

namespace toy {
namespace procgraph {

using Vertex      = coopa::gfx::engine::data::Vertex;
using MeshCpuData = coopa::gfx::engine::data::MeshCpuData;

/**
 * @struct MeshGeometry
 * @brief Triangles, in the form graph operations want them.
 *
 * `indices` is always a multiple of three; `face_slots`, when non-empty, has one entry per
 * triangle naming an index into `slot_names`.
 */
struct MeshGeometry {
    std::vector<Vertex>      vertices;
    std::vector<uint32_t>    indices;
    std::vector<uint32_t>    face_slots;   ///< Per triangle. Empty means every triangle is slot 0.
    std::vector<std::string> slot_names;   ///< Empty means one unnamed slot.

    size_t triangle_count() const { return indices.size() / 3; }
    bool   empty() const { return indices.empty() || vertices.empty(); }

    /// @brief The slot a triangle belongs to, tolerating an empty `face_slots`.
    uint32_t slot_of(size_t triangle) const {
        return triangle < face_slots.size() ? face_slots[triangle] : 0u;
    }

    /// @brief Appends `other`'s geometry, offsetting its indices and remapping its slots.
    void append(const MeshGeometry& other);

    /// @brief Recomputes every vertex normal from the faces that share it, then re-tangents.
    void recompute_normals();

    /**
     * @brief Converts to the engine's upload form, sorting triangles into contiguous slot parts.
     * @param bounds_from_vertices Recompute bounds (false leaves them zeroed, as a caller that
     *        already knows them may prefer).
     */
    MeshCpuData to_cpu_data(bool bounds_from_vertices = true) const;
};

/**
 * @struct PointCloud
 * @brief Positions with an optional per-point frame, the currency of scatter and instancing.
 *
 * `rotations` and `scales` are either empty (identity for every point) or exactly as long as
 * `positions`. Keeping them optional rather than always-present means a plain scatter of a
 * million points does not pay for a million quaternions it never uses.
 */
struct PointCloud {
    std::vector<glm::vec3> positions;
    std::vector<glm::quat> rotations;   ///< Empty means identity everywhere.
    std::vector<glm::vec3> scales;      ///< Empty means unit scale everywhere.

    size_t size() const { return positions.size(); }
    bool   empty() const { return positions.empty(); }

    glm::quat rotation_of(size_t i) const {
        return i < rotations.size() ? rotations[i] : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    glm::vec3 scale_of(size_t i) const {
        return i < scales.size() ? scales[i] : glm::vec3(1.0f);
    }
    /// @brief Point `i`'s full transform.
    glm::mat4 matrix_of(size_t i) const;

    void reserve(size_t n) { positions.reserve(n); }
};

/**
 * @struct InstanceGroup
 * @brief Many transforms of one prototype.
 *
 * `prototype` names an object asset (`objects/rock`) or a mesh (`meshes/rock`); which one it is
 * is decided by the publishing side, not here. This maps onto the engine's existing
 * `InstancePainter` prototypes, which is why a graph gets instanced rendering, culling, distance
 * thinning and streamed collision without this module owning any of it.
 */
struct InstanceGroup {
    std::string            prototype;
    /**
     * @brief Whether `prototype` names a bare mesh rather than an object asset.
     *
     * `InstancePrototype` takes EITHER -- they are not interchangeable, and handing a mesh path
     * to its `object` field fails to instantiate with nothing on screen. Carrying the choice on
     * the group rather than guessing from the path is what lets publishing build the component
     * correctly without a naming convention.
     */
    bool                   prototype_is_mesh = false;
    std::vector<glm::mat4> transforms;
    /// @brief Per instance, a stable variation seed. Empty means derive one from the index.
    std::vector<uint32_t>  seeds;

    size_t size() const { return transforms.size(); }
    bool   empty() const { return transforms.empty(); }
};

/**
 * @class GeometrySet
 * @brief An immutable-by-default bundle of geometry parts, shared by refcount and cloned on write.
 */
class GeometrySet {
public:
    /// @brief The parts. Held behind one shared_ptr so a GeometrySet is pointer-sized.
    struct Data {
        std::optional<MeshGeometry> mesh;
        std::optional<PointCloud>   points;
        std::vector<InstanceGroup>  instances;
    };

    GeometrySet() = default;

    /// @brief A set holding just a mesh.
    static GeometrySet from_mesh(MeshGeometry mesh) {
        GeometrySet out;
        out.mutable_data().mesh = std::move(mesh);
        return out;
    }

    /// @brief A set holding just points.
    static GeometrySet from_points(PointCloud points) {
        GeometrySet out;
        out.mutable_data().points = std::move(points);
        return out;
    }

    /// @brief A set holding one instance group.
    static GeometrySet from_instances(InstanceGroup group) {
        GeometrySet out;
        out.mutable_data().instances.push_back(std::move(group));
        return out;
    }

    bool empty() const {
        if (!data_) return true;
        return !has_mesh() && !has_points() && data_->instances.empty();
    }

    bool has_mesh() const { return data_ && data_->mesh.has_value() && !data_->mesh->empty(); }
    bool has_points() const { return data_ && data_->points.has_value() && !data_->points->empty(); }
    bool has_instances() const { return data_ && !data_->instances.empty(); }

    /// @brief The mesh, or nullptr.
    const MeshGeometry* mesh() const {
        return data_ && data_->mesh.has_value() ? &*data_->mesh : nullptr;
    }
    /// @brief The points, or nullptr.
    const PointCloud* points() const {
        return data_ && data_->points.has_value() ? &*data_->points : nullptr;
    }
    const std::vector<InstanceGroup>& instances() const {
        static const std::vector<InstanceGroup> none;
        return data_ ? data_->instances : none;
    }

    /**
     * @brief Writable access to the parts, cloning the payload first if it is shared.
     *
     * After this returns, this GeometrySet is the sole owner of its Data, so the caller may
     * modify it freely without disturbing any other holder.
     */
    Data& mutable_data() {
        if (!data_) {
            data_ = std::make_shared<Data>();
        } else if (data_.use_count() > 1) {
            data_ = std::make_shared<Data>(*data_);
        }
        return *data_;
    }

    /// @brief Whether another GeometrySet shares this payload. For a test, mostly.
    bool shared() const { return data_ && data_.use_count() > 1; }

    /// @brief Total triangles, points and instances, for a diagnostic line.
    std::string describe() const;

    /// @brief Everything from `other` merged in, in place.
    void merge(const GeometrySet& other);

    bool operator==(const GeometrySet& o) const { return data_ == o.data_; }

private:
    std::shared_ptr<Data> data_;
};

static_assert(sizeof(GeometrySet) <= 32,
              "GeometrySet must fit a nodegraph Value's inline buffer, or every socket copy "
              "allocates -- the property the whole COW design exists to provide");

} // namespace procgraph
} // namespace toy

#endif // TOYENGINE_PROCGRAPH_GEOMETRY_SET_H
