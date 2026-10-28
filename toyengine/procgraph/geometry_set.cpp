/**
 * @file geometry_set.cpp
 * @brief MeshGeometry's operations and GeometrySet's merge.
 */

#include <toyengine/procgraph/geometry_set.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>

namespace toy {
namespace procgraph {

glm::mat4 PointCloud::matrix_of(size_t i) const {
    if (i >= positions.size()) return glm::mat4(1.0f);
    glm::mat4 m = glm::translate(glm::mat4(1.0f), positions[i]);
    m *= glm::mat4_cast(rotation_of(i));
    return glm::scale(m, scale_of(i));
}

void MeshGeometry::append(const MeshGeometry& other) {
    if (other.empty()) return;

    const auto base_vertex = static_cast<uint32_t>(vertices.size());
    const size_t own_triangles = triangle_count();

    // Slot names are merged by name rather than by index: two meshes built independently both
    // start their slots at 0, so concatenating the index spaces would silently repaint one of
    // them with the other's materials.
    std::vector<uint32_t> slot_remap(std::max<size_t>(other.slot_names.size(), 1), 0);
    if (other.slot_names.empty()) {
        if (slot_names.empty()) slot_names.emplace_back();
        slot_remap[0] = 0;
    } else {
        for (size_t i = 0; i < other.slot_names.size(); ++i) {
            auto it = std::find(slot_names.begin(), slot_names.end(), other.slot_names[i]);
            if (it == slot_names.end()) {
                slot_remap[i] = static_cast<uint32_t>(slot_names.size());
                slot_names.push_back(other.slot_names[i]);
            } else {
                slot_remap[i] = static_cast<uint32_t>(std::distance(slot_names.begin(), it));
            }
        }
    }

    vertices.insert(vertices.end(), other.vertices.begin(), other.vertices.end());
    indices.reserve(indices.size() + other.indices.size());
    for (uint32_t i : other.indices) indices.push_back(i + base_vertex);

    // face_slots is dense once either side uses it, so an append cannot leave a hole that
    // slot_of() would silently read as slot 0.
    const bool need_slots = !face_slots.empty() || !other.face_slots.empty() ||
                            slot_names.size() > 1;
    if (need_slots) {
        face_slots.resize(own_triangles, 0u);
        for (size_t t = 0; t < other.triangle_count(); ++t) {
            uint32_t src = other.slot_of(t);
            face_slots.push_back(src < slot_remap.size() ? slot_remap[src] : 0u);
        }
    }
}

void MeshGeometry::recompute_normals() {
    for (auto& v : vertices) v.normal = glm::vec3(0.0f);

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t ia = indices[i];
        const uint32_t ib = indices[i + 1];
        const uint32_t ic = indices[i + 2];
        if (ia >= vertices.size() || ib >= vertices.size() || ic >= vertices.size()) continue;
        const glm::vec3& a = vertices[ia].position;
        const glm::vec3& b = vertices[ib].position;
        const glm::vec3& c = vertices[ic].position;
        // Unnormalized, so a larger triangle contributes proportionally more -- the usual
        // area-weighted accumulation, which behaves better on irregular meshes than a plain mean.
        const glm::vec3 face = glm::cross(b - a, c - a);
        vertices[ia].normal += face;
        vertices[ib].normal += face;
        vertices[ic].normal += face;
    }

    for (auto& v : vertices) {
        const float len = glm::length(v.normal);
        v.normal = len > 1e-12f ? v.normal / len : glm::vec3(0.0f, 0.0f, 1.0f);
        // A tangent perpendicular to the (new) normal, so normal mapping has a usable basis even
        // on generated geometry that carries no UV gradient worth deriving one from.
        const glm::vec3 up = std::abs(v.normal.z) < 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                          : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 t = glm::normalize(glm::cross(up, v.normal));
        v.tangent = glm::vec4(t, 1.0f);
    }
}

MeshCpuData MeshGeometry::to_cpu_data(bool bounds_from_vertices) const {
    MeshCpuData out;
    out.vertices   = vertices;
    out.slot_names = slot_names;

    const size_t triangles = triangle_count();
    const size_t slots = std::max<size_t>(slot_names.size(), 1);

    // Triangles are emitted grouped by slot so each part is one contiguous index range, which is
    // what MeshLod::parts means and what lets the renderer issue one draw per material.
    out.indices.reserve(indices.size());
    coopa::gfx::engine::data::MeshLod lod;
    lod.first_index   = 0;
    lod.index_count   = static_cast<uint32_t>(indices.size());
    lod.vertex_offset = 0;

    if (slots <= 1) {
        out.indices = indices;
    } else {
        lod.parts.reserve(slots);
        for (uint32_t slot = 0; slot < slots; ++slot) {
            const auto first = static_cast<uint32_t>(out.indices.size());
            for (size_t t = 0; t < triangles; ++t) {
                if (slot_of(t) != slot) continue;
                out.indices.push_back(indices[t * 3 + 0]);
                out.indices.push_back(indices[t * 3 + 1]);
                out.indices.push_back(indices[t * 3 + 2]);
            }
            coopa::gfx::engine::data::MeshPart part;
            part.first_index = first;
            part.index_count = static_cast<uint32_t>(out.indices.size()) - first;
            lod.parts.push_back(part);
        }
    }
    out.lods.push_back(std::move(lod));

    if (bounds_from_vertices && !vertices.empty()) {
        glm::vec3 lo = vertices.front().position;
        glm::vec3 hi = lo;
        for (const auto& v : vertices) {
            lo = glm::min(lo, v.position);
            hi = glm::max(hi, v.position);
        }
        out.bounds_min = lo;
        out.bounds_max = hi;
    }
    return out;
}

void GeometrySet::merge(const GeometrySet& other) {
    if (other.empty()) return;
    Data& mine = mutable_data();

    if (const MeshGeometry* m = other.mesh()) {
        if (mine.mesh.has_value()) mine.mesh->append(*m);
        else mine.mesh = *m;
    }

    if (const PointCloud* p = other.points()) {
        if (!mine.points.has_value()) {
            mine.points = *p;
        } else {
            PointCloud& dst = *mine.points;
            const size_t before = dst.positions.size();
            dst.positions.insert(dst.positions.end(), p->positions.begin(), p->positions.end());
            // Both optional arrays become dense if either side had them, so an index past the end
            // cannot quietly read as identity for points that did carry a frame.
            const bool need_rot = !dst.rotations.empty() || !p->rotations.empty();
            const bool need_scale = !dst.scales.empty() || !p->scales.empty();
            if (need_rot) {
                dst.rotations.resize(before, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
                for (size_t i = 0; i < p->size(); ++i) dst.rotations.push_back(p->rotation_of(i));
            }
            if (need_scale) {
                dst.scales.resize(before, glm::vec3(1.0f));
                for (size_t i = 0; i < p->size(); ++i) dst.scales.push_back(p->scale_of(i));
            }
        }
    }

    for (const InstanceGroup& g : other.instances()) {
        // Groups sharing a prototype are combined, so a hundred Instance-on-Points nodes over the
        // same asset publish one batch rather than a hundred.
        auto it = std::find_if(mine.instances.begin(), mine.instances.end(),
                               [&g](const InstanceGroup& existing) {
                                   return existing.prototype == g.prototype;
                               });
        if (it == mine.instances.end()) {
            mine.instances.push_back(g);
            continue;
        }
        const size_t before = it->transforms.size();
        it->transforms.insert(it->transforms.end(), g.transforms.begin(), g.transforms.end());
        const bool need_seeds = !it->seeds.empty() || !g.seeds.empty();
        if (need_seeds) {
            it->seeds.resize(before, 0u);
            for (size_t i = 0; i < g.transforms.size(); ++i) {
                it->seeds.push_back(i < g.seeds.size() ? g.seeds[i] : static_cast<uint32_t>(i));
            }
        }
    }
}

std::string GeometrySet::describe() const {
    if (empty()) return "empty";
    std::string out;
    if (const MeshGeometry* m = mesh()) {
        out += std::to_string(m->vertices.size()) + " verts / " +
               std::to_string(m->triangle_count()) + " tris";
    }
    if (const PointCloud* p = points()) {
        if (!out.empty()) out += ", ";
        out += std::to_string(p->size()) + " points";
    }
    if (has_instances()) {
        size_t total = 0;
        for (const auto& g : instances()) total += g.size();
        if (!out.empty()) out += ", ";
        out += std::to_string(total) + " instances in " + std::to_string(instances().size()) +
               " group(s)";
    }
    return out;
}

} // namespace procgraph
} // namespace toy
