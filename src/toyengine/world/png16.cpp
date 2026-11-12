#include <toyengine/world/png16.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <stb/stb_image.h>
#include <stb/stb_image_write.h>

// Defined (with C linkage) by the stb_image_write implementation gfxcoopa compiles once; not part
// of the header's public declarations, so declared here. Returns a malloc'd zlib stream.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

namespace toy {
namespace world {

namespace {

std::uint32_t crc32_of(const std::uint8_t* data, std::size_t len, std::uint32_t crc = 0xFFFFFFFFu) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    for (std::size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void put_chunk(std::vector<std::uint8_t>& out, const char type[4], const std::uint8_t* data, std::size_t len) {
    put_u32(out, static_cast<std::uint32_t>(len));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    if (len > 0) out.insert(out.end(), data, data + len);
    put_u32(out, crc32_of(out.data() + start, len + 4) ^ 0xFFFFFFFFu);
}

bool ensure_parent(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    return !ec;
}

} // namespace

bool write_png16_grey(const std::string& path, int w, int h, const std::vector<std::uint16_t>& pixels) {
    if (w <= 0 || h <= 0 || pixels.size() != static_cast<std::size_t>(w) * h) return false;

    // Raw scanlines: one filter byte, then big-endian samples. The "up" filter (2) on all rows but
    // the first compresses smooth terrain far better than none, at no decode cost worth noting.
    const std::size_t stride = static_cast<std::size_t>(w) * 2;
    std::vector<std::uint8_t> raw((stride + 1) * static_cast<std::size_t>(h));
    std::vector<std::uint8_t> prev(stride, 0), row(stride);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::uint16_t v = pixels[static_cast<std::size_t>(y) * w + x];
            row[static_cast<std::size_t>(x) * 2]     = static_cast<std::uint8_t>(v >> 8);
            row[static_cast<std::size_t>(x) * 2 + 1] = static_cast<std::uint8_t>(v & 0xFF);
        }
        std::uint8_t* dst = raw.data() + static_cast<std::size_t>(y) * (stride + 1);
        dst[0] = y == 0 ? 0 : 2;
        for (std::size_t i = 0; i < stride; ++i) {
            dst[1 + i] = y == 0 ? row[i] : static_cast<std::uint8_t>(row[i] - prev[i]);
        }
        prev.swap(row);
    }

    int zlen = 0;
    unsigned char* z = stbi_zlib_compress(raw.data(), static_cast<int>(raw.size()), &zlen, 8);
    if (z == nullptr) return false;

    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> ihdr;
    put_u32(ihdr, static_cast<std::uint32_t>(w));
    put_u32(ihdr, static_cast<std::uint32_t>(h));
    ihdr.push_back(16); // bit depth
    ihdr.push_back(0);  // colour type: greyscale
    ihdr.push_back(0);  // compression
    ihdr.push_back(0);  // filter method
    ihdr.push_back(0);  // no interlace
    put_chunk(png, "IHDR", ihdr.data(), ihdr.size());
    put_chunk(png, "IDAT", z, static_cast<std::size_t>(zlen));
    put_chunk(png, "IEND", nullptr, 0);
    std::free(z);

    if (!ensure_parent(path)) return false;
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "[png16] cannot write " << path << "\n";
        return false;
    }
    f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return static_cast<bool>(f);
}

bool read_png16_grey(const std::string& path, int& w, int& h, std::vector<std::uint16_t>& out) {
    int x = 0, y = 0, comp = 0;
    stbi_us* data = stbi_load_16(path.c_str(), &x, &y, &comp, 1);
    if (data == nullptr) return false;
    w = x;
    h = y;
    out.assign(data, data + static_cast<std::size_t>(x) * y);
    stbi_image_free(data);
    return true;
}

bool write_png_rgba8(const std::string& path, int w, int h, const std::vector<std::uint8_t>& rgba) {
    if (w <= 0 || h <= 0 || rgba.size() != static_cast<std::size_t>(w) * h * 4) return false;
    if (!ensure_parent(path)) return false;
    if (stbi_write_png(path.c_str(), w, h, 4, rgba.data(), w * 4) == 0) {
        std::cerr << "[png16] cannot write " << path << "\n";
        return false;
    }
    return true;
}

bool read_png_rgba8(const std::string& path, int& w, int& h, std::vector<std::uint8_t>& out) {
    int x = 0, y = 0, comp = 0;
    stbi_uc* data = stbi_load(path.c_str(), &x, &y, &comp, 4);
    if (data == nullptr) return false;
    w = x;
    h = y;
    out.assign(data, data + static_cast<std::size_t>(x) * y * 4);
    stbi_image_free(data);
    return true;
}

} // namespace world
} // namespace toy
