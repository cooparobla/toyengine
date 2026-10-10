#include <toyengine/particles/particle_system_runner.h>

#include <coopa/job/parallel_for.h>

namespace toy {
namespace particles {

void ParticleSimulationSystem::set_gpu(bool enabled, std::function<bool(uint64_t, uint32_t&, uint32_t&)> alive) {
    gpu_enabled_ = enabled;
    gpu_alive_ = std::move(alive);
}

void ParticleSimulationSystem::execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) {
    jobs_ = force_serial_ ? nullptr : ctx.jobs;
    step(scene, ctx.delta_time);
}

void ParticleSimulationSystem::step(coopa::scene::Scene& scene, float dt) {
    gather_(scene);

    // Serial: init (seed, mesh source, sub-emitter targets) and world matrices.
    worlds_.resize(systems_.size());
    for (std::size_t i = 0; i < systems_.size(); ++i) {
        ParticleSystem* ps = systems_[i];
        if (!ps->initialized()) ps->init();
        resolve_sub_targets_(scene, *ps);
        auto* tc = ps->owner ? ps->owner->get_transform() : nullptr;
        worlds_[i] = tc ? tc->transform().get_world_matrix() : glm::mat4(1.0f);
    }
    assign_gpu_();

    const ParallelFor par = parallel_(256);
    small_.clear();
    for (std::size_t i = 0; i < systems_.size(); ++i) {
        if (systems_[i]->particle_count() >= k_parallel_particles) {
            systems_[i]->step(dt, worlds_[i], &par);
        } else {
            small_.push_back(i);
        }
    }
    const ParallelFor across = parallel_(2);
    run_range(&across, small_.size(), [&](std::size_t b, std::size_t e) {
        for (std::size_t k = b; k < e; ++k) systems_[small_[k]]->step(dt, worlds_[small_[k]], nullptr);
    });

    // Sub emitters, serially in scene order (deterministic): deaths spawn next frame.
    for (ParticleSystem* ps : systems_) {
        for (const auto& ev : ps->death_events()) {
            if (ev.sub >= ps->sub_targets.size()) continue;
            ParticleSystem* target = ps->sub_targets[ev.sub];
            const SubEmitter& sub = ps->settings.on_death[ev.sub];
            if (!target) continue;
            auto q = [](float v) { return static_cast<uint64_t>(static_cast<int64_t>(std::llround(v * 1000.0f))); };
            Rng rng(q(ev.pos.x) ^ (q(ev.pos.y) << 21) ^ (q(ev.pos.z) << 42), 3);
            const int count = static_cast<int>(std::round(sub.count.sample(rng)));
            for (int c = 0; c < count; ++c) target->emit_at(ev.pos, ev.vel * sub.inherit_velocity, ev.normal);
        }
    }
}

std::size_t ParticleSimulationSystem::total_particles() const {
    std::size_t n = 0;
    for (const ParticleSystem* ps : systems_) n += ps->particle_count();
    return n;
}

void ParticleSimulationSystem::assign_gpu_() {
    sub_targets_.clear();
    for (ParticleSystem* ps : systems_) {
        for (ParticleSystem* t : ps->sub_targets) if (t) sub_targets_.insert(t);
    }
    for (ParticleSystem* ps : systems_) {
        bool on = false;
        if (ps->settings.simulation == SimulationMode::Gpu) {
            std::string why = gpu_enabled_ ? ps->gpu_fallback_reason(sub_targets_.count(ps) != 0)
                                           : std::string("GPU particles are off (particles.gpu_enabled / no compute)");
            on = why.empty();
            if (!on && !ps->gpu_fallback_warned && gpu_enabled_) {
                std::cerr << "[toy::particles] '" << (ps->owner ? ps->owner->name() : std::string("?"))
                          << "': simulation: gpu falls back to the CPU (" << why << ").\n";
            }
            if (!on) ps->gpu_fallback_warned = true;
        }
        ps->set_gpu_active(on);
        if (on && gpu_alive_ && ps->gpu_id() != 0) {
            uint32_t count = 0, generation = 0;
            if (gpu_alive_(ps->gpu_id(), count, generation)) ps->set_gpu_alive(count, generation);
        }
    }
}

void ParticleSimulationSystem::gather_(coopa::scene::Scene& scene) {
    systems_.clear();
    for (ParticleSystem* ps : scene.get_components<ParticleSystem>()) {
        if (!ps->owner || !active_in_hierarchy_(*ps->owner)) continue;
        systems_.push_back(ps);
    }
}

bool ParticleSimulationSystem::active_in_hierarchy_(const coopa::scene::SceneObject& obj) {
    for (const coopa::scene::SceneObject* o = &obj; o; o = o->parent()) {
        if (!o->active()) return false;
    }
    return true;
}

void ParticleSimulationSystem::resolve_sub_targets_(coopa::scene::Scene& scene, ParticleSystem& ps) {
    if (ps.sub_targets.size() == ps.settings.on_death.size()) return;
    ps.sub_targets.assign(ps.settings.on_death.size(), nullptr);
    for (std::size_t i = 0; i < ps.settings.on_death.size(); ++i) {
        coopa::scene::SceneObject* o = scene.find_object(ps.settings.on_death[i].target);
        ps.sub_targets[i] = o ? o->get_component<ParticleSystem>() : nullptr;
    }
}

ParallelFor ParticleSimulationSystem::parallel_(std::size_t min_items) const {
    coopa::job::JobEngine* jobs = jobs_;
    if (!jobs || jobs->worker_count() < 2) return {};
    return [jobs, min_items](std::size_t n, const RangeFn& fn) {
        if (n < min_items) {
            fn(0, n);
            return;
        }
        jobs->parallel_for_blocking(n, 0, [&fn](std::size_t begin, std::size_t end) { fn(begin, end); });
    };
}

ParticleSimulationSystem* install_particle_system(coopa::scene::Scene& scene,
                                                         int order) {
    if (auto* existing = dynamic_cast<ParticleSimulationSystem*>(scene.find_system("Particles"))) return existing;
    auto sys = std::make_unique<ParticleSimulationSystem>();
    ParticleSimulationSystem* raw = sys.get();
    scene.add_system(std::move(sys), order);
    return raw;
}

} // namespace particles
} // namespace toy
