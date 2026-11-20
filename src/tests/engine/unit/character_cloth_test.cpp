/**
 * @file character_cloth_test.cpp
 * @brief Character cloth without a device: the mesh YAML `cloth:` channels and the welded
 *        source mapping parse into SkinnedMeshSource, and the CharacterCloth component carries
 *        its authored fields. The live simulation and the render blend are
 *        engine/gpu/character_cloth_test.cpp.
 */

#include <coopa/testing/test.h>

#include <string>

#include <fkYAML/node.hpp>
#include <gfxcoopa/engine/data/skinned_mesh_source.h>
#include <toyengine/cloth/character_cloth.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("character_cloth");

namespace {

/** @brief A two-triangle quad whose four raw vertices carry groups and cloth paint. */
const char* k_quad_yaml =
    "vertices: [[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]]\n"
    "normals: [[0, 0, 1], [0, 0, 1], [0, 0, 1], [0, 0, 1]]\n"
    "uvs: [[0, 0], [1, 0], [1, 1], [0, 1]]\n"
    "faces: [[0, 1, 2], [0, 2, 3]]\n"
    "weights:\n"
    "  - {Spine: 1.0}\n"
    "  - {Spine: 1.0}\n"
    "  - {Spine: 0.5, Hip: 0.5}\n"
    "  - {Hip: 1.0}\n"
    "cloth:\n"
    "  max_distance: [0.0, 0.1, 0.2, 0.3]\n"
    "  backstop_distance: [0.01, 0.01, 0.02, 0.02]\n"
    "  backstop_radius: [0.1, 0.1, 0.1, 0.1]\n"
    "  anim_drive: [1.0, 0.8, 0.5, 0.2]\n";

} // namespace

/** @brief The `cloth:` key parses per RAW vertex, and the source keeps the welded topology
 *         (source_positions) plus the exact corner->raw map (source_vertex) a cloth build
 *         needs -- no positional welding anywhere. */
COOPA_TEST(mesh_cloth_channels_and_welded_mapping_parse) {
    const auto src = coopa::gfx::engine::data::SkinnedMeshSource::from_node(
        fkyaml::node::deserialize(std::string(k_quad_yaml)));

    expect(src.source_positions.size() == 4, "character_cloth: the raw vertex list is kept");
    expect(src.vertices.size() == 6 && src.source_vertex.size() == 6,
           "character_cloth: six unwelded corners, each mapped to its raw vertex");
    expect(src.source_vertex[0] == 0 && src.source_vertex[1] == 1 && src.source_vertex[2] == 2 &&
               src.source_vertex[3] == 0 && src.source_vertex[4] == 2 && src.source_vertex[5] == 3,
           "character_cloth: the corner->raw map follows the fan triangulation");

    expect(!src.cloth.empty(), "character_cloth: the cloth block parsed");
    expect(src.cloth.max_distance.size() == 4 && src.cloth.backstop_distance.size() == 4 &&
               src.cloth.backstop_radius.size() == 4 && src.cloth.anim_drive.size() == 4,
           "character_cloth: all four channels are per raw vertex");
    expect_near(src.cloth.max_distance[0], 0.0f, 1e-6f, "character_cloth: painted zero survives");
    expect_near(src.cloth.max_distance[3], 0.3f, 1e-6f, "character_cloth: painted value survives");
    expect_near(src.cloth.anim_drive[1], 0.8f, 1e-6f, "character_cloth: anim_drive survives");
}

/** @brief A wrongly sized channel array is discarded (never misindexed), and a cloth block
 *         without max_distance is ignored entirely. */
COOPA_TEST(mesh_cloth_channels_validate_their_lengths) {
    const auto bad_len = coopa::gfx::engine::data::SkinnedMeshSource::from_node(
        fkyaml::node::deserialize(std::string(
            "vertices: [[0, 0, 0], [1, 0, 0], [0, 1, 0]]\n"
            "faces: [[0, 1, 2]]\n"
            "cloth:\n"
            "  max_distance: [0.1, 0.2]\n")));
    expect(bad_len.cloth.empty(),
           "character_cloth: a max_distance of the wrong length discards the cloth block");

    const auto no_md = coopa::gfx::engine::data::SkinnedMeshSource::from_node(
        fkyaml::node::deserialize(std::string(
            "vertices: [[0, 0, 0], [1, 0, 0], [0, 1, 0]]\n"
            "faces: [[0, 1, 2]]\n"
            "cloth:\n"
            "  anim_drive: [1, 1, 1]\n")));
    expect(no_md.cloth.empty() && no_md.cloth.anim_drive.empty(),
           "character_cloth: without max_distance the other channels are not adopted");

    const auto partial = coopa::gfx::engine::data::SkinnedMeshSource::from_node(
        fkyaml::node::deserialize(std::string(
            "vertices: [[0, 0, 0], [1, 0, 0], [0, 1, 0]]\n"
            "faces: [[0, 1, 2]]\n"
            "cloth:\n"
            "  max_distance: [0.1, 0.2, 0.3]\n"
            "  backstop_radius: [0.1, 0.1]\n")));
    expect(partial.cloth.max_distance.size() == 3 && partial.cloth.backstop_radius.empty(),
           "character_cloth: a valid max_distance stays while a mis-sized sibling is dropped");
}

/** @brief The component's authored fields and defaults -- what the YAML parser writes into. */
COOPA_TEST(component_defaults_are_sane) {
    toy::cloth::CharacterCloth cc;
    expect(cc.type_name() == "CharacterCloth", "character_cloth: type name");
    expect(cc.enabled && cc.collide_world, "character_cloth: enabled and colliding by default");
    expect_near(cc.mass_per_area, 0.2f, 1e-6f, "character_cloth: default areal density");
    expect(cc.params.anim_drive_compliance > 0.0f,
           "character_cloth: anim drive enabled-by-paint (compliance non-zero)");
    expect(cc.bone_colliders.empty(), "character_cloth: no bone colliders until authored");
    expect(cc.state_key() == "<orphan>", "character_cloth: unowned component keys as orphan");
}
