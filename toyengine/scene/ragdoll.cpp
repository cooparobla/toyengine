#include <toyengine/scene/ragdoll.h>

#include <coopa/rig/ik_components.h>
#include <coopa/rig/rig_pose_math.h>
#include <coopa/scene/component.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/scene_system.h>
#include <physxcoopa/components/box_collider.h>
#include <physxcoopa/components/capsule_collider.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/components/sphere_collider.h>
#include <physxcoopa/dynamics/integrator.h>
#include <physxcoopa/dynamics/joint.h>
#include <physxcoopa/geometry/ray.h>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/scene/foot_ik.h>

namespace toy {
namespace scene {

void Ragdoll::activate(const glm::vec3& impulse, std::optional<glm::vec3> point) {
    pending_ = Request::Activate;
    pending_impulse_ = impulse;
    pending_point_ = point;
}

void Ragdoll::deactivate(float blend, const std::string& state) {
    pending_ = Request::Deactivate;
    pending_blend_ = blend;
    pending_state_ = state;
}

void Ragdoll::toggle() {
    if (mode_ == Mode::Ragdoll || pending_ == Request::Activate) deactivate();
    else activate(toggle_impulse);
}

void Ragdoll::start() {
    if (!owner) return;
    // Tried optimistically: a synchronous asset loader (e.g. a bare unit-test harness) already
    // has the sibling Rig's RigAsset ready by now, and bone_count() etc. must answer correctly
    // with zero engine ticks. A real, async loader may not -- stage_pre_physics() retries build_()
    // every frame until it succeeds (see that method's doc), so a failure here is not final.
    build_();
    if (start_ragdoll) activate(start_impulse);
    if (!rt_.empty() && scene) {
        if (auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene->find_system("Physics"))) {
            physics->refresh(); // a ragdoll spawned into a running scene: bind its new bodies
        }
    }
}

void Ragdoll::stage_pre_physics(float dt, coopa::physx::system::PhysicsSystem* physics) {
    if (!physics) return;
    if (rt_.empty()) {
        // start()'s own build_() attempt may have found the sibling Rig's RigAsset still loading
        // (see that method's doc); retry here every frame until it resolves.
        build_();
        if (rt_.empty()) return; // still unresolved (or genuinely no Rig/bones) -- retry next frame
        if (scene) physics->refresh(); // new bodies just appeared: bind them
    }
    if (!ready_) {
        if (!create_joints_(*physics)) return;
        ready_ = true;
    }
    const Request req = pending_;
    pending_ = Request::None;
    if (req == Request::Activate && mode_ != Mode::Ragdoll) {
        go_limp_(*physics);
    } else if (req == Request::Deactivate && mode_ == Mode::Ragdoll) {
        begin_recovery_(*physics);
    } else if (req == Request::Activate && mode_ == Mode::Ragdoll) {
        // A blow landing on a body that is already limp -- a corpse being hit, a cart running
        // over one. It takes the impulse like any other, waking it if it had settled.
        apply_pending_impulse_(*physics);
    } else if (mode_ == Mode::Ragdoll) {
        rest_assist_(*physics, dt);
    }
    // Every limp frame, not only the first: a joint snapping taut or a deep contact resolving
    // can hand a bone a speed that would carry it through the ground in one substep.
    if (mode_ == Mode::Ragdoll) limit_limp_speed_(*physics);
}

void Ragdoll::stage_pre_animation() {
    if (mode_ != Mode::Blending || !rig_) return;
    // Identity = exactly the rest pose (Rig::PoseChannel's contract): this bind-poses the
    // ragdoll's own bones so the SkeletalAnimator's output is a complete target pose.
    for (BoneRt& b : rt_) rig_->pose(static_cast<size_t>(b.bone_idx)) = coopa::rig::Rig::PoseChannel{};
}

void Ragdoll::stage_post_animation(float dt) {
    if (mode_ != Mode::Blending || !rig_) return;
    blend_t_ += std::max(dt, 0.0f);
    const float x = blend_dur_ > 0.0f ? std::clamp(blend_t_ / blend_dur_, 0.0f, 1.0f) : 1.0f;
    const float w = x * x * (3.0f - 2.0f * x);
    std::vector<Pose> target(rt_.size());
    for (size_t i = 0; i < rt_.size(); ++i) {
        Pose anim;
        anim.position = coopa::rig::bone_world_position(*rig_, rt_[i].bone_idx);
        anim.rotation = coopa::rig::bone_world_rotation(*rig_, rt_[i].bone_idx);
        target[i].position = glm::mix(rt_[i].from.position, anim.position, w);
        target[i].rotation = glm::slerp(rt_[i].from.rotation, anim.rotation, w);
    }
    write_world_poses_(target);
    if (x >= 1.0f) {
        mode_ = Mode::Animated;
        set_ik_suspended_(false);
        set_bones_suspended_(false);
    }
}

Ragdoll::Pose Ragdoll::world_pose_(const coopa::scene::SceneObject* obj) {
    const glm::mat4 m = obj->get_transform()->transform().get_world_matrix();
    Pose p;
    glm::vec3 scale, skew;
    glm::vec4 persp;
    glm::decompose(m, scale, p.rotation, p.position, skew, persp);
    return p;
}

void Ragdoll::auto_generate_() {
    if (!rig_) return;
    for (size_t i = 0; i < rig_->bone_count(); ++i) {
        RagdollBone b;
        b.bone = rig_->bone_name(i);
        const float len = rig_->bone_length(i);
        if (len < 0.05f) {
            // Too short to carry a capsule along its own axis -- a small sphere at the head.
            b.shape = RagdollBone::Shape::Sphere;
            b.radius = 0.08f;
        } else {
            // A bone's own rest axis is local +Y, head to tail, by the rig's convention
            // (rig_asset.h) -- no need to look at children to find "the next joint".
            b.direction = 1;
            b.radius = std::clamp(len * 0.22f, 0.03f, 0.2f);
            b.height = std::max(len, 2.0f * b.radius + 0.01f);
            b.center = glm::vec3(0.0f, len * 0.5f, 0.0f);
            b.axis = glm::vec3(0.0f, 1.0f, 0.0f);
        }
        b.joint = RagdollBone::Joint::ConeTwist;
        bones.push_back(b);
    }
}

float Ragdoll::shape_volume_(const RagdollBone& b) {
    constexpr float k_pi = 3.14159265f;
    switch (b.shape) {
        case RagdollBone::Shape::Box: return b.size.x * b.size.y * b.size.z;
        case RagdollBone::Shape::Sphere: return (4.0f / 3.0f) * k_pi * b.radius * b.radius * b.radius;
        case RagdollBone::Shape::Capsule:
        default: {
            const float cyl = std::max(0.0f, b.height - 2.0f * b.radius);
            return k_pi * b.radius * b.radius * cyl + (4.0f / 3.0f) * k_pi * b.radius * b.radius * b.radius;
        }
    }
}

coopa::scene::SceneObject* Ragdoll::proxy_object_(const RagdollBone& spec) {
    const std::string name = "_ragdoll_" + spec.bone;
    for (const auto& c : owner->children()) if (c->name() == name) return c.get();

    auto proxy_owned = std::make_unique<coopa::scene::SceneObject>(name);
    auto* tc = proxy_owned->add_component<coopa::scene::TransformComponent>();
    tc->scene = scene;
    if (auto* owner_tc = owner->get_transform()) tc->set_parent_transform(&owner_tc->transform());
    auto* att = proxy_owned->add_component<coopa::rig::BoneAttachment>();
    att->scene = scene;
    att->bone = spec.bone;
    att->at = coopa::rig::BoneAttachment::Anchor::Head;
    coopa::scene::SceneObject* proxy = owner->add_child(std::move(proxy_owned));
    // Programmatically-added components get no automatic start() call; this one must run now
    // -- AFTER add_child() (so its ancestor walk for a Rig finds owner's, not a dead end at an
    // unparented object) and BEFORE the seeding in build_() below touches the proxy's
    // Transform, so it captures an identity offset (the proxy rides the bone's head exactly)
    // rather than that seeded value. capture_offset_() only captures once (see its own doc),
    // so the scene's own start() calling this a second time later is harmless.
    att->start();
    return proxy;
}

void Ragdoll::build_() {
    rt_.clear();
    rig_ = nullptr;
    for (coopa::scene::SceneObject* o = owner; o; o = o->parent()) {
        if (auto* r = o->get_component<coopa::rig::Rig>()) { rig_ = r; break; }
    }
    if (!rig_) {
        std::cerr << "[Ragdoll] '" << (owner ? owner->name() : std::string("?")) << "': no Rig ancestor" << std::endl;
        return;
    }
    // Still loading (see start()'s doc): return quietly and let stage_pre_physics() retry next
    // frame, rather than resolving every configured bone name against an empty asset and
    // reporting each one as genuinely missing.
    if (!rig_->is_loaded()) return;
    if (bones.empty() && auto_generate) auto_generate_();
    if (bones.empty()) return;

    std::vector<BoneRt> list;
    for (size_t i = 0; i < bones.size(); ++i) {
        BoneRt b;
        b.bone_idx = rig_->find_bone(bones[i].bone);
        if (b.bone_idx < 0) {
            std::cerr << "[Ragdoll] '" << (owner ? owner->name() : std::string("?")) << "': no bone named '"
                      << bones[i].bone << "' on the rig -- skipped" << std::endl;
            continue;
        }
        b.spec = i;
        list.push_back(b);
    }
    // A RigAsset's bones are stored flat, parent-first (rig_asset.h), so sorting by bone index
    // IS parent-before-child order -- write_world_poses_() and the joints rely on it.
    std::stable_sort(list.begin(), list.end(), [](const BoneRt& x, const BoneRt& y) { return x.bone_idx < y.bone_idx; });

    float free_volume = 0.0f, fixed_mass = 0.0f;
    for (const BoneRt& b : list) {
        const RagdollBone& s = bones[b.spec];
        if (s.mass > 0.0f) fixed_mass += s.mass;
        else free_volume += shape_volume_(s);
    }
    const float spare_mass = std::max(mass - fixed_mass, 0.0f);

    const glm::mat4 rig_world = owner->get_transform()->get_world_matrix();
    for (size_t i = 0; i < list.size(); ++i) {
        BoneRt& b = list[i];
        // Nearest ANCESTOR bone in the list (the file doc): walk the rig's own parent chain,
        // not just one level -- a ragdoll bone can skip rig bones that carry no body of their
        // own (mannequin_ragdoll's "head" joints straight to "chest", skipping "neck").
        for (int p = rig_->bone_parent(static_cast<size_t>(b.bone_idx)); p >= 0 && b.parent < 0;
             p = rig_->bone_parent(static_cast<size_t>(p))) {
            for (size_t k = 0; k < i; ++k) {
                if (list[k].bone_idx == p) { b.parent = static_cast<int>(k); break; }
            }
        }

        const glm::mat4 bind_m = rig_world * rig_->rest_model_matrix(static_cast<size_t>(b.bone_idx));
        glm::vec3 skew;
        glm::vec4 persp;
        glm::decompose(bind_m, b.scale, b.bind_world.rotation, b.bind_world.position, skew, persp);

        const RagdollBone& s = bones[b.spec];
        b.com = s.center * b.scale;
        float body_mass = s.mass;
        if (body_mass <= 0.0f) {
            body_mass = free_volume > 0.0f ? spare_mass * shape_volume_(s) / free_volume : 1.0f;
            body_mass = std::max(body_mass, 0.2f);
        }

        b.obj = proxy_object_(s);
        // Seed the proxy at the bind pose immediately: BoneAttachmentSystem (order 345) will
        // also place it, but that is later this same frame, and Physics (order 100) must not
        // see it sitting at the origin for one frame.
        {
            const glm::mat4 local = glm::inverse(rig_world) * bind_m;
            glm::vec3 lpos, lscale, lskew;
            glm::quat lrot;
            glm::vec4 lpersp;
            if (glm::decompose(local, lscale, lrot, lpos, lskew, lpersp)) {
                coopa::util::Transform& lt = b.obj->get_transform()->transform();
                lt.set_position(lpos);
                lt.set_rotation_quat(glm::normalize(lrot));
            }
        }

        using namespace coopa::physx::components;
        b.rb = b.obj->get_component<RigidbodyComponent>();
        if (!b.rb) b.rb = b.obj->add_component<RigidbodyComponent>();
        b.rb->scene = scene;
        b.rb->mass = body_mass;
        b.rb->is_kinematic = true;   // Animated until the joints exist (see stage_pre_physics())
        b.rb->use_gravity = true;
        b.rb->drag = drag;
        b.rb->angular_drag = angular_drag;
        Collider* col = b.obj->get_component<Collider>();
        if (!col) {
            switch (s.shape) {
                case RagdollBone::Shape::Box: {
                    auto* c = b.obj->add_component<BoxCollider>();
                    c->set_size(s.size);
                    col = c;
                    break;
                }
                case RagdollBone::Shape::Sphere: {
                    auto* c = b.obj->add_component<SphereCollider>();
                    c->set_radius(s.radius);
                    col = c;
                    break;
                }
                case RagdollBone::Shape::Capsule:
                default: {
                    auto* c = b.obj->add_component<CapsuleCollider>();
                    c->set_radius(s.radius);
                    c->set_height(s.height);
                    c->set_direction(std::clamp(s.direction, 0, 2));
                    col = c;
                    break;
                }
            }
        }
        col->scene = scene;
        col->set_center(s.center);
        col->set_layer(layer);
    }
    rt_ = std::move(list);
    ready_ = false;
    mode_ = Mode::Animated;
}

bool Ragdoll::create_joints_(coopa::physx::system::PhysicsSystem& physics) {
    namespace dyn = coopa::physx::dynamics;
    coopa::physx::PhysicsWorld& world = physics.world();
    for (const BoneRt& b : rt_) {
        if (!b.rb || !world.is_valid(b.rb->body_id())) return false;
    }
    for (BoneRt& b : rt_) {
        if (b.parent < 0) continue;
        const BoneRt& p = rt_[static_cast<size_t>(b.parent)];
        const RagdollBone& s = bones[b.spec];
        const glm::quat rp = p.bind_world.rotation, rb = b.bind_world.rotation;
        const glm::vec3 body_p = p.bind_world.position + rp * p.com;
        const glm::vec3 body_b = b.bind_world.position + rb * b.com;
        const glm::vec3 anchor = b.bind_world.position + rb * (s.anchor * b.scale);

        glm::vec3 axis = s.axis;
        if (glm::length(axis) < 1e-6f) {
            axis = glm::length(s.center) > 1e-4f ? s.center : glm::vec3(0.0f);
            if (glm::length(axis) < 1e-6f) axis[std::clamp(s.direction, 0, 2)] = 1.0f;
        }
        axis = glm::normalize(axis);
        const glm::vec3 world_axis = rb * axis;

        dyn::Joint j;
        j.type = s.joint == RagdollBone::Joint::Hinge ? dyn::JointType::Hinge
               : s.joint == RagdollBone::Joint::Ball ? dyn::JointType::Ball
                                                     : dyn::JointType::ConeTwist;
        j.a = p.rb->body_id();
        j.b = b.rb->body_id();
        j.local_anchor_a = glm::inverse(rp) * (anchor - body_p);
        j.local_anchor_b = glm::inverse(rb) * (anchor - body_b);
        j.local_axis_a = glm::inverse(rp) * world_axis;
        j.local_axis_b = axis;
        j.rest_relative_rotation = glm::inverse(rp) * rb;
        j.collide_connected = collide_connected;
        if (j.type == dyn::JointType::Hinge) {
            j.use_limits = true;
            j.min_angle = glm::radians(s.hinge_min_deg);
            j.max_angle = glm::radians(s.hinge_max_deg);
        } else if (j.type == dyn::JointType::ConeTwist) {
            j.swing_limit = s.swing_deg >= 0.0f ? glm::radians(s.swing_deg) : -1.0f;
            j.twist_min = glm::radians(s.twist_min_deg);
            j.twist_max = glm::radians(s.twist_max_deg);
        }
        b.joint = world.add_joint(j);
    }
    // Near bones overlap by design around a joint they do not share (the spine and the
    // upper arms at the shoulders, the two thighs at the hips): contacts there would fight
    // the joints forever and keep the body from ever resting. Bones within two links of
    // each other (grandparent / sibling) never collide; the rest still do.
    auto chain = [&](size_t i) {
        std::vector<int> up;
        for (int k = static_cast<int>(i); k >= 0; k = rt_[static_cast<size_t>(k)].parent) up.push_back(k);
        return up;
    };
    for (size_t i = 0; i < rt_.size(); ++i) {
        const std::vector<int> ci = chain(i);
        for (size_t k = i + 1; k < rt_.size(); ++k) {
            const std::vector<int> ck = chain(k);
            int dist = 1 << 20;
            for (size_t a = 0; a < ci.size(); ++a) {
                for (size_t c = 0; c < ck.size(); ++c) {
                    if (ci[a] == ck[c]) dist = std::min(dist, static_cast<int>(a + c));
                }
            }
            if (dist <= 2) world.set_pair_collision(rt_[i].rb->body_id(), rt_[k].rb->body_id(), false);
        }
    }
    return true;
}

void Ragdoll::set_ik_suspended_(bool suspended) {
    if (suspended == ik_suspended_) return;
    ik_suspended_ = suspended;
    if (suspended) saved_ik_weights_.clear();
    size_t k = 0;
    owner->for_each_recursive([&](const coopa::scene::SceneObject& o) {
        for (const auto& comp : o.components()) {
            if (auto* f = dynamic_cast<FootIK*>(comp.get())) {
                f->set_suspended(suspended);
            } else if (auto* tb = dynamic_cast<coopa::rig::TwoBoneIK*>(comp.get())) {
                if (suspended) { saved_ik_weights_.push_back(tb->weight); tb->weight = 0.0f; }
                else if (k < saved_ik_weights_.size()) tb->weight = saved_ik_weights_[k++];
            } else if (auto* la = dynamic_cast<coopa::rig::LookAtIK*>(comp.get())) {
                if (suspended) { saved_ik_weights_.push_back(la->weight); la->weight = 0.0f; }
                else if (k < saved_ik_weights_.size()) la->weight = saved_ik_weights_[k++];
            }
        }
    });
}

void Ragdoll::set_bones_suspended_(bool suspended) {
    for (BoneRt& b : rt_) {
        if (!b.obj) continue;
        if (auto* att = b.obj->get_component<coopa::rig::BoneAttachment>()) att->suspended = suspended;
    }
}

void Ragdoll::go_limp_(coopa::physx::system::PhysicsSystem& physics) {
    namespace dyn = coopa::physx::dynamics;
    coopa::physx::PhysicsWorld& world = physics.world();
    if (auto* a = animator_()) {
        if (recover_state.empty()) recover_state = !a->auto_play.empty() ? a->auto_play : a->current_state();
        a->clear();
    }
    set_ik_suspended_(true);
    set_bones_suspended_(true);
    if (CharacterController* cc = character_()) cc->set_suspended(true);

    for (BoneRt& b : rt_) {
        b.rb->is_kinematic = false;
        const dyn::BodyId id = b.rb->body_id();
        world.set_body_type(id, dyn::BodyType::Dynamic, b.rb->mass);
        if (dyn::Body* body = world.get_body(id)) {
            dyn::update_world_inertia(*body);
            body->wake();
        }
    }
    mode_ = Mode::Ragdoll;
    rest_timer_ = 0.0f;
    rest_anchor_.clear();
    // Before anything else reads them: the velocities the bones arrive with are the animation's
    // (see `max_limp_speed`), and the impulse below is meant to be felt on top of a sane body.
    limit_limp_speed_(physics);
    apply_pending_impulse_(physics);
}

void Ragdoll::limit_limp_speed_(coopa::physx::system::PhysicsSystem& physics) {
    if (max_limp_speed <= 0.0f && max_limp_spin <= 0.0f) return;
    coopa::physx::PhysicsWorld& world = physics.world();
    for (BoneRt& b : rt_) {
        coopa::physx::dynamics::Body* body = world.get_body(b.rb->body_id());
        if (!body || body->type != coopa::physx::dynamics::BodyType::Dynamic) continue;
        if (max_limp_speed > 0.0f) {
            const float speed = glm::length(body->linear_velocity);
            if (speed > max_limp_speed) body->linear_velocity *= max_limp_speed / speed;
        }
        if (max_limp_spin > 0.0f) {
            const float spin = glm::length(body->angular_velocity);
            if (spin > max_limp_spin) body->angular_velocity *= max_limp_spin / spin;
        }
    }
}

void Ragdoll::apply_pending_impulse_(coopa::physx::system::PhysicsSystem& physics) {
    namespace dyn = coopa::physx::dynamics;
    coopa::physx::PhysicsWorld& world = physics.world();
    if (glm::length(pending_impulse_) > 0.0f) {
        if (pending_point_) {
            dyn::Body* best = nullptr;
            float best_d = 1e30f;
            for (BoneRt& b : rt_) {
                dyn::Body* body = world.get_body(b.rb->body_id());
                if (!body) continue;
                const float d = glm::length(body->position - *pending_point_);
                if (d < best_d) { best_d = d; best = body; }
            }
            if (best) best->apply_impulse_at_position(pending_impulse_, *pending_point_);
        } else {
            float total = 0.0f;
            for (BoneRt& b : rt_) {
                if (const dyn::Body* body = world.get_body(b.rb->body_id())) total += body->mass;
            }
            if (total > 0.0f) {
                for (BoneRt& b : rt_) {
                    if (dyn::Body* body = world.get_body(b.rb->body_id())) {
                        body->apply_impulse(pending_impulse_ * (body->mass / total));
                    }
                }
            }
        }
        // The shove restarts the clock: a body that was a frame from resting must not be frozen
        // in the pose it was in before the blow landed.
        rest_timer_ = 0.0f;
    }
    pending_impulse_ = glm::vec3(0.0f);
    pending_point_.reset();
}

void Ragdoll::rest_assist_(coopa::physx::system::PhysicsSystem& physics, float dt) {
    if (rest_time <= 0.0f) return;
    coopa::physx::PhysicsWorld& world = physics.world();
    if (rest_anchor_.size() != rt_.size()) {
        rest_anchor_.assign(rt_.size(), glm::vec3(0.0f));
        rest_timer_ = 0.0f;
        for (size_t i = 0; i < rt_.size(); ++i) {
            if (const coopa::physx::dynamics::Body* body = world.get_body(rt_[i].rb->body_id())) {
                rest_anchor_[i] = body->position;
            }
        }
        return;
    }

    bool any_awake = false, stayed = true;
    for (size_t i = 0; i < rt_.size(); ++i) {
        const coopa::physx::dynamics::Body* body = world.get_body(rt_[i].rb->body_id());
        if (!body) continue;
        any_awake = any_awake || body->awake;
        stayed = stayed && glm::length(body->position - rest_anchor_[i]) < rest_travel;
    }
    if (!any_awake) {
        rest_timer_ = 0.0f;
        return;
    }
    if (!stayed) {
        // Moved: this is where the body is now, and the window starts again from here.
        for (size_t i = 0; i < rt_.size(); ++i) {
            if (const coopa::physx::dynamics::Body* body = world.get_body(rt_[i].rb->body_id())) {
                rest_anchor_[i] = body->position;
            }
        }
        rest_timer_ = 0.0f;
        return;
    }

    rest_timer_ += std::max(dt, 0.0f);
    if (rest_timer_ < rest_time) return;
    for (BoneRt& b : rt_) {
        if (coopa::physx::dynamics::Body* body = world.get_body(b.rb->body_id())) body->sleep();
    }
    rest_timer_ = 0.0f;
}

void Ragdoll::begin_recovery_(coopa::physx::system::PhysicsSystem& physics) {
    namespace dyn = coopa::physx::dynamics;
    coopa::physx::PhysicsWorld& world = physics.world();
    for (BoneRt& b : rt_) b.from = world_pose_(b.obj);

    // Stand the root up under the pelvis (the top bone), facing the way the body lies.
    const BoneRt& top = rt_.front();
    const glm::vec3 pelvis = top.from.position;
    const glm::vec3 top_rig_space_head = rig_ ? rig_->bone_head_rest(static_cast<size_t>(top.bone_idx)) : glm::vec3(0.0f);
    glm::vec3 ground = pelvis - glm::vec3(0.0f, 0.0f, top_rig_space_head.z);
    {
        coopa::physx::system::PhysicsSystem::QueryFilter filter;
        filter.include_triggers = false;
        filter.predicate = [self = owner](const coopa::physx::components::Collider& c) {
            for (const coopa::scene::SceneObject* o = c.owner; o; o = o->parent())
                if (o == self) return false;
            return true;
        };
        coopa::physx::geometry::Ray ray;
        ray.origin = pelvis + glm::vec3(0.0f, 0.0f, 0.5f);
        ray.direction = glm::vec3(0.0f, 0.0f, -1.0f);
        ray.max_distance = 4.0f;
        coopa::physx::system::PhysicsSystem::RaycastHit hit;
        if (physics.raycast(ray, hit, filter)) ground = hit.point;
    }
    glm::vec3 fwd = top.from.rotation * glm::vec3(0.0f, 1.0f, 0.0f);
    if (glm::length(glm::vec2(fwd.x, fwd.y)) < 0.3f) {
        // Lying face up/down: the forward axis points at the sky -- face along the spine.
        fwd = top.from.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    }
    const float yaw = glm::length(glm::vec2(fwd.x, fwd.y)) > 1e-4f ? glm::degrees(std::atan2(-fwd.x, fwd.y)) : 0.0f;
    const glm::vec3 root_pos(pelvis.x, pelvis.y, ground.z);

    if (CharacterController* cc = character_()) {
        cc->teleport(root_pos, yaw);
        cc->set_suspended(false);
    } else if (reposition_root && owner->get_transform() && !owner->parent()) {
        coopa::util::Transform& t = owner->get_transform()->transform();
        t.set_position(root_pos);
        t.set_rotation(glm::vec3(0.0f, 0.0f, yaw));
    }

    for (BoneRt& b : rt_) {
        b.rb->is_kinematic = true;
        world.set_body_type(b.rb->body_id(), dyn::BodyType::Kinematic);
    }
    rest_anchor_.clear();
    // Hold the ragdoll pose under the moved root now, so this frame's physics step sees the
    // bones where they lay (zero kinematic velocity, nothing knocked away).
    std::vector<Pose> hold(rt_.size());
    for (size_t i = 0; i < rt_.size(); ++i) hold[i] = rt_[i].from;
    write_world_poses_(hold);

    if (auto* a = animator_()) {
        const std::string state = !pending_state_.empty() ? pending_state_ : recover_state;
        if (!state.empty() && a->find_state(state)) a->play(state);
    }
    blend_dur_ = pending_blend_ >= 0.0f ? pending_blend_ : blend_time;
    blend_t_ = 0.0f;
    mode_ = Mode::Blending;
}

void Ragdoll::set_bone_world_pose_(int bone_idx, const glm::vec3& pos, const glm::quat& rot) {
    if (!rig_ || bone_idx < 0) return;
    const glm::quat cur_rot = coopa::rig::bone_world_rotation(*rig_, bone_idx);
    coopa::rig::apply_world_rotation_delta(*rig_, bone_idx, rot * glm::inverse(cur_rot), 1.0f);
    const glm::vec3 cur_pos = coopa::rig::bone_world_position(*rig_, bone_idx);
    coopa::rig::apply_world_translation_delta(*rig_, bone_idx, pos - cur_pos);
}

void Ragdoll::write_world_poses_(const std::vector<Pose>& world) {
    if (!rig_) return;
    // Every proxy is a flat child of owner (see proxy_object_()), so its local transform is
    // simply the desired world pose expressed in owner's frame -- no ancestor-chain
    // composition needed; the ragdoll's own hierarchy lives in the physics joints, not in the
    // scene graph. rt_ is kept parent-bone-before-child-bone (build_()'s doc), so by the time a
    // child bone's set_bone_world_pose_() reads its "current" world pose, its parent bone's
    // target for this call has already been written into the Rig.
    const glm::mat4 owner_world = owner->get_transform()->get_world_matrix();
    const glm::mat4 inv_owner_world = glm::inverse(owner_world);
    for (size_t i = 0; i < rt_.size(); ++i) {
        set_bone_world_pose_(rt_[i].bone_idx, world[i].position, world[i].rotation);
        if (!rt_[i].obj || !rt_[i].obj->get_transform()) continue;
        const glm::mat4 local = inv_owner_world *
                                (glm::translate(glm::mat4(1.0f), world[i].position) * glm::mat4_cast(world[i].rotation));
        glm::vec3 pos, scale, skew;
        glm::quat rot;
        glm::vec4 persp;
        if (!glm::decompose(local, scale, rot, pos, skew, persp)) continue;
        coopa::util::Transform& t = rt_[i].obj->get_transform()->transform();
        t.set_position(pos);
        t.set_rotation_quat(glm::normalize(rot));
    }
}

const char* RagdollSystem::system_name() const {
    switch (stage_) {
        case Stage::PrePhysics: return "RagdollPrePhysics";
        case Stage::PreAnimation: return "RagdollPreAnimation";
        case Stage::PostAnimation:
        default: return "RagdollPostAnimation";
    }
}

void RagdollSystem::execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) {
    std::vector<Ragdoll*> ragdolls = scene.get_components<Ragdoll>();
    if (ragdolls.empty()) return;
    switch (stage_) {
        case Stage::PrePhysics: {
            auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"));
            for (Ragdoll* r : ragdolls) r->stage_pre_physics(ctx.delta_time, physics);
            break;
        }
        case Stage::PreAnimation:
            for (Ragdoll* r : ragdolls) r->stage_pre_animation();
            break;
        case Stage::PostAnimation:
            for (Ragdoll* r : ragdolls) r->stage_post_animation(ctx.delta_time);
            break;
    }
}

void install_ragdoll_system(coopa::scene::Scene& scene) {
    scene.add_system(std::make_unique<RagdollSystem>(RagdollSystem::Stage::PrePhysics), k_ragdoll_pre_physics_order);
    scene.add_system(std::make_unique<RagdollSystem>(RagdollSystem::Stage::PreAnimation), k_ragdoll_pre_animation_order);
    scene.add_system(std::make_unique<RagdollSystem>(RagdollSystem::Stage::PostAnimation), k_ragdoll_post_animation_order);
}

} // namespace scene
} // namespace toy
