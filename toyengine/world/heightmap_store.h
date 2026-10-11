/**
 * @file heightmap_store.h
 * @brief Process-wide owner of every HeightmapTerrain's data, keyed by its heightmap file.
 *
 * A scene object's components are rebuilt from YAML whenever the editor changes anything about
 * it (SceneSync re-parses the object), and a terrain's grid is far too large -- and too often
 * unsaved -- to live in that YAML. So the component holds a shared_ptr into this store instead:
 * re-parsing hands back the same live data, unsaved strokes included, and only a change of
 * geometry reshapes it. The store is also where data is written back to disk (save_dirty()) and
 * where a file changed by another tool is picked up again (when the entry has no unsaved edits).
 *
 * Resizes are kept losslessly undoable: changing geometry A -> B remembers the A-shaped data, so
 * an undo that re-parses back to A restores it exactly rather than resampling B again.
 */

#ifndef TOYENGINE_WORLD_HEIGHTMAP_STORE_H
#define TOYENGINE_WORLD_HEIGHTMAP_STORE_H

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <toyengine/world/heightmap_data.h>

namespace toy {
namespace world {

/**
 * @struct HeightmapFiles
 * @brief Where a terrain's data lives on disk (resolved, absolute or CWD-relative paths).
 */
struct HeightmapFiles {
    std::string height;
    std::string splat0;
    std::string splat1;
};

/** @brief Loads a heightmap image into `data` (resampled to its resolution, scaled by its height). */
bool load_heightmap_image(HeightmapData& data, const std::string& path);
/** @brief Writes `data`'s grid as a 16-bit greyscale PNG (0 = 0 m, 65535 = geometry().height). */
bool save_heightmap_image(const HeightmapData& data, const std::string& path);
/** @brief Loads splat map `index` (resampled to the splat resolution). */
bool load_splat_image(HeightmapData& data, int index, const std::string& path);
bool save_splat_image(const HeightmapData& data, int index, const std::string& path);

/**
 * @class HeightmapStore
 * @brief See the file comment.
 */
class HeightmapStore {
public:
    static HeightmapStore& instance();

    /**
     * @brief The live data for `files`, shaped to `geometry`.
     *
     * First call: loads the files that exist (heights, then splats) or, when the heightmap file is
     * missing, runs `generate`. Later calls return the same object; a geometry change reshapes it
     * in place (restoring an earlier shape exactly if one was cached). An entry with no unsaved
     * edits whose height file changed on disk since it was read is reloaded.
     */
    std::shared_ptr<HeightmapData> acquire(const HeightmapFiles& files, const HeightmapGeometry& geometry,
                                           const HeightmapGenerate& generate);

    /** @brief The files an acquired data object was loaded from (empty when unknown). */
    HeightmapFiles files_of(const HeightmapData* data) const;

    /** @brief Writes one entry's files and clears its unsaved flag. */
    bool save(const HeightmapData* data);
    /** @brief Writes every entry with unsaved edits; returns how many were written. */
    int save_dirty();
    /** @brief True when any entry has unsaved edits. */
    bool any_dirty() const;

    /** @brief Forgets every entry (an editor opening a scene fresh; tests). Live holders keep theirs. */
    void clear();

private:
    struct Entry {
        HeightmapFiles files;
        std::shared_ptr<HeightmapData> data;
        std::filesystem::file_time_type height_mtime{};
        /// Earlier shapes of this data, most recent last (see the file comment).
        std::vector<HeightmapData> shapes;
    };

    static std::filesystem::file_time_type mtime_(const std::string& path);
    static void load_entry_(Entry& entry, const HeightmapGeometry& geometry, const HeightmapGenerate& generate);

    std::unordered_map<std::string, Entry> entries_;
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_HEIGHTMAP_STORE_H
