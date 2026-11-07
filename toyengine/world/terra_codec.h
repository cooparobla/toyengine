/**
 * @file terra_codec.h
 * @brief The `.terra` file: one chunked, per-plane format for a terrain's whole `TerraData`.
 *
 * A field-major (SoA) byte transpose before deflate, so equal neighbours line up and the
 * terrain's coherence compresses; a plane-table header instead of one baked record size, so a
 * new plane is additive, not a version bump; and an atomic load into a scratch object, committed
 * only on full validation.
 *
 * This format has NO concept of a GPU texture, a render pipeline, or a meshing style -- it is
 * purely a chunked data format, read back into exactly the `TerraData` that was saved. Whatever
 * a meshing style needs from the data at runtime (a dense displacement texture, a welded mesh) is
 * built FROM a loaded `TerraData`, downstream of this file entirely.
 *
 * ### Format
 *
 * ```
 * header:  magic "TERA", version u32=1, tiles_x u32, tiles_y u32, storage_chunk u32=32,
 *          plane_count u32, flags u32=0, reserved u32                              [32 B]
 * geometry: tile_size f32, height f32, height_step f32,
 *           height_sub u32, layer_sub u32, chunk_tiles u32, reserved[2] u32        [32 B]
 * plane table (plane_count x 28 B): id u32, elem_bytes u32, sub u32, plus u32,
 *                                   codec u32, index_off u64
 * per plane, in table order: chunk index (chunk_count x 16 B: u64 offset, u32 size,
 *                             u32 raw_size), then its blobs concatenated
 * ```
 *
 * `sub` is samples/texels per tile edge for that plane (1 for tiles and layers unless
 * `layer_sub` differs, `height_sub` for height); `plus` is 1 when the plane shares an extra
 * corner row/column with its neighbour (the height plane only) and 0 otherwise. `codec` is 0
 * (raw) or 1 (zlib over the SoA transpose). Chunking is in TILES, at `storage_chunk` (32) tiles
 * per storage-chunk edge -- independent of `TerraGeometry::chunk_tiles`, the render/streaming
 * unit, exactly as `.tiles`' `k_storage_chunk` was independent of it.
 *
 * An unrecognised `PlaneId` in the table is skipped, not fatal, for forward compatibility.
 */

#ifndef TOYENGINE_WORLD_TERRA_CODEC_H
#define TOYENGINE_WORLD_TERRA_CODEC_H

#include <string>

namespace toy {
namespace world {

class TerraData;

/** @brief Tiles per storage-chunk edge: the file's compression and indexing unit. */
inline constexpr int k_terra_storage_chunk = 32;

/** @brief Writes `data` to `path` as a `.terra` file. */
bool save_terra(const std::string& path, const TerraData& data);

/** @brief Reads `path` into `data`. Atomic: on any failure `data` is left untouched. */
bool load_terra(const std::string& path, TerraData& data);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRA_CODEC_H
