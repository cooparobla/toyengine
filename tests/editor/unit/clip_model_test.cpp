/**
 * @file clip_model_test.cpp
 * @brief The Timeline's clip model against the runtime's clip parser: keying (quaternion hemisphere),
 * events, moving / deleting dope-sheet cells, renaming tracks, and a lossless YAML round trip --
 * unknown fields, procedural tracks and every clip file in the repository included.
 */

#include <coopa/testing/test.h>

#include <coopa/animation/animation_clip_loader.h>
#include <coopa/yaml/writer.h>

#include "editor/anim/clip_model.h"
#include "editor/support/fixtures.h"

COOPA_TEST_SUITE("clip_model");

namespace toy::editor::testing {

/** @brief The clip model: the runtime's YAML round trip, keying (quaternion hemisphere), moving
 *         and deleting dope-sheet cells, and renaming an object's tracks. */
COOPA_TEST(keys_events_cells_and_lossless_round_trip) {
    ClipModel m;
    m.name = "Wave";
    m.wrap = "pingpong";
    m.length = 2.0f;
    m.set_key("Arm", "position", 0.0f, glm::vec4(0, 0, 0, 0));
    m.set_key("Arm", "position", 1.0f, glm::vec4(1, 2, 3, 0));
    m.set_key("Arm", "rotation_quat", 0.0f, glm::vec4(0, 0, 0, 1));
    m.set_key("Arm", "rotation_quat", 1.0f, glm::vec4(0, 0, -0.7071f, -0.7071f));   // same rotation as +, other hemisphere
    m.set_key("Arm/Hand", "scale", 0.5f, glm::vec4(2, 2, 2, 0));
    expect(m.find_track("Arm", "rotation_quat")->keys[1].value.w > 0.0f,
           "a key in the opposite hemisphere is flipped next to its neighbour");
    m.set_key("Arm", "position", 1.0f, glm::vec4(5, 0, 0, 0));
    expect(m.find_track("Arm", "position")->keys.size() == 2 && m.find_track("Arm", "position")->keys[1].value.x == 5.0f,
           "keying an existing time replaces the key");

    // Events: first-class, kept in time order, payloads round-trip.
    expect(m.add_event(0.5f, "footstep") == 0 && m.add_event(0.25f, "whoosh") == 0 && m.add_event(0.5f, "clang") == 2,
           "events: inserted in time order (after equal times)");
    m.events[1].string_value = "left";
    m.events[2].float_value = 0.75f;
    expect(m.move_event(0, 1.5f) == 2 && m.events[2].name == "whoosh", "events: a moved event re-sorts");
    expect(m.delete_event(1) && m.events.size() == 2 && !m.delete_event(5), "events: delete by index");
    m.add_event(0.1f, "spare");

    const Node n = Node::deserialize(coopa::yaml::emit(m.to_node()));
    expect(ClipModel::from_node(n) == m, "the clip round-trips through YAML (events included)");
    const auto rt_ev = coopa::anim::parse_clip(n);
    expect(rt_ev.events.size() == 3 && rt_ev.events[0].name == "spare" && rt_ev.events[1].name == "footstep" &&
               rt_ev.events[1].string_value == "left" && rt_ev.events[2].name == "whoosh",
           "events: the runtime reads the editor's events (name, time order, payload)");
    // Keys the model does not know -- at clip, track and key level, and a procedural track --
    // come back verbatim.
    const Node odd = Node::deserialize(std::string(
        "clip:\n  name: odd\n  wrap: once\n  length: 2.0\n  events: [{time: 1.0, name: step}]\n  tracks:\n"
        "    - object: Arm\n      component: Light\n      component_index: 1\n      property: color.x\n"
        "      keys:\n        - {time: 0.0, value: [1.0], easing: step, note: hi}\n"
        "    - object: Arm\n      property: position\n      procedural: {type: sine, amplitude: [0.0, 0.0, 1.0], frequency: 2.0}\n"));
    const Node odd_back = Node::deserialize(coopa::yaml::emit(ClipModel::from_node(odd).to_node()));
    expect(odd_back == odd, "unknown clip, track and key fields and procedural tracks survive the editor:\n" + coopa::yaml::emit(odd_back));
    // Every clip file in the repository: the editor's model writes it back as it was.
    int clips = 0;
    for (const auto& p : asset_yaml_files()) {
        const Node src = coopa::yaml::load_document(p);
        if (!src.is_mapping() || !src.contains("clip")) continue;
        const Node back = Node::deserialize(coopa::yaml::emit(ClipModel::from_node(src).to_node()));
        const auto a = coopa::anim::parse_clip(src), b = coopa::anim::parse_clip(back);
        bool same = a.tracks.size() == b.tracks.size() && a.effective_length() == b.effective_length() && a.wrap == b.wrap;
        for (size_t t = 0; same && t < a.tracks.size(); ++t) {
            same = a.tracks[t].object_path == b.tracks[t].object_path && a.tracks[t].property == b.tracks[t].property &&
                   a.tracks[t].curve.keys().size() == b.tracks[t].curve.keys().size();
            for (size_t k = 0; same && k < a.tracks[t].curve.keys().size(); ++k) {
                const auto& ka = a.tracks[t].curve.keys()[k];
                const auto& kb = b.tracks[t].curve.keys()[k];
                same = std::abs(ka.time - kb.time) < 1e-6f && ka.easing == kb.easing;
                for (int c = 0; same && c < 4; ++c) same = std::abs(ka.value[c] - kb.value[c]) < 1e-5f;
            }
        }
        expect(same, fs::relative(p, ROOT_DIR).string() + ": the editor rewrites the clip the runtime reads identically");
        ++clips;
    }
    expect(clips >= 3, "checked the repository's clip files (" + std::to_string(clips) + ")");
    const coopa::anim::AnimationClip rt = coopa::anim::parse_clip(n);
    expect(rt.tracks.size() == 3 && rt.effective_length() == 2.0f && rt.wrap == coopa::anim::WrapMode::PingPong,
           "the runtime parses the editor's file (tracks, length, wrap)");

    const std::string arm = "Arm";
    expect(m.key_times(&arm) == std::vector<float>{0.0f, 1.0f} && m.key_times().size() == 3, "key times per object and in summary");
    auto moved = m.move_keys({{"Arm", 1.0f}}, 0.5f);
    expect(moved.count({"Arm", 1.5f}) && m.find_track("Arm", "position")->keys[1].time == 1.5f &&
               m.find_track("Arm", "rotation_quat")->keys[1].time == 1.5f,
           "moving a cell moves every track's key of that object at that time");
    m.delete_keys({{"Arm", 0.0f}});
    expect(m.find_track("Arm", "position")->keys.size() == 1, "deleting a cell removes the object's keys there");
    m.delete_keys({{"Arm/Hand", 0.5f}});
    expect(m.find_track("Arm/Hand", "scale") == nullptr, "a track left without keys goes");
    m.set_key("Arm/Hand", "scale", 0.5f, glm::vec4(1));
    m.rename_object("Arm", "Limb");
    expect(m.find_track("Limb", "position") && m.find_track("Limb/Hand", "scale"), "renaming an object renames its descendants' tracks");
}

} // namespace toy::editor::testing
