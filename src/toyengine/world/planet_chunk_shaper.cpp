/**
 * @file planet_chunk_shaper.cpp
 * @brief See planet_chunk_shaper.h. TRANSITIONAL: moves to the planet project.
 */

#include <toyengine/world/planet_chunk_shaper.h>

#include <algorithm>
#include <cmath>

#include <toyengine/world/planet.h>
#include <toyengine/world/terrain_chunk.h>
#include <toyengine/world/terrain_params.h>

namespace toy {
namespace world {

glm::vec3 PlanetChunkShaper::chunk_origin(const TerrainParams&, const ChunkCoord&) const {
    return glm::vec3(0.0f);   // planet chunks are built round the planet's centre
}

void PlanetChunkShaper::shape(const TerrainParams& params, const ChunkCoord& coord,
                              ChunkMeshData& mesh, ChunkWaterData& still,
                              ChunkWaterData& flowing) const {
    const PlanetLayout layout = params.layout();
    const std::int32_t base_x = coord.x * params.chunk_size;
    const std::int32_t face = base_x / layout.n;
    const double x0 = static_cast<double>(base_x - face * layout.n) * params.tile_size;
    const double y0 = static_cast<double>(coord.y * params.chunk_size) * params.tile_size;
    const double eps = 1e-3 * params.tile_size;
    const auto map = [&](double x, double y, double z) { return layout.to_planet(face, x0 + x, y0 + y, z); };
    for (auto& v : mesh.vertices) {
        const double x = v.position.x, y = v.position.y, z = v.position.z;
        const glm::dvec3 p = map(x, y, z);
        const glm::dvec3 jx = (map(x + eps, y, z) - map(x - eps, y, z)) / (2.0 * eps);
        const glm::dvec3 jy = (map(x, y + eps, z) - map(x, y - eps, z)) / (2.0 * eps);
        const glm::dvec3 jz = (map(x, y, z + eps) - map(x, y, z - eps)) / (2.0 * eps);
        // Normals transform by the cofactor matrix: n' ~ (jy x jz) n.x + (jz x jx) n.y + (jx x jy) n.z.
        const glm::dvec3 n(v.normal);
        glm::dvec3 nn = glm::cross(jy, jz) * n.x + glm::cross(jz, jx) * n.y + glm::cross(jx, jy) * n.z;
        const double len = glm::length(nn);
        nn = len > 0.0 ? nn / len : glm::normalize(p);
        v.position = glm::vec3(p);
        v.normal = glm::vec3(nn);
        const glm::dvec3 axis = std::abs(nn.z) < 0.9 ? glm::dvec3(0.0, 0.0, 1.0) : glm::dvec3(1.0, 0.0, 0.0);
        v.tangent = glm::vec4(glm::vec3(glm::normalize(glm::cross(axis, nn))), 1.0f);
        v.uv.y = static_cast<float>(glm::dot(nn, glm::normalize(p)));
    }
    // The water goes into a frame of its own, local Z out from the planet at the chunk's centre
    // (local X along the face's x there): over a chunk the sphere's surface is still a height
    // field in that frame, which is what the water system works on (WaterBody::local_frame).
    const double half = 0.5 * params.chunk_size * params.tile_size;
    const glm::dvec3 up = glm::normalize(map(half, half, 0.0));
    const glm::dvec3 along = map(half + eps, half, 0.0) - map(half - eps, half, 0.0);
    const glm::dvec3 ax = glm::normalize(along - up * glm::dot(along, up));
    const glm::dvec3 ay = glm::cross(up, ax);
    const glm::dvec3 origin = up * static_cast<double>(layout.radius());
    glm::mat4 frame(1.0f);
    frame[0] = glm::vec4(glm::vec3(ax), 0.0f);
    frame[1] = glm::vec4(glm::vec3(ay), 0.0f);
    frame[2] = glm::vec4(glm::vec3(up), 0.0f);
    frame[3] = glm::vec4(glm::vec3(origin), 1.0f);
    for (ChunkWaterData* water : {&still, &flowing}) {
        for (glm::vec3& p : water->positions) {
            const glm::dvec3 d = map(p.x, p.y, p.z) - origin;
            p = glm::vec3(glm::dot(d, ax), glm::dot(d, ay), glm::dot(d, up));
        }
        water->frame = frame;
        water->local_frame = true;
    }
}

std::vector<std::uint8_t> PlanetChunkShaper::seam_locks(const TerrainParams& params,
                                                        const ChunkCoord& coord,
                                                        const ChunkMeshData& mesh) const {
    const float span = params.chunk_world_size();
    const float eps  = 1e-3f * params.tile_size;
    std::vector<std::uint8_t> locks(mesh.vertices.size(), 0);

    const PlanetLayout layout = params.layout();
    const std::int32_t face = std::clamp(coord.x * params.chunk_size / std::max(1, params.face_tiles), 0, 5);
    const double face_span = static_cast<double>(params.face_tiles) * params.tile_size;
    const double x0 = static_cast<double>(coord.x * params.chunk_size - face * params.face_tiles) * params.tile_size;
    const double y0 = static_cast<double>(coord.y * params.chunk_size) * params.tile_size;
    glm::dvec3 out, ax, ay;
    PlanetLayout::axes(face, out, ax, ay);
    const auto edge_at = [&](double along, bool is_x) {
        const double t = std::tan((2.0 * along / face_span - 1.0) * PlanetLayout::k_pi * 0.25);
        return is_x ? glm::normalize(glm::cross(out + ax * t, ay))
                    : glm::normalize(glm::cross(ax, out + ay * t));
    };
    const glm::dvec3 planes[4] = {edge_at(x0, true), edge_at(x0 + span, true),
                                  edge_at(y0, false), edge_at(y0 + span, false)};
    // The dot product of a unit direction with a unit plane normal is the sine of its angle off
    // the plane, so the tolerance is an ANGLE: a thousandth of a tile, over the planet's radius.
    const double tolerance = static_cast<double>(eps) / std::max(1.0f, layout.radius());
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const glm::dvec3 dir = glm::normalize(glm::dvec3(mesh.vertices[i].position));
        for (const glm::dvec3& n : planes) {
            if (std::abs(glm::dot(dir, n)) < tolerance) { locks[i] = 1u; break; }
        }
    }
    return locks;
}

} // namespace world
} // namespace toy
