#pragma once

/**
 * @file mesh_checks.h
 * @brief Topological invariants for EditMesh results: what every mesh operation must preserve.
 */

#include <cstdint>
#include <map>
#include <utility>

#include "editor/mesh/edit_mesh.h"
#include "editor/support/fixtures.h"

namespace toy::editor::testing {

/** @brief V - E + F over a mesh (2 for a closed genus-0 surface, 1 for a disc). */
inline long euler(const EditMesh& m) {
    return static_cast<long>(m.positions.size()) - static_cast<long>(m.edges().size()) + static_cast<long>(m.faces.size());
}

/** @brief Every edge shared by exactly two faces, used in opposite directions: closed, and
 *         consistently wound. */
inline bool closed_and_consistent(const EditMesh& m) {
    std::map<std::pair<uint32_t, uint32_t>, int> directed;
    for (const auto& f : m.faces) {
        for (size_t i = 0; i < f.corners.size(); ++i) ++directed[{f.corners[i].v, f.corners[(i + 1) % f.corners.size()].v}];
    }
    for (const auto& [e, n] : directed) {
        if (n != 1) return false;
        auto it = directed.find({e.second, e.first});
        if (it == directed.end() || it->second != 1) return false;
    }
    return true;
}

} // namespace toy::editor::testing
