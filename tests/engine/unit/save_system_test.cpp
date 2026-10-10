/**
 * @file save_system_test.cpp
 * @brief Saves (toyengine/save/): every value kind round-trips through a slot, slot names cannot
 *        escape, listings read the .meta sidecars, an interrupted write leaves the previous save
 *        intact, caml-encoded slots round-trip, migrations run in order, and ISaveable components
 *        restore by SaveId into a fresh scene. Engine-level cross-scene loads are scene_loading's.
 */

#include <coopa/testing/test.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <coopa/scene/scene_loader.h>
#include <toyengine/core/runtime_paths.h>
#include <toyengine/save/register.h>
#include <toyengine/save/save_system.h>
#include <toyengine/scene/character_controller.h>
#include <toyengine/scene/register.h>

#include "engine/support/checks.h"
#include "engine/support/scratch_files.h"

COOPA_TEST_SUITE("save_system");

using namespace toy::test;

namespace {

namespace save_test {

/** @brief A SaveSystem writing to a fresh directory, plain YAML, with no host (no scene). */
std::unique_ptr<toy::save::SaveSystem> make_saves(const std::string& dir_name) {
    auto saves = std::make_unique<toy::save::SaveSystem>();
    saves->set_root(coopa::test::scratch_dir(dir_name) / "saves");
    saves->set_encoding(toy::save::SaveEncoding::Yaml);
    return saves;
}

/** @brief A file's raw bytes (no decoding). */
std::string raw_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/** @brief A test ISaveable: one counter. */
class Counter : public coopa::scene::Component, public toy::save::ISaveable {
public:
    int value = 0;
    std::string type_name() const override { return "TestSaveCounter"; }
    std::string save_key() const override { return "counter"; }
    void save(toy::save::SaveNode& out) override { out.set("value", value); }
    void load(const toy::save::SaveNode& in) override { value = in.get("value", -1); }
};

} // namespace save_test

} // namespace

/**
 * @brief A slot round-trip: on_save writes global state of every supported kind, the slot is
 *        written (directories created), on_load reads it all back; a bad slot name and a
 *        missing slot fail cleanly; delete_slot removes it.
 */
COOPA_TEST(slot_round_trips_every_value_kind) {
    auto saves = save_test::make_saves("save_roundtrip");
    auto save_conn = saves->on_save.connect_scoped([](toy::save::SaveGame& g) {
        toy::save::SaveNode s = g.global();
        s.set("gold", 120);
        s.set("name", "Ada");
        s.set("alive", true);
        s.set("ratio", 0.25f);
        s.set("home", glm::vec3(1.0f, -2.0f, 3.5f));
        s.set("rot", glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f)));
        s.section("quests").set("done", std::vector<std::string>{"intro", "bridge"});
    });
    expect(saves->save("slot_1"), "save: writes slot_1 (" + saves->last_error() + ")");
    expect(saves->has_slot("slot_1") && std::filesystem::exists(saves->meta_path("slot_1")), "save: .save and .meta exist");

    bool loaded = false;
    std::string finished;
    auto load_conn = saves->on_load.connect_scoped([&](const toy::save::SaveGame& g) {
        const toy::save::SaveNode s = g.global();
        loaded = true;
        expect(s.get("gold", 0) == 120, "save: int round-trips");
        expect(s.get("name", "") == "Ada", "save: string round-trips");
        expect(s.get("alive", false), "save: bool round-trips");
        expect_near(s.get("ratio", 0.0f), 0.25f, 1e-6f, "save: float round-trips");
        const glm::vec3 home = s.get("home", glm::vec3(0.0f));
        expect(home == glm::vec3(1.0f, -2.0f, 3.5f), "save: vec3 round-trips");
        const glm::quat rot = s.get("rot", glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        expect(std::abs(glm::dot(rot, glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f)))) > 0.9999f, "save: quat round-trips");
        const auto done = s.section("quests").get("done", std::vector<std::string>{});
        expect(done == std::vector<std::string>({"intro", "bridge"}), "save: a string list in a sub-section round-trips");
        expect(s.get("missing", 7) == 7 && s.get("name", 3) == 3, "save: a missing or mistyped value reads as the fallback");
        expect(!s.section("nope").valid() && s.section("nope").get("x", 1) == 1, "save: a missing section reads as empty");
        bool threw = false;
        try { toy::save::SaveNode copy = s; copy.set("x", 1); } catch (const std::logic_error&) { threw = true; }
        expect(threw, "save: a loaded document is read-only to on_load");
    });
    auto done_conn = saves->on_loaded.connect_scoped([&](const std::string& slot) { finished = slot; });
    expect(saves->load("slot_1") && loaded && finished == "slot_1", "save: load applies the slot and reports it");

    expect(!saves->save("../escape") && !saves->save("") && !saves->save("a/b"), "save: slot names cannot leave the save directory");
    expect(!saves->load("never_saved"), "save: loading a missing slot fails");
    expect(saves->delete_slot("slot_1") && !saves->has_slot("slot_1") && !std::filesystem::exists(saves->meta_path("slot_1")),
           "save: delete_slot removes the save and its sidecar");
    expect(!saves->delete_slot("slot_1"), "save: deleting a missing slot reports false");
}

/**
 * @brief Slot listing reads the .meta sidecars: slot, scene (from the host), play time, version
 *        and the game's summary; a slot that lost its sidecar is still listed from its .save.
 */
COOPA_TEST(listing_reads_the_meta_sidecars) {
    auto saves = save_test::make_saves("save_listing");
    toy::save::SaveSystem::Host host;
    host.scene_path = [] { return std::string("assets/scenes/tests/gameplay/character_test/scene.yaml"); };
    saves->set_host(host);
    saves->set_version(3);
    int level = 0;
    auto conn = saves->on_save.connect_scoped([&](toy::save::SaveGame& g) { g.summary().set("level", level); });
    expect(saves->list_slots().empty(), "listing: no directory yet lists nothing");
    level = 4;
    saves->set_play_time(125.0);
    expect(saves->save("alpha"), "listing: save alpha");
    level = 9;
    saves->tick(5.0f);
    expect(saves->save("beta"), "listing: save beta");

    const auto slots = saves->list_slots();
    expect(slots.size() == 2, "listing: two slots (got " + std::to_string(slots.size()) + ")");
    for (const auto& info : slots) {
        expect(info.scene == "assets/scenes/tests/gameplay/character_test/scene.yaml", "listing: the saved scene is in the meta");
        expect(info.version == 3 && info.timestamp > 0 && info.saved_at.size() == 20, "listing: version and time are in the meta");
        const int want_level = info.slot == "alpha" ? 4 : 9;
        expect(info.summary_node().get("level", -1) == want_level, "listing: the game's summary is in the meta (" + info.slot + ")");
        expect_near(static_cast<float>(info.play_time), info.slot == "alpha" ? 125.0f : 130.0f, 1e-3f, "listing: play time");
    }
    std::filesystem::remove(saves->meta_path("beta"));
    const auto info = saves->slot_info("beta");
    expect(info && info->summary_node().get("level", -1) == 9 && info->version == 3,
           "listing: a slot without its sidecar is described from the .save");
    expect(saves->list_slots().size() == 2, "listing: still two slots");
}

/**
 * @brief Atomic writes: a temp file left by a write that died midway is neither listed nor read,
 *        the previous save stays loadable, and the next save replaces it cleanly.
 */
COOPA_TEST(interrupted_write_leaves_the_previous_save_intact) {
    namespace fs = std::filesystem;
    auto saves = save_test::make_saves("save_atomic");
    int gold = 10;
    auto c1 = saves->on_save.connect_scoped([&](toy::save::SaveGame& g) { g.global().set("gold", gold); });
    int read_gold = -1;
    auto c2 = saves->on_load.connect_scoped([&](const toy::save::SaveGame& g) { read_gold = g.global().get("gold", -1); });
    expect(saves->save("slot"), "atomic: first save");

    // A crash mid-write: the temp sibling holds a truncated document; the real file is untouched.
    fs::path tmp = saves->slot_path("slot");
    tmp += ".tmp~";
    write_text_file(tmp, "version: 1\nglobal: { gold: 99");
    expect(saves->list_slots().size() == 1, "atomic: the stray temp file is not a slot");
    expect(saves->load("slot") && read_gold == 10, "atomic: the previous save still loads intact");

    gold = 20;
    expect(saves->save("slot") && !fs::exists(tmp), "atomic: the next save replaces the temp file");
    expect(saves->load("slot") && read_gold == 20, "atomic: the new save loads");
    for (const auto& e : fs::directory_iterator(saves->root())) {
        expect(e.path().extension() == ".save" || e.path().extension() == ".meta",
               "atomic: only .save/.meta files remain (" + e.path().filename().string() + ")");
    }
}

/**
 * @brief Caml saves: written encoded (CAML magic, no readable keys), read back without a
 *        registered codec, and a plain YAML slot written earlier still loads after switching.
 */
COOPA_TEST(caml_encoded_slots_round_trip) {
    auto saves = save_test::make_saves("save_caml");
    std::string secret = "plain";
    auto c1 = saves->on_save.connect_scoped([&](toy::save::SaveGame& g) { g.global().set("secret", secret); });
    std::string got;
    auto c2 = saves->on_load.connect_scoped([&](const toy::save::SaveGame& g) { got = g.global().get("secret", ""); });
    expect(saves->save("old_yaml"), "caml: a yaml slot");

    saves->set_encoding(toy::save::SaveEncoding::Caml);
    secret = "the_hidden_value";
    expect(saves->save("encoded"), "caml: an encoded slot (" + saves->last_error() + ")");
    const std::string bytes = save_test::raw_bytes(saves->slot_path("encoded"));
    expect(bytes.rfind("CAML", 0) == 0, "caml: the .save starts with the CAML magic");
    expect(bytes.find("the_hidden_value") == std::string::npos, "caml: the value is not readable in the file");
    expect(save_test::raw_bytes(saves->meta_path("encoded")).rfind("CAML", 0) == 0, "caml: the sidecar is encoded too");
    expect(saves->load("encoded") && got == "the_hidden_value", "caml: an encoded slot loads");
    expect(saves->load("old_yaml") && got == "plain", "caml: a yaml slot still loads after switching to caml");
    expect(saves->list_slots().size() == 2, "caml: both slots list");
    expect(toy::save::parse_save_encoding("auto") == (toy::core::k_shipping ? toy::save::SaveEncoding::Caml : toy::save::SaveEncoding::Yaml) &&
           !toy::save::parse_save_encoding("binary"), "caml: save.encode parsing");
}

/**
 * @brief Migrations run in version order (registered out of order), each once, before on_load,
 *        and leave the document at the current version; a document already current runs none.
 */
COOPA_TEST(migrations_run_in_version_order) {
    auto saves = save_test::make_saves("save_migrations");
    saves->set_version(1);
    auto c1 = saves->on_save.connect_scoped([](toy::save::SaveGame& g) { g.global().set("coins", 5); });
    expect(saves->save("v1"), "migration: a version-1 save");
    c1.disconnect();

    saves->set_version(4);
    std::vector<int> ran;
    saves->register_migration(3, [&](toy::save::SaveGame& g) { ran.push_back(3); g.global().set("gold", g.global().get("gold", 0) * 2); });
    saves->register_migration(1, [&](toy::save::SaveGame& g) { ran.push_back(1); g.global().set("gold", g.global().get("coins", 0)); g.global().erase("coins"); });
    saves->register_migration(2, [&](toy::save::SaveGame& g) { ran.push_back(2); g.global().set("gold", g.global().get("gold", 0) + 1); });
    int gold = -1, version = -1;
    bool has_coins = true;
    auto c2 = saves->on_load.connect_scoped([&](const toy::save::SaveGame& g) {
        gold = g.global().get("gold", -1);
        has_coins = g.global().has("coins");
        version = g.version();
    });
    expect(saves->load("v1"), "migration: load the old save");
    expect(ran == std::vector<int>({1, 2, 3}), "migration: 1 -> 2 -> 3 -> 4 in order");
    expect(gold == 12 && !has_coins, "migration: each step saw the previous one's result (gold " + std::to_string(gold) + ")");
    expect(version == 4, "migration: on_load sees the current version");

    ran.clear();
    auto c3 = saves->on_save.connect_scoped([](toy::save::SaveGame& g) { g.global().set("gold", 1); });
    expect(saves->save("v4") && saves->load("v4") && ran.empty(), "migration: a current save runs none");
}

/**
 * @brief ISaveable through a scene by SaveId: a scene file with SaveIds (explicit, empty -> the
 *        name path, on an inactive object) and ISaveable components (a CharacterController and a
 *        test counter) is saved, a fresh copy of the same scene is loaded from the save, and every
 *        component gets its own state back; objects without a SaveId are untouched.
 */
COOPA_TEST(saveables_restore_by_save_id) {
    toy::scene::register_scene_components();
    toy::save::register_save_components();
    coopa::scene::SceneLoader::register_component_parser("TestSaveCounter",
        [](const fkyaml::node&, coopa::scene::SceneObject& obj, const coopa::scene::SceneLoader::ParseContext&) {
            obj.add_component<save_test::Counter>();
        });
    const std::string yaml = R"(scene:
  scene_name: save_scene
  root_objects:
    - name: hero
      components:
        - type: Transform
          position: { x: 0, y: 0, z: 0 }
        - type: CharacterController
        - type: SaveId
          id: player
    - name: group
      components:
        - type: Transform
      children:
        - name: chest
          components:
            - type: Transform
            - type: TestSaveCounter
            - type: SaveId
    - name: sleeping
      active: false
      components:
        - type: Transform
        - type: TestSaveCounter
        - type: SaveId
          id: sleeper
    - name: untracked
      components:
        - type: Transform
        - type: TestSaveCounter
)";
    const std::string anchor = (coopa::test::scratch_dir() / "save_scene.yaml").string();
    auto build = [&] { return coopa::scene::SceneLoader::load_from_node(fkyaml::node::deserialize(yaml), anchor); };
    auto counter = [](coopa::scene::Scene& s, const char* path) {
        coopa::scene::SceneObject* o = s.find_object_by_path(path);
        return o ? o->get_component<save_test::Counter>() : nullptr;
    };

    coopa::scene::Scene first = build();
    coopa::scene::Scene* current = &first;
    auto saves = save_test::make_saves("save_scene_objects");
    toy::save::SaveSystem::Host host;
    host.scene = [&] { return current; };
    saves->set_host(host);

    auto* cc = first.find_object("hero")->get_component<toy::scene::CharacterController>();
    expect(cc != nullptr, "saveable: the hero has its CharacterController");
    if (!cc) return;
    cc->teleport(glm::vec3(4.0f, -2.5f, 1.25f));
    cc->load(toy::save::SaveGame().global());   // an empty section leaves the pose alone
    {
        // Facing comes from the controller's yaw: set it through a save section, as a load would.
        toy::save::SaveGame g;
        toy::save::SaveNode n = g.global();
        n.set("yaw", 135.0f);
        cc->load(n);
    }
    counter(first, "group:chest")->value = 7;
    counter(first, "sleeping")->value = 3;
    counter(first, "untracked")->value = 11;
    expect(saves->save("objects"), "saveable: save the scene");

    const auto game = saves->read("objects");
    expect(game && game->has_object("player") && game->has_object("group:chest") && game->has_object("sleeper"),
           "saveable: one section per SaveId (an empty id is the name path; inactive objects too)");
    expect(game && game->object_ids().size() == 3, "saveable: objects without a SaveId are not saved");
    expect(game && game->object("group:chest").section("counter").get("value", 0) == 7, "saveable: objects/<id>/<save_key>");

    coopa::scene::Scene second = build();
    current = &second;
    expect(saves->load("objects"), "saveable: load into a fresh copy of the scene");
    const glm::vec3 pos = second.find_object("hero")->get_transform()->transform().position();
    expect(glm::length(pos - glm::vec3(4.0f, -2.5f, 1.25f)) < 1e-4f, "saveable: the CharacterController's position is restored");
    auto* cc2 = second.find_object("hero")->get_component<toy::scene::CharacterController>();
    expect_near(cc2->yaw_deg(), 135.0f, 1e-3f, "saveable: the CharacterController's facing is restored");
    expect(counter(second, "group:chest")->value == 7, "saveable: a nested object's state is restored by its path id");
    expect(counter(second, "sleeping")->value == 3, "saveable: an inactive object's state is restored");
    expect(counter(second, "untracked")->value == 0, "saveable: an object without a SaveId is left as built");
    expect(toy::save::find_by_save_id(second, "sleeper") == second.find_object("sleeping") &&
           toy::save::find_by_save_id(second, "group:chest") == second.find_object_by_path("group:chest") &&
           !toy::save::find_by_save_id(second, "nobody"), "saveable: find_by_save_id (explicit ids, path ids, inactive objects)");
}
