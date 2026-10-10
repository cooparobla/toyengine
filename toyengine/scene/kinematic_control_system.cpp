#include <toyengine/scene/kinematic_control_system.h>

#include <coopa/scene/scene_system.h>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/scene/character_controller.h>
#include <toyengine/scene/kinematic_controller.h>
#include <toyengine/scene/kinematic_mover.h>

namespace toy {
namespace scene {

void KinematicControlSystem::execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) {
    // Deliberately serial. These components write Transforms, and a Transform write dirties its
    // whole child subtree -- two controllers on objects in the same subtree would race on that
    // flag. There are never many of them, and the work is a handful of flops each.
    for (KinematicController* kc : scene.get_components<KinematicController>()) {
        kc->advance(ctx.delta_time);
    }
    for (KinematicMover* km : scene.get_components<KinematicMover>()) {
        km->advance(ctx.delta_time);
    }
    // Characters last: the platforms above have already moved this frame. They query the
    // physics world, which still holds every body's pose from the previous step.
    std::vector<CharacterController*> characters = scene.get_components<CharacterController>();
    if (!characters.empty()) {
        auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"));
        for (CharacterController* cc : characters) cc->advance(ctx.delta_time, physics);
    }
}

KinematicControlSystem* install_kinematic_control_system(
    coopa::scene::Scene& scene, int order) {
    auto sys = std::make_unique<KinematicControlSystem>();
    KinematicControlSystem* raw = sys.get();
    scene.add_system(std::move(sys), order);
    return raw;
}

} // namespace scene
} // namespace toy
