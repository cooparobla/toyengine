#include <toyengine/weather/wind_system.h>

#include <toyengine/env/env_system.h>

#include <algorithm>
#include <memory>

#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <physxcoopa/components/cloth.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/system/physics_system.h>
#include <physxcoopa/world.h>
#include <toyengine/weather/weather_system.h>
#include <toyengine/weather/wind.h>
#include <toyengine/hair/hair.h>

namespace toy {
namespace weather {

WindSystem::~WindSystem() { connection_.disconnect(); }

void WindSystem::on_detach(coopa::scene::Scene&) {
    connection_.disconnect();
    physics_ = nullptr;
    bodies_.clear();
}

void WindSystem::connect_(coopa::scene::Scene& scene) {
    auto* sys = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"));
    if (sys == physics_) return;
    connection_.disconnect();
    physics_ = sys;
    if (physics_) {
        connection_ = physics_->world().on_substep.connect(
            [this](coopa::physx::PhysicsWorld& world, float h) { substep_(world, h); });
    }
}

void WindSystem::execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext&) {
    using coopa::physx::components::ClothComponent;
    using coopa::physx::components::RigidbodyComponent;
    connect_(scene);
    bodies_.clear();
    // The weather's wind AND every local influence: a rotor near a flag must move it in a
    // scene with no weather at all.
    const env::EnvSystem* es = env::find_env(scene);
    env_ = es ? es->field() : env::weather_only_field(scene);
    const bool live = env::is_windy(env_);

    // Cloth: authored wind + the weather's at the cloth (the authored wind alone without one).
    for (ClothComponent* cc : scene.get_components<ClothComponent>()) {
        coopa::physx::cloth::Cloth* cloth = cc->cloth_mut();
        if (!cloth) continue;
        glm::vec3 wind = cc->params.wind;
        if (live && cc->weather_wind != 0.0f && cc->owner) {
            const glm::vec3 at = cc->owner->get_transform() ? glm::vec3(cc->owner->get_transform()->transform().get_world_matrix()[3])
                                                            : glm::vec3(0.0f);
            wind += env::wind_at(env_, at) * cc->weather_wind;
        }
        cloth->params.wind = wind;
    }

    // Hair: the same field, written onto the component (pushed in, never pulled) for HairSystem
    // to hand the solver. A groom that is not simulated ignores it -- its motion is in the
    // shader, which reads the world wind itself.
    for (hair::Hair* h : scene.get_components<hair::Hair>()) {
        glm::vec3 wind(0.0f);
        if (live && h->weather_wind != 0.0f && h->owner != nullptr) {
            const auto* tc = h->owner->get_transform();
            const glm::vec3 at = tc ? glm::vec3(tc->transform().get_world_matrix()[3]) : glm::vec3(0.0f);
            wind = env::wind_at(env_, at) * h->weather_wind;
        }
        h->wind = wind;
    }

    if (!live) return;
    for (RigidbodyComponent* rb : scene.get_components<RigidbodyComponent>()) {
        if (rb->wind_influence <= 0.0f || rb->is_kinematic) continue;
        bodies_.push_back({rb->body_id(), rb->wind_influence});
    }
}

void WindSystem::substep_(coopa::physx::PhysicsWorld& world, float) {
    using coopa::physx::dynamics::BodyType;
    for (const Entry& e : bodies_) {
        coopa::physx::dynamics::Body* b = world.get_body(e.id);
        if (!b || b->type != BodyType::Dynamic) continue;
        const glm::vec3 w = env::wind_at(env_, b->position);
        const glm::vec3 f = wind_drag_force(e.influence, w, b->linear_velocity);
        if (!b->awake) {
            // add_force() wakes it: only for a push that could plausibly move it (a tenth of
            // its weight), so things resting in a breeze stay asleep.
            const float weight = b->mass * 9.81f;
            if (glm::length(f) < 0.1f * weight) continue;
        }
        b->add_force(f);
    }
}

WindSystem* install_wind_system(coopa::scene::Scene& scene, int order) {
    if (auto* existing = dynamic_cast<WindSystem*>(scene.find_system("Wind"))) return existing;
    auto sys = std::make_unique<WindSystem>();
    WindSystem* raw = sys.get();
    scene.add_system(std::move(sys), order);
    return raw;
}

} // namespace weather
} // namespace toy
