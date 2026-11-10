/**
 * @file image_writer.h
 * @brief Encodes an `Image` to a PNG file on disk.
 *
 * This is the one header in `coopa::maps` that pulls in an implementation
 * rather than just declarations, because stb_image_write is a single-header
 * library with no separate compilation unit.
 *
 * `STB_IMAGE_WRITE_STATIC` gives every translation unit that includes this
 * header its own internal-linkage copy of the encoder. The alternatives are
 * both worse: defining `STB_IMAGE_WRITE_IMPLEMENTATION` with external linkage
 * in a header is a one-definition-rule violation the moment a second
 * translation unit includes it, and moving the implementation into a `.cpp`
 * would stop `coopa::maps` being header-only. The
 * cost is a duplicated encoder in each translation unit that writes a PNG,
 * which in practice is one.
 */

#ifndef COOPA_MAPS_IMAGE_WRITER_H
#define COOPA_MAPS_IMAGE_WRITER_H

#include <string>

#include <coopa/maps/image.h>

#ifndef STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_STATIC
#endif
#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include <stb/stb_image_write.h>

namespace coopa {
namespace maps {

/**
 * @brief Sets the deflate effort every later `write_png()` in this TU will use.
 *
 * Encoding is the single most expensive thing this library does, so the knob is
 * worth having: it trades file size for time and changes no pixel. The effect is
 * modest once writes are threaded -- level 1 against stb's default of 8 is about
 * 17% faster for 2% larger -- and pronounced when they are not.
 *
 * **Call this before starting any threads, not per write.** stb keeps the
 * setting in a mutable global that its encoder reads as it runs, so a
 * `write_png()` that set it on entry would race with every other `write_png()`
 * running at the time. Writing it once up front, before the work fans out, gives
 * the write a happens-before edge to every read and leaves the encoder with
 * read-only state. Thread sanitizer found the other arrangement immediately.
 *
 * Under `STB_IMAGE_WRITE_STATIC` the global is this translation unit's own copy,
 * so this affects only `write_png()` calls compiled alongside it.
 *
 * @param level stb's deflate effort, 1 to 9; higher is smaller and slower.
 *              Values outside that range are ignored.
 */
inline void set_png_compression_level(int level) {
    if (level >= 1 && level <= 9) {
        stbi_write_png_compression_level = level;
    }
}

/**
 * @brief Writes an image to a PNG file.
 *
 * Safe to call concurrently for different images, which is what makes a parallel
 * export possible: stb's encoder touches no shared mutable state of its own, and
 * the one setting it does read is left alone here -- see
 * `set_png_compression_level()`.
 *
 * @param filepath Destination path, including the `.png` extension.
 * @param image The image to encode; must have a non-empty pixel buffer.
 * @return True on success, false if the image is empty or the file cannot be written.
 */
inline bool write_png(const std::string& filepath, const Image& image) {
    if (image.width <= 0 || image.height <= 0 || image.pixels.empty()) {
        return false;
    }
    return stbi_write_png(filepath.c_str(), image.width, image.height, image.channels,
                          image.pixels.data(), image.width * image.channels) != 0;
}

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_IMAGE_WRITER_H
