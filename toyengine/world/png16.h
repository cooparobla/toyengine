/**
 * @file png16.h
 * @brief Greyscale 16-bit PNG read/write (heightmaps) and RGBA8 read/write (splat maps).
 *
 * stb_image reads 16-bit PNGs (stbi_load_16) but stb_image_write only writes 8-bit ones, and a
 * heightmap quantised to 256 levels terraces visibly. The writer here emits the PNG chunks itself
 * and borrows stb_image_write's exported zlib compressor (stbi_zlib_compress) for IDAT, so no new
 * dependency is vendored.
 */

#ifndef TOYENGINE_WORLD_PNG16_H
#define TOYENGINE_WORLD_PNG16_H

#include <cstdint>
#include <string>
#include <vector>

namespace toy {
namespace world {

/**
 * @brief Writes a 16-bit greyscale PNG.
 * @param path   Destination; parent directories are created.
 * @param pixels w * h samples, row-major, row 0 written first.
 * @return False (with a message on stderr) when the file cannot be written.
 */
bool write_png16_grey(const std::string& path, int w, int h, const std::vector<std::uint16_t>& pixels);

/**
 * @brief Reads any PNG (or other stb-supported image) as 16-bit greyscale.
 *
 * 8-bit sources are widened by stb (v * 257) and colour sources are reduced to luma by stb, so
 * an 8-bit or RGB heightmap exported by another tool still loads.
 * @return False when the file is missing or undecodable; `out` is left untouched.
 */
bool read_png16_grey(const std::string& path, int& w, int& h, std::vector<std::uint16_t>& out);

/** @brief Writes an RGBA8 PNG (parent directories created). */
bool write_png_rgba8(const std::string& path, int w, int h, const std::vector<std::uint8_t>& rgba);

/** @brief Reads an image as RGBA8. False when missing or undecodable. */
bool read_png_rgba8(const std::string& path, int& w, int& h, std::vector<std::uint8_t>& out);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_PNG16_H
