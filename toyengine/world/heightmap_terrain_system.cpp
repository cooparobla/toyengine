#include <toyengine/world/heightmap_terrain_system.h>
#include <toyengine/world/terrain_water_mesh.h>
#include <toyengine/water/water_body.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>

#include <stb/stb_image.h>

#include <coopa/asset/asset_id.h>
#include <coopa/asset/asset_manager.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/scene_system.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/components/camera_component.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/memory/allocator.h>
#include <physxcoopa/components/heightfield_collider.h>
#include <physxcoopa/dynamics/physics_material.h>
#include <physxcoopa/system/physics_system.h>

#include <toyengine/render/terrain_gpu.h>
#include <toyengine/scene/runtime_object.h>

namespace toy {
namespace world {

using coopa::gfx::engine::components::MeshRenderer;
using coopa::gfx::engine::data::Mesh;
using coopa::gfx::engine::data::MeshCpuData;
using coopa::gfx::engine::data::Texture;
using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;

namespace {

/** @brief Decodes an image and resizes it (bilinear) to size x size RGBA8; empty on failure. */
std::vector<std::uint8_t> load_layer_image(const std::string& path, int size) {
    if (path.empty()) return {};
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (px == nullptr) {
        std::cerr << "[HeightmapTerrain] layer texture '" << path << "' could not be read\n";
        return {};
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y) {
        const float v = std::clamp((static_cast<float>(y) + 0.5f) * static_cast<float>(h) / static_cast<float>(size) - 0.5f,
                                   0.0f, static_cast<float>(h - 1));
        const int y0 = static_cast<int>(v), y1 = std::min(y0 + 1, h - 1);
        const float fy = v - static_cast<float>(y0);
        for (int x = 0; x < size; ++x) {
            const float u = std::clamp((static_cast<float>(x) + 0.5f) * static_cast<float>(w) / static_cast<float>(size) - 0.5f,
                                       0.0f, static_cast<float>(w - 1));
            const int x0 = static_cast<int>(u), x1 = std::min(x0 + 1, w - 1);
            const float fx = u - static_cast<float>(x0);
            for (int c = 0; c < 4; ++c) {
                auto s = [&](int xx, int yy) { return static_cast<float>(px[(static_cast<std::size_t>(yy) * w + xx) * 4 + c]); };
                const float a = s(x0, y0) + (s(x1, y0) - s(x0, y0)) * fx;
                const float b = s(x0, y1) + (s(x1, y1) - s(x0, y1)) * fx;
                out[(static_cast<std::size_t>(y) * size + x) * 4 + c] =
                    static_cast<std::uint8_t>(std::clamp(a + (b - a) * fy + 0.5f, 0.0f, 255.0f));
            }
        }
    }
    stbi_image_free(px);
    return out;
}

} // namespace

HeightmapTerrainSystem::HeightmapTerrainSystem(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                                               coopa::gfx::command::CommandPool& cmd_pool, coopa::asset::AssetManager& assets)
    : device_(device), allocator_(allocator), cmd_pool_(cmd_pool), assets_(assets) {}

HeightmapTerrainSystem::~HeightmapTerrainSystem() {
    for (auto& [key, gpu] : gpu_cache_) {
        if (!gpu->height_id.empty()) assets_.unload(coopa::asset::AssetId::from_path(gpu->height_id));
    }
}

MeshCpuData HeightmapTerrainSystem::build_patch(int resolution) {
    const int r = std::max(1, resolution);
    MeshCpuData out;
    out.vertices.reserve(static_cast<std::size_t>(r + 1) * (r + 1));
    for (int j = 0; j <= r; ++j) {
        for (int i = 0; i <= r; ++i) {
            coopa::gfx::engine::data::Vertex v{};
            v.position = glm::vec3(static_cast<float>(i) / static_cast<float>(r), static_cast<float>(j) / static_cast<float>(r), 0.0f);
            v.normal = glm::vec3(0.0f, 0.0f, 1.0f);
            v.uv = glm::vec2(v.position);
            v.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            out.vertices.push_back(v);
        }
    }
    auto idx = [r](int i, int j) { return static_cast<std::uint32_t>(j * (r + 1) + i); };
    for (int j = 0; j < r; ++j) {
        for (int i = 0; i < r; ++i) {
            // Counter-clockwise seen from above (+Z), like every upward-facing mesh.
            out.indices.insert(out.indices.end(), {idx(i, j), idx(i + 1, j), idx(i + 1, j + 1)});
            out.indices.insert(out.indices.end(), {idx(i, j), idx(i + 1, j + 1), idx(i, j + 1)});
        }
    }
    // The patch is flat, but the shader lifts it anywhere in the chunk's height range, which the
    // chunk's transform maps onto object z 0..1: the box must span it or the chunk is culled.
    out.bounds_min = glm::vec3(0.0f);
    out.bounds_max = glm::vec3(1.0f);
    out.lods.push_back({0u, static_cast<std::uint32_t>(out.indices.size()), 0, 0.0f, {}});
    return out;
}

coopa::asset::AssetHandle<Mesh> HeightmapTerrainSystem::patch_mesh(int resolution) {
    auto it = patches_.find(resolution);
    if (it != patches_.end()) return it->second;
    auto mesh = std::make_shared<Mesh>(Mesh::from_cpu(device_, allocator_, build_patch(resolution)));
    const std::string id = "runtime/heightmap/patch_" + std::to_string(resolution) + "_" +
                           std::to_string(reinterpret_cast<std::uintptr_t>(this));
    auto handle = assets_.create<Mesh>(id, mesh);
    patches_.emplace(resolution, handle);
    return handle;
}

void HeightmapTerrainSystem::chunk_box(const HeightmapData& data, int cx, int cy, glm::vec3& position, glm::vec3& scale) {
    const float cs = data.geometry().chunk_size;
    const glm::vec2 mm = data.chunk_minmax(cx, cy);
    const float lo = mm.x - k_heightmap_chunk_pad;
    const float hi = mm.y + k_heightmap_chunk_pad;
    position = glm::vec3(static_cast<float>(cx) * cs, static_cast<float>(cy) * cs, lo);
    scale = glm::vec3(cs, cs, std::max(hi - lo, 0.01f));
}

render::TerrainGpuParams HeightmapTerrainSystem::gpu_params(const HeightmapTerrain& t, const HeightmapTerrainRuntime& rt) {
    render::TerrainGpuParams p;
    const HeightmapData& d = *t.data;
    const HeightmapGeometry& g = d.geometry();
    p.origin = glm::vec4(t.origin(), g.chunk_size);
    p.cell = glm::vec4(d.cell(), g.size);
    int flags = 0;
    if (t.deep_snow) flags |= render::kTerrainDeepSnow;
    if (t.puddles) flags |= render::kTerrainPuddles;
    const int count = std::min<int>(static_cast<int>(t.layers.size()), render::kSurfaceTerrainLayers);
    p.dims = glm::ivec4(d.nx(), d.ny(), std::max(1, count), flags);
    p.misc = glm::vec4(g.height, static_cast<float>(g.splat_resolution), 0.0f, 0.0f);
    for (int i = 0; i < count; ++i) {
        const HeightmapLayer& l = t.layers[static_cast<std::size_t>(i)];
        p.layer[i] = glm::vec4(1.0f / std::max(l.tiling, 0.01f), l.roughness, l.metallic, l.normal_strength);
        const bool has_normal = rt.normal != nullptr && i < static_cast<int>(t.layer_normal_paths.size()) &&
                                !t.layer_normal_paths[static_cast<std::size_t>(i)].empty();
        p.layer_tri[i] = glm::vec4(l.triplanar ? 1.0f : 0.0f, l.triplanar_sharpness, has_normal ? 1.0f : 0.0f, 0.0f);
        p.tint[i] = glm::vec4(l.tint, 1.0f);
    }
    return p;
}

void HeightmapTerrainSystem::execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) {
    ++frame_;
    time_ += static_cast<double>(ctx.delta_time);
    bindings_.clear();
    // One component type serves both modes, so the lookup returns every terrain in the scene:
    // this system owns the heightmap ones, TerrainSystem (order 60) owns the tile ones.
    for (HeightmapTerrain* terrain : scene.get_components<HeightmapTerrain>()) {
        if (terrain->mode != TerrainMode::Heightmap) continue;
        if (terrain->data == nullptr || terrain->owner == nullptr) continue;
        terrain->sync_field();
        tick_(*terrain, scene, ctx);
    }
    evict_();
}

void HeightmapTerrainSystem::tick_(HeightmapTerrain& terrain, coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) {
    if (!terrain.runtime) terrain.runtime = std::make_unique<HeightmapTerrainRuntime>();
    HeightmapTerrainRuntime& rt = *terrain.runtime;
    const HeightmapData& data = *terrain.data;

    // 1. The grid and splats on the GPU, patched from the edit journal.
    std::shared_ptr<HeightmapGpu> gpu = gpu_for_(data);
    upload_(*gpu, data);
    gpu->last_used_frame = frame_;
    rt.gpu = gpu;

    // 2. Layer arrays.
    layers_for_(terrain, rt);

    // 3. Slot: the terrain table holds kMaxSurfaceTerrains; later terrains don't draw.
    const int slot = static_cast<int>(bindings_.size());
    if (slot >= render::kMaxSurfaceTerrains) {
        if (!warned_slots_) {
            std::cerr << "[HeightmapTerrain] more than " << render::kMaxSurfaceTerrains
                      << " heightmap terrains in one scene; the rest are not drawn\n";
            warned_slots_ = true;
        }
        destroy_chunks_(terrain, rt);
        return;
    }
    rt.slot = slot;

    // 4. Chunks: reset when the shape or patch changed, streamed by distance, materials rewritten
    //    when what they carry changed, boxes refitted where the grid was edited.
    const ChunkSettings cs = resolve_settings(terrain, camera_far_(),
                                              device_.supports_tessellation() ? static_cast<float>(device_.max_tessellation_level()) : 0.0f);
    if (rt.chunks.empty() || rt.built_geometry != data.geometry() || rt.built_patch != cs.patch) {
        destroy_chunks_(terrain, rt);
        rt.built_geometry = data.geometry();
        rt.built_patch = cs.patch;
        rt.chunk_count = data.geometry().chunk_count();
        rt.chunks.assign(static_cast<std::size_t>(rt.chunk_count.x) * rt.chunk_count.y, nullptr);
        rt.live_chunks = 0;
        rt.bounds_revision = data.revision();
    }
    write_materials_(terrain, rt, cs);
    stream_chunks_(terrain, rt, scene, cs, camera_position_(terrain));
    fit_chunk_boxes_(terrain, rt);
    // After streaming, so a chunk that arrived this frame already has its water and is not
    // rebuilt twice.
    refresh_water_(terrain, rt);

    // 5. Collider.
    update_collider_(terrain, rt, scene, ctx);

    // 6. The renderer's view of it.
    render::TerrainFrameBinding b;
    b.height = gpu->height.get();
    b.splat0 = gpu->splat0;
    b.splat1 = gpu->splat1;
    b.albedo = rt.albedo;
    b.normal = rt.normal;
    b.params = gpu_params(terrain, rt);
    bindings_.push_back(std::move(b));
}

std::shared_ptr<HeightmapGpu> HeightmapTerrainSystem::gpu_for_(const HeightmapData& data) {
    auto& slot = gpu_cache_[data.uid()];
    const bool shape_ok = slot && slot->nx == data.nx() && slot->ny == data.ny() && slot->splat_size == data.splat_w();
    if (shape_ok) return slot;

    // (Re)create at the data's current shape. The old textures stay alive through their
    // shared_ptrs (the renderer's registry holds them while its sets do) and the height asset's
    // own unload grace period.
    if (slot && !slot->height_id.empty()) assets_.unload(coopa::asset::AssetId::from_path(slot->height_id));
    auto gpu = std::make_shared<HeightmapGpu>();
    gpu->nx = data.nx();
    gpu->ny = data.ny();
    gpu->splat_size = data.splat_w();
    gpu->height = std::make_shared<Texture>(Texture::upload(
        device_, allocator_, cmd_pool_, reinterpret_cast<const std::uint8_t*>(data.heights().data()),
        static_cast<std::uint32_t>(data.nx()), static_cast<std::uint32_t>(data.ny()),
        coopa::gfx::Format::R32_Sfloat, coopa::gfx::SamplerDesc::pixel_art()));
    gpu->height_id = "runtime/heightmap/height_" + std::to_string(next_id_++) + "_" +
                     std::to_string(reinterpret_cast<std::uintptr_t>(this));
    gpu->height_handle = assets_.create<Texture>(gpu->height_id, gpu->height);
    const auto splat_sampler = coopa::gfx::SamplerDesc::pixel_art_smooth();
    gpu->splat0 = std::make_shared<Texture>(Texture::upload(
        device_, allocator_, cmd_pool_, data.splat(0).data(), static_cast<std::uint32_t>(data.splat_w()),
        static_cast<std::uint32_t>(data.splat_h()), coopa::gfx::Format::RGBA8_Unorm, splat_sampler));
    gpu->splat1 = std::make_shared<Texture>(Texture::upload(
        device_, allocator_, cmd_pool_, data.splat(1).data(), static_cast<std::uint32_t>(data.splat_w()),
        static_cast<std::uint32_t>(data.splat_h()), coopa::gfx::Format::RGBA8_Unorm, splat_sampler));
    gpu->uploaded_revision = data.revision();
    slot = gpu;
    return gpu;
}

void HeightmapTerrainSystem::upload_(HeightmapGpu& gpu, const HeightmapData& data) {
    if (gpu.uploaded_revision == data.revision()) return;
    const IRect hr = data.height_dirty_since(gpu.uploaded_revision);
    if (!hr.empty()) {
        std::vector<float> region(static_cast<std::size_t>(hr.width()) * hr.height());
        for (int j = 0; j < hr.height(); ++j) {
            std::copy_n(data.heights().begin() + static_cast<std::ptrdiff_t>((hr.y0 + j) * data.nx() + hr.x0),
                        hr.width(), region.begin() + static_cast<std::ptrdiff_t>(j) * hr.width());
        }
        gpu.height->update(device_, allocator_, cmd_pool_, region.data(), static_cast<std::uint32_t>(hr.x0),
                           static_cast<std::uint32_t>(hr.y0), static_cast<std::uint32_t>(hr.width()),
                           static_cast<std::uint32_t>(hr.height()));
    }
    const IRect sr = data.splat_dirty_since(gpu.uploaded_revision);
    if (!sr.empty()) {
        for (int k = 0; k < 2; ++k) {
            std::vector<std::uint8_t> region(static_cast<std::size_t>(sr.width()) * sr.height() * 4);
            for (int j = 0; j < sr.height(); ++j) {
                std::copy_n(data.splat(k).begin() + static_cast<std::ptrdiff_t>(((sr.y0 + j) * data.splat_w() + sr.x0) * 4),
                            sr.width() * 4, region.begin() + static_cast<std::ptrdiff_t>(j) * sr.width() * 4);
            }
            (k == 0 ? gpu.splat0 : gpu.splat1)->update(device_, allocator_, cmd_pool_, region.data(),
                                                       static_cast<std::uint32_t>(sr.x0), static_cast<std::uint32_t>(sr.y0),
                                                       static_cast<std::uint32_t>(sr.width()),
                                                       static_cast<std::uint32_t>(sr.height()));
        }
    }
    gpu.uploaded_revision = data.revision();
}

void HeightmapTerrainSystem::layers_for_(HeightmapTerrain& terrain, HeightmapTerrainRuntime& rt) {
    const int size = terrain.layer_texture_size;
    const int count = std::min<int>(static_cast<int>(terrain.layers.size()), render::kSurfaceTerrainLayers);
    auto make = [&](const std::vector<std::string>& paths, bool srgb) -> std::shared_ptr<render::TextureArray> {
        bool any = false;
        std::ostringstream key;
        key << (srgb ? "albedo|" : "normal|") << size << "|" << count;
        for (int i = 0; i < count; ++i) {
            const std::string& p = i < static_cast<int>(paths.size()) ? paths[static_cast<std::size_t>(i)] : std::string();
            any |= !p.empty();
            key << "|" << p;
        }
        if (!any) return nullptr;
        auto& entry = array_cache_[key.str()];
        entry.last_used_frame = frame_;
        if (entry.array) return entry.array;
        std::vector<std::vector<std::uint8_t>> images;
        for (int i = 0; i < count; ++i) {
            std::vector<std::uint8_t> img =
                load_layer_image(i < static_cast<int>(paths.size()) ? paths[static_cast<std::size_t>(i)] : std::string(), size);
            if (img.empty()) {
                // A layer without this map: white albedo (its tint shows) / a flat normal.
                img.resize(static_cast<std::size_t>(size) * size * 4);
                for (std::size_t k = 0; k < img.size(); k += 4) {
                    img[k] = srgb ? 255 : 128;
                    img[k + 1] = srgb ? 255 : 128;
                    img[k + 2] = 255;
                    img[k + 3] = 255;
                }
            }
            images.push_back(std::move(img));
        }
        entry.array = std::make_shared<render::TextureArray>(device_, allocator_, cmd_pool_, static_cast<std::uint32_t>(size),
                                                             static_cast<std::uint32_t>(count), images, srgb);
        return entry.array;
    };
    rt.albedo = make(terrain.layer_albedo_paths, true);
    rt.normal = make(terrain.layer_normal_paths, false);
}

HeightmapTerrainSystem::ChunkSettings HeightmapTerrainSystem::resolve_settings(const HeightmapTerrain& t, float camera_far,
                                                                               float max_tess) {
    ChunkSettings s;
    const bool tess = max_tess > 0.0f && t.tessellation.enabled;
    // Tessellation adds the near detail, so the base patch only has to hold the far shape.
    s.patch = t.patch_resolution > 0 ? t.patch_resolution : (tess ? 4 : 32);
    s.view_distance = t.view_distance > 0.0f ? t.view_distance : std::max(camera_far, 1.0f);
    s.detail_distance = t.tessellation.max_distance > 0.0f ? t.tessellation.max_distance : std::min(s.view_distance, 2000.0f);
    s.shadow_distance = t.shadow_distance > 0.0f ? t.shadow_distance : (t.view_distance > 0.0f ? s.view_distance : 0.0f);
    s.max_factor = tess ? std::min(t.tessellation.max_factor, max_tess) : 1.0f;
    return s;
}

float HeightmapTerrainSystem::chunk_distance(const HeightmapGeometry& g, const glm::vec3& origin, int cx, int cy,
                                             const glm::vec3& camera) {
    const glm::vec2 lo = glm::vec2(origin) + glm::vec2(static_cast<float>(cx), static_cast<float>(cy)) * g.chunk_size;
    const glm::vec2 hi = glm::min(lo + glm::vec2(g.chunk_size), glm::vec2(origin) + g.size);
    const glm::vec2 p(camera);
    const glm::vec2 d = glm::max(glm::max(lo - p, p - hi), glm::vec2(0.0f));
    return glm::length(d);
}

glm::vec3 HeightmapTerrainSystem::camera_position_(const HeightmapTerrain& terrain) const {
    if (auto* camera = coopa::gfx::engine::components::CameraComponent::main()) {
        if (camera->owner != nullptr) {
            if (auto* tc = camera->owner->get_transform()) return glm::vec3(tc->transform().get_world_matrix()[3]);
        }
    }
    return terrain.origin() + glm::vec3(terrain.data->geometry().size * 0.5f, 0.0f);
}

float HeightmapTerrainSystem::camera_far_() const {
    if (auto* camera = coopa::gfx::engine::components::CameraComponent::main()) return camera->clip_end;
    return 2000.0f;
}

void HeightmapTerrainSystem::stream_chunks_(HeightmapTerrain& terrain, HeightmapTerrainRuntime& rt, coopa::scene::Scene& scene,
                                            const ChunkSettings& s, const glm::vec3& camera) {
    const HeightmapGeometry& g = terrain.data->geometry();
    const glm::vec3 origin = terrain.origin();
    // One chunk of slack before dropping, so a camera on the boundary does not flicker chunks.
    const float keep = s.view_distance + g.chunk_size;
    // Spawning is cheap (an object, a transform, a renderer on the shared patch), but a jump
    // across a huge terrain should spread it over a few frames rather than hitch.
    int budget = 512;
    for (int cy = 0; cy < rt.chunk_count.y; ++cy) {
        for (int cx = 0; cx < rt.chunk_count.x; ++cx) {
            SceneObject*& slot = rt.chunks[static_cast<std::size_t>(cy) * rt.chunk_count.x + cx];
            const float d = chunk_distance(g, origin, cx, cy, camera);
            if (slot == nullptr && d <= s.view_distance && budget > 0) {
                spawn_chunk_(terrain, rt, scene, s, cx, cy);
                --budget;
            } else if (slot != nullptr && d > keep) {
                terrain.owner->detach_child(slot);
                slot = nullptr;
                --rt.live_chunks;
            }
        }
    }
}

void HeightmapTerrainSystem::refresh_chunk_water_(HeightmapTerrain& terrain,
                                                  coopa::scene::SceneObject& chunk_object, int cx, int cy) {
    ChunkWaterData still, flowing;
    build_smooth_chunk_water(terrain.field, glm::ivec2(cx, cy), terrain.field.geometry().chunk_tiles,
                             terrain.params.river_wave_scale, still, flowing);

    for (int pass = 0; pass < 2; ++pass) {
        const bool river = pass == 1;
        const ChunkWaterData& data = river ? flowing : still;
        const char* name = river ? "river" : "water";
        coopa::scene::SceneObject* existing = chunk_object.find_child(name);

        if (data.empty()) {
            // Water that has drained takes its body with it, rather than lingering as a sheet.
            if (existing != nullptr) chunk_object.detach_child(existing);
            continue;
        }
        if (existing == nullptr) {
            auto object = std::make_unique<SceneObject>(name);
            auto* tc = object->add_component<TransformComponent>();
            // add_child() does not link transforms; the water is placed in the chunk's space.
            if (auto* parent = chunk_object.get_transform()) tc->set_parent_transform(&parent->transform());
            auto* body = object->add_component<water::WaterBody>();
            water::copy_water_settings(terrain.water_look, *body);
            body->mode = river ? water::WaterMode::Flowing : water::WaterMode::Planar;
            existing = chunk_object.add_child(std::move(object));
        }
        if (auto* body = existing->get_component<water::WaterBody>()) {
            body->set_geometry(data.positions, data.uvs, data.indices, data.depths, data.wave_scales);
        }
    }
}

void HeightmapTerrainSystem::refresh_water_(HeightmapTerrain& terrain, HeightmapTerrainRuntime& rt) {
    // The records carry the water's kind and flow, so their revision is what moves when water
    // is painted, flooded, drained or applied from a map.
    const std::uint64_t revision = terrain.field.cursor().tiles;
    if (revision == rt.water_revision) return;
    rt.water_revision = revision;
    for (int cy = 0; cy < rt.chunk_count.y; ++cy) {
        for (int cx = 0; cx < rt.chunk_count.x; ++cx) {
            SceneObject* chunk = rt.chunks[static_cast<std::size_t>(cy) * rt.chunk_count.x + cx];
            if (chunk != nullptr) refresh_chunk_water_(terrain, *chunk, cx, cy);
        }
    }
}

void HeightmapTerrainSystem::spawn_chunk_(HeightmapTerrain& terrain, HeightmapTerrainRuntime& rt, coopa::scene::Scene& scene,
                                          const ChunkSettings& s, int cx, int cy) {
    const HeightmapData& data = *terrain.data;
    auto object = std::make_unique<SceneObject>("terrain_chunk_" + std::to_string(cx) + "_" + std::to_string(cy));
    object->add_component<toy::scene::RuntimeObject>("HeightmapTerrain", "terrain chunk -- edit the HeightmapTerrain");
    auto* tc = object->add_component<TransformComponent>();
    glm::vec3 pos, scale;
    chunk_box(data, cx, cy, pos, scale);
    tc->transform().set_position(pos);
    tc->transform().set_scale(scale);
    auto* mr = object->add_component<MeshRenderer>();
    mr->set_mesh(patch_mesh(s.patch));
    // Probe and GI bakes draw with stock shaders, which would see the flat patch.
    mr->affects_reflection_probes = false;
    // Detail comes from tessellation (by screen size), not from mesh LODs.
    mr->lods_enabled = false;
    apply_material_(terrain, rt, s, *mr);
    // add_child() links ownership only: the chunk's transform must follow the terrain's.
    if (auto* parent = terrain.owner->get_transform()) tc->set_parent_transform(&parent->transform());
    SceneObject* raw = terrain.owner->add_child(std::move(object));
    scene.adopt(*raw);
    rt.chunks[static_cast<std::size_t>(cy) * rt.chunk_count.x + cx] = raw;
    ++rt.live_chunks;
    // The water over it comes with it: a chunk streaming in beside a lake should arrive wet.
    refresh_chunk_water_(terrain, *raw, cx, cy);
}

void HeightmapTerrainSystem::destroy_chunks_(HeightmapTerrain& terrain, HeightmapTerrainRuntime& rt) {
    if (terrain.owner != nullptr) {
        for (SceneObject* c : rt.chunks) {
            if (c != nullptr) terrain.owner->detach_child(c);
        }
    }
    rt.chunks.clear();
    rt.live_chunks = 0;
    rt.material_signature.clear();
}

void HeightmapTerrainSystem::apply_material_(const HeightmapTerrain& terrain, const HeightmapTerrainRuntime& rt,
                                             const ChunkSettings& s, MeshRenderer& mr) const {
    auto& m = mr.material;
    m.shader = "heightmap_terrain";
    m.shader_params = glm::vec4(static_cast<float>(rt.slot), 0.0f, 0.0f, 0.0f);
    m.displacement_handle = rt.gpu->height_handle;
    m.displacement_scale = 0.0f;   // the shader places vertices itself; no stock displacement
    m.albedo = glm::vec3(1.0f);
    // Deep snow is the terrain's snow: the cover layer on top would whiten trenches shut.
    m.snow = !terrain.deep_snow;
    mr.tessellation.enabled = terrain.tessellation.enabled && s.max_factor > 1.0f;
    mr.tessellation.edge_pixels = terrain.tessellation.edge_pixels;
    mr.tessellation.max_factor = s.max_factor;
    mr.tessellation.max_distance = s.detail_distance;
    mr.cast_shadows = terrain.cast_shadows;
    mr.shadow_max_distance = s.shadow_distance;
}

void HeightmapTerrainSystem::write_materials_(HeightmapTerrain& terrain, HeightmapTerrainRuntime& rt, const ChunkSettings& s) {
    std::ostringstream sig;
    sig << rt.slot << "|" << rt.gpu->height_id << "|" << terrain.deep_snow << "|" << terrain.tessellation.enabled << "|"
        << terrain.tessellation.edge_pixels << "|" << s.max_factor << "|" << s.detail_distance << "|" << terrain.cast_shadows
        << "|" << s.shadow_distance;
    if (sig.str() == rt.material_signature) return;
    rt.material_signature = sig.str();
    for (SceneObject* c : rt.chunks) {
        if (c == nullptr) continue;
        if (auto* mr = c->get_component<MeshRenderer>()) apply_material_(terrain, rt, s, *mr);
    }
}

void HeightmapTerrainSystem::fit_chunk_boxes_(HeightmapTerrain& terrain, HeightmapTerrainRuntime& rt) {
    const HeightmapData& data = *terrain.data;
    if (rt.bounds_revision == data.revision()) return;
    const IRect r = data.height_dirty_since(rt.bounds_revision);
    rt.bounds_revision = data.revision();
    if (r.empty()) return;
    const glm::vec2 c = data.cell();
    const float cs = data.geometry().chunk_size;
    const int cx0 = std::clamp(static_cast<int>(std::floor(static_cast<float>(r.x0 - 1) * c.x / cs)), 0, rt.chunk_count.x - 1);
    const int cy0 = std::clamp(static_cast<int>(std::floor(static_cast<float>(r.y0 - 1) * c.y / cs)), 0, rt.chunk_count.y - 1);
    const int cx1 = std::clamp(static_cast<int>(std::floor(static_cast<float>(r.x1) * c.x / cs)), 0, rt.chunk_count.x - 1);
    const int cy1 = std::clamp(static_cast<int>(std::floor(static_cast<float>(r.y1) * c.y / cs)), 0, rt.chunk_count.y - 1);
    for (int cy = cy0; cy <= cy1; ++cy) {
        for (int cx = cx0; cx <= cx1; ++cx) {
            SceneObject* obj = rt.chunks[static_cast<std::size_t>(cy) * rt.chunk_count.x + cx];
            if (obj == nullptr) continue;
            glm::vec3 pos, scale;
            chunk_box(data, cx, cy, pos, scale);
            auto* tc = obj->get_transform();
            tc->transform().set_position(pos);
            tc->transform().set_scale(scale);
        }
    }
}

void HeightmapTerrainSystem::update_collider_(HeightmapTerrain& terrain, HeightmapTerrainRuntime& rt, coopa::scene::Scene& scene,
                                              const coopa::scene::FrameContext&) {
    using coopa::physx::components::HeightfieldCollider;
    auto refresh_physics = [&scene]() {
        if (auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"))) physics->refresh();
    };
    if (!terrain.collision) {
        if (rt.collision != nullptr) {
            terrain.owner->detach_child(rt.collision);
            rt.collision = nullptr;
            refresh_physics();
        }
        return;
    }
    const HeightmapData& data = *terrain.data;
    if (rt.collision == nullptr) {
        auto object = std::make_unique<SceneObject>("terrain_collision");
        object->add_component<toy::scene::RuntimeObject>("HeightmapTerrain", "terrain collider -- edit the HeightmapTerrain");
        auto* ctc = object->add_component<TransformComponent>();
        if (auto* parent = terrain.owner->get_transform()) ctc->set_parent_transform(&parent->transform());
        auto* col = object->add_component<HeightfieldCollider>();
        if (!terrain.physics_material.empty()) {
            col->set_material_asset(assets_.load<coopa::physx::dynamics::PhysicsMaterial>(
                "physics_materials/" + terrain.physics_material + ".yaml"));
        }
        rt.collision = terrain.owner->add_child(std::move(object));
        scene.adopt(*rt.collision);
        rt.collider_revision = 0;
        rt.collider_dirty_since = -1.0;
        refresh_physics();
    }
    if (rt.collider_revision == data.height_revision()) return;

    // While the grid is being sculpted, rebuild once the strokes pause rather than every frame:
    // the copy and its culling pyramid are a few milliseconds on a big grid.
    const bool first = rt.collider_revision == 0;
    if (!first) {
        if (rt.collider_dirty_since < 0.0) rt.collider_dirty_since = time_;
        if (time_ - rt.collider_dirty_since < 0.25 && rt.collider_seen_revision != data.height_revision()) {
            rt.collider_seen_revision = data.height_revision();
            rt.collider_dirty_since = time_;
            return;
        }
        if (time_ - rt.collider_dirty_since < 0.25) return;
    }
    auto* col = rt.collision->get_component<HeightfieldCollider>();
    if (col == nullptr) return;
    col->set_grid(std::make_shared<const coopa::physx::geometry::HeightGrid>(data.nx(), data.ny(), data.cell(), data.heights()));
    rt.collider_revision = data.height_revision();
    rt.collider_seen_revision = rt.collider_revision;
    rt.collider_dirty_since = -1.0;
}

void HeightmapTerrainSystem::evict_() {
    constexpr std::uint64_t k_idle_frames = 120;
    for (auto it = gpu_cache_.begin(); it != gpu_cache_.end();) {
        if (frame_ - it->second->last_used_frame > k_idle_frames) {
            if (!it->second->height_id.empty()) assets_.unload(coopa::asset::AssetId::from_path(it->second->height_id));
            it = gpu_cache_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = array_cache_.begin(); it != array_cache_.end();) {
        if (frame_ - it->second.last_used_frame > k_idle_frames) it = array_cache_.erase(it);
        else ++it;
    }
}

HeightmapTerrainSystem* install_heightmap_terrain_system(coopa::scene::Scene& scene, coopa::gfx::core::Device& device,
                                                         coopa::gfx::memory::Allocator& allocator,
                                                         coopa::gfx::command::CommandPool& cmd_pool,
                                                         coopa::asset::AssetManager& assets, int order) {
    auto system = std::make_unique<HeightmapTerrainSystem>(device, allocator, cmd_pool, assets);
    HeightmapTerrainSystem* raw = system.get();
    scene.add_system(std::move(system), order);
    return raw;
}

std::vector<HeightmapView> heightmap_views(const coopa::scene::Scene& scene) {
    std::vector<HeightmapView> out;
    for (const HeightmapTerrain* t : scene.get_components<HeightmapTerrain>()) {
        if (t->mode != TerrainMode::Heightmap) continue;
        if (t->data != nullptr && t->owner != nullptr && t->owner->active()) out.push_back(t->view());
    }
    return out;
}

HeightmapView heightmap_at(const coopa::scene::Scene& scene, const glm::vec3& world) {
    for (const HeightmapView& v : heightmap_views(scene)) {
        if (v.contains(world)) return v;
    }
    return HeightmapView{};
}

} // namespace world
} // namespace toy
