#include <toyengine/audio/audio_components.h>

#include <coopa/scene/component.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <toyengine/core/user_settings.h>
#include <toyengine/scene/register.h>

#include <coopa/scene/scene.h>
#include <sfxcoopa/components/audio_listener.h>
#include <sfxcoopa/components/audio_source.h>
#include <uicoopa/widgets/text.h>

#include <cstdio>

namespace toy {
namespace audio {

std::string volume_setting_key(const std::string& bus) {
    std::string k = "audio.";
    for (char c : bus) k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return k;
}

void VolumeBinding::update(float) {
    AudioSystem* audio = AudioSystem::active();
    if (!audio || !owner) return;
    if (!slider_) {
        // A SettingRow builds its Slider when it starts, so look until one exists.
        auto sliders = owner->get_components_in_children<coopa::ui::Slider>();
        if (sliders.empty()) return;
        slider_ = sliders.front();
        last_ = audio->bus_volume(bus);
        slider_->set_value(last_, false);
        return;
    }
    const float v = slider_->value();
    if (std::fabs(v - last_) < 1e-4f) return;
    last_ = v;
    audio->set_bus_volume(bus, v);
    core::UserSettings::instance().set_float(volume_setting_key(bus), v);
}

// --- MusicPlaylist ---------------------------------------------------------------------------

MusicPlaylistComponent::~MusicPlaylistComponent() {
    if (!started_ || !stop_on_destroy) return;
    if (AudioSystem* audio = AudioSystem::active()) audio->music().stop(crossfade);
}

void MusicPlaylistComponent::update(float) {
    // The first simulated frame, not start(): an editor loading the scene to edit it stays silent.
    if (!registered_) register_tracks();
    if (!started_ && play_on_start) play();
}

void MusicPlaylistComponent::register_tracks() {
    AudioSystem* audio = AudioSystem::active();
    if (!audio) return;
    registered_ = true;
    for (const MusicTrack& t : tracks) {
        audio->music().add_track(t);
        audio->music().preload(t.name.empty() ? t.clip : t.name);   // decode now, not mid-crossfade
    }
}

void MusicPlaylistComponent::play() {
    AudioSystem* audio = AudioSystem::active();
    if (!audio) return;
    if (!registered_) register_tracks();
    started_ = true;
    MusicPlaylist list;
    for (const MusicTrack& t : tracks) list.tracks.push_back(t.name.empty() ? t.clip : t.name);
    list.shuffle = shuffle;
    list.repeat = repeat;
    list.crossfade = crossfade;
    audio->music().set_playlist(list, start);
}

// --- MusicZone -------------------------------------------------------------------------------

namespace {
glm::vec3 listener_position_in(coopa::scene::Scene* scene, const AudioSystem& audio) {
    if (scene) {
        if (auto* l = scene->find_first_component<coopa::sfx::components::AudioListener>()) {
            if (l->owner && l->owner->get_transform()) return glm::vec3(l->owner->get_transform()->get_world_matrix()[3]);
        }
    }
    return audio.listener_position();
}
}

MusicZone::~MusicZone() {
    if (!inside_) return;
    if (AudioSystem* audio = AudioSystem::active()) audio->music().pop(fade);
}

bool MusicZone::contains(const glm::vec3& p) const {
    glm::vec3 centre(0.0f), scale(1.0f);
    if (owner && owner->get_transform()) {
        const glm::mat4 m = owner->get_transform()->get_world_matrix();
        centre = glm::vec3(m[3]);
        scale = glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
    }
    if (radius > 0.0f) return glm::length(p - centre) <= radius * std::max({scale.x, scale.y, scale.z});
    const glm::vec3 half = 0.5f * size * scale;
    const glm::vec3 d = glm::abs(p - centre);
    return d.x <= half.x && d.y <= half.y && d.z <= half.z;
}

void MusicZone::update(float) {
    AudioSystem* audio = AudioSystem::active();
    if (!audio || track.empty()) return;
    const bool in = contains(listener_position_in(scene, *audio));
    if (in == inside_) return;
    inside_ = in;
    if (in) {
        if (!audio->music().push(track, fade)) inside_ = false;
    } else {
        audio->music().pop(fade);
    }
}

// --- MusicOnSignal / AudioSourceOnSignal -----------------------------------------------------

void MusicOnSignal::on_signal(const coopa::event::EventArgs&) {
    AudioSystem* audio = AudioSystem::active();
    if (!audio) return;
    MusicPlayer& m = audio->music();
    if (action == "play") m.play(track, fade);
    else if (action == "playlist") {
        coopa::scene::SceneObject* obj = resolve_target();
        if (auto* list = obj ? obj->get_component<MusicPlaylistComponent>() : nullptr) list->play();
    } else if (action == "push") m.push(track, fade);
    else if (action == "pop") m.pop(fade);
    else if (action == "next") m.next(fade);
    else if (action == "previous") m.previous(fade);
    else if (action == "stop") m.stop(fade);
    else if (action == "pause") m.pause(fade < 0.0f ? 0.5f : fade);
    else if (action == "resume") m.resume(fade < 0.0f ? 0.5f : fade);
    else if (action == "toggle_pause") {
        if (m.paused()) m.resume(fade < 0.0f ? 0.5f : fade);
        else m.pause(fade < 0.0f ? 0.5f : fade);
    }
}

void AudioSourceOnSignal::on_signal(const coopa::event::EventArgs& args) {
    coopa::scene::SceneObject* obj = resolve_target();
    auto* src = obj ? obj->get_component<coopa::sfx::components::AudioSource>() : nullptr;
    if (!src) return;
    if (action == "play") src->play();
    else if (action == "stop") src->stop();
    else if (action == "pause") src->pause();
    else if (action == "resume") src->resume();
    else if (action == "toggle") {
        if (src->is_playing()) src->stop();
        else src->play();
    } else if (action == "follow") {
        const bool on = args.get<bool>("value", !src->is_playing());
        if (on && !src->is_playing()) src->play();
        else if (!on) src->stop();
    }
}

// --- MusicStatusText -------------------------------------------------------------------------

namespace {
std::string clock(float seconds) {
    const int s = static_cast<int>(std::max(seconds, 0.0f));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
    return buf;
}
}

std::string MusicStatusText::describe(const MusicPlayer& music) {
    const std::string track = music.current();
    if (track.empty()) return music.transitioning() ? "(fading out)" : "(silence)";
    std::string s = track + "  " + clock(music.position()) + " / " + clock(music.duration());
    if (music.paused()) s += "  [paused]";
    else if (music.transitioning()) s += "  [crossfading]";
    if (music.stack_depth() > 0) s += "  [zone]";
    return s;
}

void MusicStatusText::update(float) {
    AudioSystem* audio = AudioSystem::active();
    auto* txt = owner ? owner->get_component<coopa::ui::Text>() : nullptr;
    if (!audio || !txt) return;
    const std::string s = prefix + describe(audio->music());
    if (txt->text != s) txt->text = s;
}

namespace {
std::string str_or(const fkyaml::node& n, const char* key, const std::string& fallback) {
    return n.contains(key) && n.at(key).is_string() ? n.at(key).get_value<std::string>() : fallback;
}
float float_or(const fkyaml::node& n, const char* key, float fallback) {
    if (!n.contains(key)) return fallback;
    const fkyaml::node& v = n.at(key);
    if (v.is_float_number()) return v.get_value<float>();
    if (v.is_integer()) return static_cast<float>(v.get_value<int64_t>());
    return fallback;
}
bool bool_or(const fkyaml::node& n, const char* key, bool fallback) {
    return n.contains(key) && n.at(key).is_boolean() ? n.at(key).get_value<bool>() : fallback;
}
void parse_reactor(const fkyaml::node& n, coopa::ui::SignalReactor& r) {
    r.listen_object = str_or(n, "listen_object", r.listen_object);
    r.listen_signal = str_or(n, "listen_signal", r.listen_signal);
    r.once = bool_or(n, "once", r.once);
    r.target = str_or(n, "target", r.target);
}
}

void register_audio_components() {
    coopa::scene::SceneLoader::register_component_parser(
        "VolumeBinding", [](const fkyaml::node& node, coopa::scene::SceneObject& obj, const coopa::scene::SceneLoader::ParseContext&) {
            auto* vb = obj.add_component<VolumeBinding>();
            if (node.contains("bus") && node.at("bus").is_string()) vb->bus = node.at("bus").get_value<std::string>();
        });
    coopa::scene::SceneLoader::register_component_parser(
        "MusicPlaylist", [](const fkyaml::node& node, coopa::scene::SceneObject& obj, const coopa::scene::SceneLoader::ParseContext&) {
            auto* mp = obj.add_component<MusicPlaylistComponent>();
            if (node.contains("tracks") && node.at("tracks").is_sequence()) {
                for (const auto& t : node.at("tracks")) {
                    MusicTrack track;
                    if (t.is_string()) {
                        track.clip = t.get_value<std::string>();
                    } else if (t.is_mapping()) {
                        track.clip = str_or(t, "clip", "");
                        track.name = str_or(t, "name", "");
                        track.volume = float_or(t, "volume", track.volume);
                        track.loop = bool_or(t, "loop", track.loop);
                    }
                    if (track.name.empty()) track.name = track.clip;
                    if (!track.clip.empty()) mp->tracks.push_back(track);
                }
            }
            mp->shuffle = bool_or(node, "shuffle", mp->shuffle);
            mp->repeat = bool_or(node, "repeat", mp->repeat);
            mp->crossfade = float_or(node, "crossfade", mp->crossfade);
            mp->start = static_cast<int>(float_or(node, "start", static_cast<float>(mp->start)));
            mp->play_on_start = bool_or(node, "play_on_start", mp->play_on_start);
            mp->stop_on_destroy = bool_or(node, "stop_on_destroy", mp->stop_on_destroy);
        });
    coopa::scene::SceneLoader::register_component_parser(
        "MusicZone", [](const fkyaml::node& node, coopa::scene::SceneObject& obj, const coopa::scene::SceneLoader::ParseContext&) {
            auto* z = obj.add_component<MusicZone>();
            z->track = str_or(node, "track", z->track);
            if (node.contains("size")) z->size = scene::parse_vec3(node.at("size"), z->size);
            z->radius = float_or(node, "radius", z->radius);
            z->fade = float_or(node, "fade", z->fade);
        });
    coopa::scene::SceneLoader::register_component_parser(
        "MusicOnSignal", [](const fkyaml::node& node, coopa::scene::SceneObject& obj, const coopa::scene::SceneLoader::ParseContext&) {
            auto* r = obj.add_component<MusicOnSignal>();
            parse_reactor(node, *r);
            r->action = str_or(node, "action", r->action);
            r->track = str_or(node, "track", r->track);
            r->fade = float_or(node, "fade", r->fade);
        });
    coopa::scene::SceneLoader::register_component_parser(
        "AudioSourceOnSignal", [](const fkyaml::node& node, coopa::scene::SceneObject& obj, const coopa::scene::SceneLoader::ParseContext&) {
            auto* r = obj.add_component<AudioSourceOnSignal>();
            parse_reactor(node, *r);
            r->action = str_or(node, "action", r->action);
        });
    coopa::scene::SceneLoader::register_component_parser(
        "MusicStatusText", [](const fkyaml::node& node, coopa::scene::SceneObject& obj, const coopa::scene::SceneLoader::ParseContext&) {
            auto* m = obj.add_component<MusicStatusText>();
            m->prefix = str_or(node, "prefix", m->prefix);
        });
}

} // namespace audio
} // namespace toy
