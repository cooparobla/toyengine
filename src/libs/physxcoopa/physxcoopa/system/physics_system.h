/**
 * @file physics_system.h
 * @brief PhysicsSystem -- the ISceneSystem adapter binding PhysicsWorld to a coopa::scene
 *        Scene. Together with components/ and physx_yaml.h, the only part of physxcoopa that
 *        depends on coopa::scene.
 */

#ifndef PHYSXCOOPA_SYSTEM_PHYSICS_SYSTEM_H
#define PHYSXCOOPA_SYSTEM_PHYSICS_SYSTEM_H

#include <physxcoopa/world.h>
#include <physxcoopa/util/config.h>
#include <physxcoopa/util/error.h>
#include <physxcoopa/util/transform_bridge.h>
#include <physxcoopa/util/physics_settings.h>
#include <physxcoopa/dynamics/inertia.h>
#include <physxcoopa/components/collider.h>
#include <physxcoopa/components/collision.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/components/cloth.h>
#include <physxcoopa/components/hinge_joint.h>
#include <physxcoopa/components/ball_joint.h>
#include <physxcoopa/components/cone_twist_joint.h>
#include <physxcoopa/query/queries.h>
#include <physxcoopa/geometry/ray.h>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_system.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/job/engine.h>
#include <coopa/job/parallel_for.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coopa {
namespace physx {
namespace system {

/**
 * @class PhysicsSystem
 * @brief Registered at coopa::scene::UpdatePhase::Physics (100) by default; owns a
 *        PhysicsWorld and reconciles Collider components to bodies every frame.
 *
 * Modelled on an ordered animation system for the lazy-gather shape, but NOT for its
 * full-rebuild-on-dirty behavior: a Collider owns a live broadphase proxy, warm-start
 * impulses and a sleep timer, so a full rebuild on every refresh() would cold-start every
 * stack in the scene. Instead, refresh() only triggers a diff against the last-known
 * collider set (create/destroy), and every frame -- dirty or not -- checks each already-
 * bound collider's revision() for in-place parameter changes (see update_changed_shapes_()).
 */
class PhysicsSystem : public coopa::scene::ISceneSystem {
public:
    explicit PhysicsSystem(const util::PhysicsConfig& config = {}) : world_(config) {}

    const char* system_name() const override { return "Physics"; }

    void on_attach(coopa::scene::Scene&) override { dirty_ = true; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override {
        const auto start = std::chrono::steady_clock::now();
        world_.set_job_engine(ctx.jobs); // cheap pointer assignment; keeps the world in sync
                                          // even if the scene's JobEngine changes at runtime.
        if (dirty_) {
            regather_(scene);
            regather_joints_(scene); // after regather_() -- every JointComponent needs
                                      // BOTH connected objects already bound to a body.
            regather_cloths_(scene);  // likewise: a ClothComponent's anchors name objects whose
                                      // Rigidbody must already be bound.
            dirty_ = false;
        }
        prime_transforms_(); // serial -- see its own doc; makes every read below race-free.
        update_changed_shapes_(); // serial -- mutates world_'s broadphase trees (see doc).
        update_changed_rigidbodies_(); // serial -- see its own doc.
        sync_transforms_in_(ctx.delta_time);
        world_.step(ctx.delta_time);
        write_transforms_back_();
        dispatch_events_();
        last_step_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    /** @brief Main-thread milliseconds the last execute() took, from regather to event
     *         dispatch (the step itself, transform sync both ways, events) -- for diagnostics
     *         such as the debug overlay. */
    double last_step_ms() const { return last_step_ms_; }

    /**
     * @brief Re-gathers the Collider list on the next execute() call. Call after
     *        adding/removing a Collider (or its owning SceneObject) at runtime -- exactly
     *        the same contract AnimationSystem::refresh() documents.
     *
     * Required, not advisory: every per-binding pass dereferences Binding::collider and its
     * owner (prime_transforms_() takes that object's Transform), so a binding left over from a
     * destroyed collider is a read of freed memory. Destroying a Collider -- or the
     * SceneObject carrying one -- without calling this is a use-after-free waiting for the
     * next step. (Collider::instance_id() keeps a RECYCLED collider address from being
     * mistaken for the one that was freed, but it cannot conjure a gather nobody asked for.)
     */
    void refresh() { dirty_ = true; }

    /**
     * @brief Unbinds every Collider in `object`'s subtree NOW: their bodies leave the world and
     *        their bindings are dropped, before the objects themselves are destroyed.
     *
     * refresh() is not sufficient for a runtime despawn, and the difference is pure ordering.
     * refresh() only marks the system dirty; the re-gather that drops a dead collider's binding
     * runs in this system's own slot (order 100). A game destroying an object from a component
     * update (order 200) or a deferred scene command (end of frame) therefore leaves the
     * binding, and the body, live across the START of the next frame -- and the character motor
     * queries the physics world at order 50, resolving whatever the broadphase hands back
     * through collider_for(). That read lands on a freed Collider.
     *
     * So anything that destroys a collider-bearing object at runtime should call this first and
     * destroy afterwards. refresh() is still the right call for a collider ADDED at runtime, or
     * for parameter changes.
     *
     * @param object Root of the subtree to unbind. Harmless if it carries no colliders.
     */
    void detach(const coopa::scene::SceneObject& object) {
        std::size_t removed = 0;
        for (std::size_t i = bindings_.size(); i-- > 0;) {
            const Binding& b = bindings_[i];
            if (b.collider == nullptr || b.collider->owner == nullptr) continue;
            if (!is_in_subtree_(b.collider->owner, &object)) continue;
            auto index_it = collider_by_index_.find(b.body.index);
            // Only if it is still OUR entry: a compound body produces one binding per collider
            // and the index map holds the primary, so the erase must not take an entry a body
            // created since then has claimed.
            if (index_it != collider_by_index_.end() && index_it->second != nullptr &&
                is_in_subtree_(index_it->second->owner, &object)) {
                collider_by_index_.erase(index_it);
            }
            collider_by_shape_index_.erase(b.shape_index);
            world_.remove_body(b.body);
            b.collider->set_body_id(dynamics::BodyId{});
            bindings_.erase(bindings_.begin() + static_cast<std::ptrdiff_t>(i));
            ++removed;
        }
        if (removed == 0) return;
        // Every index-addressed link into bindings_ (Binding::body_parent, the write-back
        // scratch) is stale now; the re-gather rebuilds them, and nothing reads them before it.
        dirty_ = true;
    }

    PhysicsWorld& world() { return world_; }
    const PhysicsWorld& world() const { return world_; }

    /** @brief Number of colliders currently bound to a body -- for tests/diagnostics. */
    size_t bound_count() const { return bindings_.size(); }

    /** @brief Minimum bound-collider count before this system's own job-parallel passes
     *         (sync_transforms_in_/write_transforms_back_) dispatch instead of running inline;
     *         also forwarded to world().set_parallel_threshold() for its own passes. */
    void set_parallel_threshold(std::size_t n) {
        parallel_threshold_ = n;
        world_.set_parallel_threshold(n);
    }

private:
    /**
     * @brief One Collider bound to one physics body -- bindings_' index is stable for one
     *        frame at a time (rebuilt by regather_()), which is what lets the job-parallel
     *        passes below address it purely by index instead of through a pointer-keyed map.
     *
     * A COMPOUND body (more than one Collider grouped under one Rigidbody -- see
     * group_root_for_()/create_compound_body_()'s docs) produces one Binding PER COLLIDER, all
     * sharing the same `body`, but only ONE of them has `is_primary == true`: the one whose
     * owning object's Transform actually drives the body (`sync_owner`, the group's ROOT --
     * see create_compound_body_()'s doc for why this isn't always simply `collider->owner`).
     * Every other member's Binding exists only so update_changed_shapes_()/regather_() can
     * still track and diff it by its own Collider pointer/revision; sync_transforms_in_()/
     * write_transforms_back_() skip non-primary bindings entirely (no independent Transform
     * sync for a compound child -- its shape's fixed offset from the body was baked in once,
     * at creation, into the Shape itself, see create_compound_body_()'s doc for the current
     * scope trim on live-editing a compound child's own parameters).
     */
    struct Binding {
        components::Collider* collider = nullptr;
        /** @brief `collider`'s Collider::instance_id() when this binding was built.
         *
         * What tells "the same collider" from "another collider that happens to sit where the
         * last one was freed" -- which a streaming world produces constantly, and which, taken
         * for the same collider, had this reuse a body and a `sync_owner` belonging to an
         * object that no longer exists. */
        std::uint64_t collider_id = 0;
        dynamics::BodyId body;
        /** @brief This collider's own shape slot within `body` (PhysicsWorld::shape_at()'s
         *         return type) -- always valid; for a single (non-compound) binding this is
         *         `body`'s only shape (slot 0). */
        uint32_t shape_index = 0xFFFFFFFFu;
        uint32_t last_revision = 0;
        bool is_primary = false;
        /** @brief Meaningful only when `is_primary`: the object whose Transform drives this
         *         body -- see the class doc and create_compound_body_()'s doc for why this can
         *         differ from `collider->owner` (a compound group whose Rigidbody-bearing root
         *         has no Collider of its own, only children). For a non-compound binding this
         *         is always `collider->owner`. */
        coopa::scene::SceneObject* sync_owner = nullptr;
        /** @brief Meaningful only when `is_primary`: world-scaled, local-space (pre-rotation)
         *         offset from `sync_owner`'s own pivot to the body's true center of mass --
         *         see create_compound_body_()'s doc for how this is composed across every
         *         member (a single-collider body's is exactly `col->center() * world_scale`).
         *         create_compound_body_()
         *         folds this into body.position instead of leaving it on any one Shape (which
         *         couldn't represent it for N>1 children anyway), so sync_transforms_in_()/
         *         write_transforms_back_() need it every frame to convert between
         *         `sync_owner`'s pivot and body.position. NOT refreshed by
         *         update_changed_shapes_() for a compound body (see that function's doc) -- only by a full regather_()
         *         rebuild. */
        glm::vec3 center_offset{0.0f};
        /** @brief Meaningful only when `is_primary`: index (into bindings_) of the primary binding
         *         whose sync_owner is the NEAREST strict ancestor of this one's sync_owner, or -1
         *         -- the body hierarchy a ragdoll's bones form (see write_transforms_back_()).
         *         Rebuilt by link_body_hierarchy_() whenever bindings_ is. */
        int32_t body_parent = -1;
    };

    /** @brief write_transforms_back_()'s per-binding scratch (see its doc). */
    struct WriteBack {
        bool active = false;
        glm::vec3 pivot{0.0f};
        glm::vec3 center{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::mat4 old_world{1.0f};        ///< sync_owner's world matrix before this write-back.
        glm::mat4 new_world{1.0f};        ///< ...and after it (old scale kept).
        glm::mat4 parent_old_world{1.0f}; ///< sync_owner's parent's world matrix before it.
    };

    /** @brief Runs `body(begin, end, JobContext)` over `[0, bindings_.size())`, dispatched onto
     *         ctx.jobs when installed and bindings_.size() clears parallel_threshold_,
     *         otherwise inline on the calling thread -- same idiom as PhysicsWorld::for_range_. */
    template <typename Fn>
    void for_range_(Fn&& body) {
        coopa::job::JobEngine* jobs = world_.job_engine();
        if (jobs && bindings_.size() >= parallel_threshold_) {
            // Normal help floor: this loop is frame-critical and often trivial (a few dozen
            // bindings), so a thread that descended to Low here would pick up a background
            // chunk-mesh or bake job and bill its whole cost to the physics step.
            jobs->parallel_for_blocking(bindings_.size(), 0, std::forward<Fn>(body), 0,
                                        coopa::job::Priority::Normal, coopa::job::Priority::Normal);
        } else {
            coopa::job::JobContext ctx{};
            body(std::size_t{0}, bindings_.size(), ctx);
        }
    }

    /**
     * @brief The nearest object that should own the physics Body for `col` -- walks UP from
     *        `col->owner` (inclusive) through parent() until it finds a RigidbodyComponent,
     *        returning that object (the group's ROOT for a compound body -- see
     *        create_compound_body_()'s doc). Falls back to `col->owner` itself if no ancestor
     *        (including `col->owner`) has one -- a standalone static collider.
     *
     * When a Rigidbody sits on the SAME object as its Collider this returns `col->owner` on the
     * FIRST iteration, with no ancestor walking. Grouping only gathers several objects' colliders
     * into one body once a scene authors a child SceneObject's Collider under a
     * Rigidbody-bearing ancestor; several Colliders on one object always share that object's
     * group.
     */
    static coopa::scene::SceneObject* group_root_for_(components::Collider* col) {
        coopa::scene::SceneObject* obj = col->owner;
        while (obj) {
            if (obj->get_component<components::RigidbodyComponent>()) return obj;
            coopa::scene::SceneObject* parent = obj->parent();
            if (!parent) break;
            obj = parent;
        }
        return col->owner;
    }

    /** @brief Whether `obj` and every ancestor of it is active. */
    static bool active_in_hierarchy_(const coopa::scene::SceneObject& obj) {
        for (const coopa::scene::SceneObject* o = &obj; o != nullptr; o = o->parent()) {
            if (!o->active()) return false;
        }
        return true;
    }

    void regather_(coopa::scene::Scene& scene) {
        std::vector<components::Collider*> current;
        for (components::Collider* col : scene.get_components<components::Collider>()) {
            if (col->owner == nullptr || !active_in_hierarchy_(*col->owner)) continue;
            current.push_back(col);
        }
        std::unordered_set<components::Collider*> current_set(current.begin(), current.end());

        std::unordered_map<components::Collider*, Binding> old_by_collider;
        old_by_collider.reserve(bindings_.size());
        for (const Binding& b : bindings_) old_by_collider.emplace(b.collider, b);

        // Group current colliders by owning root (see group_root_for_()'s doc). group_order
        // preserves first-seen order so rebuilds are deterministic frame to frame.
        std::unordered_map<coopa::scene::SceneObject*, std::vector<components::Collider*>> groups;
        std::vector<coopa::scene::SceneObject*> group_order;
        for (components::Collider* col : current) {
            coopa::scene::SceneObject* root = group_root_for_(col);
            auto it = groups.find(root);
            if (it == groups.end()) {
                group_order.push_back(root);
                groups.emplace(root, std::vector<components::Collider*>{col});
            } else {
                it->second.push_back(col);
            }
        }

        std::vector<Binding> new_bindings;
        new_bindings.reserve(current.size());

        for (coopa::scene::SceneObject* root : group_order) {
            std::vector<components::Collider*>& members = groups[root];

            // A group reuses its existing body only if EVERY member already existed last
            // frame and all agreed on the same body -- otherwise (a brand new group, a new
            // member added to an existing group, or a member that moved out of a DIFFERENT
            // group since last frame, i.e. a reparent) the whole group rebuilds from scratch.
            // Not maximally incremental, but correct and simple.
            std::unordered_set<uint32_t> old_body_indices;
            bool all_old = true;
            for (auto* col : members) {
                auto it = old_by_collider.find(col);
                // The id, not just the pointer: an entry left by a collider freed since last
                // frame can be found again under the address its replacement was given.
                if (it == old_by_collider.end() || it->second.collider_id != col->instance_id()) {
                    all_old = false;
                    continue;
                }
                old_body_indices.insert(it->second.body.index);
            }

            if (all_old && old_body_indices.size() == 1) {
                for (auto* col : members) new_bindings.push_back(old_by_collider.at(col));
                continue;
            }

            for (uint32_t old_index : old_body_indices) {
                for (auto* col : members) {
                    auto it = old_by_collider.find(col);
                    if (it != old_by_collider.end() && it->second.body.index == old_index) {
                        collider_by_index_.erase(it->second.body.index);
                        world_.remove_body(it->second.body);
                        break;
                    }
                }
            }
            create_compound_body_(root, members, new_bindings);
        }

        // Destroy bodies for colliders that no longer exist at all -- remove_body() no-ops on
        // an already-dead BodyId, so this is safe even where a rebuild above already handled it.
        for (const auto& kv : old_by_collider) {
            const auto live = current_set.find(kv.first);
            // Present by address but a different instance: the collider this body was built
            // for is gone, so the body goes with it (the replacement gets its own above).
            if (live != current_set.end() && (*live)->instance_id() == kv.second.collider_id) continue;
            collider_by_index_.erase(kv.second.body.index);
            collider_by_shape_index_.erase(kv.second.shape_index);
            world_.remove_body(kv.second.body);
        }

        bindings_ = std::move(new_bindings);
        link_body_hierarchy_();
    }

    /** @brief Whether `node` is `root` or sits anywhere under it. */
    static bool is_in_subtree_(const coopa::scene::SceneObject* node,
                               const coopa::scene::SceneObject* root) {
        for (const coopa::scene::SceneObject* o = node; o != nullptr; o = o->parent()) {
            if (o == root) return true;
        }
        return false;
    }

    /** @brief Fills every primary binding's Binding::body_parent from the scene hierarchy. */
    void link_body_hierarchy_() {
        std::unordered_map<const coopa::scene::SceneObject*, int32_t> primary_by_owner;
        primary_by_owner.reserve(bindings_.size());
        for (std::size_t k = 0; k < bindings_.size(); ++k) {
            if (bindings_[k].is_primary && bindings_[k].sync_owner) {
                primary_by_owner.emplace(bindings_[k].sync_owner, static_cast<int32_t>(k));
            }
        }
        for (Binding& b : bindings_) {
            b.body_parent = -1;
            if (!b.is_primary || !b.sync_owner) continue;
            for (const coopa::scene::SceneObject* p = b.sync_owner->parent(); p; p = p->parent()) {
                auto it = primary_by_owner.find(p);
                if (it != primary_by_owner.end()) { b.body_parent = it->second; break; }
            }
        }
    }

    /**
     * @brief Diffs the scene's JointComponent (Hinge/Ball/ConeTwist) set against joint_bindings_ (identity-diffed
     *        by Component*, same spirit as regather_()'s Collider diffing) and creates/destroys
     *        dynamics::Joint entries to match. Runs AFTER regather_() so every
     *        JointComponent's own object and its `connected_object` are both guaranteed
     *        already bound to a body, if they're bindable at all this frame.
     *
     * A JointComponent whose own object or `connected_object` has no bound Collider yet
     * (or `connected_object` doesn't resolve to a live SceneObject at all) is silently skipped,
     * not an error -- create_joint_for_() returns an invalid JointId in that case, which
     * is stored as-is; it simply stays inert until the next refresh() finds both sides bound.
     * A joint whose CONNECTED body is destroyed later (without this component itself being
     * removed) is left similarly inert -- PhysicsWorld::remove_body() already marks the
     * dynamics::HingeJoint invalid (every solver pass skips it, see world.h's doc), but this
     * pass does not notice and attempt to rebind it; a known, documented limitation rather than
     * added bookkeeping for what should be a rare scene-editing edge case.
     */
    void regather_joints_(coopa::scene::Scene& scene) {
        std::vector<components::JointComponent*> current = scene.get_components<components::JointComponent>();
        std::unordered_set<components::JointComponent*> current_set(current.begin(), current.end());

        std::unordered_map<components::JointComponent*, dynamics::JointId> old_by_component;
        old_by_component.reserve(joint_bindings_.size());
        for (const auto& kv : joint_bindings_) old_by_component.emplace(kv.first, kv.second);

        for (const auto& kv : old_by_component) {
            if (current_set.find(kv.first) != current_set.end()) continue;
            world_.remove_joint(kv.second);
        }

        std::vector<std::pair<components::JointComponent*, dynamics::JointId>> new_bindings;
        new_bindings.reserve(current.size());
        for (components::JointComponent* jc : current) {
            auto it = old_by_component.find(jc);
            if (it != old_by_component.end()) {
                new_bindings.push_back(*it); // already bound -- creation-time-only, see the class doc
                continue;
            }
            dynamics::JointId id = create_joint_for_(jc, scene);
            jc->set_joint_id(id);
            new_bindings.emplace_back(jc, id);
        }
        joint_bindings_ = std::move(new_bindings);
    }

    /**
     * @brief Resolves `jc`'s `connected_object` and both sides' bound Collider/BodyId, computes
     *        the CONNECTED side's anchor/axis automatically from wherever it currently sits
     *        (Unity's `autoConfigureConnectedAnchor` default -- see HingeJointComponent's own
     *        doc for why only one side is authored), and creates the joint. Returns an invalid
     *        JointId (world_.add_hinge_joint() itself already no-ops safely on an invalid body)
     *        if anything isn't resolvable yet.
     */
    dynamics::JointId create_joint_for_(components::JointComponent* jc, coopa::scene::Scene& scene) {
        if (jc->connected_object.empty()) return dynamics::JointId{};
        coopa::scene::SceneObject* owner = jc->owner;
        coopa::scene::SceneObject* connected = scene.find_object(jc->connected_object);
        if (!owner || !connected) return dynamics::JointId{};

        auto* owner_col = owner->get_component<components::Collider>();
        auto* connected_col = connected->get_component<components::Collider>();
        if (!owner_col || !connected_col) return dynamics::JointId{};

        dynamics::BodyId owner_body = owner_col->body_id();
        dynamics::BodyId connected_body = connected_col->body_id();
        if (!world_.is_valid(owner_body) || !world_.is_valid(connected_body)) return dynamics::JointId{};

        const dynamics::Body* owner_phys = world_.get_body(owner_body);
        const dynamics::Body* connected_phys = world_.get_body(connected_body);
        if (!owner_phys || !connected_phys) return dynamics::JointId{};

        util::Trs owner_trs = util::world_trs(owner->get_transform()->transform());
        util::Trs connected_trs = util::world_trs(connected->get_transform()->transform());

        // World-scaled, matching every other local-offset convention in this codebase (e.g.
        // Collider::center()'s own `col->center() * world_scale` -- see center_offset_for_()'s
        // doc). Axis is a direction, rotated but not scaled. `jc->anchor` is authored relative
        // to the owner's TRANSFORM PIVOT (the natural authoring point -- see
        // HingeJointComponent's own doc), computed here via owner_trs, but HingeJoint's
        // local_anchor_a/b are consumed by the solver as offsets from each BODY'S TRUE CENTER
        // (dynamics::Body::position -- see create_compound_body_()'s doc), which only equals
        // the pivot when the collider's own `center` is zero. Whenever it isn't (e.g. this
        // codebase's "BoxCollider size {1,1,1} center {0.5,0.5,0.5}" convention -- see
        // spinner's own comment in the demo scene), true center is offset from the pivot by
        // that center-of-mass term, so both anchors must be re-expressed relative to each
        // body's OWN stored position/orientation, not re-derived from owner_trs/connected_trs.
        glm::vec3 owner_local_anchor_from_pivot = jc->anchor * owner_trs.scale;
        glm::vec3 world_anchor = owner_trs.position + owner_trs.rotation * owner_local_anchor_from_pivot;
        glm::vec3 owner_local_anchor = glm::inverse(owner_phys->orientation) * (world_anchor - owner_phys->position);
        glm::vec3 connected_local_anchor =
            glm::inverse(connected_phys->orientation) * (world_anchor - connected_phys->position);

        // hinge_current_angle() (and thus the limit constraint) measures body b's rotation
        // relative to body a, about a's local axis -- so the CONNECTED object (the static/
        // reference side, e.g. a door frame) must be body a, and this component's own object
        // (the side that actually swings, e.g. the door) must be body b. Otherwise the measured
        // angle runs backwards relative to what min_angle_deg/max_angle_deg authors expect, and
        // an asymmetric limit range (e.g. [0, 90]) clamps the hinge rigid at the very first
        // instant of motion instead of letting it swing open.
        const bool collide = jc->enable_collision;
        if (auto* hinge = dynamic_cast<components::HingeJointComponent*>(jc)) {
            glm::vec3 world_axis = glm::normalize(owner_trs.rotation * hinge->axis);
            glm::vec3 connected_local_axis = glm::inverse(connected_phys->orientation) * world_axis;
            glm::vec3 owner_local_axis = glm::inverse(owner_phys->orientation) * world_axis;
            return world_.add_hinge_joint(connected_body, owner_body, connected_local_anchor, owner_local_anchor,
                                           connected_local_axis, owner_local_axis, hinge->use_limits,
                                           glm::radians(hinge->min_angle_deg), glm::radians(hinge->max_angle_deg),
                                           collide);
        }
        if (auto* cone = dynamic_cast<components::ConeTwistJointComponent*>(jc)) {
            // The twist axis is authored on the owner (the limb); add_cone_twist_joint() takes it
            // in body a's (the connected side's) frame and derives b's from the current pose.
            glm::vec3 world_axis = glm::normalize(owner_trs.rotation * cone->axis);
            glm::vec3 connected_local_axis = glm::inverse(connected_phys->orientation) * world_axis;
            return world_.add_cone_twist_joint(connected_body, owner_body, connected_local_anchor, owner_local_anchor,
                                                connected_local_axis, glm::radians(cone->swing_limit_deg),
                                                glm::radians(cone->twist_min_deg), glm::radians(cone->twist_max_deg),
                                                collide);
        }
        return world_.add_ball_joint(connected_body, owner_body, connected_local_anchor, owner_local_anchor, collide);
    }

    /**
     * @brief Creates a cloth::Cloth for every ClothComponent that does not have one yet, and
     *        destroys the cloths of components that have gone away.
     *
     * Same identity-diff shape as regather_joints_(), and binding is likewise creation-time only
     * (see ClothComponent's doc): rebuilding a sheet would throw away its simulated state, which
     * is never what an author editing a wind vector wants, and the runtime-editable tunables are
     * reachable through ClothComponent::cloth_mut() without a rebuild anyway.
     *
     * A ClothComponent whose anchor names an object with no bound Collider is not an error -- that
     * anchor is simply skipped, and the rest of the sheet still simulates. An anchor with an empty
     * `object` pins its particles in place with no body to follow.
     */
    void regather_cloths_(coopa::scene::Scene& scene) {
        std::vector<components::ClothComponent*> current = scene.get_components<components::ClothComponent>();
        std::unordered_set<components::ClothComponent*> current_set(current.begin(), current.end());

        std::unordered_map<components::ClothComponent*, cloth::ClothId> old_by_component;
        old_by_component.reserve(cloth_bindings_.size());
        for (const auto& kv : cloth_bindings_) old_by_component.emplace(kv.first, kv.second);

        for (const auto& kv : old_by_component) {
            if (current_set.find(kv.first) != current_set.end()) continue;
            world_.remove_cloth(kv.second);
        }

        std::vector<std::pair<components::ClothComponent*, cloth::ClothId>> new_bindings;
        new_bindings.reserve(current.size());
        for (components::ClothComponent* cc : current) {
            auto it = old_by_component.find(cc);
            if (it != old_by_component.end()) {
                new_bindings.push_back(*it); // already bound -- creation-time-only, see the doc
                continue;
            }
            cloth::ClothId id = create_cloth_for_(cc, scene);
            cc->set_cloth_binding(&world_, id);
            new_bindings.emplace_back(cc, id);
        }
        cloth_bindings_ = std::move(new_bindings);
    }

    /**
     * @brief Builds one ClothComponent's sheet at its owner's current world pose, resolves and
     *        applies its anchors, derives its tethers, and hands it to the world.
     *
     * The sheet's extent is scaled by the owner's world scale, matching how Collider::center() and
     * every other authored local length in this codebase is treated -- a scene author scaling the
     * cloth object expects a bigger sheet, not the same sheet with a stretched-looking material.
     *
     * @return An invalid ClothId if the owner has no Transform.
     */
    cloth::ClothId create_cloth_for_(components::ClothComponent* cc, coopa::scene::Scene& scene) {
        coopa::scene::SceneObject* owner = cc->owner;
        if (!owner || !owner->get_transform()) return cloth::ClothId{};
        const util::Trs trs = util::world_trs(owner->get_transform()->transform());

        cloth::GridClothDesc desc;
        desc.columns = cc->columns;
        desc.rows = cc->rows;
        desc.width = cc->width * trs.scale.x;
        desc.height = cc->height * trs.scale.y;
        desc.center = trs.position;
        desc.orientation = trs.rotation;
        desc.total_mass = cc->mass;
        desc.shear = cc->shear;
        desc.params = cc->params;

        cloth::Cloth sheet = cloth::make_grid_cloth(desc);

        for (const components::ClothAnchorSpec& spec : cc->anchors) {
            if (spec.object.empty()) {
                // World-space pin with nothing to follow.
                cloth::pin_static(sheet, spec.point, spec.radius);
                continue;
            }
            coopa::scene::SceneObject* target = scene.find_object(spec.object);
            if (!target) continue;
            auto* col = target->get_component<components::Collider>();
            if (!col) continue;
            const dynamics::BodyId body_id = col->body_id();
            const dynamics::Body* body = world_.get_body(body_id);
            if (!body) continue;
            // `point` is authored in the TARGET's local frame (see ClothAnchorSpec), which is the
            // only frame in which "the top of the ball" stays the top of the ball as the ball
            // moves. Resolved against the BODY's pose rather than the target's Transform, since
            // those differ by the collider's own centre offset on a compound body.
            const glm::vec3 world_point = body->position + body->orientation * spec.point;
            cloth::pin_to_body(sheet, body_id, *body, world_point, spec.radius);
        }

        cloth::build_tethers(sheet);
        return world_.add_cloth(std::move(sheet));
    }


    /**
     * @brief Touches get_world_matrix() on every bound owner's Transform, serially, once --
     *        the prerequisite for every parallel pass below being race-free.
     *
     * coopa::util::Transform::get_world_matrix() lazily mutates a cache (and, walking up
     * parent_ to satisfy an ancestor's own dirty flag, potentially a whole shared ancestor
     * chain) from what is nominally a const read -- safe for any number of CONCURRENT readers
     * only once every touched transform (and its ancestors) is already clean, which is exactly
     * what one serial pass over every bound transform guarantees: PhysicsSystem runs at
     * UpdatePhase::Physics (100), before TransformSystem's own resolve at TransformResolve
     * (350), so nothing has cleaned these transforms yet this frame.
     */
    void prime_transforms_() {
        for (const Binding& b : bindings_) {
            b.collider->owner->get_transform()->transform().get_world_matrix();
            // A compound group's root can have no Collider of its own (see Binding::sync_owner's
            // doc) -- in that case `sync_owner != collider->owner` for the primary binding, and
            // sync_transforms_in_()/write_transforms_back_() read/write sync_owner's Transform
            // directly from job-parallel code, which needs THIS object's world matrix cache
            // clean too, not just collider->owner's -- otherwise those parallel passes could
            // race the very lazy-mutation this serial pass exists to front-load.
            if (b.is_primary && b.sync_owner && b.sync_owner != b.collider->owner) {
                b.sync_owner->get_transform()->transform().get_world_matrix();
            }
        }
    }

    /** @brief Checked every frame regardless of dirty_ -- see the plan's rationale: runtime
     *         parameter changes ride this pass, no separate dirty flag needed for them.
     *         Deliberately serial: make_shape()/set_shape() mutates world_'s broadphase AABB
     *         trees (proxy destroy+recreate), which is shared, non-thread-safe state -- unlike
     *         sync_transforms_in_()/write_transforms_back_() below, each binding here does NOT
     *         touch disjoint memory only.
     *
     *         SCOPE TRIM: a COMPOUND body (more than one shape -- see create_compound_body_()'s
     *         doc) is skipped here entirely, for every one of its bindings, even if only one
     *         child's revision actually changed -- correctly re-deriving one child's offset
     *         would require re-running mass/center-of-mass/inertia composition across ALL its
     *         siblings (a single child's offset shift moves the shared composite center of
     *         mass), which this pass isn't structured for. A live parameter edit on a compound
     *         child's Collider is simply not applied until the whole group next rebuilds (a
     *         structural change: refresh() + the collider set actually differing) -- a known
     *         limitation, not a silent-corruption risk (the current shape stays in effect,
     *         nothing goes stale/wrong, it just doesn't hot-reload). Single-shape bodies are
     *         unaffected by this trim. */
    void update_changed_shapes_() {
        for (Binding& b : bindings_) {
            if (world_.shapes_of(b.body).size() > 1) continue; // compound: see doc above
            components::Collider* col = b.collider;
            if (b.last_revision == col->revision()) continue;
            glm::vec3 world_scale = current_world_scale_(col->owner);
            collision::Shape shape = col->make_shape(world_scale);
            shape.enabled = shape.enabled && col->is_enabled();
            shape.is_trigger = col->is_trigger();
            shape.layer = col->layer();
            shape.material = col->material();
            // Mirror create_compound_body_()'s treatment: body.position (not Shape::local_center)
            // carries the center-of-mass offset, so the shape must stay zeroed here too --
            // otherwise a runtime collider-parameter change (e.g. a script editing `center`)
            // would double-apply the offset (once in body.position, once in the shape).
            shape.local_center = glm::vec3(0.0f);
            world_.set_shape_at(b.shape_index, shape);
            b.last_revision = col->revision();
            b.center_offset = center_offset_for_(col, world_scale);
            if (auto* rb = col->owner->get_component<components::RigidbodyComponent>()) {
                rb->set_local_center_of_mass(b.center_offset);
            }
        }
    }

    /**
     * @brief Reconciles each bound RigidbodyComponent's live-editable fields into its Body every
     *        frame, so a runtime write like `rb->mass = 5.0f` or `rb->is_kinematic = true` takes
     *        effect instead of being silently ignored after the body's creation-time bind in
     *        create_compound_body_().
     *
     * No separate revision counter on RigidbodyComponent: unlike Collider (whose setters bump
     * revision_ to gate an expensive make_shape() + broadphase proxy rebuild), Rigidbody's fields
     * are plain public members with no rebuild to gate -- Body already mirrors every one of them
     * (mass, linear_drag, angular_drag, use_gravity, constraints, interpolation) from the
     * component at creation time, so comparing the component's CURRENT value against the body's
     * own stored copy IS the "did this change since last frame" signal, with nothing extra to
     * keep in sync. Unconditional for the cheap scalar copies; mass is compared first since
     * changing it also means recomputing inv_mass and (unless inertia_tensor_override is set)
     * inv_inertia_local, which isn't free enough to redo every frame regardless.
     *
     * `is_kinematic` toggling is the one field that changes BodyType, not just a scalar -- see
     * PhysicsWorld::set_body_type()'s own doc for why that's safe here (Dynamic<->Kinematic only,
     * both live in dynamic_tree_, no broadphase tree migration). Note set_body_type() always
     * recomputes inv_inertia_local analytically from the shape, so a Kinematic->Dynamic flip on a
     * body with inertia_tensor_override set is corrected back to the override by the mass check
     * just below it, on the SAME frame -- the mass check runs unconditionally after the type
     * flip, and set_body_type() already made body->mass == rb->mass, so it falls through to the
     * override-only branch instead of a redundant analytic recompute.
     *
     * center_of_mass_override/inertia_tensor_override changing the body's actual center of mass
     * at runtime is intentionally NOT handled here -- see their own doc on RigidbodyComponent,
     * creation-time only, like initial_velocity/initial_angular_velocity above them.
     *
     * Serial like update_changed_shapes_() -- set_body_type() is, like set_shape(), not
     * documented safe for concurrent callers.
     */
    void update_changed_rigidbodies_() {
        for (Binding& b : bindings_) {
            auto* rb = b.collider->owner->get_component<components::RigidbodyComponent>();
            if (!rb) continue;
            dynamics::Body* body = world_.get_body(b.body);
            if (!body || body->type == dynamics::BodyType::Static) continue;

            dynamics::BodyType desired_type =
                rb->is_kinematic ? dynamics::BodyType::Kinematic : dynamics::BodyType::Dynamic;
            if (body->type != desired_type) {
                world_.set_body_type(b.body, desired_type, rb->mass);
            }

            if (body->type == dynamics::BodyType::Dynamic) {
                if (body->mass != rb->mass) {
                    body->mass = rb->mass;
                    body->inv_mass = rb->mass > 0.0f ? 1.0f / rb->mass : 0.0f;
                    if (rb->inertia_tensor_override) {
                        body->inv_inertia_local = *rb->inertia_tensor_override;
                    } else if (const collision::Shape* shape = world_.get_shape(b.body)) {
                        body->inv_inertia_local = inertia_for_(*shape, body->mass);
                    }
                } else if (rb->inertia_tensor_override &&
                           body->inv_inertia_local != *rb->inertia_tensor_override) {
                    body->inv_inertia_local = *rb->inertia_tensor_override;
                }
            }

            body->linear_drag = rb->drag;
            body->angular_drag = rb->angular_drag;
            body->use_gravity = rb->use_gravity;
            body->constraints = rb->constraints;
            body->interpolation = rb->interpolation;
        }
    }

    /** @brief Thin alias for dynamics::inertia_for_shape(); see that function's doc for why the
     *         dispatch lives in dynamics/inertia.h rather than here. */
    static glm::vec3 inertia_for_(const collision::Shape& shape, float mass) {
        return dynamics::inertia_for_shape(shape, mass);
    }

    /** @brief Approximate solid volume for a shape -- used only to split one compound
     *         Rigidbody's authored `mass` across its children proportionally (a uniform-
     *         density approximation; there is no per-collider density/mass field anywhere in
     *         this codebase to do better). TriangleMesh returns 1.0 (a mesh child is always
     *         static or kinematic, where mass is irrelevant, or the dynamic-mesh case throws
     *         in create_compound_body_() before this value would matter). ConvexHull returns
     *         its real built volume (scaled by hull_scale^3) rather than guessing, since a
     *         dynamic hull body -- unlike a mesh -- is exactly the case this split exists for. */
    static float shape_volume_(const collision::Shape& s) {
        constexpr float k_pi = 3.14159265358979323846f;
        switch (s.type) {
            case collision::ShapeType::Sphere:
                return (4.0f / 3.0f) * k_pi * s.radius * s.radius * s.radius;
            case collision::ShapeType::Box:
                return 8.0f * s.half_extents.x * s.half_extents.y * s.half_extents.z;
            case collision::ShapeType::Capsule: {
                float r = s.capsule_radius, hh = s.capsule_half_height;
                return k_pi * r * r * (2.0f * hh) + (4.0f / 3.0f) * k_pi * r * r * r;
            }
            case collision::ShapeType::ConvexHull:
                return s.hull ? s.hull->volume * s.hull_scale * s.hull_scale * s.hull_scale : 1.0f;
            case collision::ShapeType::TriangleMesh:
            default:
                return 1.0f;
        }
    }

    void bind_rigidbody_(coopa::scene::SceneObject* obj, dynamics::BodyId id) {
        if (auto* rb = obj->get_component<components::RigidbodyComponent>()) {
            rb->set_body_binding(&world_, id);
        }
    }

    /**
     * @brief Builds and inserts ONE Body for `root`'s Rigidbody, from `members` (every Collider
     *        grouped under it by group_root_for_() -- one for a non-compound object, more than
     *        one for a genuine compound collider), and appends a Binding per member to
     *        `new_bindings`.
     *
     * The engine's dynamics math assumes body.position IS the true center of mass, so the
     * collider offsets are folded into it across N children: every child's shape offset/rotation is first expressed relative to `root`
     * (`rel_pos`/`rel_rot`, a rigid transform composition via world_trs() that works regardless
     * of how deeply a child is nested under root), then each child's OWN collider `center`
     * offset is folded in on top of that (center_offset_for_()'s formula, applied per child),
     * giving every child's shape a `local_center`/`local_rotation` already
     * expressed in root's frame. Mass is split across children by volume (shape_volume_()'s
     * doc), producing per-child inertia inputs to dynamics::compose_mass_properties() (see its
     * own doc for the diagonal-only approximation that composition makes). The resulting
     * composite center of mass is folded into body.position (plus any
     * RigidbodyComponent::center_of_mass_override, composed the same way) -- every child shape's
     * local_center is then re-based from ROOT's pivot to the TRUE CENTER, since N children can't
     * all fold to a zero offset the way a single shape can.
     *
     * For a single member with no ancestor Rigidbody offset, this reduces EXACTLY to the
     * single-collider formulas: rel_pos=0, rel_rot=identity, mass fraction=1.0, and compose_mass_properties() over one child has zero
     * parallel-axis contribution (its own center IS the composite center of mass) and zero
     * rotation to apply, so the composite tensor is precisely that child's own inertia_for_()
     * result with no approximation error.
     *
     * The PRIMARY binding (the one driving Transform sync, see Binding's own doc) is the member
     * directly on `root` if one exists, else the first-gathered member -- `sync_owner` is
     * always `root` itself either way, which is what actually matters (see Binding's doc for
     * why a compound group whose root has no Collider of its own still needs ITS Transform,
     * not any particular child's, to drive the body).
     */
    void create_compound_body_(coopa::scene::SceneObject* root, std::vector<components::Collider*> members,
                                std::vector<Binding>& new_bindings) {
        // Primary member (directly on root) sorted to the front, if one exists.
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (members[i]->owner == root) { std::swap(members[0], members[i]); break; }
        }

        auto* rb = root->get_component<components::RigidbodyComponent>();
        util::Trs root_trs = util::world_trs(root->get_transform()->transform());
        glm::vec3 root_world_scale = root_trs.scale;

        struct Child {
            components::Collider* col;
            collision::Shape shape;      // local_center/local_rotation already expressed relative to root
            glm::vec3 local_offset;      // == shape.local_center, kept separately for compose_mass_properties()
        };
        std::vector<Child> children;
        children.reserve(members.size());

        for (auto* col : members) {
            glm::vec3 world_scale = current_world_scale_(col->owner);
            collision::Shape shape = col->make_shape(world_scale);
            shape.enabled = shape.enabled && col->is_enabled();
            shape.is_trigger = col->is_trigger();
            shape.layer = col->layer();
            shape.material = col->material();

            glm::vec3 rel_pos(0.0f);
            glm::quat rel_rot(1.0f, 0.0f, 0.0f, 0.0f);
            if (col->owner != root) {
                util::Trs child_trs = util::world_trs(col->owner->get_transform()->transform());
                glm::quat inv_root_rot = glm::inverse(root_trs.rotation);
                rel_pos = inv_root_rot * (child_trs.position - root_trs.position);
                rel_rot = inv_root_rot * child_trs.rotation;
            }

            // shape.local_center/local_rotation are this collider's OWN offset/rotation, in
            // the CHILD's own local frame -- compose with rel_pos/rel_rot to express in ROOT's.
            glm::vec3 offset_in_root = rel_pos + rel_rot * shape.local_center;
            shape.local_center = offset_in_root;
            shape.local_rotation = rel_rot * shape.local_rotation;

            children.push_back(Child{col, shape, offset_in_root});
        }

        float total_mass = rb ? rb->mass : 0.0f;
        float total_volume = 0.0f;
        for (auto& c : children) total_volume += shape_volume_(c.shape);
        bool degenerate = total_volume <= util::k_epsilon;

        std::vector<dynamics::ChildMassInput> mass_inputs;
        mass_inputs.reserve(children.size());
        for (auto& c : children) {
            float fraction = degenerate ? (1.0f / static_cast<float>(children.size()))
                                         : (shape_volume_(c.shape) / total_volume);
            float child_mass = total_mass * fraction;
            glm::vec3 inv_i = child_mass > util::k_epsilon ? inertia_for_(c.shape, child_mass) : glm::vec3(0.0f);
            mass_inputs.push_back(dynamics::ChildMassInput{c.local_offset, c.shape.local_rotation, child_mass, inv_i});
        }
        dynamics::MassProperties mp = dynamics::compose_mass_properties(mass_inputs);

        glm::vec3 com = mp.center_of_mass;
        if (rb && rb->center_of_mass_override) com += *rb->center_of_mass_override * root_world_scale;
        glm::vec3 true_center = root_trs.position + root_trs.rotation * com;

        dynamics::Body body;
        body.position = true_center;
        body.orientation = root_trs.rotation;

        if (!rb) {
            body.type = dynamics::BodyType::Static;
        } else if (rb->is_kinematic) {
            body.type = dynamics::BodyType::Kinematic;
        } else {
            body.type = dynamics::BodyType::Dynamic;
            body.mass = total_mass;
            body.inv_mass = total_mass > 0.0f ? 1.0f / total_mass : 0.0f;
            body.linear_drag = rb->drag;
            body.angular_drag = rb->angular_drag;
            body.use_gravity = rb->use_gravity;
            body.constraints = rb->constraints;
            body.interpolation = rb->interpolation;
            body.linear_velocity = rb->initial_velocity;
            body.angular_velocity = rb->initial_angular_velocity;
        }

        for (auto& c : children) {
            if (c.shape.type == collision::ShapeType::TriangleMesh && body.type == dynamics::BodyType::Dynamic) {
                util::throw_error("MeshCollider on '" + c.col->owner->name() +
                                   "' is attached to a non-kinematic Rigidbody -- non-convex mesh "
                                   "colliders are static-only (matching Unity); mark the Rigidbody "
                                   "kinematic or remove the MeshCollider.");
            }
        }

        if (body.type == dynamics::BodyType::Dynamic) {
            body.inv_inertia_local = (rb && rb->inertia_tensor_override) ? *rb->inertia_tensor_override
                                                                          : mp.inv_inertia_local;
        }

        // Re-base every child's offset from root's pivot to the TRUE CENTER just computed. A
        // single collider's offset folds to zero; N children can't all fold to zero, so each
        // keeps its own (much smaller) offset.
        for (auto& c : children) c.shape.local_center -= com;

        dynamics::BodyId id = world_.add_body(body, children[0].shape);
        dynamics::Body* stored = world_.get_body(id);
        stored->last_written_position = true_center;
        stored->last_written_orientation = root_trs.rotation;
        bind_rigidbody_(root, id);
        if (rb) rb->set_local_center_of_mass(com);

        components::Collider* primary_col = children[0].col;
        coopa::scene::SceneObject* sync_owner = root;

        for (std::size_t i = 0; i < children.size(); ++i) {
            uint32_t slot = (i == 0) ? world_.shape_at(id, 0) : world_.add_shape(id, children[i].shape);
            children[i].col->set_body_id(id);
            collider_by_shape_index_[slot] = children[i].col;

            Binding b;
            b.collider = children[i].col;
            b.collider_id = children[i].col->instance_id();
            b.body = id;
            b.shape_index = slot;
            b.last_revision = children[i].col->revision();
            b.is_primary = (i == 0);
            b.sync_owner = sync_owner;
            b.center_offset = com;
            new_bindings.push_back(b);
        }
        collider_by_index_[id.index] = primary_col;
    }

    /**
     * @brief Reads world position/rotation for every bound body, deriving kinematic velocity
     *        or detecting a script-driven teleport on a dynamic body. Job-parallel: each
     *        primary binding's Body and sync_owner Transform (a pure, race-free
     *        read post prime_transforms_()) are touched by exactly one binding, so concurrent
     *        iterations never share mutable state.
     *
     *        Skips every non-primary binding (a compound body's child colliders -- see
     *        Binding's own doc): they have no independent Transform to sync, their shape's
     *        fixed offset from the body was baked in once at creation.
     */
    void sync_transforms_in_(float dt) {
        for_range_([&](std::size_t begin, std::size_t end, const coopa::job::JobContext&) {
            for (std::size_t k = begin; k < end; ++k) {
                Binding& b = bindings_[k];
                if (!b.is_primary) continue;
                dynamics::Body* body = world_.get_body(b.body);
                if (!body) continue;
                coopa::util::Transform& transform = b.sync_owner->get_transform()->transform();
                util::Trs trs = util::world_trs(transform);
                // body->position lives in true-center space (see create_compound_body_()'s doc);
                // trs.position is the Transform's authored pivot -- convert before every
                // comparison/assignment below via the same rotated offset make_shape() bakes
                // into the shape.
                glm::vec3 true_center = trs.position + trs.rotation * b.center_offset;

                if (body->type == dynamics::BodyType::Kinematic) {
                    if (dt > util::k_epsilon) {
                        const glm::vec3 linear = (true_center - body->last_written_position) / dt;
                        const glm::vec3 angular = derive_angular_velocity_(
                            body->last_written_orientation, trs.rotation, dt);
                        // A placement, not a motion: see PhysicsConfig::max_kinematic_speed.
                        // Both go to zero together, since a body that was PUT somewhere was put
                        // there at an orientation too.
                        const util::PhysicsConfig& cfg = world_.config();
                        const bool teleported =
                            (cfg.max_kinematic_speed > 0.0f &&
                             glm::length(linear) > cfg.max_kinematic_speed) ||
                            (cfg.max_kinematic_spin > 0.0f &&
                             glm::length(angular) > cfg.max_kinematic_spin);
                        body->linear_velocity = teleported ? glm::vec3(0.0f) : linear;
                        body->angular_velocity = teleported ? glm::vec3(0.0f) : angular;
                    }
                    body->position = true_center;
                    body->orientation = trs.rotation;
                    body->last_written_position = true_center;
                    body->last_written_orientation = trs.rotation;
                } else if (body->type == dynamics::BodyType::Dynamic) {
                    float pos_delta = glm::length(true_center - body->last_written_position);
                    glm::quat rot_diff = trs.rotation * glm::inverse(body->last_written_orientation);
                    float rot_delta = 1.0f - std::abs(std::clamp(rot_diff.w, -1.0f, 1.0f));
                    if (pos_delta > 1e-4f || rot_delta > 1e-5f) {
                        body->position = true_center;
                        body->orientation = trs.rotation;
                        body->wake();
                    }
                }
            }
        });
    }

    static glm::vec3 derive_angular_velocity_(const glm::quat& from, const glm::quat& to, float dt) {
        glm::quat delta = to * glm::inverse(from);
        float w = std::clamp(delta.w, -1.0f, 1.0f);
        float angle = 2.0f * std::acos(w);
        glm::vec3 axis(delta.x, delta.y, delta.z);
        float axis_len = glm::length(axis);
        if (axis_len < util::k_epsilon || angle < util::k_epsilon) return glm::vec3(0.0f);
        return (axis / axis_len) * (angle / dt);
    }

    /**
     * @brief Writes each dynamic body's interpolated (or raw, per Body::interpolation)
     *        transform back, and records exactly what was written for next frame's
     *        teleport-detection comparison. Skips every non-primary binding, same reasoning as
     *        sync_transforms_in_()'s own doc.
     *
     * A dynamic body can sit UNDER another dynamic body in the scene hierarchy (a ragdoll: every
     * bone has its own body, and bones are children of bones). The Transform stores a LOCAL pose,
     * so writing the child's world pose means knowing its parent's world matrix -- AFTER this very
     * pass has moved the parent. Reading that lazily through parent->get_world_matrix() would be
     * order-dependent (a child written before its parent lands relative to the stale parent, and
     * next frame's teleport check then snaps the body back) and, on the job-parallel path, a race
     * on the parent's cached matrix. So this runs in two passes:
     *
     *   1. Read-only: each dynamic primary binding's target world pose, its owner's current world
     *      matrix and its parent's (all clean -- see prime_transforms_()). Nothing is written.
     *   2. Write: each binding's NEW parent world matrix is derived from its nearest dynamic
     *      body ancestor's pass-1 result (`ancestor_new * inverse(ancestor_old) * parent_old` --
     *      exact, and it covers non-body objects between the two), never from a lazily evaluated
     *      matrix, so the local pose it writes is correct in any order. Each iteration writes only
     *      its own Transform (a write's dirty propagation to children is atomic flag stores), so
     *      both passes are race-free on the job-parallel path.
     */
    void write_transforms_back_() {
        float alpha = world_.interpolation_alpha();
        if (writeback_.size() != bindings_.size()) writeback_.resize(bindings_.size());

        for_range_([&](std::size_t begin, std::size_t end, const coopa::job::JobContext&) {
            for (std::size_t k = begin; k < end; ++k) {
                Binding& b = bindings_[k];
                WriteBack& w = writeback_[k];
                w.active = false;
                if (!b.is_primary) continue;
                dynamics::Body* body = world_.get_body(b.body);
                if (!body || body->type != dynamics::BodyType::Dynamic) continue;

                glm::vec3 center_pos;
                glm::quat rot;
                if (body->interpolation == dynamics::Interpolation::Interpolate) {
                    center_pos = glm::mix(body->prev_position, body->position, alpha);
                    rot = glm::slerp(body->prev_orientation, body->orientation, alpha);
                } else {
                    center_pos = body->position;
                    rot = body->orientation;
                }

                // center_pos is the true center of mass (see create_compound_body_()'s doc);
                // convert back to sync_owner's authored pivot before writing it out -- the
                // inverse of the offset applied in sync_transforms_in_(). last_written_position
                // stays in true-center space so next frame's teleport-detection comparison in
                // sync_transforms_in_() lines up with the true_center it computes there.
                w.active = true;
                w.center = center_pos;
                w.rotation = rot;
                w.pivot = center_pos - rot * b.center_offset;

                const coopa::util::Transform& transform = b.sync_owner->get_transform()->transform();
                w.old_world = transform.get_world_matrix();
                const glm::vec3 scale(glm::length(glm::vec3(w.old_world[0])), glm::length(glm::vec3(w.old_world[1])),
                                      glm::length(glm::vec3(w.old_world[2])));
                w.new_world = glm::translate(glm::mat4(1.0f), w.pivot) * glm::mat4_cast(rot) *
                              glm::scale(glm::mat4(1.0f), scale);
                const coopa::util::Transform* parent = transform.parent();
                w.parent_old_world = parent ? parent->get_world_matrix() : glm::mat4(1.0f);
            }
        });

        for_range_([&](std::size_t begin, std::size_t end, const coopa::job::JobContext&) {
            for (std::size_t k = begin; k < end; ++k) {
                const WriteBack& w = writeback_[k];
                if (!w.active) continue;
                Binding& b = bindings_[k];
                coopa::util::Transform& transform = b.sync_owner->get_transform()->transform();

                int32_t anc = b.body_parent;
                while (anc >= 0 && !writeback_[anc].active) anc = bindings_[anc].body_parent;

                if (!transform.parent()) {
                    transform.set_position(w.pivot);
                    transform.set_rotation_quat(w.rotation);
                } else {
                    glm::mat4 parent_world = w.parent_old_world;
                    if (anc >= 0) {
                        const WriteBack& a = writeback_[anc];
                        parent_world = a.new_world * glm::inverse(a.old_world) * w.parent_old_world;
                    }
                    const glm::mat4 local = glm::inverse(parent_world) *
                                            (glm::translate(glm::mat4(1.0f), w.pivot) * glm::mat4_cast(w.rotation));
                    glm::vec3 local_position, local_scale, skew;
                    glm::quat local_rotation;
                    glm::vec4 perspective;
                    glm::decompose(local, local_scale, local_rotation, local_position, skew, perspective);
                    transform.set_position(local_position);
                    transform.set_rotation_quat(local_rotation);
                }

                dynamics::Body* body = world_.get_body(b.body);
                body->last_written_position = w.center;
                body->last_written_orientation = w.rotation;
            }
        });
    }

    static glm::vec3 current_world_scale_(coopa::scene::SceneObject* owner) {
        return util::world_trs(owner->get_transform()->transform()).scale;
    }

    /**
     * @brief The world-scaled, local-space (pre-rotation) offset from a Collider's owning
     *        Transform's own pivot to the body's true center of mass -- exactly the quantity
     *        every make_shape() override bakes into Shape::local_center (`col->center() *
     *        world_scale`), PLUS a RigidbodyComponent::center_of_mass_override on top if one is
     *        set (Unity's Rigidbody.centerOfMass, world-scaled the same way `center` already is
     *        for consistency). body.position is folded to the true center instead of Shape
     *        carrying the offset (see create_compound_body_()'s doc), so update_changed_shapes_()
     *        recomputes this whenever a single-shape Binding's center_offset needs a refresh (a
     *        shape-changing revision bump).
     */
    static glm::vec3 center_offset_for_(components::Collider* col, const glm::vec3& world_scale) {
        glm::vec3 offset = col->center() * world_scale;
        if (auto* rb = col->owner->get_component<components::RigidbodyComponent>()) {
            if (rb->center_of_mass_override) offset += *rb->center_of_mass_override * world_scale;
        }
        return offset;
    }

    /** @brief Packs two body indices into an order-independent 64-bit key, for the manifold
     *         lookup below -- same canonical-pair convention as broadphase::PairCache. */
    static uint64_t pack_pair_(uint32_t a, uint32_t b) {
        uint32_t lo = std::min(a, b), hi = std::max(a, b);
        return (static_cast<uint64_t>(lo) << 32) | hi;
    }

    /**
     * @brief Fires each of this frame's ContactEvents on both Colliders involved -- e.g. an
     *        Enter between A and B fires A's on_collision_enter(A, B) AND B's own
     *        on_collision_enter(B, A), matching Unity's two-sided callback convention.
     *
     * A BodyId whose index no longer maps to a bound Collider (the body was destroyed and its
     * Collider unbound this same frame, e.g. via a substep callback's deferred destroy) is
     * silently skipped -- there is no Collider left to fire on, and PhysicsWorld's own
     * generation check already keeps a reused slot's events from being misattributed (see
     * PhysicsWorld::emit_contact_events_'s doc).
     *
     * Non-trigger events additionally carry contact geometry: this step's manifolds are
     * indexed by canonical body-index pair once, then looked up per event -- an Exit event's
     * pair is (by definition) no longer in this step's manifold set, so it naturally resolves
     * to no contact data, matching Unity.
     */
    void dispatch_events_() {
        std::unordered_map<uint64_t, const collision::ContactManifold*> manifold_by_pair;
        for (const auto& m : world_.manifolds()) {
            if (m.is_trigger) continue;
            manifold_by_pair[pack_pair_(m.a.index, m.b.index)] = &m;
        }

        for (const auto& ev : world_.events()) {
            auto it_a = collider_by_index_.find(ev.a.index);
            auto it_b = collider_by_index_.find(ev.b.index);
            if (it_a == collider_by_index_.end() || it_b == collider_by_index_.end()) continue;

            if (ev.is_trigger) {
                fire_trigger_(ev, it_a->second, it_b->second);
                fire_trigger_(ev, it_b->second, it_a->second);
                continue;
            }

            auto mit = manifold_by_pair.find(pack_pair_(ev.a.index, ev.b.index));
            const collision::ContactManifold* m = mit != manifold_by_pair.end() ? mit->second : nullptr;
            fire_collision_(ev, it_a->second, it_b->second, m, /*self_is_a=*/true);
            fire_collision_(ev, it_b->second, it_a->second, m, /*self_is_a=*/false);
        }
    }

    static void fire_trigger_(const collision::ContactEvent& ev, components::Collider* self, components::Collider* other) {
        coopa::event::Signal<components::Collider&, components::Collider&>* signal = nullptr;
        switch (ev.type) {
            case collision::ContactEvent::Type::Enter: signal = &self->on_trigger_enter; break;
            case collision::ContactEvent::Type::Stay: signal = &self->on_trigger_stay; break;
            case collision::ContactEvent::Type::Exit: signal = &self->on_trigger_exit; break;
        }
        signal->emit(*self, *other);
    }

    /**
     * @brief Builds this event's Collision payload (from `self`'s perspective, against `other`)
     *        and fires the matching signal. `m` is nullptr on an Exit (pair no longer touching,
     *        so no contact geometry this step) -- the payload's contacts/impulse/contact_count
     *        stay at their zero defaults in that case, matching Unity.
     */
    void fire_collision_(const collision::ContactEvent& ev, components::Collider* self, components::Collider* other,
                          const collision::ContactManifold* m, bool self_is_a) {
        components::Collision c;
        c.collider = other;
        c.object = other->owner;

        dynamics::Body* self_body = world_.get_body(self->body_id());
        dynamics::Body* other_body = world_.get_body(other->body_id());
        if (self_body && other_body) c.relative_velocity = self_body->linear_velocity - other_body->linear_velocity;

        if (m) {
            // m.normal points from body a toward body b; self_is_a means self==a, so the
            // self-relative direction (Collision::normal's own convention -- see its doc) is
            // m.normal as-is; otherwise it's the other way around.
            c.normal = self_is_a ? m->normal : -m->normal;
            c.contact_count = m->count;
            float normal_impulse_sum = 0.0f;
            for (uint8_t i = 0; i < m->count; ++i) {
                c.contacts[i] = m->points[i];
                normal_impulse_sum += m->points[i].normal_impulse;
            }
            c.impulse = c.normal * normal_impulse_sum;
        }

        coopa::event::Signal<components::Collider&, const components::Collision&>* signal = nullptr;
        switch (ev.type) {
            case collision::ContactEvent::Type::Enter: signal = &self->on_collision_enter; break;
            case collision::ContactEvent::Type::Stay: signal = &self->on_collision_stay; break;
            case collision::ContactEvent::Type::Exit: signal = &self->on_collision_exit; break;
        }
        signal->emit(*self, c);
    }

public:
    /**
     * @name Scene-level queries
     * @brief Thin wrappers over PhysicsWorld's raw-BodyId queries that resolve each hit back
     *        to its owning Collider/SceneObject/RigidbodyComponent -- the ergonomic surface
     *        gameplay code actually wants (Unity's Physics.Raycast(...) returns a Collider,
     *        never a bare body handle). Same "only valid between phases, not from inside
     *        on_substep" caveat as the underlying PhysicsWorld queries.
     */
    ///@{
    struct RaycastHit {
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f, 0.0f, 1.0f};
        float distance = 0.0f;
        components::Collider* collider = nullptr;
        coopa::scene::SceneObject* object = nullptr;
        components::RigidbodyComponent* rigidbody = nullptr;
        bool started_inside = false; ///< Casts only -- see query::RaycastHit::started_inside.
    };

    /**
     * @brief Scene-level query filter: query::QueryFilter's layer mask and trigger switch, plus
     *        "don't hit this" in scene terms. `ignore_collider` skips the BODY that collider is
     *        bound to -- for a child collider of a compound Rigidbody that is the whole
     *        rigidbody, every child included. `ignore_rigidbody` skips that rigidbody's body.
     *        `predicate`, if set, gets the candidate body's PRIMARY Collider (collider_for()) and
     *        returns false to skip it; bodies with no bound Collider pass untested.
     */
    struct QueryFilter {
        uint32_t layer_mask = ~0u;
        bool include_triggers = true;
        const components::Collider* ignore_collider = nullptr;
        const components::RigidbodyComponent* ignore_rigidbody = nullptr;
        std::function<bool(const components::Collider&)> predicate;
    };

    /** @brief One overlap from compute_penetration() -- see query::Penetration; `collider` is the
     *         exact Collider whose shape is penetrated. */
    struct Penetration {
        glm::vec3 normal{0.0f, 0.0f, 1.0f};
        float depth = 0.0f;
        glm::vec3 point{0.0f};
        components::Collider* collider = nullptr;
    };

    bool raycast(const geometry::Ray& ray, RaycastHit& hit, const QueryFilter& filter) const {
        query::RaycastHit raw;
        if (!world_.raycast(ray, raw, world_filter_(filter))) return false;
        fill_hit_(raw, hit);
        return true;
    }
    bool raycast(const geometry::Ray& ray, RaycastHit& hit, uint32_t layer_mask = ~0u,
                 bool include_triggers = true) const {
        return raycast(ray, hit, scene_filter_(layer_mask, include_triggers));
    }

    /** @brief True if anything along `ray` is hit -- see PhysicsWorld::raycast_any()'s doc:
     *         first tree-traversal hit, not necessarily the closest. No Collider resolution
     *         (unlike every other query here) since the caller only asked whether, not what. */
    bool raycast_any(const geometry::Ray& ray, const QueryFilter& filter) const {
        return world_.raycast_any(ray, world_filter_(filter));
    }
    bool raycast_any(const geometry::Ray& ray, uint32_t layer_mask = ~0u, bool include_triggers = true) const {
        return world_.raycast_any(ray, layer_mask, include_triggers);
    }

    std::vector<RaycastHit> raycast_all(const geometry::Ray& ray, const QueryFilter& filter) const {
        std::vector<RaycastHit> out;
        for (const auto& raw : world_.raycast_all(ray, world_filter_(filter))) {
            RaycastHit hit;
            fill_hit_(raw, hit);
            out.push_back(hit);
        }
        return out;
    }
    std::vector<RaycastHit> raycast_all(const geometry::Ray& ray, uint32_t layer_mask = ~0u,
                                         bool include_triggers = true) const {
        return raycast_all(ray, scene_filter_(layer_mask, include_triggers));
    }

    /** @brief See PhysicsWorld::shape_cast() -- exact sweep of a convex shape, first hit only. */
    bool shape_cast(const collision::Shape& shape, const glm::vec3& pos, const glm::quat& rot, const glm::vec3& dir,
                    float max_distance, RaycastHit& hit, const QueryFilter& filter) const {
        query::RaycastHit raw;
        if (!world_.shape_cast(shape, pos, rot, dir, max_distance, raw, world_filter_(filter))) return false;
        fill_hit_(raw, hit);
        return true;
    }
    // (No `= {}` defaults for the nested QueryFilter inside its own class -- separate overloads.)
    bool shape_cast(const collision::Shape& shape, const glm::vec3& pos, const glm::quat& rot, const glm::vec3& dir,
                    float max_distance, RaycastHit& hit) const {
        return shape_cast(shape, pos, rot, dir, max_distance, hit, QueryFilter{});
    }

    bool sphere_cast(const glm::vec3& origin, float radius, const glm::vec3& dir, float max_distance,
                      RaycastHit& hit, const QueryFilter& filter) const {
        query::RaycastHit raw;
        if (!world_.sphere_cast(origin, radius, dir, max_distance, raw, world_filter_(filter))) return false;
        fill_hit_(raw, hit);
        return true;
    }
    bool sphere_cast(const glm::vec3& origin, float radius, const glm::vec3& dir, float max_distance,
                      RaycastHit& hit, uint32_t layer_mask = ~0u, bool include_triggers = true) const {
        return sphere_cast(origin, radius, dir, max_distance, hit, scene_filter_(layer_mask, include_triggers));
    }

    bool box_cast(const glm::vec3& origin, const glm::vec3& half_extents, const glm::quat& orientation,
                  const glm::vec3& dir, float max_distance, RaycastHit& hit, const QueryFilter& filter) const {
        query::RaycastHit raw;
        if (!world_.box_cast(origin, half_extents, orientation, dir, max_distance, raw, world_filter_(filter)))
            return false;
        fill_hit_(raw, hit);
        return true;
    }
    bool box_cast(const glm::vec3& origin, const glm::vec3& half_extents, const glm::quat& orientation,
                  const glm::vec3& dir, float max_distance, RaycastHit& hit,
                  uint32_t layer_mask = ~0u, bool include_triggers = true) const {
        return box_cast(origin, half_extents, orientation, dir, max_distance, hit,
                        scene_filter_(layer_mask, include_triggers));
    }

    bool capsule_cast(const glm::vec3& origin, float radius, float half_height, int direction_axis,
                       const glm::quat& orientation, const glm::vec3& dir, float max_distance,
                       RaycastHit& hit, const QueryFilter& filter) const {
        query::RaycastHit raw;
        if (!world_.capsule_cast(origin, radius, half_height, direction_axis, orientation, dir, max_distance,
                                  raw, world_filter_(filter)))
            return false;
        fill_hit_(raw, hit);
        return true;
    }
    bool capsule_cast(const glm::vec3& origin, float radius, float half_height, int direction_axis,
                       const glm::quat& orientation, const glm::vec3& dir, float max_distance,
                       RaycastHit& hit, uint32_t layer_mask = ~0u, bool include_triggers = true) const {
        return capsule_cast(origin, radius, half_height, direction_axis, orientation, dir, max_distance, hit,
                            scene_filter_(layer_mask, include_triggers));
    }

    /** @brief See PhysicsWorld::compute_penetration() -- every overlap of a convex shape, as
     *         minimum-translation vectors, resolved to the exact Collider penetrated. */
    std::vector<Penetration> compute_penetration(const collision::Shape& shape, const glm::vec3& pos,
                                                 const glm::quat& rot, const QueryFilter& filter) const {
        std::vector<Penetration> out;
        for (const query::Penetration& raw : world_.compute_penetration(shape, pos, rot, world_filter_(filter))) {
            Penetration p;
            p.normal = raw.normal;
            p.depth = raw.depth;
            p.point = raw.point;
            p.collider = collider_for_shape_(raw.shape_index);
            if (!p.collider) p.collider = collider_for(raw.body);
            out.push_back(p);
        }
        return out;
    }
    std::vector<Penetration> compute_penetration(const collision::Shape& shape, const glm::vec3& pos,
                                                 const glm::quat& rot) const {
        return compute_penetration(shape, pos, rot, QueryFilter{});
    }

    std::vector<components::Collider*> overlap_sphere(const glm::vec3& center, float radius,
                                                        const QueryFilter& filter) const {
        return colliders_for_(world_.overlap_sphere(center, radius, world_filter_(filter)));
    }
    std::vector<components::Collider*> overlap_sphere(const glm::vec3& center, float radius,
                                                        uint32_t layer_mask = ~0u,
                                                        bool include_triggers = true) const {
        return colliders_for_(world_.overlap_sphere(center, radius, layer_mask, include_triggers));
    }

    std::vector<components::Collider*> overlap_box(const geometry::OBB& box, const QueryFilter& filter) const {
        return colliders_for_(world_.overlap_box(box, world_filter_(filter)));
    }
    std::vector<components::Collider*> overlap_box(const geometry::OBB& box, uint32_t layer_mask = ~0u,
                                                     bool include_triggers = true) const {
        return colliders_for_(world_.overlap_box(box, layer_mask, include_triggers));
    }

    std::vector<components::Collider*> overlap_capsule(const geometry::Capsule& capsule,
                                                         const QueryFilter& filter) const {
        return colliders_for_(world_.overlap_capsule(capsule, world_filter_(filter)));
    }
    std::vector<components::Collider*> overlap_capsule(const geometry::Capsule& capsule,
                                                         uint32_t layer_mask = ~0u,
                                                         bool include_triggers = true) const {
        return colliders_for_(world_.overlap_capsule(capsule, layer_mask, include_triggers));
    }

    /** @brief The PRIMARY Collider bound to `id`, or nullptr if `id` doesn't address a bound
     *         body -- for a compound body (see Binding's doc) this is the one member driving
     *         Transform sync, not necessarily the exact child a query touched. Event
     *         resolution (dispatch_events_()) and overlap_sphere()/overlap_box()/
     *         overlap_capsule() both use this; raycast-family queries use collider_for_shape_()
     *         instead, which resolves the EXACT child (see query::RaycastHit::shape_index's doc). */
    components::Collider* collider_for(dynamics::BodyId id) const {
        // The generation first: collider_by_index_ is keyed by body INDEX, and an index is
        // recycled as soon as its body is destroyed. Gameplay code holds BodyIds across frames
        // (CharacterController::ground_body_ is one), so a stale id must resolve to nothing
        // rather than to whichever collider now occupies the slot.
        if (!world_.is_valid(id)) return nullptr;
        auto it = collider_by_index_.find(id.index);
        return it != collider_by_index_.end() ? it->second : nullptr;
    }
    ///@}

private:
    /** @brief The exact Collider owning shape slot `shape_index` (query::RaycastHit::
     *         shape_index's doc) -- for a compound body this resolves to the specific CHILD
     *         touched, unlike collider_for(BodyId)'s primary-only resolution. */
    components::Collider* collider_for_shape_(uint32_t shape_index) const {
        auto it = collider_by_shape_index_.find(shape_index);
        return it != collider_by_shape_index_.end() ? it->second : nullptr;
    }

    static QueryFilter scene_filter_(uint32_t layer_mask, bool include_triggers) {
        QueryFilter filter;
        filter.layer_mask = layer_mask;
        filter.include_triggers = include_triggers;
        return filter;
    }

    /** @brief Lowers a scene QueryFilter to the world's: both ignores become body ids (folded
     *         into the predicate when both are set), the Collider predicate a BodyId one. */
    query::QueryFilter world_filter_(const QueryFilter& filter) const {
        query::QueryFilter out;
        out.layer_mask = filter.layer_mask;
        out.include_triggers = filter.include_triggers;
        dynamics::BodyId ignore_a = filter.ignore_collider ? filter.ignore_collider->body_id() : dynamics::BodyId{};
        dynamics::BodyId ignore_b = filter.ignore_rigidbody ? filter.ignore_rigidbody->body_id() : dynamics::BodyId{};
        out.ignore = ignore_a.is_valid() ? ignore_a : ignore_b;
        const bool second_ignore = ignore_a.is_valid() && ignore_b.is_valid() && ignore_a != ignore_b;
        if (filter.predicate || second_ignore) {
            out.predicate = [this, predicate = filter.predicate, second_ignore, ignore_b](dynamics::BodyId id) {
                if (second_ignore && id == ignore_b) return false;
                if (!predicate) return true;
                const components::Collider* c = collider_for(id);
                return !c || predicate(*c);
            };
        }
        return out;
    }

    std::vector<components::Collider*> colliders_for_(const std::vector<dynamics::BodyId>& ids) const {
        std::vector<components::Collider*> out;
        for (dynamics::BodyId id : ids) {
            if (components::Collider* c = collider_for(id)) out.push_back(c);
        }
        return out;
    }

    void fill_hit_(const query::RaycastHit& raw, RaycastHit& out) const {
        out.started_inside = raw.started_inside;
        out.point = raw.point;
        out.normal = raw.normal;
        out.distance = raw.distance;
        out.collider = collider_for_shape_(raw.shape_index);
        if (!out.collider) out.collider = collider_for(raw.body); // fallback: e.g. a shapeless probe result
        out.object = out.collider ? out.collider->owner : nullptr;
        out.rigidbody = out.object ? out.object->get_component<components::RigidbodyComponent>() : nullptr;
    }

    PhysicsWorld world_;
    std::vector<Binding> bindings_;
    /** @brief Body index -> PRIMARY Collider* (collider_for()'s doc). */
    std::unordered_map<uint32_t, components::Collider*> collider_by_index_;
    /** @brief Shape slot index -> the exact Collider owning it (collider_for_shape_()'s doc). */
    std::unordered_map<uint32_t, components::Collider*> collider_by_shape_index_;
    /** @brief JointComponent* -> the dynamics::Joint it's currently bound to (see
     *         regather_joints_()'s doc); a flat vector, not a map, since it only ever needs
     *         full-scan diffing (same identity-diff pattern as bindings_'s own Collider set),
     *         never random lookup by component. */
    std::vector<std::pair<components::JointComponent*, dynamics::JointId>> joint_bindings_;
    /** @brief ClothComponent* -> the cloth::Cloth it's currently bound to; same flat-vector
     *         identity-diff shape as joint_bindings_ above, for the same reason. */
    std::vector<std::pair<components::ClothComponent*, cloth::ClothId>> cloth_bindings_;
    bool dirty_ = true;
    double last_step_ms_ = 0.0;   ///< See last_step_ms().
    std::size_t parallel_threshold_ = 64;
    std::vector<WriteBack> writeback_;
};

/**
 * @brief Constructs a PhysicsSystem and registers it, mirroring install_animation_system().
 *
 * @param scene Scene to install into.
 * @param order Registration order; defaults to UpdatePhase::Physics (100), the slot
 *              scene_system.h reserves for exactly this. Pass e.g. 325 for an app whose
 *              gameplay is entirely animation-driven platforms with dynamic riders, so
 *              physics runs after Animation(300) but before TransformResolve(350) --
 *              see the plan's "PhysicsSystem — reconcile, don't rebuild" for why 100 stays
 *              the default and why the value must stay below LateBehaviour(400) regardless.
 * @return Non-owning pointer to the installed system.
 */
inline PhysicsSystem* install_physics_system(coopa::scene::Scene& scene,
                                              int order = static_cast<int>(coopa::scene::UpdatePhase::Physics),
                                              const util::PhysicsConfig& config = {}) {
    auto sys = std::make_unique<PhysicsSystem>(config);
    PhysicsSystem* raw = sys.get();
    scene.add_system(std::move(sys), order);
    return raw;
}

/**
 * @brief Constructs a PhysicsSystem from a full project-wide PhysicsSettings (gravity, solver
 *        tunables, fixed timestep, layer collision matrix, job-parallel threshold) and
 *        registers it -- the settings-aware counterpart to the PhysicsConfig-only overload
 *        above. `settings` is a required argument here (unlike `order`) specifically so that
 *        `install_physics_system(scene)` alone still resolves unambiguously to the simpler
 *        overload.
 *
 * @param scene    Scene to install into.
 * @param settings Project-wide physics settings (see util/physics_settings.h), applied via
 *                 apply_physics_settings() immediately after construction.
 * @param order    Registration order; see the other overload's doc.
 * @return Non-owning pointer to the installed system.
 */
inline PhysicsSystem* install_physics_system(coopa::scene::Scene& scene,
                                              const util::PhysicsSettings& settings,
                                              int order = static_cast<int>(coopa::scene::UpdatePhase::Physics)) {
    auto sys = std::make_unique<PhysicsSystem>(settings.solver);
    PhysicsSystem* raw = sys.get();
    apply_physics_settings(raw->world(), settings);
    raw->set_parallel_threshold(settings.parallel_threshold);
    scene.add_system(std::move(sys), order);
    return raw;
}

} // namespace system
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_SYSTEM_PHYSICS_SYSTEM_H
