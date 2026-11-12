/**
 * @file terra_store.h
 * @brief Process-wide owner of every terrain's `TerraData`, keyed by its `.terra` file.
 *
 * One kind of storage, whichever meshing style draws it. A scene object's components are
 * rebuilt from YAML whenever the editor changes anything about it (SceneSync re-parses the
 * object), and a terrain's grid is far too large --
 * and too often unsaved -- to live in that YAML. So the component holds a shared_ptr into this
 * store instead: re-parsing hands back the same live data, unsaved strokes included, and only a
 * geometry change reshapes it. The store is also where data is written back to disk
 * (save_dirty()) and where a file changed by another tool is picked up again (when the entry has
 * no unsaved edits).
 *
 * Resizes are kept losslessly undoable: changing geometry A -> B remembers the A-shaped data, so
 * an undo that re-parses back to A restores it exactly rather than resampling B again.
 */

#ifndef TOYENGINE_WORLD_TERRA_STORE_H
#define TOYENGINE_WORLD_TERRA_STORE_H

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <toyengine/world/terra_data.h>

namespace toy {
namespace world {

/**
 * @class TerraStore
 * @brief See the file comment.
 */
class TerraStore {
public:
    static TerraStore& instance();

    /**
     * @brief The live data for `path`, shaped to `geometry`.
     *
     * First call: loads `path` if it exists, otherwise leaves a fresh grid that needs_bake().
     * Later calls return the same object; a geometry change reshapes it in place (restoring an
     * earlier shape exactly if one was cached). An entry with no unsaved edits whose file changed
     * on disk since it was read is reloaded.
     */
    std::shared_ptr<TerraData> acquire(const std::string& path, const TerraGeometry& geometry);

    /** @brief The file an acquired data object was loaded from (empty when unknown). */
    std::string path_of(const TerraData* data) const;

    /** @brief Writes one entry's file and clears its unsaved flag. */
    bool save(const TerraData* data);
    /** @brief Writes every entry with unsaved edits; returns how many were written. */
    int save_dirty();
    /** @brief True when any entry has unsaved edits. */
    bool any_dirty() const;

    /** @brief Forgets every entry (an editor opening a scene fresh; tests). Live holders keep theirs. */
    void clear();

private:
    struct Entry {
        std::string path;
        std::shared_ptr<TerraData> data;
        std::filesystem::file_time_type mtime{};
        std::vector<TerraData> shapes;   ///< Earlier shapes of this data, most recent last.
    };

    static std::filesystem::file_time_type mtime_(const std::string& path);
    static void load_entry_(Entry& entry, const TerraGeometry& geometry);

    std::unordered_map<std::string, Entry> entries_;
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRA_STORE_H
