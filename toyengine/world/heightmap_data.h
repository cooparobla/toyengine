/**
 * @file heightmap_data.h
 * @brief The CPU source of truth of a HeightmapTerrain: a height grid in metres, two RGBA8 splat
 *        maps (eight paint layers), the dirty rectangles the GPU upload and the collider rebuild
 *        drain, and per-chunk revisions the foliage scatter watches.
 *
 * Everything that reads terrain shape -- the renderer's height texture, the physics heightfield,
 * GroundProbe, the particle scatter, the editor brushes -- reads it from here, so a stroke is one
 * write that every consumer picks up. Pure CPU: no Vulkan, no scene, so it is unit-testable and the
 * editor can hold one without an Engine.
 *
 * ### Coordinates
 *
 * "Local" positions are metres relative to the terrain's origin corner: x in [0, size.x], y in
 * [0, size.y], z up. Height texel (i, j) sits at local (i * cell.x, j * cell.y); the grid has
 * `resolution` samples per side, so the outermost samples land exactly on the edges. Splat texel
 * (i, j) covers the same area with its own (usually finer) resolution, sampled texel-centred.
 */

#ifndef TOYENGINE_WORLD_HEIGHTMAP_DATA_H
#define TOYENGINE_WORLD_HEIGHTMAP_DATA_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace toy {
namespace world {

/** @brief Paint layers a terrain carries: two RGBA8 splat maps, one channel each. */
inline constexpr int k_heightmap_layer_count = 8;

/**
 * @struct IRect
 * @brief A half-open texel rectangle [x0, x1) x [y0, y1); empty when either extent is <= 0.
 */
struct IRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    bool empty() const { return x1 <= x0 || y1 <= y0; }
    int width() const { return std::max(0, x1 - x0); }
    int height() const { return std::max(0, y1 - y0); }

    /** @brief Grows this rectangle to cover `o` too (an empty side is ignored). */
    void include(const IRect& o) {
        if (o.empty()) return;
        if (empty()) { *this = o; return; }
        x0 = std::min(x0, o.x0); y0 = std::min(y0, o.y0);
        x1 = std::max(x1, o.x1); y1 = std::max(y1, o.y1);
    }

    /** @brief This rectangle clipped to [0, w) x [0, h). */
    IRect clipped(int w, int h) const {
        IRect r{std::max(0, x0), std::max(0, y0), std::min(w, x1), std::min(h, y1)};
        if (r.empty()) return IRect{};
        return r;
    }

    bool operator==(const IRect& o) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
};

/**
 * @struct HeightmapLayer
 * @brief One paint layer's look: its textures, how they tile, and how the surface responds.
 */
struct HeightmapLayer {
    std::string name;
    std::string albedo;            ///< Texture path (sRGB); empty paints the flat `tint`.
    std::string normal;            ///< Tangent-space normal map path; empty = flat.
    glm::vec3   tint{1.0f};        ///< Multiplies the albedo texture (or is the colour without one).
    float       tiling = 8.0f;     ///< Metres one texture repeat covers.
    float       roughness = 0.85f;
    float       metallic = 0.0f;
    float       normal_strength = 1.0f;
    bool        triplanar = false; ///< Project on three axes on steep slopes (cliffs) instead of top-down.
    float       triplanar_sharpness = 4.0f;
};

/**
 * @struct HeightmapGeometry
 * @brief The authored dimensions: what changes the data's shape (and so resamples it).
 */
struct HeightmapGeometry {
    glm::vec2 size{256.0f, 256.0f}; ///< World width (x) and depth (y), metres.
    float     height = 64.0f;       ///< Height at a normalised value of 1, metres.
    float     chunk_size = 32.0f;   ///< Render / collision / scatter chunk edge, metres.
    int       resolution = 257;     ///< Height samples per side (N x N; 2^k + 1 recommended).
    int       splat_resolution = 512; ///< Splat texels per side.

    bool operator==(const HeightmapGeometry& o) const {
        return size == o.size && height == o.height && chunk_size == o.chunk_size &&
               resolution == o.resolution && splat_resolution == o.splat_resolution;
    }
    bool operator!=(const HeightmapGeometry& o) const { return !(*this == o); }

    /** @brief Clamps every field into its supported range. */
    HeightmapGeometry sanitized() const;

    /** @brief Chunks along x and y (ceil, at least one each). */
    glm::ivec2 chunk_count() const;
};

/**
 * @struct HeightmapGenerate
 * @brief Procedural seed used when a terrain names no heightmap file (or the file is missing).
 */
struct HeightmapGenerate {
    std::uint32_t seed = 1;
    int           octaves = 5;
    float         feature_size = 160.0f; ///< Metres per base-octave feature.
    float         ridges = 0.35f;        ///< 0 = rolling fBm, 1 = fully ridged.
    float         base = 0.15f;          ///< Normalised floor added under the noise.
    float         amplitude = 0.7f;      ///< Normalised range of the noise.
    bool          island = false;        ///< Fade to the floor towards the edges.
};

/** @brief Deterministic 2D fBm in roughly [-1, 1]; the generator's and the Noise brush's field. */
float heightmap_fbm(float x, float y, std::uint32_t seed, int octaves, float ridges);

/**
 * @class HeightmapData
 * @brief Owns a terrain's grid and splats; see the file comment for coordinates.
 */
class HeightmapData {
public:
    HeightmapData() : uid_(next_uid_()) { reset(HeightmapGeometry{}); }
    explicit HeightmapData(const HeightmapGeometry& geometry) : uid_(next_uid_()) { reset(geometry); }

    /** @brief Process-unique id (consumers key caches by it, never by address). */
    std::uint64_t uid() const { return uid_; }

    // ---------------------------------------------------------------- shape

    /** @brief Reallocates to `geometry`: flat zero heights, layer 0 everywhere. */
    void reset(const HeightmapGeometry& geometry);

    /**
     * @brief Changes the geometry, keeping the content: resamples the grid and splats to the new
     *        resolutions (bilinear) and rescales heights so the same normalised shape spans the
     *        new height. Size and chunk size change only the metres a texel covers.
     */
    void set_geometry(const HeightmapGeometry& geometry);

    /**
     * @brief Replaces geometry and content with `src`'s, keeping this object's revisions moving
     *        forward (a restored snapshot must never look older to a consumer than what it built).
     */
    void assign_content(const HeightmapData& src);

    const HeightmapGeometry& geometry() const { return geometry_; }
    int nx() const { return geometry_.resolution; }
    int ny() const { return geometry_.resolution; }
    int splat_w() const { return geometry_.splat_resolution; }
    int splat_h() const { return geometry_.splat_resolution; }
    glm::vec2 cell() const {
        return geometry_.size / glm::vec2(static_cast<float>(nx() - 1), static_cast<float>(ny() - 1));
    }

    // ---------------------------------------------------------------- raw access

    std::vector<float>& heights() { return heights_; }
    const std::vector<float>& heights() const { return heights_; }
    float& at(int i, int j) { return heights_[static_cast<std::size_t>(j) * nx() + i]; }
    float at(int i, int j) const { return heights_[static_cast<std::size_t>(j) * nx() + i]; }
    /** @brief Height at a texel, clamped to the grid. */
    float at_clamped(int i, int j) const {
        return at(std::clamp(i, 0, nx() - 1), std::clamp(j, 0, ny() - 1));
    }

    /** @brief Splat map `index` (0 = layers 0-3, 1 = layers 4-7), RGBA8, row-major. */
    std::vector<std::uint8_t>& splat(int index) { return splat_[index]; }
    const std::vector<std::uint8_t>& splat(int index) const { return splat_[index]; }
    /** @brief Weight byte of `layer` at splat texel (i, j). */
    std::uint8_t& splat_at(int layer, int i, int j) {
        return splat_[layer / 4][(static_cast<std::size_t>(j) * splat_w() + i) * 4 + (layer % 4)];
    }
    std::uint8_t splat_at(int layer, int i, int j) const {
        return splat_[layer / 4][(static_cast<std::size_t>(j) * splat_w() + i) * 4 + (layer % 4)];
    }

    // ---------------------------------------------------------------- queries (local metres)

    /** @brief Bilinear height at local xy, clamped to the terrain's edge. */
    float sample_height(glm::vec2 local) const;
    /** @brief Height texel coordinates (fractional) of a local position. */
    glm::vec2 to_texel(glm::vec2 local) const { return local / cell(); }
    /** @brief Unit surface normal at local xy (central differences, bilinear). */
    glm::vec3 normal(glm::vec2 local) const;
    /** @brief Normalised weight (0..1, layers sum to 1) of `layer` at local xy, bilinear. */
    float layer_weight(int layer, glm::vec2 local) const;
    /** @brief All eight normalised weights at local xy. */
    std::array<float, k_heightmap_layer_count> layer_weights(glm::vec2 local) const;
    /** @brief True when local xy lies on the terrain. */
    bool contains(glm::vec2 local) const {
        return local.x >= 0.0f && local.y >= 0.0f && local.x <= geometry_.size.x && local.y <= geometry_.size.y;
    }

    /** @brief Min and max height inside a texel rectangle (inclusive of its far edge). */
    glm::vec2 minmax(const IRect& texels) const;
    /** @brief Min and max height over chunk (cx, cy), including its shared border texels. */
    glm::vec2 chunk_minmax(int cx, int cy) const;
    /** @brief Min and max height over the whole grid (cached per height revision). */
    glm::vec2 height_range() const;
    /** @brief Texel rectangle covering chunk (cx, cy), shared borders included. */
    IRect chunk_texels(int cx, int cy) const;

    /**
     * @brief Ray against the surface, local space. Marches in sub-cell steps then bisects.
     * @return True on a hit; `t` is the ray parameter (direction need not be unit).
     */
    bool raycast(glm::vec3 origin, glm::vec3 dir, float max_t, float& t) const;

    // ---------------------------------------------------------------- editing

    /**
     * @brief Records an edit to a height texel rectangle: grows the pending GPU/collider dirty
     *        rect and bumps the revision of every chunk it touches.
     */
    void mark_height_dirty(const IRect& texels);
    /** @brief Records an edit to a splat texel rectangle (same bookkeeping, splat space). */
    void mark_splat_dirty(const IRect& texels);
    /** @brief Marks the whole grid and both splats dirty (import, resize, generate). */
    void mark_all_dirty();

    /**
     * @brief Height texels edited after revision `since` (the union; the whole grid when the
     *        journal no longer reaches back that far). Every consumer -- a scene's GPU copy, a
     *        collider, chunk bounds -- keeps its own `since`, so any number of them can follow
     *        the same data without draining each other's changes.
     */
    IRect height_dirty_since(std::uint64_t since) const;
    /** @brief Splat texels edited after revision `since` (same contract). */
    IRect splat_dirty_since(std::uint64_t since) const;

    /** @brief Bumped by every edit; consumers compare against the value they last built from. */
    std::uint64_t revision() const { return revision_; }
    /** @brief Bumped by edits to the grid only (not paint); the collider watches this one. */
    std::uint64_t height_revision() const { return height_revision_; }
    /** @brief Per-chunk revision (any edit inside the chunk's area). */
    std::uint64_t chunk_revision(int cx, int cy) const;

    /** @brief True when there are edits not yet written to disk. */
    bool dirty_on_disk() const { return unsaved_; }
    void set_saved() { unsaved_ = false; }

    // ---------------------------------------------------------------- content

    /** @brief Fills the grid from `params` (fBm + ridges + optional island fade). */
    void generate(const HeightmapGenerate& params);
    /** @brief Paints every splat texel 100% `layer`. */
    void fill_layer(int layer);
    /** @brief Renormalises a splat texel so its eight weights sum to 255, favouring `keep`. */
    void normalize_splat_texel(int i, int j, int keep = -1);

private:
    void bump_chunks_(const glm::vec2& lo, const glm::vec2& hi);

    static std::uint64_t next_uid_();

    std::uint64_t uid_ = 0;
    HeightmapGeometry geometry_;
    std::vector<float> heights_;
    std::array<std::vector<std::uint8_t>, 2> splat_;

    struct JournalEntry {
        std::uint64_t revision;
        IRect rect;
    };
    void journal_(std::vector<JournalEntry>& journal, std::uint64_t& floor, const IRect& rect);
    static IRect dirty_since_(const std::vector<JournalEntry>& journal, std::uint64_t floor, std::uint64_t since,
                              const IRect& full);

    std::vector<JournalEntry> height_journal_, splat_journal_;
    std::uint64_t height_floor_ = 0, splat_floor_ = 0;  ///< Edits at or below these are no longer journalled.
    std::uint64_t revision_ = 1;
    std::uint64_t height_revision_ = 1;
    std::vector<std::uint64_t> chunk_revisions_;
    bool unsaved_ = false;
    mutable glm::vec2 range_{0.0f};
    mutable std::uint64_t range_revision_ = 0;
};

/**
 * @struct HeightmapView
 * @brief A terrain's data plus where it sits in the world: the narrow world-space query surface
 *        GroundProbe, particle scatter and gameplay code use without knowing about the component.
 */
struct HeightmapView {
    const HeightmapData* data = nullptr;
    glm::vec3 origin{0.0f}; ///< World position of the local (0, 0, 0) corner.

    explicit operator bool() const { return data != nullptr; }
    glm::vec2 local(glm::vec3 world) const { return glm::vec2(world) - glm::vec2(origin); }
    bool contains(glm::vec3 world) const { return data && data->contains(local(world)); }
    float height(glm::vec3 world) const { return origin.z + data->sample_height(local(world)); }
    glm::vec3 normal(glm::vec3 world) const { return data->normal(local(world)); }
    float layer_weight(int layer, glm::vec3 world) const { return data->layer_weight(layer, local(world)); }
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_HEIGHTMAP_DATA_H
