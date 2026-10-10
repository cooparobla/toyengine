#include <toyengine/scene/ragdoll.h>

#include <coopa/animation/ik_components.h>
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
    build_();
    if (start_ragdoll) activate(start_impulse);
    if (scene) {
        if (auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene->find_system("Physics"))) {
            physics->refresh(); // a ragdoll spawned into a running scene: bind its new bodies
        }
    }
}

void Ragdoll::stage_pre_physics(float dt, coopa::physx::system::PhysicsSystem* physics) {
    if (!physics || rt_.empty()) return;
    if (!ready_) {
        if (!create_joints_(*physics)) return;
        ready_ = true;
    }
    const Request req = pending_;
    pending_ = Request::None;
    if (req == Request::Activate && mode_ != Mode::Ragdoll) go_limp_(*physics);
    else if (req == Request::Deactivate && mode_ == Mode::Ragdoll) begin_recovery_(*physics);
    else if (mode_ == Mode::Ragdoll) rest_assist_(*physics, dt);
}

void Ragdoll::stage_pre_animation() {
    if (mode_ != Mode::Blending) return;
    for (BoneRt& b : rt_) {
        coopa::util::Transform& t = b.obj->get_transform()->transform();
        t.set_position(b.bind_local_pos);
        t.set_rotation_quat(b.bind_local_rot);
    }
}

void Ragdoll::stage_post_animation(float dt) {
    if (mode_ != Mode::Blending) return;
    blend_t_ += std::max(dt, 0.0f);
    const float x = blend_dur_ > 0.0f ? std::clamp(blend_t_ / blend_dur_, 0.0f, 1.0f) : 1.0f;
    const float w = x * x * (3.0f - 2.0f * x);
    std::vector<Pose> target(rt_.size());
    for (size_t i = 0; i < rt_.size(); ++i) {
        const Pose anim = world_pose_(rt_[i].obj);
        target[i].position = glm::mix(rt_[i].from.position, anim.position, w);
        target[i].rotation = glm::slerp(rt_[i].from.rotation, anim.rotation, w);
    }
    write_world_poses_(target);
    if (x >= 1.0f) {
        mode_ = Mode::Animated;
        set_ik_suspended_(false);
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

int Ragdoll::depth_(const coopa::scene::SceneObject* o) {
    int d = 0;
    for (; o; o = o->parent()) ++d;
    return d;
}

coopa::scene::SceneObject* Ragdoll::resolve_(const std::string& path) const {
    coopa::scene::SceneObject* cur = owner;
    size_t start = 0;
    while (cur && start <= path.size()) {
        size_t end = path.find('/', start);
        const std::string name = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!name.empty()) {
            coopa::scene::SceneObject* next = nullptr;
            for (const auto& c : cur->children()) {
                if (c->name() == name) { next = c.get(); break; }
            }
            cur = next;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return cur == owner ? nullptr : cur;
}

std::string Ragdoll::path_of_(const coopa::scene::SceneObject* o) const {
    std::string p;
    for (; o && o != owner; o = o->parent()) p = p.empty() ? o->name() : o->name() + "/" + p;
    return p;
}

void Ragdoll::auto_generate_() {
    std::vector<coopa::scene::SceneObject*> joints;
    owner->for_each_recursive([&](const coopa::scene::SceneObject& o) {
        if (&o != owner && only_transform_(o)) joints.push_back(const_cast<coopa::scene::SceneObject*>(&o));
    });
    auto child_bones = [&](const coopa::scene::SceneObject* o) {
        std::vector<coopa::scene::SceneObject*> out;
        for (const auto& c : o->children()) {
            if (std::find(joints.begin(), joints.end(), c.get()) != joints.end()) out.push_back(c.get());
        }
        return out;
    };
    for (coopa::scene::SceneObject* j : joints) {
        RagdollBone b;
        b.bone = path_of_(j);
        const std::vector<coopa::scene::SceneObject*> kids = child_bones(j);
        glm::vec3 seg(0.0f);
        if (kids.size() == 1) {
            seg = kids[0]->get_transform()->transform().position();
        } else if (kids.size() > 1) {
            for (auto* k : kids) seg += k->get_transform()->transform().position();
            seg /= static_cast<float>(kids.size());
        } else if (j->parent()) {
            // A leaf (hand, foot, head): half its parent segment's length, onward.
            const glm::vec3 from_parent = j->get_transform()->transform().position();
            seg = from_parent * 0.5f;
        }
        float len = glm::length(seg);
        if (len < 0.05f) {
            // A hub (pelvis: children spread around it) -- a sphere covering their reach.
            float reach = 0.0f;
            for (auto* k : kids) reach = std::max(reach, glm::length(k->get_transform()->transform().position()));
            b.shape = RagdollBone::Shape::Sphere;
            b.radius = std::max(0.05f, reach * 0.6f);
        } else {
            // A capsule in the bone's frame, rotated onto `seg` by direction + an axis pick:
            // the dominant component of `seg` picks X/Y/Z (rigs are axis-aligned at bind).
            glm::vec3 a = glm::abs(seg);
            b.direction = (a.x >= a.y && a.x >= a.z) ? 0 : (a.y >= a.z ? 1 : 2);
            b.radius = std::clamp(len * 0.22f, 0.03f, 0.2f);
            b.height = std::max(len, 2.0f * b.radius + 0.01f);
            b.center = seg * 0.5f;
            b.axis = glm::normalize(seg);
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

void Ragdoll::build_() {
    rt_.clear();
    if (bones.empty() && auto_generate) auto_generate_();
    if (bones.empty()) return;

    std::vector<BoneRt> list;
    for (size_t i = 0; i < bones.size(); ++i) {
        BoneRt b;
        b.obj = resolve_(bones[i].bone);
        if (!b.obj || !b.obj->get_transform()) {
            std::cerr << "[Ragdoll] '" << (owner ? owner->name() : std::string("?")) << "': bone '" << bones[i].bone
                      << "' not found -- skipped" << std::endl;
            continue;
        }
        b.spec = i;
        list.push_back(b);
    }
    // Parents before children (write_world_poses_() and the joints rely on it).
    std::stable_sort(list.begin(), list.end(),
                     [](const BoneRt& x, const BoneRt& y) { return depth_(x.obj) < depth_(y.obj); });

    float free_volume = 0.0f, fixed_mass = 0.0f;
    for (const BoneRt& b : list) {
        const RagdollBone& s = bones[b.spec];
        if (s.mass > 0.0f) fixed_mass += s.mass;
        else free_volume += shape_volume_(s);
    }
    const float spare_mass = std::max(mass - fixed_mass, 0.0f);

    for (size_t i = 0; i < list.size(); ++i) {
        BoneRt& b = list[i];
        for (const coopa::scene::SceneObject* p = b.obj->parent(); p && p != owner && b.parent < 0; p = p->parent()) {
            for (size_t k = 0; k < i; ++k) {
                if (list[k].obj == p) { b.parent = static_cast<int>(k); break; }
            }
        }
        coopa::util::Transform& t = b.obj->get_transform()->transform();
        b.bind_local_pos = t.position();
        b.bind_local_rot = t.rotation_quat();
        const glm::mat4 m = t.get_world_matrix();
        glm::vec3 skew;
        glm::vec4 persp;
        glm::decompose(m, b.scale, b.bind_world.rotation, b.bind_world.position, skew, persp);

        const RagdollBone& s = bones[b.spec];
        b.com = s.center * b.scale;
        float body_mass = s.mass;
        if (body_mass <= 0.0f) {
            body_mass = free_volume > 0.0f ? spare_mass * shape_volume_(s) / free_volume : 1.0f;
            body_mass = std::max(body_mass, 0.2f);
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
        Collider* col = nullptr;
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
            } else if (auto* tb = dynamic_cast<coopa::anim::TwoBoneIK*>(comp.get())) {
                if (suspended) { saved_ik_weights_.push_back(tb->weight); tb->weight = 0.0f; }
                else if (k < saved_ik_weights_.size()) tb->weight = saved_ik_weights_[k++];
            } else if (auto* la = dynamic_cast<coopa::anim::LookAtIK*>(comp.get())) {
                if (suspended) { saved_ik_weights_.push_back(la->weight); la->weight = 0.0f; }
                else if (k < saved_ik_weights_.size()) la->weight = saved_ik_weights_[k++];
            }
        }
    });
}

void Ragdoll::go_limp_(coopa::physx::system::PhysicsSystem& physics) {
    namespace dyn = coopa::physx::dynamics;
    coopa::physx::PhysicsWorld& world = physics.world();
    if (auto* a = animator_()) {
        if (recover_state.empty()) recover_state = !a->auto_play.empty() ? a->auto_play : a->current_state();
        a->clear();
    }
    set_ik_suspended_(true);
    if (CharacterController* cc = character_()) cc->set_suspended(true);

    float total = 0.0f;
    for (BoneRt& b : rt_) {
        b.rb->is_kinematic = false;
        const dyn::BodyId id = b.rb->body_id();
        world.set_body_type(id, dyn::BodyType::Dynamic, b.rb->mass);
        if (dyn::Body* body = world.get_body(id)) {
            dyn::update_world_inertia(*body);
            body->wake();
            total += body->mass;
        }
    }
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
        } else if (total > 0.0f) {
            for (BoneRt& b : rt_) {
                if (dyn::Body* body = world.get_body(b.rb->body_id())) {
                    body->apply_impulse(pending_impulse_ * (body->mass / total));
                }
            }
        }
    }
    pending_impulse_ = glm::vec3(0.0f);
    pending_point_.reset();
    mode_ = Mode::Ragdoll;
}

void Ragdoll::rest_assist_(coopa::physx::system::PhysicsSystem& physics, float dt) {
    if (rest_time <= 0.0f) return;
    coopa::physx::PhysicsWorld& world = physics.world();
    bool any_awake = false, calm = true;
    for (BoneRt& b : rt_) {
        const coopa::physx::dynamics::Body* body = world.get_body(b.rb->body_id());
        if (!body || !body->awake) continue;
        any_awake = true;
        calm = calm && glm::length(body->linear_velocity) < rest_speed && glm::length(body->angular_velocity) < rest_spin;
    }
    if (!any_awake || !calm) {
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
    glm::vec3 ground = pelvis - glm::vec3(0.0f, 0.0f, top.bind_local_pos.z);
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

void Ragdoll::write_world_poses_(const std::vector<Pose>& world) {
    const size_t n = rt_.size();
    std::vector<glm::mat4> old_world(n), new_world(n), parent_old(n);
    for (size_t i = 0; i < n; ++i) {
        const coopa::util::Transform& t = rt_[i].obj->get_transform()->transform();
        old_world[i] = t.get_world_matrix();
        parent_old[i] = t.parent() ? t.parent()->get_world_matrix() : glm::mat4(1.0f);
        const glm::vec3 scale(glm::length(glm::vec3(old_world[i][0])), glm::length(glm::vec3(old_world[i][1])),
                              glm::length(glm::vec3(old_world[i][2])));
        new_world[i] = glm::translate(glm::mat4(1.0f), world[i].position) * glm::mat4_cast(world[i].rotation) *
                       glm::scale(glm::mat4(1.0f), scale);
    }
    for (size_t i = 0; i < n; ++i) {
        coopa::util::Transform& t = rt_[i].obj->get_transform()->transform();
        glm::mat4 parent_world = parent_old[i];
        if (rt_[i].parent >= 0) {
            const size_t a = static_cast<size_t>(rt_[i].parent);
            parent_world = new_world[a] * glm::inverse(old_world[a]) * parent_old[i];
        }
        const glm::mat4 local = glm::inverse(parent_world) *
                                (glm::translate(glm::mat4(1.0f), world[i].position) * glm::mat4_cast(world[i].rotation));
        glm::vec3 pos, scale, skew;
        glm::quat rot;
        glm::vec4 persp;
        glm::decompose(local, scale, rot, pos, skew, persp);
        t.set_position(pos);
        t.set_rotation_quat(rot);
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
