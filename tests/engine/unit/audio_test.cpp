/**
 * @file audio_test.cpp
 * @brief The engine's AudioSystem over sfxcoopa, rendered offline (no sound card): AudioSources
 *        play through the SFX bus, play_on_start waits for simulation, one-shots and stop_all,
 *        the camera is the listener, and VolumeBinding ties a slider to a bus and the player's
 *        settings.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <coopa/scene/scene_loader.h>
#include <sfxcoopa/sfx_yaml.h>
#include <toyengine/audio/audio_components.h>
#include <toyengine/audio/audio_system.h>
#include <toyengine/core/user_settings.h>
#include <uicoopa/ui_yaml.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("audio");

namespace audio_test {

std::string whoosh() { return std::string(PROJ_DIR) + "/sfxcoopa/ex/SEFE_Whoosh03.wav"; }

/** @brief A mono copy of the whoosh (a .import sidecar with force_mono) for spatial tests. */
std::string mono_whoosh() {
    const std::filesystem::path dst = coopa::test::scratch_dir() / "audio_mono_whoosh.wav";
    std::filesystem::copy_file(whoosh(), dst, std::filesystem::copy_options::overwrite_existing);
    coopa::sfx::data::ClipImportSettings settings;
    settings.force_mono = true;
    settings.save(dst.string());
    return dst.string();
}

/** @brief Left/right RMS of the next `frames` the engine renders. */
void render_rms(coopa::sfx::core::AudioEngine& e, uint32_t frames, float& l, float& r) {
    std::vector<float> buf(frames * 2);
    e.render_offline(buf.data(), frames);
    double sl = 0, sr = 0;
    for (uint32_t i = 0; i < frames; ++i) { sl += buf[i * 2] * buf[i * 2]; sr += buf[i * 2 + 1] * buf[i * 2 + 1]; }
    l = static_cast<float>(std::sqrt(sl / frames));
    r = static_cast<float>(std::sqrt(sr / frames));
}

/** @brief An AudioSystem wired as Engine wires it, minus the device. */
struct Rig {
    toy::audio::AudioSystem audio{toy::audio::AudioSystemOptions{48000, false, false}};
    Rig() {
        coopa::sfx::SfxResources::instance().set_engine(&audio.engine());
        coopa::sfx::register_sfx_components();
        toy::audio::register_audio_components();
        toy::audio::AudioSystem::set_active(&audio);
    }
    ~Rig() {
        toy::audio::AudioSystem::set_active(nullptr);
        coopa::sfx::SfxResources::instance().set_engine(nullptr);
    }
};

coopa::scene::Scene load(const std::string& yaml) {
    coopa::scene::Scene scene = coopa::scene::SceneLoader::load_from_node(fkyaml::node::deserialize(yaml),
                                                                          (coopa::test::scratch_dir() / "audio_scene.yaml").string());
    scene.update(0.0f);   // first frame: play_on_start
    return scene;
}

std::string source_yaml(const std::string& clip, const std::string& extra, float x = 0.0f) {
    return "scene:\n  root_objects:\n    - name: Speaker\n      components:\n        - type: Transform\n"
           "          position: { x: " + std::to_string(x) + ", y: 0.0, z: 0.0 }\n"
           "        - type: AudioSource\n          clip: \"" + clip + "\"\n" + extra;
}

} // namespace audio_test

COOPA_TEST(source_plays_through_the_sfx_bus) {
    using namespace audio_test;
    Rig rig;
    coopa::scene::Scene scene = load(source_yaml(whoosh(), "          play_on_start: true\n          loop: true\n"));
    rig.audio.update(0.016f, nullptr);
    float l = 0, r = 0;
    render_rms(rig.audio.engine(), 4096, l, r);
    expect(l > 1e-3f && r > 1e-3f, "an AudioSource with play_on_start is heard (" + std::to_string(l) + ")");
    rig.audio.set_bus_volume(toy::audio::k_bus_sfx, 0.0f);
    render_rms(rig.audio.engine(), 4096, l, r);   // the bus ramps to its new gain
    render_rms(rig.audio.engine(), 4096, l, r);
    expect(l < 1e-4f && r < 1e-4f, "SFX bus at 0 silences it (" + std::to_string(l) + ")");
}

COOPA_TEST(play_on_start_waits_for_the_first_update) {
    using namespace audio_test;
    Rig rig;
    coopa::scene::Scene scene = coopa::scene::SceneLoader::load_from_node(
        fkyaml::node::deserialize(source_yaml(whoosh(), "          play_on_start: true\n")), (coopa::test::scratch_dir() / "audio_scene.yaml").string());
    float l = 0, r = 0;
    render_rms(rig.audio.engine(), 2048, l, r);
    expect(l < 1e-6f, "a loaded (editor, edit mode) scene stays silent until it simulates a frame");
}

COOPA_TEST(oneshots_play_and_stop_all_silences_them) {
    using namespace audio_test;
    Rig rig;
    const auto h = rig.audio.play_oneshot(whoosh());
    expect(h.is_valid(), "play_oneshot starts a voice");
    float l = 0, r = 0;
    render_rms(rig.audio.engine(), 2048, l, r);
    expect(l > 1e-3f, "the one-shot is heard");
    rig.audio.stop_all();
    render_rms(rig.audio.engine(), 4096, l, r);   // stop fades out
    render_rms(rig.audio.engine(), 2048, l, r);
    expect(l < 1e-5f, "stop_all silences one-shots (" + std::to_string(l) + ")");
    expect(!rig.audio.play_oneshot("does/not/exist.wav").is_valid(), "a missing clip is an invalid handle, not a throw");
}

COOPA_TEST(camera_is_the_listener) {
    using namespace audio_test;
    Rig rig;
    // Source at +X; the camera (listener) at the origin looking down -Z: the source is on the right.
    coopa::scene::Scene scene = load(source_yaml(mono_whoosh(), "          play_on_start: true\n          loop: true\n"
                                                                "          spatialize: true\n", 3.0f));
    const glm::mat4 cam(1.0f);
    rig.audio.update(0.016f, &cam);
    float l = 0, r = 0;
    render_rms(rig.audio.engine(), 4096, l, r);
    render_rms(rig.audio.engine(), 4096, l, r);
    expect(r > l * 1.5f, "with no AudioListener the camera hears: a source on its right is louder right (" +
                             std::to_string(l) + " / " + std::to_string(r) + ")");
    // Turn the camera around (look down +Z): now the source is on its left.
    const glm::mat4 turned = glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(0, 1, 0));
    rig.audio.update(0.016f, &turned);
    render_rms(rig.audio.engine(), 4096, l, r);
    render_rms(rig.audio.engine(), 4096, l, r);
    expect(l > r * 1.5f, "turning the camera swaps the ears");
}

COOPA_TEST(volume_binding_drives_bus_and_settings) {
    using namespace audio_test;
    Rig rig;
    const std::filesystem::path file = coopa::test::scratch_dir("volume_binding") / "settings.yaml";
    toy::core::UserSettings::instance().load(file);
    rig.audio.set_bus_volume(toy::audio::k_bus_music, 0.7f);
    coopa::scene::SceneObject obj("MusicRow");
    auto* slider = obj.add_component<coopa::ui::Slider>();
    auto* vb = obj.add_component<toy::audio::VolumeBinding>();
    vb->bus = toy::audio::k_bus_music;
    vb->update(0.016f);
    expect(vb->slider() == slider && std::fabs(slider->value() - 0.7f) < 1e-4f, "the slider starts at the bus's volume");
    slider->set_value(0.25f, false);
    vb->update(0.016f);
    expect_near(rig.audio.bus_volume(toy::audio::k_bus_music), 0.25f, 1e-4f, "moving the slider sets the bus");
    expect_near(toy::core::UserSettings::instance().get_float("audio.music", -1.0f), 0.25f, 1e-4f, "and remembers it for the player");
    toy::core::UserSettings::instance().load(coopa::test::scratch_dir() / "no_such_settings.yaml");   // leave nothing dirty behind
}
