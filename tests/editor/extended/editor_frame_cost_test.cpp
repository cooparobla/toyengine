/**
 * @file editor_frame_cost_test.cpp
 * @brief Extended (perf budget): the editor's per-frame cost on a large flat scene stays linear in
 * object count. Guards a known regression -- per-frame editor work went quadratic (~800 ms a
 * frame on nav_stress_demo) before the O(1) document index, cached mesh resolution and the
 * virtualized outliner.
 */

#include <coopa/testing/test.h>

#include <chrono>
#include <cstdio>

#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("editor_frame_cost");

namespace toy::editor::testing {

/**
 * @brief Editor frame cost on a large flat scene: nav_stress_demo's 758 root objects (500 rocks,
 *        246 agents). Per-frame editor work must stay linear in object count -- O(1) document
 *        lookups, cached mesh resolution, a virtualized outliner -- or the editor stops being
 *        usable on real levels long before the renderer cares.
 */
COOPA_TEST(large_scene_frames_stay_interactive) {
    setenv("FIXED_DT", "0", 1);
    setenv("NO_INPUT", "1", 1);
    use_scratch_home();
    const fs::path root = coopa::test::scratch_dir("large_scene_project");
    Project project = Project::create(root);
    const fs::path src = fs::path(ROOT_DIR) / "assets/scenes/navigation/nav_stress_demo/scene.yaml";
    const fs::path dst = project.assets() / "scenes/navigation/nav_stress_demo/scene.yaml";
    fs::create_directories(dst.parent_path());
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing);

    toy::core::Engine engine(shell_config(project), shell_options(project));
    using clock = std::chrono::steady_clock;
    auto ms_since = [](clock::time_point t) { return std::chrono::duration<double, std::milli>(clock::now() - t).count(); };
    auto t_open = clock::now();
    EditorApp app(engine, project, dst);
    tick(engine, 1);
    const double open_ms = ms_since(t_open);
    expect(app.document().all_ids().size() >= 758, "the large scene opened in the editor");
    tick(engine, 30);   // past the first frames' asset uploads

    const int frames = 30;
    auto t_idle = clock::now();
    tick(engine, frames);
    const double idle_ms = ms_since(t_idle) / frames;

    // With an object selected the Inspector (Relations, transform, components) is live too.
    const std::vector<ObjectId> ids = app.document().all_ids();
    app.document().select(ids[ids.size() / 2]);
    tick(engine, 2);
    auto t_sel = clock::now();
    tick(engine, frames);
    const double selected_ms = ms_since(t_sel) / frames;

    // Idle again, after the selection: steady state.
    app.document().clear_selection();
    tick(engine, 2);
    auto t_idle2 = clock::now();
    tick(engine, frames);
    const double idle2_ms = ms_since(t_idle2) / frames;

    // Play mode: the game (physics, 246 nav agents on two flow fields) under the editor.
    auto t_play = clock::now();
    app.play();
    tick(engine, 1);
    const double play_start_ms = ms_since(t_play);
    expect(app.playing(), "the large scene enters play mode");
    tick(engine, 30);   // the navmesh build and first flow fields
    auto t_run = clock::now();
    tick(engine, frames * 2);
    const double play_ms = ms_since(t_run) / (frames * 2);
    app.stop();
    tick(engine, 2);

    std::printf("  large scene (%zu objects): open %.0f ms, idle frame %.2f ms (again %.2f), selected frame %.2f ms, "
                "play start %.0f ms, play frame %.2f ms\n",
                ids.size(), open_ms, idle_ms, idle2_ms, selected_ms, play_start_ms, play_ms);
    expect(play_ms < 40.0, "the large scene plays at an interactive frame rate in the editor");
    // Generous ceilings: headless frames include the render (~10 ms here); the point is to catch
    // per-frame work going quadratic in object count again (that was ~800 ms per frame).
    expect(idle_ms < 25.0 && idle2_ms < 25.0, "an idle editor frame on a 758-object scene stays interactive");
    expect(selected_ms < 25.0, "an editor frame with a selection stays interactive");
}

} // namespace toy::editor::testing
