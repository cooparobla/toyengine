#include <toyengine/audio/audio_system.h>

#include <toyengine/audio/music_player.h>

#include <sfxcoopa/components/audio_listener.h>
#include <sfxcoopa/components/sfx_resources.h>
#include <sfxcoopa/core/device.h>
#include <sfxcoopa/core/engine.h>

namespace toy {
namespace audio {

AudioSystem::AudioSystem(const AudioSystemOptions& opt)
    : format_{opt.sample_rate, 2}, engine_(format_) {
    music_ = &engine_.create_bus(k_bus_music, engine_.master());
    sfx_ = &engine_.create_bus(k_bus_sfx, engine_.master());
    ui_bus_ = &engine_.create_bus(k_bus_ui, engine_.master());
    music_player_ = std::make_unique<MusicPlayer>(engine_, k_bus_music);
    music_player_->resolve = [](const std::string& p) { return coopa::sfx::SfxResources::instance().resolve(p); };
#ifdef UICOOPA_HAS_AUDIO
    ui_ = std::make_unique<coopa::ui::UiAudio>(engine_, *ui_bus_);
#endif
    if (!opt.open_device) return;
    coopa::sfx::core::DeviceConfig dc = coopa::sfx::core::DeviceConfig::from_env();
    dc.sample_rate = opt.sample_rate;
    if (opt.null_backend) dc.null_backend = true;
    auto callback = [this](float* out, uint32_t frames) {
        if (paused_.load(std::memory_order_relaxed)) {
            std::memset(out, 0, sizeof(float) * frames * format_.channels);
            return;
        }
        engine_.render_offline(out, frames);
    };
    try {
        device_ = std::make_unique<coopa::sfx::core::AudioDevice>(dc, callback);
        null_device_ = dc.null_backend;
    } catch (const std::exception& e) {
        std::cerr << "[toyengine] Audio: no output device (" << e.what() << "); playing silently\n";
        if (!dc.null_backend) {
            dc.null_backend = true;
            try {
                device_ = std::make_unique<coopa::sfx::core::AudioDevice>(dc, callback);
                null_device_ = true;
            } catch (const std::exception&) {}
        }
    }
}

AudioSystem::~AudioSystem() {
    // Stop the device thread before the engine it renders goes away.
    device_.reset();
    music_player_.reset();
}

coopa::sfx::mixer::MixerBus* AudioSystem::bus(const std::string& name) {
    if (name == k_bus_master) return &engine_.master();
    return engine_.find_bus(name);
}

bool AudioSystem::set_bus_volume(const std::string& name, float linear) {
    coopa::sfx::mixer::MixerBus* b = bus(name);
    if (!b) return false;
    b->set_volume(std::max(0.0f, linear));
    return true;
}

float AudioSystem::bus_volume(const std::string& name) {
    coopa::sfx::mixer::MixerBus* b = bus(name);
    return b ? b->volume() : 0.0f;
}

coopa::sfx::mixer::VoiceHandle AudioSystem::play_oneshot(const std::string& path, const std::string& bus_name,
                                            float gain, float pitch) {
    coopa::sfx::core::PlayParams p;
    p.bus_name = bus_name;
    p.gain = gain;
    p.pitch = pitch;
    try {
        return track_(engine_.play(coopa::sfx::SfxResources::instance().resolve(path), p));
    } catch (const std::exception& e) {
        warn_(path, e.what());
        return {};
    }
}

coopa::sfx::mixer::VoiceHandle AudioSystem::play_oneshot_3d(const std::string& path, const glm::vec3& position,
                                               const std::string& bus_name, float gain) {
    coopa::sfx::core::PlayParams p;
    p.bus_name = bus_name;
    p.gain = gain;
    const std::string resolved = coopa::sfx::SfxResources::instance().resolve(path);
    try {
        return track_(engine_.play_3d(resolved, position, p));
    } catch (const std::exception&) {
        return play_oneshot(path, bus_name, gain);
    }
}

void AudioSystem::stop_all() {
    stop_oneshots();
    music_player_->stop_now();
}

void AudioSystem::stop_oneshots() {
    for (const auto& h : oneshots_) engine_.stop(h);
    oneshots_.clear();
}

void AudioSystem::update(float dt, const glm::mat4* camera_world) {
    auto& res = coopa::sfx::SfxResources::instance();
    if (!res.consume_listener_pushed() && camera_world) {
        coopa::sfx::components::apply_listener_matrix(engine_, *camera_world);
        listener_position_ = glm::vec3((*camera_world)[3]);
    }
    // A paused game freezes the music's clock with its output.
    music_player_->update(paused_ ? 0.0f : dt);
    engine_.update(dt);
    oneshots_.erase(std::remove_if(oneshots_.begin(), oneshots_.end(),
                                   [&](const coopa::sfx::mixer::VoiceHandle& h) { return !engine_.is_voice_active(h); }),
                    oneshots_.end());
}

AudioSystem*& AudioSystem::active_ptr_() {
    static AudioSystem* a = nullptr;
    return a;
}

coopa::sfx::mixer::VoiceHandle AudioSystem::track_(coopa::sfx::mixer::VoiceHandle h) {
    oneshots_.push_back(h);
    return h;
}

void AudioSystem::warn_(const std::string& path, const std::string& why) {
    if (std::find(warned_.begin(), warned_.end(), path) != warned_.end()) return;
    warned_.push_back(path);
    std::cerr << "[toyengine] Audio: cannot play '" << path << "': " << why << "\n";
}

} // namespace audio
} // namespace toy
