/**
 * @file particle_shape.h
 * @brief Where particles are born and which way they leave: Unity's Shape module (point, sphere,
 *        hemisphere, cone, box, circle, edge) plus Blender's mesh emitter -- spawning on a mesh's
 *        faces, vertices or edges, with the surface normal and tangent carried to the particle so
 *        it can be oriented to the surface.
 *
 * Every sample is in the emitter's LOCAL space; ParticleSystem moves it to world space (or keeps
 * it local, for `simulation_space: local`). Pure CPU, no scene or GPU dependency.
 *
 * ## The mesh surface
 *
 * MeshSurface is built once from a mesh's triangles (any source of positions/normals/indices --
 * the engine hands it a SkinnedMeshSource, the CPU-side mesh WaterBody also bakes from) and
 * answers sample(index, rng) in O(log n):
 *
 * - **faces**: a point on a triangle chosen with probability proportional to its AREA (a
 *   cumulative-area table, binary searched), uniformly within it (the sqrt-barycentric map), so
 *   density is even over the surface however the mesh is tessellated -- Blender's "Random".
 *   The normal is the interpolated vertex normal (smooth meshes emit smoothly), or the face
 *   normal when the mesh carries none.
 * - **even** distribution: the i-th of N samples takes the area CDF at (i + jitter) / N
 *   instead of a fresh random number -- stratified sampling, Blender's "Jittered" -- so a scatter
 *   of a few hundred instances covers the surface without the clumps and holes pure random
 *   leaves.
 * - **vertices**: one of the mesh's distinct vertices (corners welded by position).
 * - **edges**: a point along an edge, chosen proportional to edge length.
 *
 * The tangent is the mesh's own (its UV +U direction) when it has one, else the first edge of
 * the triangle, so an `aligned` sprite or an instanced mesh keeps a consistent twist across a
 * face instead of spinning randomly -- unless `random_spin` asks for that.
 */

#ifndef TOYENGINE_PARTICLES_PARTICLE_SHAPE_H
#define TOYENGINE_PARTICLES_PARTICLE_SHAPE_H

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <toyengine/particles/particle_math.h>

namespace toy {
namespace particles {

enum class EmitShape { Point, Sphere, Hemisphere, Cone, Box, Circle, Edge, Mesh };
enum class MeshEmitFrom { Faces, Vertices, Edges };
enum class MeshDistribution { Random, Even };

/** @brief One emission sample, emitter-local. `normal`/`tangent` frame the particle when it is
 *         aligned to the surface; `direction` is where start_speed sends it. */
struct EmitSample {
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, 0.0f, 1.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec3 tangent{1.0f, 0.0f, 0.0f};
};

/**
 * @class MeshSurface
 * @brief A mesh's triangles, ready to be sampled by area, vertex or edge. See the file doc.
 */
class MeshSurface {
public:
    MeshSurface() = default;

    /**
     * @brief Builds the sampling tables.
     * @param positions Triangle-corner positions (object space).
     * @param normals   Parallel to positions, or empty (face normals are used).
     * @param tangents  Parallel to positions (xyz used), or empty.
     * @param indices   Three per triangle; empty means positions are already a triangle list.
     */
    void build(const std::vector<glm::vec3>& positions, const std::vector<glm::vec3>& normals,
               const std::vector<glm::vec3>& tangents, const std::vector<uint32_t>& indices);

    bool empty() const { return tris_.empty(); }
    size_t triangle_count() const { return tris_.size(); }
    size_t vertex_count() const { return verts_.size(); }
    size_t edge_count() const { return edges_.size(); }
    float area() const { return total_area_; }
    glm::vec3 bounds_min() const { return bmin_; }
    glm::vec3 bounds_max() const { return bmax_; }

    /**
     * @brief One sample. `u` in [0, 1) picks the element through its CDF (a fresh random number
     *        for `random`, a stratified one for `even`); `rng` places the point within it.
     */
    EmitSample sample(MeshEmitFrom from, float u, Rng& rng) const;

    /**
     * @brief The faces for the GPU emitter (particles_emit.comp): 8 vec4 per triangle -- p0
     *        (w = cumulative area, the CDF), p1, p2, n0, n1, n2, face normal, tangent.
     */
    std::vector<glm::vec4> export_faces() const;

private:
    struct Tri {
        glm::vec3 p[3];
        glm::vec3 n[3];
        glm::vec3 face_n{0.0f, 0.0f, 1.0f};
        glm::vec3 tangent{1.0f, 0.0f, 0.0f};
    };
    struct Vert {
        glm::vec3 p{0.0f};
        glm::vec3 n{0.0f};
    };

    static size_t search_(const std::vector<float>& cdf, float x);
    static glm::vec3 any_perpendicular_(const glm::vec3& n);

    std::vector<Tri>   tris_;
    std::vector<float> face_cdf_;
    std::vector<Vert>  verts_;
    std::vector<std::pair<uint32_t, uint32_t>> edges_;
    std::vector<float> edge_cdf_;
    float total_area_ = 0.0f;
    glm::vec3 bmin_{0.0f}, bmax_{0.0f};
};

/**
 * @struct ShapeSettings
 * @brief Unity's Shape module plus Blender's mesh emitter. Analytic shapes emit along their
 *        local +Z (up, in this engine's Z-up world), so a default cone is a fountain.
 */
struct ShapeSettings {
    EmitShape type = EmitShape::Cone;
    float radius = 0.5f;
    /// 0: born on the shell (sphere / circle / cone base rim) only; 1: anywhere in the volume.
    float radius_thickness = 1.0f;
    float angle_deg = 25.0f;            ///< Cone half-angle.
    float arc_deg = 360.0f;             ///< Sphere / circle / cone sweep around +Z.
    glm::vec3 box{1.0f};                ///< Box full extents.
    float length = 1.0f;                ///< Edge: along local X, centred.
    glm::vec3 offset{0.0f};             ///< Added to every sample.
    /// Blend each emission direction toward a random one (0 = the shape's, 1 = fully random).
    float random_direction = 0.0f;

    // --- mesh (Blender) ---
    std::string      mesh_path;          ///< meshes/<mesh_path>.yaml, sampled CPU-side.
    MeshEmitFrom     emit_from = MeshEmitFrom::Faces;
    MeshDistribution distribution = MeshDistribution::Random;
    float            normal_offset = 0.0f;   ///< Push the spawn point off the surface (m).
};

/**
 * @brief One analytic-shape sample (emitter-local). Mesh shapes go through MeshSurface instead
 *        (see ParticleSystem::sample_shape_()), since they need the loaded surface.
 */
EmitSample sample_analytic_shape(const ShapeSettings& s, Rng& rng);

/** @brief Applies offset and random_direction, shared by every shape. */
void finish_shape_sample(const ShapeSettings& s, EmitSample& e, Rng& rng);

} // namespace particles
} // namespace toy

#endif // TOYENGINE_PARTICLES_PARTICLE_SHAPE_H
