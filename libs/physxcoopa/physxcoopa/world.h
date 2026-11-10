/**
 * @file world.h
 * @brief PhysicsWorld -- the Scene-agnostic simulation core. No dependency on coopa::scene;
 *        the ISceneSystem adapter (system/physics_system.h), the components and physx_yaml.h
 *        are the only parts that know about Scene/SceneObject/Transform.
 */

#ifndef PHYSXCOOPA_WORLD_H
#define PHYSXCOOPA_WORLD_H

#include <physxcoopa/util/config.h>
#include <physxcoopa/util/math.h>
#include <physxcoopa/dynamics/body.h>
#include <physxcoopa/dynamics/integrator.h>
#include <physxcoopa/dynamics/inertia.h>
#include <physxcoopa/dynamics/joint.h>
#include <physxcoopa/dynamics/solver.h>
#include <physxcoopa/cloth/cloth.h>
#include <physxcoopa/cloth/cloth_builder.h>
#include <physxcoopa/cloth/cloth_collision.h>
#include <physxcoopa/cloth/cloth_solver.h>
#include <physxcoopa/hair/strand_builder.h>
#include <physxcoopa/hair/strand_solver.h>
#include <physxcoopa/collision/shape.h>
#include <physxcoopa/collision/manifold.h>
#include <physxcoopa/collision/contact_event.h>
#include <physxcoopa/collision/narrowphase.h>
#include <physxcoopa/broadphase/aabb_tree.h>
#include <physxcoopa/broadphase/layer_matrix.h>
#include <physxcoopa/broadphase/pair_cache.h>
#include <physxcoopa/query/queries.h>
#include <physxcoopa/query/sweep.h>
#include <physxcoopa/debug/debug_draw.h>
#include <physxcoopa/debug/shape_draw.h>
#include <physxcoopa/util/physics_settings.h>

#include <coopa/event/signal.h>
#include <coopa/debug/logger.h>
#include <coopa/job/engine.h>
#include <coopa/job/parallel_for.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include <iterator>
#include <unordered_set>

namespace coopa {
namespace physx {

/**
 * @class PhysicsWorld
 * @brief Owns every Body and Shape and drives the fixed-substep simulation loop.
 *
 * Depends only on glm, coopa::event::Signal, coopa::debug::Logger and coopa::job's JobEngine --
 * never on coopa::scene. This is what lets test.cpp exercise the whole solver/narrowphase/broadphase
 * stack with no Scene at all, and lets gameplay reach the world (via
 * `scene->find_system("Physics")`, see PhysicsSystem) without ever seeing a Scene reference
 * baked into PhysicsWorld itself.
 */
class PhysicsWorld {
public:
    /**
     * @brief Fired once per step_fixed(), after force integration and contact generation
     *        (broadphase + narrowphase) but before the solve -- see step_fixed()'s doc for
     *        exactly where.
     *        Gameplay may call add_force/add_torque/set_velocity/wake from a connected slot
     *        (applied immediately); create_body/destroy_body issued from here are deferred
     *        to the top of the NEXT step_fixed instead -- see those methods' docs.
     */
    coopa::event::Signal<PhysicsWorld&, float> on_substep;

    explicit PhysicsWorld(const util::PhysicsConfig& config = {})
        : config_(config), logger_("Physics") {}

    // --- Body lifecycle (immediate -- for setup-time use, outside step_fixed) ---

    /**
     * @brief Creates a new body with an optional collision shape, returning a generational
     *        handle to it. A default-constructed Shape (`enabled == false`) means the body
     *        participates in dynamics but never in collision, matching Unity's "Rigidbody
     *        with no Collider" case.
     *
     * @param initial Initial dynamic state.
     * @param shape   Collision geometry, or a disabled Shape for none.
     * @return Handle to the new body.
     */
    dynamics::BodyId add_body(const dynamics::Body& initial, const collision::Shape& shape = collision::Shape{}) {
        uint32_t index;
        if (!free_indices_.empty()) {
            index = free_indices_.back();
            free_indices_.pop_back();
            bodies_[index] = initial;
            body_shapes_[index].clear();
        } else {
            index = static_cast<uint32_t>(bodies_.size());
            bodies_.push_back(initial);
            generations_.push_back(0);
            alive_.push_back(false);
            body_shapes_.push_back({});
        }
        alive_[index] = true;
        dynamics::Body& b = bodies_[index];
        b.prev_position = b.position;
        b.prev_orientation = b.orientation;
        b.last_written_position = b.position;
        b.last_written_orientation = b.orientation;
        dynamics::update_world_inertia(b);
        dynamics::BodyId id{index, generations_[index]};
        add_shape(id, shape);
        return id;
    }

    /** @brief Destroys a body immediately, including every shape it owns (see add_shape()'s
     *         doc for compound bodies). A no-op if `id` is already invalid/stale. */
    void remove_body(dynamics::BodyId id) {
        if (!is_valid(id)) return;
        // Copy first -- remove_shape() below mutates body_shapes_[id.index] (its own erase),
        // so iterating that vector directly while shrinking it under our feet is exactly the
        // kind of bug this guards against.
        std::vector<uint32_t> owned = body_shapes_[id.index];
        for (uint32_t shape_index : owned) remove_shape(shape_index);
        // Any joint referencing this body must be invalidated NOW -- unlike a contact manifold
        // (rediscovered fresh by broadphase every substep, so a stale one simply never
        // reappears), a Joint is a persistent object the solver would otherwise keep
        // indexing into bodies_[id.index] via the raw index alone (no generation check --
        // see solve_joint_velocity_pass()'s doc), silently acting on WHATEVER body this slot
        // gets recycled for next.
        for (auto& j : joints_) {
            if (j.valid && (j.a == id || j.b == id)) j.valid = false;
        }
        for (auto it = ignored_pairs_.begin(); it != ignored_pairs_.end();) {
            const bool involved = static_cast<uint32_t>(*it >> 32) == id.index || static_cast<uint32_t>(*it) == id.index;
            it = involved ? ignored_pairs_.erase(it) : std::next(it);
        }
        alive_[id.index] = false;
        ++generations_[id.index];
        free_indices_.push_back(id.index);
    }

    /**
     * @brief Adds an ADDITIONAL shape to an already-live body -- the compound-collider
     *        primitive: call this once per extra child Collider bound to a Rigidbody (the
     *        first/primary shape is created for you by add_body() itself). The returned slot is
     *        a raw, non-generational index (unlike BodyId) -- this is an advanced, PhysicsSystem-
     *        internal-facing API whose caller (the Scene binding layer) always operates within
     *        well-defined frame boundaries and never holds a slot across a state where it could
     *        have gone stale without also already knowing to drop it (regather_() fully
     *        recomputes what shapes SHOULD exist every time it runs). No-op (returns
     *        k_invalid_shape) if `owner` is invalid.
     *
     * Every world_*() shape-instancing helper (collision/shape.h) already reads a shape's pose
     * as `owner_body.position + owner_body.orientation * (shape.local_center, .local_rotation)`
     * -- exactly the composition a compound child's own local offset/rotation needs.
     */
    static constexpr uint32_t k_invalid_shape = 0xFFFFFFFFu;
    uint32_t add_shape(dynamics::BodyId owner, const collision::Shape& shape) {
        if (!is_valid(owner)) return k_invalid_shape;
        uint32_t slot;
        if (!free_shape_indices_.empty()) {
            slot = free_shape_indices_.back();
            free_shape_indices_.pop_back();
            shapes_[slot] = shape;
            shape_owner_[slot] = owner;
        } else {
            slot = static_cast<uint32_t>(shapes_.size());
            shapes_.push_back(shape);
            shape_owner_.push_back(owner);
            shape_alive_.push_back(false);
            tree_proxy_.push_back(broadphase::k_null_node);
            in_static_tree_.push_back(false);
        }
        shape_alive_[slot] = true;
        body_shapes_[owner.index].push_back(slot);
        create_shape_proxy_(slot);
        return slot;
    }

    /** @brief Removes one shape slot (as returned by add_shape()/add_body()) immediately. A
     *         body left with zero shapes participates in dynamics but never collision, same as
     *         Shape::enabled's "Rigidbody with no Collider" doc already describes for the
     *         single-shape case. No-op if `shape_index` is out of range or already free. */
    void remove_shape(uint32_t shape_index) {
        if (shape_index >= shapes_.size() || !shape_alive_[shape_index]) return;
        destroy_shape_proxy_(shape_index);
        dynamics::BodyId owner = shape_owner_[shape_index];
        shape_alive_[shape_index] = false;
        free_shape_indices_.push_back(shape_index);
        if (owner.index < body_shapes_.size()) {
            auto& owned = body_shapes_[owner.index];
            owned.erase(std::remove(owned.begin(), owned.end(), shape_index), owned.end());
        }
    }

    /** @brief The shape slot at `body_shapes_[id.index][i]`, or k_invalid_shape if `id` is
     *         invalid or has no shape at that position (i=0 is the "primary" shape). */
    uint32_t shape_at(dynamics::BodyId id, std::size_t i = 0) const {
        if (!is_valid(id) || i >= body_shapes_[id.index].size()) return k_invalid_shape;
        return body_shapes_[id.index][i];
    }

    /** @brief Every shape slot `id` currently owns, primary first. Empty for a shapeless body. */
    const std::vector<uint32_t>& shapes_of(dynamics::BodyId id) const {
        static const std::vector<uint32_t> empty;
        return is_valid(id) ? body_shapes_[id.index] : empty;
    }

    /** @brief Replaces a live body's PRIMARY shape immediately -- for a compound body (more
     *         than one shape), every OTHER shape is untouched; use remove_shape()/add_shape()
     *         to add/remove additional children. No-op if `id` is invalid or already shapeless
     *         (use add_shape() to give a shapeless body its first shape instead). */
    void set_shape(dynamics::BodyId id, const collision::Shape& shape) {
        uint32_t slot = shape_at(id);
        if (slot == k_invalid_shape) return;
        destroy_shape_proxy_(slot);
        shapes_[slot] = shape;
        create_shape_proxy_(slot);
    }

    /** @brief Replaces the shape at slot `shape_index` immediately (see add_shape()'s doc for
     *         where the slot comes from). No-op if `shape_index` is invalid/free. */
    void set_shape_at(uint32_t shape_index, const collision::Shape& shape) {
        if (shape_index >= shapes_.size() || !shape_alive_[shape_index]) return;
        destroy_shape_proxy_(shape_index);
        shapes_[shape_index] = shape;
        create_shape_proxy_(shape_index);
    }

    /**
     * @brief Transitions a live body between BodyType::Dynamic and BodyType::Kinematic in
     *        place -- the only supported transition (a Static body has no Rigidbody to flip
     *        `is_kinematic` on in the first place, so Static is never a valid `from` or `to`
     *        here; both other types live in dynamic_tree_, never static_tree_ -- see
     *        create_proxy_()'s branch -- so this never touches broadphase tree membership or
     *        the body's proxy at all). No-op (returns false) if `id` is invalid, `new_type` is
     *        Static, or the body is already Static.
     *
     * Becoming Kinematic leaves `mass`/`inv_mass`/`inv_inertia_local` untouched, matching
     * PhysicsSystem::create_compound_body_()'s own Kinematic branch -- those fields are
     * simply never read for a non-Dynamic body (every consumer gates on `type == Dynamic`
     * first: integrate_forces/integrate_velocities in dynamics/integrator.h,
     * apply_impulse_pair in dynamics/solver.h), so leaving stale values is harmless and
     * consistent with how a body created straight into Kinematic already behaves.
     * Becoming Dynamic recomputes `inv_mass` from `mass` and `inv_inertia_local` from `mass`
     * and the body's CURRENT shape (dynamics::inertia_for_shape()) -- required, since these
     * are actively consumed by every Dynamic-only pass the moment the flip takes effect.
     *
     * @param mass Mass to apply if transitioning TO Dynamic; ignored transitioning to Kinematic.
     */
    bool set_body_type(dynamics::BodyId id, dynamics::BodyType new_type, float mass = 1.0f) {
        if (new_type == dynamics::BodyType::Static) return false;
        dynamics::Body* body = get_body(id);
        if (!body || body->type == dynamics::BodyType::Static) return false;
        if (body->type == new_type) return true;

        body->type = new_type;
        if (new_type == dynamics::BodyType::Dynamic) {
            body->mass = mass;
            body->inv_mass = mass > 0.0f ? 1.0f / mass : 0.0f;
            const collision::Shape* shape = get_shape(id);
            if (shape) body->inv_inertia_local = dynamics::inertia_for_shape(*shape, mass);
        }
        // inv_inertia_world self-corrects to zero for a non-Dynamic body on the very next
        // substep (dynamics::update_world_inertia()'s own type gate) -- no action needed here.
        return true;
    }

    /** @brief The layer matrix consulted before any narrowphase work. */
    broadphase::LayerMatrix& layers() { return layer_matrix_; }
    const broadphase::LayerMatrix& layers() const { return layer_matrix_; }

    /**
     * @brief Installs the JobEngine generate_manifolds_() dispatches its broadphase-bounds,
     *        pair-discovery and narrowphase passes onto (see those methods' docs). Pass
     *        nullptr (the default) to force fully serial stepping -- e.g. for a headless test
     *        that needs bit-exact single-threaded behavior to compare against.
     */
    void set_job_engine(coopa::job::JobEngine* jobs) { jobs_ = jobs; }
    coopa::job::JobEngine* job_engine() const { return jobs_; }

    /** @brief Minimum item count (bodies for bounds/pairs, pairs for narrowphase) before a
     *         stage dispatches to job_engine() instead of running inline on the calling
     *         thread -- mirrors the should_parallelize_() idiom used elsewhere in this
     *         workspace (see toyengine's ToyRenderPipeline). */
    void set_parallel_threshold(std::size_t n) { parallel_threshold_ = n; }
    std::size_t parallel_threshold() const { return parallel_threshold_; }

    // --- Body lifecycle (deferrable -- safe to call from an on_substep slot) ---

    /**
     * @brief Creates a body, deferring the actual insertion to the top of the next
     *        step_fixed() if called while a substep is in progress (i.e. from an on_substep
     *        slot). When deferred, returns an invalid BodyId -- a substep callback cannot
     *        learn the new body's id synchronously; give it a stable external key of your
     *        own (e.g. store it in a member the callback owns) if you need to find it later.
     *        Outside step_fixed(), behaves exactly like add_body().
     */
    dynamics::BodyId create_body(const dynamics::Body& initial, const collision::Shape& shape = collision::Shape{}) {
        if (!in_step_) return add_body(initial, shape);
        DeferredCommand cmd;
        cmd.type = DeferredCommand::Type::Create;
        cmd.body = initial;
        cmd.shape = shape;
        deferred_commands_.push_back(cmd);
        return dynamics::BodyId{};
    }

    /**
     * @brief Destroys a body, deferring the actual slot free to the top of the next
     *        step_fixed() if called from an on_substep slot -- but invalidates any of THIS
     *        step's already-generated manifolds referencing `id` immediately, so the solver
     *        never solves a constraint against a body destroyed mid-step. Outside
     *        step_fixed(), behaves exactly like remove_body().
     */
    void destroy_body(dynamics::BodyId id) {
        if (!in_step_) {
            remove_body(id);
            return;
        }
        if (!is_valid(id)) return;
        for (auto& m : manifolds_) {
            if (m.valid && (m.a == id || m.b == id)) m.valid = false;
        }
        for (auto& j : joints_) {
            if (j.valid && (j.a == id || j.b == id)) j.valid = false;
        }
        DeferredCommand cmd;
        cmd.type = DeferredCommand::Type::Destroy;
        cmd.target = id;
        deferred_commands_.push_back(cmd);
    }

    /** @brief True if `id` addresses a currently-alive body (index in range, generation matches). */
    bool is_valid(dynamics::BodyId id) const {
        return id.is_valid() && id.index < bodies_.size() && alive_[id.index] &&
               generations_[id.index] == id.generation;
    }

    /** @brief Returns the body `id` addresses, or nullptr if `id` is invalid/stale. */
    dynamics::Body* get_body(dynamics::BodyId id) {
        return is_valid(id) ? &bodies_[id.index] : nullptr;
    }
    const dynamics::Body* get_body(dynamics::BodyId id) const {
        return is_valid(id) ? &bodies_[id.index] : nullptr;
    }

    /** @brief Returns `id`'s PRIMARY shape (see body_shapes_'s doc), or nullptr if `id` is
     *         invalid/stale or currently shapeless. For a compound body's other shapes, use
     *         shapes_of()/shape_at() to get the slot, then get_shape_at(). */
    collision::Shape* get_shape(dynamics::BodyId id) {
        uint32_t slot = shape_at(id);
        return slot != k_invalid_shape ? &shapes_[slot] : nullptr;
    }
    const collision::Shape* get_shape(dynamics::BodyId id) const {
        uint32_t slot = shape_at(id);
        return slot != k_invalid_shape ? &shapes_[slot] : nullptr;
    }

    /** @brief Returns the shape at slot `shape_index` (as returned by add_shape()/shape_at()),
     *         or nullptr if the slot is out of range or currently free. */
    collision::Shape* get_shape_at(uint32_t shape_index) {
        return (shape_index < shapes_.size() && shape_alive_[shape_index]) ? &shapes_[shape_index] : nullptr;
    }
    const collision::Shape* get_shape_at(uint32_t shape_index) const {
        return (shape_index < shapes_.size() && shape_alive_[shape_index]) ? &shapes_[shape_index] : nullptr;
    }

    // --- Joints ---

    /**
     * @brief Creates a hinge joint between `a` and `b`, returning a generational handle. Both
     *        must already be valid bodies (no-op, returns an invalid JointId, otherwise).
     *        `local_anchor_a`/`local_axis_a` are in `a`'s own local frame, `local_anchor_b`/
     *        `local_axis_b` in `b`'s -- exactly like collision::Shape's local_center/
     *        local_rotation (shape.h), rotated/translated by each body's own current pose to
     *        get the world-space constraint target every solve (see
     *        dynamics::solve_joint_velocity_pass()'s doc).
     *
     * `Joint::rest_relative_rotation` (the reference angle limits are measured from) is
     * captured automatically from the two bodies' CURRENT orientations at the moment this is
     * called -- matching Unity's HingeJoint.limits, which are relative to the joint's initial
     * relative orientation, not some externally-authored zero.
     *
     * @param use_limits When true, rotation about the hinge axis is clamped to
     *                    [min_angle, max_angle] (radians) -- `min_angle == max_angle` (0 is the
     *                    natural choice) locks the one remaining DOF entirely, giving a fully
     *                    rigid attachment with no separate "fixed joint" implementation needed.
     * @param collide_connected When false (the default), `a` and `b` never generate contacts
     *                          against each other -- see Joint::collide_connected's own doc
     *                          for why that's the sane default for a hinge specifically (its two
     *                          bodies are EXPECTED to overlap right at the anchor).
     */
    dynamics::JointId add_hinge_joint(dynamics::BodyId a, dynamics::BodyId b, const glm::vec3& local_anchor_a,
                                       const glm::vec3& local_anchor_b, const glm::vec3& local_axis_a,
                                       const glm::vec3& local_axis_b, bool use_limits = false, float min_angle = 0.0f,
                                       float max_angle = 0.0f, bool collide_connected = false) {
        const dynamics::Body* ba = get_body(a);
        const dynamics::Body* bb = get_body(b);
        if (!ba || !bb) return dynamics::JointId{};

        dynamics::Joint j;
        j.type = dynamics::JointType::Hinge;
        j.a = a;
        j.b = b;
        j.local_anchor_a = local_anchor_a;
        j.local_anchor_b = local_anchor_b;
        j.local_axis_a = local_axis_a;
        j.local_axis_b = local_axis_b;
        j.use_limits = use_limits;
        j.min_angle = min_angle;
        j.max_angle = max_angle;
        j.collide_connected = collide_connected;
        j.rest_relative_rotation = glm::inverse(ba->orientation) * bb->orientation;
        return insert_joint_(j);
    }

    /**
     * @brief Creates a ball-and-socket joint: `a`'s `local_anchor_a` and `b`'s `local_anchor_b`
     *        are held together, every rotation stays free. Same body/frame conventions as
     *        add_hinge_joint(); returns an invalid JointId if either body is invalid.
     */
    dynamics::JointId add_ball_joint(dynamics::BodyId a, dynamics::BodyId b, const glm::vec3& local_anchor_a,
                                      const glm::vec3& local_anchor_b, bool collide_connected = false) {
        const dynamics::Body* ba = get_body(a);
        const dynamics::Body* bb = get_body(b);
        if (!ba || !bb) return dynamics::JointId{};
        dynamics::Joint j;
        j.type = dynamics::JointType::Ball;
        j.a = a;
        j.b = b;
        j.local_anchor_a = local_anchor_a;
        j.local_anchor_b = local_anchor_b;
        j.collide_connected = collide_connected;
        j.rest_relative_rotation = glm::inverse(ba->orientation) * bb->orientation;
        return insert_joint_(j);
    }

    /**
     * @brief Creates a cone-twist joint (a shoulder, hip or neck): the anchors are held together
     *        like a ball joint, `b`'s twist axis may swing at most `swing_limit` radians away from
     *        `a`'s (a cone; negative = free), and twist about that axis stays within
     *        [twist_min, twist_max] radians of the rest twist (`twist_min > twist_max` = free).
     *
     * `local_twist_axis_a` is in `a`'s frame; `b`'s matching axis is derived from the bodies'
     * CURRENT relative orientation, which also becomes the rest pose both limits are measured
     * from (like add_hinge_joint()). For a rest pose other than the current one, fill a
     * dynamics::Joint yourself and call add_joint().
     */
    dynamics::JointId add_cone_twist_joint(dynamics::BodyId a, dynamics::BodyId b, const glm::vec3& local_anchor_a,
                                            const glm::vec3& local_anchor_b, const glm::vec3& local_twist_axis_a,
                                            float swing_limit, float twist_min, float twist_max,
                                            bool collide_connected = false) {
        const dynamics::Body* ba = get_body(a);
        const dynamics::Body* bb = get_body(b);
        if (!ba || !bb) return dynamics::JointId{};
        dynamics::Joint j;
        j.type = dynamics::JointType::ConeTwist;
        j.a = a;
        j.b = b;
        j.local_anchor_a = local_anchor_a;
        j.local_anchor_b = local_anchor_b;
        j.rest_relative_rotation = glm::inverse(ba->orientation) * bb->orientation;
        j.local_axis_a = glm::normalize(local_twist_axis_a);
        j.local_axis_b = glm::inverse(j.rest_relative_rotation) * j.local_axis_a;
        j.swing_limit = swing_limit;
        j.twist_min = twist_min;
        j.twist_max = twist_max;
        j.collide_connected = collide_connected;
        return insert_joint_(j);
    }

    /**
     * @brief Inserts a fully described joint as-is -- every field, including
     *        `rest_relative_rotation` and both local axes, is the caller's (e.g. a ragdoll
     *        building its joints from the rig's bind pose while the bones are posed elsewhere).
     *        Accumulators are reset. Returns an invalid JointId if either body is invalid.
     */
    dynamics::JointId add_joint(const dynamics::Joint& desc) {
        if (!get_body(desc.a) || !get_body(desc.b)) return dynamics::JointId{};
        dynamics::Joint j = desc;
        j.point_impulse = glm::vec3(0.0f);
        j.axis_impulse = glm::vec2(0.0f);
        j.limit_impulse = 0.0f;
        j.swing_impulse = 0.0f;
        return insert_joint_(j);
    }

    /** @brief True if `id` addresses a currently-alive joint. */
    bool is_valid(dynamics::JointId id) const {
        return id.is_valid() && id.index < joints_.size() && joint_alive_[id.index] &&
               joint_generations_[id.index] == id.generation;
    }

    /** @brief Destroys a joint immediately. A no-op if `id` is already invalid/stale. */
    void remove_joint(dynamics::JointId id) {
        if (!is_valid(id)) return;
        // valid = false too: the solver passes walk joints_ directly and gate on `valid`, so a
        // removed joint must stop acting immediately, not linger until its slot is reused.
        joints_[id.index].valid = false;
        joint_alive_[id.index] = false;
        ++joint_generations_[id.index];
        free_joint_indices_.push_back(id.index);
    }

    /** @brief Returns the joint `id` addresses, or nullptr if `id` is invalid/stale -- mutable
     *         access is intentional (e.g. a caller adjusting limits at runtime). */
    dynamics::Joint* get_joint(dynamics::JointId id) {
        return is_valid(id) ? &joints_[id.index] : nullptr;
    }
    const dynamics::Joint* get_joint(dynamics::JointId id) const {
        return is_valid(id) ? &joints_[id.index] : nullptr;
    }

    /**
     * @brief Turns contacts between two specific bodies off (`collide == false`) or back on,
     *        regardless of layers -- for bodies that overlap by design without being jointed to
     *        each other (a ragdoll's grandparent/sibling bones around a shoulder or hip). Either
     *        body being removed forgets the pair.
     */
    void set_pair_collision(dynamics::BodyId a, dynamics::BodyId b, bool collide) {
        if (!is_valid(a) || !is_valid(b) || a == b) return;
        const uint64_t key = pair_key_(a.index, b.index);
        if (collide) ignored_pairs_.erase(key);
        else ignored_pairs_.insert(key);
    }
    /** @brief False when set_pair_collision() turned this pair's contacts off. */
    bool pair_collides(dynamics::BodyId a, dynamics::BodyId b) const {
        return ignored_pairs_.empty() || ignored_pairs_.count(pair_key_(a.index, b.index)) == 0;
    }

    // --- Cloth -------------------------------------------------------------------------------

    /**
     * @brief Takes ownership of a cloth and starts simulating it.
     *
     * Build the sheet with cloth::make_grid_cloth(), pin it with cloth::pin_to_body() /
     * cloth::pin_static(), then cloth::build_tethers(), and hand the finished Cloth here. That
     * split is deliberate: construction allocates freely and runs Dijkstra, none of which belongs
     * anywhere near step_fixed(), so it happens once up front against a plain value type that
     * needs no world at all.
     *
     * @param c Cloth to move in. Its `valid` flag is set by this call.
     * @return A generational handle, or an invalid one if `c` has no particles.
     */
    cloth::ClothId add_cloth(cloth::Cloth&& c) {
        if (c.particles.empty()) return cloth::ClothId{};
        c.valid = true;
        c.bounds = cloth::compute_bounds(c);

        uint32_t index;
        if (!free_cloth_indices_.empty()) {
            index = free_cloth_indices_.back();
            free_cloth_indices_.pop_back();
            cloths_[index] = std::move(c);
        } else {
            index = static_cast<uint32_t>(cloths_.size());
            cloths_.push_back(std::move(c));
            cloth_generations_.push_back(0);
            cloth_alive_.push_back(false);
        }
        cloth_alive_[index] = true;
        return cloth::ClothId{index, cloth_generations_[index]};
    }

    /** @brief True if `id` addresses a currently-alive cloth. */
    bool is_valid(cloth::ClothId id) const {
        return id.is_valid() && id.index < cloths_.size() && cloth_alive_[id.index] &&
               cloth_generations_[id.index] == id.generation;
    }

    /** @brief Destroys a cloth immediately. A no-op if `id` is already invalid/stale. The slot's
     *         particle/constraint vectors are cleared so their memory is released rather than
     *         held until the slot is recycled by a differently-sized sheet. */
    void remove_cloth(cloth::ClothId id) {
        if (!is_valid(id)) return;
        cloths_[id.index] = cloth::Cloth{};
        cloth_alive_[id.index] = false;
        ++cloth_generations_[id.index];
        free_cloth_indices_.push_back(id.index);
    }

    /** @brief Returns the cloth `id` addresses, or nullptr if `id` is invalid/stale. Mutable
     *         access is intentional -- retuning ClothParams at runtime (wind, friction) needs no
     *         rebuild, and a renderer reads the particle array through the const overload. */
    cloth::Cloth* get_cloth(cloth::ClothId id) {
        return is_valid(id) ? &cloths_[id.index] : nullptr;
    }
    const cloth::Cloth* get_cloth(cloth::ClothId id) const {
        return is_valid(id) ? &cloths_[id.index] : nullptr;
    }

    /** @brief Number of currently-alive cloths. */
    std::size_t cloth_count() const {
        std::size_t n = 0;
        for (std::size_t i = 0; i < cloths_.size(); ++i) if (cloth_alive_[i]) ++n;
        return n;
    }

    /** @brief Invokes `fn(ClothId, Cloth&)` for every currently-alive cloth, in index order. */
    template <typename Fn>
    void for_each_cloth(Fn&& fn) {
        for (uint32_t i = 0; i < cloths_.size(); ++i) {
            if (!cloth_alive_[i]) continue;
            fn(cloth::ClothId{i, cloth_generations_[i]}, cloths_[i]);
        }
    }
    template <typename Fn>
    void for_each_cloth(Fn&& fn) const {
        for (uint32_t i = 0; i < cloths_.size(); ++i) {
            if (!cloth_alive_[i]) continue;
            fn(cloth::ClothId{i, cloth_generations_[i]}, cloths_[i]);
        }
    }

    // --- Hair strands ---------------------------------------------------------------------
    // The same shape as the cloth API above, and for the same reasons: a groom is a persistent,
    // independently-created object with no 1:1 relationship to any body, and it is one-way
    // coupled so nothing downstream ever reads it mid-step. See hair/strand.h.

    /**
     * @brief Takes ownership of a strand set and begins solving it.
     * @param set Built by hair::make_strands(). Its `valid` flag is set by this call.
     * @return A generational handle, or an invalid one if `set` has no particles.
     */
    hair::StrandSetId add_strands(hair::StrandSet&& set) {
        if (set.particles.empty() || set.strands.empty()) return hair::StrandSetId{};
        set.valid = true;
        set.bounds = hair::compute_bounds(set);

        uint32_t index;
        if (!free_strand_indices_.empty()) {
            index = free_strand_indices_.back();
            free_strand_indices_.pop_back();
            strand_sets_[index] = std::move(set);
        } else {
            index = static_cast<uint32_t>(strand_sets_.size());
            strand_sets_.push_back(std::move(set));
            strand_generations_.push_back(0);
            strand_alive_.push_back(false);
        }
        strand_alive_[index] = true;
        return hair::StrandSetId{index, strand_generations_[index]};
    }

    /** @brief True if `id` addresses a currently-alive strand set. */
    bool is_valid(hair::StrandSetId id) const {
        return id.is_valid() && id.index < strand_sets_.size() && strand_alive_[id.index] &&
               strand_generations_[id.index] == id.generation;
    }

    /** @brief Destroys a strand set immediately. A no-op if `id` is already invalid/stale. The
     *         slot's vectors are cleared so their memory is released rather than held until a
     *         differently-sized groom recycles the slot. */
    void remove_strands(hair::StrandSetId id) {
        if (!is_valid(id)) return;
        strand_sets_[id.index] = hair::StrandSet{};
        strand_alive_[id.index] = false;
        ++strand_generations_[id.index];
        free_strand_indices_.push_back(id.index);
    }

    /** @brief The set `id` addresses, or nullptr. Mutable: driving the roots each frame and
     *         retuning StrandParams need no rebuild, and a renderer reads the particles through
     *         the const overload without a copy. */
    hair::StrandSet* get_strands(hair::StrandSetId id) {
        return is_valid(id) ? &strand_sets_[id.index] : nullptr;
    }
    const hair::StrandSet* get_strands(hair::StrandSetId id) const {
        return is_valid(id) ? &strand_sets_[id.index] : nullptr;
    }

    /** @brief Invokes `fn(StrandSetId, StrandSet&)` for every alive set, in index order. */
    template <typename Fn>
    void for_each_strands(Fn&& fn) {
        for (uint32_t i = 0; i < strand_sets_.size(); ++i) {
            if (!strand_alive_[i]) continue;
            fn(hair::StrandSetId{i, strand_generations_[i]}, strand_sets_[i]);
        }
    }
    template <typename Fn>
    void for_each_strands(Fn&& fn) const {
        for (uint32_t i = 0; i < strand_sets_.size(); ++i) {
            if (!strand_alive_[i]) continue;
            fn(hair::StrandSetId{i, strand_generations_[i]}, strand_sets_[i]);
        }
    }

    /** @brief Number of currently-alive strand sets. */
    std::size_t strand_set_count() const {
        std::size_t n = 0;
        for (std::size_t i = 0; i < strand_sets_.size(); ++i) if (strand_alive_[i]) ++n;
        return n;
    }

    /**
     * @brief Invokes `fn(BodyId, Body&)` for every currently-alive body, in index order.
     *
     * Index order (not creation order once slots are recycled) -- fine for anything that
     * doesn't depend on iteration order for determinism, since the body array itself is
     * never reordered by removal (see "Determinism and parallelism" in the plan).
     */
    template <typename Fn>
    void for_each_body(Fn&& fn) {
        for (uint32_t i = 0; i < bodies_.size(); ++i) {
            if (alive_[i]) fn(dynamics::BodyId{i, generations_[i]}, bodies_[i]);
        }
    }
    template <typename Fn>
    void for_each_body(Fn&& fn) const {
        for (uint32_t i = 0; i < bodies_.size(); ++i) {
            if (alive_[i]) fn(dynamics::BodyId{i, generations_[i]}, bodies_[i]);
        }
    }

    /** @brief Number of currently-alive bodies. */
    size_t alive_body_count() const {
        size_t n = 0;
        for (bool a : alive_) n += a ? 1 : 0;
        return n;
    }

    /** @brief This step's contact manifolds (valid between step_fixed() calls, for debug draw
     *         and diagnostics -- do not mutate). */
    const std::vector<collision::ContactManifold>& manifolds() const { return manifolds_; }

    /**
     * @brief This frame's collision/trigger enter/stay/exit events -- accumulated across every
     *        substep step() ran, cleared at the top of the next step() call. Drain (read, then
     *        act on) once per frame, after step() returns, same as the plan's "Events are
     *        queued during the substep loop and emitted once at the end of execute()" -- the
     *        emitting (firing coopa::event::Signal callbacks) belongs to the caller (e.g.
     *        PhysicsSystem), since PhysicsWorld itself has no Collider/SceneObject to fire on.
     *        A caller driving step_fixed() directly (test.cpp, the determinism harness) is
     *        responsible for its own clear_events() between checks if it cares about
     *        per-substep rather than accumulated deltas.
     */
    const std::vector<collision::ContactEvent>& events() const { return events_; }

    /** @brief Clears the accumulated event queue -- step() already does this each call; only
     *         needed by a caller driving step_fixed() directly across multiple checks. */
    void clear_events() { events_.clear(); }

    // --- Queries ---
    //
    // Meant to be used between phases. From inside on_substep they see poses and a broadphase
    // that are consistent with each other (generate_manifolds_() refits the trees just before the
    // emit) but velocities and contacts that this substep has not solved yet -- see queries.h's
    // file doc for the full statement, and vehicle/vehicle_sim.h for the caller that wants it.
    // Every query takes a query::QueryFilter (layer mask, trigger inclusion, one body to
    // ignore, an optional predicate -- see its doc); the `(layer_mask, include_triggers)`
    // overloads are the original shorthand and behave exactly like a filter with only those two
    // fields set. `layer_mask` follows Unity's convention: bit `shape.layer` of the mask, not the
    // LayerMatrix used for solver pair rejection -- a query has no "other side" to look up a
    // collision-matrix entry against. `include_triggers` (default true -- a trigger collider
    // has a normal broadphase proxy like any other) lets a caller exclude trigger colliders,
    // e.g. a raycast that should only ever see solid geometry.

    /** @brief Closest hit along `ray`, or false if nothing was hit. */
    bool raycast(const geometry::Ray& ray, query::RaycastHit& hit, const query::QueryFilter& filter) const {
        bool found = false;
        float best_t = ray.max_distance;
        auto visit = [&](void* user_data) {
            uint32_t i = user_data_to_index_(user_data);
            const dynamics::BodyId owner = shape_owner_[i];
            if (!filter.accepts(shapes_[i], owner)) return;
            const dynamics::Body& body = bodies_[owner.index];
            geometry::Ray clipped = ray;
            clipped.max_distance = best_t;
            float t;
            glm::vec3 n;
            if (!query::raycast_shape(clipped, shapes_[i], body.position, body.orientation, t, n)) return;
            best_t = t;
            hit = query::RaycastHit{};
            hit.point = ray.origin + ray.direction * t;
            hit.normal = n;
            hit.distance = t;
            hit.body = owner;
            hit.shape_index = i;
            found = true;
        };
        dynamic_tree_.raycast(ray, visit);
        static_tree_.raycast(ray, visit);
        return found;
    }
    bool raycast(const geometry::Ray& ray, query::RaycastHit& hit, uint32_t layer_mask = ~0u,
                 bool include_triggers = true) const {
        return raycast(ray, hit, filter_(layer_mask, include_triggers));
    }

    /**
     * @brief True if ANYTHING along `ray` is hit, stopping at the first tree-traversal hit --
     *        NOT necessarily the closest one (unlike raycast()). A distinct, cheaper query for
     *        "is anything blocking this line" checks that don't care which thing is in the way,
     *        built on AABBTree::raycast_until()'s early-out traversal.
     */
    bool raycast_any(const geometry::Ray& ray, const query::QueryFilter& filter) const {
        bool found = false;
        auto visit = [&](void* user_data) -> bool {
            uint32_t i = user_data_to_index_(user_data);
            if (!filter.accepts(shapes_[i], shape_owner_[i])) return false;
            const dynamics::Body& body = bodies_[shape_owner_[i].index];
            float t;
            glm::vec3 n;
            if (!query::raycast_shape(ray, shapes_[i], body.position, body.orientation, t, n)) return false;
            found = true;
            return true; // stop the traversal -- this is the one difference from raycast_all()
        };
        dynamic_tree_.raycast_until(ray, visit);
        if (!found) static_tree_.raycast_until(ray, visit);
        return found;
    }
    bool raycast_any(const geometry::Ray& ray, uint32_t layer_mask = ~0u, bool include_triggers = true) const {
        return raycast_any(ray, filter_(layer_mask, include_triggers));
    }

    /** @brief Every hit along `ray`, sorted nearest-first (unlike raycast(), not clipped to the
     *         closest hit as it goes, since every overlapping body along the ray is wanted). */
    std::vector<query::RaycastHit> raycast_all(const geometry::Ray& ray, const query::QueryFilter& filter) const {
        std::vector<query::RaycastHit> hits;
        auto visit = [&](void* user_data) {
            uint32_t i = user_data_to_index_(user_data);
            const dynamics::BodyId owner = shape_owner_[i];
            if (!filter.accepts(shapes_[i], owner)) return;
            const dynamics::Body& body = bodies_[owner.index];
            float t;
            glm::vec3 n;
            if (!query::raycast_shape(ray, shapes_[i], body.position, body.orientation, t, n)) return;
            query::RaycastHit hit;
            hit.point = ray.origin + ray.direction * t;
            hit.normal = n;
            hit.distance = t;
            hit.body = owner;
            hit.shape_index = i;
            hits.push_back(hit);
        };
        dynamic_tree_.raycast(ray, visit);
        static_tree_.raycast(ray, visit);
        std::sort(hits.begin(), hits.end(), [](const query::RaycastHit& a, const query::RaycastHit& b) {
            return a.distance < b.distance;
        });
        return hits;
    }
    std::vector<query::RaycastHit> raycast_all(const geometry::Ray& ray, uint32_t layer_mask = ~0u,
                                                bool include_triggers = true) const {
        return raycast_all(ray, filter_(layer_mask, include_triggers));
    }

    /**
     * @brief Sweeps convex `shape` (Sphere, Capsule or Box -- its local_center/local_rotation/
     *        capsule_axis honoured as for a body's shape) from pose `pos`/`rot` along `dir`, up to
     *        `max_distance`, without rotating, and reports the FIRST thing it would touch.
     *
     * Exact against every target type, meshes included (BVH-narrowed per triangle):
     * conservative advancement for round cast shapes, a swept separating-axis test for box vs
     * box/triangle -- see query/sweep.h's file doc. `hit.distance` is how far the shape can
     * travel before touching (within query::k_sweep_tolerance), `hit.normal` the contact normal
     * (target toward cast shape), `hit.point` a contact point on the target, `hit.shape_index`
     * the exact shape slot hit.
     *
     * A target already touching/overlapping the shape at `pos` counts only when `dir` leads into
     * it (distance 0, `started_inside` when genuinely overlapping) -- a shape resting against
     * something can still be swept along or away from it. Use compute_penetration() to resolve
     * an overlap first.
     *
     * @param dir Travel direction; normalized here (false for a zero vector).
     * @return False if nothing is hit within `max_distance`, or `shape` is a TriangleMesh or a
     *         ConvexHull -- see query/sweep.h's file doc for why hull casts are a documented
     *         gap (on either side of the pair) rather than an approximation.
     */
    bool shape_cast(const collision::Shape& shape, const glm::vec3& pos, const glm::quat& rot, const glm::vec3& dir,
                    float max_distance, query::RaycastHit& hit, const query::QueryFilter& filter = {}) const {
        if (shape.type == collision::ShapeType::TriangleMesh || shape.type == collision::ShapeType::ConvexHull ||
            !(max_distance >= 0.0f)) return false;
        float dir_len = glm::length(dir);
        if (dir_len < 1e-8f) return false;
        const glm::vec3 unit_dir = dir / dir_len;

        geometry::AABB swept = geometry::AABB::merge(collision::world_bounds(shape, pos, rot),
                                                     collision::world_bounds(shape, pos + unit_dir * max_distance, rot));
        bool found = false;
        float best = max_distance;
        query_aabb_(swept, filter, [&](uint32_t i) {
            if (found && best <= 0.0f) return; // nothing beats a distance-0 hit
            const dynamics::BodyId owner = shape_owner_[i];
            const dynamics::Body& body = bodies_[owner.index];
            query::SweepResult r;
            if (!query::sweep_shape(shape, pos, rot, unit_dir, best, shapes_[i], body.position, body.orientation, r)) return;
            if (found && r.distance >= best) return;
            best = r.distance;
            hit = query::RaycastHit{};
            hit.point = r.point;
            hit.normal = r.normal;
            hit.distance = r.distance;
            hit.body = owner;
            hit.shape_index = i;
            hit.started_inside = r.started_inside;
            found = true;
        });
        return found;
    }

    /** @brief Casts a sphere of `radius` from `origin` along `dir`, up to `max_distance` --
     *         shape_cast() with a sphere (exact against every target, see its doc). */
    bool sphere_cast(const glm::vec3& origin, float radius, const glm::vec3& dir, float max_distance,
                      query::RaycastHit& hit, const query::QueryFilter& filter) const {
        return shape_cast(collision::Shape::make_sphere(radius), origin, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), dir,
                          max_distance, hit, filter);
    }
    bool sphere_cast(const glm::vec3& origin, float radius, const glm::vec3& dir, float max_distance,
                      query::RaycastHit& hit, uint32_t layer_mask = ~0u, bool include_triggers = true) const {
        return sphere_cast(origin, radius, dir, max_distance, hit, filter_(layer_mask, include_triggers));
    }

    /** @brief Casts a `half_extents`-sized box from `origin` along `dir`, up to `max_distance`,
     *         keeping `orientation` fixed throughout the sweep (no tumbling mid-cast) --
     *         shape_cast() with a box. */
    bool box_cast(const glm::vec3& origin, const glm::vec3& half_extents, const glm::quat& orientation,
                  const glm::vec3& dir, float max_distance, query::RaycastHit& hit,
                  const query::QueryFilter& filter) const {
        return shape_cast(collision::Shape::make_box(half_extents), origin, orientation, dir, max_distance, hit, filter);
    }
    bool box_cast(const glm::vec3& origin, const glm::vec3& half_extents, const glm::quat& orientation,
                  const glm::vec3& dir, float max_distance, query::RaycastHit& hit,
                  uint32_t layer_mask = ~0u, bool include_triggers = true) const {
        return box_cast(origin, half_extents, orientation, dir, max_distance, hit, filter_(layer_mask, include_triggers));
    }

    /** @brief Casts a capsule (`radius`, `half_height` along `direction_axis` -- 0=X, 1=Y, 2=Z,
     *         matching CapsuleCollider's own convention) from `origin` along `dir`, up to
     *         `max_distance`, keeping `orientation` fixed -- shape_cast() with a capsule. */
    bool capsule_cast(const glm::vec3& origin, float radius, float half_height, int direction_axis,
                       const glm::quat& orientation, const glm::vec3& dir, float max_distance,
                       query::RaycastHit& hit, const query::QueryFilter& filter) const {
        return shape_cast(collision::Shape::make_capsule(radius, half_height, direction_axis), origin, orientation, dir,
                          max_distance, hit, filter);
    }
    bool capsule_cast(const glm::vec3& origin, float radius, float half_height, int direction_axis,
                       const glm::quat& orientation, const glm::vec3& dir, float max_distance,
                       query::RaycastHit& hit, uint32_t layer_mask = ~0u, bool include_triggers = true) const {
        return capsule_cast(origin, radius, half_height, direction_axis, orientation, dir, max_distance, hit,
                            filter_(layer_mask, include_triggers));
    }

    /**
     * @brief Every overlap between convex `shape` (Sphere, Capsule, Box or ConvexHull at
     *        `pos`/`rot`) and the
     *        world, as minimum-translation vectors: moving the shape by `normal * depth` clears
     *        that target (see query::Penetration). Candidates come from both broadphase trees;
     *        each runs the same pairwise contact math as the solver (collision::generate_contacts,
     *        or the BVH-narrowed per-triangle path for meshes, internal-edge-corrected), so a
     *        flat tessellated floor reports one upward entry, not one per triangle -- entries of
     *        one mesh whose normals agree are merged, keeping the deepest. Only positive depths
     *        are reported. A character motor resolves these (deepest first, re-querying) before
     *        sweeping; pass a filter that ignores the motor's own body and excludes triggers.
     */
    std::vector<query::Penetration> compute_penetration(const collision::Shape& shape, const glm::vec3& pos,
                                                        const glm::quat& rot,
                                                        const query::QueryFilter& filter = {}) const {
        std::vector<query::Penetration> out;
        if (shape.type == collision::ShapeType::TriangleMesh) return out;
        query_aabb_(collision::world_bounds(shape, pos, rot), filter, [&](uint32_t i) {
            const dynamics::BodyId owner = shape_owner_[i];
            const dynamics::Body& body = bodies_[owner.index];
            const collision::Shape& target = shapes_[i];
            if (target.type == collision::ShapeType::TriangleMesh) {
                const std::size_t first = out.size();
                query::mesh_penetrations(shape, pos, rot, target, body.position, body.orientation,
                                         [&](const glm::vec3& normal, float depth, const glm::vec3& point) {
                                             for (std::size_t k = first; k < out.size(); ++k) {
                                                 if (glm::dot(out[k].normal, normal) > 0.999f) {
                                                     if (depth > out[k].depth) {
                                                         out[k].depth = depth;
                                                         out[k].point = point;
                                                     }
                                                     return;
                                                 }
                                             }
                                             out.push_back(query::Penetration{normal, depth, point, owner, i});
                                         });
                return;
            }
            glm::vec3 normal, point;
            float depth;
            if (query::convex_penetration(shape, pos, rot, target, body.position, body.orientation, normal, depth, point))
                out.push_back(query::Penetration{normal, depth, point, owner, i});
        });
        return out;
    }

    /** @brief Every body whose shape overlaps a world-space sphere -- for a compound body
     *         (more than one shape), its BodyId appears once per CHILD shape that overlaps, so
     *         it can appear more than once (Unity's own overlap semantics are per-collider, not
     *         per-body; this is the closest match without a new return type -- see the plan's
     *         Phase 5 doc for why that trade was made). */
    std::vector<dynamics::BodyId> overlap_sphere(const glm::vec3& center, float radius,
                                                  const query::QueryFilter& filter) const {
        geometry::Sphere query_sphere{center, radius};
        geometry::AABB bounds;
        bounds.min = center - glm::vec3(radius);
        bounds.max = center + glm::vec3(radius);

        std::vector<dynamics::BodyId> results;
        query_aabb_(bounds, filter, [&](uint32_t i) {
            const dynamics::Body& body = bodies_[shape_owner_[i].index];
            if (query::shape_overlaps_sphere(shapes_[i], body.position, body.orientation, query_sphere)) {
                results.push_back(shape_owner_[i]);
            }
        });
        return results;
    }
    std::vector<dynamics::BodyId> overlap_sphere(const glm::vec3& center, float radius, uint32_t layer_mask = ~0u,
                                                  bool include_triggers = true) const {
        return overlap_sphere(center, radius, filter_(layer_mask, include_triggers));
    }

    /** @brief Every body whose shape overlaps a world-space OBB -- see overlap_sphere()'s doc
     *         for the per-child-shape duplication a compound body can produce here too. */
    std::vector<dynamics::BodyId> overlap_box(const geometry::OBB& box, const query::QueryFilter& filter) const {
        std::vector<dynamics::BodyId> results;
        query_aabb_(box.bounds(), filter, [&](uint32_t i) {
            const dynamics::Body& body = bodies_[shape_owner_[i].index];
            if (query::shape_overlaps_obb(shapes_[i], body.position, body.orientation, box)) {
                results.push_back(shape_owner_[i]);
            }
        });
        return results;
    }
    std::vector<dynamics::BodyId> overlap_box(const geometry::OBB& box, uint32_t layer_mask = ~0u,
                                               bool include_triggers = true) const {
        return overlap_box(box, filter_(layer_mask, include_triggers));
    }

    /** @brief Every body whose shape overlaps a world-space capsule -- see overlap_sphere()'s
     *         doc for the per-child-shape duplication a compound body can produce here too. */
    std::vector<dynamics::BodyId> overlap_capsule(const geometry::Capsule& capsule,
                                                   const query::QueryFilter& filter) const {
        geometry::AABB bounds;
        bounds.min = glm::min(capsule.a, capsule.b) - glm::vec3(capsule.radius);
        bounds.max = glm::max(capsule.a, capsule.b) + glm::vec3(capsule.radius);

        std::vector<dynamics::BodyId> results;
        query_aabb_(bounds, filter, [&](uint32_t i) {
            const dynamics::Body& body = bodies_[shape_owner_[i].index];
            if (query::shape_overlaps_capsule(shapes_[i], body.position, body.orientation, capsule)) {
                results.push_back(shape_owner_[i]);
            }
        });
        return results;
    }
    std::vector<dynamics::BodyId> overlap_capsule(const geometry::Capsule& capsule, uint32_t layer_mask = ~0u,
                                                   bool include_triggers = true) const {
        return overlap_capsule(capsule, filter_(layer_mask, include_triggers));
    }

    /**
     * @brief Fills `out` with this world's current debug geometry: collider wireframes (colored
     *        white/awake, green/sleeping, yellow/trigger -- trigger takes priority over
     *        sleep state), last step's contact points/normals (red), and both broadphase
     *        trees' node bounds. Pure data -- no Vulkan, no gfxcoopa; the consuming engine
     *        renders `out.lines` however it likes (see the plan's toyengine-integration
     *        section for where that render pass belongs).
     */
    void debug_draw(debug::DebugDraw& out, debug::DebugDrawFlags flags = debug::DebugDrawFlags::All) const {
        if (debug::has_flag(flags, debug::DebugDrawFlags::Colliders)) {
            for (uint32_t i = 0; i < shapes_.size(); ++i) {
                if (!shape_alive_[i] || !shapes_[i].enabled) continue;
                const collision::Shape& shape = shapes_[i];
                const dynamics::Body& body = bodies_[shape_owner_[i].index];
                uint32_t color = shape.is_trigger ? debug::colors::k_trigger
                                                   : (body.awake ? debug::colors::k_awake : debug::colors::k_sleeping);
                debug::add_shape(out, shape, body.position, body.orientation, color);
            }
        }

        if (debug::has_flag(flags, debug::DebugDrawFlags::Contacts)) {
            constexpr float k_normal_length = 0.3f;
            for (const auto& m : manifolds_) {
                if (m.is_trigger) continue;
                for (uint8_t i = 0; i < m.count; ++i) {
                    const glm::vec3& p = m.points[i].position;
                    out.add_line(p, p + m.normal * k_normal_length, debug::colors::k_contact_normal);
                }
            }
        }

        if (debug::has_flag(flags, debug::DebugDrawFlags::Joints)) {
            constexpr float k_axis_length = 0.3f;
            for (const auto& j : joints_) {
                if (!j.valid || !j.enabled) continue;
                const dynamics::Body& a = bodies_[j.a.index];
                const dynamics::Body& b = bodies_[j.b.index];
                glm::vec3 world_anchor_a = a.position + a.orientation * j.local_anchor_a;
                glm::vec3 world_anchor_b = b.position + b.orientation * j.local_anchor_b;
                // The two anchors should be nearly coincident once converged -- drawing both
                // ends (rather than just one) makes any solver drift visually obvious.
                out.add_line(world_anchor_a, world_anchor_b, debug::colors::k_joint);
                if (j.type == dynamics::JointType::Ball) continue; // no axis to show
                glm::vec3 world_axis_a = glm::normalize(a.orientation * j.local_axis_a);
                if (j.type == dynamics::JointType::Hinge) {
                    out.add_line(world_anchor_a - world_axis_a * k_axis_length,
                                 world_anchor_a + world_axis_a * k_axis_length, debug::colors::k_joint);
                    continue;
                }
                // ConeTwist: both twist axes from the anchor (their angle is the swing), plus
                // the limit cone's rim around a's axis when the swing is limited.
                glm::vec3 world_axis_b = glm::normalize(b.orientation * j.local_axis_b);
                out.add_line(world_anchor_a, world_anchor_a + world_axis_a * k_axis_length, debug::colors::k_joint);
                out.add_line(world_anchor_b, world_anchor_b + world_axis_b * k_axis_length, debug::colors::k_joint);
                if (j.has_swing_limit()) {
                    glm::vec3 p, q;
                    util::orthonormal_basis(world_axis_a, p, q);
                    const float limit = std::min(j.swing_limit, 1.55f);
                    const float rim_r = std::sin(limit) * k_axis_length;
                    const glm::vec3 rim_c = world_anchor_a + world_axis_a * (std::cos(limit) * k_axis_length);
                    constexpr int k_segments = 12;
                    for (int s = 0; s < k_segments; ++s) {
                        float t0 = 6.2831853f * static_cast<float>(s) / k_segments;
                        float t1 = 6.2831853f * static_cast<float>(s + 1) / k_segments;
                        out.add_line(rim_c + (p * std::cos(t0) + q * std::sin(t0)) * rim_r,
                                     rim_c + (p * std::cos(t1) + q * std::sin(t1)) * rim_r, debug::colors::k_joint);
                    }
                }
            }
        }

        if (debug::has_flag(flags, debug::DebugDrawFlags::BVH)) {
            auto draw_tree = [&](const broadphase::AABBTree& tree) {
                tree.for_each_node([&](const geometry::AABB& box, int32_t) { out.add_aabb(box, debug::colors::k_bvh_node); });
            };
            draw_tree(static_tree_);
            draw_tree(dynamic_tree_);
        }
    }

    // --- Configuration ---

    void set_gravity(const glm::vec3& g) { gravity_ = g; }
    /** @brief The uniform gravity: what acts wherever no gravity source does. */
    const glm::vec3& gravity() const { return gravity_; }

    /**
     * @struct GravitySource
     * @brief A body that pulls toward its centre -- a planet. Inside `radius` the pull is
     *        `strength` (m/s^2); beyond it, it falls off with the square of the distance; past
     *        `range` (0: everywhere) it stops.
     */
    struct GravitySource {
        glm::vec3 centre{0.0f};
        float strength = 9.81f;   ///< Acceleration at (and under) the surface, m/s^2.
        float radius = 1.0f;      ///< Surface radius: full strength up to here.
        float range = 0.0f;       ///< Reach; 0 is unlimited.
    };

    /** @brief Adds a gravity source; returns its id. */
    uint32_t add_gravity_source(const GravitySource& source) {
        const uint32_t id = next_gravity_id_++;
        gravity_sources_.push_back({id, source});
        return id;
    }
    /** @brief Moves or re-tunes a source (a planet that moves, say). False if it is gone. */
    bool update_gravity_source(uint32_t id, const GravitySource& source) {
        for (auto& entry : gravity_sources_) {
            if (entry.first == id) { entry.second = source; return true; }
        }
        return false;
    }
    void remove_gravity_source(uint32_t id) {
        gravity_sources_.erase(std::remove_if(gravity_sources_.begin(), gravity_sources_.end(),
                                              [id](const auto& e) { return e.first == id; }),
                               gravity_sources_.end());
    }
    std::size_t gravity_source_count() const { return gravity_sources_.size(); }

    /**
     * @brief The gravity acting at a point: the strongest source pulling there, or the uniform
     *        `gravity()` where none reaches. With no sources (every flat scene) it is exactly
     *        `gravity()`.
     */
    glm::vec3 gravity_at(const glm::vec3& p) const {
        if (gravity_sources_.empty()) return gravity_;
        glm::vec3 up;
        return field_at_(p, up);
    }

    /**
     * @brief Which way is up at a point: away from the source acting there (even one pulling
     *        with zero strength, as a planet does until its ground is ready), else against the
     *        uniform gravity (+Z where nothing pulls).
     */
    glm::vec3 up_at(const glm::vec3& p) const {
        if (!gravity_sources_.empty()) {
            glm::vec3 up;
            field_at_(p, up);
            if (glm::length(up) > 0.5f) return up;
        }
        const float len = glm::length(gravity_);
        return len > 1e-6f ? -gravity_ / len : glm::vec3(0.0f, 0.0f, 1.0f);
    }

    util::PhysicsConfig& config() { return config_; }
    const util::PhysicsConfig& config() const { return config_; }

    // --- Stepping ---

    /**
     * @brief Accumulates `dt` and runs zero or more fixed RIGID substeps, then advances every
     *        cloth by one FRAME step.
     *
     * `dt` is clamped to k_max_frame_time before accumulating -- mandatory, not defensive:
     * a consumer's very first frame can include all of window/device/scene initialization.
     *
     * Cloth deliberately runs here, on the frame clock, and NOT inside step_fixed() -- see
     * step_cloths_()'s doc for the full argument. The short version: a cloth is drawn from its
     * raw solver positions with no render interpolation, while the kinematic/static bodies it
     * drapes over are drawn wherever the wall clock put them this frame. Solving the sheet on
     * the fixed grid would make the two disagree about what time it is by up to one whole
     * frame whenever the accumulator crossed a substep boundary, which reads on screen as the
     * sheet slipping against a moving body and snapping back.
     *
     * @param dt Frame delta time in seconds (unclamped, raw).
     */
    void step(float dt) {
        events_.clear();
        const float frame_dt = std::min(dt, util::k_max_frame_time);
        accumulator_ += frame_dt;
        const float h = config_.fixed_dt;
        uint32_t n = 0;
        while (accumulator_ >= h && n < config_.max_substeps) {
            step_fixed(h);
            accumulator_ -= h;
            ++n;
        }
        interpolation_alpha_ = accumulator_ / h;

        // Cloth, then hair. Both run once per FRAME here rather than inside the substep loop,
        // both query the broadphase for candidate colliders, and both are one-way coupled -- see
        // step_cloths_() and step_strands_() for the full argument.
        const bool soft_bodies = !cloths_.empty() || !strand_sets_.empty();
        if (soft_bodies && frame_dt > 0.0f) {
            // With no substep this frame, nothing has synced the broadphase trees these two
            // query, so their proxies still hold last frame's bounds. The query box absorbs a
            // margin's worth of staleness but not a fast body's whole frame of travel -- sync
            // first rather than rely on that.
            if (n == 0) sync_broadphase_(frame_dt);
            if (!cloths_.empty()) step_cloths_(frame_dt);
            // Hair last, so a groom pinned to something a cloth is draped over still sees the
            // frame's final poses.
            if (!strand_sets_.empty()) step_strands_(frame_dt);
        }
    }

    /**
     * @brief Runs exactly one fixed substep, with no accumulator involved.
     *
     * RIGID bodies only -- cloth is advanced once per FRAME by step(), not once per substep;
     * see step()'s and step_cloths_()'s docs for why. A caller driving this directly (test.cpp,
     * the determinism harness) therefore has to go through step() to advance a cloth at all.
     *
     * Order:
     *   1. drain deferred structural commands queued by last step's on_substep
     *   2. integrate forces (gravity, drag) -> velocities
     *   3. broadphase (two AABB trees + pair cache) + layer/joint reject
     *   4. narrowphase -> this step's manifolds, plus enter/stay/exit events
     *   5. fire on_substep
     *   6. solve: warm start -> velocity+relax iterations -> position correction, then
     *      islands + sleep/wake (all inside dynamics::solve() -- see solver.h)
     *   7. integrate velocities -> positions/orientations
     *
     * @param h Substep length in seconds.
     */
    void step_fixed(float h) {
        drain_deferred_commands_();

        in_step_ = true;

        if (gravity_sources_.empty()) {
            for_each_body([&](dynamics::BodyId, dynamics::Body& b) {
                dynamics::apply_gravity(b, gravity_);
            });
        } else {
            // Each body under whatever pulls where it is (a planet's centre, say).
            for_each_body([&](dynamics::BodyId, dynamics::Body& b) {
                dynamics::apply_gravity(b, gravity_at(b.position));
            });
        }
        for_each_body([&](dynamics::BodyId, dynamics::Body& b) {
            dynamics::integrate_forces(b, h);
        });

        generate_manifolds_(h);

        on_substep.emit(*this, h);

        dynamics::solve(bodies_, alive_, manifolds_, joints_, solver_state_, config_, h);

        for_each_body([&](dynamics::BodyId, dynamics::Body& b) {
            b.prev_position = b.position;
            b.prev_orientation = b.orientation;
            dynamics::integrate_velocities(b, h, config_.use_exact_quaternion_integration,
                                            config_.max_linear_velocity);
            dynamics::update_world_inertia(b);
        });
        if (!joints_.empty()) {
            // Joint drift is corrected on the integrated poses -- see solve()'s note.
            dynamics::solve_joints_post_integrate(bodies_, joints_, config_);
            for_each_body([&](dynamics::BodyId, dynamics::Body& b) { dynamics::update_world_inertia(b); });
        }

        in_step_ = false;
    }

    /** @brief Fractional progress (in [0,1)) through the next substep, for render interpolation. */
    float interpolation_alpha() const { return interpolation_alpha_; }

    /**
     * @brief FNV-1a hash over every alive body's position/orientation/velocity bit patterns.
     *
     * The determinism harness: two independent runs of the same scene must produce the same
     * hash after the same number of equal-sized step()/step_fixed() calls (cloth state is part of
     * the hash and only step() advances it), and the hash must be invariant to
     * spawning and destroying a distant, non-interacting body mid-run (the actual test for
     * pair-ordering nondeterminism -- a same-process double-run alone would pass even with a
     * pointer-keyed hash map in the solve loop, since allocator state repeats too).
     *
     * @return FNV-1a hash of every alive body's kinematic state.
     */
    uint64_t world_state_hash() const {
        uint64_t h = 1469598103934665603ull; // FNV-1a 64-bit offset basis
        auto mix = [&h](const void* data, size_t len) {
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < len; ++i) {
                h ^= bytes[i];
                h *= 1099511628211ull; // FNV-1a 64-bit prime
            }
        };
        for_each_body([&](dynamics::BodyId, const dynamics::Body& b) {
            mix(&b.position, sizeof(b.position));
            mix(&b.orientation, sizeof(b.orientation));
            mix(&b.linear_velocity, sizeof(b.linear_velocity));
            mix(&b.angular_velocity, sizeof(b.angular_velocity));
        });
        // Cloth particles are mixed in too, so the serial-vs-parallel determinism harness covers
        // the cloth pass for free rather than needing a parallel oracle of its own. A world with
        // no cloths mixes in nothing here, so its hash covers bodies alone.
        for_each_cloth([&](cloth::ClothId, const cloth::Cloth& c) {
            for (const cloth::ClothParticle& p : c.particles) {
                mix(&p.position, sizeof(p.position));
                mix(&p.velocity, sizeof(p.velocity));
            }
        });
        return h;
    }

private:
    /** @brief Creates one shape slot's broadphase proxy if the shape is enabled. Static bodies'
     *         shapes go into static_tree_ (built once, never refit here); Dynamic and Kinematic
     *         bodies' shapes go into dynamic_tree_ (refit every step in sync_broadphase_()). A
     *         disabled shape gets no proxy at all -- it never participates in broadphase or
     *         narrowphase (the per-shape form of the "body with no shape" case). user_data packs the SHAPE slot index (see
     *         user_data_to_index_()'s doc) -- resolving which BODY it belongs to is always a
     *         second step (shape_owner_[slot]), never folded into the packed value itself. */
    void create_shape_proxy_(uint32_t slot) {
        const collision::Shape& shape = shapes_[slot];
        if (!shape.enabled) return;
        const dynamics::Body& body = bodies_[shape_owner_[slot].index];
        geometry::AABB bounds = collision::world_bounds(shape, body.position, body.orientation);
        void* user_data = index_to_user_data_(slot);
        if (body.type == dynamics::BodyType::Static) {
            tree_proxy_[slot] = static_tree_.create_proxy(bounds, user_data);
            in_static_tree_[slot] = true;
        } else {
            tree_proxy_[slot] = dynamic_tree_.create_proxy(bounds, user_data);
            in_static_tree_[slot] = false;
        }
    }

    /** @brief Destroys one shape slot's broadphase proxy, if it has one. */
    void destroy_shape_proxy_(uint32_t slot) {
        if (tree_proxy_[slot] == broadphase::k_null_node) return;
        if (in_static_tree_[slot]) static_tree_.destroy_proxy(tree_proxy_[slot]);
        else dynamic_tree_.destroy_proxy(tree_proxy_[slot]);
        tree_proxy_[slot] = broadphase::k_null_node;
    }

    /** @brief Packs a SHAPE slot index (into shapes_/tree_proxy_/etc -- see their own doc) as
     *         opaque proxy user-data. Deliberately not a body index -- see shapes_'s doc:
     *         broadphase/narrowphase operate on shapes, one level below bodies, and a compound
     *         body owns several. */
    static void* index_to_user_data_(uint32_t index) {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(index));
    }
    static uint32_t user_data_to_index_(void* user_data) {
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user_data));
    }

    /** @brief A shape's smallest characteristic dimension -- the threshold narrowphase_()'s CCD
     *         fast-pair gate compares a substep's motion against. TriangleMesh returns +infinity
     *         (never gates speculative treatment on; meshes are static or kinematic and
     *         speculative mesh contacts are not supported -- see generate_contacts()'s `allow_speculative`
     *         doc), so a mesh's own motion never triggers the gate, though a fast DYNAMIC body
     *         moving toward a static mesh still can via the mesh's own min_extent contributing
     *         infinity to the pairwise std::min() at the call site -- i.e. the OTHER shape's
     *         extent alone decides it, correctly, since only that side can plausibly tunnel. */
    static float shape_min_extent_(const collision::Shape& s) {
        switch (s.type) {
            case collision::ShapeType::Sphere: return s.radius;
            case collision::ShapeType::Box: return std::min({s.half_extents.x, s.half_extents.y, s.half_extents.z});
            case collision::ShapeType::Capsule: return std::min(s.capsule_radius, s.capsule_half_height);
            case collision::ShapeType::TriangleMesh: return std::numeric_limits<float>::max();
            case collision::ShapeType::ConvexHull: {
                if (!s.hull) return std::numeric_limits<float>::max();
                const glm::vec3 half = s.hull->local_bounds.extents() * 0.5f * s.hull_scale;
                return std::min({half.x, half.y, half.z});
            }
        }
        return std::numeric_limits<float>::max();
    }

    /** @brief The query::QueryFilter equivalent of the original `(layer_mask, include_triggers)`
     *         query parameters. */
    static query::QueryFilter filter_(uint32_t layer_mask, bool include_triggers) {
        query::QueryFilter filter;
        filter.layer_mask = layer_mask;
        filter.include_triggers = include_triggers;
        return filter;
    }

    /**
     * @brief Calls `fn(uint32_t shape_slot)` for every shape whose broadphase proxy (fat bounds)
     *        overlaps world-space `bounds` and which `filter` accepts -- both trees, dynamic
     *        first. The shared candidate pass behind the overlap queries, shape_cast() and
     *        compute_penetration(); exact per-shape tests are the caller's.
     */
    template <typename Fn>
    void query_aabb_(const geometry::AABB& bounds, const query::QueryFilter& filter, Fn&& fn) const {
        auto visit = [&](void* user_data) {
            uint32_t i = user_data_to_index_(user_data);
            if (!filter.accepts(shapes_[i], shape_owner_[i])) return;
            fn(i);
        };
        dynamic_tree_.query(bounds, visit);
        static_tree_.query(bounds, visit);
    }

    /**
     * @brief Runs `body(begin, end, JobContext)` over `[0, count)`, dispatched onto job_engine()
     *        when one is installed and `count` clears parallel_threshold_, otherwise run inline
     *        on the calling thread as a single [0, count) chunk -- the same should_parallelize_
     *        idiom ToyRenderPipeline uses. `body` must write only through index-addressed
     *        state (its own chunk's slice of a pre-sized scratch vector, or a per-worker
     *        bucket keyed by JobContext::worker_index) -- never through a shared container
     *        that isn't safe for concurrent mutation.
     */
    template <typename Fn>
    void for_range_(std::size_t count, Fn&& body) {
        if (jobs_ && count >= parallel_threshold_) {
            // Normal help floor -- see PhysicsSystem::for_range_(): the solver's own passes are
            // frame-critical, so the waiting thread must not run background Low work here.
            jobs_->parallel_for_blocking(count, 0, std::forward<Fn>(body), 0,
                                         coopa::job::Priority::Normal, coopa::job::Priority::Normal);
        } else {
            coopa::job::JobContext ctx{};
            body(std::size_t{0}, count, ctx);
        }
    }

    /** @brief Maps a JobContext::worker_index to a dense [0, worker_slots) bucket index --
     *         real workers pass through as-is, and both k_main_thread_index (a guest thread
     *         participating in a blocking wait) and any out-of-range value fall into the last
     *         slot. `worker_slots` is always job_engine()'s worker_count() + 1 when a job
     *         engine is installed (1 otherwise), so the last slot is never a real worker's. */
    static uint32_t bucket_for_(uint32_t worker_index, uint32_t worker_slots) {
        return (worker_index < worker_slots - 1) ? worker_index : worker_slots - 1;
    }

    /** @brief Recomputes bounds_scratch_[i] for every alive, proxied SHAPE (not body -- see
     *         shapes_'s file doc) -- shared by sync_broadphase_() (which only reads the
     *         non-static entries, to move proxies) and discover_pairs_() (which reads every
     *         entry, static included, as query bounds). Pure per-index math over
     *         shapes_/shape_owner_/bodies_, so this is safe to run job-parallel: each iteration
     *         only ever writes its own index. */
    void compute_broadphase_bounds_() {
        if (bounds_scratch_.size() != shapes_.size()) bounds_scratch_.resize(shapes_.size());
        for_range_(shapes_.size(), [&](std::size_t begin, std::size_t end, const coopa::job::JobContext&) {
            for (std::size_t k = begin; k < end; ++k) {
                uint32_t i = static_cast<uint32_t>(k);
                if (!shape_alive_[i] || tree_proxy_[i] == broadphase::k_null_node) continue;
                const dynamics::Body& body = bodies_[shape_owner_[i].index];
                bounds_scratch_[i] = collision::world_bounds(shapes_[i], body.position, body.orientation);
            }
        });
    }

    /** @brief Refits every dynamic-tree proxy (shapes owned by a Dynamic or Kinematic body) to
     *         this step's current world bounds, fattened along its OWNER's direction of travel
     *         by `linear_velocity * h` -- the upcoming substep's motion, not the motion that
     *         already happened (see compute_broadphase_bounds_()'s ordering note: bounds are
     *         computed from the PREVIOUS substep's positions but CURRENT, post-gravity velocity,
     *         so this predicts forward rather than re-describing the past). This is what makes a
     *         fast body's candidate-pair set actually include a thin target it's about to
     *         tunnel through (see AABBTree::move_proxy()'s `displacement` doc). Static proxies never move, so
     *         static_tree_ needs no per-step work. Tree mutation itself stays single-threaded --
     *         only the bounds math feeding it (compute_broadphase_bounds_()) is job-parallel. */
    void sync_broadphase_(float h) {
        compute_broadphase_bounds_();
        for (uint32_t i = 0; i < shapes_.size(); ++i) {
            if (!shape_alive_[i] || tree_proxy_[i] == broadphase::k_null_node || in_static_tree_[i]) continue;
            const dynamics::Body& body = bodies_[shape_owner_[i].index];
            dynamic_tree_.move_proxy(tree_proxy_[i], bounds_scratch_[i], body.linear_velocity * h);
        }
    }

    /**
     * @brief Job-parallel broadphase pair discovery: queries both trees for every alive,
     *        proxied body and records candidate pairs.
     *
     * Static-static, static-kinematic and kinematic-kinematic pairs are never generated:
     * dynamic_tree_ holds both Dynamic and Kinematic bodies (so a self-query against it can
     * surface a kinematic-kinematic pair), filtered out later by narrowphase_()'s
     * `either_dynamic` check; static_tree_ holds only Static
     * bodies, so a dynamic-tree-vs-static-tree cross query can only ever surface dynamic-static
     * or kinematic-static pairs.
     *
     * Each worker (and the calling thread, participating as a guest -- see bucket_for_()) owns
     * a private pair_buckets_ slot, since AABBTree::query() is read-only/thread-safe (its
     * traversal stack is thread_local -- see aabb_tree.h) but PairCache::add() is not. The
     * buckets are concatenated into pair_cache_ afterward in a fixed slot order, and
     * PairCache::finalize()'s sort+dedup makes the result independent of that order and of
     * which worker discovered which pair -- so the result is bit-identical to a serial
     * ascending-index loop regardless of thread count.
     *
     * Iterates SHAPES, not bodies (see shapes_'s file doc) -- `i`/`j` below are shape slot
     * indices, and pair_cache_ ends up holding shape pairs, not body pairs. The self-rejection
     * check is `shape_owner_[i] != shape_owner_[j]` (different OWNING BODIES), not `i != j`
     * (different shapes) -- the latter would let two sibling shapes of the same compound body
     * generate a self-collision pair against each other, which is never physically meaningful.
     */
    void discover_pairs_() {
        uint32_t worker_slots = jobs_ ? jobs_->worker_count() + 1 : 1;
        if (pair_buckets_.size() != worker_slots) pair_buckets_.resize(worker_slots);
        for (auto& bucket : pair_buckets_) bucket.clear();

        for_range_(shapes_.size(), [&](std::size_t begin, std::size_t end, const coopa::job::JobContext& ctx) {
            std::vector<broadphase::ProxyPair>& out = pair_buckets_[bucket_for_(ctx.worker_index, worker_slots)];
            for (std::size_t k = begin; k < end; ++k) {
                uint32_t i = static_cast<uint32_t>(k);
                if (!shape_alive_[i] || tree_proxy_[i] == broadphase::k_null_node) continue;
                const geometry::AABB& bounds = bounds_scratch_[i];
                const dynamics::BodyId owner_i = shape_owner_[i];

                dynamic_tree_.query(bounds, [&](void* user_data) {
                    uint32_t j = user_data_to_index_(user_data);
                    if (owner_i != shape_owner_[j]) out.push_back(broadphase::ProxyPair{std::min(i, j), std::max(i, j)});
                });
                if (!in_static_tree_[i]) {
                    // A static shape's own tree never needs querying against itself; only a
                    // dynamic/kinematic shape queries the static tree, giving each cross pair
                    // exactly one discovery path.
                    static_tree_.query(bounds, [&](void* user_data) {
                        uint32_t j = user_data_to_index_(user_data);
                        if (owner_i != shape_owner_[j]) out.push_back(broadphase::ProxyPair{std::min(i, j), std::max(i, j)});
                    });
                }
            }
        });

        for (const auto& bucket : pair_buckets_) {
            for (const auto& p : bucket) pair_cache_.add(p.a, p.b);
        }
    }

    /**
     * @brief Job-parallel narrowphase: generates a contact manifold for every candidate pair
     *        pair_cache_.current() holds.
     *
     * manifold_scratch_ is pre-sized to pairs.size() and each worker writes only its own index
     * k -- a pure function of pairs[k]/bodies_/shapes_, so results are bit-identical regardless
     * of thread count or scheduling. The serial compaction pass afterward preserves pair-index
     * order (== pair_cache_'s sorted order, deterministic) when building manifolds_ and the
     * collision/trigger pair caches, exactly matching a fully serial loop's result.
     *
     * `pairs[k].a/.b` are SHAPE slot indices (see shapes_'s file doc), not body indices --
     * resolved to their owning BodyId via shape_owner_ below. ContactManifold.a/.b are still
     * BodyIds (the solver only ever acts on bodies), so two different shape-pairs of the same
     * two compound bodies produce two manifolds with identical `.a`/`.b` but different geometry
     * -- exactly the intended, physically correct outcome (each child shape contributes its own
     * independent contact). Event/Collider resolution (PhysicsSystem) only resolves to each
     * body's PRIMARY shape's Collider, not the exact child touched -- a deliberate scope trim;
     * raycast queries above DO resolve the exact child (query::RaycastHit::shape_index).
     */
    void narrowphase_(float h) {
        const std::vector<broadphase::ProxyPair>& pairs = pair_cache_.current();
        if (manifold_scratch_.size() != pairs.size()) manifold_scratch_.resize(pairs.size());

        for_range_(pairs.size(), [&](std::size_t begin, std::size_t end, const coopa::job::JobContext&) {
            for (std::size_t k = begin; k < end; ++k) {
                collision::ContactManifold& m = manifold_scratch_[k];
                m = collision::ContactManifold{};

                uint32_t i = pairs[k].a;
                uint32_t j = pairs[k].b;
                if (i >= shapes_.size() || j >= shapes_.size() || !shape_alive_[i] || !shape_alive_[j]) continue;

                dynamics::BodyId id_i = shape_owner_[i];
                dynamics::BodyId id_j = shape_owner_[j];
                if (!is_valid(id_i) || !is_valid(id_j)) continue;
                const dynamics::Body& bi = bodies_[id_i.index];
                const dynamics::Body& bj = bodies_[id_j.index];
                bool either_dynamic = bi.type == dynamics::BodyType::Dynamic || bj.type == dynamics::BodyType::Dynamic;
                if (!either_dynamic) continue;
                if (!layer_matrix_.should_collide(shapes_[i].layer, shapes_[j].layer)) continue;
                if (joint_blocks_collision_(id_i, id_j)) continue;
                if (!ignored_pairs_.empty() && ignored_pairs_.count(pair_key_(id_i.index, id_j.index))) continue;

                // CCD fast-pair gate: only bother with the (slightly pricier) speculative path
                // when a body's OWN per-substep displacement exceeds its OWN smallest extent --
                // the classic "is this a bullet" test (same criterion Box2D's `IsBullet`/Unity's
                // "Continuous Dynamic" mode use), deliberately NOT compared against the other
                // shape's size. Comparing against min(shape_a, shape_b)'s extent would falsely
                // trigger on completely ordinary fast falls onto any thin static platform (ground
                // slabs are routinely thin) and measurably rob energy from restitution well
                // before actual contact (test_restitution_one_bounces_high guards this). This is a known, accepted
                // trade-off, not unique to this engine: a slow-but-large body CAN still tunnel
                // through an extremely thin (near-zero-thickness) target this gate won't catch,
                // exactly like every other engine using this same per-body heuristic. See
                // collision::generate_contacts()'s `allow_speculative` doc for exactly which
                // shape-type pairs actually use this once triggered.
                float extent_i = shape_min_extent_(shapes_[i]);
                float extent_j = shape_min_extent_(shapes_[j]);
                bool allow_speculative =
                    (extent_i < std::numeric_limits<float>::max() &&
                     glm::length(bi.linear_velocity) * h > extent_i) ||
                    (extent_j < std::numeric_limits<float>::max() &&
                     glm::length(bj.linear_velocity) * h > extent_j);
                collision::generate_contacts(id_i, shapes_[i], bi.position, bi.orientation,
                                              id_j, shapes_[j], bj.position, bj.orientation, m, allow_speculative);
            }
        });

        manifolds_.clear();
        for (std::size_t k = 0; k < manifold_scratch_.size(); ++k) {
            const collision::ContactManifold& m = manifold_scratch_[k];
            if (!m.valid) continue;
            manifolds_.push_back(m);
            (m.is_trigger ? trigger_pair_cache_ : collision_pair_cache_).add(pairs[k].a, pairs[k].b);
        }
    }

    /** @brief Order-independent key for a body-index pair. */
    static uint64_t pair_key_(uint32_t a, uint32_t b) {
        const uint32_t lo = std::min(a, b), hi = std::max(a, b);
        return (static_cast<uint64_t>(lo) << 32) | hi;
    }

    /** @brief Stores `j` (already fully filled in) in a free or new joint slot. */
    dynamics::JointId insert_joint_(dynamics::Joint j) {
        j.enabled = true;
        j.valid = true;
        uint32_t index;
        if (!free_joint_indices_.empty()) {
            index = free_joint_indices_.back();
            free_joint_indices_.pop_back();
            joints_[index] = j;
        } else {
            index = static_cast<uint32_t>(joints_.size());
            joints_.push_back(j);
            joint_generations_.push_back(0);
            joint_alive_.push_back(false);
        }
        joint_alive_[index] = true;
        return dynamics::JointId{index, joint_generations_[index]};
    }

    /** @brief True if some valid Joint connects `id_i`/`id_j` (either order) with
     *         `collide_connected == false` -- narrowphase_()'s cue to skip contact generation for
     *         this pair entirely (see Joint::collide_connected's own doc for why). A linear
     *         scan over joints_: v1's joint counts (a handful of doors per scene, not thousands)
     *         make an index unnecessary, matching this feature's "relatively simple" scope. */
    bool joint_blocks_collision_(dynamics::BodyId id_i, dynamics::BodyId id_j) const {
        for (const auto& j : joints_) {
            if (!j.valid || j.collide_connected) continue;
            bool same_pair = (j.a.index == id_i.index && j.b.index == id_j.index) ||
                              (j.a.index == id_j.index && j.b.index == id_i.index);
            if (same_pair) return true;
        }
        return false;
    }

    /**
     * @brief Advances every alive cloth by one FRAME step, called once per step() after the rigid
     *        substep loop.
     *
     * Per cloth: resolve anchors from their bodies, gather the rigid shapes the sheet could
     * possibly touch, run the XPBD solve, then update sleep state.
     *
     * **Why the frame clock and not the fixed grid.** A cloth is drawn straight from these
     * particle positions (toyengine's ClothRenderer), with none of the render interpolation
     * PhysicsSystem::write_transforms_back_() applies to a dynamic rigid body. A kinematic body --
     * the thing sheets are actually pinned to and draped over: a character, a flagpole, the
     * cloth_test ball -- is drawn wherever its script put it on the wall clock this frame.
     * Solving the sheet on the 1/60 s grid would make those two disagree: whenever the
     * accumulator crossed a substep boundary, a frame would run ZERO substeps and the whole sheet
     * would stand still in world space while the body travelled a full frame, then the next
     * frame would run two and the sheet would catch up in one jump. That beat is visible as
     * jitter on any moving body a sheet touches, and no amount of collision tuning can remove
     * it -- the two objects would be on different clocks. Stepping here, with the frame's own
     * dt, puts them on one: the sheet is
     * always solved against the poses this frame will draw. One-way coupling (see cloth.h) is
     * what makes this legal -- no rigid body ever reads a cloth, so nothing inside the substep
     * loop needs the cloth result.
     *
     * The substep COUNT is sized to hold the internal XPBD substep length at
     * `fixed_dt / cloth_substeps` (1/240 s by default) rather than dividing a variable dt by a
     * constant count. At 60 Hz that is 4 substeps; at 144 Hz
     * it is 2 and at 30 Hz it is 8, so both the sheet's behaviour and its cost PER SECOND stay
     * frame-rate independent. XPBD compliance is already timestep-independent (see ClothParams),
     * so the material does not change with the count.
     *
     * Collider gathering goes through the SAME two broadphase trees rigid pairs use, so a cloth
     * automatically respects the layer matrix and never pays for shapes on the far side of the
     * level. Two details make that safe here:
     *
     *   - The trees hold each proxy's FAT bounds from this frame's last sync_broadphase_() (the
     *     last substep's, or step()'s own explicit sync when no substep ran), while the bodies
     *     have since been integrated. The query box therefore absorbs that staleness explicitly
     *     (thickness + the sheet's own travel this frame + aabb_margin) instead of assuming the
     *     tree is current. Candidate selection is the only thing affected -- the actual collision
     *     math uses each body's live pose, not its proxy.
     *   - AABBTree::query() keeps its traversal stack in thread_local storage and mutates nothing,
     *     so it is safe to call here even though narrowphase_() has finished with the trees.
     *
     * The solve itself runs serially, via cloth::SerialDispatch. The constraint batches are
     * provably particle-disjoint (see cloth::ConstraintBatch) so a job-parallel dispatcher would
     * be both safe and bit-identical -- but at realistic sheet sizes it is a pessimization: a
     * 25x25 sheet costs ~0.12 ms per substep serially, while the parallel path would pay
     * JobEngine dispatch overhead 30+ times per substep (8 stretch batches + 8 bend batches, per
     * iteration, per cloth substep). solve_cloth() is templated on the dispatcher precisely so
     * that this stays a one-line change if sheets ever get large enough to justify it.
     *
     * @param dt Frame length in seconds, already clamped to util::k_max_frame_time by step().
     */
    void step_cloths_(float dt) {
        // Substeps per frame, sized to hold the internal substep length at fixed_dt/cloth_substeps
        // (see this function's doc). A per-cloth ClothParams::substeps override still wins inside
        // solve_cloth(), untouched by this.
        const float internal_h = std::max(
            config_.fixed_dt / static_cast<float>(std::max(1u, config_.cloth_substeps)),
            util::k_epsilon); // a zero fixed_dt would divide by zero below, not merely misbehave
        const uint32_t frame_substeps = std::clamp<uint32_t>(
            static_cast<uint32_t>(std::lround(dt / internal_h)), 1u, util::k_max_cloth_substeps);

        for (uint32_t ci = 0; ci < cloths_.size(); ++ci) {
            if (!cloth_alive_[ci]) continue;
            cloth::Cloth& c = cloths_[ci];
            if (!c.enabled || c.particles.empty()) continue;

            // --- Anchors: resolve this frame's world target from each body's live pose ---
            bool anchor_moved = false;
            for (cloth::ClothAnchor& a : c.anchors) {
                const dynamics::Body* body = get_body(a.body);
                const glm::vec3 target = body ? (body->position + body->orientation * a.local_position)
                                              : a.world_position;
                const glm::vec3 delta = target - a.world_position;
                if (glm::dot(delta, delta) > util::k_epsilon) anchor_moved = true;
                a.world_position = target;
            }

            // --- Candidate colliders ---
            cloth_colliders_.clear();
            const float travel = cloth::max_particle_speed(c) * dt;
            const geometry::AABB query =
                c.bounds.expand(c.params.thickness + travel + config_.aabb_margin);

            // No collider sweep is fed here (ClothCollider::sweep stays zero). The cloth is solved
            // once per frame against the poses that frame draws, so there are no in-between drawn
            // poses for a sweep to cover -- see this function's doc.
            bool collider_moving = false;
            auto gather = [&](void* user_data) {
                const uint32_t slot = user_data_to_index_(user_data);
                if (slot >= shapes_.size() || !shape_alive_[slot]) return;
                const collision::Shape& shape = shapes_[slot];
                if (!shape.enabled || shape.is_trigger) return;
                if (!layer_matrix_.should_collide(c.params.layer, shape.layer)) return;

                const dynamics::BodyId owner = shape_owner_[slot];
                if (!is_valid(owner)) return;
                const dynamics::Body& body = bodies_[owner.index];

                cloth::ClothCollider collider;
                collider.shape = &shape;
                collider.position = body.position;
                collider.orientation = body.orientation;
                collider.bounds = collision::world_bounds(shape, body.position, body.orientation);

                collider.end_orientation = body.orientation;

                if (!collider.bounds.overlaps(query)) return;
                cloth_colliders_.push_back(collider);

                // A kinematic platform sliding under a settled sheet must wake it. Same rule
                // dynamics::wake_if_kinematic_moving() applies to rigid bodies, and for the same
                // reason -- a spinner has near-zero linear velocity but is very much moving.
                if (body.type != dynamics::BodyType::Static &&
                    (glm::dot(body.linear_velocity, body.linear_velocity) > util::k_epsilon ||
                     glm::dot(body.angular_velocity, body.angular_velocity) > util::k_epsilon)) {
                    collider_moving = true;
                }
            };
            static_tree_.query(query, gather);
            dynamic_tree_.query(query, gather);

            // --- Sleep gate ---
            if (anchor_moved || collider_moving) {
                c.awake = true;
                c.sleep_timer = 0.0f;
            }
            if (!c.awake) {
                // Keep the anchor lerp's start point pinned to where the sheet actually is, or the
                // first substep after waking would interpolate from a position several seconds
                // stale and snap the sheet across the gap.
                for (cloth::ClothAnchor& a : c.anchors) a.prev_world_position = a.world_position;
                continue;
            }

            cloth::solve_cloth(c, cloth_colliders_,
                               gravity_sources_.empty() ? gravity_ : gravity_at(cloth_centre_(c)), frame_substeps,
                               config_.cloth_iterations, dt, cloth_scratch_);

            // --- Sleep bookkeeping ---
            if (cloth::max_particle_speed(c) < c.params.sleep_threshold) {
                c.sleep_timer += dt;
                if (c.sleep_timer >= c.params.sleep_time) {
                    c.awake = false;
                    for (cloth::ClothParticle& p : c.particles) p.velocity = glm::vec3(0.0f);
                }
            } else {
                c.sleep_timer = 0.0f;
            }
        }
    }

    /**
     * @brief Advances every hair groom once, by this frame's dt.
     *
     * The structure mirrors step_cloths_() exactly -- resolve the driven roots, gather candidate
     * colliders from both broadphase trees, gate on sleep, solve -- because hair makes the same
     * three choices cloth does: one-way coupling, the frame clock rather than the substep grid,
     * and no collider sweep (there is no un-solved drawn frame for a sweep to cover).
     *
     * Roots are driven from OUTSIDE -- hair::set_strand_roots(), called by the engine from the
     * scalp's transform -- rather than from a body, because a groom follows a mesh, not a
     * rigidbody. That is the one structural difference from a cloth anchor.
     */
    void step_strands_(float dt) {
        const float internal_h = std::max(
            config_.fixed_dt / static_cast<float>(std::max(1u, config_.cloth_substeps)),
            util::k_epsilon);
        const uint32_t frame_substeps = std::clamp<uint32_t>(
            static_cast<uint32_t>(std::lround(dt / internal_h)), 1u, util::k_max_cloth_substeps);

        for (uint32_t si = 0; si < strand_sets_.size(); ++si) {
            if (!strand_alive_[si]) continue;
            hair::StrandSet& set = strand_sets_[si];
            if (!set.enabled || set.empty()) continue;

            // A root that moved is motion the groom has to answer, so it also wakes it.
            bool root_moved = false;
            for (const hair::Strand& strand : set.strands) {
                const glm::vec3 delta = strand.root_position - strand.prev_root_position;
                if (glm::dot(delta, delta) > util::k_epsilon) { root_moved = true; break; }
            }

            cloth_colliders_.clear();
            const float travel = hair::max_particle_speed(set) * dt;
            const geometry::AABB query =
                set.bounds.expand(set.params.thickness + travel + config_.aabb_margin);

            bool collider_moving = false;
            auto add_shape = [&](uint32_t slot) {
                if (slot >= shapes_.size() || !shape_alive_[slot]) return;
                const collision::Shape& shape = shapes_[slot];
                if (!shape.enabled || shape.is_trigger) return;
                if (!layer_matrix_.should_collide(set.params.layer, shape.layer)) return;

                const dynamics::BodyId owner = shape_owner_[slot];
                if (!is_valid(owner)) return;
                const dynamics::Body& body = bodies_[owner.index];

                cloth::ClothCollider collider;
                collider.shape = &shape;
                collider.position = body.position;
                collider.orientation = body.orientation;
                collider.end_orientation = body.orientation;
                collider.bounds = collision::world_bounds(shape, body.position, body.orientation);
                if (!collider.bounds.overlaps(query)) return;
                cloth_colliders_.push_back(collider);

                // A spinning head has near-zero linear velocity and is very much moving, so the
                // wake test reads both -- the same rule cloth and rigid sleep both apply.
                if (body.type != dynamics::BodyType::Static &&
                    (glm::dot(body.linear_velocity, body.linear_velocity) > util::k_epsilon ||
                     glm::dot(body.angular_velocity, body.angular_velocity) > util::k_epsilon)) {
                    collider_moving = true;
                }
            };

            if (set.collide_bodies.empty()) {
                auto gather = [&](void* user_data) { add_shape(user_data_to_index_(user_data)); };
                static_tree_.query(query, gather);
                dynamic_tree_.query(query, gather);
            } else {
                // An explicit allowlist: skip the broadphase entirely and build colliders straight
                // from the named bodies, so cost is proportional to what the groom was told to
                // avoid rather than to whatever happens to be nearby.
                for (const dynamics::BodyId id : set.collide_bodies) {
                    if (!is_valid(id)) continue;
                    for (const uint32_t slot : shapes_of(id)) add_shape(slot);
                }
            }

            if (root_moved || collider_moving) {
                set.awake = true;
                set.sleep_timer = 0.0f;
            }
            if (!set.awake) {
                // Keep the root lerp's start where the groom actually is, or the first substep
                // after waking would interpolate from a seconds-stale position and snap.
                for (hair::Strand& strand : set.strands) {
                    strand.prev_root_position = strand.root_position;
                    strand.prev_root_orientation = strand.root_orientation;
                }
                continue;
            }

            const glm::vec3 centre = 0.5f * (set.bounds.min + set.bounds.max);
            hair::solve_strands(set, cloth_colliders_,
                                gravity_sources_.empty() ? gravity_ : gravity_at(centre),
                                frame_substeps, config_.cloth_iterations, dt, strand_scratch_);

            if (hair::max_particle_speed(set) < set.params.sleep_threshold) {
                set.sleep_timer += dt;
                if (set.sleep_timer >= set.params.sleep_time) {
                    set.awake = false;
                    for (cloth::ClothParticle& p : set.particles) p.velocity = glm::vec3(0.0f);
                }
            } else {
                set.sleep_timer = 0.0f;
            }
        }
    }

    /**
     * @brief Broadphase (two-tree query, job-parallel) + narrowphase (job-parallel) over this
     *        step's overlapping pairs. See discover_pairs_()/narrowphase_()'s docs for the
     *        parallelization scheme and the determinism argument for each.
     */
    void generate_manifolds_(float h) {
        sync_broadphase_(h);
        pair_cache_.advance(); // roll last step's pairs into "previous"; start this step fresh
        collision_pair_cache_.advance();
        trigger_pair_cache_.advance();

        discover_pairs_();
        pair_cache_.finalize();

        narrowphase_(h);

        collision_pair_cache_.finalize();
        trigger_pair_cache_.finalize();
        emit_contact_events_(collision_pair_cache_, /*is_trigger=*/false);
        emit_contact_events_(trigger_pair_cache_, /*is_trigger=*/true);
    }

    /**
     * @brief Turns one pair cache's added/stayed/removed classification into ContactEvents,
     *        appended to `events_` for the caller to drain after step()/step_fixed() returns.
     *
     * `pair.a`/`pair.b` are SHAPE slot indices (see shapes_'s file doc; narrowphase_() is what
     * feeds this cache), resolved to owning BodyIds via shape_owner_ -- correct because a
     * removed pair's shapes are, by definition, either still alive (an ordinary exit as they
     * separate) or were destroyed via remove_body()/remove_shape(), which only ever run outside
     * a substep or via the deferred-command queue that resolves before generate_manifolds_()
     * runs (see drain_deferred_commands_()'s doc) -- so any index still present in `removed` at
     * this point addresses whatever now legitimately occupies that slot; an exit event for a
     * shape that no longer exists is meaningless and safe to drop. Two different manifolds
     * between the same two compound bodies (different child shape pairs) fire independent
     * enter/stay/exit events -- each carries the SAME BodyId pair as every other event for
     * those two bodies, since ContactEvent (like ContactManifold) is body-identity-only, not
     * shape-identity (see narrowphase_()'s doc for that same scope trim).
     */
    void emit_contact_events_(const broadphase::PairCache& cache, bool is_trigger) {
        std::vector<broadphase::ProxyPair> added, stayed, removed;
        cache.classify(added, stayed, removed);

        auto push = [&](const broadphase::ProxyPair& pair, collision::ContactEvent::Type type) {
            if (pair.a >= shapes_.size() || pair.b >= shapes_.size()) return;
            if ((type != collision::ContactEvent::Type::Exit) && (!shape_alive_[pair.a] || !shape_alive_[pair.b])) return;
            dynamics::BodyId owner_a = shape_owner_[pair.a];
            dynamics::BodyId owner_b = shape_owner_[pair.b];
            if (!is_valid(owner_a) || !is_valid(owner_b)) return;
            collision::ContactEvent ev;
            ev.a = owner_a;
            ev.b = owner_b;
            ev.type = type;
            ev.is_trigger = is_trigger;
            events_.push_back(ev);
        };
        for (const auto& pair : added) push(pair, collision::ContactEvent::Type::Enter);
        for (const auto& pair : stayed) push(pair, collision::ContactEvent::Type::Stay);
        for (const auto& pair : removed) push(pair, collision::ContactEvent::Type::Exit);
    }

    /** @brief Applies deferred structural commands queued by the previous step's on_substep,
     *         in the order they were pushed (deterministic, single-threaded). */
    void drain_deferred_commands_() {
        for (auto& cmd : deferred_commands_) {
            if (cmd.type == DeferredCommand::Type::Create) {
                add_body(cmd.body, cmd.shape);
            } else {
                remove_body(cmd.target);
            }
        }
        deferred_commands_.clear();
    }

    struct DeferredCommand {
        enum class Type { Create, Destroy } type = Type::Create;
        dynamics::Body body;
        collision::Shape shape;
        dynamics::BodyId target;
    };

    util::PhysicsConfig config_;
    coopa::debug::Logger logger_;

    glm::vec3 gravity_{0.0f, 0.0f, util::k_gravity_z};
    std::vector<std::pair<uint32_t, GravitySource>> gravity_sources_;   ///< See add_gravity_source().

    /// gravity_at() over the sources: the strongest acting at `p` (or the uniform gravity), and in
    /// `up` the unit direction away from that source's centre (zero when none acts).
    glm::vec3 field_at_(const glm::vec3& p, glm::vec3& up) const {
        float best = 0.0f;
        glm::vec3 pull = gravity_;
        up = glm::vec3(0.0f);
        bool found = false;
        for (const auto& entry : gravity_sources_) {
            const GravitySource& g = entry.second;
            const glm::vec3 d = g.centre - p;
            const float r = glm::length(d);
            if (g.range > 0.0f && r > g.range) continue;
            if (r < 1e-6f) continue;   // at the very centre there is no "down"
            const float a = r <= g.radius ? g.strength : g.strength * (g.radius / r) * (g.radius / r);
            if (!found || a > best) {
                best = a;
                pull = d / r * a;
                up = -d / r;
                found = true;
            }
        }
        return found ? pull : gravity_;
    }
    uint32_t next_gravity_id_ = 1;

    /** @brief A cloth's mean particle position, where a gravity source is read for it. */
    static glm::vec3 cloth_centre_(const cloth::Cloth& c) {
        glm::vec3 sum(0.0f);
        for (const cloth::ClothParticle& p : c.particles) sum += p.position;
        return c.particles.empty() ? sum : sum / static_cast<float>(c.particles.size());
    }

    std::vector<dynamics::Body> bodies_;
    std::vector<uint32_t> generations_;
    std::vector<bool> alive_;
    std::vector<uint32_t> free_indices_;
    /** @brief Parallel to bodies_ -- the shape SLOT indices (into shapes_ below) this body
     *         currently owns, in insertion order; slot [0] is the "primary" shape the
     *         single-shape API (get_shape(BodyId), set_shape()) reads/writes. Empty
     *         for a shapeless body (Shape::enabled's "Rigidbody with no Collider" case). See
     *         add_shape()'s doc for how a body acquires more than one entry here. */
    std::vector<std::vector<uint32_t>> body_shapes_;

    /**
     * @brief Shape storage, with its OWN independent lifecycle from bodies_ (not parallel to
     *        it) -- this is the compound-collider primitive: a body owns zero or more shape
     *        slots (body_shapes_[body.index]), not exactly one. Mirrors bodies_'s own
     *        alive_/free_indices_ pattern one level down, applied to shapes instead. Broadphase
     *        proxies (tree_proxy_/in_static_tree_ below) are parallel to THIS array, not to
     *        bodies_ -- the fundamental unit narrowphase/broadphase operate on is a shape (a
     *        1-shape body just makes the two concepts look identical). shape_owner_ is how a shape slot resolves back to the
     *        body it belongs to (position/orientation/dynamics live on the Body, never the
     *        Shape) -- see e.g. narrowphase_() or any query visit lambda.
     */
    std::vector<collision::Shape> shapes_;
    std::vector<dynamics::BodyId> shape_owner_; /**< Parallel to shapes_. */
    std::vector<bool> shape_alive_;             /**< Parallel to shapes_. */
    std::vector<uint32_t> free_shape_indices_;

    broadphase::AABBTree static_tree_{config_.aabb_margin};
    broadphase::AABBTree dynamic_tree_{config_.aabb_margin};
    std::vector<int32_t> tree_proxy_;   /**< Parallel to shapes_ (see its own doc above -- NOT
                                              bodies_, even for a single-shape body); k_null_node
                                              if no proxy. */
    std::vector<bool> in_static_tree_;  /**< Parallel to shapes_; which tree tree_proxy_ indexes into. */
    broadphase::PairCache pair_cache_;
    broadphase::PairCache collision_pair_cache_; /**< Confirmed (narrowphase-hit) non-trigger pairs, for enter/stay/exit. */
    broadphase::PairCache trigger_pair_cache_;   /**< Confirmed (narrowphase-hit) trigger pairs, for enter/stay/exit. */
    broadphase::LayerMatrix layer_matrix_;

    std::vector<collision::ContactManifold> manifolds_;
    std::vector<collision::ContactEvent> events_; /**< This frame's enter/stay/exit events; cleared at the top of step(). */
    dynamics::SolverState solver_state_;

    /** @brief Joint storage -- own independent generational-handle lifecycle (free list +
     *         generation bump), the exact same shape as bodies_/generations_/alive_/
     *         free_indices_ above, just for joints instead of bodies (see add_hinge_joint()'s
     *         doc). Joints have no 1:1 relationship with bodies_, so they get their own
     *         lifecycle, like shapes_. */
    std::vector<dynamics::Joint> joints_;
    /** @brief Body-index pairs set_pair_collision() turned off (pair_key_()). */
    std::unordered_set<uint64_t> ignored_pairs_;
    std::vector<uint32_t> joint_generations_;
    std::vector<bool> joint_alive_;
    std::vector<uint32_t> free_joint_indices_;

    /** @brief Cloth storage -- the same generational-handle lifecycle as joints_ above, for the
     *         same reason: a cloth is a persistent, independently-created object with no 1:1
     *         relationship to any body. Kept separate from bodies_ entirely; see cloth/cloth.h's
     *         file comment for why cloth particles are not bodies. */
    std::vector<cloth::Cloth> cloths_;
    std::vector<uint32_t> cloth_generations_;
    std::vector<bool> cloth_alive_;
    std::vector<uint32_t> free_cloth_indices_;

    /** @brief Reused per-cloth solve workspace; see step_cloths_(). Both are cleared and refilled
     *         per cloth rather than reallocated, matching bounds_scratch_/manifold_scratch_. */
    cloth::ClothSolverScratch cloth_scratch_;
    std::vector<cloth::ClothCollider> cloth_colliders_;

    /** @brief Hair grooms, stored exactly as cloths are and for the same reasons; see
     *         hair/strand.h. The collider snapshot type is shared with cloth deliberately --
     *         hair needs precisely the sphere/box/capsule/mesh projection cloth already has. */
    std::vector<hair::StrandSet> strand_sets_;
    std::vector<uint32_t> strand_generations_;
    std::vector<bool> strand_alive_;
    std::vector<uint32_t> free_strand_indices_;
    hair::StrandSolverScratch strand_scratch_;

    std::vector<DeferredCommand> deferred_commands_;
    bool in_step_ = false;

    float accumulator_ = 0.0f;
    float interpolation_alpha_ = 0.0f;

    coopa::job::JobEngine* jobs_ = nullptr;
    std::size_t parallel_threshold_ = 64;
    std::vector<geometry::AABB> bounds_scratch_;                        /**< Parallel to shapes_. */
    std::vector<std::vector<broadphase::ProxyPair>> pair_buckets_;      /**< One per worker slot; see discover_pairs_(). */
    std::vector<collision::ContactManifold> manifold_scratch_;         /**< Parallel to pair_cache_.current(). */
};

/**
 * @brief Applies a scene's `physics:` settings to a freshly-constructed PhysicsWorld -- gravity,
 *        solver tunables, fixed timestep and the layer collision matrix. Called once by
 *        install_physics_system() (system/physics_system.h); safe to call again later (e.g. a
 *        runtime "reload physics settings" action) since every field it touches is a plain
 *        assignment, not additive.
 */
inline void apply_physics_settings(PhysicsWorld& world, const util::PhysicsSettings& settings) {
    world.set_gravity(settings.gravity);
    world.config() = settings.solver;
    for (const auto& pair : settings.ignore_pairs) {
        world.layers().set_layer_collision(pair.first, pair.second, /*collide=*/false);
    }
}

} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_WORLD_H
