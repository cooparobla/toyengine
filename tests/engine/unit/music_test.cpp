/**
 * @file music_test.cpp
 * @brief MusicPlayer and the music components, rendered offline (no sound card): equal-power
 *        crossfades, playlists handing off on their own near a track's end, push/pop resuming
 *        where the music paused, pause/stop fades, a paused game freezing the music clock, the
 *        scene components (MusicPlaylist carrying a track across scenes, MusicZone, MusicOnSignal,
 *        AudioSourceOnSignal), and the audio_demo's clips and scene.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>
#include <sfxcoopa/sfx_yaml.h>
#include <toyengine/audio/audio_components.h>
#include <toyengine/audio/audio_system.h>
#include <toyengine/audio/music_player.h>
#include <uicoopa/ui_yaml.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("music");

namespace music_test {

constexpr uint32_t k_rate = 48000;

/** @brief Writes a mono 16-bit sine WAV (`seconds` long) and returns its path. */
std::string sine_wav(const std::string& name, float hz, float seconds) {
    const std::filesystem::path p = coopa::test::scratch_dir("music") / (name + ".wav");
    const uint32_t frames = static_cast<uint32_t>(seconds * k_rate);
    std::vector<int16_t> pcm(frames);
    for (uint32_t i = 0; i < frames; ++i) pcm[i] = static_cast<int16_t>(12000.0 * std::sin(2.0 * glm::pi<double>() * hz * i / k_rate));
    std::ofstream f(p, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = frames * 2;
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(1); u32(k_rate); u32(k_rate * 2); u16(2); u16(16);
    f.write("data", 4); u32(bytes);
    f.write(reinterpret_cast<const char*>(pcm.data()), bytes);
    return p.string();
}

/** @brief Magnitude of `hz` in the left channel of an interleaved stereo block (Goertzel). */
float tone(const std::vector<float>& buf, float hz) {
    const size_t frames = buf.size() / 2;
    const double w = 2.0 * glm::pi<double>() * hz / k_rate;
    const double c = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < frames; ++i) {
        const double s0 = buf[i * 2] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return static_cast<float>(std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / frames * 2.0);
}

/** @brief An AudioSystem wired as Engine wires it, minus the device. */
struct Rig {
    toy::audio::AudioSystem audio{toy::audio::AudioSystemOptions{k_rate, false, false}};
    glm::mat4 camera{1.0f};
    Rig() {
        coopa::sfx::SfxResources::instance().set_engine(&audio.engine());
        coopa::sfx::register_sfx_components();
        toy::audio::register_audio_components();
        coopa::ui::register_ui_components();
        toy::audio::AudioSystem::set_active(&audio);
    }
    ~Rig() {
        toy::audio::AudioSystem::set_active(nullptr);
        coopa::sfx::SfxResources::instance().set_engine(nullptr);
    }
    toy::audio::MusicPlayer& music() { return audio.music(); }

    /** @brief Advances one frame of `dt`: scene, audio update, then renders it. Returns the block. */
    std::vector<float> step(float dt = 1.0f / 60.0f, coopa::scene::Scene* scene = nullptr) {
        if (scene) scene->update(dt);
        audio.update(dt, &camera);
        const uint32_t frames = static_cast<uint32_t>(dt * k_rate);
        std::vector<float> buf(frames * 2);
        audio.engine().render_offline(buf.data(), frames);
        return buf;
    }
    /** @brief Runs `seconds` of frames; returns the last block. */
    std::vector<float> run(float seconds, coopa::scene::Scene* scene = nullptr) {
        std::vector<float> last;
        for (float t = 0.0f; t < seconds - 1e-4f; t += 1.0f / 60.0f) last = step(1.0f / 60.0f, scene);
        return last;
    }
};

coopa::scene::Scene load(const std::string& yaml) {
    return coopa::scene::SceneLoader::load_from_node(fkyaml::node::deserialize(yaml),
                                                     (coopa::test::scratch_dir("music") / "scene.yaml").string());
}

} // namespace music_test

COOPA_TEST(crossfade_is_equal_power_and_retires_the_old_track) {
    using namespace music_test;
    Rig rig;
    const std::string a = sine_wav("a440", 420.0f, 4.0f), b = sine_wav("b660", 660.0f, 4.0f);
    rig.music().add_track({"a", a, 1.0f, true});
    rig.music().add_track({"b", b, 1.0f, true});
    expect(rig.music().play("a", 0.0f), "play starts a registered track");
    const float steady = tone(rig.run(0.2f), 420.0f);
    expect(steady > 0.1f, "track a is heard (" + std::to_string(steady) + ")");
    expect(rig.music().play("a"), "playing the current track again is a no-op that succeeds");
    expect(rig.music().deck_count() == size_t(1), "and starts no second deck");

    rig.music().play("b", 1.0f);
    expect(rig.music().current() == "b" && rig.music().transitioning(), "b is current at once and the fade runs");
    const std::vector<float> mid = rig.run(0.5f);
    const float ma = tone(mid, 420.0f), mb = tone(mid, 660.0f);
    expect(ma > 0.3f * steady && mb > 0.3f * steady, "halfway, both tracks are heard");
    expect_near(ma * ma + mb * mb, steady * steady, 0.25f * steady * steady, "equal-power: total power holds through the crossfade");
    const std::vector<float> after = rig.run(0.7f);
    expect(tone(after, 420.0f) < 0.01f * steady && tone(after, 660.0f) > 0.9f * steady, "after the fade only b plays");
    expect(rig.music().deck_count() == size_t(1), "the faded-out deck is retired");
    expect(!rig.music().transitioning(), "and the transition is over");
}

COOPA_TEST(playlist_hands_off_near_each_tracks_end) {
    using namespace music_test;
    Rig rig;
    const std::string a = sine_wav("pa", 420.0f, 1.0f), b = sine_wav("pb", 660.0f, 1.0f);
    std::vector<std::string> started;
    rig.music().on_track_started.connect([&](std::string t) { started.push_back(t); });
    toy::audio::MusicPlaylist list;
    list.tracks = {a, b};
    list.crossfade = 0.3f;
    rig.music().set_playlist(list);
    expect(rig.music().current() == a, "the playlist starts on its first track");
    rig.run(0.6f);
    expect(rig.music().current() == a && !rig.music().transitioning(), "mid-track nothing happens");
    rig.run(0.2f);   // 0.8 s: inside the last 0.3 s
    expect(rig.music().current() == b, "near its end the next track starts on its own");
    expect(rig.music().transitioning() && rig.music().deck_count() == 2, "and crossfades with the outgoing one");
    const std::vector<float> after = rig.run(0.4f);
    expect(rig.music().deck_count() == size_t(1), "the outgoing track is gone by the end of the crossfade");
    expect(tone(after, 660.0f) > 0.1f && tone(after, 420.0f) < 0.01f, "b alone plays");
    rig.run(0.6f);
    expect(rig.music().current() == a, "repeat wraps back to the first track");
    expect(started.size() == 3 && started[0] == a && started[1] == b && started[2] == a, "on_track_started reports every start");

    expect(rig.music().next(0.0f) && rig.music().current() == b, "next() skips forward");
    expect(rig.music().previous(0.0f) && rig.music().current() == a, "previous() skips back");
}

COOPA_TEST(non_repeating_playlist_ends) {
    using namespace music_test;
    Rig rig;
    toy::audio::MusicPlaylist list;
    list.tracks = {sine_wav("ea", 420.0f, 0.6f), sine_wav("eb", 660.0f, 0.6f)};
    list.crossfade = 0.2f;
    list.repeat = false;
    rig.music().set_playlist(list);
    rig.run(2.0f);
    expect(rig.music().current().empty() && rig.music().deck_count() == 0, "after the last track the music is silent");
}

COOPA_TEST(push_pauses_and_pop_resumes_where_it_stopped) {
    using namespace music_test;
    Rig rig;
    rig.music().add_track({"base", sine_wav("base", 420.0f, 6.0f), 1.0f, true});
    rig.music().add_track({"zone", sine_wav("zone", 660.0f, 6.0f), 1.0f, true});
    rig.music().play("base", 0.0f);
    rig.run(1.0f);
    rig.music().push("zone", 0.25f);
    expect(rig.music().current() == "zone" && rig.music().stack_depth() == 1, "push makes the new track current");
    const std::vector<float> pushed = rig.run(1.0f);
    expect(tone(pushed, 420.0f) < 0.01f && tone(pushed, 660.0f) > 0.1f, "the music under the push is silent");
    expect(rig.music().deck_count() == size_t(2), "but its deck is kept (paused)");
    rig.music().pop(0.25f);
    expect(rig.music().current() == "base" && rig.music().stack_depth() == 0, "pop returns to the track underneath");
    expect_near(rig.music().position(), 1.25f, 0.1f, "which continues from where it paused, not from the start");
    const std::vector<float> popped = rig.run(0.5f);
    expect(tone(popped, 420.0f) > 0.1f && tone(popped, 660.0f) < 0.01f, "base plays again, the pushed track is gone");
    expect(!rig.music().pop(), "popping an empty stack does nothing");
}

COOPA_TEST(pause_resume_and_stop_fade) {
    using namespace music_test;
    Rig rig;
    rig.music().play(sine_wav("pr", 420.0f, 4.0f), 0.0f);
    rig.run(0.5f);
    rig.music().pause(0.2f);
    expect(tone(rig.run(0.4f), 420.0f) < 0.01f && rig.music().paused(), "pause fades the music out");
    const float at = rig.music().position();
    rig.run(0.5f);
    expect_near(rig.music().position(), at, 1e-4f, "the paused track's clock stands still");
    rig.music().resume(0.2f);
    expect(tone(rig.run(0.4f), 420.0f) > 0.1f, "resume brings it back");
    rig.music().stop(0.3f);
    expect(rig.music().current().empty(), "stop clears the current track at once");
    rig.run(0.5f);
    expect(rig.music().deck_count() == size_t(0), "and its fade-out finishes");
    expect(!rig.music().play("does/not/exist.wav"), "a missing clip fails without throwing");
}

COOPA_TEST(paused_game_freezes_the_music_clock_and_stop_all_ends_it) {
    using namespace music_test;
    Rig rig;
    rig.music().play(sine_wav("pg", 420.0f, 4.0f), 0.0f);
    rig.run(0.5f);
    const float at = rig.music().position();
    rig.audio.pause_all();
    rig.run(0.5f);
    expect_near(rig.music().position(), at, 1e-4f, "AudioSystem::pause_all freezes the music clock with the output");
    rig.audio.resume_all();
    rig.audio.stop_all();
    expect(rig.music().deck_count() == 0 && rig.music().current().empty(), "stop_all stops the music too");
    rig.music().play(sine_wav("pg", 420.0f, 4.0f), 0.0f);
    rig.audio.stop_oneshots();
    expect(!rig.music().current().empty(), "stop_oneshots (a scene change) leaves the music playing");
}

COOPA_TEST(music_playlist_component_waits_for_simulation_and_carries_over) {
    using namespace music_test;
    Rig rig;
    const std::string a = sine_wav("ca", 420.0f, 8.0f), b = sine_wav("cb", 660.0f, 8.0f);
    auto yaml = [&](const std::string& tracks) {
        return "scene:\n  root_objects:\n    - name: music\n      components:\n        - type: MusicPlaylist\n"
               "          crossfade: 1.0\n          tracks:\n" + tracks;
    };
    const std::string tracks_ab = "            - { name: alpha, clip: \"" + a + "\", volume: 0.5 }\n            - \"" + b + "\"\n";
    {
        coopa::scene::Scene scene = load(yaml(tracks_ab));
        expect(rig.music().current().empty(), "loading the scene (edit mode) plays nothing");
        rig.run(1.0f, &scene);
        expect(rig.music().current() == "alpha" && rig.music().has_playlist(), "the first simulated frame starts the playlist");
        expect(rig.music().find_track("alpha") && rig.music().find_track("alpha")->volume == 0.5f, "named tracks keep their volume");
        rig.audio.stop_oneshots();   // what Engine does between scenes
    }
    // The next scene lists the same track: it carries on instead of restarting.
    const float before = rig.music().position();
    coopa::scene::Scene next = load(yaml("            - \"" + b + "\"\n            - { name: alpha, clip: \"" + a + "\" }\n"));
    rig.run(0.5f, &next);
    expect(rig.music().current() == "alpha" && rig.music().deck_count() == 1, "the playing track is kept across the scene change");
    expect(rig.music().position() > before, "and keeps its place");
}

COOPA_TEST(music_zone_pushes_while_the_listener_is_inside) {
    using namespace music_test;
    Rig rig;
    rig.music().add_track({"base", sine_wav("zb", 420.0f, 8.0f), 1.0f, true});
    rig.music().play("base", 0.0f);
    coopa::scene::Scene scene = load("scene:\n  root_objects:\n    - name: zone\n      components:\n"
                                     "        - type: Transform\n          position: { x: 10.0, y: 0.0, z: 0.0 }\n"
                                     "          rotation: { x: 0.0, y: 0.0, z: 0.0 }\n          scale: { x: 1.0, y: 1.0, z: 1.0 }\n"
                                     "        - type: MusicZone\n          track: \"" + sine_wav("zz", 660.0f, 8.0f) + "\"\n"
                                     "          size: { x: 4.0, y: 4.0, z: 4.0 }\n          fade: 0.2\n");
    rig.run(0.3f, &scene);
    expect(rig.music().current() == "base", "outside the zone the scene music plays");
    rig.camera = glm::translate(glm::mat4(1.0f), glm::vec3(9.0f, 1.0f, 0.5f));
    rig.run(0.5f, &scene);
    expect(rig.music().stack_depth() == 1 && rig.music().current() != "base", "walking in pushes the zone's track");
    rig.camera = glm::translate(glm::mat4(1.0f), glm::vec3(13.0f, 0.0f, 0.0f));
    rig.run(0.5f, &scene);
    expect(rig.music().current() == "base" && rig.music().stack_depth() == 0, "walking out pops back to the scene music");
}

COOPA_TEST(signal_reactors_drive_music_and_sources) {
    using namespace music_test;
    Rig rig;
    const std::string a = sine_wav("ra", 420.0f, 8.0f), b = sine_wav("rb", 660.0f, 8.0f);
    coopa::scene::Scene scene = load(
        "scene:\n  root_objects:\n    - name: music\n      components:\n        - type: MusicPlaylist\n"
        "          tracks: [\"" + a + "\", \"" + b + "\"]\n"
        "    - name: speaker\n      components:\n        - type: AudioSource\n          clip: \"" + a + "\"\n          loop: true\n"
        "    - name: panel\n      components:\n"
        "        - {type: MusicOnSignal, listen_object: NextButton, listen_signal: click, action: next, fade: 0.0}\n"
        "        - {type: MusicOnSignal, listen_object: StopButton, listen_signal: click, action: stop}\n"
        "        - {type: MusicOnSignal, listen_object: AgainButton, listen_signal: click, action: playlist, target: music}\n"
        "        - {type: AudioSourceOnSignal, listen_object: SpeakerToggle, listen_signal: value_changed, action: follow, target: speaker}\n");
    rig.run(0.1f, &scene);
    expect(rig.music().current() == a, "the playlist runs");
    scene.events().emit("NextButton", "click", {});
    expect(rig.music().current() == b, "MusicOnSignal next");
    scene.events().emit("StopButton", "click", {});
    expect(rig.music().current().empty(), "MusicOnSignal stop");
    scene.events().emit("AgainButton", "click", {});
    expect(rig.music().current() == a, "MusicOnSignal playlist restarts the target's MusicPlaylist");

    auto* src = scene.find_object("speaker")->get_component<coopa::sfx::components::AudioSource>();
    coopa::event::EventArgs on;
    on.set("value", true);
    scene.events().emit("SpeakerToggle", "value_changed", on);
    expect(src->is_playing(), "AudioSourceOnSignal follow: a toggle switched on plays the source");
    coopa::event::EventArgs off;
    off.set("value", false);
    scene.events().emit("SpeakerToggle", "value_changed", off);
    expect(!src->is_playing(), "and switched off stops it");
}

COOPA_TEST(music_status_text_describes_the_state) {
    using namespace music_test;
    Rig rig;
    expect(toy::audio::MusicStatusText::describe(rig.music()) == "(silence)", "silence");
    rig.music().add_track({"song", sine_wav("st", 420.0f, 75.0f), 1.0f, true});
    rig.music().play("song", 1.0f);
    rig.run(0.5f);
    const std::string s = toy::audio::MusicStatusText::describe(rig.music());
    expect(s.find("song  0:00 / 1:15") == 0 && s.find("[crossfading]") != std::string::npos, "track, clock and fade: " + s);
}

COOPA_TEST(audio_demo_clips_decode) {
    using namespace music_test;
    Rig rig;
    const std::string root = std::string(PROJ_DIR) + "/../assets/audio/";
    // The music is IMA-ADPCM: check the decoded length is exact (the fact chunk, not padded blocks)
    // so looping tracks loop without a gap.
    const std::pair<const char*, float> music[] = {{"meadow", 12 * 4 * 60.0f / 92.0f}, {"expedition", 12 * 4 * 60.0f / 116.0f},
                                                   {"nocturne", 8 * 4 * 60.0f / 68.0f}};
    for (const auto& [name, seconds] : music) {
        auto clip = rig.audio.engine().load_clip(root + "music/" + name + ".wav");
        expect(clip->format().channels == 2u, std::string(name) + " is stereo");
        expect_near(static_cast<float>(clip->frame_count()) / k_rate, seconds, 0.002f, std::string(name) + " decodes to its bar length");
        double sum = 0;
        for (uint64_t i = 0; i < clip->frame_count() * 2; i += 97) sum += std::fabs(clip->data()[i]);
        expect(sum > 100.0, std::string(name) + " is not silent");
    }
    for (const char* name : {"beacon", "drone", "crickets", "chimes", "fire", "music_box", "ping"}) {
        auto clip = rig.audio.engine().load_clip(root + "sfx/" + name + ".wav");
        expect(clip->format().channels == 1u, std::string(name) + " is mono (spatializable)");
    }
}

COOPA_TEST(audio_demo_scene_wires_up) {
    // Textual checks on the generated scene and panel (loading them needs the GPU-side parsers):
    // every clip exists, and every name a reactor listens to or targets exists, so a typo can't
    // silently leave a button dead.
    const std::filesystem::path assets = std::filesystem::path(PROJ_DIR) / ".." / "assets";
    const std::filesystem::path dir = assets / "scenes" / "audio" / "audio_demo";
    auto read = [](const std::filesystem::path& p) {
        std::ifstream f(p);
        return std::string(std::istreambuf_iterator<char>(f), {});
    };
    const std::string scene = read(dir / "scene.yaml"), panel = read(dir / "ui" / "audio_panel.yaml");
    expect(scene.find("type: MusicPlaylist") != std::string::npos && scene.find("type: MusicZone") != std::string::npos,
           "the scene has its playlist and music zone");
    auto values = [](const std::string& text, const std::string& key) {
        std::vector<std::string> out;
        for (size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + 1)) {
            size_t b = at + key.size(), e = text.find_first_of(",}\n", b);
            out.push_back(text.substr(b, e - b));
        }
        return out;
    };
    int clips = 0;
    for (const std::string& c : values(scene, "clip: ")) {
        ++clips;
        expect(std::filesystem::exists(assets / c), "clip exists: " + c);
    }
    expect(clips >= 17, "music tracks, beacons, drone, ambience, fire, music box and pings (" + std::to_string(clips) + ")");
    for (const std::string& n : values(panel, "listen_object: ")) {
        expect(panel.find("name: " + n + "\n") != std::string::npos || panel.find("{name: " + n + ",") != std::string::npos,
               "the panel has the widget a reactor listens to: " + n);
    }
    for (const std::string& t : values(panel, "target: ")) {
        expect(scene.find("- name: " + t + "\n") != std::string::npos, "the scene has the object a reactor targets: " + t);
    }
}
