#include "editor/instance/instance_select.h"

#include <algorithm>
#include <cmath>
#include <map>

#include <glm/gtc/quaternion.hpp>

namespace toy::editor {

using toy::instance::ChunkCoord;
using toy::instance::InstanceRecord;
using toy::instance::InstanceRef;
using toy::instance::InstanceSetData;

namespace {

bool same_ref(InstanceRef a, InstanceRef b) { return a.chunk == b.chunk && a.index == b.index; }

const glm::vec3 k_up(0.0f, 0.0f, 1.0f);

} // namespace

bool InstanceSelection::contains(Ref r) const {
    return std::any_of(refs_.begin(), refs_.end(), [&](Ref x) { return same_ref(x, r); });
}

void InstanceSelection::add(Ref r) {
    if (!contains(r)) refs_.push_back(r);
}

void InstanceSelection::remove(Ref r) {
    refs_.erase(std::remove_if(refs_.begin(), refs_.end(), [&](Ref x) { return same_ref(x, r); }), refs_.end());
}

void InstanceSelection::toggle(Ref r) {
    if (contains(r)) remove(r); else add(r);
}

void InstanceSelection::validate(const InstanceSetData& data) {
    refs_.erase(std::remove_if(refs_.begin(), refs_.end(),
                               [&](Ref r) {
                                   const std::vector<InstanceRecord>* v = data.records(r.chunk);
                                   return v == nullptr || r.index >= v->size();
                               }),
                refs_.end());
}

std::vector<ChunkCoord> InstanceSelection::chunks() const {
    std::vector<ChunkCoord> out;
    for (Ref r : refs_) {
        const bool seen = std::any_of(out.begin(), out.end(), [&](ChunkCoord c) { return c == r.chunk; });
        if (!seen) out.push_back(r.chunk);
    }
    return out;
}

glm::vec3 InstanceSelection::centroid(const InstanceSetData& data) const {
    glm::vec3 sum(0.0f);
    std::size_t n = 0;
    for (Ref r : refs_) {
        if (const InstanceRecord* rec = data.at(r)) {
            sum += rec->position;
            ++n;
        }
    }
    return n == 0 ? glm::vec3(0.0f) : sum / static_cast<float>(n);
}

std::size_t InstanceSelection::edit(InstanceSetData& data,
                                    const std::function<void(InstanceRecord&)>& fn) const {
    // Grouped by chunk, so each one is written back once: set_records() bumps a revision, and
    // doing that per record would make a ten-instance edit look like ten separate changes to
    // every consumer watching the chunk.
    std::map<std::pair<int, int>, std::vector<std::uint32_t>> by_chunk;
    for (Ref r : refs_) by_chunk[{r.chunk.x, r.chunk.y}].push_back(r.index);

    std::size_t edited = 0;
    for (const auto& [key, indices] : by_chunk) {
        const ChunkCoord c(key.first, key.second);
        const std::vector<InstanceRecord>* current = data.records(c);
        if (current == nullptr) continue;
        std::vector<InstanceRecord> records = *current;
        for (std::uint32_t i : indices) {
            if (i >= records.size()) continue;
            fn(records[i]);
            ++edited;
        }
        data.set_records(c, std::move(records));
    }
    return edited;
}

std::size_t InstanceSelection::erase(InstanceSetData& data) {
    // Highest index first within each chunk, so removing one never shifts another still to go.
    std::map<std::pair<int, int>, std::vector<std::uint32_t>> by_chunk;
    for (Ref r : refs_) by_chunk[{r.chunk.x, r.chunk.y}].push_back(r.index);

    std::size_t erased = 0;
    for (auto& [key, indices] : by_chunk) {
        const ChunkCoord c(key.first, key.second);
        const std::vector<InstanceRecord>* current = data.records(c);
        if (current == nullptr) continue;
        std::vector<InstanceRecord> records = *current;
        std::sort(indices.begin(), indices.end(), std::greater<std::uint32_t>());
        for (std::uint32_t i : indices) {
            if (i >= records.size()) continue;
            records.erase(records.begin() + static_cast<std::ptrdiff_t>(i));
            ++erased;
        }
        data.set_records(c, std::move(records));
    }
    refs_.clear();
    return erased;
}

// --- per-record edits ---

float instance_yaw(const InstanceRecord& r) {
    const glm::vec3 facing = r.rotation * glm::vec3(1.0f, 0.0f, 0.0f);
    if (std::abs(facing.x) < 1e-6f && std::abs(facing.y) < 1e-6f) return 0.0f;   // facing straight up
    return std::atan2(facing.y, facing.x);
}

void turn_instance(InstanceRecord& r, float radians) {
    if (radians == 0.0f) return;
    r.rotation = glm::normalize(glm::angleAxis(radians, k_up) * r.rotation);
}

void set_instance_yaw(InstanceRecord& r, float radians) {
    turn_instance(r, radians - instance_yaw(r));
}

void scale_instance(InstanceRecord& r, float factor) {
    set_instance_scale(r, r.scale.x * factor);
}

void set_instance_scale(InstanceRecord& r, float scale) {
    r.scale = glm::vec3(std::clamp(scale, 0.01f, 100.0f));
}

void offset_instance(InstanceRecord& r, const glm::vec3& delta) {
    r.position += delta;
}

void orbit_instance(InstanceRecord& r, const glm::vec3& pivot, float radians) {
    if (radians == 0.0f) return;
    const glm::quat spin = glm::angleAxis(radians, k_up);
    r.position = pivot + spin * (r.position - pivot);
    r.rotation = glm::normalize(spin * r.rotation);
}

} // namespace toy::editor
