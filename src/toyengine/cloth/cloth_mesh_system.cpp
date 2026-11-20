#include <toyengine/cloth/cloth_mesh_system.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/gtc/quaternion.hpp>

#include <coopa/debug/logger.h>
#include <coopa/job/engine.h>
#include <coopa/scene/components/transform_component.h>
#include <gfxcoopa/memory/storage_buffer.h>
#include <gfxcoopa/types/enums.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <physxcoopa/cloth/cloth_mesh_builder.h>
#include <physxcoopa/system/physics_system.h>
#include <physxcoopa/util/math.h>
#include <physxcoopa/world.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <toyengine/rig/bone_attachment.h>
#include <toyengine/rig/rig_component.h>
#include <toyengine/scene/skinned_mesh_renderer.h>

namespace toy {
namespace cloth {

namespace px = coopa::physx;

namespace {

/** @brief FNV-1a over arbitrary bytes, for the rebuild gate. */
uint64_t hash_bytes(uint64_t h, const void* data, std::size_t bytes) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

template <typename T>
uint64_t hash_value(uint64_t h, const T& v) {
    return hash_bytes(h, &v, sizeof(T));
}

/**
 * @brief The dispatcher handed to solve_cloth() when a JobEngine is available: parallel for
 *        large ranges, inline below the point where dispatch overhead beats the work (the
 *        cloth README's measured argument). Bit-identical to SerialDispatch either way --
 *        every dispatched stage writes only inside its own sub-range.
 */
struct JobDispatch {
    coopa::job::JobEngine* jobs = nullptr;
    template <typename Fn>
    void operator()(std::size_t count, Fn&& fn) const {
        if (count == 0) return;
        if (count < 1024 || jobs == nullptr) {
            fn(std::size_t{0}, count);
            return;
        }
        jobs->parallel_for_blocking(
            count, 0, [&fn](std::size_t begin, std::size_t end) { fn(begin, end); }, 0,
            coopa::job::Priority::Normal, coopa::job::Priority::Normal);
    }
};

/** @brief Linear-blend-skins one object-space point (w == 1) or direction (w == 0). */
glm::vec3 lbs(const glm::ivec4& j, const glm::vec4& w, const std::vector<glm::mat4>& palette,
              const glm::vec3& v, float point, const glm::vec3& fallback) {
    glm::vec3 acc(0.0f);
    float weight_sum = 0.0f;
    for (int c = 0; c < 4; ++c) {
        if (j[c] < 0 || w[c] <= 0.0f || static_cast<std::size_t>(j[c]) >= palette.size()) continue;
        acc += w[c] * glm::vec3(palette[static_cast<std::size_t>(j[c])] * glm::vec4(v, point));
        weight_sum += w[c];
    }
    return weight_sum > 1e-6f ? acc / weight_sum : fallback;
}

coopa::debug::Logger& logger() {
    static coopa::debug::Logger log{"cloth"};
    return log;
}

/** @brief One drawn body part fitted to a collider, in the frame of the bone it rides. */
struct BonePartFit {
    int bone = -1;
    px::collision::Shape shape;
};

/**
 * @brief Every object riding `rig` on a BoneAttachment that draws a mesh, with a capsule fitted
 *        around that mesh in its bone's frame.
 *
 * **Why fit the drawn part rather than the bone.** auto_bone_capsule() derives a capsule from a
 * bone's LENGTH, which is a guess about a body it has never seen -- and on a real character it is
 * wrong by centimetres in both directions: the mannequin's head bone is 0.12 m long and gets a
 * 0.026 m capsule for a 0.11 m head, while its chest bone's capsule is a third of the chest's
 * width. Cloth is then held off a body the viewer is not looking at, and the body the viewer IS
 * looking at hangs through the garment with every penetration test still green. The part meshes
 * riding the bones are that body, exactly, so they are what the colliders are fitted to.
 *
 * The capsule covers a part whose cross-section is an ELLIPSE, which is what a character's parts
 * are (a capsule mesh, possibly under non-uniform scale): the radius takes the larger of the two
 * minor half-extents, so the smaller one is covered with room to spare. A genuinely box-section
 * part has its corners left uncovered by up to 41% of that radius -- for the mannequin that is the
 * 5 cm visor on its face, 9 mm out, less than the standoff the garment is held at anyway. Author a
 * `bone_colliders` box for a part where that matters.
 */
std::vector<BonePartFit> fit_drawn_parts(toy::rig::Rig& rig, float collider_scale) {
    using MeshRenderer = coopa::gfx::engine::components::MeshRenderer;
    std::vector<BonePartFit> out;
    if (rig.owner == nullptr) return out;
    const float scale = collider_scale > 0.0f ? collider_scale : 1.0f;

    for (toy::rig::BoneAttachment* ba :
         rig.owner->get_components_in_children<toy::rig::BoneAttachment>()) {
        if (ba->owner == nullptr) continue;
        auto* mr = ba->owner->get_component<MeshRenderer>();
        auto* tc = ba->owner->get_transform();
        if (mr == nullptr || !mr->is_ready() || tc == nullptr) continue;
        const int bone = rig.find_bone(ba->bone);
        if (bone < 0) continue;

        // Part -> bone. Read from the live matrices rather than BoneAttachment's captured offset
        // because the two are the same rigid relationship in any pose, and this needs no access
        // to the attachment's internals.
        const glm::mat4 to_bone =
            glm::inverse(rig.world_matrix(static_cast<std::size_t>(bone))) * tc->get_world_matrix();
        glm::vec3 axis_scale(glm::length(glm::vec3(to_bone[0])), glm::length(glm::vec3(to_bone[1])),
                             glm::length(glm::vec3(to_bone[2])));
        glm::mat3 rot(to_bone);
        for (int c = 0; c < 3; ++c) {
            if (axis_scale[c] > 1e-6f) rot[c] /= axis_scale[c];
        }

        // The mesh's own bounds, scaled -- still in the PART's axes, which is where its capsule
        // is aligned. The rotation goes on the shape (local_rotation) rather than into the box.
        const glm::vec3 lo = mr->get_mesh()->bounds_min() * axis_scale;
        const glm::vec3 hi = mr->get_mesh()->bounds_max() * axis_scale;
        const glm::vec3 centre = 0.5f * (lo + hi);
        const glm::vec3 half = glm::max(0.5f * (hi - lo), glm::vec3(1e-4f));

        const int axis = (half.x >= half.y && half.x >= half.z) ? 0 : (half.y >= half.z ? 1 : 2);
        const float minor_a = half[(axis + 1) % 3];
        const float minor_b = half[(axis + 2) % 3];
        // Radius from the WIDER minor half-extent, and the capsule kept exactly as long as the
        // part: with a uniform scale this is the part's own capsule, to the micron.
        //
        // Under a non-uniform scale it is a hair tight at the CAPS -- an unevenly scaled cap is
        // an ellipsoid and a cap sphere of the wide radius pinches in faster than it does, by
        // 11 mm on the mannequin's 1.25x-wide chest. Running the cylinder on until that is
        // covered is the alternative and it is worse: it adds the same slack along the AXIS,
        // which for a pelvis capsule laid ACROSS the hips is 26 mm of phantom hip sticking out
        // sideways, exactly where a skirt hangs. A garment whose cap clearance matters wants an
        // authored `bone_colliders` entry.
        const float radius = std::max(minor_a, minor_b) * scale;
        const float half_height = std::max(0.0f, half[axis] - radius);

        BonePartFit fit;
        fit.bone = bone;
        // local_center is measured in the BONE's frame (Shape::local_center's contract), so the
        // part's own rotation applies to the offset here and to the capsule's axis via
        // local_rotation -- never twice.
        const glm::vec3 bone_centre = glm::vec3(to_bone[3]) + rot * centre;
        fit.shape = (half_height > 1e-4f)
                        ? px::collision::Shape::make_capsule(radius, half_height, axis, bone_centre)
                        : px::collision::Shape::make_sphere(std::max(radius, half[axis] * scale),
                                                            bone_centre);
        fit.shape.local_rotation = glm::normalize(glm::quat_cast(rot));
        out.push_back(std::move(fit));
    }
    return out;
}

/** @brief How many of `rig`'s attached parts have a mesh ready to fit. Mixed into the rebuild
 *         hash so a garment built while the body was still loading refits once it arrives. */
std::size_t ready_bone_parts(toy::rig::Rig& rig) {
    using MeshRenderer = coopa::gfx::engine::components::MeshRenderer;
    if (rig.owner == nullptr) return 0;
    std::size_t ready = 0;
    for (toy::rig::BoneAttachment* ba :
         rig.owner->get_components_in_children<toy::rig::BoneAttachment>()) {
        if (ba->owner == nullptr) continue;
        auto* mr = ba->owner->get_component<MeshRenderer>();
        if (mr != nullptr && mr->is_ready()) ++ready;
    }
    return ready;
}

} // namespace

bool ClothMeshSystem::claims(const toy::scene::SkinnedMeshRenderer& smr) const {
    // Not before the renderer is ready: its own first upload() builds the dynamic mesh (and,
    // on the compute path, the bind-pose SSBO and palette ring this system's deform dispatch
    // aliases), so frame one draws plain skinning and the blend takes over on frame two.
    if (!smr.is_ready()) return false;
    const CharacterCloth* cc =
        smr.owner != nullptr ? smr.owner->get_component<CharacterCloth>() : nullptr;
    if (cc == nullptr || !cc->enabled) return false;
    auto it = entries_.find(cc->state_key());
    return it != entries_.end() && it->second.live;
}

const px::cloth::Cloth* ClothMeshSystem::cloth(const std::string& key) const {
    auto it = entries_.find(key);
    return (it != entries_.end() && it->second.live) ? &it->second.sim : nullptr;
}

std::size_t ClothMeshSystem::particle_count(const std::string& key) const {
    const px::cloth::Cloth* c = cloth(key);
    return c != nullptr ? c->particles.size() : 0;
}

std::size_t ClothMeshSystem::bone_collider_count(const std::string& key) const {
    auto it = entries_.find(key);
    return (it != entries_.end() && it->second.live) ? it->second.bone_shapes.size() : 0;
}

std::vector<px::cloth::ClothCollider> ClothMeshSystem::bone_colliders(const std::string& key) const {
    std::vector<px::cloth::ClothCollider> out;
    auto it = entries_.find(key);
    if (it == entries_.end() || !it->second.live) return out;
    const Entry& e = it->second;
    // prev_bone_* hold the pose gather_colliders_() ended on, i.e. the one this frame solved
    // against and draws. Before the first gather they are empty and there is nothing to report.
    if (e.prev_bone_position.size() != e.bone_shapes.size()) return out;
    out.reserve(e.bone_shapes.size());
    for (std::size_t k = 0; k < e.bone_shapes.size(); ++k) {
        px::cloth::ClothCollider c;
        c.shape = &e.bone_shapes[k];
        c.position = e.prev_bone_position[k];
        c.orientation = e.prev_bone_orientation[k];
        c.end_orientation = c.orientation;
        c.bounds = px::collision::world_bounds(e.bone_shapes[k], c.position, c.orientation);
        out.push_back(c);
    }
    return out;
}

bool ClothMeshSystem::build_entry_(Entry& e, CharacterCloth& cc,
                                   toy::scene::SkinnedMeshRenderer& smr) {
    using Source = coopa::gfx::engine::data::SkinnedMeshSource;
    const Source* src = smr.source();
    if (src == nullptr) return false; // still loading; retry next frame silently

    const std::string key = cc.state_key();
    if (src->cloth.empty()) {
        if (!e.warned) {
            logger().warn("'" + key + "' has no cloth paint on its mesh (no valid `cloth:` "
                          "channels) -- CharacterCloth is inert. Paint it in the editor's "
                          "Cloth Paint mode.");
            e.warned = true;
        }
        return false;
    }
    if (smr.rig() == nullptr) {
        if (!e.warned) {
            logger().warn("'" + key + "' found no Rig for its SkinnedMeshRenderer -- "
                          "CharacterCloth is inert.");
            e.warned = true;
        }
        return false;
    }

    const std::size_t n = src->source_positions.size();
    if (n == 0 || src->source_vertex.size() != src->vertices.size()) return false;

    // --- Welded skinning data: the first render corner of each welded vertex wins (the
    // exporter writes the same influences on every corner of a vertex).
    e.render_to_welded = src->source_vertex;
    e.welded_bind = src->source_positions;
    e.welded_joints.assign(n, glm::ivec4(-1));
    e.welded_weights.assign(n, glm::vec4(0.0f));
    e.welded_bind_normal.assign(n, glm::vec3(0.0f, 0.0f, 1.0f));
    std::vector<bool> seen(n, false);
    for (std::size_t i = 0; i < src->vertices.size(); ++i) {
        const uint32_t v = src->source_vertex[i];
        if (v >= n || seen[v]) continue;
        seen[v] = true;
        if (i < src->joints.size()) e.welded_joints[v] = src->joints[i];
        if (i < src->weights.size()) e.welded_weights[v] = src->weights[i];
        e.welded_bind_normal[v] = src->vertices[i].normal;
    }

    // --- Per-render-vertex blend fade: 0 at painted max_distance 0 (pure skinning), 1 once
    // the paint exceeds the seam band (pure simulation).
    const float blend = std::max(cc.max_distance_blend, 1e-4f);
    e.render_weight.resize(src->vertices.size());
    for (std::size_t i = 0; i < src->vertices.size(); ++i) {
        const uint32_t v = src->source_vertex[i];
        const float md = v < src->cloth.max_distance.size() ? src->cloth.max_distance[v] : 0.0f;
        e.render_weight[i] = glm::clamp(md / blend, 0.0f, 1.0f);
    }

    // --- The simulation, built from the BIND pose in world scale: the owner's world matrix is
    // rigid-plus-scale, so rest lengths come out correct no matter what pose the scene starts
    // in (the current ANIMATED pose would bake stretched rest lengths). Particles are then
    // teleported onto the current skinned pose below.
    auto* tc = cc.owner != nullptr ? cc.owner->get_transform() : nullptr;
    const glm::mat4 owner_world = tc != nullptr ? tc->get_world_matrix() : glm::mat4(1.0f);

    px::cloth::MeshClothDesc desc;
    desc.positions.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        desc.positions[i] = glm::vec3(owner_world * glm::vec4(src->source_positions[i], 1.0f));
    }
    desc.triangles.resize(src->vertices.size());
    for (std::size_t i = 0; i < src->vertices.size(); ++i) desc.triangles[i] = src->source_vertex[i];
    desc.channels.max_distance = src->cloth.max_distance;
    desc.channels.backstop_distance = src->cloth.backstop_distance;
    desc.channels.backstop_radius = src->cloth.backstop_radius;
    desc.channels.anim_drive = src->cloth.anim_drive;
    desc.mass_per_area = cc.mass_per_area;
    desc.params = cc.params;
    e.sim = px::cloth::make_mesh_cloth(desc);
    if (e.sim.particles.empty() || e.sim.skin.empty()) return false;

    // Start the garment ON the current skinned pose, at rest.
    skin_targets_(e, smr, owner_world, nullptr);
    for (std::size_t i = 0; i < n; ++i) {
        px::cloth::ClothParticle& p = e.sim.particles[i];
        p.position = e.sim.skin_targets.position[i];
        p.prev_position = p.position;
        p.velocity = glm::vec3(0.0f);
    }
    e.sim.skin_targets.prev_position = e.sim.skin_targets.position;
    if (!e.sim.skin_targets.normal.empty()) {
        e.sim.skin_targets.prev_normal = e.sim.skin_targets.normal;
    }
    e.sim.bounds = px::cloth::compute_bounds(e.sim);

    // --- Bone colliders: persistent shapes with the bone-local offset/rotation baked in, posed
    // from the bone's world matrix each frame.
    //
    // Every rig bone is fitted first (auto_colliders), then the authored specs correct
    // individual bones by name. That order is the point: a garment collides with the WHOLE
    // body by default, and an author adds detail rather than remembering to list an elbow.
    e.bone_shapes.clear();
    e.bone_index.clear();
    toy::rig::Rig& rig = *smr.rig();
    if (cc.auto_colliders) {
        // The drawn parts first: where a bone carries geometry, that geometry IS the body, and a
        // capsule fitted to it beats one guessed from the bone's length (see fit_drawn_parts()).
        std::vector<bool> fitted(rig.bone_count(), false);
        for (BonePartFit& fit : fit_drawn_parts(rig, cc.collider_scale)) {
            if (fit.bone >= 0 && static_cast<std::size_t>(fit.bone) < fitted.size()) {
                fitted[static_cast<std::size_t>(fit.bone)] = true;
            }
            e.bone_shapes.push_back(std::move(fit.shape));
            e.bone_index.push_back(fit.bone);
        }
        for (std::size_t b = 0; b < rig.bone_count(); ++b) {
            if (fitted[b]) continue; // already covered by the geometry riding it
            const glm::vec2 capsule = auto_bone_capsule(rig.bone_length(b), cc.collider_scale);
            if (capsule.x <= 0.0f) continue;
            // Along the bone's own +Y rest axis, centred on its midpoint (rig_asset.h's
            // convention) -- the same fit Ragdoll::auto_generate_() makes. This is the fallback
            // for a bone nothing is drawn on: a skinned body mesh, or a bone that only carries
            // other bones.
            e.bone_shapes.push_back(
                capsule.y > 0.0f
                    ? px::collision::Shape::make_capsule(capsule.x, capsule.y, /*axis=*/1,
                                                         glm::vec3(0.0f, capsule.y, 0.0f))
                    : px::collision::Shape::make_sphere(capsule.x));
            e.bone_index.push_back(static_cast<int>(b));
        }
    }
    for (const BoneColliderSpec& spec : cc.bone_colliders) {
        const int bi = rig.find_bone(spec.bone);
        if (bi < 0) {
            logger().warn("'" + key + "' bone collider names unknown bone '" + spec.bone + "'.");
            continue;
        }
        // radius 0 removes the bone's collider (its auto one included).
        const bool drop = spec.shape != "box" && spec.radius <= 0.0f;
        // A capsule along the bone axis is centred ON THE BONE, exactly as the auto fit is:
        // `{bone: shin_l, radius: 0.062, half_height: 0.205}` then covers the shin, which is
        // what an author writing a correction means. Centred on the bone's HEAD instead -- the
        // raw shape convention -- it would cover the upper half and jut out past the joint,
        // leaving the limb's far half with no collider at all. Every other shape (and a capsule
        // deliberately laid ACROSS the bone) sits at the head, where its own offset is measured
        // from.
        glm::vec3 offset = spec.offset;
        if (spec.shape != "sphere" && spec.shape != "box" && spec.axis == 1) {
            offset.y += spec.half_height;
        }
        px::collision::Shape shape =
            spec.shape == "sphere" ? px::collision::Shape::make_sphere(spec.radius, offset)
            : spec.shape == "box"  ? px::collision::Shape::make_box(spec.size, offset)
                                   : px::collision::Shape::make_capsule(spec.radius, spec.half_height,
                                                                        spec.axis, offset);
        shape.local_rotation = glm::quat(glm::radians(spec.rotation));

        // An authored spec REPLACES everything the bone had, however many shapes that was: a
        // bone can now carry one collider per drawn part riding it (a head and its visor), and
        // overriding only the first would leave the other sitting alongside the correction,
        // which is the opposite of what writing a correction means.
        for (std::size_t i = e.bone_index.size(); i-- > 0;) {
            if (e.bone_index[i] != bi) continue;
            e.bone_shapes.erase(e.bone_shapes.begin() + static_cast<std::ptrdiff_t>(i));
            e.bone_index.erase(e.bone_index.begin() + static_cast<std::ptrdiff_t>(i));
        }
        if (drop) continue;
        e.bone_shapes.push_back(shape);
        e.bone_index.push_back(bi);
    }

    e.obj_positions.assign(n, glm::vec3(0.0f));
    e.obj_normals.assign(n, glm::vec3(0.0f, 0.0f, 1.0f));
    e.live = true;
    return true;
}

void ClothMeshSystem::skin_targets_(Entry& e, toy::scene::SkinnedMeshRenderer& smr,
                                    const glm::mat4& owner_world, coopa::job::JobEngine* jobs) {
    const std::vector<glm::mat4>& palette = smr.updated_skin_matrices();
    const std::size_t n = e.welded_bind.size();
    px::cloth::ClothSkinTargets& t = e.sim.skin_targets;
    t.position.resize(n);
    const bool want_normals = !e.sim.skin.backstop_radius.empty();
    if (want_normals) t.normal.resize(n);

    const glm::mat3 owner_rot(owner_world);
    auto body = [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const glm::vec3 pos = lbs(e.welded_joints[i], e.welded_weights[i], palette,
                                      e.welded_bind[i], 1.0f, e.welded_bind[i]);
            t.position[i] = glm::vec3(owner_world * glm::vec4(pos, 1.0f));
            if (want_normals) {
                const glm::vec3 nrm = lbs(e.welded_joints[i], e.welded_weights[i], palette,
                                          e.welded_bind_normal[i], 0.0f, e.welded_bind_normal[i]);
                const glm::vec3 wn = owner_rot * nrm;
                const float len = glm::length(wn);
                t.normal[i] = len > 1e-6f ? wn / len : glm::vec3(0.0f, 0.0f, 1.0f);
            }
        }
    };
    if (jobs != nullptr && n >= 2048 && jobs->worker_count() >= 2) {
        jobs->parallel_for_blocking(
            n, 0, [&body](std::size_t b, std::size_t en) { body(b, en); }, 0,
            coopa::job::Priority::Normal, coopa::job::Priority::Normal);
    } else {
        body(0, n);
    }
}

void ClothMeshSystem::gather_colliders_(Entry& e, CharacterCloth& cc,
                                        toy::scene::SkinnedMeshRenderer& smr,
                                        px::system::PhysicsSystem* physics, float dt,
                                        bool* any_moving) {
    colliders_.clear();
    const float margin = physics != nullptr ? physics->world().config().aabb_margin : 0.05f;
    const float travel = px::cloth::max_particle_speed(e.sim) * dt;
    const px::geometry::AABB query =
        e.sim.bounds.expand(e.sim.params.thickness + travel + margin);

    if (cc.collide_world && physics != nullptr) {
        // dt so the world's own moving shapes (a platform, a driven prop) are swept across
        // the solve's substeps, exactly as the bone colliders below are.
        physics->world().gather_cloth_colliders(query, e.sim.params.layer, colliders_, any_moving, dt);
    }

    toy::rig::Rig* rig = smr.rig();
    if (rig == nullptr) return;
    const bool have_prev = e.prev_bone_position.size() == e.bone_shapes.size();
    e.prev_bone_position.resize(e.bone_shapes.size());
    e.prev_bone_orientation.resize(e.bone_shapes.size());
    for (std::size_t k = 0; k < e.bone_shapes.size(); ++k) {
        const glm::mat4 bone_world = rig->world_matrix(static_cast<std::size_t>(e.bone_index[k]));
        // Normalise the rotation columns: bone matrices may carry (near-uniform) scale, and the
        // shape's own dimensions are authored in metres, not bone units.
        glm::mat3 rot(bone_world);
        for (int c = 0; c < 3; ++c) {
            const float len = glm::length(rot[c]);
            if (len > 1e-6f) rot[c] /= len;
        }
        const glm::vec3 now_pos(bone_world[3]);
        const glm::quat now_rot = glm::normalize(glm::quat_cast(rot));

        // **Bone colliders carry their own sweep**, because nothing else knows it: the world's
        // shapes get theirs from their bodies' velocities (PhysicsWorld::gather_cloth_colliders),
        // but a bone is posed by the rig, not by a rigidbody. The pose at the previous solve and
        // the pose now are the two ends of it. A shin swings several centimetres per frame, and
        // the volume it crossed between the two solves is exactly where a garment gets left
        // inside the leg; handing the solver both poses makes it project out of the swept volume
        // (exact for a sphere -- a swept sphere IS a capsule -- and a two-pose probe otherwise)
        // and lets it advance the bone across its substeps instead of teleporting it.
        px::cloth::ClothCollider collider;
        collider.shape = &e.bone_shapes[k];
        collider.position = have_prev ? e.prev_bone_position[k] : now_pos;
        collider.orientation = have_prev ? e.prev_bone_orientation[k] : now_rot;
        collider.sweep = now_pos - collider.position;
        collider.end_orientation = now_rot;
        // Over BOTH poses, as ClothCollider::bounds requires for the cheap reject.
        collider.bounds =
            px::collision::world_bounds(e.bone_shapes[k], collider.position, collider.orientation);
        const px::geometry::AABB end_bounds =
            px::collision::world_bounds(e.bone_shapes[k], now_pos, now_rot);
        collider.bounds.min = glm::min(collider.bounds.min, end_bounds.min);
        collider.bounds.max = glm::max(collider.bounds.max, end_bounds.max);
        colliders_.push_back(collider);

        e.prev_bone_position[k] = now_pos;
        e.prev_bone_orientation[k] = now_rot;
    }
}

void ClothMeshSystem::execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) {
    ++frame_stamp_;
    auto* physics =
        dynamic_cast<px::system::PhysicsSystem*>(scene.find_system("Physics"));
    const px::util::PhysicsConfig config =
        physics != nullptr ? physics->world().config() : px::util::PhysicsConfig{};
    // Substep length held at fixed_dt / cloth_substeps -- the same arithmetic as
    // PhysicsWorld::step_cloths_(), for the same frame-rate independence. Resolved here rather than
    // at the solve call because gather_colliders_() needs `dt` for its sweeps first.
    const float internal_h = std::max(
        config.fixed_dt / static_cast<float>(std::max(1u, config.cloth_substeps)),
        px::util::k_epsilon);
    const float dt = ctx.delta_time;

    for (CharacterCloth* cc : scene.get_components<CharacterCloth>()) {
        if (cc->owner == nullptr) continue;
        auto* smr = cc->owner->get_component<toy::scene::SkinnedMeshRenderer>();
        Entry& e = entries_[cc->state_key()];
        e.stamp = frame_stamp_;
        if (smr == nullptr) {
            if (!e.warned) {
                logger().warn("'" + cc->state_key() +
                              "' has no sibling SkinnedMeshRenderer -- CharacterCloth is inert.");
                e.warned = true;
            }
            continue;
        }

        // --- Rebuild gate: topology-affecting inputs only. Tunables are written through to the
        // live sim below, so an inspector drag never resets the simulation.
        uint64_t h = 1469598103934665603ull;
        const void* src_id = smr->source();
        h = hash_value(h, src_id);
        h = hash_value(h, cc->mass_per_area);
        h = hash_value(h, cc->max_distance_blend);
        h = hash_value(h, cc->params.self_collision);
        h = hash_value(h, cc->params.tether_scale);
        h = hash_value(h, cc->auto_colliders);
        h = hash_value(h, cc->collider_scale);
        // How many drawn parts were fittable. A garment built on frame one, before the body's
        // meshes finished loading, would otherwise keep the bone-length fallback for good.
        if (toy::rig::Rig* rig = smr->rig(); rig != nullptr && cc->auto_colliders) {
            h = hash_value(h, ready_bone_parts(*rig));
        }
        for (const BoneColliderSpec& s : cc->bone_colliders) {
            h = hash_bytes(h, s.bone.data(), s.bone.size());
            h = hash_bytes(h, s.shape.data(), s.shape.size());
            h = hash_value(h, s.radius);
            h = hash_value(h, s.half_height);
            h = hash_value(h, s.axis);
            h = hash_value(h, s.size);
            h = hash_value(h, s.offset);
            h = hash_value(h, s.rotation);
        }
        if (auto* tc = cc->owner->get_transform()) {
            // Owner scale is baked into rest lengths; a rescale must rebuild.
            const glm::mat4 w = tc->get_world_matrix();
            const glm::vec3 scale(glm::length(glm::vec3(w[0])), glm::length(glm::vec3(w[1])),
                                  glm::length(glm::vec3(w[2])));
            h = hash_value(h, glm::round(scale * 1000.0f));
        }

        if (!e.live || e.build_hash != h) {
            const bool was_warned = e.warned;
            e = Entry{};
            e.stamp = frame_stamp_;
            e.warned = was_warned;
            if (!build_entry_(e, *cc, *smr)) continue;
            e.build_hash = h;
        }
        if (!cc->enabled) continue; // claims() lets the plain skinned upload run

        // --- Tunables through, every frame; keep the builder-derived self_distance when the
        // author left it at 0, and fold in the weather wind.
        const float derived_self_distance = e.sim.params.self_distance;
        e.sim.params = cc->params;
        if (cc->params.self_distance <= 0.0f) e.sim.params.self_distance = derived_self_distance;
        e.sim.params.wind = cc->params.wind + cc->wind;

        auto* tc = cc->owner->get_transform();
        const glm::mat4 owner_world = tc != nullptr ? tc->get_world_matrix() : glm::mat4(1.0f);

        // --- Targets (this frame's skinned pose), then the wake test against last frame's.
        skin_targets_(e, *smr, owner_world, ctx.jobs);
        bool targets_moved = false;
        const px::cloth::ClothSkinTargets& t = e.sim.skin_targets;
        if (t.prev_position.size() == t.position.size()) {
            for (std::size_t i = 0; i < t.position.size(); ++i) {
                const glm::vec3 d = t.position[i] - t.prev_position[i];
                if (glm::dot(d, d) > px::util::k_epsilon) { targets_moved = true; break; }
            }
        } else {
            targets_moved = true;
        }

        bool collider_moving = false;
        gather_colliders_(e, *cc, *smr, physics, dt, &collider_moving);

        // --- Sleep gate, mirroring PhysicsWorld::step_cloths_().
        if (targets_moved || collider_moving) {
            e.sim.awake = true;
            e.sim.sleep_timer = 0.0f;
        }
        if (!e.sim.awake) {
            // Keep the target lerp's start pinned where the garment is, or the first substep
            // after waking would snap across the gap (the anchor-sleep rule).
            e.sim.skin_targets.prev_position = e.sim.skin_targets.position;
            if (!e.sim.skin_targets.normal.empty()) {
                e.sim.skin_targets.prev_normal = e.sim.skin_targets.normal;
            }
            continue;
        }

        // --- Solve on the frame clock; `internal_h` was resolved once at the top of execute().
        const uint32_t substeps = px::cloth::frame_substeps(dt, internal_h);
        const glm::vec3 gravity =
            physics != nullptr ? physics->world().gravity() : glm::vec3(0.0f, 0.0f, -9.81f);

        if (ctx.jobs != nullptr && ctx.jobs->worker_count() >= 2 &&
            e.sim.particles.size() >= 2048) {
            px::cloth::solve_cloth(e.sim, colliders_, gravity, substeps, config.cloth_iterations,
                                   dt, scratch_, JobDispatch{ctx.jobs});
        } else {
            px::cloth::solve_cloth(e.sim, colliders_, gravity, substeps, config.cloth_iterations,
                                   dt, scratch_);
        }

        // --- Sleep bookkeeping, again step_cloths_()'s.
        if (px::cloth::max_particle_speed(e.sim) < e.sim.params.sleep_threshold) {
            e.sim.sleep_timer += dt;
            if (e.sim.sleep_timer >= e.sim.params.sleep_time) {
                e.sim.awake = false;
                for (px::cloth::ClothParticle& p : e.sim.particles) p.velocity = glm::vec3(0.0f);
            }
        } else {
            e.sim.sleep_timer = 0.0f;
        }

        // --- Post-solve render data: object-space positions, area-weighted welded normals.
        const glm::mat4 inv_owner = glm::inverse(owner_world);
        const std::size_t n = e.sim.particles.size();
        e.obj_positions.resize(n);
        e.obj_normals.assign(n, glm::vec3(0.0f));
        for (std::size_t i = 0; i < n; ++i) {
            e.obj_positions[i] = glm::vec3(inv_owner * glm::vec4(e.sim.particles[i].position, 1.0f));
        }
        for (std::size_t tri = 0; tri + 2 < e.sim.triangles.size(); tri += 3) {
            const uint32_t i0 = e.sim.triangles[tri], i1 = e.sim.triangles[tri + 1],
                           i2 = e.sim.triangles[tri + 2];
            const glm::vec3 fn = glm::cross(e.obj_positions[i1] - e.obj_positions[i0],
                                            e.obj_positions[i2] - e.obj_positions[i0]);
            e.obj_normals[i0] += fn; // un-normalised cross = area weighting, ClothRenderer's rule
            e.obj_normals[i1] += fn;
            e.obj_normals[i2] += fn;
        }
        for (glm::vec3& nrm : e.obj_normals) {
            const float len = glm::length(nrm);
            nrm = len > 1e-8f ? nrm / len : glm::vec3(0.0f, 0.0f, 1.0f);
        }

        // Conservative object-space bounds for the GPU deform's mark_gpu_written(): every
        // blended vertex is a convex mix of a skinned position and a sim position, and a mix of
        // points from two boxes lies inside their union's AABB. Targets stand in for the
        // skinned positions (same influences, same palette). The CPU path never reads this --
        // update_vertices() recomputes bounds from the data.
        glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
        for (const glm::vec3& p : e.obj_positions) {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        for (std::size_t i = 0; i < e.sim.skin_targets.position.size(); ++i) {
            const glm::vec3 t_obj =
                glm::vec3(inv_owner * glm::vec4(e.sim.skin_targets.position[i], 1.0f));
            lo = glm::min(lo, t_obj);
            hi = glm::max(hi, t_obj);
        }
        const glm::vec3 margin(e.sim.params.thickness + 0.01f);
        e.bounds_min = lo - margin;
        e.bounds_max = hi + margin;
    }

    // --- Reconcile: drop entries whose component vanished (deleted object, renamed owner).
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.stamp != frame_stamp_) it = entries_.erase(it);
        else ++it;
    }
}

void ClothMeshSystem::upload(coopa::scene::Scene& scene, std::uint32_t frame_slot,
                             toy::render::passes::ClothDeformPass* deform,
                             coopa::gfx::memory::Allocator* allocator) {
    using Vertex = coopa::gfx::engine::data::Vertex;
    using Pass = toy::render::passes::ClothDeformPass;
    for (CharacterCloth* cc : scene.get_components<CharacterCloth>()) {
        if (cc->owner == nullptr || !cc->enabled) continue;
        auto it = entries_.find(cc->state_key());
        if (it == entries_.end() || !it->second.live) continue;
        Entry& e = it->second;
        auto* smr = cc->owner->get_component<toy::scene::SkinnedMeshRenderer>();
        if (smr == nullptr || !smr->is_ready()) continue; // claims() left frame one to the renderer

        // --- GPU path: skin + blend in one compute dispatch; the CPU uploads only the welded
        // sim vertices. preserve_volume stays on the CPU blend (the kernel is LBS-only).
        if (deform != nullptr && allocator != nullptr && smr->gpu_skinned() &&
            !smr->preserve_volume() && smr->gpu_bind_pose() != nullptr) {
            const auto& mesh = smr->mesh();
            if (e.gpu == nullptr || e.gpu->mesh_id != mesh.get()) {
                e.gpu.reset(); // waits idle first; the old sets may be in flight
                using namespace coopa::gfx;
                const std::size_t welded = e.welded_bind.size();
                const uint32_t slots = mesh->vertex_buffer_count();
                auto gpu = std::make_unique<Entry::GpuDeform>(
                    deform->device(),
                    memory::StorageBufferRing(deform->device(), *allocator,
                                              sizeof(Pass::SimVertex) * std::max<std::size_t>(welded, 1),
                                              slots, BufferUsage::None, MemoryResidency::CpuToGpu),
                    memory::make_storage_buffer(deform->device(), *allocator,
                                                sizeof(Pass::MapEntry) *
                                                    std::max<std::size_t>(e.render_to_welded.size(), 1),
                                                BufferUsage::None, MemoryResidency::CpuToGpu),
                    pipeline::DescriptorPoolBuilder().add_sets(deform->layout(), slots).build(
                        deform->device()));
                std::vector<Pass::MapEntry> map(e.render_to_welded.size());
                for (std::size_t i = 0; i < map.size(); ++i) {
                    map[i].welded = e.render_to_welded[i];
                    map[i].sim_weight = e.render_weight[i];
                }
                gpu->map.upload(map.data(), sizeof(Pass::MapEntry) * map.size());
                for (uint32_t s = 0; s < slots; ++s) {
                    gpu->sets.emplace_back(deform->device(), gpu->pool, deform->layout());
                    gpu->sets.back().bind_storage_buffer(0, *smr->gpu_bind_pose());
                    gpu->sets.back().bind_storage_buffer(1, smr->gpu_palette()->current(s));
                    gpu->sets.back().bind_storage_buffer(2, mesh->vertex_buffer(s));
                    gpu->sets.back().bind_storage_buffer(3, gpu->sim.current(s));
                    gpu->sets.back().bind_storage_buffer(4, gpu->map);
                }
                gpu->mesh_id = mesh.get();
                e.gpu = std::move(gpu);
            }

            e.sim_upload.resize(e.obj_positions.size());
            for (std::size_t i = 0; i < e.obj_positions.size(); ++i) {
                e.sim_upload[i].pos = glm::vec4(e.obj_positions[i], 0.0f);
                e.sim_upload[i].nrm = glm::vec4(e.obj_normals[i], 0.0f);
            }
            const std::vector<glm::mat4>& palette = smr->updated_skin_matrices();
            Pass::Job job;
            job.set = &e.gpu->sets[frame_slot % e.gpu->sets.size()];
            job.palette = smr->gpu_palette();
            job.matrices = palette.empty() ? nullptr : palette.data();
            job.sim = &e.gpu->sim;
            job.sim_data = e.sim_upload.data();
            job.bone_count = static_cast<uint32_t>(palette.size());
            job.vertex_count = static_cast<uint32_t>(e.render_to_welded.size());
            job.welded_count = static_cast<uint32_t>(e.sim_upload.size());
            job.frame_slot = frame_slot;
            deform->enqueue(job);
            smr->mesh()->mark_gpu_written(frame_slot, e.bounds_min, e.bounds_max);
            continue;
        }

        const std::vector<Vertex>& skinned = smr->skin_now();
        if (skinned.size() != e.render_to_welded.size()) continue;
        e.blend_scratch = skinned;
        for (std::size_t i = 0; i < e.blend_scratch.size(); ++i) {
            const float w = e.render_weight[i];
            if (w <= 0.0f) continue;
            const uint32_t v = e.render_to_welded[i];
            if (v >= e.obj_positions.size()) continue;
            Vertex& out = e.blend_scratch[i];
            out.position = glm::mix(out.position, e.obj_positions[v], w);
            const glm::vec3 nrm = glm::mix(out.normal, e.obj_normals[v], w);
            const float nlen = glm::length(nrm);
            out.normal = nlen > 1e-6f ? nrm / nlen : out.normal;
            // Keep the skinned tangent's frame, re-orthogonalised against the blended normal.
            glm::vec3 tan = glm::vec3(out.tangent);
            tan -= out.normal * glm::dot(out.normal, tan);
            const float tlen = glm::length(tan);
            if (tlen > 1e-6f) out.tangent = glm::vec4(tan / tlen, out.tangent.w);
        }
        smr->mesh()->update_vertices(e.blend_scratch.data(), e.blend_scratch.size(), frame_slot);
    }
}

ClothMeshSystem* install_cloth_mesh_system(coopa::scene::Scene& scene) {
    if (auto* existing = dynamic_cast<ClothMeshSystem*>(scene.find_system("ClothMesh"))) {
        return existing;
    }
    auto sys = std::make_unique<ClothMeshSystem>();
    ClothMeshSystem* raw = sys.get();
    scene.add_system(std::move(sys), k_cloth_mesh_system_order);
    return raw;
}

} // namespace cloth
} // namespace toy
