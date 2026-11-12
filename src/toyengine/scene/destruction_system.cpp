#include <toyengine/scene/destruction_system.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>

#include <coopa/asset/asset_handle.h>
#include <coopa/asset/asset_manager.h>
#include <coopa/yaml/document.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <gfxcoopa/engine/components/register.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <physxcoopa/components/box_collider.h>
#include <physxcoopa/components/convex_collider.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/damage/damage_material.h>
#include <physxcoopa/damage/damage_resolve.h>
#include <physxcoopa/dynamics/physics_material.h>
#include <physxcoopa/geometry/convex_hull.h>
#include <physxcoopa/system/physics_system.h>

#include <toyengine/destruct/dent.h>
#include <toyengine/destruct/slice.h>
#include <toyengine/scene/damage_cursor.h>
#include <toyengine/scene/damageable.h>
#include <toyengine/scene/destructible.h>
#include <toyengine/scene/runtime_object.h>
#include <toyengine/scene/sliceable.h>

namespace toy {
namespace scene {

namespace {

using coopa::gfx::engine::components::MeshRenderer;
using coopa::gfx::engine::components::PBRMaterial;
using coopa::gfx::engine::data::Mesh;
using coopa::gfx::engine::data::MeshCpuData;
using coopa::gfx::engine::data::MeshLod;
using coopa::gfx::engine::data::MeshPart;
using coopa::gfx::engine::data::SkinnedMeshSource;
using coopa::scene::TransformComponent;

/// Fracture jobs are their own type so the engine's diagnostics can tell them apart.
constexpr coopa::job::JobType k_fracture_job_type = 7;

/// Below this the chunk is a crumb: too small to read, not worth a body.
constexpr float k_min_chunk_volume_fraction = 1e-4f;

/// A dented hero mesh above this is refused rather than silently costing ~2 * 48 bytes per
/// vertex of GPU memory (200000 * 96 B =~ 19 MB) -- the blow still costs HP, it just leaves the
/// mesh alone.
constexpr std::size_t k_max_dent_vertices = 200000;

/// Default target mean world edge for a dent clone's subdivision, metres -- Damageable::
/// dent_resolution's 0 means this.
constexpr float k_default_dent_resolution = 0.05f;

/// How much of a mesh's own shortest world extent one vertex may be pushed in, when
/// Damageable::max_dent's 0 leaves it to derive one -- enough that a 0.3 m-thick plate dents
/// visibly without being crushed through itself.
constexpr float k_default_max_dent_fraction = 0.35f;

/** @brief Parses a DamageCursor::damage_type name; an unknown or absent value falls back to
 *         Impact rather than throwing, same policy as the scene YAML parsers use. */
coopa::physx::damage::DamageType parse_damage_type_name_(const std::string& name) {
    using coopa::physx::damage::DamageType;
    if (name == "Slash") return DamageType::Slash;
    if (name == "Pierce") return DamageType::Pierce;
    if (name == "Explosive") return DamageType::Explosive;
    if (name == "Fire") return DamageType::Fire;
    return DamageType::Impact;
}

} // namespace

DestructionSystem::~DestructionSystem() {
    // Same contract as TerrainComponent's destructor: a fracture job holds a shared_ptr to its
    // own output, but the handles it was given must be closed or the JobEngine reports leaked
    // slots at shutdown.
    if (jobs_ == nullptr) return;
    for (auto& [key, fracture] : cache_) {
        if (!fracture.job.is_valid()) continue;
        jobs_->wait_for(fracture.job);
        fracture.job.close();
    }
}

std::string DestructionSystem::cache_key_(const std::string& mesh_path, const Destructible& d) {
    return mesh_path + "|" + std::to_string(d.pieces) + "|" + std::to_string(d.seed) + "|" +
           std::to_string(static_cast<int>(d.irregularity * 100.0f)) + "|" +
           std::to_string(static_cast<int>(d.collider_fill * 100.0f)) + "|" +
           std::to_string(d.max_boxes);
}

bool DestructionSystem::ensure_fracture_(Destructible& d, const std::string& mesh_path, Fracture*& out) {
    const std::string key = cache_key_(mesh_path, d);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        Fracture& f = it->second;
        if (!f.ready && f.job.is_valid() && f.job.is_complete()) {
            f.job.close();
            f.job = coopa::job::JobHandle{};
            f.ready = true;
        }
        out = &f;
        return f.ready;
    }

    // The geometry is read from the mesh FILE rather than through the asset manager: the same
    // file is already loaded as a GPU Mesh for the renderer, and one file cannot be two asset
    // types at once ("refusing to reinterpret its payload"). SkinnedMeshSource::from_node() is
    // the CPU-side parse of exactly that format, so this is the same vertices the renderer
    // draws, read once and cached in the fracture.
    const std::string file = assets_.source().resolve("meshes/" + mesh_path + ".yaml");
    SkinnedMeshSource src;
    try {
        src = SkinnedMeshSource::from_node(coopa::yaml::load_document(file));
    } catch (const std::exception& e) {
        // Cached as an empty, ready fracture: a mesh that cannot be read must not be re-read
        // every frame for the life of the scene.
        Fracture dead;
        dead.pieces = std::make_shared<std::vector<destruct::FracturePiece>>();
        dead.ready = true;
        std::fprintf(stderr, "[destruction] cannot read '%s' to fracture it: %s\n", file.c_str(), e.what());
        out = &cache_.emplace(key, std::move(dead)).first->second;
        return true;
    }

    Fracture fracture;
    fracture.pieces = std::make_shared<std::vector<destruct::FracturePiece>>();
    destruct::FractureSettings settings;
    settings.pieces = std::max(1, d.pieces);
    settings.seed = d.seed;
    settings.irregularity = d.irregularity;
    settings.collider_fill = d.collider_fill;
    settings.max_boxes = std::max(1, d.max_boxes);

    auto pieces = fracture.pieces;
    // Copied, not borrowed: the job outlives this call and the asset slot may be reloaded.
    auto vertices = std::make_shared<std::vector<destruct::Vertex>>(src.vertices);
    auto indices = std::make_shared<std::vector<std::uint32_t>>(src.indices);
    if (jobs_ != nullptr) {
        fracture.job = jobs_->create_handle();
        jobs_->submit([pieces, vertices, indices, settings]() {
            *pieces = destruct::fracture_mesh(*vertices, *indices, settings);
        }, k_fracture_job_type, fracture.job, nullptr, 0u, coopa::job::Priority::Low);
    } else {
        *pieces = destruct::fracture_mesh(*vertices, *indices, settings);
        fracture.ready = true;
    }
    out = &cache_.emplace(key, std::move(fracture)).first->second;
    return out->ready;
}

int DestructionSystem::spawn_chunks_(coopa::scene::Scene& scene, Destructible& d, const Fracture& fracture) {
    coopa::scene::SceneObject* owner = d.owner;
    if (owner == nullptr || fracture.pieces == nullptr) return 0;
    if (owner->get_transform() == nullptr) return 0;

    const glm::vec3 centre = glm::vec3(owner->get_transform()->transform().get_world_matrix()[3]);

    PieceStyle style;
    style.density = d.chunk_density;
    style.physics_material = d.chunk_physics_material;
    style.interior_material = d.interior_material;
    style.lifetime = d.chunk_lifetime;
    style.fade = d.chunk_fade;
    style.use_hull = d.collider != "boxes";
    style.collider_fill = d.collider_fill;
    style.max_boxes = d.max_boxes;
    style.hit_point = glm::length(d.hit_point()) > 0.0f ? d.hit_point() : centre;
    style.hit_velocity = d.hit_impulse() * d.impact_transfer;
    style.scatter = d.scatter;

    const std::vector<destruct::FracturePiece>& pieces = *fracture.pieces;
    float largest = 0.0f;
    for (const destruct::FracturePiece& p : pieces) largest = std::max(largest, p.volume);

    const int wanted = d.max_chunks > 0 ? std::min<int>(d.max_chunks, static_cast<int>(pieces.size()))
                                        : static_cast<int>(pieces.size());
    int spawned = 0;
    for (int i = 0; i < wanted; ++i) {
        const destruct::FracturePiece& piece = pieces[static_cast<std::size_t>(i)];
        if (piece.volume < largest * k_min_chunk_volume_fraction) continue;

        // Copied, not moved: `pieces` is the shared cache for this mesh + settings, read again
        // by every other Destructible that breaks the same way.
        PieceSpawn spawn;
        spawn.vertices = piece.vertices;
        spawn.indices = piece.indices;
        spawn.cap_first_index = piece.cap_first_index;
        spawn.centre = piece.centre;
        spawn.orientation = piece.orientation;
        spawn.half_extent = piece.half_extent;
        spawn.volume = piece.volume;
        spawn.boxes = piece.boxes;

        if (spawn_piece_(scene, *owner, spawn, style) != nullptr) ++spawned;
    }

    if (std::getenv("DESTRUCT_DEBUG") != nullptr) {
        std::fprintf(stderr, "[destruct] %s broke into %d chunks\n", owner->name().c_str(), spawned);
    }

    // Gone, not deleted: a name, a script or a selection still resolves to the object, and the
    // chunks are what is drawn from here on.
    owner->set_active(false);
    return spawned;
}

coopa::scene::SceneObject* DestructionSystem::spawn_piece_(coopa::scene::Scene& scene, coopa::scene::SceneObject& owner,
                                                            const PieceSpawn& piece, const PieceStyle& style) {
    auto* owner_tc = owner.get_transform();
    if (owner_tc == nullptr) return nullptr;

    const glm::mat4 world = owner_tc->transform().get_world_matrix();
    const glm::mat3 basis(world);
    const auto* renderer = owner.get_component<MeshRenderer>();
    const glm::mat3 owner_rotation_mat(glm::normalize(basis[0]), glm::normalize(basis[1]), glm::normalize(basis[2]));
    const glm::quat owner_rotation = glm::quat_cast(owner_rotation_mat);
    const glm::vec3 scale(glm::length(basis[0]), glm::length(basis[1]), glm::length(basis[2]));

    // A TRS Transform can only express rotation*scale, never the general rotation*scale*rotation
    // a piece in its OWN principal frame needs under a non-uniformly scaled owner (world_vertex =
    // R_owner * S * R_piece * v -- scale sits BETWEEN the two rotations, and does not commute
    // past R_piece unless S is uniform). So under a non-uniform scale, S and R_piece are baked
    // straight into a local copy of the piece's geometry instead of being split across the
    // chunk's own Transform -- every uniformly-scaled Destructible/Sliceable in the repo today
    // takes the untouched fast path below, unchanged in cost or result.
    const float max_scale = std::max({scale.x, scale.y, scale.z});
    const float min_scale = std::min({scale.x, scale.y, scale.z});
    const bool uniform_scale = min_scale > 1e-6f && max_scale / min_scale < 1.001f;

    std::vector<destruct::Vertex> vertices;
    glm::vec3 half_extent = piece.half_extent;
    std::vector<destruct::FractureBox> boxes = piece.boxes;
    float volume = piece.volume * scale.x * scale.y * scale.z;
    glm::quat chunk_rotation = owner_rotation * piece.orientation;
    glm::vec3 chunk_scale = scale;
    glm::vec3 position_offset(0.0f);

    if (uniform_scale) {
        vertices = piece.vertices;
    } else {
        const glm::mat3 piece_rotation = glm::mat3_cast(piece.orientation);
        vertices.reserve(piece.vertices.size());
        for (const destruct::Vertex& v : piece.vertices) {
            destruct::Vertex baked = v;
            baked.position = scale * (piece_rotation * v.position);
            const glm::vec3 n = (piece_rotation * v.normal) / scale; // inverse-transpose of a diagonal S
            const float n_len = glm::length(n);
            baked.normal = n_len > 1e-8f ? n / n_len : v.normal;
            const glm::vec3 t = scale * (piece_rotation * glm::vec3(v.tangent));
            const float t_len = glm::length(t);
            baked.tangent = t_len > 1e-8f ? glm::vec4(t / t_len, v.tangent.w) : v.tangent;
            vertices.push_back(baked);
        }

        // Re-centre on the BAKED shape's own bounds -- not necessarily the origin any more, since
        // rotating then non-uniformly scaling an off-centre piece can shift its true middle -- so
        // the chunk spins about its own centre instead of orbiting an arbitrary point (same
        // reasoning mesh_solid.h's centre_in_frame() gives for why a piece is centred at all).
        glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
        for (const destruct::Vertex& v : vertices) {
            lo = glm::min(lo, v.position);
            hi = glm::max(hi, v.position);
        }
        const glm::vec3 centre_offset = (lo + hi) * 0.5f;
        for (destruct::Vertex& v : vertices) v.position -= centre_offset;
        half_extent = (hi - lo) * 0.5f;
        boxes.clear(); // the fracture-time boxes are fit to the stale, unbaked piece frame
        volume = destruct::mesh_volume(vertices, piece.indices);
        chunk_rotation = owner_rotation; // baked vertices are already in owner-local (unrotated-by-piece) axes
        chunk_scale = glm::vec3(1.0f);   // ...and already scaled
        position_offset = owner_rotation_mat * centre_offset;
    }

    // "chunk_" rather than "piece_": a fracture chunk and a slice half are both spawned here,
    // and existing scene code/tests already walk root objects by this prefix to find debris.
    auto object = std::make_unique<coopa::scene::SceneObject>("chunk_" + std::to_string(spawned_));
    object->add_component<RuntimeObject>("Destruction", "a piece of " + owner.name());

    // The piece's own space, placed where that space sits in the world: its box centre under
    // the owner's transform, turned by the owner's rotation AND by the piece's own principal
    // frame (toyengine/destruct/mesh_solid.h) -- the frame its collider is measured in.
    auto* tc = object->add_component<TransformComponent>();
    tc->transform().set_position(glm::vec3(world * glm::vec4(piece.centre, 1.0f)) + position_offset);
    tc->transform().set_rotation_quat(chunk_rotation);
    tc->transform().set_scale(chunk_scale);

    auto* mesh_renderer = object->add_component<MeshRenderer>();
    if (renderer != nullptr) mesh_renderer->material = renderer->material;

    // A second material slot for the cut face, only when the piece actually has one and the
    // caller asked for one -- otherwise a single, unsplit part, same as every other chunk mesh.
    const bool has_cap = piece.cap_first_index < static_cast<std::uint32_t>(piece.indices.size());
    std::vector<MeshLod> lods;
    if (has_cap && !style.interior_material.empty()) {
        MeshLod lod;
        lod.first_index = 0;
        lod.index_count = static_cast<std::uint32_t>(piece.indices.size());
        lod.parts = {MeshPart{0u, piece.cap_first_index},
                     MeshPart{piece.cap_first_index, lod.index_count - piece.cap_first_index}};
        lods.push_back(lod);
    }
    auto gpu = std::make_shared<Mesh>(Mesh::from_arrays(device_, allocator_, vertices, piece.indices, 1,
                                                         false, lods.empty() ? nullptr : &lods));
    mesh_renderer->set_mesh(assets_.create<Mesh>("chunk:" + std::to_string(spawned_), gpu));
    if (!lods.empty()) {
        PBRMaterial interior = renderer != nullptr ? renderer->material : PBRMaterial{};
        coopa::scene::SceneLoader::ParseContext ctx; // asset-root resolution only; no scene dir at runtime
        coopa::gfx::engine::components::parse_material_value_(fkyaml::node(style.interior_material), interior,
                                                               assets_, ctx);
        mesh_renderer->material_for_mut(1) = interior;
    }

    coopa::asset::AssetHandle<coopa::physx::dynamics::PhysicsMaterial> material;
    if (!style.physics_material.empty()) {
        material = assets_.load<coopa::physx::dynamics::PhysicsMaterial>(
            "physics_materials/" + style.physics_material + ".yaml");
    }

    // The collider: a convex hull built from the piece's own vertices by default (physxcoopa's
    // ConvexCollider), since a hull fits a shard far closer than any box compound. Falls back
    // to the box path -- the only shape this had before physxcoopa gained ShapeType::ConvexHull
    // -- for an explicit `collider: boxes`, or when the hull builder degenerates (a near-flat
    // sliver: too few usable vertices, or all of them collinear/coplanar).
    std::shared_ptr<coopa::physx::geometry::ConvexHull> hull;
    if (style.use_hull) {
        std::vector<glm::vec3> positions;
        positions.reserve(vertices.size());
        for (const destruct::Vertex& v : vertices) positions.push_back(v.position);
        auto built = std::make_shared<coopa::physx::geometry::ConvexHull>();
        if (coopa::physx::geometry::build_convex_hull(positions.data(), positions.size(), *built)) {
            hull = built;
        }
    }
    if (hull != nullptr) {
        auto* collider = object->add_component<coopa::physx::components::ConvexCollider>();
        collider->set_hull(hull);
        if (material.is_valid()) collider->set_material_asset(material);
    } else {
        if (boxes.empty()) {
            boxes = destruct::fit_boxes(vertices, volume, half_extent, glm::clamp(style.collider_fill, 0.0f, 1.0f),
                                        std::max(1, style.max_boxes));
        }
        if (boxes.size() <= 1) {
            const glm::vec3 half = boxes.empty() ? half_extent : boxes.front().half_extent;
            const glm::vec3 box_centre = boxes.empty() ? glm::vec3(0.0f) : boxes.front().centre;
            auto* collider = object->add_component<coopa::physx::components::BoxCollider>();
            collider->set_size(glm::max(half * 2.0f, glm::vec3(1e-3f)));
            collider->set_center(box_centre);
            if (material.is_valid()) collider->set_material_asset(material);
        } else {
            for (std::size_t b = 0; b < boxes.size(); ++b) {
                auto part = std::make_unique<coopa::scene::SceneObject>("box_" + std::to_string(b));
                auto* part_tc = part->add_component<TransformComponent>();
                part_tc->transform().set_position(boxes[b].centre);
                // add_child() links ownership only -- a child's Transform must be parented by
                // hand (see SceneObject::add_child). Without this the box's world matrix IS its
                // local one, and PhysicsSystem, which places a compound's shapes from
                // world_child - world_root, would hang every shape a whole piece position away
                // from the body it belongs to: the piece then falls through the floor its
                // collision is standing on, somewhere near the origin.
                part_tc->set_parent_transform(&tc->transform());
                auto* collider = part->add_component<coopa::physx::components::BoxCollider>();
                collider->set_size(glm::max(boxes[b].half_extent * 2.0f, glm::vec3(1e-3f)));
                if (material.is_valid()) collider->set_material_asset(material);
                object->add_child(std::move(part));
            }
        }
    }

    auto* body = object->add_component<coopa::physx::components::RigidbodyComponent>();
    body->mass = std::max(0.02f, volume * std::max(1.0f, style.density));

    // The outward push, plus any inherited motion: set on the creation-time-only initial_*
    // fields (see this method's doc) rather than add_impulse(), since the body is not bound
    // into PhysicsWorld until PhysicsSystem's next reconcile.
    glm::vec3 away = tc->transform().position() - style.hit_point;
    const float len = glm::length(away);
    away = len > 1e-4f ? away / len : glm::vec3(0.0f, 0.0f, 1.0f);
    body->initial_velocity = style.hit_velocity + away * style.scatter;
    body->initial_angular_velocity = style.initial_angular_velocity;

    coopa::scene::SceneObject* raw = scene.add_root_object(std::move(object));
    scene.adopt(*raw);

    Chunk chunk;
    chunk.object = raw;
    chunk.lifetime = style.lifetime;
    chunk.fade = style.fade;
    chunks_.push_back(chunk);

    physics_dirty_ = true;
    ++spawned_;
    return raw;
}

int DestructionSystem::slice_object(coopa::scene::Scene& scene, coopa::scene::SceneObject& object,
                                    const glm::vec3& world_point, const glm::vec3& world_normal,
                                    float separation_speed) {
    auto* tc = object.get_transform();
    const auto* renderer = object.get_component<MeshRenderer>();
    if (tc == nullptr || renderer == nullptr) return 0;

    Sliceable* sliceable = object.get_component<Sliceable>();
    Destructible* destructible = object.get_component<Destructible>();
    if (sliceable == nullptr && destructible == nullptr) return 0;
    const int remaining = sliceable != nullptr ? sliceable->max_slices : destructible->max_slices;
    if (remaining <= 0) return 0;

    // The vertices to cut: an authored object's mesh ASSET is read from its file (the same
    // reason ensure_fracture_() does -- a loaded Mesh is GPU-only, its CPU vertices discarded
    // after upload), but a half spawned by an earlier slice has no such file, so it carries its
    // own local-frame copy on `Sliceable::cached_*` instead (see that field's doc).
    std::vector<destruct::Vertex> src_vertices;
    std::vector<std::uint32_t> src_indices;
    if (sliceable != nullptr && !sliceable->cached_vertices.empty()) {
        src_vertices = sliceable->cached_vertices;
        src_indices = sliceable->cached_indices;
    } else if (!renderer->mesh_path().empty()) {
        const std::string file = assets_.source().resolve("meshes/" + renderer->mesh_path() + ".yaml");
        SkinnedMeshSource src;
        try {
            src = SkinnedMeshSource::from_node(coopa::yaml::load_document(file));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[destruct] cannot read '%s' to slice it: %s\n", file.c_str(), e.what());
            return 0;
        }
        src_vertices = std::move(src.vertices);
        src_indices = std::move(src.indices);
    } else {
        return 0; // neither a cached half nor a mesh asset to read -- nothing to cut
    }

    // The plane into the object's local space. Points go through the plain inverse; a normal
    // does not transform the same way a point does (the standard rule: if points transform by
    // a matrix A, a normal transforms by (A^-1)^T). Points here go world->local via `inv_world`
    // (A = inv_world), so the normal's A^-1 is `world` itself -- transpose(world), NOT
    // transpose(inv_world) (which is instead the matrix the LOCAL->WORLD direction wants, e.g.
    // this same method's own local_normal -> world_normal step below). Getting this backwards
    // is invisible under a uniform scale with no rotation -- both matrices are then scalar
    // multiples of each other and normalize() hides it -- which is exactly why it must be
    // reasoned from the rule, not eyeballed against the demo scene's own crate.
    const glm::mat4 world = tc->transform().get_world_matrix();
    const glm::mat4 inv_world = glm::inverse(world);
    const glm::vec3 local_point = glm::vec3(inv_world * glm::vec4(world_point, 1.0f));
    const glm::vec3 local_normal = glm::normalize(
        glm::vec3(glm::transpose(world) * glm::vec4(world_normal, 0.0f)));
    const float local_distance = glm::dot(local_normal, local_point);

    destruct::SlicePiece neg, pos;
    if (!destruct::slice_mesh(src_vertices, src_indices, local_normal, local_distance,
                              destruct::SliceSettings{}, neg, pos)) {
        return 0;
    }

    PieceStyle style;
    if (sliceable != nullptr) {
        style.density = sliceable->density;
        style.physics_material = sliceable->physics_material;
        style.interior_material = sliceable->interior_material;
        style.lifetime = sliceable->piece_lifetime;
        style.fade = sliceable->piece_fade;
        style.use_hull = sliceable->collider != "boxes";
        style.collider_fill = sliceable->collider_fill;
        style.max_boxes = sliceable->max_boxes;
    } else {
        style.density = destructible->chunk_density;
        style.physics_material = destructible->chunk_physics_material;
        style.interior_material = destructible->interior_material;
        style.lifetime = destructible->chunk_lifetime;
        style.fade = destructible->chunk_fade;
        style.use_hull = destructible->collider != "boxes";
        style.collider_fill = destructible->collider_fill;
        style.max_boxes = destructible->max_boxes;
    }
    // The halves inherit the original's own motion -- a cut object thrown mid-air stays
    // thrown -- plus the separation push along the plane normal so they part rather than
    // interpenetrate.
    glm::vec3 inherited_velocity(0.0f), inherited_angular(0.0f);
    if (auto* rb = object.get_component<coopa::physx::components::RigidbodyComponent>()) {
        inherited_velocity = rb->velocity();
        inherited_angular = rb->angular_velocity();
    }
    style.hit_point = world_point; // only matters if scatter != 0, which it is not here
    style.scatter = 0.0f;
    style.initial_angular_velocity = inherited_angular;

    const auto to_spawn = [](const destruct::SlicePiece& p) {
        PieceSpawn s;
        s.vertices = p.vertices;
        s.indices = p.indices;
        s.cap_first_index = p.cap_first_index;
        s.centre = p.centre;
        s.orientation = p.orientation;
        s.half_extent = p.half_extent;
        s.volume = p.volume;
        return s;
    };

    PieceStyle neg_style = style;
    neg_style.hit_velocity = inherited_velocity - world_normal * separation_speed;
    PieceStyle pos_style = style;
    pos_style.hit_velocity = inherited_velocity + world_normal * separation_speed;

    coopa::scene::SceneObject* neg_obj = spawn_piece_(scene, object, to_spawn(neg), neg_style);
    coopa::scene::SceneObject* pos_obj = spawn_piece_(scene, object, to_spawn(pos), pos_style);

    int spawned = 0;
    const int next_slices = remaining - 1;
    const std::pair<coopa::scene::SceneObject*, const destruct::SlicePiece*> halves[2] = {
        {neg_obj, &neg}, {pos_obj, &pos}};
    for (const auto& [half, piece] : halves) {
        if (half == nullptr) continue;
        ++spawned;
        if (next_slices <= 0) continue;
        auto* half_sliceable = half->add_component<Sliceable>();
        half_sliceable->interior_material = style.interior_material;
        half_sliceable->density = style.density;
        half_sliceable->piece_lifetime = style.lifetime;
        half_sliceable->piece_fade = style.fade;
        half_sliceable->physics_material = style.physics_material;
        half_sliceable->collider = style.use_hull ? "hull" : "boxes";
        half_sliceable->collider_fill = style.collider_fill;
        half_sliceable->max_boxes = style.max_boxes;
        half_sliceable->max_slices = next_slices;
        // This half has no mesh asset of its own -- stash the exact local-frame vertices its
        // GPU mesh was built from, so a future slice_object() call on it has something to cut.
        half_sliceable->cached_vertices = piece->vertices;
        half_sliceable->cached_indices = piece->indices;
    }

    if (std::getenv("DESTRUCT_DEBUG") != nullptr) {
        std::fprintf(stderr, "[destruct] %s sliced into %d halves\n", object.name().c_str(), spawned);
    }

    // Gone, not deleted: same reasoning as a fracture break.
    object.set_active(false);
    return spawned;
}

void DestructionSystem::upload_dents(std::uint32_t frame_slot) {
    for (auto& entry : dents_) {
        DentedMesh& dented = entry.second;
        if (dented.mesh) dented.mesh->update_vertices(dented.vertices.data(), dented.vertices.size(), frame_slot);
    }
}

DestructionSystem::DentedMesh* DestructionSystem::ensure_dent_mesh_(Damageable& damageable) {
    const auto existing = dents_.find(&damageable);
    if (existing != dents_.end()) return &existing->second;

    coopa::scene::SceneObject* owner = damageable.owner;
    if (owner == nullptr) return nullptr;
    auto* renderer = owner->get_component<MeshRenderer>();
    auto* tc = owner->get_transform();
    if (renderer == nullptr || tc == nullptr || renderer->mesh_path().empty()) return nullptr;

    // Read from the mesh FILE, not the asset manager -- the same reason ensure_fracture_() and
    // slice_object() do: this same file is already loaded as a GPU Mesh for the renderer, and
    // one file cannot be two asset types at once. build_cpu() (not SkinnedMeshSource::from_node,
    // which the fracture/slice paths use) because it welds -- the layout this clone's vertices
    // need to match is the GPU mesh's own, not the file's raw per-corner one.
    const std::string file = assets_.source().resolve("meshes/" + renderer->mesh_path() + ".yaml");
    // Suppresses the file's own `lods:`: a dent clone is one LOD for its whole life (topology is
    // fixed once built), and without this an LOD level would be appended behind LOD 0 in the
    // same index buffer, so subdividing or denting would reach into geometry a different screen
    // size was meant to show.
    const fkyaml::node no_lods = fkyaml::node::mapping();
    MeshCpuData cpu;
    try {
        cpu = Mesh::build_cpu(coopa::yaml::load_document(file), &no_lods);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[destruct] cannot read '%s' to dent it: %s\n", file.c_str(), e.what());
        return nullptr;
    }
    if (cpu.vertices.size() > k_max_dent_vertices) {
        std::fprintf(stderr, "[destruct] '%s' has %zu vertices, past the %zu-vertex dent limit; "
                              "this blow costs HP but leaves its mesh alone\n",
                     file.c_str(), cpu.vertices.size(), k_max_dent_vertices);
        return nullptr;
    }

    const glm::mat4 world = tc->transform().get_world_matrix();
    const glm::mat3 basis(world);
    const glm::vec3 axis_lengths(glm::length(basis[0]), glm::length(basis[1]), glm::length(basis[2]));
    const float world_scale = std::max({axis_lengths.x, axis_lengths.y, axis_lengths.z});
    const float target_edge = damageable.dent_resolution > 0.0f ? damageable.dent_resolution
                                                                 : k_default_dent_resolution;
    destruct::subdivide_for_dent(cpu.vertices, cpu.indices, target_edge, world_scale, /*max_levels=*/4,
                                 k_max_dent_vertices);

    DentedMesh dented;
    dented.vertices = std::move(cpu.vertices);
    dented.indices = std::move(cpu.indices);
    dented.state.build(dented.vertices, dented.indices);

    // The CAP, not the current shape: computed once, from the clone's UNDENTED bounds, so it
    // does not shrink as the mesh itself does -- see the field's own doc.
    const glm::vec3 object_extent = cpu.bounds_max - cpu.bounds_min;
    const glm::vec3 world_extent = object_extent * axis_lengths;
    const float min_extent = std::min({world_extent.x, world_extent.y, world_extent.z});
    dented.default_max_offset = k_default_max_dent_fraction * std::max(0.0f, min_extent);

    dented.mesh = std::make_shared<Mesh>(
        Mesh::from_arrays(device_, allocator_, dented.vertices, dented.indices, frames_in_flight_));
    // A monotonic suffix, unlike ClothRenderer's bare owner name: two objects that happen to
    // share a name must not end up sharing one dent clone.
    const std::string asset_id = "runtime/dent/" + owner->name() + "/" + std::to_string(next_dent_id_++);
    renderer->set_mesh(assets_.create<Mesh>(asset_id, dented.mesh));

    return &dents_.emplace(&damageable, std::move(dented)).first->second;
}

void DestructionSystem::apply_dent_(Damageable& damageable, const coopa::physx::damage::DamageEvent& world_blow,
                                    const coopa::physx::damage::ResolvedDamage& resolved, const glm::mat4& world,
                                    const glm::mat4& inv_world) {
    if (!damageable.dent) return;
    DentedMesh* dented = ensure_dent_mesh_(damageable);
    if (dented == nullptr) return;

    // resolve_damage()'s radius/depth are sized in the SAME space DamageField already consumes
    // them in -- the object's own local space, agnostic to its world scale. A DentBlow is
    // deliberately a WORLD-space footprint (apply_dent()'s own doc: that is what keeps it round
    // under a non-uniform scale), so both are scaled up by the object's own world scale here,
    // uniformly -- the true non-uniform shape is still honoured per vertex, inside apply_dent().
    const glm::mat3 basis(world);
    const float world_scale = std::max({glm::length(basis[0]), glm::length(basis[1]), glm::length(basis[2])});

    float radius_local = resolved.radius;
    if (resolved.response == coopa::physx::damage::DamageResponse::Chip) {
        // A Slash/Pierce chip's footprint (resolve_damage.h: 0.015 m / 0.008 m) can be smaller
        // than this mesh's own triangles, in which case it would touch zero or one vertex --
        // floor it so a chip reads as a narrow gouge, not nothing.
        radius_local = std::max(radius_local, 2.0f * dented->state.mean_edge);
    }

    destruct::DentBlow blow;
    blow.point = world_blow.point;
    blow.direction = world_blow.direction;
    blow.radius = radius_local * world_scale;
    blow.depth = resolved.depth * world_scale;
    blow.max_offset = damageable.max_dent > 0.0f ? damageable.max_dent : dented->default_max_offset;

    destruct::apply_dent(dented->state, dented->vertices, dented->indices, blow, world, inv_world);
}

void DestructionSystem::process_damage_(coopa::scene::Scene& scene, Damageable& damageable) {
    if (damageable.queued().empty()) return;
    coopa::scene::SceneObject* owner = damageable.owner;
    auto* tc = owner != nullptr ? owner->get_transform() : nullptr;
    if (tc == nullptr) { damageable.clear_queue(); return; }

    using coopa::physx::damage::DamageEvent;
    using coopa::physx::damage::DamageMaterial;
    using coopa::physx::damage::DamageResponse;
    using coopa::physx::damage::ResolvedDamage;
    using coopa::physx::damage::resolve_damage;

    DamageMaterial material = DamageMaterial::default_material();
    if (!damageable.material.empty()) {
        auto handle = assets_.load<DamageMaterial>("damage_materials/" + damageable.material + ".yaml");
        if (handle.is_loaded()) material = *handle.get();
    }
    damageable.start_health(damageable.hit_points > 0.0f ? damageable.hit_points : material.hit_points);

    if (!damageable.field_ready()) {
        glm::vec3 lo(-0.5f), hi(0.5f);
        if (const auto* renderer = owner->get_component<MeshRenderer>()) {
            if (renderer->is_ready()) {
                lo = renderer->get_mesh()->bounds_min();
                hi = renderer->get_mesh()->bounds_max();
            }
        }
        damageable.field_mut().reset(lo, hi, damageable.damage_resolution);
        damageable.mark_field_ready();
    }

    const glm::mat4 world = tc->transform().get_world_matrix();
    const glm::mat4 inv_world = glm::inverse(world);

    // Copied out, not iterated by reference: a Fracture/Cleave response below can deactivate
    // `owner` (and, via slice_object(), read a DIFFERENT Damageable's cached vertices entirely
    // for the next call), so the loop must not depend on `damageable`'s own state surviving it.
    const std::vector<DamageEvent> blows = damageable.queued();
    damageable.clear_queue();

    for (const DamageEvent& blow : blows) {
        if (!owner->active()) break; // sliced or fractured away by an earlier blow this pass

        // Into the object's local space. Points go through the plain inverse; a direction/edge
        // (no translation) goes through the inverse-transpose -- see slice_object()'s own doc
        // for the rule and why getting it backwards is invisible under a uniform scale with no
        // rotation.
        DamageEvent local = blow;
        local.point = glm::vec3(inv_world * glm::vec4(blow.point, 1.0f));
        local.direction = glm::length(blow.direction) > 1e-6f
                              ? glm::normalize(glm::vec3(glm::transpose(world) * glm::vec4(blow.direction, 0.0f)))
                              : glm::vec3(0.0f);
        local.edge = glm::length(blow.edge) > 1e-6f
                        ? glm::vec3(glm::transpose(world) * glm::vec4(blow.edge, 0.0f))
                        : glm::vec3(0.0f);

        const ResolvedDamage resolved = resolve_damage(local, material);
        damageable.field_mut().deposit(local.point, local.direction, local.edge, resolved);
        damageable.apply_hp_loss(resolved.hp_loss);
        damageable.on_damaged.emit(resolved);

        const bool wants_fracture = resolved.response == DamageResponse::Fracture ||
                                    (damageable.destroyed() && damageable.fracture_on_death);
        if (wants_fracture) {
            if (auto* destructible = owner->get_component<Destructible>()) {
                // shatter()'s impulse is a velocity (m/s), not the blow's own N*s amount; this
                // is a ballpark push in the blow's direction, not a precise conversion -- the
                // chunks' own `scatter` setting is what actually makes the break read as one.
                destructible->shatter(blow.point, blow.direction * resolved.hp_loss * 0.02f);
            }
            continue; // owner may now be pending a break; nothing else to do with this blow
        }

        if (resolved.response == DamageResponse::Cleave && damageable.cleave) {
            glm::vec3 local_normal(0.0f);
            float local_distance = 0.0f, flatness = 0.0f;
            // A single slash isn't a cleave on its own -- only once the field's saturated
            // region reads flat enough (a streak that has cut all the way through, not a
            // scatter of separate nicks) is it trusted as an actual cut plane.
            if (damageable.field().saturated_plane(local_normal, local_distance, flatness) && flatness > 0.6f) {
                const glm::vec3 world_normal = glm::normalize(
                    glm::vec3(glm::transpose(inv_world) * glm::vec4(local_normal, 0.0f)));
                const glm::vec3 world_point = glm::vec3(world * glm::vec4(local_normal * local_distance, 1.0f));
                slice_object(scene, *owner, world_point, world_normal, 0.3f);
            }
        }

        if (resolved.response == DamageResponse::Dent || resolved.response == DamageResponse::Chip) {
            // Both push the mesh's vertices in (see apply_dent_()'s doc); Hole stays HP-and-field
            // only -- punching through is a topology change, which a dynamic Mesh's fixed index
            // buffer cannot express.
            apply_dent_(damageable, blow, resolved, world, inv_world);
        }
    }

    if (damageable.destroyed()) damageable.on_destroyed.emit();
}

void DestructionSystem::process_damage_cursors_(coopa::scene::Scene& scene,
                                                 coopa::physx::system::PhysicsSystem& physics) {
    for (DamageCursor* cursor : scene.get_components<DamageCursor>()) {
        if (cursor == nullptr) continue;
        if (!cursor->slice_requested && !cursor->hit_requested) continue;
        const bool slice = cursor->slice_requested, hit = cursor->hit_requested;
        cursor->slice_requested = false;
        cursor->hit_requested = false;

        const glm::vec3 view_dir = glm::length(cursor->ray_direction) > 1e-6f
                                       ? glm::normalize(cursor->ray_direction)
                                       : glm::vec3(0.0f, 1.0f, 0.0f);
        coopa::physx::system::PhysicsSystem::RaycastHit raycast_hit;
        if (!physics.raycast(coopa::physx::geometry::Ray{cursor->ray_origin, view_dir, cursor->max_distance},
                             raycast_hit) ||
            raycast_hit.object == nullptr) {
            continue; // a miss is the demo's own click, not an error
        }

        if (slice) {
            // A plane through the hit point, roughly ACROSS the view rather than along it (a
            // plane containing the view ray would be edge-on and invisible): build two axes
            // perpendicular to view_dir and pick one at the cursor's own seeded angle, so a cut
            // is always visible and a scripted run of clicks is deterministic.
            const glm::vec3 up_hint = std::abs(view_dir.z) < 0.9f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                                  : glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 right = glm::normalize(glm::cross(view_dir, up_hint));
            const glm::vec3 up = glm::normalize(glm::cross(right, view_dir));
            const float angle = cursor->next_slice_angle();
            const glm::vec3 normal = right * std::cos(angle) + up * std::sin(angle);
            slice_object(scene, *raycast_hit.object, raycast_hit.point, normal, 0.3f);
        }

        if (hit) {
            if (auto* damageable = raycast_hit.object->get_component<Damageable>()) {
                coopa::physx::damage::DamageEvent blow;
                blow.type = parse_damage_type_name_(cursor->damage_type);
                blow.amount = cursor->amount;
                blow.point = raycast_hit.point;
                // Into the SURFACE, not along the view ray: raycast_hit.normal points out of
                // whatever was hit, so -normal is what actually digs a dent in rather than
                // mostly shearing it sideways (a near-tangential view_dir, on a face closer to
                // perpendicular to the camera than to it, wastes most of the push that way).
                blow.direction = glm::length(raycast_hit.normal) > 1e-6f ? -raycast_hit.normal : -view_dir;
                damageable->apply_damage(blow);

                // A physical reaction, synced with the damage dealt: resolve_damage() itself
                // isn't available for another frame (it runs inside process_damage_(), once this
                // queued blow is drained), but the two lines it needs -- load the material,
                // divide by its resistance to this damage type -- are cheap (the asset is
                // already cache-loaded by that same call on every later frame) and give the
                // identical resistance-adjusted magnitude (resolve_damage.h's hp_loss) without
                // waiting on it. See Damageable::impact_transfer's own doc for why this lives
                // here and not in the general per-blow loop.
                if (raycast_hit.rigidbody != nullptr) {
                    using coopa::physx::damage::DamageMaterial;
                    DamageMaterial material = DamageMaterial::default_material();
                    if (!damageable->material.empty()) {
                        auto handle = assets_.load<DamageMaterial>("damage_materials/" + damageable->material + ".yaml");
                        if (handle.is_loaded()) material = *handle.get();
                    }
                    const float resistance = std::max(1e-3f, material.resistance_to(blow.type));
                    const float impulse_mag =
                        std::max(0.0f, blow.amount) / resistance * damageable->impact_transfer;
                    raycast_hit.rigidbody->add_impulse_at_position(blow.direction * impulse_mag, blow.point);
                }
            }
        }
    }
}

void DestructionSystem::age_chunks_(coopa::scene::Scene& scene, float dt) {
    for (std::size_t i = 0; i < chunks_.size();) {
        Chunk& chunk = chunks_[i];
        if (chunk.object == nullptr || chunk.lifetime <= 0.0f) { ++i; continue; }
        chunk.age += dt;
        const float over = chunk.age - chunk.lifetime;
        if (over < 0.0f) { ++i; continue; }
        if (chunk.fade > 0.0f && over < chunk.fade) {
            // Sinking out of sight beats vanishing: debris that pops out mid-view reads as a
            // bug, and there is no per-object fade in the opaque pass to dissolve it with.
            if (auto* tc = chunk.object->get_transform()) {
                const glm::vec3 p = tc->transform().position();
                tc->transform().set_position(p - glm::vec3(0.0f, 0.0f, dt * 2.0f * glm::length(chunk.object->get_transform()->transform().scale())));
            }
            ++i;
            continue;
        }
        scene.remove_root_object(chunk.object);
        chunks_[i] = chunks_.back();
        chunks_.pop_back();
        physics_dirty_ = true;
    }
}

void DestructionSystem::execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) {
    std::vector<Destructible*> breakables = scene.get_components<Destructible>();
    physics_dirty_ = false;

    for (Destructible* d : breakables) {
        if (d == nullptr || d->owner == nullptr || d->shattered()) continue;
        const auto* renderer = d->owner->get_component<MeshRenderer>();
        if (renderer == nullptr || renderer->mesh_path().empty()) continue;

        Fracture* fracture = nullptr;
        const bool ready = (d->prefracture || d->pending()) &&
                           ensure_fracture_(*d, renderer->mesh_path(), fracture);
        if (!d->pending() || !ready || fracture == nullptr) continue;
        d->mark_shattered(spawn_chunks_(scene, *d, *fracture));
    }

    for (Damageable* dm : scene.get_components<Damageable>()) {
        if (dm != nullptr && dm->owner != nullptr) process_damage_(scene, *dm);
    }

    auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"));
    if (physics != nullptr) process_damage_cursors_(scene, *physics);

    age_chunks_(scene, ctx.delta_time);

    if (physics_dirty_ && physics != nullptr) physics->refresh();
}

DestructionSystem* install_destruction_system(coopa::scene::Scene& scene,
                                              coopa::gfx::core::Device& device,
                                              coopa::gfx::memory::Allocator& allocator,
                                              coopa::asset::AssetManager& assets,
                                              coopa::job::JobEngine* jobs,
                                              std::uint32_t frames_in_flight) {
    if (auto* existing = scene.find_system("Destruction")) {
        return dynamic_cast<DestructionSystem*>(existing);
    }
    auto system = std::make_unique<DestructionSystem>(device, allocator, assets, jobs, frames_in_flight);
    DestructionSystem* raw = system.get();
    scene.add_system(std::move(system), 70);
    return raw;
}

} // namespace scene
} // namespace toy
