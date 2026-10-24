#include <toyengine/scene/skinned_mesh_renderer.h>

#include <coopa/animation/animator.h>
#include <coopa/asset/asset_manager.h>
#include <coopa/scene/component.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/storage_buffer.h>
#include <gfxcoopa/pipeline/descriptor.h>

#include <glm/gtc/quaternion.hpp>

namespace toy {
namespace scene {

namespace {

/** @brief The quaternion packing compute_dq_palette() documents: (x, y, z, w) in a vec4. */
glm::vec4 pack_quat(const glm::quat& q) { return glm::vec4(q.x, q.y, q.z, q.w); }

/** @brief Rotates `v` by the unit quaternion `q` in (x, y, z, w) order -- skin.comp's `rotate_by`. */
glm::vec3 rotate_by(const glm::vec4& q, const glm::vec3& v) {
    const glm::vec3 r(q);
    return v + 2.0f * glm::cross(r, glm::cross(r, v) + q.w * v);
}

} // namespace

void SkinnedMeshRenderer::start() {
    if (!owner) return;
    renderer_ = owner->get_component<MeshRenderer>();
    rig_ = find_rig_();
    rest_world_.clear();
    if (auto* tc = owner->get_transform()) owner_rest_world_ = tc->get_world_matrix();
    if (rig_) snapshot_rest_(rig_);
}

void SkinnedMeshRenderer::compute_dq_palette(const std::vector<glm::mat4>& skin_matrices, std::vector<glm::mat4>& out) {
    out.assign(skin_matrices.size(), glm::mat4(0.0f));
    for (size_t i = 0; i < skin_matrices.size(); ++i) {
        const glm::mat4& m = skin_matrices[i];
        glm::vec3 scale(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
        glm::mat3 rot(glm::vec3(m[0]) / (scale.x > 1e-8f ? scale.x : 1.0f),
                      glm::vec3(m[1]) / (scale.y > 1e-8f ? scale.y : 1.0f),
                      glm::vec3(m[2]) / (scale.z > 1e-8f ? scale.z : 1.0f));
        // A mirrored bone: keep the reflection in the scale, where it cannot turn into a
        // 180-degree rotation the blend would then interpolate THROUGH.
        if (glm::determinant(rot) < 0.0f) { rot[0] = -rot[0]; scale.x = -scale.x; }
        const glm::quat q = glm::normalize(glm::quat_cast(rot));
        const glm::vec3 t(m[3]);
        const glm::quat dual = 0.5f * (glm::quat(0.0f, t.x, t.y, t.z) * q);
        out[i][0] = pack_quat(q);
        out[i][1] = pack_quat(dual);
        out[i][2] = glm::vec4(scale, 0.0f);
        out[i][3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    }
}

void SkinnedMeshRenderer::skin(const SkinnedMeshSource& src, const std::vector<glm::mat4>& skin_matrices,
                               std::vector<Vertex>& out, bool preserve_volume) {
    std::vector<glm::mat4> dq;
    if (preserve_volume) compute_dq_palette(skin_matrices, dq);
    out.resize(src.vertices.size());
    for (size_t i = 0; i < src.vertices.size(); ++i) {
        const glm::ivec4& j = (i < src.joints.size())  ? src.joints[i]  : glm::ivec4(-1);
        const glm::vec4&  w = (i < src.weights.size()) ? src.weights[i] : glm::vec4(0.0f);
        if (preserve_volume) {
            // The dual-quaternion blend skin.comp's mode 1 runs, step for step: the real parts
            // are brought into one hemisphere against the first influence (a quaternion and its
            // negation are the same rotation, but averaging them cancels), summed, and the
            // normalisation of the real part divides the dual part too.
            glm::vec4 qr(0.0f), qd(0.0f), pivot(0.0f);
            glm::vec3 scale(0.0f);
            float weight_sum = 0.0f;
            for (int c = 0; c < 4; ++c) {
                if (j[c] < 0 || w[c] <= 0.0f || static_cast<size_t>(j[c]) >= dq.size()) continue;
                const glm::mat4& p = dq[static_cast<size_t>(j[c])];
                if (weight_sum == 0.0f) pivot = p[0];
                const float sign = (glm::dot(glm::vec4(p[0]), pivot) < 0.0f) ? -1.0f : 1.0f;
                qr += w[c] * sign * glm::vec4(p[0]);
                qd += w[c] * sign * glm::vec4(p[1]);
                scale += w[c] * glm::vec3(p[2]);
                weight_sum += w[c];
            }
            out[i] = src.vertices[i];
            const float qr_len = glm::length(qr);
            if (weight_sum > 1e-6f && qr_len > 1e-8f) {
                qr /= qr_len;
                qd /= qr_len;
                const glm::vec3 p = src.vertices[i].position * (scale / weight_sum);
                out[i].position = rotate_by(qr, p) +
                                  2.0f * (qr.w * glm::vec3(qd) - qd.w * glm::vec3(qr) +
                                          glm::cross(glm::vec3(qr), glm::vec3(qd)));
                // Rigid/uniformly-scaled bones only, exactly as the matrix path assumes.
                out[i].normal = glm::normalize(rotate_by(qr, src.vertices[i].normal));
                const glm::vec3 t = glm::normalize(rotate_by(qr, glm::vec3(src.vertices[i].tangent)));
                out[i].tangent = glm::vec4(t, src.vertices[i].tangent.w);
            }
            continue;
        }
        glm::vec3 pos(0.0f), nrm(0.0f), tan(0.0f);
        float weight_sum = 0.0f;
        for (int c = 0; c < 4; ++c) {
            if (j[c] < 0 || w[c] <= 0.0f || static_cast<size_t>(j[c]) >= skin_matrices.size()) continue;
            const glm::mat4& m = skin_matrices[static_cast<size_t>(j[c])];
            pos += w[c] * glm::vec3(m * glm::vec4(src.vertices[i].position, 1.0f));
            // Rigid/uniformly-scaled bones only (the common case for a character rig) --
            // no inverse-transpose here, matching the same simplification most small
            // engines make for skinning normals.
            nrm += w[c] * glm::vec3(m * glm::vec4(src.vertices[i].normal, 0.0f));
            tan += w[c] * glm::vec3(m * glm::vec4(glm::vec3(src.vertices[i].tangent), 0.0f));
            weight_sum += w[c];
        }
        if (weight_sum > 1e-6f) {
            out[i] = src.vertices[i];
            out[i].position = pos / weight_sum;
            const float n_len = glm::length(nrm);
            out[i].normal = (n_len > 1e-6f) ? nrm / n_len : src.vertices[i].normal;
            const float t_len = glm::length(tan);
            const glm::vec3 t = (t_len > 1e-6f) ? tan / t_len : glm::vec3(src.vertices[i].tangent);
            out[i].tangent = glm::vec4(t, src.vertices[i].tangent.w);
        } else {
            // No (or zero-weight) influences on this vertex -- leave it at the bind pose
            // rather than collapsing it to the origin.
            out[i] = src.vertices[i];
        }
    }
}

void SkinnedMeshRenderer::compute_palette(const glm::mat4& owner_world, const std::vector<glm::mat4>& bone_worlds,
                            const std::vector<uint8_t>& bone_valid, const std::vector<glm::mat4>& inverse_bind,
                            std::vector<glm::mat4>& out) {
    const glm::mat4 inv_owner = glm::inverse(owner_world);
    out.assign(bone_worlds.size(), glm::mat4(1.0f));
    for (size_t i = 0; i < bone_worlds.size(); ++i) {
        if (i < bone_valid.size() && !bone_valid[i]) continue;
        out[i] = inv_owner * bone_worlds[i] * (i < inverse_bind.size() ? inverse_bind[i] : glm::mat4(1.0f));
    }
}

void SkinnedMeshRenderer::palette_bounds(const std::vector<glm::vec3>& bone_min, const std::vector<glm::vec3>& bone_max,
                           const glm::vec3& static_min, const glm::vec3& static_max,
                           const std::vector<glm::mat4>& skin_matrices, glm::vec3& out_min, glm::vec3& out_max,
                           bool preserve_volume) {
    out_min = static_min;
    out_max = static_max;
    for (size_t b = 0; b < bone_min.size() && b < skin_matrices.size(); ++b) {
        if (bone_min[b].x > bone_max[b].x) continue;   // influences no vertex
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 corner((c & 1) ? bone_max[b].x : bone_min[b].x,
                                   (c & 2) ? bone_max[b].y : bone_min[b].y,
                                   (c & 4) ? bone_max[b].z : bone_min[b].z);
            const glm::vec3 p = glm::vec3(skin_matrices[b] * glm::vec4(corner, 1.0f));
            out_min = glm::min(out_min, p);
            out_max = glm::max(out_max, p);
        }
    }
    // A dual-quaternion blend bows outside the union -- see k_dq_bounds_margin.
    if (preserve_volume && out_min.x <= out_max.x) {
        const float margin = glm::length(out_max - out_min) * k_dq_bounds_margin;
        out_min -= glm::vec3(margin);
        out_max += glm::vec3(margin);
    }
}

void SkinnedMeshRenderer::upload(uint32_t frame_slot, toy::render::passes::SkinningPass* gpu) {
    if (!mesh_ && !ensure_mesh_(gpu)) return;
    update_palette_();
    if (gpu_) {
        glm::vec3 lo, hi;
        palette_bounds(bone_min_, bone_max_, static_min_, static_max_, skin_matrices_, lo, hi, preserve_volume_);
        if (preserve_volume_) compute_dq_palette(skin_matrices_, dq_palette_);
        // Either palette is one mat4 per bone, so the buffer and the descriptor set are the
        // same either way; the push constant tells skin.comp how to read it.
        const std::vector<glm::mat4>& palette = preserve_volume_ ? dq_palette_ : skin_matrices_;
        toy::render::passes::SkinningPass::Job job;
        job.set          = &gpu_->sets[frame_slot % gpu_->sets.size()];
        job.palette      = &gpu_->palette;
        job.matrices     = palette.empty() ? nullptr : palette.data();
        job.bone_count   = static_cast<uint32_t>(palette.size());
        job.vertex_count = static_cast<uint32_t>(source_->vertices.size());
        job.frame_slot   = frame_slot;
        job.preserve_volume = preserve_volume_;
        gpu->enqueue(job);
        mesh_->mark_gpu_written(frame_slot, lo, hi);
        cpu_vertices_dirty_ = true;
    } else {
        skin(*source_, skin_matrices_, vertices_, preserve_volume_);
        cpu_vertices_dirty_ = false;
        mesh_->update_vertices(vertices_.data(), vertices_.size(), frame_slot);
    }
}

void SkinnedMeshRenderer::rebuild() {
    if (gpu_) device_.wait_idle();   // an in-flight dispatch may still read its buffers
    gpu_.reset();
    mesh_.reset();
    vertices_.clear();
}

const std::vector<SkinnedMeshRenderer::Vertex>& SkinnedMeshRenderer::skinned_vertices() const {
    if (cpu_vertices_dirty_ && source_.is_loaded()) {
        skin(*source_, skin_matrices_, vertices_, preserve_volume_);
        cpu_vertices_dirty_ = false;
    }
    return vertices_;
}

bool SkinnedMeshRenderer::ensure_mesh_(toy::render::passes::SkinningPass* gpu) {
    if (!source_.is_loaded() || !renderer_) return false;
    const SkinnedMeshSource& src = *source_;
    if (src.vertices.empty() || src.indices.empty()) return false;
    resolve_bones_(src);
    build_bone_boxes_(src);

    vertices_ = src.vertices; // bind pose; upload() re-skins
    mesh_ = std::make_shared<Mesh>(
        Mesh::from_arrays(device_, allocator_, vertices_, src.indices, frames_in_flight_,
                          /*compute_writable=*/gpu != nullptr));
    if (gpu) build_gpu_(*gpu, src);

    // Synthetic id, prefixed out of the real-path namespace -- see
    // AssetManager::create()'s doc and ClothRenderer::ensure_mesh_()'s identical use.
    renderer_->set_mesh(assets_.create<Mesh>("runtime/skinned/" + owner->name(), mesh_));
    return true;
}

void SkinnedMeshRenderer::update_palette_() {
    auto* tc = owner ? owner->get_transform() : nullptr;
    const glm::mat4 owner_world = tc ? tc->get_world_matrix() : glm::mat4(1.0f);
    bone_worlds_.assign(bones_.size(), glm::mat4(1.0f));
    bone_valid_.assign(bones_.size(), 0);
    for (size_t i = 0; i < bones_.size(); ++i) {
        if (!bones_[i]) continue; // unresolved bone -- stays identity, see resolve_bones_()
        auto* bone_tc = bones_[i]->get_transform();
        if (!bone_tc) continue;
        bone_worlds_[i] = bone_tc->get_world_matrix();
        bone_valid_[i] = 1;
    }
    compute_palette(owner_world, bone_worlds_, bone_valid_, inverse_bind_, skin_matrices_);
}

void SkinnedMeshRenderer::build_bone_boxes_(const SkinnedMeshSource& src) {
    const glm::vec3 empty_min(std::numeric_limits<float>::max()), empty_max(std::numeric_limits<float>::lowest());
    bone_min_.assign(bones_.size(), empty_min);
    bone_max_.assign(bones_.size(), empty_max);
    static_min_ = empty_min;
    static_max_ = empty_max;
    for (size_t i = 0; i < src.vertices.size(); ++i) {
        const glm::ivec4 j = (i < src.joints.size())  ? src.joints[i]  : glm::ivec4(-1);
        const glm::vec4  w = (i < src.weights.size()) ? src.weights[i] : glm::vec4(0.0f);
        const glm::vec3& p = src.vertices[i].position;
        bool any = false;
        for (int c = 0; c < 4; ++c) {
            if (j[c] < 0 || w[c] <= 0.0f || static_cast<size_t>(j[c]) >= bones_.size()) continue;
            bone_min_[j[c]] = glm::min(bone_min_[j[c]], p);
            bone_max_[j[c]] = glm::max(bone_max_[j[c]], p);
            any = true;
        }
        if (!any) { static_min_ = glm::min(static_min_, p); static_max_ = glm::max(static_max_, p); }
    }
}

void SkinnedMeshRenderer::build_gpu_(toy::render::passes::SkinningPass& pass, const SkinnedMeshSource& src) {
    using namespace coopa::gfx;
    using BindVertex = toy::render::passes::SkinningPass::BindVertex;
    std::vector<BindVertex> bind(src.vertices.size());
    for (size_t i = 0; i < src.vertices.size(); ++i) {
        const Vertex& v = src.vertices[i];
        bind[i].pos_u   = glm::vec4(v.position, v.uv.x);
        bind[i].nrm_v   = glm::vec4(v.normal, v.uv.y);
        bind[i].tangent = v.tangent;
        bind[i].joints  = (i < src.joints.size())  ? src.joints[i]  : glm::ivec4(-1);
        bind[i].weights = (i < src.weights.size()) ? src.weights[i] : glm::vec4(0.0f);
    }
    const uint64_t bind_bytes = sizeof(BindVertex) * bind.size();
    const uint64_t pal_bytes  = sizeof(glm::mat4) * std::max<size_t>(bones_.size(), 1);
    auto g = std::make_unique<GpuSkin>(
        memory::make_storage_buffer(device_, allocator_, bind_bytes, BufferUsage::None, MemoryResidency::CpuToGpu),
        memory::StorageBufferRing(device_, allocator_, pal_bytes, frames_in_flight_, BufferUsage::None,
                                  MemoryResidency::CpuToGpu),
        pipeline::DescriptorPoolBuilder().add_sets(pass.layout(), frames_in_flight_).build(device_));
    g->bind_pose.upload(bind.data(), bind_bytes);
    const std::vector<glm::mat4> identity(std::max<size_t>(bones_.size(), 1), glm::mat4(1.0f));
    for (uint32_t s = 0; s < frames_in_flight_; ++s) {
        g->palette.upload(s, identity.data(), pal_bytes);
        g->sets.emplace_back(device_, g->pool, pass.layout());
        g->sets.back().bind_storage_buffer(0, g->bind_pose);
        g->sets.back().bind_storage_buffer(1, g->palette.current(s));
        g->sets.back().bind_storage_buffer(2, mesh_->vertex_buffer(s));
    }
    gpu_ = std::move(g);
}

coopa::scene::SceneObject* SkinnedMeshRenderer::find_rig_() const {
    if (!rig_path_.empty() && scene) {
        if (auto* r = scene->find_object_by_path(rig_path_)) return r;
    }
    for (coopa::scene::SceneObject* o = owner; o; o = o->parent()) {
        if (o->get_component<coopa::anim::Animator>()) return o;
    }
    coopa::scene::SceneObject* top = owner;
    while (top && top->parent()) top = top->parent();
    return top;
}

void SkinnedMeshRenderer::snapshot_rest_(coopa::scene::SceneObject* o) {
    if (auto* tc = o->get_transform()) rest_world_[o] = tc->get_world_matrix();
    for (const auto& c : o->children()) snapshot_rest_(c.get());
}

coopa::scene::SceneObject* SkinnedMeshRenderer::find_bone_(const std::string& path) const {
    if (rig_) {
        if (path == rig_->name()) return rig_;
        coopa::scene::SceneObject* cur = rig_;
        size_t start = 0;
        bool first = true;
        while (cur && start <= path.size()) {
            const size_t slash = path.find('/', start);
            const std::string seg = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            cur = first ? cur->find_descendant(seg) : cur->find_child(seg);
            first = false;
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        if (cur) return cur;
    }
    return scene ? scene->find_object_by_path(path) : nullptr;   // the scene-wide form
}

void SkinnedMeshRenderer::resolve_bones_(const SkinnedMeshSource& src) {
    const std::vector<std::string>& names = !bone_paths_.empty() ? bone_paths_ : src.groups;
    bones_.clear();
    inverse_bind_.clear();
    for (size_t i = 0; i < names.size(); ++i) {
        coopa::scene::SceneObject* b = find_bone_(names[i]);
        bones_.push_back(b);
        glm::mat4 inv_bind(1.0f);
        if (i < src.inverse_bind_matrices.size()) {
            inv_bind = src.inverse_bind_matrices[i];
        } else if (b) {
            // Rest pose: identity skinning at the authored pose. bone_world_rest^-1 * owner_world_rest.
            auto it = rest_world_.find(b);
            const glm::mat4 bone_rest = it != rest_world_.end() ? it->second
                                      : (b->get_transform() ? b->get_transform()->get_world_matrix() : glm::mat4(1.0f));
            inv_bind = glm::inverse(bone_rest) * owner_rest_world_;
        }
        inverse_bind_.push_back(inv_bind);
    }
}

} // namespace scene
} // namespace toy
