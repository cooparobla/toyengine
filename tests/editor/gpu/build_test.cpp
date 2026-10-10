/**
 * @file build_test.cpp
 * @brief Build: Build > Refresh's modal lists compiler errors, blocks while building and cancels; and a
 * real Development build of a fresh project runs relocated -- from an unrelated directory with a
 * scrubbed environment -- loading nothing from the source tree or Homebrew.
 */

#include <coopa/testing/test.h>

#include <chrono>
#include <sstream>
#include <thread>

#include "editor/build/build_pipeline.h"
#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("build");

namespace toy::editor::testing {

/** @brief Build > Refresh: a failed build lists its errors (modal and Console) and offers no
 *         relaunch, an up-to-date one succeeds quietly, and the modal blocks until cancelled. */
COOPA_TEST(build_refresh_lists_errors_blocks_and_cancels) {
    EditorSession session({.fixed_dt = "0", .settle_frames = 3});
    auto& engine = session.engine;
    auto& app = session.app;
    auto run = [&](const std::string& cmd) {
        app.build_refresh(cmd);
        tick(engine, 2);
        for (int i = 0; i < 400 && app.build_task().running(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); tick(engine, 1); }
        tick(engine, 2);
    };
    auto modal_open = [&] { return app.ui().is_popup_open("Build Project"); };
    expect(EditorApp::default_build_command().find("cmake --build") != std::string::npos &&
           EditorApp::default_build_command().find("--target") != std::string::npos,
           "Refresh builds this editor's own targets with cmake --build");

    // A failing build: blocking modal, errors listed and in the Console, no relaunch.
    run("echo '[ 50%] Building CXX object game.cpp.o'; echo '/p/src/game.cpp:12:5: error: no member named x'; exit 2");
    expect(modal_open(), "the build modal is up");
    expect(!app.build_task().succeeded() && app.build_diagnostics().size() == 1 && app.build_diagnostics()[0].line == 12,
           "a failed build lists its compiler errors");
    bool console = false;
    for (const auto& [lvl, msg] : app.log()) console |= lvl == 2 && msg.find("game.cpp:12") != std::string::npos;
    expect(console, "...and puts them in the Console for the traceback");
    expect(!app.build_relaunch_offered() && !app.relaunch_requested(), "a failed build offers no relaunch");
    dump(engine, "build_refresh_failed");

    // A build that leaves the binary as it was: up to date, nothing to relaunch.
    run("echo all up to date");
    expect(app.build_task().succeeded() && app.build_diagnostics().empty() && !app.build_relaunch_offered(),
           "an up-to-date build succeeds without offering a relaunch");

    // While building, the modal can't be dismissed: Escape leaves it up.
    app.build_refresh("sleep 2");
    tick(engine, 3);
    engine.queue_input([](coopa::input::Input& in) { in.push_key(coopa::input::Key::Escape, 0, coopa::input::KeyAction::Press, coopa::input::Mods::None); });
    tick(engine, 2);
    expect(modal_open() && app.build_task().running(), "the build modal blocks the editor while it builds");
    const_cast<Task&>(app.build_task()).cancel();
    for (int i = 0; i < 200 && app.build_task().running(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); tick(engine, 1); }
    tick(engine, 2);
    expect(app.build_task().cancelled(), "Cancel Build stops it");
}

/**
 * @brief The real thing: a Development build of a fresh project, launched from an unrelated
 *        directory with a scrubbed environment, must load nothing from the source tree or
 *        Homebrew.
 */
COOPA_TEST(a_development_build_runs_relocated) {
    use_scratch_home();
    // The game binary of the build tree this test was built in (a dependency of the test
    // target): beside this executable, or one directory up from tests/.
    fs::path game;
    for (const fs::path& dir : {current_executable().parent_path(), current_executable().parent_path().parent_path()}) {
        if (game.empty() && fs::exists(dir / "toyengine")) game = dir / "toyengine";
    }
    coopa::test::require(!game.empty(), "the toyengine game binary is built beside the tests (it is a dependency of the test target)");
    const fs::path root = coopa::test::scratch_dir("build_reloc_src");
    Project project = Project::create(root);
    const fs::path out = coopa::test::scratch_dir("build_reloc_out");
    BuildRequest req;
    req.profile = BuildProfile::Development;
    req.out_dir = out;
    req.skip_compile = true;
    req.binary_override = game;
    std::vector<std::string> log;
    CommandRunner run([&](const std::string& l) { log.push_back(l); });
    const BuildResult res = run_build(project, BuildSettings::load(project), req, BuildEnvironment::current(), run);
    if (!res.ok) for (const auto& l : log) std::cout << "    " << l << "\n";
    expect(res.ok, "the Development build succeeds (" + res.error + ")");
    if (!res.ok) return;
    expect(fs::exists(res.executable), "the packaged executable exists");
#if defined(__APPLE__)
    expect(res.artifact.extension() == ".app" && fs::exists(res.artifact / "Contents" / "Info.plist"), "a .app with an Info.plist");
    for (const auto& e : fs::directory_iterator(res.artifact / "Contents" / "Frameworks")) {
        for (const std::string& ref : parse_otool_L(run.run("otool -L " + shell_quote(e.path().string()), false).output)) {
            expect(is_system_dep(ref, DepPlatform::MacOS) || ref.rfind("@rpath/", 0) == 0,
                   e.path().filename().string() + " references only system libraries or @rpath (" + ref + ")");
        }
    }
    expect(run.run("codesign --verify --deep --strict " + shell_quote(res.artifact.string()), false).ok(), "the .app's signature verifies");
    expect(fs::exists(res.artifact / "Contents" / "Frameworks" / "libMoltenVK.dylib") &&
           fs::exists(res.artifact / "Contents" / "Resources" / "vulkan" / "icd.d" / "MoltenVK_icd.json"),
           "MoltenVK and its ICD manifest are bundled");
    const std::string trace = "DYLD_PRINT_LIBRARIES=1";
#else
    const std::string trace = "LD_DEBUG=libs";
#endif
    // Run it: a scrubbed environment, an unrelated working directory, its own HOME.
    const fs::path cwd = coopa::test::scratch_dir("build_reloc_cwd");
    const fs::path home = coopa::test::scratch_dir("build_reloc_home");
    const CommandResult r = run.run("cd " + shell_quote(cwd.string()) + " && env -i PATH=/usr/bin:/bin HOME=" + shell_quote(home.string()) +
                                    " HEADLESS=1 MAX_FRAMES=20 SFX_DEVICE=null " + trace + " " + shell_quote(res.executable.string()), false);
    if (r.exit_code != 0) std::cout << r.output.substr(r.output.size() > 4000 ? r.output.size() - 4000 : 0) << "\n";
    expect(r.exit_code == 0, "the packaged game runs relocated and exits cleanly");
    expect(r.output.find("Layout : packaged") != std::string::npos, "it detects its packaged layout");
    // Only the runtime's resolved roots line names asset/shader directories; none may be the checkout.
    std::istringstream lines(r.output);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find("Assets :") != std::string::npos || line.find("Shaders:") != std::string::npos) {
            expect(line.find(std::string(ROOT_DIR) + "/assets") == std::string::npos && line.find(std::string(PROJ_DIR) + "/gfxcoopa") == std::string::npos,
                   "no asset/shader root in the source tree: " + line);
        }
        if (line.find("dyld[") != std::string::npos || line.find("calling init") != std::string::npos) {
            expect(line.find("/opt/homebrew") == std::string::npos && line.find("/usr/local/") == std::string::npos,
                   "no library loads from Homebrew: " + line);
        }
    }
    expect(!fs::exists(cwd / "output"), "nothing is written into the working directory");
    bool logged = false;
    for (auto it = fs::recursive_directory_iterator(home); it != fs::recursive_directory_iterator(); ++it) {
        logged |= it->path().extension() == ".log";
    }
    expect(logged, "a log file is written under the player's HOME");
}

} // namespace toy::editor::testing
