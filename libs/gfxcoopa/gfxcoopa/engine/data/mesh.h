/**
 * @file mesh.h
 * @brief GPU-resident mesh data loaded from the Blender-exported YAML format.
 *
 * Reads the mesh YAML structure:
 *   vertices: [[x,y,z], ...]
 *   normals:  [[x,y,z], ...]
 *   uvs:      [[u,v], ...]
 *   faces:    [[i0,i1,i2,i3], ...] (quads, triangulated on load)
 *
 * Data is uploaded into host-visible vertex + index buffers via the
 * Buffer::vertex() and Buffer::index() factory methods. Since these are
 * already host-accessible (mapped), no staging copy is needed.
 *
 * A Mesh can also be built from in-memory arrays (from_arrays()) and, when
 * created with more than one vertex buffer, rewritten every frame
 * (update_vertices()) -- the path a CPU-simulated surface such as cloth needs.
 * See from_arrays()'s doc for why that takes a buffer COUNT rather than
 * flipping a "dynamic" bool.
 */

#ifndef GFXCOOPA_ENGINE_DATA_MESH_H
#define GFXCOOPA_ENGINE_DATA_MESH_H

#include <volk/volk.h>
#include <glm/glm.hpp>
#include <fkYAML/node.hpp>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/types/vertex_layout.h>
#include <gfxcoopa/types/format.h>

#include <meshoptimizer.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <array>
#include <stdexcept>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace coopa {
namespace gfx {
namespace engine {
namespace data {

/**
 * @struct Vertex
 * @brief Interleaved per-vertex data (vertex binding 0) for every mesh pipeline.
 */
struct Vertex {
    glm::vec3 position; /**< Object-space vertex position. */
    glm::vec3 normal;   /**< Object-space vertex normal (normalized). */
    glm::vec2 uv;       /**< Texture UV coordinate. */
    glm::vec4 tangent;  /**< Object-space vertex tangent (xyz) and handedness sign (w). */

    /**
     * @brief Returns the VkVertexInputBindingDescription for a Vertex stream.
     */
    static VkVertexInputBindingDescription binding_description() {
        VkVertexInputBindingDescription desc{};
        desc.binding   = 0;
        desc.stride    = sizeof(Vertex);
        desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return desc;
    }

    /**
     * @brief Returns the VkVertexInputAttributeDescriptions for position, normal, uv, tangent.
     */
    static std::array<VkVertexInputAttributeDescription, 4> attribute_descriptions() {
        std::array<VkVertexInputAttributeDescription, 4> attrs{};

        // location = 0: position (vec3)
        attrs[0].binding  = 0;
        attrs[0].location = 0;
        attrs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[0].offset   = offsetof(Vertex, position);

        // location = 1: normal (vec3)
        attrs[1].binding  = 0;
        attrs[1].location = 1;
        attrs[1].format   = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[1].offset   = offsetof(Vertex, normal);

        // location = 2: uv (vec2)
        attrs[2].binding  = 0;
        attrs[2].location = 2;
        attrs[2].format   = VK_FORMAT_R32G32_SFLOAT;
        attrs[2].offset   = offsetof(Vertex, uv);

        // location = 3: tangent (vec4)
        attrs[3].binding  = 0;
        attrs[3].location = 3;
        attrs[3].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[3].offset   = offsetof(Vertex, tangent);

        return attrs;
    }

    /**
     * @brief Sealed vertex input layout for binding 0 (position/normal/uv/tangent),
     * replacing binding_description()/attribute_descriptions() above.
     */
    static VertexLayout layout() {
        return VertexLayout{}
            .binding(0, sizeof(Vertex))
            .attribute(0, Format::RGB32_Sfloat,  static_cast<uint32_t>(offsetof(Vertex, position)))
            .attribute(1, Format::RGB32_Sfloat,  static_cast<uint32_t>(offsetof(Vertex, normal)))
            .attribute(2, Format::RG32_Sfloat,   static_cast<uint32_t>(offsetof(Vertex, uv)))
            .attribute(3, Format::RGBA32_Sfloat, static_cast<uint32_t>(offsetof(Vertex, tangent)));
    }
};

/**
 * @struct InstanceData
 * @brief Per-instance vertex stream at binding 1: this frame's and last frame's world
 *        matrix per instance, its snow anchor, and its foliage sway data (224 bytes).
 *
 * normal_matrix is deliberately NOT streamed — every consuming shader derives
 * it as transpose(inverse(mat3(in_model))) in-shader instead, keeping a
 * glm::inverse() call per object per frame off the CPU side of every geometry
 * pass. Scenes here use non-uniform scale (e.g. Cornell box walls), so the
 * shader must do the full 3x3 inverse-transpose, not just mat3(in_model) directly.
 *
 * prev_model is what the G-buffer backbone (gfx/surface/gbuffer_vs.glsl) projects
 * through the previous frame's camera to write per-object motion vectors; passes
 * that only read locations 4-7 (shadows, transparent, probes) simply leave 8-11
 * unconsumed, which Vulkan permits. A static or newly-seen object streams
 * prev_model == model.
 *
 * foliage (location 16) is per-instance wind data for the `foliage` surface shader, and is
 * zero for everything else. An instanced leaf card is pinned to a twig of a tree whose trunk
 * mesh BENDS in the wind; the card's own world matrix was baked once and cannot follow it, so
 * the card has to reproduce its parent's displacement itself. That displacement depends on the
 * card's height up its own tree, which is per-instance -- one batch holds cards from all over a
 * canopy -- so it cannot ride in the push constants, which are per draw (and, at 128 bytes on
 * the cube-shadow path, full). See assets/shaders/foliage_surface.glsl for the channel meanings.
 *
 * snow_anchor (locations 12-15) is the world matrix the object's snow pattern is laid out in:
 * equal to model for anything that has not moved (so the world's lying snow is one continuous
 * pattern across tiles and chunks), frozen at the pose it had when it first moved for anything
 * that has -- so a moving object carries its snow with it instead of sliding under a pattern
 * fixed in the world (which TAA would smear). Read by the G-buffer backbone's snow cover only.
 */
struct InstanceData {
    glm::mat4 model      = glm::mat4(1.0f); /**< Object-to-world; consumed as locations 4..7, one vec4 per column. */
    glm::mat4 prev_model = glm::mat4(1.0f); /**< Last frame's object-to-world; locations 8..11. */
    glm::mat4 snow_anchor = glm::mat4(1.0f); /**< Where the snow pattern is laid out; locations 12..15. */
    /** @brief Foliage parent sway; location 16. Zero (the default) means "no parent", which is
     *         every instance in the engine except a leaf card published by a procedural graph. */
    glm::vec4 foliage = glm::vec4(0.0f);
    /** @brief Foliage parent sway, second half; location 17. The parent's reach (its height
     *         times its bend at this instance) and its height, which `foliage` cannot yield
     *         because it folds flexibility, height and bend into one number. Needed for the
     *         parts of the parent's motion that are not proportional to flexibility -- being
     *         pressed DOWNWARD by a descending wind, most of all. */
    glm::vec4 foliage1 = glm::vec4(0.0f);

    /** @brief Binding 1, per-instance rate. Binding 0 stays Vertex's per-vertex stream. */
    static VkVertexInputBindingDescription binding_description() {
        VkVertexInputBindingDescription desc{};
        desc.binding   = 1;
        desc.stride    = sizeof(InstanceData);
        desc.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        return desc;
    }

    /**
     * @brief Returns the VkVertexInputAttributeDescriptions for locations 4-7 (model),
     *        8-11 (prev_model), 12-15 (snow_anchor) -- one mat4 each -- and 16/17 (foliage).
     *
     * A mat4 attribute occupies four consecutive vec4 locations. These
     * locations are used uniformly by every pipeline that consumes instance
     * data (including the shadow pipelines, whose binding-0 attributes only
     * use location 0), so this one array serves all of them.
     */
    static std::array<VkVertexInputAttributeDescription, 14> attribute_descriptions() {
        std::array<VkVertexInputAttributeDescription, 14> attrs{};
        for (uint32_t i = 0; i < 12; ++i) {
            const size_t base = (i < 4) ? offsetof(InstanceData, model)
                              : (i < 8) ? offsetof(InstanceData, prev_model) : offsetof(InstanceData, snow_anchor);
            attrs[i].binding  = 1;
            attrs[i].location = 4 + i;
            attrs[i].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
            attrs[i].offset   = static_cast<uint32_t>(base + (i % 4) * sizeof(glm::vec4));
        }
        attrs[12].binding  = 1;
        attrs[12].location = 16;
        attrs[12].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[12].offset   = static_cast<uint32_t>(offsetof(InstanceData, foliage));
        attrs[13].binding  = 1;
        attrs[13].location = 17;
        attrs[13].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[13].offset   = static_cast<uint32_t>(offsetof(InstanceData, foliage1));
        return attrs;
    }

    /**
     * @brief Sealed vertex input layout for binding 1 (per-instance model matrix at
     * locations 4-7, previous-frame model matrix at 8-11), replacing
     * binding_description()/attribute_descriptions() above.
     * Combine with Vertex::layout() via `Vertex::layout().append(InstanceData::layout())`
     * for a pipeline that reads both streams.
     */
    static VertexLayout layout() {
        VertexLayout vl;
        vl.binding(1, sizeof(InstanceData), VertexRate::Instance);
        for (uint32_t i = 0; i < 4; ++i) {
            vl.attribute(4 + i, Format::RGBA32_Sfloat,
                        static_cast<uint32_t>(offsetof(InstanceData, model) + i * sizeof(glm::vec4)));
        }
        for (uint32_t i = 0; i < 4; ++i) {
            vl.attribute(8 + i, Format::RGBA32_Sfloat,
                        static_cast<uint32_t>(offsetof(InstanceData, prev_model) + i * sizeof(glm::vec4)));
        }
        for (uint32_t i = 0; i < 4; ++i) {
            vl.attribute(12 + i, Format::RGBA32_Sfloat,
                        static_cast<uint32_t>(offsetof(InstanceData, snow_anchor) + i * sizeof(glm::vec4)));
        }
        vl.attribute(16, Format::RGBA32_Sfloat, static_cast<uint32_t>(offsetof(InstanceData, foliage)));
        vl.attribute(17, Format::RGBA32_Sfloat, static_cast<uint32_t>(offsetof(InstanceData, foliage1)));
        return vl;
    }
};

/**
 * @struct MeshLod
 * @brief One level of detail: a range of the mesh's shared index buffer.
 *
 * Every level lives in the same vertex/index buffers, so switching levels changes only the
 * draw's index range (and vertex offset, for a hand-authored level whose vertices are
 * appended after LOD 0's) -- never a buffer bind.
 */
/**
 * @struct MeshPart
 * @brief One material slot's range of a LOD's indices (a "submesh").
 *
 * A mesh file may assign each face a material slot (`material_slots` + `face_materials`);
 * triangles are grouped by slot, so slot i's triangles are one contiguous index range in
 * every LOD. A renderer draws each part with that slot's material. Ranges are absolute into
 * the shared index buffer and use the LOD's vertex_offset; a part may be empty in a LOD.
 */
struct MeshPart {
    uint32_t first_index = 0;
    uint32_t index_count = 0;
};

struct MeshLod {
    uint32_t first_index   = 0;
    uint32_t index_count   = 0;
    int32_t  vertex_offset = 0;
    /// Draw this level while the object's projected height (fraction of the screen, see
    /// toyengine/render/visibility.h's screen_height_fraction) is below this. Unused for LOD 0.
    float    screen_size   = 0.0f;
    /// Per material slot, contiguous and covering [first_index, first_index + index_count).
    /// Empty means a single part: the whole level.
    std::vector<MeshPart> parts;
};

/**
 * @struct LodRequest
 * @brief One simplified level to build: how much of the mesh to keep, and when to draw it.
 *
 * The argument form of Mesh::simplify_levels(), for geometry built in code (a terrain chunk)
 * rather than authored in YAML -- where the same three numbers are the `lods:` block's
 * `ratio` / `screen_size` / `error`.
 */
struct LodRequest {
    float ratio       = 0.5f;  ///< Triangles to keep, as a fraction of the full mesh.
    float screen_size = 0.0f;  ///< Drawn while the object covers less of the screen's height than this.
    float error       = 1.0f;  ///< Relative error cap; the simplifier stops early once it is reached.
};

/**
 * @struct MeshCpuData
 * @brief A mesh fully prepared on the CPU (Mesh::build_cpu), awaiting upload (Mesh::from_cpu).
 */
struct MeshCpuData {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    std::vector<MeshLod>  lods;              ///< [0] is the full mesh.
    std::vector<std::string> slot_names;     ///< Material slot names (`material_slots`); empty = one unnamed slot.
    float                 cull_screen_size = 0.0f;
    glm::vec3             bounds_min{0.0f};
    glm::vec3             bounds_max{0.0f};
};

/**
 * @class Mesh
 * @brief GPU-resident interleaved vertex + index buffer, loaded from YAML.
 *
 * Quads in the YAML faces list are triangulated on load (split into 2 triangles).
 *
 * Usage:
 * @code
 * caml::CAMLMap map = caml::CAMLMap::load_yaml("cube.000.yaml");
 * Mesh mesh = Mesh::from_node(device, allocator, cmd_pool, map.get_raw_node());
 * // Per draw call:
 * mesh.bind(cmd);
 * mesh.draw(cmd);
 * @endcode
 */
class Mesh {
public:
    /**
     * @brief Constructs a Mesh by loading from a fkYAML node.
     *
     * The YAML node is expected at the root level with fields:
     *   vertices, normals, uvs, faces
     *
     * @param device    Vulkan logical device.
     * @param allocator VMA allocator.
     * @param cmd_pool  Command pool (used for submit, not staging — data is host-visible).
     * @param node      fkYAML root node of the mesh YAML file.
     * @return A new GPU-resident Mesh.
     * @throws std::runtime_error on parse failures.
     */
    static Mesh from_node(core::Device&             device,
                          memory::Allocator&        allocator,
                          command::CommandPool&     cmd_pool,
                          const fkyaml::node&       node)
    {
        (void)cmd_pool; // Host-visible buffers; staging not required.
        return from_cpu(device, allocator, build_cpu(node));
    }

    /**
     * @brief The CPU half of from_node(): parses a mesh YAML node into welded, cache-ordered
     *        vertex/index arrays plus its LOD table. No GPU calls -- MeshLoader runs this on a
     *        JobEngine worker and only from_cpu()'s upload on the main thread.
     *
     * Blender's export lists one vertex per face CORNER; the triangulated stream is welded
     * back together here (meshopt_generateVertexRemap over the whole Vertex, so only
     * byte-identical corners merge -- hard edges and UV seams, whose normals/UVs differ, stay
     * split), then reordered for the post-transform vertex cache and for fetch locality.
     *
     * @param node         The mesh YAML root (vertices, normals, uvs, tangents, faces, and
     *                     optionally `lods` / `cull_screen_size`).
     * @param lod_config   A node whose `lods` / `cull_screen_size` override the mesh's own --
     *                     the `<mesh>.lod.yaml` sidecar MeshLoader looks for, so a Blender
     *                     re-export cannot wipe the LOD setup. Null: use the mesh's own.
     * @param load_sibling Loads another mesh YAML by logical name, for a LOD level that names
     *                     a hand-authored `mesh:` instead of a simplification `ratio:`. Empty:
     *                     such levels are skipped with a warning.
     */
    static MeshCpuData build_cpu(const fkyaml::node& node,
                                 const fkyaml::node* lod_config = nullptr,
                                 const std::function<std::shared_ptr<fkyaml::node>(const std::string&)>&
                                     load_sibling = {})
    {
        MeshCpuData out;
        out.slot_names = parse_slot_names_(node);
        std::vector<uint32_t> tri_slots;
        const bool needs_tangents = parse_unwelded_(node, out.vertices, out.indices, &tri_slots);
        // Group triangles by material slot (stable), so each slot is one index range.
        const std::vector<MeshPart> parts = sort_by_slot_(out.indices, tri_slots, slot_count_(out.slot_names, tri_slots));
        weld_and_optimize_(out.vertices, out.indices, parts);
        if (needs_tangents) compute_tangents_(out.vertices, out.indices);

        out.bounds_min = glm::vec3(std::numeric_limits<float>::max());
        out.bounds_max = glm::vec3(std::numeric_limits<float>::lowest());
        for (const auto& v : out.vertices) {
            out.bounds_min = glm::min(out.bounds_min, v.position);
            out.bounds_max = glm::max(out.bounds_max, v.position);
        }

        // LOD 0 is always the full mesh; its threshold is never consulted.
        out.lods.push_back({0u, static_cast<uint32_t>(out.indices.size()), 0, 0.0f, parts.size() > 1 ? parts : std::vector<MeshPart>{}});

        const fkyaml::node* cfg = lod_config ? lod_config : &node;
        if (cfg->contains("cull_screen_size")) {
            out.cull_screen_size = cfg->at("cull_screen_size").get_value<float>();
        }
        if (cfg->contains("lods")) {
            build_lods_(cfg->at("lods"), out, load_sibling);
        }
        return out;
    }

    /**
     * @brief The GPU half of from_node(): uploads a build_cpu() result.
     */
    static Mesh from_cpu(core::Device& device, memory::Allocator& allocator, MeshCpuData data) {
        if (data.vertices.empty() || data.indices.empty()) {
            throw std::runtime_error("[Mesh] from_cpu() needs non-empty vertex and index arrays.");
        }
        const VkDeviceSize vb_size = sizeof(Vertex)   * data.vertices.size();
        const VkDeviceSize ib_size = sizeof(uint32_t) * data.indices.size();

        auto vb = memory::Buffer::vertex(device, allocator, vb_size);
        auto ib = memory::Buffer::index (device, allocator, ib_size);
        vb.upload(data.vertices.data(), vb_size);
        ib.upload(data.indices.data(),  ib_size);

        std::vector<memory::Buffer> vbs;
        vbs.push_back(std::move(vb)); // a YAML-loaded mesh is static: exactly one buffer

        Mesh mesh(std::move(vbs), std::move(ib),
                  data.lods.empty() ? static_cast<uint32_t>(data.indices.size()) : data.lods[0].index_count,
                  static_cast<uint32_t>(data.vertices.size()),
                  data.bounds_min, data.bounds_max);
        if (!data.lods.empty()) mesh.lods_ = std::move(data.lods);
        mesh.cull_screen_size_ = data.cull_screen_size;
        mesh.slot_names_ = std::move(data.slot_names);
        return mesh;
    }

    /**
     * @brief Constructs a Mesh from already-interleaved vertex and index arrays.
     *
     * Unlike from_node(), which unwelds every triangle corner into its own Vertex, this keeps the
     * caller's indexed topology exactly as given. That is the whole point for a simulated surface:
     * a cloth with one Vertex per particle rewrites `particle_count` vertices per frame, where an
     * unwelded copy would rewrite six times that and then have to average the duplicates' normals
     * back together to avoid faceting.
     *
     * `buffer_count` is a COUNT, not a `bool dynamic`, because the correct number is a property of
     * the presentation loop (how many frames it keeps in flight), not of the mesh -- and the
     * caller is the only one who knows it. Pass 1 for a mesh that is uploaded once; pass
     * `Context::frames_in_flight()` for one that is rewritten per frame. A single shared buffer
     * would be wrong for the latter: this engine's pipeline never waits per frame, so a rewrite
     * would race a still-in-flight GPU read of the previous frame -- the same reason
     * toyengine's DebugLinePass keeps per-frame-in-flight buffers.
     *
     * @param device       Vulkan logical device.
     * @param allocator    VMA allocator.
     * @param vertices     Interleaved vertex data; must be non-empty.
     * @param indices      32-bit index data; must be non-empty.
     * @param buffer_count Number of vertex buffers to allocate (clamped to at least 1).
     * @param compute_writable Also create the vertex buffers with BufferUsage::Storage, so a
     *        compute pass can write vertices in place (GPU skinning): bind vertex_buffer(slot)
     *        as a storage buffer, dispatch, then mark_gpu_written(slot, ...) and put a
     *        CommandBuffer::compute_to_draw_barrier() before the draws. The buffers stay
     *        host-visible, so update_vertices() (the CPU fallback) keeps working on the same
     *        mesh. Off by default: every existing mesh keeps its vertex-only usage.
     * @param lods         The LOD table `indices` carries (Mesh::simplify_levels()), or null for a
     *                     single-level mesh. Entry 0 must be the full mesh; every level indexes
     *                     into the SAME buffers, so this adds no allocation of its own.
     * @param cull_screen_size Below this projected height (a fraction of the screen's), no view
     *                     draws the mesh at all; 0 never culls.
     * @return A new GPU-resident Mesh.
     * @throws std::runtime_error if either array is empty.
     */
    static Mesh from_arrays(core::Device&               device,
                            memory::Allocator&          allocator,
                            const std::vector<Vertex>&  vertices,
                            const std::vector<uint32_t>& indices,
                            uint32_t                    buffer_count = 1,
                            bool                        compute_writable = false,
                            const std::vector<MeshLod>* lods = nullptr,
                            float                       cull_screen_size = 0.0f)
    {
        if (vertices.empty() || indices.empty()) {
            throw std::runtime_error("[Mesh] from_arrays() needs non-empty vertex and index arrays.");
        }

        const VkDeviceSize vb_size = sizeof(Vertex) * vertices.size();
        const VkDeviceSize ib_size = sizeof(uint32_t) * indices.size();
        const uint32_t count = (buffer_count < 1u) ? 1u : buffer_count;

        std::vector<memory::Buffer> vbs;
        vbs.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            auto vb = compute_writable
                ? memory::Buffer(device, allocator, vb_size, BufferUsage::Vertex | BufferUsage::Storage,
                                 MemoryResidency::CpuToGpu)   // Buffer::vertex()'s memory, plus Storage
                : memory::Buffer::vertex(device, allocator, vb_size);
            vb.upload(vertices.data(), vb_size);   // seed every slot, so frame 0 draws correctly
            vbs.push_back(std::move(vb));           // whichever slot it happens to land on
        }

        auto ib = memory::Buffer::index(device, allocator, ib_size);
        ib.upload(indices.data(), ib_size);

        glm::vec3 bounds_min(std::numeric_limits<float>::max());
        glm::vec3 bounds_max(std::numeric_limits<float>::lowest());
        for (const auto& v : vertices) {
            bounds_min = glm::min(bounds_min, v.position);
            bounds_max = glm::max(bounds_max, v.position);
        }

        // index_count() is LOD 0's, not the buffer's: the coarser levels live in the same index
        // buffer behind it, and a draw that takes no level (Mesh::draw()) means the full mesh.
        // Same split from_cpu() makes for an authored `lods:` block.
        const bool has_lods = lods != nullptr && !lods->empty();
        Mesh mesh(std::move(vbs), std::move(ib),
                  has_lods ? (*lods)[0].index_count : static_cast<uint32_t>(indices.size()),
                  static_cast<uint32_t>(vertices.size()),
                  bounds_min, bounds_max);
        if (has_lods) mesh.lods_ = *lods;
        mesh.cull_screen_size_ = cull_screen_size;
        return mesh;
    }

    /**
     * @brief Rewrites this frame's vertex buffer and makes it the one bind() will use.
     *
     * Call exactly once per frame, before the frame's command buffer is recorded, passing that
     * frame's in-flight slot (Context::current_frame()). Writing into the slot the GPU is not
     * currently reading is what makes this safe without a per-frame fence wait.
     *
     * The object-space bounds are recomputed here rather than left at their creation values: the
     * renderer frustum-culls every view against bounds_min()/bounds_max(), so a cloth that has
     * drooped well outside its rest-pose box would otherwise be culled while still visible.
     *
     * @param data       Vertices to upload; must hold at least `count` entries.
     * @param count      Number of vertices to write. Must not exceed vertex_count().
     * @param frame_slot In-flight frame index; taken modulo the buffer count, so passing a
     *                   monotonically increasing frame counter also works.
     * @throws std::runtime_error if `count` exceeds the allocated vertex count.
     */
    void update_vertices(const Vertex* data, std::size_t count, uint32_t frame_slot) {
        if (!data || count == 0) return;
        if (count > vertex_count_) {
            throw std::runtime_error("[Mesh] update_vertices() exceeds the allocated vertex count.");
        }
        const uint32_t slot = frame_slot % static_cast<uint32_t>(vertex_buffers_.size());
        vertex_buffers_[slot].upload(data, sizeof(Vertex) * count);
        active_slot_ = slot;

        glm::vec3 lo(std::numeric_limits<float>::max());
        glm::vec3 hi(std::numeric_limits<float>::lowest());
        for (std::size_t i = 0; i < count; ++i) {
            lo = glm::min(lo, data[i].position);
            hi = glm::max(hi, data[i].position);
        }
        bounds_min_ = lo;
        bounds_max_ = hi;
    }

    /**
     * @brief The GPU-written counterpart of update_vertices(): a compute pass has written (or
     * is about to write, in this frame's command buffer) `frame_slot`'s vertex buffer, so
     * bind() uses that slot from now on. Nothing is uploaded; `bounds_min`/`bounds_max` are
     * the caller's conservative object-space bounds for culling (e.g. grown from bone
     * positions), since the CPU never sees the vertices.
     */
    void mark_gpu_written(uint32_t frame_slot, const glm::vec3& bounds_min, const glm::vec3& bounds_max) {
        active_slot_ = frame_slot % static_cast<uint32_t>(vertex_buffers_.size());
        bounds_min_  = bounds_min;
        bounds_max_  = bounds_max;
    }

    /** @brief `frame_slot`'s vertex buffer (modulo the buffer count) -- what a compute pass
     *         binds as a storage buffer on a compute_writable mesh. */
    memory::Buffer&       vertex_buffer(uint32_t frame_slot)       { return vertex_buffers_[frame_slot % vertex_buffers_.size()]; }
    const memory::Buffer& vertex_buffer(uint32_t frame_slot) const { return vertex_buffers_[frame_slot % vertex_buffers_.size()]; }
    /** @brief Number of vertex buffers (1 static, frames-in-flight dynamic). */
    uint32_t vertex_buffer_count() const { return static_cast<uint32_t>(vertex_buffers_.size()); }
    /** @brief The slot bind() currently draws from. */
    uint32_t active_slot() const { return active_slot_; }

    /** @brief True if this mesh has more than one vertex buffer, i.e. is safe to rewrite per frame. */
    bool is_dynamic() const { return vertex_buffers_.size() > 1; }

    /** @brief Number of vertices the vertex buffers were allocated for. */
    uint32_t vertex_count() const { return vertex_count_; }

    // --- Draw calls ---

    /**
     * @brief Binds the vertex and index buffers into the command buffer.
     * @param cmd Command buffer to record into.
     */
    void bind(command::CommandBuffer& cmd) const {
        // A static mesh has exactly one slot and active_slot_ never leaves 0, so this is the same
        // single-buffer bind it always was; only a dynamic mesh ever advances the slot.
        cmd.bind_vertex_buffer(vertex_buffers_[active_slot_]);
        cmd.bind_index_buffer(index_buffer_);
    }

    /**
     * @brief Records an indexed draw call for this mesh.
     * @param cmd Command buffer to record into.
     */
    void draw(command::CommandBuffer& cmd) const {
        cmd.draw_indexed(index_count_);
    }

    /**
     * @brief Records an instanced indexed draw call for this mesh.
     *
     * Callers bind the shared per-instance stream (see InstanceData,
     * engine::util::InstanceBatcher::bind()) once per pass before looping
     * batches — bind() above only binds slot 0 (this mesh's own vertex/index
     * buffers), so per-batch mesh rebinding never disturbs the instance
     * stream at slot 1.
     *
     * @param cmd            Command buffer to record into.
     * @param instance_count Number of instances to draw.
     * @param first_instance Offset into the bound instance-rate stream.
     */
    void draw(command::CommandBuffer& cmd, uint32_t instance_count, uint32_t first_instance) const {
        cmd.draw_indexed(index_count_, 0, 0, instance_count, first_instance);
    }

    /** @brief Returns the number of indices in the mesh (LOD 0). */
    uint32_t index_count() const { return index_count_; }

    /** @brief The LOD table; [0] is the full mesh. A mesh without a `lods` block has one entry. */
    const std::vector<MeshLod>& lods() const { return lods_; }

    /** @brief Below this projected screen size the mesh is not drawn at all; 0 = never culled. */
    float cull_screen_size() const { return cull_screen_size_; }

    /**
     * @brief Records an instanced indexed draw of one LOD level (clamped to the table).
     * @param lod            Level index into lods().
     * @param instance_count Number of instances to draw.
     * @param first_instance Offset into the bound instance-rate stream.
     */
    void draw_lod(command::CommandBuffer& cmd, uint32_t lod, uint32_t instance_count,
                  uint32_t first_instance) const {
        const MeshLod& l = lods_[std::min<size_t>(lod, lods_.size() - 1)];
        cmd.draw_indexed(l.index_count, l.first_index, l.vertex_offset, instance_count, first_instance);
    }

    /**
     * @brief Records an instanced draw of one material slot's part of a LOD level. A level
     *        without parts draws whole as part 0 (and nothing for other parts); an empty part
     *        records nothing.
     */
    void draw_lod_part(command::CommandBuffer& cmd, uint32_t lod, uint32_t part, uint32_t instance_count,
                       uint32_t first_instance) const {
        const MeshLod& l = lods_[std::min<size_t>(lod, lods_.size() - 1)];
        if (l.parts.empty()) {
            if (part == 0) cmd.draw_indexed(l.index_count, l.first_index, l.vertex_offset, instance_count, first_instance);
            return;
        }
        if (part >= l.parts.size() || l.parts[part].index_count == 0) return;
        cmd.draw_indexed(l.parts[part].index_count, l.parts[part].first_index, l.vertex_offset, instance_count, first_instance);
    }

    /** @brief Number of material slots (parts); at least 1. */
    uint32_t part_count() const {
        size_t n = std::max<size_t>(1, slot_names_.size());
        for (const auto& l : lods_) n = std::max(n, l.parts.size());
        return static_cast<uint32_t>(n);
    }
    /** @brief Material slot names from the file (`material_slots`); empty when unnamed. */
    const std::vector<std::string>& slot_names() const { return slot_names_; }
    /** @brief Slot index for a name, or -1. */
    int slot_index(const std::string& name) const {
        for (size_t i = 0; i < slot_names_.size(); ++i) if (slot_names_[i] == name) return static_cast<int>(i);
        return -1;
    }
    /** @brief Index count of one part in a LOD (stats). */
    uint32_t part_index_count(uint32_t lod, uint32_t part) const {
        const MeshLod& l = lods_[std::min<size_t>(lod, lods_.size() - 1)];
        if (l.parts.empty()) return part == 0 ? l.index_count : 0;
        return part < l.parts.size() ? l.parts[part].index_count : 0;
    }

    /** @brief Returns the minimum corner of the object-space bounding box. */
    const glm::vec3& bounds_min() const { return bounds_min_; }

    /** @brief Returns the maximum corner of the object-space bounding box. */
    const glm::vec3& bounds_max() const { return bounds_max_; }

    // Move only (buffers are not copyable).
    Mesh(Mesh&&) = default;
    Mesh& operator=(Mesh&&) = default;
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

private:
    /// Parses Blender's per-corner mesh YAML into a triangulated, UNWELDED stream (one Vertex
    /// per triangle corner, indices 0..N-1).
    /// @return True when the file carries no tangents: every corner then holds the same
    ///         placeholder tangent, and the caller must run compute_tangents_() after welding.
    static bool parse_unwelded_(const fkyaml::node& node, std::vector<Vertex>& vertices,
                                std::vector<uint32_t>& indices, std::vector<uint32_t>* tri_slots = nullptr) {
        // Optional per-face material slot (`face_materials`, parallel to `faces`).
        std::vector<uint32_t> face_slots;
        if (tri_slots && node.contains("face_materials")) {
            for (const auto& fm : node.at("face_materials")) face_slots.push_back(fm.get_value<uint32_t>());
        }
        size_t face_index = 0;
        // --- Parse positions ---
        std::vector<glm::vec3> positions;
        if (node.contains("vertices")) {
            for (const auto& v : node.at("vertices")) {
                positions.push_back({
                    v.at(0).get_value<float>(),
                    v.at(1).get_value<float>(),
                    v.at(2).get_value<float>()
                });
            }
        }

        // --- Parse normals ---
        std::vector<glm::vec3> normals;
        if (node.contains("normals")) {
            for (const auto& n : node.at("normals")) {
                normals.push_back({
                    n.at(0).get_value<float>(),
                    n.at(1).get_value<float>(),
                    n.at(2).get_value<float>()
                });
            }
        }

        // --- Parse UVs ---
        std::vector<glm::vec2> uvs;
        if (node.contains("uvs")) {
            for (const auto& uv : node.at("uvs")) {
                uvs.push_back({
                    uv.at(0).get_value<float>(),
                    uv.at(1).get_value<float>()
                });
            }
        }

        // --- Parse tangents ---
        std::vector<glm::vec4> tangents;
        if (node.contains("tangents")) {
            for (const auto& tan : node.at("tangents")) {
                float w = (tan.size() > 3) ? tan.at(3).get_value<float>() : 1.0f;
                tangents.push_back({
                    tan.at(0).get_value<float>(),
                    tan.at(1).get_value<float>(),
                    tan.at(2).get_value<float>(),
                    w
                });
            }
        }

        // --- Parse faces and build interleaved vertices + indices ---
        // Faces are quads [i0,i1,i2,i3]; split into two triangles:
        //   tri1: [i0, i1, i2]   tri2: [i0, i2, i3]

        if (node.contains("faces")) {
            for (const auto& face : node.at("faces")) {
                // Each face element is a list of vertex indices.
                std::vector<uint32_t> face_indices;
                for (const auto& idx_node : face) {
                    face_indices.push_back(idx_node.get_value<uint32_t>());
                }

                const uint32_t slot = face_index < face_slots.size() ? face_slots[face_index] : 0u;
                ++face_index;
                if (face_indices.size() < 3) continue;

                // Fan triangulation from the first vertex.
                for (size_t i = 1; i + 1 < face_indices.size(); ++i) {
                    if (tri_slots) tri_slots->push_back(slot);
                    uint32_t v_indices[3] = {
                        face_indices[0],
                        face_indices[i],
                        face_indices[i + 1]
                    };

                    for (uint32_t vi : v_indices) {
                        Vertex vert{};
                        vert.position = (vi < positions.size()) ? positions[vi] : glm::vec3(0.0f);
                        vert.normal   = (vi < normals.size())   ? normals[vi]   : glm::vec3(0.0f, 1.0f, 0.0f);
                        vert.uv       = (vi < uvs.size())       ? uvs[vi]       : glm::vec2(0.0f);
                        vert.tangent  = (vi < tangents.size())  ? tangents[vi]  : glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
                        // Canonicalize signed zeros (-0.0 + 0.0 == +0.0): the exporter writes
                        // both, and welding compares bytes, so otherwise identical corners on
                        // an axis would never merge.
                        vert.position += glm::vec3(0.0f);
                        vert.normal   += glm::vec3(0.0f);
                        vert.uv       += glm::vec2(0.0f);

                        indices.push_back(static_cast<uint32_t>(vertices.size()));
                        vertices.push_back(vert);
                    }
                }
            }
        }

        if (vertices.empty()) {
            throw std::runtime_error("[Mesh] No vertices parsed — empty or invalid mesh YAML.");
        }

        // Tangents absent from the YAML are computed AFTER welding (compute_tangents_(), from
        // build_cpu()), on the indexed mesh -- computing them here, per unwelded corner, would
        // give every triangle's corners a different tangent and stop identical corners welding.
        return tangents.empty();
    }

    /// Per-vertex tangents for an INDEXED mesh: each triangle's UV-derived tangent summed onto
    /// its three vertices, then Gram-Schmidt'd against the vertex normal. Shared vertices
    /// therefore get the average of their triangles' tangents -- smooth across a welded
    /// surface, and still split wherever welding kept a hard edge or UV seam.
    static void compute_tangents_(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
        std::vector<glm::vec3> tan_sum(vertices.size(), glm::vec3(0.0f));
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            const Vertex& v0 = vertices[indices[i]];
            const Vertex& v1 = vertices[indices[i + 1]];
            const Vertex& v2 = vertices[indices[i + 2]];
            const glm::vec3 edge1 = v1.position - v0.position;
            const glm::vec3 edge2 = v2.position - v0.position;
            const glm::vec2 duv1  = v1.uv - v0.uv;
            const glm::vec2 duv2  = v2.uv - v0.uv;
            const float f = duv1.x * duv2.y - duv2.x * duv1.y;
            const glm::vec3 t = (std::abs(f) > 1e-6f) ? (edge1 * duv2.y - edge2 * duv1.y) / f
                                                     : glm::vec3(1.0f, 0.0f, 0.0f);
            for (int c = 0; c < 3; ++c) tan_sum[indices[i + c]] += t;
        }
        for (size_t i = 0; i < vertices.size(); ++i) {
            glm::vec3 t = glm::length(tan_sum[i]) > 1e-5f ? glm::normalize(tan_sum[i]) : glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 n = vertices[i].normal;
            t = t - n * glm::dot(n, t);
            if (glm::length(t) < 1e-5f) {   // tangent parallel to the normal: any perpendicular
                t = glm::cross(n, std::abs(n.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0));
            }
            vertices[i].tangent = glm::vec4(glm::normalize(t), 1.0f);
        }
    }

    /** @brief `material_slots` names, if the file has them. */
    static std::vector<std::string> parse_slot_names_(const fkyaml::node& node) {
        std::vector<std::string> names;
        if (node.contains("material_slots")) {
            for (const auto& n : node.at("material_slots")) names.push_back(n.get_value<std::string>());
        }
        return names;
    }
    static uint32_t slot_count_(const std::vector<std::string>& names, const std::vector<uint32_t>& tri_slots) {
        uint32_t n = static_cast<uint32_t>(std::max<size_t>(1, names.size()));
        for (uint32_t s : tri_slots) n = std::max(n, s + 1);
        return n;
    }

    /**
     * @brief Stable-reorders an unwelded triangle list so each slot's triangles are
     *        contiguous; returns the per-slot ranges (one entry per slot, possibly empty).
     */
    static std::vector<MeshPart> sort_by_slot_(std::vector<uint32_t>& indices, const std::vector<uint32_t>& tri_slots,
                                               uint32_t slot_count) {
        const size_t tris = indices.size() / 3;
        std::vector<MeshPart> parts(slot_count);
        if (slot_count <= 1 || tri_slots.size() != tris) {
            parts.assign(1, MeshPart{0u, static_cast<uint32_t>(indices.size())});
            return parts;
        }
        std::vector<uint32_t> sorted;
        sorted.reserve(indices.size());
        for (uint32_t s = 0; s < slot_count; ++s) {
            parts[s].first_index = static_cast<uint32_t>(sorted.size());
            for (size_t t = 0; t < tris; ++t) {
                if (tri_slots[t] != s) continue;
                sorted.insert(sorted.end(), indices.begin() + static_cast<std::ptrdiff_t>(t * 3),
                              indices.begin() + static_cast<std::ptrdiff_t>(t * 3 + 3));
            }
            parts[s].index_count = static_cast<uint32_t>(sorted.size()) - parts[s].first_index;
        }
        indices = std::move(sorted);
        return parts;
    }

public:
    /// Welds byte-identical vertices and reorders for the vertex cache and fetch locality.
    /// Public for CPU-built meshes (e.g. terrain chunks) that go through from_arrays().
    static void weld_and_optimize(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices) {
        weld_and_optimize_(vertices, indices);
    }

    /**
     * @brief Appends simplified levels of `indices` to it, and returns the LOD table describing
     *        them -- entry 0 being the full mesh the levels were built from.
     *
     * The code-built counterpart to a mesh file's `lods:` block, for geometry that never passes
     * through build_cpu(): a terrain chunk meshed on a worker, a tool's generated mesh. Every
     * level shares LOD 0's VERTICES and adds only its own indices, which is what makes switching
     * level a change of index range rather than a buffer bind (see MeshLod) -- and what makes
     * the levels cheap: a table of 0.4 / 0.15 / 0.05 costs 60% of LOD 0's indices, and no
     * vertices at all.
     *
     * `vertex_lock` pins named vertices into every level. That is how a grid of independently
     * simplified patches -- terrain chunks -- can sit at DIFFERENT levels side by side without
     * cracking: lock the vertices on each patch's seam and every level still meets its
     * neighbours on exactly those vertices. Note that this is NOT what `lock_border` does:
     * meshopt's own border flag holds the *topological* boundary of the welded surface, which
     * on a terrain chunk is not the seam (a chunk's edge wall carries the surface over the
     * seam and on down, so the simplifier sees no boundary there and collapses it).
     *
     * A level that simplified to nothing, to no fewer triangles than the level before it, or to
     * one that LOST a locked vertex is dropped rather than stored -- the first two would cost
     * memory and draw the same geometry, the third would open the seam the lock was there to
     * protect (a lock stops a vertex moving, not its last triangle collapsing out from under
     * it). So every level this returns carries all of `vertex_lock`'s vertices.
     *
     * @param vertices    LOD 0's vertices; read for positions and normals, never modified.
     * @param indices     In: LOD 0's indices. Out: those, followed by each level's.
     * @param levels      What to build, coarsening in order (`screen_size` decreasing).
     * @param lock_border Also hold the surface's topological boundary (meshopt's own flag).
     * @param vertex_lock One flag per entry of `vertices`, 1 pinning that vertex; null locks none.
     * @return The LOD table, [0] being the full mesh. Pass it to from_arrays().
     */
    static std::vector<MeshLod> simplify_levels(const std::vector<Vertex>& vertices,
                                                std::vector<uint32_t>& indices,
                                                const std::vector<LodRequest>& levels,
                                                bool lock_border = false,
                                                const std::vector<uint8_t>* vertex_lock = nullptr) {
        const uint32_t base_count = static_cast<uint32_t>(indices.size());
        std::vector<MeshLod> table{MeshLod{0u, base_count, 0, 0.0f}};
        if (vertices.empty() || base_count < 3 || levels.empty()) return table;

        const std::vector<uint32_t> base(indices.begin(), indices.begin() + base_count);
        const SimplifyTopology topo(vertices, base);
        const std::vector<unsigned char> locks = topo.compact_locks(vertex_lock);
        for (const LodRequest& level : levels) {
            if (level.ratio <= 0.0f || level.ratio >= 1.0f) continue;
            const std::vector<uint32_t> simplified =
                topo.simplify(0u, base_count, level.ratio, level.error, lock_border,
                              locks.empty() ? nullptr : locks.data());
            if (simplified.size() < 3 || simplified.size() >= table.back().index_count) continue;
            // A level that lost a locked vertex is no level: it is a hole where something else
            // was going to meet this mesh. Dropping it leaves the next-finer level to cover
            // that range of sizes, which is a few more triangles and no seam.
            if (!topo.locks_survived(simplified, locks)) continue;
            table.push_back(MeshLod{static_cast<uint32_t>(indices.size()),
                                    static_cast<uint32_t>(simplified.size()), 0, level.screen_size});
            indices.insert(indices.end(), simplified.begin(), simplified.end());
        }
        return table;
    }

private:
    /**
     * @struct SimplifyTopology
     * @brief The position-only view of a mesh that edge-collapse simplification needs, built
     *        once and reused by every level.
     *
     * A hard-edged mesh (a flat-shaded sphere, a cube, a terrace of tiles) welds into islands --
     * its corners differ in normal, so no two triangles share a vertex -- and a simplifier
     * cannot collapse an island's border, which is every edge there is. So the mesh is
     * presented to it by POSITION: a shadow index buffer points every corner at one canonical
     * vertex per position, joining the islands into one surface, and the simplifier is handed
     * ONLY the distinct positions, compacted. Compaction matters as much as the shadow buffer:
     * an array that still holds the unreferenced split copies of each position reads to
     * meshopt_simplify as a pile of complex seams and locks nearly every vertex.
     *
     * Coming back, each simplified triangle picks, for every corner, the real vertex at that
     * position whose normal best matches the triangle's own -- so a corner shared by a top and
     * a wall keeps the right normal, UV chart and hard-edge side for the face it ended up in.
     */
    struct SimplifyTopology {
        const std::vector<Vertex>&         vertices;
        std::size_t                        vertex_count = 0;
        std::vector<uint32_t>              compact_indices;    ///< One per base index.
        std::vector<uint32_t>              canonical_of;       ///< Compact id -> a vertex at that position.
        std::vector<glm::vec3>             compact_positions;  ///< The distinct positions.
        std::vector<std::vector<uint32_t>> at_position;        ///< Canonical vertex -> every vertex there.
        std::vector<uint32_t>              canonical_at;       ///< Vertex -> the canonical vertex at its position.

        SimplifyTopology(const std::vector<Vertex>& verts, const std::vector<uint32_t>& base)
            : vertices(verts), vertex_count(verts.size()) {
            const uint32_t base_count = static_cast<uint32_t>(base.size());
            if (vertex_count == 0 || base_count == 0) return;
            std::vector<uint32_t> shadow(base_count);
            meshopt_generateShadowIndexBuffer(shadow.data(), base.data(), base_count,
                                              &verts[0].position.x, vertex_count,
                                              sizeof(glm::vec3), sizeof(Vertex));
            at_position.assign(vertex_count, {});
            canonical_at.assign(vertex_count, UINT32_MAX);
            std::vector<uint8_t> seen(vertex_count, 0);
            for (uint32_t k = 0; k < base_count; ++k) {
                const uint32_t v = base[k];
                canonical_at[v] = shadow[k];
                if (seen[v]) continue;
                seen[v] = 1;
                at_position[shadow[k]].push_back(v);
            }
            std::vector<uint32_t> compact_of(vertex_count, UINT32_MAX);
            compact_indices.resize(base_count);
            for (uint32_t k = 0; k < base_count; ++k) {
                const uint32_t v = shadow[k];
                if (compact_of[v] == UINT32_MAX) {
                    compact_of[v] = static_cast<uint32_t>(compact_positions.size());
                    compact_positions.push_back(verts[v].position);
                    canonical_of.push_back(v);
                }
                compact_indices[k] = compact_of[v];
            }
        }

        /** @brief Every corner of `tri_indices` moved to the vertex whose normal suits its triangle. */
        void resolve_corners(std::vector<uint32_t>& tri_indices) const {
            for (std::size_t t = 0; t + 2 < tri_indices.size(); t += 3) {
                const glm::vec3& p0 = vertices[tri_indices[t]].position;
                const glm::vec3& p1 = vertices[tri_indices[t + 1]].position;
                const glm::vec3& p2 = vertices[tri_indices[t + 2]].position;
                glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
                const float len = glm::length(n);
                if (len > 0.0f) n /= len;
                for (int c = 0; c < 3; ++c) {
                    const auto& candidates = at_position[tri_indices[t + c]];
                    uint32_t best = tri_indices[t + c];
                    float best_dot = -2.0f;
                    for (uint32_t v : candidates) {
                        const float d = glm::dot(vertices[v].normal, n);
                        if (d > best_dot) { best_dot = d; best = v; }
                    }
                    tri_indices[t + c] = best;
                }
            }
        }

        /**
         * @brief A per-POSITION lock array from a per-vertex one: a position is pinned when any
         *        of the vertices sharing it is. Empty when nothing is locked.
         *
         * The simplifier sees the compacted positions, not the real vertices, and its contract
         * asks for one consistent flag per position -- which is exactly what collapsing the
         * split copies of a position into one entry gives.
         */
        std::vector<unsigned char> compact_locks(const std::vector<uint8_t>* per_vertex) const {
            if (per_vertex == nullptr || per_vertex->empty() || canonical_of.empty()) return {};
            std::vector<unsigned char> locks(canonical_of.size(), 0);
            for (std::size_t c = 0; c < canonical_of.size(); ++c) {
                for (uint32_t v : at_position[canonical_of[c]]) {
                    if (v < per_vertex->size() && (*per_vertex)[v]) { locks[c] = 1; break; }
                }
            }
            return locks;
        }

        /**
         * @brief Whether every locked position still has a vertex in `level_indices`.
         *
         * A lock stops the simplifier MOVING a vertex; it does not stop the last triangle
         * touching one from collapsing away under it, which at aggressive targets does happen
         * and would leave a hole exactly where the lock was protecting a seam. Cheaper to
         * detect than to repair -- see simplify_levels(), which drops such a level.
         */
        bool locks_survived(const std::vector<uint32_t>& level_indices,
                            const std::vector<unsigned char>& compact_lock) const {
            if (compact_lock.empty()) return true;
            std::vector<uint8_t> present(vertex_count, 0);
            for (uint32_t idx : level_indices) {
                const uint32_t canonical = idx < canonical_at.size() ? canonical_at[idx] : UINT32_MAX;
                if (canonical != UINT32_MAX) present[canonical] = 1;
            }
            for (std::size_t c = 0; c < compact_lock.size(); ++c) {
                if (compact_lock[c] && !present[canonical_of[c]]) return false;
            }
            return true;
        }

        /**
         * @brief One level of `[first_index, first_index + index_count)` of the base indices,
         *        simplified to about `ratio` of its triangles, in real vertex indices.
         */
        std::vector<uint32_t> simplify(uint32_t first_index, uint32_t index_count,
                                       float ratio, float error, bool lock_border,
                                       const unsigned char* compact_lock = nullptr) const {
            if (index_count < 3 || compact_positions.empty()) return {};
            const std::size_t target = std::max<std::size_t>(3, static_cast<std::size_t>(index_count * ratio) / 3 * 3);
            std::vector<uint32_t> simplified(index_count);
            float result_error = 0.0f;
            const unsigned int options = lock_border ? meshopt_SimplifyLockBorder : 0u;
            // The lock array is the only reason for the WithAttributes entry point here: with no
            // attributes and no weights it is meshopt_simplify, plus the per-vertex pins.
            const std::size_t count = compact_lock != nullptr
                ? meshopt_simplifyWithAttributes(
                      simplified.data(), compact_indices.data() + first_index, index_count,
                      &compact_positions[0].x, compact_positions.size(), sizeof(glm::vec3),
                      nullptr, 0, nullptr, 0, compact_lock, target, error, options, &result_error)
                : meshopt_simplify(
                      simplified.data(), compact_indices.data() + first_index, index_count,
                      &compact_positions[0].x, compact_positions.size(), sizeof(glm::vec3),
                      target, error, options, &result_error);
            simplified.resize(count);
            if (count == 0) return simplified;
            for (uint32_t& idx : simplified) idx = canonical_of[idx];   // back to real vertices
            resolve_corners(simplified);
            meshopt_optimizeVertexCache(simplified.data(), simplified.data(), count, vertex_count);
            return simplified;
        }
    };

private:
    static void weld_and_optimize_(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                                   const std::vector<MeshPart>& parts = {}) {
        if (vertices.empty() || indices.empty()) return;
        std::vector<unsigned int> remap(vertices.size());
        const size_t unique = meshopt_generateVertexRemap(remap.data(), indices.data(), indices.size(),
                                                          vertices.data(), vertices.size(), sizeof(Vertex));
        std::vector<Vertex> welded(unique);
        meshopt_remapVertexBuffer(welded.data(), vertices.data(), vertices.size(), sizeof(Vertex), remap.data());
        meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
        // Cache-optimize each material part on its own: a global pass would reorder triangles
        // across parts and break their contiguous ranges. (Fetch optimization below only
        // renumbers vertices, so it can stay global.)
        if (parts.size() > 1) {
            for (const MeshPart& p : parts) {
                if (p.index_count == 0) continue;
                meshopt_optimizeVertexCache(indices.data() + p.first_index, indices.data() + p.first_index, p.index_count, unique);
            }
        } else {
            meshopt_optimizeVertexCache(indices.data(), indices.data(), indices.size(), unique);
        }
        meshopt_optimizeVertexFetch(welded.data(), indices.data(), indices.size(),
                                    welded.data(), unique, sizeof(Vertex));
        vertices = std::move(welded);
    }

    /// Appends the `lods` list's levels to `out` (see build_cpu()). Each entry is either
    /// `{ ratio: r, screen_size: s [, error: e] }` -- LOD 0's indices simplified to about r of
    /// their count (stopping early if the optional relative error cap e is reached), sharing
    /// LOD 0's vertices -- or `{ mesh: name, screen_size: s }`, a
    /// hand-authored mesh whose vertices are appended behind LOD 0's (drawn with a vertex
    /// offset). Levels must get coarser in order: screen_size decreasing.
    static void build_lods_(const fkyaml::node& lods, MeshCpuData& out,
                            const std::function<std::shared_ptr<fkyaml::node>(const std::string&)>& load_sibling) {
        const uint32_t base_count = out.lods[0].index_count;
        const std::vector<MeshPart> base_parts = out.lods[0].parts.empty()
            ? std::vector<MeshPart>{MeshPart{0u, base_count}} : out.lods[0].parts;
        const uint32_t base_slot_count = static_cast<uint32_t>(base_parts.size());
        // Built from LOD 0 once, and reused by every simplified level below.
        const SimplifyTopology topo(out.vertices,
                                    std::vector<uint32_t>(out.indices.begin(), out.indices.begin() + base_count));

        for (const auto& level : lods) {
            const float screen_size = level.contains("screen_size")
                                    ? level.at("screen_size").get_value<float>() : 0.0f;
            MeshLod lod{static_cast<uint32_t>(out.indices.size()), 0u, 0, screen_size};

            if (level.contains("mesh")) {
                const std::string name = level.at("mesh").get_value<std::string>();
                std::shared_ptr<fkyaml::node> sib = load_sibling ? load_sibling(name) : nullptr;
                if (!sib) {
                    std::fprintf(stderr, "[Mesh] LOD mesh '%s' could not be loaded; level skipped\n", name.c_str());
                    continue;
                }
                std::vector<Vertex>   v;
                std::vector<uint32_t> i;
                std::vector<uint32_t> sib_slots;
                const bool sib_needs_tangents = parse_unwelded_(*sib, v, i, &sib_slots);
                // The sibling's slots map onto the base mesh's by NAME (a sibling without slot
                // names is drawn whole as the base's slot 0).
                const std::vector<std::string> sib_names = parse_slot_names_(*sib);
                for (uint32_t& sl : sib_slots) {
                    uint32_t mapped = 0;
                    if (sl < sib_names.size()) {
                        for (size_t k = 0; k < out.slot_names.size(); ++k) if (out.slot_names[k] == sib_names[sl]) mapped = static_cast<uint32_t>(k);
                    }
                    sl = mapped;
                }
                const std::vector<MeshPart> sib_parts = sort_by_slot_(i, sib_slots, base_slot_count);
                weld_and_optimize_(v, i, sib_parts);
                if (sib_needs_tangents) compute_tangents_(v, i);
                lod.vertex_offset = static_cast<int32_t>(out.vertices.size());
                lod.index_count   = static_cast<uint32_t>(i.size());
                if (base_slot_count > 1) {
                    for (const MeshPart& p : sib_parts) lod.parts.push_back({lod.first_index + p.first_index, p.index_count});
                }
                out.vertices.insert(out.vertices.end(), v.begin(), v.end());
                out.indices.insert(out.indices.end(), i.begin(), i.end());
            } else {
                const float ratio = level.contains("ratio") ? level.at("ratio").get_value<float>() : 0.5f;
                const float error = level.contains("error") ? level.at("error").get_value<float>() : 1.0f;
                // Simplify each material part separately (its own range of LOD 0), locking the
                // part's border when there are several so neighbouring parts stay stitched.
                std::vector<uint32_t> level_indices;
                std::vector<MeshPart> level_parts;
                const bool lock_border = base_parts.size() > 1;
                for (const MeshPart& bp : base_parts) {
                    const uint32_t start = static_cast<uint32_t>(level_indices.size());
                    if (bp.index_count < 3) { level_parts.push_back({lod.first_index + start, 0u}); continue; }
                    const std::vector<uint32_t> simplified =
                        topo.simplify(bp.first_index, bp.index_count, ratio, error, lock_border);
                    const size_t count = simplified.size();
                    level_indices.insert(level_indices.end(), simplified.begin(), simplified.end());
                    level_parts.push_back({lod.first_index + start, static_cast<uint32_t>(count)});
                }
                if (level_indices.empty()) {
                    std::fprintf(stderr, "[Mesh] LOD ratio %.3f simplified to nothing; level skipped\n", ratio);
                    continue;
                }
                lod.index_count = static_cast<uint32_t>(level_indices.size());
                if (base_parts.size() > 1) lod.parts = std::move(level_parts);
                out.indices.insert(out.indices.end(), level_indices.begin(), level_indices.end());
            }
            out.lods.push_back(lod);
        }
    }

    Mesh(std::vector<memory::Buffer> vbs, memory::Buffer ib, uint32_t index_count,
         uint32_t vertex_count, const glm::vec3& bounds_min, const glm::vec3& bounds_max)
        : vertex_buffers_(std::move(vbs)),
          index_buffer_(std::move(ib)),
          index_count_(index_count),
          vertex_count_(vertex_count),
          bounds_min_(bounds_min),
          bounds_max_(bounds_max),
          lods_{MeshLod{0u, index_count, 0, 0.0f}}
    {}

    /** @brief One entry for a static mesh; one per frame-in-flight for a dynamic one. A vector
     *         rather than a fixed array so a static mesh pays for exactly one buffer -- most
     *         meshes in a scene are static, and triple-buffering all of them would waste GPU
     *         memory proportional to the whole scene. */
    std::vector<memory::Buffer> vertex_buffers_;
    memory::Buffer index_buffer_;  /**< 32-bit index data; topology is fixed, so never per-frame. */
    uint32_t       index_count_;   /**< Total number of indices to draw. */
    uint32_t       vertex_count_;  /**< Vertices each buffer was allocated for. */
    glm::vec3      bounds_min_;    /**< Object-space AABB minimum corner. */
    glm::vec3      bounds_max_;    /**< Object-space AABB maximum corner. */
    std::vector<MeshLod> lods_;            /**< [0] = full mesh; see MeshLod. */
    float                cull_screen_size_ = 0.0f;
    std::vector<std::string> slot_names_;  /**< Material slot names (`material_slots`). */

    /** @brief Which slot bind() uses. Mutable-free: only update_vertices() advances it, and that
     *         runs before the frame is recorded, never during. */
    uint32_t       active_slot_ = 0;
};

} // namespace data
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_ENGINE_MESH_H
