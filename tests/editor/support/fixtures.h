#pragma once

/**
 * @file fixtures.h
 * @brief Light helpers every editor suite may use: no Engine, no editor app.
 *
 * Everything in the editor's tests lives in toy::editor::testing, so a suite sees the editor's
 * own names (Node, EditMesh, SceneDocument, ...) unqualified, exactly as editor code does.
 * Scratch files go under coopa::test::scratch_dir() -- never into the repository.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <root_directory.h>

#include "editor/core/yaml_util.h"

namespace toy::editor::testing {

namespace fs = std::filesystem;
using coopa::test::expect;

/** @brief Writes `text` to `p`, creating its directories. */
inline void write_text(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

/** @brief The whole file as a string. */
inline std::string read_text(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/** @brief Every .yaml / .yml file under the repository's assets/, sorted. */
inline std::vector<fs::path> asset_yaml_files() {
    std::vector<fs::path> out;
    for (const auto& e : fs::recursive_directory_iterator(fs::path(ROOT_DIR) / "assets")) {
        const auto ext = e.path().extension().string();
        if (e.is_regular_file() && (ext == ".yaml" || ext == ".yml")) out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

/** @brief The repository's asset meshes that carry faces (Blender exports and editor saves; LOD
 *         sidecars excluded). */
inline bool is_asset_mesh_file(const fs::path& p) {
    return p.parent_path().filename() == "meshes" && p.filename().string().find(".lod.") == std::string::npos;
}

/**
 * @brief Points HOME at this test's scratch directory, so editor preferences
 *        (~/.toyengine_editor.yaml), recent projects and the hub's ~/.toyengine start empty and
 *        never touch the real ones. The runner restores HOME after the test.
 */
inline fs::path use_scratch_home() {
    const fs::path home = coopa::test::scratch_dir("home");
    setenv("HOME", home.c_str(), 1);
    return home;
}

} // namespace toy::editor::testing
