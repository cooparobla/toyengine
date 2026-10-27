/**
 * @file instance_set.h
 * @brief One InstancePainter's painted instances, stored: a sparse grid of chunks, each holding
 *        the `InstanceRecord`s painted inside it.
 *
 * Pure CPU, like `world::TileMapData` and `world::HeightmapData`: no Vulkan, no `Scene`, so it
 * is unit-tested directly and the editor's brushes are plain functions over it.
 *
 * ### Why chunks, and why sparse
 *
 * Chunks are the unit of three different things at once: the render cache's instanced draw (one
 * batch per chunk, so it frustum-culls on its own), collision streaming, and the file's
 * compression and dirty tracking. They are **sparse** because a painter is expected to cover a
 * world rather than a bounded grid -- a copse here, a town there, nothing in between.
 *
 * ### Editing
 *
 * Mutate through `add()` / `erase_in_radius()` / `set_records()`, which route by world position
 * and bump the chunk's `chunk_revision()` as well as the set's `revision()`. Every consumer
 * keeps its own "seen" revision per chunk and rebuilds only what moved; consumers never drain
 * each other's changes.
 *
 * ### Files
 *
 * `save()` / `load()` write a `.instances` file: a header, a chunk index, then one deflated
 * block of records per chunk. Blocks are kept compressed in memory after a load or save, so a
 * save recompresses only the chunks edited since. Records are packed field-major inside a block
 * (all the x positions, then all the y...), because that is what lets deflate see the
 * coherence between neighbouring instances.
 *
 * ### Threads
 *
 * Owner thread only. Nothing reads this from a job: the render cache copies the matrices it
 * needs out while it builds a chunk.
 */

#ifndef TOYENGINE_INSTANCE_INSTANCE_SET_H
#define TOYENGINE_INSTANCE_INSTANCE_SET_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace toy {
namespace instance {

/** @brief A chunk's integer coordinate: `floor(world.xy / chunk_size)`. */
using ChunkCoord = glm::ivec2;

/** @brief Hash for ChunkCoord, so chunks can live in an unordered_map. */
struct ChunkCoordHash {
    std::size_t operator()(const ChunkCoord& c) const {
        const std::uint64_t x = static_cast<std::uint32_t>(c.x), y = static_cast<std::uint32_t>(c.y);
        return static_cast<std::size_t>((x * 0x9E3779B97F4A7C15ull) ^ (y * 0xC2B2AE3D27D4EB4Full));
    }
};

/**
 * @struct InstanceRecord
 * @brief Everything stored about one painted instance: 48 bytes.
 *
 * Plain fields rather than a packed encoding, because the set is read far more often than it is
 * stored and the file deflates the redundancy away anyway. The world matrix is built from this
 * once per chunk rebuild, never per frame.
 */
struct InstanceRecord {
    glm::vec3     position{0.0f};        ///< World space.
    glm::quat     rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3     scale{1.0f};
    std::uint16_t prototype = 0;         ///< Index into `InstancePainter::prototypes`.
    std::uint16_t flags = 0;             ///< See `Flags`.
    std::uint32_t seed = 0;              ///< Per-instance variation, stable across rebuilds.

    enum Flags : std::uint16_t {
        Hidden = 1u << 0,                ///< Painted but not drawn or collided (a soft erase).
    };

    bool hidden() const { return (flags & Hidden) != 0; }

    /** @brief The instance's world matrix (translate * rotate * scale). */
    glm::mat4 matrix() const;
};

static_assert(sizeof(InstanceRecord) == 48, "InstanceRecord is stored field-major by its size");

/** @brief One instance's address inside a set: which chunk, and which record in it. */
struct InstanceRef {
    ChunkCoord  chunk{0};
    std::uint32_t index = 0;
};

/**
 * @class InstanceSetData
 * @brief See the file comment.
 */
class InstanceSetData {
public:
    /// Metres per chunk edge when nothing says otherwise; matches a terrain chunk.
    static constexpr float k_default_chunk_size = 32.0f;

    InstanceSetData();

    /** @brief Process-unique id, so a cache can tell one set from another. */
    std::uint64_t uid() const { return uid_; }

    float chunk_size() const { return chunk_size_; }

    /**
     * @brief Sets the chunk edge in metres, re-routing every record when it changes.
     *
     * Chunking is a storage and streaming decision, not authored data: changing it must never
     * move an instance, only which chunk holds it.
     */
    void set_chunk_size(float metres);

    /** @brief The chunk a world position belongs to. */
    ChunkCoord chunk_of(const glm::vec3& world) const;

    /** @brief The local origin (lower corner) of a chunk, world space, z unused. */
    glm::vec2 chunk_origin(ChunkCoord c) const {
        return glm::vec2(static_cast<float>(c.x), static_cast<float>(c.y)) * chunk_size_;
    }

    bool empty() const { return count_ == 0; }
    /** @brief Total records, hidden ones included. */
    std::size_t size() const { return count_; }
    std::size_t chunk_count() const { return chunks_.size(); }

    /** @brief The records in `c`, or null when nothing has been painted there. */
    const std::vector<InstanceRecord>* records(ChunkCoord c) const;

    /** @brief Every chunk holding records, in no particular order. */
    std::vector<ChunkCoord> chunks() const;

    /** @brief Bumped by every edit, so "did anything change" is one comparison. */
    std::uint64_t revision() const { return revision_; }
    /** @brief Bumped by every edit to `c`; 0 for a chunk that holds nothing. */
    std::uint64_t chunk_revision(ChunkCoord c) const;

    // --- editing ---

    /** @brief Adds one record, routed to the chunk its position falls in. */
    void add(const InstanceRecord& r);

    /** @brief Replaces a chunk's records wholesale (what an undo step applies). */
    void set_records(ChunkCoord c, std::vector<InstanceRecord> records);

    /**
     * @brief Erases every record within `radius` of `centre` (in xy, ignoring z).
     * @param prototype Only this prototype, or -1 for any.
     * @return How many were erased.
     */
    std::size_t erase_in_radius(const glm::vec3& centre, float radius, int prototype = -1);

    /** @brief Erases the records in `c` that `pred` accepts. @return How many were erased. */
    std::size_t erase_if(ChunkCoord c, const std::function<bool(const InstanceRecord&)>& pred);

    /** @brief Erases one record. Invalidates every `InstanceRef` into its chunk. */
    bool erase(const InstanceRef& ref);

    // --- queries ---

    /** @brief Appends every instance within `radius` of `centre` (xy) to `out`. */
    void query_radius(const glm::vec3& centre, float radius, std::vector<InstanceRef>& out) const;

    /**
     * @brief Is there already an instance within `radius` of `centre` (xy)?
     *
     * What a brush's minimum spacing asks, once per candidate, so it short-circuits.
     */
    bool any_within(const glm::vec3& centre, float radius) const;

    /** @brief The record a ref addresses, or null if the ref is stale. */
    const InstanceRecord* at(const InstanceRef& ref) const;

    /** @brief The chunks a disc of `radius` about `centre` touches (xy). */
    std::vector<ChunkCoord> chunks_in_radius(const glm::vec3& centre, float radius) const;

    void clear();

    // --- files ---

    /** @brief Edited since the last save or load. */
    bool unsaved() const { return unsaved_; }


    /** @brief Writes a `.instances` file; recompresses only the chunks edited since. */
    bool save(const std::string& path);
    /** @brief Reads a `.instances` file, replacing everything held. */
    bool load(const std::string& path);

private:
    struct Chunk {
        std::vector<InstanceRecord>  records;
        std::vector<std::uint8_t>    compressed;   ///< As last saved / loaded; empty when stale.
        std::uint64_t                revision = 1;
        bool                         dirty = true; ///< Edited since `compressed` was built.
    };

    Chunk& chunk_for_(ChunkCoord c);
    void touch_(Chunk& ch, ChunkCoord c);
    std::vector<std::uint8_t> pack_(const std::vector<InstanceRecord>& records) const;
    static bool unpack_(const std::vector<std::uint8_t>& blob, std::uint32_t count,
                        std::vector<InstanceRecord>& out);
    void recount_();

    std::uint64_t uid_;
    float         chunk_size_ = k_default_chunk_size;
    std::unordered_map<ChunkCoord, Chunk, ChunkCoordHash> chunks_;
    std::size_t   count_ = 0;
    std::uint64_t revision_ = 1;
    bool          unsaved_ = false;
};

/**
 * @class InstanceSetStore
 * @brief Process-wide owner of every painter's instances, keyed by its `.instances` file -- so
 *        re-parsing a component (the editor does on every edit) hands back the same live data,
 *        unsaved edits included. Modelled on `world::TileMapStore`.
 */
class InstanceSetStore {
public:
    static InstanceSetStore& instance();

    /**
     * @brief The live set for `path`. First call: loads the file when it exists, otherwise an
     *        empty set. Later calls return the same object -- reloaded from disk only when the
     *        file changed there and nothing here is unsaved.
     */
    std::shared_ptr<InstanceSetData> acquire(const std::string& path);

    /** @brief Writes one set to its file. */
    bool save(InstanceSetData& data);
    /** @brief Writes every set with unsaved edits; returns how many were written. */
    int save_dirty();
    /** @brief Any set with unsaved edits. */
    bool any_dirty() const;
    /** @brief The file `data` was acquired for, or empty. */
    std::string path_of(const InstanceSetData& data) const;
    /** @brief Forgets everything (a scene change). */
    void clear();

private:
    struct Entry {
        std::shared_ptr<InstanceSetData> data;
        std::int64_t mtime = 0;
    };
    std::vector<std::pair<std::string, Entry>> entries_;
};

} // namespace instance
} // namespace toy

#endif // TOYENGINE_INSTANCE_INSTANCE_SET_H
