/**
 * @file spatial_audio_test.cpp
 * @brief 3D sound through scene AudioSources, rendered offline: a source's direction around the
 *        listener (left / right / front / behind / above), sources parented to the camera staying
 *        on their side of the head as it turns (the audio_demo's pings), distance rolloff (inverse
 *        and linear, silent past max_distance) and doppler from a source's motion.
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
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <sfxcoopa/sfx_yaml.h>
#include <toyengine/audio/audio_system.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("spatial_audio");

namespace spatial_audio_test {

constexpr uint32_t k_rate = 48000;

std::string sine_wav(float hz) {
    const std::filesystem::path p = coopa::test::scratch_dir("spatial_audio") / ("sine" + std::to_string(int(hz)) + ".wav");
    const uint32_t frames = k_rate * 2;
    std::vector<int16_t> pcm(frames);
    for (uint32_t i = 0; i < frames; ++i) pcm[i] = static_cast<int16_t>(12000.0 * std::sin(2.0 * glm::pi<double>() * hz * i / k_rate));
    std::ofstream f(p, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4); u32(36 + frames * 2); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(1); u32(k_rate); u32(k_rate * 2); u16(2); u16(16);
    f.write("data", 4); u32(frames * 2);
    f.write(reinterpret_cast<const char*>(pcm.data()), frames * 2);
    return p.string();
}

/** @brief Goertzel magnitude of `hz` in the left channel. */
float tone(const std::vector<float>& buf, float hz) {
    const size_t frames = buf.size() / 2;
    const double c = 2.0 * std::cos(2.0 * glm::pi<double>() * hz / k_rate);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < frames; ++i) {
        const double s0 = buf[i * 2] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return static_cast<float>(std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / frames * 2.0);
}

struct Rig {
    toy::audio::AudioSystem audio{toy::audio::AudioSystemOptions{k_rate, false, false}};
    glm::mat4 camera{1.0f};   // at the origin, looking down -Z, +Y up
    Rig() {
        coopa::sfx::SfxResources::instance().set_engine(&audio.engine());
        coopa::sfx::register_sfx_components();
    }
    ~Rig() { coopa::sfx::SfxResources::instance().set_engine(nullptr); }

    std::vector<float> step(coopa::scene::Scene& scene, float dt = 1.0f / 60.0f) {
        scene.update(dt);
        audio.update(dt, &camera);
        std::vector<float> buf(static_cast<size_t>(dt * k_rate) * 2);
        audio.engine().render_offline(buf.data(), static_cast<uint32_t>(buf.size() / 2));
        return buf;
    }
    /** @brief Settles ramps for a few frames, then returns the L/R RMS of the next 0.1 s. */
    void ears(coopa::scene::Scene& scene, float& l, float& r) {
        for (int i = 0; i < 6; ++i) step(scene);
        double sl = 0, sr = 0;
        size_t n = 0;
        for (int i = 0; i < 6; ++i) {
            const std::vector<float> b = step(scene);
            for (size_t k = 0; k < b.size(); k += 2) { sl += b[k] * b[k]; sr += b[k + 1] * b[k + 1]; ++n; }
        }
        l = static_cast<float>(std::sqrt(sl / n));
        r = static_cast<float>(std::sqrt(sr / n));
    }
};

/** @brief One looping, spatialized source at `pos` (extra keys appended to its AudioSource). */
coopa::scene::Scene source_scene(const glm::vec3& pos, const std::string& extra = "", float hz = 600.0f) {
    auto f = [](float v) { return std::to_string(v); };
    return coopa::scene::SceneLoader::load_from_node(fkyaml::node::deserialize(
        "scene:\n  root_objects:\n    - name: speaker\n      components:\n        - type: Transform\n"
        "          position: { x: " + f(pos.x) + ", y: " + f(pos.y) + ", z: " + f(pos.z) + " }\n"
        "          rotation: { x: 0.0, y: 0.0, z: 0.0 }\n          scale: { x: 1.0, y: 1.0, z: 1.0 }\n"
        "        - type: AudioSource\n          clip: \"" + sine_wav(hz) + "\"\n          loop: true\n"
        "          play_on_start: true\n          spatialize: true\n" + extra),
        (coopa::test::scratch_dir("spatial_audio") / "scene.yaml").string());
}

} // namespace spatial_audio_test

COOPA_TEST(direction_around_the_listener) {
    using namespace spatial_audio_test;
    struct Case { const char* what; glm::vec3 pos; int side; };   // side: -1 left louder, +1 right, 0 balanced
    const Case cases[] = {{"left", {-3, 0, 0}, -1}, {"right", {3, 0, 0}, +1}, {"front", {0, 0, -3}, 0},
                          {"behind", {0, 0, 3}, 0}, {"above", {0, 3, 0}, 0}, {"front-right", {2, 0, -2}, +1}};
    for (const Case& c : cases) {
        Rig rig;
        coopa::scene::Scene scene = source_scene(c.pos);
        float l = 0, r = 0;
        rig.ears(scene, l, r);
        const std::string got = std::string(c.what) + " (L " + std::to_string(l) + " / R " + std::to_string(r) + ")";
        expect(l + r > 1e-3f, std::string("heard: ") + got);
        if (c.side < 0) expect(l > r * 1.5f, "louder in the left ear: " + got);
        if (c.side > 0) expect(r > l * 1.2f, "louder in the right ear: " + got);
        if (c.side == 0) expect(std::fabs(l - r) < 0.1f * (l + r), "balanced between the ears: " + got);
    }
}

COOPA_TEST(camera_children_stay_on_their_side_as_the_head_turns) {
    using namespace spatial_audio_test;
    // The audio_demo's pings: sources parented to the camera (Z-up world, camera pitched to the
    // horizon). The camera's local -X is its left whichever way it faces.
    Rig rig;
    coopa::scene::Scene scene = coopa::scene::SceneLoader::load_from_node(fkyaml::node::deserialize(
        "scene:\n  root_objects:\n    - name: camera\n      components:\n        - type: Transform\n"
        "          position: { x: 5.0, y: -2.0, z: 1.7 }\n          rotation: { x: 88.0, y: 0.0, z: 0.0 }\n"
        "          scale: { x: 1.0, y: 1.0, z: 1.0 }\n      children:\n        - name: ping_left\n          components:\n"
        "            - type: Transform\n              position: { x: -4.0, y: 0.0, z: 0.0 }\n"
        "              rotation: { x: 0.0, y: 0.0, z: 0.0 }\n              scale: { x: 1.0, y: 1.0, z: 1.0 }\n"
        "            - type: AudioSource\n              clip: \"" + sine_wav(600.0f) + "\"\n              loop: true\n"
        "              play_on_start: true\n              spatialize: true\n"),
        (coopa::test::scratch_dir("spatial_audio") / "scene.yaml").string());
    coopa::scene::SceneObject* cam = scene.find_object("camera");
    for (float heading : {0.0f, 90.0f, 200.0f}) {
        cam->get_transform()->transform().set_rotation(glm::vec3(88.0f, 0.0f, heading));
        rig.camera = cam->get_transform()->get_world_matrix();
        float l = 0, r = 0;
        rig.ears(scene, l, r);
        expect(l > r * 1.5f, "heading " + std::to_string(int(heading)) + ": the left ping stays left (L " + std::to_string(l) +
                                 " / R " + std::to_string(r) + ")");
    }
}

COOPA_TEST(distance_rolloff) {
    using namespace spatial_audio_test;
    auto level = [](const glm::vec3& pos, const std::string& extra) {
        Rig rig;
        coopa::scene::Scene scene = source_scene(pos, extra);
        float l = 0, r = 0;
        rig.ears(scene, l, r);
        return l + r;
    };
    const std::string inverse = "          min_distance: 1.0\n          max_distance: 50.0\n          curve: Inverse\n";
    const float near_ = level({0, 0, -2}, inverse), far_ = level({0, 0, -8}, inverse);
    expect(far_ < near_ * 0.5f && far_ > 0.0f, "inverse: 8 m is quieter than 2 m but still heard (" + std::to_string(near_) + " / " +
                                                   std::to_string(far_) + ")");
    const std::string linear = "          min_distance: 1.0\n          max_distance: 10.0\n          curve: Linear\n";
    const float inside = level({0, 0, -5}, linear), outside = level({0, 0, -12}, linear);
    expect(inside > 1e-3f, "linear: heard inside max_distance");
    expect(outside < 1e-4f, "linear: silent past max_distance (" + std::to_string(outside) + ")");
}

COOPA_TEST(doppler_follows_source_motion) {
    using namespace spatial_audio_test;
    // A 600 Hz source passing at 40 m/s: approaching plays ~343/303 higher (~679 Hz), receding
    // ~343/383 lower (~537 Hz). The source starts 60 m out along -Z and flies through the listener.
    Rig rig;
    coopa::scene::Scene scene = source_scene({0, 0, -60}, "          max_distance: 200.0\n          doppler: 1.0\n");
    coopa::scene::SceneObject* s = scene.find_object("speaker");
    glm::vec3 pos(0.0f, 0.0f, -60.0f);
    const float dt = 1.0f / 60.0f;
    std::vector<float> approach, recede;
    for (int i = 0; i < 180; ++i) {   // 3 s: -60 m -> +60 m
        pos.z += 40.0f * dt;
        s->get_transform()->transform().set_position(glm::vec3(pos.x + 1.0f, pos.y, pos.z));
        const std::vector<float> b = rig.step(scene, dt);
        if (i == 40) approach = b;     // z ~ -33: closing in
        if (i == 150) recede = b;      // z ~ +40: moving away
    }
    expect(tone(approach, 680.0f) > 2.0f * tone(approach, 600.0f), "approaching raises the pitch");
    expect(tone(recede, 537.0f) > 2.0f * tone(recede, 600.0f), "receding lowers it");
}
