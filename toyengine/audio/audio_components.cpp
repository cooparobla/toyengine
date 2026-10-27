#include <toyengine/audio/audio_components.h>

#include <coopa/scene/component.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <toyengine/core/user_settings.h>
#include <toyengine/scene/register.h>

#include <coopa/scene/scene.h>
#include <sfxcoopa/components/audio_listener.h>
#include <sfxcoopa/components/audio_source.h>

#include <cstdio>

namespace toy {
namespace audio {

std::string volume_setting_key(const std::string& bus) {
    std::string k = "audio.";
    for (char c : bus) k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return k;
}

bool set_bus_volume_persisted(const std::string& bus, float linear) {
    AudioSystem* audio = AudioSystem::active();
    if (!audio || !audio->set_bus_volume(bus, linear)) return false;
    core::UserSettings::instance().set_float(volume_setting_key(bus), linear);
    return true;
}

float bus_volume_persisted(const std::string& bus) {
    AudioSystem* audio = AudioSystem::active();
    const float live = audio ? audio->bus_volume(bus) : 1.0f;
    // A player who has never touched it gets what the mix ships with, not a zero.
    return core::UserSettings::instance().get_float(volume_setting_key(bus), live);
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
}

void register_audio_components() {
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
}

} // namespace audio
} // namespace toy
