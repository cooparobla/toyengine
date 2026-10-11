#include <toyengine/world/tile_map_data.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

#include <stb/stb_image.h>

// Defined (with C linkage) by the stb_image_write implementation gfxcoopa compiles once; not part
// of the header's public declarations, so declared here (as png16.cpp does). Returns a malloc'd
// zlib stream.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

namespace toy {
namespace world {

namespace {

std::atomic<std::uint64_t> g_next_uid{1};

/// Journal bound: a long session of single-tile edits merges into few entries, and a consumer
/// further behind than this re-reads the whole grid.
constexpr std::size_t k_journal_cap = 256;

constexpr char k_magic[4] = {'T', 'T', 'I', 'L'};
constexpr std::uint32_t k_version = 1;

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::uint32_t get_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

} // namespace

bool TileRecord::operator==(const TileRecord& o) const {
    return std::memcmp(this, &o, sizeof(TileRecord)) == 0;
}

TileMapData::TileMapData() : uid_(g_next_uid.fetch_add(1)) {}

void TileMapData::resize(int w, int h) {
    width_ = std::max(0, w);
    height_ = std::max(0, h);
    records_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), TileRecord{});
    compressed_.assign(static_cast<std::size_t>(chunks_x_()) * static_cast<std::size_t>(chunks_y_()), {});
    chunk_dirty_.assign(compressed_.size(), 1);
    mark_all_dirty();
}

const TileRecord& TileMapData::get(int x, int y) const {
    static const TileRecord k_empty{};
    if (empty()) return k_empty;
    return records_[index_(std::clamp(x, 0, width_ - 1), std::clamp(y, 0, height_ - 1))];
}

std::vector<TileRecord> TileMapData::capture(const IRect& rect) const {
    std::vector<TileRecord> out;
    const IRect r = rect.clipped(width_, height_);
    out.reserve(static_cast<std::size_t>(r.width()) * static_cast<std::size_t>(r.height()));
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) out.push_back(at(x, y));
    return out;
}

void TileMapData::restore(const IRect& rect, const std::vector<TileRecord>& recs) {
    const IRect r = rect.clipped(width_, height_);
    if (r.empty() || recs.size() != static_cast<std::size_t>(r.width()) * static_cast<std::size_t>(r.height())) return;
    std::size_t i = 0;
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) at(x, y) = recs[i++];
    mark_dirty(r);
}

void TileMapData::mark_dirty(const IRect& rect) {
    const IRect r = rect.clipped(width_, height_);
    if (r.empty()) return;
    ++revision_;
    unsaved_ = true;
    // Merge into the last entry when the two touch and stay small -- a brush stroke's dabs --
    // so a stroke is a few entries, while separate edits stay separate for the consumers.
    if (!journal_.empty()) {
        const IRect& last = journal_.back().rect;
        const bool touch = r.x0 <= last.x1 && last.x0 <= r.x1 && r.y0 <= last.y1 && last.y0 <= r.y1;
        IRect merged = last;
        merged.include(r);
        if (touch && merged.width() * merged.height() <= 64 * 64) {
            journal_.back() = JournalEntry{revision_, merged};
        } else {
            journal_.push_back(JournalEntry{revision_, r});
        }
    } else {
        journal_.push_back(JournalEntry{revision_, r});
    }
    if (journal_.size() > k_journal_cap) {
        journal_floor_ = journal_.front().revision;
        journal_.erase(journal_.begin());
    }
    const int s = k_storage_chunk;
    for (int cy = r.y0 / s; cy <= (r.y1 - 1) / s; ++cy)
        for (int cx = r.x0 / s; cx <= (r.x1 - 1) / s; ++cx)
            chunk_dirty_[static_cast<std::size_t>(cy * chunks_x_() + cx)] = 1;
}

void TileMapData::mark_all_dirty() {
    ++revision_;
    journal_.clear();
    journal_floor_ = revision_;
}

void TileMapData::for_each_dirty_since(std::uint64_t since, const std::function<void(const IRect&)>& fn) const {
    if (since >= revision_ || empty()) return;
    if (since < journal_floor_) {
        fn(IRect{0, 0, width_, height_});
        return;
    }
    for (const JournalEntry& e : journal_) {
        if (e.revision > since) fn(e.rect);
    }
}

IRect TileMapData::dirty_since(std::uint64_t since) const {
    IRect out;
    for_each_dirty_since(since, [&out](const IRect& r) { out.include(r); });
    return out;
}

void TileMapData::set_saved() { unsaved_ = false; }

void TileMapData::assign_baked(TileMapData&& baked) {
    width_ = baked.width_;
    height_ = baked.height_;
    records_ = std::move(baked.records_);
    compressed_.assign(static_cast<std::size_t>(chunks_x_()) * static_cast<std::size_t>(chunks_y_()), {});
    chunk_dirty_.assign(compressed_.size(), 1);
    mark_all_dirty();
    unsaved_ = true;
    needs_bake_ = false;
    ++bakes_done_;
}

bool TileMapData::raycast(const glm::vec3& origin, const glm::vec3& dir, float tile_size, float height_step, float max_t,
                          TileHit& hit) const {
    if (empty() || tile_size <= 0.0f) return false;
    // Clip the ray to the grid's footprint.
    float t0 = 0.0f, t1 = max_t;
    const glm::vec2 lo(0.0f), hi(static_cast<float>(width_) * tile_size, static_cast<float>(height_) * tile_size);
    for (int a = 0; a < 2; ++a) {
        if (std::abs(dir[a]) < 1e-12f) {
            if (origin[a] < lo[a] || origin[a] > hi[a]) return false;
            continue;
        }
        float ta = (lo[a] - origin[a]) / dir[a], tb = (hi[a] - origin[a]) / dir[a];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
    }
    if (t0 > t1) return false;

    const glm::vec3 start = origin + dir * (t0 + 1e-5f);
    int ix = std::clamp(static_cast<int>(std::floor(start.x / tile_size)), 0, width_ - 1);
    int iy = std::clamp(static_cast<int>(std::floor(start.y / tile_size)), 0, height_ - 1);
    const int step_x = dir.x > 0.0f ? 1 : -1, step_y = dir.y > 0.0f ? 1 : -1;
    const float inf = std::numeric_limits<float>::infinity();
    float t_max_x = std::abs(dir.x) > 1e-12f ? ((static_cast<float>(ix + (dir.x > 0.0f ? 1 : 0)) * tile_size) - origin.x) / dir.x : inf;
    float t_max_y = std::abs(dir.y) > 1e-12f ? ((static_cast<float>(iy + (dir.y > 0.0f ? 1 : 0)) * tile_size) - origin.y) / dir.y : inf;
    const float t_delta_x = std::abs(dir.x) > 1e-12f ? tile_size / std::abs(dir.x) : inf;
    const float t_delta_y = std::abs(dir.y) > 1e-12f ? tile_size / std::abs(dir.y) : inf;

    float t_enter = t0;
    glm::vec3 entry_normal(0.0f);
    bool first = true;
    while (ix >= 0 && iy >= 0 && ix < width_ && iy < height_ && t_enter <= t1) {
        const float t_exit = std::min({t_max_x, t_max_y, t1});
        const float top = static_cast<float>(std::max<int>(0, at(ix, iy).steps)) * height_step;
        const float z_enter = origin.z + dir.z * t_enter;
        if (z_enter <= top) {
            // Entered this column below its top: a wall (or the start is already inside).
            hit.tile = glm::ivec2(ix, iy);
            hit.t = t_enter;
            hit.point = origin + dir * t_enter;
            hit.normal = first ? glm::vec3(0.0f, 0.0f, 1.0f) : entry_normal;
            return true;
        }
        if (dir.z < 0.0f) {
            const float t_top = (top - origin.z) / dir.z;
            if (t_top >= t_enter && t_top <= t_exit) {
                hit.tile = glm::ivec2(ix, iy);
                hit.t = t_top;
                hit.point = origin + dir * t_top;
                hit.normal = glm::vec3(0.0f, 0.0f, 1.0f);
                return true;
            }
        }
        first = false;
        if (t_max_x < t_max_y) {
            ix += step_x;
            t_enter = t_max_x;
            t_max_x += t_delta_x;
            entry_normal = glm::vec3(static_cast<float>(-step_x), 0.0f, 0.0f);
        } else {
            iy += step_y;
            t_enter = t_max_y;
            t_max_y += t_delta_y;
            entry_normal = glm::vec3(0.0f, static_cast<float>(-step_y), 0.0f);
        }
    }
    return false;
}

// --- files ---

std::vector<std::uint8_t> TileMapData::pack_chunk_(int cx, int cy) const {
    const int s = k_storage_chunk;
    const int x0 = cx * s, y0 = cy * s, x1 = std::min(width_, x0 + s), y1 = std::min(height_, y0 + s);
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(x1 - x0) * static_cast<std::size_t>(y1 - y0) * sizeof(TileRecord));
    // Field-major (all steps, then all kinds...): equal neighbours line up byte for byte, which
    // is what lets deflate see the terrain's coherence.
    for (int field = 0; field < static_cast<int>(sizeof(TileRecord)); ++field) {
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(&at(x, y));
                raw.push_back(bytes[field]);
            }
        }
    }
    int out_len = 0;
    unsigned char* z = stbi_zlib_compress(raw.data(), static_cast<int>(raw.size()), &out_len, 8);
    std::vector<std::uint8_t> out(z, z + out_len);
    std::free(z);
    return out;
}

bool TileMapData::unpack_chunk_(int cx, int cy, const std::vector<std::uint8_t>& blob) {
    int raw_len = 0;
    char* raw = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(blob.data()), static_cast<int>(blob.size()), &raw_len);
    if (raw == nullptr) return false;
    const int s = k_storage_chunk;
    const int x0 = cx * s, y0 = cy * s, x1 = std::min(width_, x0 + s), y1 = std::min(height_, y0 + s);
    const std::size_t n = static_cast<std::size_t>(x1 - x0) * static_cast<std::size_t>(y1 - y0);
    bool ok = static_cast<std::size_t>(raw_len) == n * sizeof(TileRecord);
    if (ok) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(raw);
        for (int field = 0; field < static_cast<int>(sizeof(TileRecord)); ++field) {
            std::size_t i = 0;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x, ++i) {
                    reinterpret_cast<std::uint8_t*>(&at(x, y))[field] = bytes[static_cast<std::size_t>(field) * n + i];
                }
            }
        }
    }
    std::free(raw);
    return ok;
}

bool TileMapData::save(const std::string& path) {
    if (empty()) return false;
    const int count = chunks_x_() * chunks_y_();
    for (int cy = 0; cy < chunks_y_(); ++cy) {
        for (int cx = 0; cx < chunks_x_(); ++cx) {
            const std::size_t i = static_cast<std::size_t>(cy * chunks_x_() + cx);
            if (chunk_dirty_[i] || compressed_[i].empty()) {
                compressed_[i] = pack_chunk_(cx, cy);
                chunk_dirty_[i] = 0;
            }
        }
    }
    std::vector<std::uint8_t> header(k_magic, k_magic + 4);
    put_u32(header, k_version);
    put_u32(header, static_cast<std::uint32_t>(width_));
    put_u32(header, static_cast<std::uint32_t>(height_));
    put_u32(header, static_cast<std::uint32_t>(k_storage_chunk));
    put_u32(header, static_cast<std::uint32_t>(sizeof(TileRecord)));
    put_u32(header, static_cast<std::uint32_t>(count));
    std::uint32_t offset = 0;
    for (const auto& blob : compressed_) {
        put_u32(header, offset);
        put_u32(header, static_cast<std::uint32_t>(blob.size()));
        offset += static_cast<std::uint32_t>(blob.size());
    }

    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    for (const auto& blob : compressed_) out.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
    if (!out) return false;
    unsaved_ = false;
    return true;
}

bool TileMapData::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (file.size() < 28 || std::memcmp(file.data(), k_magic, 4) != 0) return false;
    const std::uint8_t* p = file.data() + 4;
    if (get_u32(p) != k_version) return false;
    const int w = static_cast<int>(get_u32(p + 4)), h = static_cast<int>(get_u32(p + 8));
    if (get_u32(p + 12) != static_cast<std::uint32_t>(k_storage_chunk) || get_u32(p + 16) != sizeof(TileRecord)) return false;
    const std::uint32_t count = get_u32(p + 20);
    if (w <= 0 || h <= 0 || w > 65536 || h > 65536) return false;
    const std::size_t table = 28, data_start = table + static_cast<std::size_t>(count) * 8;
    if (file.size() < data_start) return false;

    TileMapData loaded;
    loaded.width_ = w;
    loaded.height_ = h;
    loaded.records_.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), TileRecord{});
    if (static_cast<int>(count) != loaded.chunks_x_() * loaded.chunks_y_()) return false;
    loaded.compressed_.resize(count);
    loaded.chunk_dirty_.assign(count, 0);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t off = get_u32(file.data() + table + i * 8), size = get_u32(file.data() + table + i * 8 + 4);
        if (data_start + off + size > file.size()) return false;
        loaded.compressed_[i].assign(file.begin() + static_cast<std::ptrdiff_t>(data_start + off),
                                     file.begin() + static_cast<std::ptrdiff_t>(data_start + off + size));
        const int cx = static_cast<int>(i) % loaded.chunks_x_(), cy = static_cast<int>(i) / loaded.chunks_x_();
        if (!loaded.unpack_chunk_(cx, cy, loaded.compressed_[i])) return false;
    }
    width_ = w;
    height_ = h;
    records_ = std::move(loaded.records_);
    compressed_ = std::move(loaded.compressed_);
    chunk_dirty_ = std::move(loaded.chunk_dirty_);
    mark_all_dirty();
    unsaved_ = false;
    needs_bake_ = false;
    return true;
}

// --- store ---

namespace {

std::int64_t mtime_of(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    return ec ? 0 : static_cast<std::int64_t>(t.time_since_epoch().count());
}

} // namespace

TileMapStore& TileMapStore::instance() {
    static TileMapStore store;
    return store;
}

std::shared_ptr<TileMapData> TileMapStore::acquire(const std::string& path) {
    for (auto& [key, entry] : entries_) {
        if (key != path) continue;
        // Changed on disk by something else, and nothing here would be lost: take it.
        const std::int64_t mtime = mtime_of(path);
        if (mtime != 0 && mtime != entry.mtime && !entry.data->dirty_on_disk() && !entry.data->needs_bake()) {
            if (entry.data->load(path)) entry.mtime = mtime;
        }
        return entry.data;
    }
    auto data = std::make_shared<TileMapData>();
    Entry entry;
    entry.data = data;
    if (!path.empty() && std::filesystem::exists(path) && data->load(path)) entry.mtime = mtime_of(path);
    entries_.emplace_back(path, std::move(entry));
    return data;
}

bool TileMapStore::save(TileMapData& data) {
    for (auto& [key, entry] : entries_) {
        if (entry.data.get() != &data || key.empty()) continue;
        if (!data.save(key)) return false;
        entry.mtime = mtime_of(key);
        return true;
    }
    return false;
}

int TileMapStore::save_dirty() {
    int n = 0;
    for (auto& [key, entry] : entries_) {
        if (!entry.data->dirty_on_disk() || entry.data->empty() || key.empty()) continue;
        if (entry.data->save(key)) {
            entry.mtime = mtime_of(key);
            ++n;
        }
    }
    return n;
}

std::string TileMapStore::path_of(const TileMapData& data) const {
    for (const auto& [key, entry] : entries_) {
        if (entry.data.get() == &data) return key;
    }
    return {};
}

void TileMapStore::clear() { entries_.clear(); }

} // namespace world
} // namespace toy
