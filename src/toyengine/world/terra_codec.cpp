#include <toyengine/world/terra_codec.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <stb/stb_image.h>

#include <toyengine/world/terra_data.h>

// Defined (with C linkage) by the stb_image_write implementation gfxcoopa compiles once; not
// part of its public header, so declared here exactly as tile_map_data.cpp and png16.cpp do.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

namespace toy {
namespace world {

namespace {

constexpr char k_magic[4] = {'T', 'E', 'R', 'A'};
constexpr std::uint32_t k_version = 1;

/// One plane-table entry: id u32, elem_bytes u32, sub u32, plus u32, codec u32, index_off u64.
constexpr std::size_t k_plane_entry_bytes = 28;

/** @brief Which plane a table entry names. `0` and anything else unrecognised: skip, don't fail. */
enum class PlaneId : std::uint32_t { Tiles = 1, Height = 2, Layer0 = 3, Layer1 = 4 };

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put_f32(std::vector<std::uint8_t>& out, float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, 4);
    put_u32(out, bits);
}
std::uint32_t get_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint64_t get_u64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return v;
}
float get_f32(const std::uint8_t* p) {
    const std::uint32_t bits = get_u32(p);
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

int storage_chunks_x(int tiles_x) { return (tiles_x + k_terra_storage_chunk - 1) / k_terra_storage_chunk; }
int storage_chunks_y(int tiles_y) { return (tiles_y + k_terra_storage_chunk - 1) / k_terra_storage_chunk; }

/** @brief One storage chunk's extent, in TILES -- clipped at the grid's edge, never padded. */
struct TileRange { int x0, y0, x1, y1; };
TileRange storage_chunk_tiles(int cx, int cy, int tiles_x, int tiles_y) {
    const int s = k_terra_storage_chunk;
    return {cx * s, cy * s, std::min(tiles_x, cx * s + s), std::min(tiles_y, cy * s + s)};
}

/** @brief A tile-space range mapped into a plane's own units (`sub` per tile, `plus` for the
 *         height plane's shared corner row/column), clipped to the plane's extent. */
struct PlaneRange { int x0, y0, x1, y1; };
PlaneRange to_plane_range(const TileRange& t, int sub, int plus, int native_w, int native_h) {
    PlaneRange r{t.x0 * sub, t.y0 * sub, t.x1 * sub + plus, t.y1 * sub + plus};
    r.x1 = std::min(r.x1, native_w);
    r.y1 = std::min(r.y1, native_h);
    return r;
}

/** @brief Field-major (SoA) byte transpose of [x0,x1) x [y0,y1) of a `stride`-wide plane of
 *         `elem_bytes`-byte elements, then one zlib stream. Equal neighbours line up byte for
 *         byte, which is what lets deflate see the terrain's coherence (the `.tiles` trick,
 *         generalised to any element size). */
std::vector<std::uint8_t> pack_range(const std::uint8_t* base, int stride, int elem_bytes, int x0, int y0, int x1,
                                     int y1) {
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(x1 - x0) * static_cast<std::size_t>(y1 - y0) * static_cast<std::size_t>(elem_bytes));
    for (int field = 0; field < elem_bytes; ++field) {
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) raw.push_back(base[(static_cast<std::size_t>(y) * stride + x) * elem_bytes + field]);
        }
    }
    int out_len = 0;
    unsigned char* z = stbi_zlib_compress(raw.data(), static_cast<int>(raw.size()), &out_len, 8);
    std::vector<std::uint8_t> out(z, z + out_len);
    std::free(z);
    return out;
}

/** @brief Inverse of pack_range(), writing directly into a full plane's buffer at its own stride. */
bool unpack_range(std::uint8_t* base, int stride, int elem_bytes, int x0, int y0, int x1, int y1,
                  const std::uint8_t* blob, std::size_t blob_size, std::uint32_t expected_raw) {
    int raw_len = 0;
    char* raw = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(blob), static_cast<int>(blob_size), &raw_len);
    if (raw == nullptr) return false;
    const std::size_t n = static_cast<std::size_t>(x1 - x0) * static_cast<std::size_t>(y1 - y0);
    const bool ok = static_cast<std::uint32_t>(raw_len) == expected_raw && static_cast<std::size_t>(raw_len) == n * static_cast<std::size_t>(elem_bytes);
    if (ok) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(raw);
        for (int field = 0; field < elem_bytes; ++field) {
            std::size_t i = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x, ++i)
                    base[(static_cast<std::size_t>(y) * stride + x) * elem_bytes + field] = bytes[static_cast<std::size_t>(field) * n + i];
        }
    }
    std::free(raw);
    return ok;
}

/** @brief Inverse of pack_range(), into a LOCAL buffer sized exactly to the range (no plane
 *         stride) -- what the tile plane uses, since TerraData exposes no raw mutable array for
 *         it, only at()/restore(). */
bool unpack_range_local(int elem_bytes, int x1_minus_x0, int y1_minus_y0, const std::uint8_t* blob,
                        std::size_t blob_size, std::uint32_t expected_raw, std::vector<std::uint8_t>& out) {
    int raw_len = 0;
    char* raw = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(blob), static_cast<int>(blob_size), &raw_len);
    if (raw == nullptr) return false;
    const std::size_t n = static_cast<std::size_t>(x1_minus_x0) * static_cast<std::size_t>(y1_minus_y0);
    const bool ok = static_cast<std::uint32_t>(raw_len) == expected_raw && static_cast<std::size_t>(raw_len) == n * static_cast<std::size_t>(elem_bytes);
    if (ok) {
        out.resize(n * static_cast<std::size_t>(elem_bytes));
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(raw);
        for (int field = 0; field < elem_bytes; ++field) {
            for (std::size_t i = 0; i < n; ++i) out[i * static_cast<std::size_t>(elem_bytes) + field] = bytes[static_cast<std::size_t>(field) * n + i];
        }
    }
    std::free(raw);
    return ok;
}

struct PlaneSpec {
    PlaneId id;
    int elem_bytes;
    int sub;
    int plus;
    int native_w, native_h;
};

} // namespace

bool save_terra(const std::string& path, const TerraData& data) {
    if (data.empty()) return false;
    const TerraGeometry& g = data.geometry();
    const int tiles_x = g.tiles_x, tiles_y = g.tiles_y;
    const int scx = storage_chunks_x(tiles_x), scy = storage_chunks_y(tiles_y);
    const int chunk_count = scx * scy;

    std::vector<PlaneSpec> specs;
    specs.push_back({PlaneId::Tiles, static_cast<int>(sizeof(TileCell)), 1, 0, tiles_x, tiles_y});
    if (data.has_heights()) specs.push_back({PlaneId::Height, 4, g.height_sub, 1, data.height_nx(), data.height_ny()});
    specs.push_back({PlaneId::Layer0, 4, g.layer_sub, 0, data.layer_nx(), data.layer_ny()});
    specs.push_back({PlaneId::Layer1, 4, g.layer_sub, 0, data.layer_nx(), data.layer_ny()});

    auto base_of = [&](PlaneId id) -> const std::uint8_t* {
        switch (id) {
            case PlaneId::Tiles: return reinterpret_cast<const std::uint8_t*>(data.tiles().data());
            case PlaneId::Height: return reinterpret_cast<const std::uint8_t*>(data.heights().data());
            case PlaneId::Layer0: return data.layer_plane(0).data();
            case PlaneId::Layer1: return data.layer_plane(1).data();
        }
        return nullptr;
    };

    // Compress every chunk of every plane first, so each plane's total size (and so the next
    // plane's index_off) is known before anything is written.
    struct Packed {
        PlaneSpec spec;
        std::vector<std::vector<std::uint8_t>> blobs;
        std::vector<std::uint32_t> raw_sizes;
    };
    std::vector<Packed> planes;
    for (const PlaneSpec& spec : specs) {
        Packed p;
        p.spec = spec;
        const std::uint8_t* base = base_of(spec.id);
        for (int cy = 0; cy < scy; ++cy) {
            for (int cx = 0; cx < scx; ++cx) {
                const TileRange t = storage_chunk_tiles(cx, cy, tiles_x, tiles_y);
                const PlaneRange r = to_plane_range(t, spec.sub, spec.plus, spec.native_w, spec.native_h);
                p.blobs.push_back(pack_range(base, spec.native_w, spec.elem_bytes, r.x0, r.y0, r.x1, r.y1));
                p.raw_sizes.push_back(static_cast<std::uint32_t>(r.x1 - r.x0) * static_cast<std::uint32_t>(r.y1 - r.y0) *
                                      static_cast<std::uint32_t>(spec.elem_bytes));
            }
        }
        planes.push_back(std::move(p));
    }

    // Lay out: header(32) + geometry(32) + plane table(planes.size() * k_plane_entry_bytes),
    // then each plane's own chunk index (chunk_count * 16) followed immediately by its blobs.
    std::uint64_t pos = 32 + 32 + static_cast<std::uint64_t>(planes.size()) * k_plane_entry_bytes;
    std::vector<std::uint64_t> index_offs(planes.size());
    for (std::size_t i = 0; i < planes.size(); ++i) {
        index_offs[i] = pos;
        pos += static_cast<std::uint64_t>(chunk_count) * 16;
        for (const auto& blob : planes[i].blobs) pos += blob.size();
    }

    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(pos));
    out.insert(out.end(), k_magic, k_magic + 4);
    put_u32(out, k_version);
    put_u32(out, static_cast<std::uint32_t>(tiles_x));
    put_u32(out, static_cast<std::uint32_t>(tiles_y));
    put_u32(out, static_cast<std::uint32_t>(k_terra_storage_chunk));
    put_u32(out, static_cast<std::uint32_t>(planes.size()));
    put_u32(out, 0); // flags
    put_u32(out, 0); // reserved

    put_f32(out, g.tile_size);
    put_f32(out, g.height);
    put_f32(out, g.height_step);
    put_u32(out, static_cast<std::uint32_t>(g.height_sub));
    put_u32(out, static_cast<std::uint32_t>(g.layer_sub));
    put_u32(out, static_cast<std::uint32_t>(g.chunk_tiles));
    put_u32(out, 0);
    put_u32(out, 0);

    for (std::size_t i = 0; i < planes.size(); ++i) {
        put_u32(out, static_cast<std::uint32_t>(planes[i].spec.id));
        put_u32(out, static_cast<std::uint32_t>(planes[i].spec.elem_bytes));
        put_u32(out, static_cast<std::uint32_t>(planes[i].spec.sub));
        put_u32(out, static_cast<std::uint32_t>(planes[i].spec.plus));
        put_u32(out, 1); // codec: zlib over the SoA transpose
        put_u64(out, index_offs[i]);
    }

    // Header and table are written; append each plane's chunk index (absolute file offsets,
    // now known from the layout pass above) followed immediately by its blobs.
    for (std::size_t pi = 0; pi < planes.size(); ++pi) {
        const Packed& p = planes[pi];
        std::uint64_t blob_pos = index_offs[pi] + static_cast<std::uint64_t>(chunk_count) * 16;
        for (std::size_t c = 0; c < p.blobs.size(); ++c) {
            put_u64(out, blob_pos);
            put_u32(out, static_cast<std::uint32_t>(p.blobs[c].size()));
            put_u32(out, p.raw_sizes[c]);
            blob_pos += p.blobs[c].size();
        }
        for (const auto& blob : p.blobs) out.insert(out.end(), blob.begin(), blob.end());
    }

    std::error_code ec;
    const std::filesystem::path fp(path);
    if (fp.has_parent_path()) std::filesystem::create_directories(fp.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(f);
}

bool load_terra(const std::string& path, TerraData& data) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (file.size() < 64 || std::memcmp(file.data(), k_magic, 4) != 0) return false;

    const std::uint8_t* h = file.data() + 4;
    if (get_u32(h) != k_version) return false;
    const int tiles_x = static_cast<int>(get_u32(h + 4)), tiles_y = static_cast<int>(get_u32(h + 8));
    if (tiles_x <= 0 || tiles_y <= 0 || tiles_x > 65536 || tiles_y > 65536) return false;
    if (get_u32(h + 12) != static_cast<std::uint32_t>(k_terra_storage_chunk)) return false;
    const std::uint32_t plane_count = get_u32(h + 16);
    if (plane_count == 0 || plane_count > 64) return false;

    const std::uint8_t* geo = file.data() + 32;
    TerraGeometry g;
    g.tiles_x = tiles_x;
    g.tiles_y = tiles_y;
    g.tile_size = get_f32(geo);
    g.height = get_f32(geo + 4);
    g.height_step = get_f32(geo + 8);
    g.height_sub = static_cast<int>(get_u32(geo + 12));
    g.layer_sub = static_cast<int>(get_u32(geo + 16));
    g.chunk_tiles = static_cast<int>(get_u32(geo + 20));

    const std::size_t table_off = 64;
    const std::size_t table_bytes = static_cast<std::size_t>(plane_count) * k_plane_entry_bytes;
    if (file.size() < table_off + table_bytes) return false;

    TerraData scratch(g.sanitized());
    const TerraGeometry& sg = scratch.geometry();
    const int scx = storage_chunks_x(sg.tiles_x), scy = storage_chunks_y(sg.tiles_y);
    const std::uint32_t chunk_count = static_cast<std::uint32_t>(scx) * static_cast<std::uint32_t>(scy);

    bool saw_tiles = false, saw_height = false, saw_layer0 = false, saw_layer1 = false;

    for (std::uint32_t pi = 0; pi < plane_count; ++pi) {
        const std::uint8_t* e = file.data() + table_off + static_cast<std::size_t>(pi) * k_plane_entry_bytes;
        const std::uint32_t id_raw = get_u32(e);
        const int elem_bytes = static_cast<int>(get_u32(e + 4));
        const int sub = static_cast<int>(get_u32(e + 8));
        const int plus = static_cast<int>(get_u32(e + 12));
        const std::uint32_t codec = get_u32(e + 16);
        const std::uint64_t index_off = get_u64(e + 20);

        PlaneId id;
        int expected_elem_bytes = 0, expected_sub = 0, expected_plus = 0, native_w = 0, native_h = 0;
        bool* seen = nullptr;
        switch (id_raw) {
            case static_cast<std::uint32_t>(PlaneId::Tiles):
                id = PlaneId::Tiles; expected_elem_bytes = static_cast<int>(sizeof(TileCell)); expected_sub = 1;
                expected_plus = 0; native_w = sg.tiles_x; native_h = sg.tiles_y; seen = &saw_tiles;
                break;
            case static_cast<std::uint32_t>(PlaneId::Height):
                id = PlaneId::Height; expected_elem_bytes = 4; expected_sub = sg.height_sub; expected_plus = 1;
                native_w = scratch.height_nx(); native_h = scratch.height_ny(); seen = &saw_height;
                break;
            case static_cast<std::uint32_t>(PlaneId::Layer0):
                id = PlaneId::Layer0; expected_elem_bytes = 4; expected_sub = sg.layer_sub; expected_plus = 0;
                native_w = scratch.layer_nx(); native_h = scratch.layer_ny(); seen = &saw_layer0;
                break;
            case static_cast<std::uint32_t>(PlaneId::Layer1):
                id = PlaneId::Layer1; expected_elem_bytes = 4; expected_sub = sg.layer_sub; expected_plus = 0;
                native_w = scratch.layer_nx(); native_h = scratch.layer_ny(); seen = &saw_layer1;
                break;
            default:
                continue; // Unknown plane: forward-compatible skip, not a failure.
        }
        if (elem_bytes != expected_elem_bytes || sub != expected_sub || plus != expected_plus || codec != 1) return false;
        if (index_off + static_cast<std::uint64_t>(chunk_count) * 16 > file.size()) return false;
        *seen = true;

        for (std::uint32_t c = 0; c < chunk_count; ++c) {
            const std::uint8_t* idx = file.data() + index_off + static_cast<std::size_t>(c) * 16;
            const std::uint64_t blob_off = get_u64(idx);
            const std::uint32_t blob_size = get_u32(idx + 8), raw_size = get_u32(idx + 12);
            if (blob_off + blob_size > file.size()) return false;
            const int cx = static_cast<int>(c) % scx, cy = static_cast<int>(c) / scx;
            const TileRange t = storage_chunk_tiles(cx, cy, sg.tiles_x, sg.tiles_y);
            const PlaneRange r = to_plane_range(t, sub, plus, native_w, native_h);
            const std::uint8_t* blob = file.data() + blob_off;

            bool ok = false;
            if (id == PlaneId::Tiles) {
                std::vector<std::uint8_t> bytes;
                ok = unpack_range_local(elem_bytes, r.x1 - r.x0, r.y1 - r.y0, blob, blob_size, raw_size, bytes);
                if (ok) {
                    std::vector<TileCell> cells(static_cast<std::size_t>(r.x1 - r.x0) * (r.y1 - r.y0));
                    std::memcpy(cells.data(), bytes.data(), bytes.size());
                    scratch.restore(IRect{r.x0, r.y0, r.x1, r.y1}, cells);
                }
            } else if (id == PlaneId::Height) {
                ok = unpack_range(reinterpret_cast<std::uint8_t*>(scratch.heights().data()), native_w, elem_bytes, r.x0,
                                  r.y0, r.x1, r.y1, blob, blob_size, raw_size);
            } else {
                const int which = id == PlaneId::Layer0 ? 0 : 1;
                ok = unpack_range(scratch.layer_plane(which).data(), native_w, elem_bytes, r.x0, r.y0, r.x1, r.y1, blob,
                                  blob_size, raw_size);
            }
            if (!ok) return false;
        }
    }

    if (!saw_tiles || !saw_layer0 || !saw_layer1) return false;
    if (scratch.has_heights() != saw_height) return false;

    scratch.mark_all_dirty();
    // A file that loaded successfully is committed content, whatever filled it originally -- a
    // mapcoopa bake or a hand sculpt -- so it needs no bake of its own.
    scratch.clear_needs_bake();
    scratch.set_saved();
    data.assign_content(scratch);
    data.set_saved();
    return true;
}

} // namespace world
} // namespace toy
