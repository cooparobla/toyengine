/**
 * @file clip_referrers_test.cpp
 * @brief Project::clip_referrers(): which documents play an animation clip.
 *
 * One clip file is routinely played by a whole family of rigs, and the editor warns about that
 * before a clip edit reaches them. The cases that matter are the ones a text search would get
 * wrong: a clip named under a different spelling, a document-local clip shadowing a shared one
 * of the same relative path, and the two other places the key `clip` appears (an AudioSource's
 * sound, and a clip document's own `clip:` block).
 */

#include <coopa/testing/test.h>

#include "editor/app/project.h"
#include "editor/support/fixtures.h"

COOPA_TEST_SUITE("clip_referrers");

namespace toy::editor::testing {
namespace {

/** @brief A project with one shared clip set and the rigs below; returns it opened. */
Project make_project() {
    const fs::path root = coopa::test::scratch_dir("ClipRefs");
    write_text(root / "clipped.toy", "format: toyproject\nversion: 1\ntarget: clipped\n");
    Project p(root);
    const fs::path a = root / "assets";

    for (const char* clip : {"walk", "idle"}) {
        write_text(a / "animations/hero" / (std::string(clip) + ".yaml"),
                   std::string("clip:\n  name: ") + clip + "\n  wrap: loop\n  length: 1\n  tracks: []\n");
    }

    // Two rigs sharing hero/walk.yaml, each spelling the reference differently.
    write_text(a / "objects/characters/knight.yaml",
               "object:\n  name: knight\n  components:\n"
               "    - type: Animator\n      auto_play: idle\n      states:\n"
               "        - {name: idle, clip: animations/hero/idle.yaml}\n"
               "        - {name: walk, clip: animations/hero/walk.yaml}\n");
    // Named without its tag folder: AssetSource::resolve() falls back to AssetIndex, which
    // finds animations/<anything>/walk.yaml by name. The same file as the knight's reference,
    // spelt differently -- the alias a text search over the asset tree would miss.
    write_text(a / "objects/characters/squire.yaml",
               "object:\n  name: squire\n  components:\n"
               "    - type: Animator\n      states:\n"
               "        - {name: walk, clip: animations/walk.yaml}\n");

    // A scene declaring its Animator inline: scenes refer to clips too.
    write_text(a / "scenes/keep/scene.yaml",
               "scene:\n  root_objects:\n    - name: dummy\n      components:\n"
               "        - type: Animator\n          states:\n"
               "            - {name: walk, clip: animations/hero/walk.yaml}\n");
    return p;
}

std::string joined(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) out += (out.empty() ? "" : ", ") + s;
    return out;
}

} // namespace

COOPA_TEST(every_rig_playing_a_clip_is_listed_however_it_spells_the_path) {
    Project p = make_project();
    const fs::path walk = p.assets() / "animations/hero/walk.yaml";

    const std::vector<std::string> refs = p.clip_referrers(walk);
    expect(refs.size() == 3, "three documents play hero/walk.yaml, got: " + joined(refs));
    expect(joined(refs) == "objects/characters/knight.yaml, objects/characters/squire.yaml, scenes/keep/scene.yaml",
           "object assets and scenes alike, sorted, each listed once: " + joined(refs));

    // A clip only one rig names is not "shared".
    const std::vector<std::string> idle = p.clip_referrers(p.assets() / "animations/hero/idle.yaml");
    expect(idle.size() == 1 && idle[0] == "objects/characters/knight.yaml", "idle has one referrer: " + joined(idle));

    // A clip nothing names at all.
    write_text(p.assets() / "animations/hero/orphan.yaml", "clip:\n  name: orphan\n  length: 1\n  tracks: []\n");
    p.refresh();
    expect(p.clip_referrers(p.assets() / "animations/hero/orphan.yaml").empty(), "an unplayed clip has no referrers");
}

COOPA_TEST(a_document_local_clip_is_a_different_file_from_the_shared_one) {
    Project p = make_project();
    // The scene keeps its own animations/hero/walk.yaml. The engine (and the Timeline) resolve a
    // clip next to the declaring document first, so the scene now plays ITS copy -- and editing
    // the shared one must no longer claim the scene among its referrers.
    write_text(p.assets() / "scenes/keep/animations/hero/walk.yaml",
               "clip:\n  name: walk\n  length: 1\n  tracks: []\n");
    p.refresh();

    const std::vector<std::string> shared = p.clip_referrers(p.assets() / "animations/hero/walk.yaml");
    expect(shared.size() == 2, "the shared clip keeps only the two object assets: " + joined(shared));
    expect(joined(shared).find("scenes/keep") == std::string::npos, "the scene is no longer a referrer of it");

    const std::vector<std::string> local = p.clip_referrers(p.assets() / "scenes/keep/animations/hero/walk.yaml");
    expect(local.size() == 1 && local[0] == "scenes/keep/scene.yaml", "the local copy has the scene: " + joined(local));
}

COOPA_TEST(other_uses_of_the_key_clip_are_not_animation_referrers) {
    Project p = make_project();
    // An AudioSource's `clip`, and a clip document's own `clip:` block, both sit under the same
    // key name. Only an Animator's states[].clip counts, so neither can inflate the count.
    write_text(p.assets() / "objects/props/bell.yaml",
               "object:\n  name: bell\n  components:\n"
               "    - type: AudioSource\n      clip: animations/hero/walk.yaml\n");
    p.refresh();

    const std::vector<std::string> refs = p.clip_referrers(p.assets() / "animations/hero/walk.yaml");
    expect(refs.size() == 3, "the AudioSource is not an animation referrer: " + joined(refs));
    expect(joined(refs).find("bell") == std::string::npos, "...and is not listed");

    // The clip file itself contains `clip:` at its root and must never be its own referrer.
    expect(joined(refs).find("animations/") == std::string::npos, "a clip document is not a referrer: " + joined(refs));
}

COOPA_TEST(repointing_rewrites_only_the_documents_it_is_given) {
    Project p = make_project();
    const std::vector<std::string> others{"scenes/keep/scene.yaml", "objects/characters/squire.yaml"};
    const std::vector<std::string> changed =
        p.repoint_clip_refs(others, "animations/hero/walk.yaml", "animations/hero/strut.yaml");

    // Both move: RefRename rewrites the full path and the tag-folder-less short form alike,
    // so a reference that reaches the clip by name is carried along too.
    expect(changed.size() == 2, "both named documents were rewritten");
    const Node scene = coopa::yaml::load_document(p.assets() / "scenes/keep/scene.yaml");
    const std::string sc = get_string(scene["scene"]["root_objects"][0]["components"][0]["states"][0], "clip");
    expect(sc == "animations/hero/strut.yaml", "...to the new path, got: " + sc);

    // The document NOT named is untouched — which is how the editor keeps its own open file,
    // whose in-memory copy would overwrite a disk edit, out of the rewrite.
    const Node knight = coopa::yaml::load_document(p.assets() / "objects/characters/knight.yaml");
    const std::string k = get_string(knight["object"]["components"][0]["states"][1], "clip");
    expect(k == "animations/hero/walk.yaml", "the document left out keeps its reference, got: " + k);

    // The aliased reference keeps its own shape — short in, short out — rather than being
    // expanded to the full path.
    const Node squire = coopa::yaml::load_document(p.assets() / "objects/characters/squire.yaml");
    const std::string sq = get_string(squire["object"]["components"][0]["states"][0], "clip");
    expect(sq == "animations/strut.yaml", "the short-form reference moved as a short form, got: " + sq);
}

} // namespace toy::editor::testing
