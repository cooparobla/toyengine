#include <toyengine/render/passes/gpu_particle_pass.h>

namespace toy {
namespace render {
namespace passes {

void GpuParticlePass::record(coopa::gfx::command::CommandBuffer& cmd, uint32_t frame_slot, const std::vector<GpuParticleJob>& jobs) {
    using coopa::gfx::BufferAccess;
    ++frame_counter_;
    frame_slot %= kFrames;
    read_back_(frame_slot);
    retire_unused_();

    active_.clear();
    for (const GpuParticleJob& job : jobs) {
        if (job.capacity == 0) continue;
        State* st = state_for_(job);
        if (!st) continue;
        st->last_used = frame_counter_;
        st->params[frame_slot]->upload(&job.params, sizeof(GpuParticleParams));
        active_.push_back({st, &job});
    }
    if (active_.empty()) return;

    // Last frame's draw (vertex + indirect reads) and header copy must finish before this
    // frame overwrites the same buffers.
    cmd.buffer_barrier(BufferAccess::VertexAttribute | BufferAccess::Indirect | BufferAccess::TransferRead,
                       BufferAccess::TransferWrite | BufferAccess::ComputeWrite | BufferAccess::ComputeRead);
    for (auto& a : active_) {
        cmd.fill_buffer(*a.st->lists, 0u, 4, 4);   // instance_count = 0
        if (a.job->sort) cmd.fill_buffer(*a.st->keys, 0xFFFFFFFFu);
    }
    cmd.transfer_to_compute_barrier();

    bool any_init = false;
    for (auto& a : active_) {
        if (!a.st->needs_init) continue;
        if (!any_init) cmd.bind_pipeline(*init_);
        any_init = true;
        bind_(cmd, *a.st, frame_slot, Push{});
        cmd.dispatch(coopa::gfx::pipeline::ComputePipeline::groups_for(a.st->capacity, 64));
        a.st->needs_init = false;
    }
    if (any_init) cmd.compute_to_compute_barrier();

    cmd.bind_pipeline(*simulate_);
    for (auto& a : active_) {
        bind_(cmd, *a.st, frame_slot, Push{});
        cmd.dispatch(coopa::gfx::pipeline::ComputePipeline::groups_for(a.st->capacity, 64));
    }
    cmd.compute_to_compute_barrier();

    bool any_emit = false;
    for (auto& a : active_) {
        const uint32_t n = static_cast<uint32_t>(a.job->params.frame.z);
        if (n == 0) continue;
        if (!any_emit) cmd.bind_pipeline(*emit_);
        any_emit = true;
        bind_(cmd, *a.st, frame_slot, Push{});
        cmd.dispatch(coopa::gfx::pipeline::ComputePipeline::groups_for(n, 64));
    }
    if (any_emit) cmd.compute_to_compute_barrier();

    record_sort_(cmd, frame_slot);

    cmd.buffer_barrier(BufferAccess::ComputeWrite,
                       BufferAccess::VertexAttribute | BufferAccess::Indirect | BufferAccess::TransferRead);
    for (auto& a : active_) {
        cmd.copy_buffer(*a.st->lists, *a.st->readback[frame_slot], kHeaderBytes);
        a.st->readback_generation[frame_slot] = a.st->generation;
        a.st->readback_written[frame_slot] = true;
        if (a.st->debug_requested) {
            cmd.copy_buffer(*a.st->instances, *a.st->debug_instances,
                            sizeof(ParticleInstance) * static_cast<uint64_t>(a.st->capacity));
            cmd.copy_buffer(*a.st->lists, *a.st->debug_header, kHeaderBytes);
            a.st->debug_requested = false;
            a.st->debug_frame = frame_counter_;
        }
    }
    cmd.buffer_barrier(BufferAccess::TransferWrite, BufferAccess::HostRead);
}

bool GpuParticlePass::draw(coopa::gfx::command::CommandBuffer& cmd, uint64_t id) const {
    auto it = states_.find(id);
    if (it == states_.end() || it->second->needs_init) return false;
    cmd.bind_vertex_buffer(*it->second->instances, 0, 0);
    cmd.draw_indirect(*it->second->lists, 0);
    return true;
}

bool GpuParticlePass::alive(uint64_t id, uint32_t& count, uint32_t& generation) const {
    auto it = states_.find(id);
    if (it == states_.end() || !it->second->has_alive) return false;
    count = it->second->alive;
    generation = it->second->alive_generation;
    return true;
}

void GpuParticlePass::request_debug_readback(uint64_t id) {
    auto it = states_.find(id);
    if (it == states_.end()) return;
    State& st = *it->second;
    if (!st.debug_instances) {   // allocated on first use: tests only
        using coopa::gfx::BufferUsage;
        using coopa::gfx::MemoryResidency;
        st.debug_instances = storage_(sizeof(ParticleInstance) * static_cast<uint64_t>(st.capacity),
                                      BufferUsage::None, MemoryResidency::GpuToCpu);
        st.debug_header = storage_(kHeaderBytes, BufferUsage::None, MemoryResidency::GpuToCpu);
    }
    st.debug_requested = true;
}

bool GpuParticlePass::debug_instances(uint64_t id, std::vector<ParticleInstance>& out) const {
    auto it = states_.find(id);
    if (it == states_.end() || it->second->debug_frame == 0 ||
        frame_counter_ < it->second->debug_frame + kFrames) return false;
    uint32_t header[8] = {};
    it->second->debug_header->download(header, kHeaderBytes);
    const uint32_t n = std::min(header[1], it->second->capacity);
    out.resize(n);
    if (n) it->second->debug_instances->download(out.data(), sizeof(ParticleInstance) * static_cast<uint64_t>(n));
    return true;
}

uint32_t GpuParticlePass::pow2_at_least_(uint32_t n) {
    uint32_t p = 1024;
    while (p < n) p <<= 1;
    return p;
}

std::unique_ptr<coopa::gfx::memory::Buffer> GpuParticlePass::storage_(uint64_t bytes, coopa::gfx::BufferUsage extra,
                                                     coopa::gfx::MemoryResidency res) {
    return std::make_unique<coopa::gfx::memory::Buffer>(
        coopa::gfx::memory::make_storage_buffer(device_, allocator_, std::max<uint64_t>(bytes, 16), extra, res));
}

GpuParticlePass::State* GpuParticlePass::state_for_(const GpuParticleJob& job) {
    auto it = states_.find(job.id);
    const uint32_t want_sort = job.sort ? pow2_at_least_(job.capacity) : 0u;
    const void* tris_src = job.triangles ? job.triangles.get() : nullptr;
    if (it != states_.end()) {
        State& s = *it->second;
        if (s.capacity == job.capacity && s.sort_size == want_sort && s.triangles_src == tris_src) {
            if (s.generation != job.generation) {
                s.generation = job.generation;
                s.needs_init = true;
                s.has_alive = false;
            }
            return &s;
        }
        retired_.push_back({frame_counter_, std::move(it->second)});
        states_.erase(it);
    }
    if (states_.size() >= kMaxSystems) return nullptr;

    using coopa::gfx::BufferUsage;
    using coopa::gfx::MemoryResidency;
    auto st = std::make_unique<State>();
    st->id = job.id;
    st->generation = job.generation;
    st->capacity = job.capacity;
    st->sort_size = want_sort;
    st->triangles_src = tris_src;
    const uint64_t cap = job.capacity;
    st->particles = storage_(kParticleBytes * cap);
    st->lists = storage_(kHeaderBytes + 4 * cap, BufferUsage::Indirect);
    st->instances = storage_(sizeof(ParticleInstance) * cap, BufferUsage::Vertex);
    if (want_sort) {
        st->unsorted = storage_(sizeof(ParticleInstance) * cap);
        st->keys = storage_(8ull * want_sort);
    } else {
        st->keys = storage_(16);
    }
    if (job.triangles && !job.triangles->empty()) {
        const uint64_t bytes = sizeof(glm::vec4) * job.triangles->size();
        st->tris = storage_(bytes, BufferUsage::None, MemoryResidency::CpuToGpu);
        st->tris->upload(job.triangles->data(), bytes);
    } else {
        st->tris = storage_(16);
    }
    for (uint32_t f = 0; f < kFrames; ++f) {
        st->params[f] = storage_(sizeof(GpuParticleParams), BufferUsage::None, MemoryResidency::CpuToGpu);
        st->readback[f] = storage_(kHeaderBytes, BufferUsage::None, MemoryResidency::GpuToCpu);
        st->sets[f] = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device_, *pool_, *layout_);
        auto& set = *st->sets[f];
        set.bind_storage_buffer(0, *st->params[f]);
        set.bind_storage_buffer(1, *st->particles);
        set.bind_storage_buffer(2, *st->lists);
        set.bind_storage_buffer(3, *st->instances);
        set.bind_storage_buffer(4, st->unsorted ? *st->unsorted : *st->instances);
        set.bind_storage_buffer(5, *st->keys);
        set.bind_storage_buffer(6, *st->tris);
    }
    State* raw = st.get();
    states_.emplace(job.id, std::move(st));
    return raw;
}

void GpuParticlePass::bind_(coopa::gfx::command::CommandBuffer& cmd, const State& st, uint32_t slot, const Push& push) const {
    cmd.bind_descriptor_set(*st.sets[slot], 0);
    cmd.push_constants(coopa::gfx::ShaderStage::Compute, push);
}

void GpuParticlePass::record_sort_(coopa::gfx::command::CommandBuffer& cmd, uint32_t slot) {
    uint32_t max_size = 0;
    for (auto& a : active_) max_size = std::max(max_size, a.st->sort_size);
    if (max_size == 0) return;
    using coopa::gfx::pipeline::ComputePipeline;
    cmd.bind_pipeline(*sort_);
    // Every stage k <= 1024, in shared memory.
    for (auto& a : active_) {
        if (!a.st->sort_size) continue;
        bind_(cmd, *a.st, slot, Push{0, 0, 0, 0});
        cmd.dispatch(a.st->sort_size / 1024);
    }
    cmd.compute_to_compute_barrier();
    for (uint32_t k = 2048; k <= max_size; k <<= 1) {
        for (uint32_t j = k >> 1; j >= 1024; j >>= 1) {
            for (auto& a : active_) {
                if (a.st->sort_size < k) continue;
                bind_(cmd, *a.st, slot, Push{1, k, j, 0});
                cmd.dispatch(a.st->sort_size / 2 / 512);
            }
            cmd.compute_to_compute_barrier();
        }
        for (auto& a : active_) {
            if (a.st->sort_size < k) continue;
            bind_(cmd, *a.st, slot, Push{2, k, 0, 0});
            cmd.dispatch(a.st->sort_size / 1024);
        }
        cmd.compute_to_compute_barrier();
    }
    cmd.bind_pipeline(*gather_);
    for (auto& a : active_) {
        if (!a.st->sort_size) continue;
        bind_(cmd, *a.st, slot, Push{});
        cmd.dispatch(ComputePipeline::groups_for(a.st->capacity, 64));
    }
    cmd.compute_to_compute_barrier();
}

void GpuParticlePass::read_back_(uint32_t slot) {
    for (auto& [id, st] : states_) {
        if (!st->readback_written[slot]) continue;
        if (st->readback_generation[slot] != st->generation) continue;
        uint32_t header[8] = {};
        st->readback[slot]->download(header, kHeaderBytes);
        st->alive = std::min(header[1], st->capacity);
        st->alive_generation = st->generation;
        st->has_alive = true;
    }
}

void GpuParticlePass::retire_unused_() {
    for (auto it = states_.begin(); it != states_.end();) {
        if (frame_counter_ > it->second->last_used + 8) {
            retired_.push_back({frame_counter_, std::move(it->second)});
            it = states_.erase(it);
        } else {
            ++it;
        }
    }
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                  [&](const auto& r) { return frame_counter_ > r.first + kFrames + 1; }),
                   retired_.end());
}

} // namespace passes
} // namespace render
} // namespace toy
