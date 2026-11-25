/**
 * @file derived_cache.h
 * @brief Where a project keeps what it DERIVED from an asset, so it only has to derive it once.
 *
 * Some assets are cheap to describe and expensive to realise: a few authored keys that take
 * seconds of generation to turn into the images and stored worlds a scene actually draws. A
 * project can commit that output beside the scene and ship it, which is the right answer for a
 * demo everyone opens. The other cases are a scene being authored right now, and the editor --
 * which rebuilds its scene from the open document every time Play is pressed, so whatever the
 * edit-mode scene generated is generated again the moment you run it.
 *
 * This is where the second kind goes. One directory per source file, under the user's cache
 * directory, holding exactly the files the project's own offline baker would have written beside
 * the scene -- so the loading path is the same path, pointed somewhere else, and a cache entry
 * can be promoted to a committed bake by copying it.
 *
 * **Nothing here understands what is in an entry.** It hands out a directory, remembers which
 * file the directory was derived from, and throws entries away again: when the source is gone,
 * and when the cache has grown past its cap. What goes in it, how it is validated and when it is
 * stale are the project's business -- see the manifest a project writes beside its own output.
 *
 * Three properties the callers depend on:
 *
 * - **It never writes into the project.** A derived file appearing in a source tree is a file the
 *   author has to explain to their version control, so the cache lives in `user_cache_dir()`.
 * - **It is keyed on the source's ABSOLUTE path**, so a scene copied to a scratch directory (a
 *   test) gets its own entry and prunes itself away when that directory does, rather than
 *   colliding with the original.
 * - **It is optional.** `TOY_DERIVED_CACHE=0` turns it off and `enabled()` says so; every caller
 *   is expected to work without it, more slowly.
 */

#ifndef TOYENGINE_IO_DERIVED_CACHE_H
#define TOYENGINE_IO_DERIVED_CACHE_H

#include <cstdint>
#include <filesystem>
#include <string>

namespace toy {
namespace io {

/** @brief How much the derived cache may hold before the oldest entries are dropped. */
inline constexpr std::uint64_t k_derived_cache_default_cap = 4ull * 1024 * 1024 * 1024;

/**
 * @brief Whether the cache is on at all (`TOY_DERIVED_CACHE=0` turns it off).
 *
 * Read once. A caller that gets `false` is expected to carry on generating, which is what it did
 * before any of this existed.
 */
bool derived_cache_enabled();

/**
 * @brief The directory holding what was derived from `source`, created if it is not there.
 *
 * The entry also gets a `source.path` file naming the absolute source, which is the only thing
 * `prune_derived_cache()` needs to tell a live entry from an orphan.
 *
 * @param kind A short name for what the caller derives, so two kinds never share a directory
 *        (letters, digits, `_` and `-`).
 * @param source The file the output is derived from -- a scene, a model, an authored asset.
 *        Must exist; a source with no file on disk has no identity to key on.
 * @return The directory, or an empty path when the cache is off, `source` is empty, or the
 *         directory could not be created. An empty return is not an error: it means "generate".
 */
std::filesystem::path derived_cache_dir(const std::string& kind,
                                        const std::filesystem::path& source);

/**
 * @brief Drops orphaned entries, then the oldest ones until the cache fits `cap` bytes.
 *
 * An entry is an orphan when the file named in its `source.path` no longer exists -- which is
 * what makes a deleted or renamed scene take its derived data with it without anything having to
 * notice the deletion. Age is the entry's newest file, so an entry in use is the last to go.
 *
 * Cheap enough to call at startup: it stats the tree and deletes, and a cache of a few hundred
 * files takes microseconds. Safe to call when the cache is off (it does nothing).
 *
 * @param cap Bytes the cache may keep. 0 empties it.
 * @return Bytes freed.
 */
std::uint64_t prune_derived_cache(std::uint64_t cap = k_derived_cache_default_cap);

/** @brief The cache root (`user_cache_dir()/derived`), whether or not it exists. */
std::filesystem::path derived_cache_root();

} // namespace io
} // namespace toy

#endif // TOYENGINE_IO_DERIVED_CACHE_H
