#include <toyengine/instance/instance_set.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <glm/gtc/matrix_transform.hpp>

#include <stb/stb_image.h>

// Defined (with C linkage) by the stb_image_write implementation gfxcoopa compiles once; not
// part of the header's public declarations, so declared here (as png16.cpp and
// tile_map_data.cpp do). Returns a malloc'd zlib stream.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

namespace toy {
namespace instance {

namespace {

std::atomic<std::uint64_t> g_next_uid{1};

constexpr char k_magic[4] = {'T', 'I', 'N', 'S'};
constexpr std::uint32_t k_version = 1;
/// Header bytes before the chunk index: magic, version, chunk size, record bytes, chunk count.
constexpr std::size_t k_header_bytes = 4 + 4 * 4;
/// Index entry: cx, cy, record count, byte offset, byte length.
constexpr std::size_t k_index_entry_bytes = 5 * 4;

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

void put_f32(std::vector<std::uint8_t>& out, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, 4);
    put_u32(out, bits);
}

std::uint32_t get_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

float get_f32(const std::uint8_t* p) {
    const std::uint32_t bits = get_u32(p);
    float v = 0.0f;
    std::memcpy(&v, &bits, 4);
    return v;
}

std::int64_t mtime_of(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    return ec ? 0 : static_cast<std::int64_t>(t.time_since_epoch().count());
}

} // namespace

glm::mat4 InstanceRecord::matrix() const {
    glm::mat4 m = glm::mat4_cast(rotation);
    m[0] *= scale.x;
    m[1] *= scale.y;
    m[2] *= scale.z;
    m[3] = glm::vec4(position, 1.0f);
    return m;
}

InstanceSetData::InstanceSetData() : uid_(g_next_uid.fetch_add(1)) {}

ChunkCoord InstanceSetData::chunk_of(const glm::vec3& world) const {
    return ChunkCoord(static_cast<int>(std::floor(world.x / chunk_size_)),
                      static_cast<int>(std::floor(world.y / chunk_size_)));
}

void InstanceSetData::set_chunk_size(float metres) {
    const float m = std::max(metres, 1.0f);
    if (m == chunk_size_) return;
    // Re-route everything: chunking is storage, never placement.
    std::vector<InstanceRecord> all;
    all.reserve(count_);
    for (auto& [coord, ch] : chunks_) {
        all.insert(all.end(), ch.records.begin(), ch.records.end());
    }
    chunks_.clear();
    count_ = 0;
    chunk_size_ = m;
    for (const InstanceRecord& r : all) add(r);
    ++revision_;
}

const std::vector<InstanceRecord>* InstanceSetData::records(ChunkCoord c) const {
    const auto it = chunks_.find(c);
    return it == chunks_.end() ? nullptr : &it->second.records;
}

std::vector<ChunkCoord> InstanceSetData::chunks() const {
    std::vector<ChunkCoord> out;
    out.reserve(chunks_.size());
    for (const auto& [coord, ch] : chunks_) {
        if (!ch.records.empty()) out.push_back(coord);
    }
    return out;
}

std::uint64_t InstanceSetData::chunk_revision(ChunkCoord c) const {
    const auto it = chunks_.find(c);
    return it == chunks_.end() ? 0 : it->second.revision;
}

InstanceSetData::Chunk& InstanceSetData::chunk_for_(ChunkCoord c) {
    return chunks_[c];
}

void InstanceSetData::touch_(Chunk& ch, ChunkCoord) {
    ch.revision = ++revision_;
    ch.dirty = true;
    ch.compressed.clear();
    unsaved_ = true;
}

void InstanceSetData::add(const InstanceRecord& r) {
    const ChunkCoord c = chunk_of(r.position);
    Chunk& ch = chunk_for_(c);
    ch.records.push_back(r);
    ++count_;
    touch_(ch, c);
}

void InstanceSetData::set_records(ChunkCoord c, std::vector<InstanceRecord> records) {
    if (records.empty()) {
        const auto it = chunks_.find(c);
        if (it == chunks_.end()) return;
        count_ -= it->second.records.size();
        // Keep the entry so its revision keeps rising -- a cache watching this chunk must be
        // told it emptied, and an erased entry would read as "revision 0, never painted".
        it->second.records.clear();
        touch_(it->second, c);
        return;
    }
    Chunk& ch = chunk_for_(c);
    count_ = count_ - ch.records.size() + records.size();
    ch.records = std::move(records);
    touch_(ch, c);
}

std::vector<ChunkCoord> InstanceSetData::chunks_in_radius(const glm::vec3& centre, float radius) const {
    std::vector<ChunkCoord> out;
    const ChunkCoord lo = chunk_of(centre - glm::vec3(radius, radius, 0.0f));
    const ChunkCoord hi = chunk_of(centre + glm::vec3(radius, radius, 0.0f));
    for (int cy = lo.y; cy <= hi.y; ++cy) {
        for (int cx = lo.x; cx <= hi.x; ++cx) {
            const ChunkCoord c(cx, cy);
            if (chunks_.find(c) != chunks_.end()) out.push_back(c);
        }
    }
    return out;
}

std::size_t InstanceSetData::erase_in_radius(const glm::vec3& centre, float radius, int prototype) {
    const float r2 = radius * radius;
    std::size_t erased = 0;
    for (ChunkCoord c : chunks_in_radius(centre, radius)) {
        erased += erase_if(c, [&](const InstanceRecord& r) {
            if (prototype >= 0 && r.prototype != static_cast<std::uint16_t>(prototype)) return false;
            const glm::vec2 d = glm::vec2(r.position) - glm::vec2(centre);
            return glm::dot(d, d) <= r2;
        });
    }
    return erased;
}

std::size_t InstanceSetData::erase_if(ChunkCoord c, const std::function<bool(const InstanceRecord&)>& pred) {
    const auto it = chunks_.find(c);
    if (it == chunks_.end() || it->second.records.empty()) return 0;
    std::vector<InstanceRecord>& v = it->second.records;
    const std::size_t before = v.size();
    v.erase(std::remove_if(v.begin(), v.end(), pred), v.end());
    const std::size_t erased = before - v.size();
    if (erased != 0) {
        count_ -= erased;
        touch_(it->second, c);
    }
    return erased;
}

bool InstanceSetData::erase(const InstanceRef& ref) {
    const auto it = chunks_.find(ref.chunk);
    if (it == chunks_.end() || ref.index >= it->second.records.size()) return false;
    std::vector<InstanceRecord>& v = it->second.records;
    v.erase(v.begin() + static_cast<std::ptrdiff_t>(ref.index));
    --count_;
    touch_(it->second, ref.chunk);
    return true;
}

void InstanceSetData::query_radius(const glm::vec3& centre, float radius, std::vector<InstanceRef>& out) const {
    const float r2 = radius * radius;
    for (ChunkCoord c : chunks_in_radius(centre, radius)) {
        const std::vector<InstanceRecord>& v = chunks_.at(c).records;
        for (std::uint32_t i = 0; i < v.size(); ++i) {
            const glm::vec2 d = glm::vec2(v[i].position) - glm::vec2(centre);
            if (glm::dot(d, d) <= r2) out.push_back({c, i});
        }
    }
}

bool InstanceSetData::any_within(const glm::vec3& centre, float radius) const {
    const float r2 = radius * radius;
    for (ChunkCoord c : chunks_in_radius(centre, radius)) {
        for (const InstanceRecord& r : chunks_.at(c).records) {
            const glm::vec2 d = glm::vec2(r.position) - glm::vec2(centre);
            if (glm::dot(d, d) <= r2) return true;
        }
    }
    return false;
}

const InstanceRecord* InstanceSetData::at(const InstanceRef& ref) const {
    const auto it = chunks_.find(ref.chunk);
    if (it == chunks_.end() || ref.index >= it->second.records.size()) return nullptr;
    return &it->second.records[ref.index];
}

void InstanceSetData::clear() {
    if (chunks_.empty()) return;
    chunks_.clear();
    count_ = 0;
    ++revision_;
    unsaved_ = true;
}

void InstanceSetData::recount_() {
    count_ = 0;
    for (const auto& [coord, ch] : chunks_) count_ += ch.records.size();
}

// --- files ---

std::vector<std::uint8_t> InstanceSetData::pack_(const std::vector<InstanceRecord>& records) const {
    // Field-major: all the first bytes of every record, then all the second... Neighbouring
    // instances agree in most of their bytes (the same prototype, similar scales, exponents
    // that barely move), and that is the redundancy deflate can actually see.
    constexpr std::size_t bytes = sizeof(InstanceRecord);
    const std::size_t n = records.size();
    std::vector<std::uint8_t> raw(n * bytes);
    for (std::size_t field = 0; field < bytes; ++field) {
        const std::size_t base = field * n;
        for (std::size_t i = 0; i < n; ++i) {
            raw[base + i] = reinterpret_cast<const std::uint8_t*>(&records[i])[field];
        }
    }
    int out_len = 0;
    unsigned char* z = stbi_zlib_compress(raw.data(), static_cast<int>(raw.size()), &out_len, 8);
    if (z == nullptr) return {};
    std::vector<std::uint8_t> out(z, z + out_len);
    std::free(z);
    return out;
}

bool InstanceSetData::unpack_(const std::vector<std::uint8_t>& blob, std::uint32_t count,
                              std::vector<InstanceRecord>& out) {
    constexpr std::size_t bytes = sizeof(InstanceRecord);
    int raw_len = 0;
    char* raw = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(blob.data()),
                                        static_cast<int>(blob.size()), &raw_len);
    if (raw == nullptr) return false;
    const std::size_t n = count;
    const bool ok = static_cast<std::size_t>(raw_len) == n * bytes;
    if (ok) {
        out.assign(n, InstanceRecord{});
        const auto* src = reinterpret_cast<const std::uint8_t*>(raw);
        for (std::size_t field = 0; field < bytes; ++field) {
            const std::size_t base = field * n;
            for (std::size_t i = 0; i < n; ++i) {
                reinterpret_cast<std::uint8_t*>(&out[i])[field] = src[base + i];
            }
        }
    }
    std::free(raw);
    return ok;
}

bool InstanceSetData::save(const std::string& path) {
    if (path.empty()) return false;
    // Chunks that emptied are dropped on the way out: they exist in memory only so a watching
    // cache is told they emptied, and the file has no use for them.
    std::vector<ChunkCoord> order;
    order.reserve(chunks_.size());
    for (auto& [coord, ch] : chunks_) {
        if (ch.records.empty()) continue;
        if (ch.dirty || ch.compressed.empty()) {
            ch.compressed = pack_(ch.records);
            if (ch.compressed.empty()) return false;
            ch.dirty = false;
        }
        order.push_back(coord);
    }
    // Sorted, so the same set always writes the same bytes whatever the hash order was.
    std::sort(order.begin(), order.end(), [](ChunkCoord a, ChunkCoord b) {
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    });

    std::vector<std::uint8_t> header(k_magic, k_magic + 4);
    put_u32(header, k_version);
    put_f32(header, chunk_size_);
    put_u32(header, static_cast<std::uint32_t>(sizeof(InstanceRecord)));
    put_u32(header, static_cast<std::uint32_t>(order.size()));
    std::uint32_t offset = 0;
    for (ChunkCoord c : order) {
        const Chunk& ch = chunks_.at(c);
        put_u32(header, static_cast<std::uint32_t>(c.x));
        put_u32(header, static_cast<std::uint32_t>(c.y));
        put_u32(header, static_cast<std::uint32_t>(ch.records.size()));
        put_u32(header, offset);
        put_u32(header, static_cast<std::uint32_t>(ch.compressed.size()));
        offset += static_cast<std::uint32_t>(ch.compressed.size());
    }

    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    for (ChunkCoord c : order) {
        const Chunk& ch = chunks_.at(c);
        out.write(reinterpret_cast<const char*>(ch.compressed.data()),
                  static_cast<std::streamsize>(ch.compressed.size()));
    }
    if (!out) return false;
    unsaved_ = false;
    return true;
}

bool InstanceSetData::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (file.size() < k_header_bytes || std::memcmp(file.data(), k_magic, 4) != 0) return false;
    const std::uint8_t* p = file.data() + 4;
    if (get_u32(p) != k_version) return false;
    const float chunk_size = get_f32(p + 4);
    if (!(chunk_size >= 1.0f) || get_u32(p + 8) != sizeof(InstanceRecord)) return false;
    const std::uint32_t count = get_u32(p + 12);
    const std::size_t index = k_header_bytes;
    const std::size_t data_start = index + static_cast<std::size_t>(count) * k_index_entry_bytes;
    if (file.size() < data_start) return false;

    // Into a local map first: a file that turns out to be truncated leaves what we had intact.
    std::unordered_map<ChunkCoord, Chunk, ChunkCoordHash> loaded;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* e = file.data() + index + static_cast<std::size_t>(i) * k_index_entry_bytes;
        const ChunkCoord c(static_cast<std::int32_t>(get_u32(e)), static_cast<std::int32_t>(get_u32(e + 4)));
        const std::uint32_t records = get_u32(e + 8);
        const std::uint32_t off = get_u32(e + 12), size = get_u32(e + 16);
        if (data_start + off + size > file.size()) return false;
        Chunk ch;
        ch.compressed.assign(file.begin() + static_cast<std::ptrdiff_t>(data_start + off),
                             file.begin() + static_cast<std::ptrdiff_t>(data_start + off + size));
        if (!unpack_(ch.compressed, records, ch.records)) return false;
        ch.dirty = false;
        ch.revision = ++revision_;
        loaded.emplace(c, std::move(ch));
    }
    chunk_size_ = chunk_size;
    chunks_ = std::move(loaded);
    recount_();
    ++revision_;
    unsaved_ = false;
    return true;
}

// --- store ---

InstanceSetStore& InstanceSetStore::instance() {
    static InstanceSetStore store;
    return store;
}

std::shared_ptr<InstanceSetData> InstanceSetStore::acquire(const std::string& path) {
    for (auto& [key, entry] : entries_) {
        if (key != path) continue;
        // Changed on disk by something else, and nothing here would be lost: take it.
        const std::int64_t mtime = mtime_of(path);
        if (mtime != 0 && mtime != entry.mtime && !entry.data->unsaved()) {
            if (entry.data->load(path)) entry.mtime = mtime;
        }
        return entry.data;
    }
    auto data = std::make_shared<InstanceSetData>();
    Entry entry;
    entry.data = data;
    if (!path.empty() && std::filesystem::exists(path) && data->load(path)) entry.mtime = mtime_of(path);
    entries_.emplace_back(path, std::move(entry));
    return data;
}

bool InstanceSetStore::save(InstanceSetData& data) {
    for (auto& [key, entry] : entries_) {
        if (entry.data.get() != &data || key.empty() || !entry.persistent) continue;
        if (!data.save(key)) return false;
        entry.mtime = mtime_of(key);
        return true;
    }
    return false;
}

int InstanceSetStore::save_dirty() {
    int n = 0;
    for (auto& [key, entry] : entries_) {
        if (!entry.data->unsaved() || key.empty() || !entry.persistent) continue;
        if (entry.data->save(key)) {
            entry.mtime = mtime_of(key);
            ++n;
        }
    }
    return n;
}

bool InstanceSetStore::any_dirty() const {
    for (const auto& [key, entry] : entries_) {
        if (!key.empty() && entry.persistent && entry.data->unsaved()) return true;
    }
    return false;
}

std::shared_ptr<InstanceSetData> InstanceSetStore::adopt_transient(const std::string& key) {
    for (auto& [existing, entry] : entries_) {
        if (existing != key) continue;
        // An existing persistent entry is NOT silently downgraded: that would be a painted set
        // quietly losing its ability to save because a graph picked a colliding name.
        if (!entry.persistent) return entry.data;
        return nullptr;
    }
    auto data = std::make_shared<InstanceSetData>();
    Entry entry;
    entry.data       = data;
    entry.persistent = false;   // Never loaded from, never written to, disk.
    entries_.emplace_back(key, std::move(entry));
    return data;
}

bool InstanceSetStore::forget(const std::string& key) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->first != key) continue;
        entries_.erase(it);
        return true;
    }
    return false;
}

bool InstanceSetStore::is_transient(const std::string& key) const {
    for (const auto& [existing, entry] : entries_) {
        if (existing == key) return !entry.persistent;
    }
    return false;
}

std::string InstanceSetStore::path_of(const InstanceSetData& data) const {
    for (const auto& [key, entry] : entries_) {
        if (entry.data.get() == &data) return key;
    }
    return {};
}

void InstanceSetStore::clear() { entries_.clear(); }

} // namespace instance
} // namespace toy
