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
 * @brief Points GFX_PIPELINE_CACHE_DIR at a directory that outlives one test's scratch space.
 *
 * gfxcoopa's Vulkan pipeline cache resolves its default location through HOME, and
 * use_scratch_home() repoints HOME at a per-test directory that is then wiped -- so without this
 * every case recompiles every pipeline from SPIR-V. That is not a rounding error: measured on
 * `engine_contract`, a suite of three cases went 26.80 s -> 2.21 s once the cache survived, with
 * per-case Engine construction dropping from ~8.9 s to ~0.74 s.
 *
 * The directory is deliberately shared by every suite and every run, since a Vulkan cache blob is
 * keyed by the driver's own pipelineCacheUUID and is safe to pool. An explicit
 * GFX_PIPELINE_CACHE_DIR from the environment always wins.
 */
inline void pin_pipeline_cache_outside_scratch() {
    if (const char* set = std::getenv("GFX_PIPELINE_CACHE_DIR"); set && *set) return;
    const fs::path shared = fs::temp_directory_path() / "toyengine_pipeline_cache";
    std::error_code ec;
    fs::create_directories(shared, ec);
    if (!ec) setenv("GFX_PIPELINE_CACHE_DIR", shared.c_str(), 1);
}

/**
 * @brief Points HOME at this test's scratch directory, so editor preferences
 *        (~/.toyengine_editor.yaml), recent projects and the hub's ~/.toyengine start empty and
 *        never touch the real ones. The runner restores HOME after the test.
 */
inline fs::path use_scratch_home() {
    const fs::path home = coopa::test::scratch_dir("home");
    setenv("HOME", home.c_str(), 1);
    pin_pipeline_cache_outside_scratch();
    return home;
}

} // namespace toy::editor::testing
