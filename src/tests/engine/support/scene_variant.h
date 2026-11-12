#pragma once

/**
 * @file scene_variant.h
 * @brief A patched copy of a demo scene, for a test that needs one of the demo's scene-level
 *        variants.
 *
 * Each area has one demo scene (assets/scenes/<area>/<name>_demo), but some settings are
 * per scene and hold a single value there: the cloud type, a Terrain's styles, the weather
 * schedule, the camera's start pose. A test that covers the other value loads a variant: the
 * demo's folder (scene.yaml and its meshes/ / materials/ sidecars) copied to the test's
 * scratch directory, with `patch` applied to the scene document. Asset references still
 * resolve -- by name, through the project's asset roots -- so the copy loads like the original.
 */

#include <coopa/testing/test.h>
#include <coopa/yaml/document.h>
#include <coopa/yaml/writer.h>

#include <filesystem>
#include <functional>
#include <string>

#include <root_directory.h>
#include <toyengine/io/runtime_paths.h>

namespace toy::test {

/**
 * @brief Copies the demo folder holding `scene` (a path under a checkout, e.g.
 *        "assets/scenes/rendering/clouds_demo/scene.yaml") to scratch_dir()/<tag>, applies
 *        `patch` to its scene.yaml, and returns the copy's scene.yaml path.
 *
 * `scene` is resolved against the PROJECT this runner was built for first, then against the
 * engine checkout -- the same order as the engine's own asset roots. A game project's suites run
 * against scenes that ship with the project, and `ROOT_DIR` is the engine either way.
 */
inline std::string scene_variant(const std::string& scene, const std::string& tag,
                                 const std::function<void(fkyaml::node& doc)>& patch) {
    std::filesystem::path src = std::filesystem::path(ROOT_DIR) / scene;
    if (const char* project = toy::io::build_project_root(); project != nullptr && *project != '\0') {
        const std::filesystem::path theirs = std::filesystem::path(project) / scene;
        std::error_code ec;
        if (std::filesystem::exists(theirs, ec)) src = theirs;
    }
    const std::filesystem::path dir = coopa::test::scratch_dir(tag);
    std::filesystem::copy(src.parent_path(), dir,
                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
    const std::filesystem::path out = dir / src.filename();
    fkyaml::node doc = coopa::yaml::load_document(out);
    patch(doc);
    coopa::yaml::save_document(out, doc);
    return out.string();
}

/** @brief The first component of `type` on the root object named `object` in a scene document (nullptr if absent). */
inline fkyaml::node* scene_component(fkyaml::node& doc, const std::string& object, const std::string& type) {
    for (auto& obj : doc["scene"]["root_objects"].as_seq()) {
        if (!obj.contains("name") || obj["name"].get_value<std::string>() != object) continue;
        for (auto& c : obj["components"].as_seq()) {
            if (c.contains("type") && c["type"].get_value<std::string>() == type) return &c;
        }
    }
    return nullptr;
}

/**
 * @brief Removes the first component of `type` from the root object named `object`.
 *
 * What a variant needs when a component would OVERRIDE the very fields it is pinning: a test
 * that hand-sets a camera's zoom range, say, has to remove whatever component derives it first,
 * or that component writes its own values straight back.
 *
 * @return True when one was removed.
 */
inline bool remove_scene_component(fkyaml::node& doc, const std::string& object, const std::string& type) {
    for (auto& obj : doc["scene"]["root_objects"].as_seq()) {
        if (!obj.contains("name") || obj["name"].get_value<std::string>() != object) continue;
        auto& comps = obj["components"].as_seq();
        for (auto it = comps.begin(); it != comps.end(); ++it) {
            if (it->contains("type") && (*it)["type"].get_value<std::string>() == type) {
                comps.erase(it);
                return true;
            }
        }
    }
    return false;
}

} // namespace toy::test
