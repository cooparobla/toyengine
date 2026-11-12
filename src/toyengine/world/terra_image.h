/**
 * @file terra_image.h
 * @brief Import/export a terrain's height and splat planes as ordinary PNGs.
 *
 * The `.terra` format (terra_codec.h) has no PNG concept at all -- these are a pixel-level
 * bridge for two workflows: bringing in a real heightmap (a DEM, a World Machine export, a
 * hand-painted splat set) and pulling a sculpted/painted terrain back out for external editing.
 * Neither function is wired into `.terra` loading or saving; a caller decides when to use them
 * (a one-shot `import:` scene key -- see TerrainComponent::import_height_path -- an editor
 * button, a script). An import call marks the ONE plane it touched dirty, which is enough for
 * any already-running system to pick the change up; it does not call mark_all_dirty() or touch
 * needs_bake()/dirty_on_disk() -- a caller treating an import as a terrain's first real content
 * (as the scene `import:` key does) does that itself, same as a mapcoopa bake landing.
 *
 * A source PNG of a different size than the destination plane is resampled (bilinear) rather
 * than rejected -- the point of this header is "any heightmap works with any terrain," not an
 * exact pixel match.
 */

#ifndef TOYENGINE_WORLD_TERRA_IMAGE_H
#define TOYENGINE_WORLD_TERRA_IMAGE_H

#include <string>

namespace toy {
namespace world {

class TerraData;

/**
 * @brief Imports a greyscale heightmap PNG into `data`'s height plane.
 *
 * The PNG's full 16-bit range `[0, 65535]` maps onto the plane's `[0, data.geometry().height]`
 * metres. Resampled (bilinear) to the plane's `height_nx() x height_ny()` if the PNG is a
 * different size. Marks the whole height plane dirty on success.
 *
 * @return False, `data` left untouched, if the PNG can't be read or `data` has no height plane
 *         (`geometry().height_sub == 1` -- there is nothing to import into).
 */
bool import_height_png(const std::string& path, TerraData& data);

/** @brief The inverse of import_height_png(): writes the height plane at its own resolution. */
bool export_height_png(const std::string& path, const TerraData& data);

/**
 * @brief Imports an RGBA8 splat PNG into layer plane `which` (0: layers 0-3, 1: layers 4-7).
 *
 * Resampled (bilinear) to the plane's `layer_nx() x layer_ny()` if the PNG is a different size.
 * Marks that plane dirty on success.
 *
 * @return False, `data` left untouched, if the PNG can't be read or `which` is not 0 or 1.
 */
bool import_layer_png(const std::string& path, TerraData& data, int which);

/** @brief The inverse of import_layer_png(): writes layer plane `which` at its own resolution. */
bool export_layer_png(const std::string& path, const TerraData& data, int which);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRA_IMAGE_H
