/**
 * @file tile_mesh_library.h
 * @brief The per-side tile geometry, baked into its six orientations, and the primitive that
 *        stamps one side into a chunk's merged vertex/index buffers.
 *
 * This is the file that makes "a tile side is a mesh" true rather than a figure of speech. A
 * side is authored once as an ordinary mesh YAML -- the same Blender-exported schema every
 * other mesh in this repo uses -- in the canonical orientation tile_types.h describes (the +Z
 * face of a unit cube spanning `[0,1]^3`). This class rotates it onto all six faces ONCE at
 * load, and from then on stamping a side into a chunk is a translate, a UV remap and an index
 * rebase. Nothing downstream knows whether that side is a flat quad, a chamfered rim or a
 * curved patch -- which is exactly the seam smoother tiles will arrive through.
 *
 * ### Why the geometry is copied rather than referenced
 *
 * Chunk meshing runs on coopa::job worker threads while the main thread may still be
 * publishing assets. AssetHandle is a refcounted view onto a slot the AssetManager mutates, so
 * a worker dereferencing one races that. bake() therefore takes a plain `const
 * SkinnedMeshSource&` on the main thread and **copies** the vertices out; a baked library is
 * immutable, self-contained, and safe for unlimited concurrent readers. It also means a test
 * can build a library from hand-written triangles without an AssetManager, a Device, or a file.
 *
 * ### Why SkinnedMeshSource
 *
 * coopa::gfx::engine::data::SkinnedMeshSource is gfxcoopa's CPU-only mesh payload (see its file
 * doc): its `joints`/`joint_weights` keys are optional, so it parses a plain mesh YAML verbatim
 * -- quads triangulated, tangents generated when absent -- and holds no GPU resources. Its
 * loader is already registered by toy::core::Engine, so authored side meshes need no new asset
 * type and no new loader. A plain coopa::gfx::engine::data::Mesh would be the wrong tool: it
 * uploads to the GPU and does not retain the CPU arrays a merge needs.
 */

#ifndef TOYENGINE_WORLD_TILE_MESH_LIBRARY_H
#define TOYENGINE_WORLD_TILE_MESH_LIBRARY_H

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/engine/data/skinned_mesh_source.h>

#include <toyengine/world/tile_types.h>

namespace toy {
namespace world {

/**
 * @class TileMeshLibrary
 * @brief Six baked side geometries, and append() to stamp one into a chunk buffer.
 *
 * @code
 * TileMeshLibrary lib;
 * lib.bake_canonical(*flat_quad_source);            // all six faces from one authored mesh
 * lib.bake_side(TileFace::Top, *bevelled_source);   // ...overriding just the top
 *
 * lib.append(TileFace::Top, origin, scale, TileKind::Grass, vertices, indices);
 * @endcode
 */
class TileMeshLibrary {
public:
    using Vertex = coopa::gfx::engine::data::Vertex;
    using SkinnedMeshSource = coopa::gfx::engine::data::SkinnedMeshSource;

    /**
     * @struct SideGeometry
     * @brief One face's geometry, already rotated out of canonical space onto that face.
     *
     * Positions still span the unit cube `[0,1]^3`; append() applies the tile's scale and
     * origin. UVs are the authored `[0,1]` ones; append() maps them into an atlas cell.
     */
    struct SideGeometry {
        std::vector<Vertex>        vertices;
        std::vector<std::uint32_t> indices;

        bool empty() const { return vertices.empty() || indices.empty(); }
    };

    /**
     * @brief Bakes one authored mesh onto all six faces.
     *
     * The ordinary case: one `tile_side_flat.yaml` becomes a whole cube's worth of sides.
     * Call bake_side() afterwards to override individual faces.
     *
     * @param canonical A side mesh authored in canonical orientation (+Z face of `[0,1]^3`).
     */
    void bake_canonical(const SkinnedMeshSource& canonical) {
        for (std::size_t i = 0; i < k_tile_face_count; ++i) {
            bake_side(static_cast<TileFace>(i), canonical);
        }
    }

    /**
     * @brief Bakes one authored mesh onto a single face, replacing whatever was there.
     *
     * @param face      The face to orient onto.
     * @param canonical A side mesh authored in canonical orientation (+Z face of `[0,1]^3`).
     */
    void bake_side(TileFace face, const SkinnedMeshSource& canonical) {
        SideGeometry& out = sides_[static_cast<std::size_t>(face)];
        out.vertices.clear();
        out.indices = canonical.indices;

        const glm::mat4 transform = face_transform(face);
        // A pure rotation, so the inverse transpose is the rotation itself -- normals and
        // tangents take the same matrix as positions, with no renormalisation needed.
        const glm::mat3 rotation(transform);

        out.vertices.reserve(canonical.vertices.size());
        for (const Vertex& v : canonical.vertices) {
            Vertex rotated = v;
            rotated.position = glm::vec3(transform * glm::vec4(v.position, 1.0f));
            rotated.normal   = rotation * v.normal;
            rotated.tangent  = glm::vec4(rotation * glm::vec3(v.tangent), v.tangent.w);
            out.vertices.push_back(rotated);
        }
    }

    /** @brief True once every face has geometry -- i.e. a chunk built from this can draw. */
    bool is_baked() const {
        for (const SideGeometry& side : sides_) {
            if (side.empty()) return false;
        }
        return true;
    }

    /** @brief Read-only access to one face's baked geometry, for tests and diagnostics. */
    const SideGeometry& side(TileFace face) const {
        return sides_[static_cast<std::size_t>(face)];
    }

    /**
     * @brief Stamps one side into a chunk's merged buffers.
     *
     * The inner loop of chunk meshing: everything expensive (rotation, tangent transport) was
     * done once at bake time, leaving a scale, a translate, a UV remap and an index rebase.
     *
     * Non-uniform `scale` is supported because a tile's vertical step need not equal its
     * horizontal size, and it is handled correctly rather than approximately: positions take
     * the scale, normals and tangents take its component-wise INVERSE (the inverse transpose of
     * a diagonal matrix), then renormalise. Skipping that would tilt the shading of every
     * lateral face the moment height_step and tile_size diverged.
     *
     * @param face      Which baked side to stamp.
     * @param origin    World position of the tile cell's minimum corner.
     * @param scale     Per-axis size of one cell: `{tile_size, tile_size, height_step}`.
     * @param kind      Surface to texture it with; selects the atlas cell.
     * @param out_v     Vertex buffer to append to.
     * @param out_i     Index buffer to append to; rebased onto `out_v`'s current end.
     */
    void append(TileFace face, const glm::vec3& origin, const glm::vec3& scale, TileKind kind,
                std::vector<Vertex>& out_v, std::vector<std::uint32_t>& out_i) const {
        const SideGeometry& side = sides_[static_cast<std::size_t>(face)];
        if (side.empty()) return;

        const std::uint32_t base = static_cast<std::uint32_t>(out_v.size());
        const glm::vec4 cell = atlas_cell(kind);
        const glm::vec3 normal_scale(1.0f / scale.x, 1.0f / scale.y, 1.0f / scale.z);

        out_v.reserve(out_v.size() + side.vertices.size());
        for (const Vertex& v : side.vertices) {
            Vertex placed;
            placed.position = origin + v.position * scale;
            placed.normal   = glm::normalize(v.normal * normal_scale);

            const glm::vec3 tangent = glm::vec3(v.tangent) * scale; // a tangent scales, unlike a normal
            const float tangent_length = glm::length(tangent);
            placed.tangent = glm::vec4(tangent_length > 1e-6f ? tangent / tangent_length
                                                              : glm::vec3(1.0f, 0.0f, 0.0f),
                                       v.tangent.w);

            placed.uv = glm::vec2(cell.x + v.uv.x * cell.z, cell.y + v.uv.y * cell.w);
            out_v.push_back(placed);
        }

        out_i.reserve(out_i.size() + side.indices.size());
        for (std::uint32_t index : side.indices) out_i.push_back(base + index);
    }

private:
    /** @brief One baked geometry per TileFace, indexed by `static_cast<std::size_t>(face)`. */
    std::array<SideGeometry, k_tile_face_count> sides_;
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TILE_MESH_LIBRARY_H
