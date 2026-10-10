#pragma once

/**
 * @file editor_session.h
 * @brief The whole editor, headless, against a scratch project: what every gpu suite builds on.
 *
 * An EditorSession is one Engine (an invisible 1280 x 760 window, never shown, never grabbing
 * input) plus one EditorApp over a fresh Project::create()d project in the test's scratch
 * directory, with HOME pointed at scratch too -- so preferences, recent projects and themes start
 * from nothing and nothing here writes into the repository or the real home directory.
 *
 * Input goes through InputDriver: the same Input::push_* path GLFW events take, with the cursor
 * placed by Engine::set_cursor_override(), so hover offsets, click routing and the Blender keymap
 * are exercised for real.
 *
 * EDITOR_DUMP_DIR=<dir> makes dump() save the named UI screenshots for a look; without it dump()
 * does nothing.
 */

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <toyengine/core/caml_codec.h>
#include <toyengine/core/engine.h>

#include "editor/app/editor_app.h"
#include "editor/support/fixtures.h"

namespace toy::editor::testing {

/** @brief Registers the .caml decoder (what the editor's and the game's main() do), once. */
void install_codecs_once();

/** @brief The editor's engine config for `p`: invisible, no vsync, temporal resolves off. */
toy::core::AppConfig shell_config(const Project& p);

/** @brief Engine options as the editor sets them: edit mode, no default scene, Esc never quits. */
toy::core::EngineOptions shell_options(const Project& p);

void tick(toy::core::Engine& e, int n);

/** @brief Saves the frame as <EDITOR_DUMP_DIR>/<name>.png when that is set (for a look). */
void dump(toy::core::Engine& engine, const std::string& name);

/** @brief A fresh project in the test's scratch directory, with HOME isolated (use_scratch_home). */
Project new_project(const std::string& name = "project");

/**
 * @brief Drives the editor through REAL input events (Input::push_mouse_button/push_key via
 *        Engine::queue_input, cursor via set_cursor_override) -- the same path GLFW events take.
 */
struct InputDriver {
    toy::core::Engine& e;
    float scale;   // canvas pixels -> cursor (framebuffer) pixels
    glm::vec2 at{0.0f};

    void move(glm::vec2 canvas, int frames = 1);
    void drag(glm::vec2 to, coopa::input::MouseButton b, int steps = 6);
    void click(glm::vec2 canvas, coopa::input::MouseButton b = coopa::input::MouseButton::Left,
               coopa::input::Mods mods = coopa::input::Mods::None);
    void key(coopa::input::Key k, coopa::input::Mods mods = coopa::input::Mods::None);
    /** @brief Types `text` into whatever has keyboard focus, one character per frame. */
    void type(const std::string& text);
};

/** @brief How an EditorSession is set up. */
struct SessionOptions {
    /** FIXED_DT for the engine: "0" freezes time (exact A/B captures), 1/60 lets things move. */
    std::string fixed_dt = "0.016666";
    /** Frames ticked before the test starts (kFillDebounceFrames + 4 lets the viewport's
     *  fill-mode pipeline rebuild settle first). */
    int settle_frames = 4;
    /** Runs on the fresh project before the Engine exists: copy assets in, write files. */
    std::function<void(Project&)> prepare;
};

/**
 * @brief One editor over a fresh scratch project. The app goes before the engine on the way out
 *        (member order), as in editor.cpp.
 */
struct EditorSession {
    Project project;
    std::unique_ptr<toy::core::Engine> engine_;
    std::unique_ptr<EditorApp> app_;
    toy::core::Engine& engine;
    EditorApp& app;
    InputDriver in;

    explicit EditorSession(const SessionOptions& o = {}) : EditorSession(o, prepared(o)) {}

    /** @brief The id of the first document object named `name`, or 0. */
    ObjectId named(const std::string& name) const;

private:
    static Project prepared(const SessionOptions& o);
    EditorSession(const SessionOptions& o, Project p)
        : project(std::move(p)),
          engine_(std::make_unique<toy::core::Engine>(shell_config(project), shell_options(project))),
          app_(std::make_unique<EditorApp>(*engine_, project)),
          engine(*engine_),
          app(*app_),
          in{*engine_, std::max(1.0f, engine_->display_scale())} {
        tick(engine, o.settle_frames);
    }
};

/** @brief The id of the first document object named `name`, or 0. */
ObjectId object_named(EditorApp& app, const std::string& name);

inline ObjectId EditorSession::named(const std::string& name) const { return object_named(app, name); }

/** @brief The live object's world matrix (identity when it has none). */
glm::mat4 world_of(EditorApp& app, ObjectId id);

/** @brief Window point (canvas px) of a mesh-local point on `id`, or nullopt. */
std::optional<glm::vec2> screen_of(toy::core::Engine& engine, EditorApp& app, ObjectId id, const glm::vec3& local,
                                          float scale);

/** @brief The cube corner (x, y) nearest the camera, so its vertical edge is visible. */
glm::vec2 visible_corner(EditorApp& app, ObjectId cube);

/**
 * @brief Copies toyengine's animation test rigs into a project's assets/, untagged: the rig
 *        object assets (objects/animation/ -> objects/), their clips, and the meshes they use.
 */
void copy_rig_assets(const fs::path& dst);

} // namespace toy::editor::testing
