#include <toyengine/scene/view_blocker_system.h>

#include <algorithm>
#include <vector>

#include <coopa/scene/scene_object.h>
#include <coopa/scene/scene_system.h>
#include <gfxcoopa/engine/components/camera_component.h>
#include <physxcoopa/components/collider.h>
#include <physxcoopa/geometry/capsule.h>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/scene/view_blocker.h>
#include <toyengine/scene/xray_markers.h>

namespace toy {
namespace scene {
namespace {

/// World position of an object's origin, or nullopt-ish via the bool return.
bool world_origin(const coopa::scene::SceneObject* obj, glm::vec3& out) {
    if (!obj) return false;
    auto* t = const_cast<coopa::scene::SceneObject*>(obj)->get_transform();
    if (!t) return false;
    out = glm::vec3(t->transform().get_world_matrix()[3]);
    return true;
}

} // namespace

void ViewBlockerSystem::execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) {
    std::vector<ViewBlockerFade*> walls = scene.get_components<ViewBlockerFade>();
    if (walls.empty()) return;

    // Clear first, then mark: a wall that stopped blocking has to be told, and the query below
    // can early-out at several points without leaving last frame's marks behind.
    for (ViewBlockerFade* w : walls) w->set_blocking(false);

    std::vector<XraySubject*> subjects = scene.get_components<XraySubject>();
    auto* camera  = coopa::gfx::engine::components::CameraComponent::main();
    auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"));

    glm::vec3 eye(0.0f);
    if (!subjects.empty() && physics && camera && world_origin(camera->owner, eye)) {
        for (XraySubject* s : subjects) {
            glm::vec3 target(0.0f);
            if (!s->enabled || !world_origin(s->owner, target)) continue;

            const glm::vec3 to_target = target - eye;
            const float     span      = glm::length(to_target);
            // Trim the subject end, or the capsule catches the floor under the subject and the
            // whole level fades the moment the feature is switched on.
            if (span <= subject_margin + 1e-3f) continue;
            const glm::vec3 dir = to_target / span;

            coopa::physx::geometry::Capsule probe;
            probe.a      = eye;
            probe.b      = eye + dir * (span - std::max(subject_margin, 0.0f));
            probe.radius = std::max(probe_radius, 0.0f);

            coopa::physx::system::PhysicsSystem::QueryFilter filter;
            filter.include_triggers = false;
            // Exclude the subject's whole subtree: a rig's capsule, its weapon and its mount
            // are all "the thing being looked at", never something in the way of it.
            const coopa::scene::SceneObject* subject_root = s->owner;
            filter.predicate = [subject_root](const coopa::physx::components::Collider& c) {
                for (const coopa::scene::SceneObject* o = c.owner; o; o = o->parent()) {
                    if (o == subject_root) return false;
                }
                return true;
            };

            for (coopa::physx::components::Collider* c : physics->overlap_capsule(probe, filter)) {
                if (!c || !c->owner) continue;
                // The marker may sit on a parent (a wall kit piece whose collider is a child),
                // so walk up for it rather than only checking the collider's own object.
                for (coopa::scene::SceneObject* o = c->owner; o; o = o->parent()) {
                    if (auto* f = o->get_component<ViewBlockerFade>()) { f->set_blocking(true); break; }
                }
            }
        }
    }

    for (ViewBlockerFade* w : walls) w->advance(ctx.delta_time);
}

ViewBlockerSystem* install_view_blocker_system(coopa::scene::Scene& scene, int order) {
    auto sys = std::make_unique<ViewBlockerSystem>();
    ViewBlockerSystem* raw = sys.get();
    scene.add_system(std::move(sys), order);
    return raw;
}

} // namespace scene
} // namespace toy
