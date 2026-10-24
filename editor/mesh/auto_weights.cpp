#include "editor/mesh/auto_weights.h"

#include <algorithm>

namespace toy {
namespace editor {

namespace {

/** @brief Drops the file's explicit skinning palette, which would otherwise outrank the groups. */
void clear_explicit_palette(EditMesh& m) {
    m.joints.clear();
    m.joint_weights.clear();
    erase_key(m.passthrough, "inverse_bind_matrices");
}

/** @brief The groups `bones` claims, created where the mesh has none, and emptied of old weights. */
std::vector<uint32_t> prepare_bone_groups(EditMesh& m, const std::vector<BoneSegments>& bones,
                                          std::vector<std::string>& foreign) {
    std::vector<uint32_t> indices;
    indices.reserve(bones.size());
    for (const BoneSegments& b : bones) indices.push_back(m.group_index(b.name));
    m.sync_vertex_data();
    if (m.weights.size() < m.positions.size()) m.weights.resize(m.positions.size());
    for (auto& list : m.weights) {
        list.erase(std::remove_if(list.begin(), list.end(), [&](const VertexWeight& w) {
                       return std::find(indices.begin(), indices.end(), w.group) != indices.end();
                   }),
                   list.end());
    }
    for (uint32_t g = 0; g < m.groups.size(); ++g) {
        if (std::find(indices.begin(), indices.end(), g) == indices.end()) foreign.push_back(m.groups[g]);
    }
    return indices;
}

} // namespace

std::vector<uint32_t> mesh_triangles(const EditMesh& m) {
    std::vector<uint32_t> out;
    for (const Face& f : m.faces) {
        for (size_t i = 1; i + 1 < f.corners.size(); ++i) {
            out.insert(out.end(), {f.corners[0].v, f.corners[i].v, f.corners[i + 1].v});
        }
    }
    return out;
}

AutoWeightReport apply_auto_weights(EditMesh& m, const std::vector<BoneSegments>& bones,
                                    const AutoWeightSettings& settings) {
    AutoWeightReport report;
    if (bones.empty() || m.positions.empty()) return report;

    const std::vector<uint32_t> triangles = mesh_triangles(m);
    const toy::scene::AutoWeights generated =
        toy::scene::compute_auto_weights(m.positions, triangles, bones, settings);

    const std::vector<uint32_t> groups = prepare_bone_groups(m, bones, report.foreign_groups);
    for (uint32_t v = 0; v < m.positions.size() && v < generated.weights.size(); ++v) {
        for (const auto& [bone, weight] : generated.weights[v]) {
            if (bone < 0 || static_cast<size_t>(bone) >= groups.size() || weight <= 0.0f) continue;
            m.weights[v].push_back({groups[static_cast<size_t>(bone)], weight});
        }
        if (!m.weights[v].empty()) ++report.weighted_vertices;
    }
    clear_explicit_palette(m);

    report.groups = groups.size();
    report.unsolved = generated.unsolved;
    report.fell_back_to_envelope = generated.fell_back_to_envelope;
    return report;
}

AutoWeightReport add_empty_bone_groups(EditMesh& m, const std::vector<BoneSegments>& bones) {
    AutoWeightReport report;
    if (bones.empty()) return report;
    report.groups = prepare_bone_groups(m, bones, report.foreign_groups).size();
    clear_explicit_palette(m);
    return report;
}

} // namespace editor
} // namespace toy
