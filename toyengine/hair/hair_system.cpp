#include "toyengine/hair/hair_system.h"

#include <cstring>
#include <iterator>
#include <vector>

#include <coopa/asset/asset_manager.h>
#include <coopa/yaml/document.h>

#include <physxcoopa/hair/strand_builder.h>
#include <physxcoopa/system/physics_system.h>

#include "toyengine/curve/curve.h"
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>

#include "toyengine/scene/runtime_object.h"
#include "toyengine/scene/skinned_mesh_renderer.h"

namespace toy {
namespace hair {

namespace {

using coopa::gfx::engine::components::MeshRenderer;
using coopa::gfx::engine::data::Mesh;
using coopa::gfx::engine::data::MeshCpuData;
using coopa::scene::Scene;
using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;

/// The sibling SkinnedMeshRenderer whose bind pose hair should grow on, or null for a rigid scalp.
toy::scene::SkinnedMeshRenderer* skinned_scalp(const Hair& hair) {
    if (hair.owner == nullptr) return nullptr;
    auto* smr = hair.owner->get_component<toy::scene::SkinnedMeshRenderer>();
    return (smr != nullptr && smr->source() != nullptr) ? smr : nullptr;
}

/**
 * The scalp's mesh: the component's own `scalp_mesh`, else the sibling MeshRenderer's.
 *
 * Only consulted for a RIGID scalp. A skinned one grows on its SkinnedMeshRenderer's own bind
 * pose instead -- which it must, because `Mesh::build_cpu()` welds and reorders the file's
 * vertices while a `SkinnedMeshSource` keeps them per-corner, so the two do not share triangle
 * indices and a root recorded against one would be meaningless against the other.
 */
std::string scalp_mesh_key(const Hair& hair) {
    if (!hair.scalp_mesh.empty()) return hair.scalp_mesh;
    if (hair.owner == nullptr) return {};
    if (auto* mr = hair.owner->get_component<MeshRenderer>()) return mr->mesh_path();
    return {};
}

/// A ScalpMesh view over a SkinnedMeshRenderer's BIND POSE. The renderer outlives the view.
ScalpMesh skinned_scalp_mesh(const toy::scene::SkinnedMeshRenderer& smr) {
    ScalpMesh out;
    const auto* src = smr.source();
    if (src == nullptr) return out;
    out.vertices     = src->vertices.data();
    out.vertex_count = src->vertices.size();
    out.indices      = src->indices.data();
    out.index_count  = src->indices.size();
    return out;
}

/// World down (-Z, this engine is Z-up) expressed in the scalp's local space, for droop.
glm::vec3 down_in_local_space(const SceneObject& owner) {
    const auto* tc = owner.get_transform();
    if (tc == nullptr) return glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::mat4 world = tc->get_world_matrix();
    // Direction, so the inverse of the upper 3x3 and no translation. Scale is divided out by the
    // normalise in grow_strands(), which is why the full inverse is not needed here.
    const glm::mat3 inv = glm::inverse(glm::mat3(world));
    const glm::vec3 down = inv * glm::vec3(0.0f, 0.0f, -1.0f);
    return glm::length(down) > 1e-6f ? glm::normalize(down) : glm::vec3(0.0f, 0.0f, -1.0f);
}

} // namespace

HairSystem::HairSystem(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                       coopa::asset::AssetManager& assets, std::uint32_t frames_in_flight)
    : device_(device), allocator_(allocator), assets_(assets),
      frames_in_flight_(frames_in_flight < 1u ? 1u : frames_in_flight) {}

HairSystem::~HairSystem() = default;

void HairSystem::regenerate(const std::string& key) { forced_.insert(key); }

void HairSystem::regenerate_all() { force_all_ = true; }

std::size_t HairSystem::strand_count(const std::string& key) const {
    const auto it = grooms_.find(key);
    return it == grooms_.end() ? 0u : it->second.set.strand_count();
}

std::size_t HairSystem::vertex_count(const std::string& key) const {
    const auto it = grooms_.find(key);
    if (it == grooms_.end() || it->second.mesh == nullptr) return 0u;
    return it->second.mesh->vertex_count();
}

coopa::scene::WarmupState HairSystem::warm_up_state() const {
    coopa::scene::WarmupState state;
    int outstanding = 0;
    for (const auto& [key, groom] : grooms_) {
        if (groom.pending || !groom.built) ++outstanding;
    }
    if (grooms_.empty()) return state;  // nothing authored: hold nothing up
    state.participates = true;
    state.what         = "hair";
    state.remaining    = outstanding;
    state.warm         = outstanding == 0;
    state.progress     = grooms_.empty() ? 1.0f
                                         : 1.0f - static_cast<float>(outstanding) /
                                                      static_cast<float>(grooms_.size());
    return state;
}

bool HairSystem::should_rebuild_(const Groom& groom, Hair& hair, const ScalpEntry& scalp,
                                 std::uint64_t guides_hash, bool forced) const {
    if (forced || !groom.built) return true;
    // A re-saved scalp invalidates the strands even though nothing the author typed changed.
    if (scalp.write_time != groom.scalp_write_time) return true;
    if (hair.params.regenerate != HairRegenerate::OnChange) return false;
    // A guide dragged in Pen Mode is a change the params hash cannot see.
    if (guides_hash != groom.guides_hash) return true;
    return hair.params.hash() != groom.params_hash;
}

std::vector<HairGuide> HairSystem::collect_guides_(const Hair& hair, int points) const {
    std::vector<HairGuide> guides;
    if (hair.params.guides.empty() || hair.owner == nullptr || hair.scene == nullptr) return guides;

    // Named under the owner first, so two heads can each have a child called "Guides" without
    // one wearing the other's hair; only then scene-wide, which is what lets a shared rig of
    // guides sit elsewhere in the hierarchy.
    SceneObject* root = nullptr;
    for (const auto& child : hair.owner->children()) {
        if (child != nullptr && child->name() == hair.params.guides) {
            root = child.get();
            break;
        }
    }
    if (root == nullptr) root = hair.scene->find_object(hair.params.guides);
    if (root == nullptr) return guides;

    const auto* scalp_tc = hair.owner->get_transform();
    const glm::mat4 to_scalp = scalp_tc != nullptr ? glm::inverse(scalp_tc->get_world_matrix())
                                                   : glm::mat4(1.0f);

    root->for_each_recursive([&](SceneObject& object) {
        const auto* curve = object.get_component<toy::curve::Curve>();
        if (curve == nullptr || curve->data.empty()) return;

        // Curve data is LOCAL to its own object, so it goes through world space to reach the
        // scalp's. Handles are POSITIONS, not deltas -- the same transform procgraph's `curve:`
        // binding applies, and getting it wrong bends every guide toward the origin.
        toy::curve::CurveData local = curve->data;
        const auto* tc = object.get_transform();
        const glm::mat4 to_world = tc != nullptr ? tc->get_world_matrix() : glm::mat4(1.0f);
        const glm::mat4 m = to_scalp * to_world;
        for (auto& point : local.points) {
            point.position   = glm::vec3(m * glm::vec4(point.position, 1.0f));
            point.in_handle  = glm::vec3(m * glm::vec4(point.in_handle, 1.0f));
            point.out_handle = glm::vec3(m * glm::vec4(point.out_handle, 1.0f));
        }

        HairGuide guide = make_hair_guide(local, points);
        if (guide.valid()) guides.push_back(std::move(guide));
    });
    return guides;
}

std::uint64_t HairSystem::hash_guides_(const std::vector<HairGuide>& guides) {
    std::uint64_t h = 14695981039346656037ull;
    const auto mix = [&h](float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        h ^= static_cast<std::uint64_t>(bits);
        h *= 1099511628211ull;
    };
    for (const HairGuide& guide : guides) {
        mix(guide.root.x); mix(guide.root.y); mix(guide.root.z);
        mix(guide.length);
        for (const glm::vec3& offset : guide.offsets) { mix(offset.x); mix(offset.y); mix(offset.z); }
    }
    mix(static_cast<float>(guides.size()));
    return h;
}

const HairSystem::ScalpEntry* HairSystem::load_scalp_(const std::string& mesh_key,
                                                      const std::string& base_dir,
                                                      const std::string& groom_key) {
    const std::string path = assets_.source().resolve("meshes/" + mesh_key + ".yaml", base_dir);

    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec) {
        if (warned_.insert(groom_key + "|missing").second) {
            logger_.warn("'" + groom_key + "' cannot find its scalp mesh '" + mesh_key +
                         "' (looked for " + path + ").");
        }
        return nullptr;
    }

    auto it = scalps_.find(path);
    if (it != scalps_.end() && it->second.write_time == stamp && it->second.data != nullptr) {
        return &it->second;   // one parse per unique scalp mesh, however many grooms share it
    }

    try {
        const fkyaml::node node = coopa::yaml::load_document(path);
        // An EMPTY lod_config suppresses the mesh file's own `lods:` -- Mesh::build_cpu() reads
        // them off lod_config when given one and off the mesh otherwise. Hair wants the base
        // triangles; building the simplified levels would run meshoptimizer for a result nothing
        // here reads, and would grow hair twice over the same surface.
        const fkyaml::node no_lods = fkyaml::node::mapping();
        ScalpEntry entry;
        entry.data = std::make_shared<MeshCpuData>(
            coopa::gfx::engine::data::Mesh::build_cpu(node, &no_lods));
        entry.write_time = stamp;
        scalps_[path] = std::move(entry);
    } catch (const std::exception& e) {
        if (warned_.insert(groom_key + "|parse").second) {
            logger_.warn("'" + groom_key + "' could not read its scalp mesh '" + mesh_key +
                         "': " + e.what());
        }
        return nullptr;
    }
    return &scalps_[path];
}

void HairSystem::execute(Scene& scene, const coopa::scene::FrameContext&) {
    std::unordered_set<std::string> live;

    for (Hair* hair : scene.get_components<Hair>()) {
        if (hair->owner == nullptr) continue;
        const std::string key = hair->state_key();
        live.insert(key);
        Groom& groom = grooms_[key];

        // A skinned scalp grows on its renderer's own bind pose -- no file parse, and the only
        // triangles whose indices a deforming root can be recorded against.
        toy::scene::SkinnedMeshRenderer* skin = skinned_scalp(*hair);
        if (skin != nullptr) {
            const ScalpMesh scalp = skinned_scalp_mesh(*skin);
            std::vector<HairGuide> guides = collect_guides_(*hair, hair->params.segments + 1);
            const std::uint64_t guides_hash = hash_guides_(guides);
            const bool forced =
                hair->take_regenerate_request() || force_all_ || forced_.erase(key) > 0;
            groom.pending = false;
            if (forced || !groom.built ||
                (hair->params.regenerate == HairRegenerate::OnChange &&
                 (hair->params.hash() != groom.params_hash || guides_hash != groom.guides_hash))) {
                build_on_(key, groom, *hair, scalp, std::move(guides), guides_hash, scene);
                bind_strands_(groom, *hair, scene);
            } else if (groom.params.simulate && !groom.strands.is_valid()) {
                bind_strands_(groom, *hair, scene);
            }
            republish_if_missing_(key, groom, *hair, scene);
            drive_strand_roots_(groom, *hair, scene);
            refresh_material_(groom, *hair);
            continue;
        }

        const std::string mesh_key = scalp_mesh_key(*hair);
        if (mesh_key.empty()) {
            if (warned_.insert(key).second) {
                logger_.warn("'" + key +
                             "' has no scalp mesh. Give the object a MeshRenderer with a "
                             "mesh_path, or name one on the component as scalp_mesh -- which is "
                             "what a skinned scalp needs, since a SkinnedMeshRenderer does not "
                             "keep its mesh path.");
            }
            groom.pending = false;
            continue;
        }

        if (groom.scalp_key != mesh_key) {
            groom.scalp_key = mesh_key;
            groom.built     = false;
        }
        const ScalpEntry* scalp = load_scalp_(mesh_key, hair->asset_base_dir, key);
        if (scalp == nullptr) {
            // Unreadable, or not written yet. Keep trying: a scene whose mesh is still being
            // saved by the editor should grow its hair as soon as the file appears, and
            // warm_up_state() holds a loading screen open while this is true.
            groom.pending = true;
            continue;
        }
        groom.pending = false;

        // Snapshotted every frame rather than only on a rebuild: a guide moving is one of the
        // things that MAKES a rebuild due, and a handful of curves is cheap to copy.
        std::vector<HairGuide> guides = collect_guides_(*hair, hair->params.segments + 1);
        const std::uint64_t    guides_hash = hash_guides_(guides);

        const bool forced = hair->take_regenerate_request() || force_all_ || forced_.erase(key) > 0;
        if (should_rebuild_(groom, *hair, *scalp, guides_hash, forced)) {
            // A rebuilt groom's old guides describe geometry that no longer exists. Toggling
            // `simulate` comes through here too, since it is part of the params hash.
            build_(key, groom, *hair, *scalp, std::move(guides), guides_hash, scene);
            bind_strands_(groom, *hair, scene);
        } else if (groom.params.simulate && !groom.strands.is_valid()) {
            // Deferred, like ClothRenderer's binding: PhysicsSystem may not have been installed
            // -- or may not have bound -- on the frame this groom was first built, and a groom
            // that silently never simulates is the worst of the available failures.
            bind_strands_(groom, *hair, scene);
        }

        // The runtime child can go missing without anything in the groom changing: the editor
        // destroys and recreates an object on every inspector keystroke, and that rebuild lands
        // AFTER this system has run for the frame -- taking the published child with it. Nothing
        // then marks the groom dirty, so without this check the hair simply stays gone until the
        // scene is reloaded. Re-publishing is a pointer swap; the mesh itself is untouched.
        republish_if_missing_(key, groom, *hair, scene);

        // Before Physics (100) steps: this system is order 64, so the roots the solver reads are
        // this frame's. They are one frame stale against an ANIMATED scalp, whose bones resolve at
        // 300 -- which is exactly the gap a cloth anchor's substep lerp also covers.
        drive_strand_roots_(groom, *hair, scene);

        // Every frame, and deliberately outside the rebuild: the wind response and the LOD
        // thresholds are not part of the geometry hash, so tuning them must take effect without
        // re-scattering the groom.
        refresh_material_(groom, *hair);
    }

    force_all_ = false;
    forced_.clear();
    prune_(scene, live);
}

void HairSystem::build_(const std::string& key, Groom& groom, Hair& hair, const ScalpEntry& entry,
                        std::vector<HairGuide> guides, std::uint64_t guides_hash, Scene& scene) {
    if (entry.data == nullptr) return;
    groom.scalp_write_time = entry.write_time;
    build_on_(key, groom, hair, ScalpMesh::from_cpu_data(*entry.data), std::move(guides),
              guides_hash, scene);
}

void HairSystem::build_on_(const std::string& key, Groom& groom, Hair& hair, const ScalpMesh& scalp,
                           std::vector<HairGuide> guides, std::uint64_t guides_hash, Scene& scene) {
    groom.params = hair.params;
    groom.params.sanitize();
    groom.params_hash = hair.params.hash();
    groom.guides_hash = guides_hash;

    if (!scalp.valid()) {
        if (warned_.insert(key + "|empty").second) {
            logger_.warn("'" + key + "' scalp mesh '" + groom.scalp_key +
                         "' has no triangles to grow from.");
        }
        return;
    }

    MaskImage       mask;
    const MaskImage* mask_ptr = nullptr;
    if (!groom.params.root_mask.empty()) {
        const std::string resolved = assets_.source().resolve(groom.params.root_mask, hair.asset_base_dir);
        if (load_mask_image(resolved, mask) && mask.valid()) {
            mask_ptr = &mask;
        } else if (warned_.insert(key + "|mask").second) {
            logger_.warn("'" + key + "' could not read root_mask '" + groom.params.root_mask +
                         "'; growing unmasked.");
        }
    }

    groom.set = scatter_roots(scalp, groom.params, mask_ptr);
    grow_strands(groom.set, groom.params, down_in_local_space(*hair.owner), guides);

    MeshCpuData built;
    build_geometry(groom.set, groom.params, built);
    if (built.vertices.empty() || built.indices.empty()) {
        // A groom that grew nothing (density 0, or a mask that rejected everything) is not an
        // error, but it must not leave last build's mesh on screen.
        groom.mesh.reset();
        groom.vertices.clear();
        groom.built = true;
        if (SceneObject* child = find_child_(*hair.owner)) {
            if (auto* mr = child->get_component<MeshRenderer>()) mr->set_mesh({});
        }
        return;
    }

    // A simulated groom is rewritten per frame, so it needs one buffer per frame in flight: this
    // pipeline never waits, and a single shared buffer would race the GPU still reading the last
    // frame. A static groom is uploaded once and keeps one buffer.
    const std::uint32_t buffers = groom.params.simulate ? frames_in_flight_ : 1u;
    groom.mesh = std::make_shared<Mesh>(Mesh::from_arrays(device_, allocator_, built.vertices,
                                                          built.indices, buffers, false, &built.lods,
                                                          built.cull_screen_size));
    // Kept only for the simulated path's in-place strand rewrites; a static groom never reads it.
    if (groom.params.simulate) {
        groom.vertices = std::move(built.vertices);
    } else {
        groom.vertices.clear();
        groom.vertices.shrink_to_fit();
    }
    groom.built = true;

    publish_(key, groom, hair, scene);
}

SceneObject* HairSystem::find_child_(SceneObject& owner) const {
    for (const auto& child : owner.children()) {
        if (child == nullptr || child->name() != Hair::child_object_name()) continue;
        // Confirm it is ours rather than an authored object that happens to share the name.
        const auto* marker = child->get_component<toy::scene::RuntimeObject>();
        if (marker != nullptr && marker->owner_system == "Hair") return child.get();
    }
    return nullptr;
}

void HairSystem::publish_(const std::string& key, Groom& groom, Hair& hair, Scene& scene) {
    SceneObject* child = find_child_(*hair.owner);
    if (child == nullptr) {
        auto object = std::make_unique<SceneObject>(Hair::child_object_name());
        // Marked before it is adopted, so the editor never sees an unmarked runtime object even
        // for a frame -- and the whole subtree inherits the marker.
        object->add_component<toy::scene::RuntimeObject>(
            "Hair", "generated groom -- edit the Hair component, not this");
        auto* tc = object->add_component<TransformComponent>();
        auto* mr = object->add_component<MeshRenderer>();
        mr->lods_enabled = true;
        // Probe and GI bakes draw with stock shaders, which would see the strands unswayed.
        mr->affects_reflection_probes = false;
        // add_child() links OWNERSHIP only. Without this the groom would be drawn at the world
        // origin however the head moves. The geometry is already in the scalp's local space, so
        // the child's own transform stays identity and the scalp's matrix applies exactly once.
        if (auto* parent = hair.owner->get_transform()) tc->set_parent_transform(&parent->transform());
        SceneObject* raw = hair.owner->add_child(std::move(object));
        scene.adopt(*raw);   // stamps Component::scene and bumps the structure revision
        child = raw;
    }

    if (auto* mr = child->get_component<MeshRenderer>()) {
        // A stable synthetic id, so a rebuild replaces the payload rather than leaking a slot per
        // keystroke. "runtime/" keeps it out of the real-path namespace.
        mr->set_mesh(assets_.create<Mesh>("runtime/hair/" + key, groom.mesh));
    }
}

void HairSystem::republish_if_missing_(const std::string& key, Groom& groom, Hair& hair,
                                       Scene& scene) {
    if (!groom.built || groom.mesh == nullptr || hair.owner == nullptr) return;
    if (find_child_(*hair.owner) != nullptr) return;
    publish_(key, groom, hair, scene);
}

void HairSystem::refresh_material_(Groom& groom, Hair& hair) const {
    if (hair.owner == nullptr) return;
    SceneObject* child = find_child_(*hair.owner);
    if (child == nullptr) return;
    auto* mr = child->get_component<MeshRenderer>();
    if (mr == nullptr) return;

    mr->material = hair.material;
    // What the `hair` surface shader reads. z is the groom's length in metres: the shader scales
    // the sway by it, and uv.x alone cannot say how long a strand is.
    mr->material.shader_params = glm::vec4(hair.params.wind_flexibility, hair.params.wind_frequency,
                                           hair.params.length, 0.0f);
}


// --- Simulation ---------------------------------------------------------------------------------

coopa::physx::PhysicsWorld* HairSystem::physics_world_(Scene& scene) const {
    // By NAME, the way VehicleSystem finds it: nothing hands one system a pointer to another, and
    // a scene with no physics must simply not simulate rather than fail to load.
    auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"));
    return physics != nullptr ? &physics->world() : nullptr;
}

void HairSystem::release_strands_(Groom& groom, Scene& scene) {
    if (groom.strands.is_valid()) {
        if (auto* world = physics_world_(scene)) world->remove_strands(groom.strands);
    }
    groom.strands = {};
    groom.guide_index.clear();
    groom.guide_weights.clear();
    groom.bind_offsets.clear();
    groom.guides_per_strand = 0;
    groom.simulated_guides = 0;
}

void HairSystem::bind_strands_(Groom& groom, Hair& hair, Scene& scene) {
    release_strands_(groom, scene);
    if (!groom.params.simulate || groom.set.empty() || hair.owner == nullptr) return;

    auto* world = physics_world_(scene);
    if (world == nullptr) return;   // no physics in this scene: the groom stays static

    const std::size_t strand_count = groom.set.strand_count();
    const int         points = groom.set.points_per_strand;

    // Guides are a SUBSET of the render strands, taken on a stride. Because the groom's strands
    // were already ordered for even thinning, a stride over them is spread across the whole scalp
    // rather than clustered -- the same property the LOD prefixes rely on.
    const std::size_t wanted =
        std::min<std::size_t>(static_cast<std::size_t>(groom.params.sim_guides), strand_count);
    if (wanted == 0) return;
    const std::size_t stride = std::max<std::size_t>(1u, strand_count / wanted);

    const glm::mat4 to_world = hair.owner->get_transform() != nullptr
                                   ? hair.owner->get_transform()->get_world_matrix()
                                   : glm::mat4(1.0f);
    const glm::quat rotation = glm::quat_cast(glm::mat3(to_world));

    std::vector<coopa::physx::hair::StrandDesc> descs;
    groom.guide_strand.clear();
    for (std::size_t i = 0; i < strand_count && descs.size() < wanted; i += stride) {
        coopa::physx::hair::StrandDesc desc;
        desc.root_orientation = rotation;
        const glm::vec3* p = groom.set.strand(i);
        for (int k = 0; k < points; ++k) {
            desc.points.push_back(glm::vec3(to_world * glm::vec4(p[k], 1.0f)));
        }
        descs.push_back(std::move(desc));
        groom.guide_strand.push_back(static_cast<std::uint32_t>(i));
    }
    if (descs.empty()) return;

    groom.strands = world->add_strands(
        coopa::physx::hair::make_strands(descs, coopa::physx::hair::StrandParams{}));
    if (!groom.strands.is_valid()) return;
    groom.simulated_guides = descs.size();

    // Bind every render strand to its nearest guides, once, so the per-frame deform is a weighted
    // sum and nothing searches.
    const std::uint32_t k_guides =
        static_cast<std::uint32_t>(std::min<std::size_t>(3u, descs.size()));
    groom.guides_per_strand = k_guides;
    groom.guide_index.assign(strand_count * k_guides, 0u);
    groom.guide_weights.assign(strand_count * k_guides, 0.0f);
    groom.bind_offsets.assign(strand_count * static_cast<std::size_t>(points), glm::vec3(0.0f));

    std::vector<std::pair<float, std::uint32_t>> nearest;
    const auto farther = [](const std::pair<float, std::uint32_t>& a,
                            const std::pair<float, std::uint32_t>& b) { return a.first < b.first; };
    for (std::size_t i = 0; i < strand_count; ++i) {
        const glm::vec3 root = groom.set.roots[i].position;
        nearest.clear();
        for (std::uint32_t g = 0; g < groom.guide_strand.size(); ++g) {
            const glm::vec3 delta = groom.set.roots[groom.guide_strand[g]].position - root;
            const float d2 = glm::dot(delta, delta);
            if (nearest.size() < k_guides) {
                nearest.emplace_back(d2, g);
                std::push_heap(nearest.begin(), nearest.end(), farther);
            } else if (d2 < nearest.front().first) {
                std::pop_heap(nearest.begin(), nearest.end(), farther);
                nearest.back() = {d2, g};
                std::push_heap(nearest.begin(), nearest.end(), farther);
            }
        }

        float total = 0.0f;
        for (const auto& [d2, g] : nearest) total += 1.0f / (d2 + 1e-4f);
        for (std::size_t n = 0; n < nearest.size(); ++n) {
            groom.guide_index[i * k_guides + n] = nearest[n].second;
            groom.guide_weights[i * k_guides + n] =
                total > 0.0f ? (1.0f / (nearest[n].first + 1e-4f)) / total : 0.0f;
        }

        // What this strand looked like RELATIVE to the blend of its guides at bind time. Carrying
        // that offset is what keeps a lock spread out: without it every strand following the same
        // guides would collapse onto one line the moment the solver moved them.
        const glm::vec3* mine = groom.set.strand(i);
        for (int k = 0; k < points; ++k) {
            glm::vec3 blended(0.0f);
            for (std::size_t n = 0; n < nearest.size(); ++n) {
                const glm::vec3* guide = groom.set.strand(groom.guide_strand[nearest[n].second]);
                blended += guide[k] * groom.guide_weights[i * k_guides + n];
            }
            groom.bind_offsets[i * static_cast<std::size_t>(points) + k] = mine[k] - blended;
        }
    }
}

void HairSystem::drive_strand_roots_(Groom& groom, Hair& hair, Scene& scene) {
    if (!groom.strands.is_valid() || hair.owner == nullptr) return;
    auto* world = physics_world_(scene);
    if (world == nullptr) return;
    auto* set = world->get_strands(groom.strands);
    if (set == nullptr) return;

    const glm::mat4 to_world = hair.owner->get_transform() != nullptr
                                   ? hair.owner->get_transform()->get_world_matrix()
                                   : glm::mat4(1.0f);
    groom.to_local = glm::inverse(to_world);
    const glm::quat rotation = glm::quat_cast(glm::mat3(to_world));

    // A SKINNED scalp deforms, so a root is not where it was grown: it is the same barycentric
    // point on the same triangle, wherever skinning has since put that triangle's three corners.
    // Only the GUIDES are recomputed -- a few dozen interpolations -- because every other strand
    // follows them anyway.
    const toy::scene::SkinnedMeshRenderer* skin = skinned_scalp(hair);
    const std::vector<Vertex>* deformed = nullptr;
    if (skin != nullptr) {
        const std::vector<Vertex>& live = skin->skinned_vertices();
        const auto* src = skin->source();
        if (src != nullptr && live.size() == src->vertices.size() && !live.empty()) deformed = &live;
    }

    std::vector<glm::vec3> positions;
    std::vector<glm::quat> orientations;
    positions.reserve(set->strands.size());
    orientations.reserve(set->strands.size());
    for (std::size_t g = 0; g < set->strands.size() && g < groom.guide_strand.size(); ++g) {
        const std::size_t  i = groom.guide_strand[g];
        const StrandRoot&  root = groom.set.roots[i];
        glm::vec3 local = root.position;
        if (deformed != nullptr) {
            const std::size_t t = static_cast<std::size_t>(root.triangle) * 3u;
            const auto* indices = skin->source()->indices.data();
            if (t + 2 < skin->source()->indices.size()) {
                local = (*deformed)[indices[t]].position * root.bary.x +
                        (*deformed)[indices[t + 1]].position * root.bary.y +
                        (*deformed)[indices[t + 2]].position * root.bary.z;
            }
        }
        positions.push_back(glm::vec3(to_world * glm::vec4(local, 1.0f)));
        orientations.push_back(rotation);
    }
    coopa::physx::hair::set_strand_roots(*set, positions, orientations);

    // Every tunable pushed in fresh each frame, not copied once at bind time: dragging stiffness
    // in the inspector then takes effect immediately instead of needing the groom re-scattered,
    // and none of these change a single vertex so a rebuild would be pure waste.
    set->params.global_shape         = hair.params.sim_stiffness;
    set->params.global_shape_falloff = hair.params.sim_stiffness_falloff;
    set->params.local_shape          = hair.params.sim_curl_hold;
    set->params.damping              = hair.params.sim_damping;
    set->params.air_drag             = hair.params.sim_drag;
    set->params.thickness            = hair.params.sim_collision_thickness;
    set->params.friction             = hair.params.sim_friction;
    set->params.gravity_scale        = hair.params.sim_gravity;
    // The weather's wind, the same value WindSystem writes onto a cloth -- so hair and a cape in
    // one gust agree instead of each inventing their own.
    set->params.wind = hair.wind * hair.weather_wind;
}

std::size_t HairSystem::simulated_guide_count(const std::string& key) const {
    const auto it = grooms_.find(key);
    return it == grooms_.end() ? 0u : it->second.simulated_guides;
}

void HairSystem::upload(Scene& scene, std::uint32_t frame_slot) {
    auto* world = physics_world_(scene);
    if (world == nullptr) return;

    for (auto& [key, groom] : grooms_) {
        if (!groom.built || groom.mesh == nullptr || !groom.params.simulate) continue;
        if (groom.vertices.empty() || groom.guides_per_strand == 0) continue;
        const auto* set = world->get_strands(groom.strands);
        if (set == nullptr || set->strands.empty()) continue;

        const std::size_t points = static_cast<std::size_t>(groom.set.points_per_strand);
        const std::size_t strand_count = groom.set.strand_count();
        const std::uint32_t k_guides = groom.guides_per_strand;

        // Each render strand is the weighted blend of its guides' LIVE shapes, plus the offset it
        // had from that blend at bind time -- back in the scalp's local space, which is the space
        // the mesh was built in and the space the child object's identity transform expects.
        for (std::size_t i = 0; i < strand_count; ++i) {
            glm::vec3* mine = groom.set.strand(i);
            for (std::size_t k = 0; k < points; ++k) {
                glm::vec3 blended(0.0f);
                for (std::uint32_t n = 0; n < k_guides; ++n) {
                    const std::uint32_t g = groom.guide_index[i * k_guides + n];
                    const float w = groom.guide_weights[i * k_guides + n];
                    if (w <= 0.0f || g >= set->strands.size()) continue;
                    const coopa::physx::hair::Strand& strand = set->strands[g];
                    if (strand.count == 0) continue;
                    const std::size_t pi =
                        strand.first + std::min<std::size_t>(k, strand.count - 1u);
                    blended += set->particles[pi].position * w;
                }
                mine[k] = glm::vec3(groom.to_local * glm::vec4(blended, 1.0f)) +
                          groom.bind_offsets[i * points + k];
            }
            write_strand_vertices(groom.set, groom.params, i, groom.vertices);
        }
        groom.mesh->update_vertices(groom.vertices.data(), groom.vertices.size(), frame_slot);
    }
}

void HairSystem::prune_(Scene& scene, const std::unordered_set<std::string>& live) {
    for (auto it = grooms_.begin(); it != grooms_.end();) {
        if (live.count(it->first) != 0) {
            ++it;
            continue;
        }
        // Hand the guides back before the groom's record goes, or the world would solve strands
        // for hair nothing draws any more.
        release_strands_(it->second, scene);
        it = grooms_.erase(it);
    }

    // A deleted Hair component (or a renamed object) leaves its generated child behind. Collect
    // the orphans by walking the scene for our own marker -- never from a pointer this map held,
    // which is the mistake that made procgraph's published root a use-after-free -- then detach in
    // a second pass, since the first is iterating the very children it would remove.
    std::vector<SceneObject*> orphans;
    for (auto& root : scene.root_objects()) {
        if (root == nullptr) continue;
        root->for_each_recursive([&](SceneObject& object) {
            if (object.name() != Hair::child_object_name()) return;
            const auto* marker = object.get_component<toy::scene::RuntimeObject>();
            if (marker == nullptr || marker->owner_system != "Hair") return;
            SceneObject* parent = object.parent();
            if (parent == nullptr || parent->get_component<Hair>() == nullptr) orphans.push_back(&object);
        });
    }
    for (SceneObject* orphan : orphans) {
        if (SceneObject* parent = orphan->parent()) {
            parent->detach_child(orphan);  // destroyed as the returned unique_ptr drops
        }
    }
}

void install_hair_system(Scene& scene, coopa::gfx::core::Device& device,
                         coopa::gfx::memory::Allocator& allocator, coopa::asset::AssetManager& assets,
                         std::uint32_t frames_in_flight) {
    scene.add_system(std::make_unique<HairSystem>(device, allocator, assets, frames_in_flight),
                     k_hair_system_order);
}

} // namespace hair
} // namespace toy
