#include <toyengine/instance/instance_cache.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

#include <gfxcoopa/engine/data/mesh.h>

namespace toy {
namespace instance {

namespace {

std::atomic<std::uint64_t> g_next_build_id{1};

/** @brief PCG32, so a shuffle is reproducible from a chunk's coordinate alone. */
struct Rng {
    std::uint64_t state = 0x853c49e6748fea9bull;
    explicit Rng(std::uint64_t seed) : state(seed * 6364136223846793005ull + 1442695040888963407ull) {}
    std::uint32_t next() {
        const std::uint64_t old = state;
        state = old * 6364136223846793005ull + 1442695040888963407ull;
        const std::uint32_t xorshifted = static_cast<std::uint32_t>(((old >> 18) ^ old) >> 27);
        const std::uint32_t rot = static_cast<std::uint32_t>(old >> 59);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
    }
    std::uint32_t below(std::uint32_t n) { return n == 0 ? 0 : next() % n; }
};

std::uint64_t chunk_seed(ChunkCoord c, std::uint16_t prototype, std::uint32_t part) {
    const std::uint64_t x = static_cast<std::uint32_t>(c.x), y = static_cast<std::uint32_t>(c.y);
    return (x * 0x9E3779B97F4A7C15ull) ^ (y * 0xC2B2AE3D27D4EB4Full) ^
           (static_cast<std::uint64_t>(prototype) << 48) ^ (static_cast<std::uint64_t>(part) << 32) ^ 0x2545F4914F6CDD1Dull;
}

/**
 * @brief Draw distance for a prototype: authored, or derived from how tall it is.
 *
 * A 0.3 m tuft stops mattering long before a 10 m pine does -- about 150 instance heights,
 * which is the same rule the terrain scatter uses (particles/particle_system_runner.cpp).
 */
float resolve_draw_distance(const InstancePrototype& p) {
    if (p.draw_distance > 0.0f) return p.draw_distance;
    return std::clamp(150.0f * p.nominal_height(), 40.0f, 2000.0f);
}

} // namespace

InstanceRenderCache::Chunk InstanceRenderCache::build_chunk(const InstanceSetData& data, ChunkCoord coord,
                                                            const std::vector<InstancePrototype>& prototypes) {
    Chunk out;
    out.revision = data.chunk_revision(coord);
    const std::vector<InstanceRecord>* records = data.records(coord);
    if (records == nullptr || records->empty()) return out;

    // (prototype, part) -> index into out.parts, so one pass over the records fills every batch.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> keys;
    auto batch_for = [&](std::uint16_t proto, std::uint32_t part) -> PartBatch& {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (keys[i].first == proto && keys[i].second == part) return out.parts[i];
        }
        keys.emplace_back(proto, part);
        PartBatch b;
        b.prototype = proto;
        b.part = part;
        b.bounds_min = glm::vec3(std::numeric_limits<float>::max());
        b.bounds_max = glm::vec3(std::numeric_limits<float>::lowest());
        const InstancePrototype& p = prototypes[proto];
        b.draw_distance = resolve_draw_distance(p);
        b.fade_start = p.fade_start;
        out.parts.push_back(std::move(b));
        return out.parts.back();
    };

    for (const InstanceRecord& r : *records) {
        if (r.hidden() || r.prototype >= prototypes.size()) continue;
        const InstancePrototype& p = prototypes[r.prototype];
        if (!p.ready()) continue;   // still loading: rebuilt when it lands
        const glm::mat4 world = r.matrix();
        // A sphere about each instance rather than its eight transformed corners: this runs per
        // instance per chunk build, and a conservative radius is all culling needs.
        const float scale = std::max({r.scale.x, r.scale.y, r.scale.z});
        const float radius = 0.5f * glm::length(p.bounds_max - p.bounds_min) * scale;
        const glm::vec3 centre = r.position + glm::vec3(0.0f, 0.0f, 0.5f * (p.bounds_min.z + p.bounds_max.z) * scale);
        // The displacement of the twig this instance is pinned to, which the card's own baked
        // world matrix cannot follow. Mirrors the parent's own bend (t-squared up its height) so
        // the two agree exactly; `root - position` rather than the root itself, because the
        // shader only has the instance's own world position to add it back to.
        glm::vec4 foliage(0.0f);
        glm::vec4 foliage1(0.0f);
        if (p.foliage_parent_flex > 0.0f && p.foliage_parent_height > 0.0f) {
            const float t = std::clamp((r.position.z - p.foliage_root.z) / p.foliage_parent_height,
                                       0.0f, 1.0f);
            const float reach = p.foliage_parent_height * (t * t);
            foliage = glm::vec4(p.foliage_parent_flex * reach,
                                p.foliage_parent_freq,
                                p.foliage_root.x - r.position.x,
                                p.foliage_root.y - r.position.y);
            // Reach and height on their own: the parent's DOWNWARD press scales with its reach
            // but not with its flexibility, so it cannot be recovered from the product above.
            foliage1 = glm::vec4(reach, p.foliage_parent_height, 0.0f, 0.0f);
        }
        for (std::uint32_t part = 0; part < p.parts.size(); ++part) {
            PartBatch& b = batch_for(r.prototype, part);
            b.matrices.push_back(world * p.parts[part].local);
            if (foliage.x > 0.0f) {
                b.foliage.resize(b.matrices.size() - 1, glm::vec4(0.0f));
                b.foliage.push_back(foliage);
                b.foliage1.resize(b.matrices.size() - 1, glm::vec4(0.0f));
                b.foliage1.push_back(foliage1);
            }
            b.bounds_min = glm::min(b.bounds_min, centre - glm::vec3(radius));
            b.bounds_max = glm::max(b.bounds_max, centre + glm::vec3(radius));
        }
    }

    out.bounds_min = glm::vec3(std::numeric_limits<float>::max());
    out.bounds_max = glm::vec3(std::numeric_limits<float>::lowest());
    for (PartBatch& b : out.parts) {
        // Shuffled, so drawing the first `n` of them is an even subset of the whole chunk --
        // which is what makes distance thinning a prefix draw rather than a rebuild.
        Rng rng(chunk_seed(coord, b.prototype, b.part));
        // A batch with any parent data carries it for EVERY instance, so index i of the two
        // vectors keeps describing the same instance through the shuffle below.
        if (!b.foliage.empty()) {
            b.foliage.resize(b.matrices.size(), glm::vec4(0.0f));
            b.foliage1.resize(b.matrices.size(), glm::vec4(0.0f));
        }
        for (std::size_t i = b.matrices.size(); i > 1; --i) {
            // Drawn once per iteration, exactly as before: the shuffle must stay identical for
            // every existing painter, or their draw order changes for nothing.
            const std::uint32_t j = rng.below(static_cast<std::uint32_t>(i));
            std::swap(b.matrices[i - 1], b.matrices[j]);
            if (!b.foliage.empty()) {
                std::swap(b.foliage[i - 1], b.foliage[j]);
                std::swap(b.foliage1[i - 1], b.foliage1[j]);
            }
        }
        b.build_id = g_next_build_id.fetch_add(1);
        out.bounds_min = glm::min(out.bounds_min, b.bounds_min);
        out.bounds_max = glm::max(out.bounds_max, b.bounds_max);
    }
    if (out.parts.empty()) {
        out.bounds_min = glm::vec3(0.0f);
        out.bounds_max = glm::vec3(0.0f);
    }
    return out;
}

void InstanceRenderCache::update(const InstanceSetData& data, const std::vector<InstancePrototype>& prototypes,
                                 const glm::vec3& camera, float max_distance, int budget) {
    // A different set, or a prototype that just FINISHED loading: what is built was built
    // without it, so start over rather than reason about which chunks it touched.
    //
    // Deliberately not the other direction. The editor rebuilds a painter's component on every
    // inspector keystroke, which re-requests each prototype's mesh asynchronously and leaves it
    // un-ready for a frame or two. Treating that as a reason to clear would empty the cache on
    // every frame of a slider drag, and the painted world would blink out while you adjusted it.
    // The chunks are still perfectly good; collect_render() simply skips a proxy that cannot
    // draw yet.
    std::vector<std::uint8_t> ready(prototypes.size(), 0);
    for (std::size_t i = 0; i < prototypes.size(); ++i) ready[i] = prototypes[i].ready() ? 1u : 0u;
    bool became_ready = ready.size() != proto_ready_.size();
    for (std::size_t i = 0; i < ready.size() && !became_ready; ++i) {
        became_ready = ready[i] != 0 && proto_ready_[i] == 0;
    }
    if (data.uid() != data_uid_ || became_ready) {
        chunks_.clear();
        data_uid_ = data.uid();
    }
    proto_ready_ = std::move(ready);

    const float cs = data.chunk_size();
    auto chunk_distance = [&](ChunkCoord c) {
        const glm::vec2 lo = data.chunk_origin(c);
        const glm::vec2 d = glm::max(glm::max(lo - glm::vec2(camera), glm::vec2(camera) - (lo + glm::vec2(cs))),
                                     glm::vec2(0.0f));
        return glm::length(d);
    };
    const float keep = max_distance > 0.0f ? max_distance * 1.2f + cs : std::numeric_limits<float>::max();

    // Drop what went out of range, or what the data no longer agrees with.
    for (auto it = chunks_.begin(); it != chunks_.end();) {
        if (chunk_distance(it->first) > keep || data.chunk_revision(it->first) != it->second.revision) {
            it = chunks_.erase(it);
        } else {
            ++it;
        }
    }

    // Build the missing ones, nearest first, within the budget.
    std::vector<std::pair<float, ChunkCoord>> wanted;
    for (ChunkCoord c : data.chunks()) {
        if (chunks_.find(c) != chunks_.end()) continue;
        const float d = chunk_distance(c);
        if (d > keep) continue;
        wanted.emplace_back(d, c);
    }
    std::sort(wanted.begin(), wanted.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        return a.second.y != b.second.y ? a.second.y < b.second.y : a.second.x < b.second.x;
    });
    const int n = budget <= 0 ? static_cast<int>(wanted.size()) : std::min<int>(budget, static_cast<int>(wanted.size()));
    for (int i = 0; i < n; ++i) {
        chunks_.emplace(wanted[static_cast<std::size_t>(i)].second,
                        build_chunk(data, wanted[static_cast<std::size_t>(i)].second, prototypes));
    }
}

std::size_t InstanceRenderCache::instance_count() const {
    std::size_t n = 0;
    for (const auto& [coord, chunk] : chunks_) {
        for (const PartBatch& b : chunk.parts) n += b.matrices.size();
    }
    return n;
}

void InstanceRenderCache::clear() {
    chunks_.clear();
    data_uid_ = 0;
    proto_ready_.clear();
}

} // namespace instance
} // namespace toy
