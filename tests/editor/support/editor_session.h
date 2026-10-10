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
inline void install_codecs_once() {
    static const bool installed = [] { toy::core::install_caml_codec(); return true; }();
    (void)installed;
}

/** @brief The editor's engine config for `p`: invisible, no vsync, temporal resolves off. */
inline toy::core::AppConfig shell_config(const Project& p) {
    install_codecs_once();
    toy::core::AppConfig cfg = toy::core::AppConfig::load(p.config_path().string());
    cfg.window.width = 1280;
    cfg.window.height = 760;
    cfg.window.visible = false;
    cfg.window.vsync = false;
    cfg.render.screen_ui_enabled = true;
    cfg.render.ssao_temporal_enabled = false;
    cfg.render.ssr_temporal_enabled = false;
    cfg.output.save_on_exit = false;
    apply_editor_render_overrides(cfg);
    return cfg;
}

/** @brief Engine options as the editor sets them: edit mode, no default scene, Esc never quits. */
inline toy::core::EngineOptions shell_options(const Project& p) {
    toy::core::EngineOptions o;
    o.project_root = p.root();
    o.load_default_scene = false;
    o.edit_mode = true;
    o.escape_quits = false;
    return o;
}

inline void tick(toy::core::Engine& e, int n) { for (int i = 0; i < n; ++i) e.tick(); }

/** @brief Saves the frame as <EDITOR_DUMP_DIR>/<name>.png when that is set (for a look). */
inline void dump(toy::core::Engine& engine, const std::string& name) {
    const char* dir = std::getenv("EDITOR_DUMP_DIR");
    if (!dir) return;
    fs::create_directories(dir);
    engine.save_screenshot((fs::path(dir) / (name + ".png")).string(), false);
}

/** @brief A fresh project in the test's scratch directory, with HOME isolated (use_scratch_home). */
inline Project new_project(const std::string& name = "project") {
    use_scratch_home();
    return Project::create(coopa::test::scratch_dir(name));
}

/**
 * @brief Drives the editor through REAL input events (Input::push_mouse_button/push_key via
 *        Engine::queue_input, cursor via set_cursor_override) -- the same path GLFW events take.
 */
struct InputDriver {
    toy::core::Engine& e;
    float scale;   // canvas pixels -> cursor (framebuffer) pixels
    glm::vec2 at{0.0f};

    void move(glm::vec2 canvas, int frames = 1) {
        at = canvas;
        e.set_cursor_override(canvas * scale);
        tick(e, frames);
    }
    void drag(glm::vec2 to, coopa::input::MouseButton b, int steps = 6) {
        using coopa::input::KeyAction;
        e.queue_input([b](coopa::input::Input& in) { in.push_mouse_button(b, KeyAction::Press, coopa::input::Mods::None); });
        tick(e, 1);
        const glm::vec2 from = at;
        for (int i = 1; i <= steps; ++i) move(glm::mix(from, to, i / float(steps)));
        e.queue_input([b](coopa::input::Input& in) { in.push_mouse_button(b, KeyAction::Release, coopa::input::Mods::None); });
        tick(e, 1);
    }
    void click(glm::vec2 canvas, coopa::input::MouseButton b = coopa::input::MouseButton::Left,
               coopa::input::Mods mods = coopa::input::Mods::None) {
        using coopa::input::KeyAction;
        move(canvas);
        e.queue_input([b, mods](coopa::input::Input& in) { in.push_mouse_button(b, KeyAction::Press, mods); });
        tick(e, 1);
        e.queue_input([b, mods](coopa::input::Input& in) { in.push_mouse_button(b, KeyAction::Release, mods); });
        tick(e, 2);
    }
    void key(coopa::input::Key k, coopa::input::Mods mods = coopa::input::Mods::None) {
        using coopa::input::KeyAction;
        e.queue_input([k, mods](coopa::input::Input& in) { in.push_key(k, 0, KeyAction::Press, mods); });
        tick(e, 1);
        e.queue_input([k, mods](coopa::input::Input& in) { in.push_key(k, 0, KeyAction::Release, coopa::input::Mods::None); });
        tick(e, 1);
    }
    /** @brief Types `text` into whatever has keyboard focus, one character per frame. */
    void type(const std::string& text) {
        for (char ch : text) {
            e.queue_input([ch](coopa::input::Input& i) { i.push_char(static_cast<uint32_t>(ch)); });
            tick(e, 1);
        }
    }
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
    static Project prepared(const SessionOptions& o) {
        setenv("FIXED_DT", o.fixed_dt.c_str(), 1);
        setenv("NO_INPUT", "1", 1);   // gameplay input drivers push zeros: the desktop can't perturb a run
        Project p = new_project();
        if (o.prepare) {
            o.prepare(p);
            p.refresh();
        }
        return p;
    }
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
inline ObjectId object_named(EditorApp& app, const std::string& name) {
    for (ObjectId id : app.document().all_ids()) if (get_string(*app.document().find(id), "name") == name) return id;
    return 0;
}

inline ObjectId EditorSession::named(const std::string& name) const { return object_named(app, name); }

/** @brief The live object's world matrix (identity when it has none). */
inline glm::mat4 world_of(EditorApp& app, ObjectId id) {
    auto* live = app.sync().live(id);
    return live && live->get_transform() ? live->get_transform()->transform().get_world_matrix() : glm::mat4(1.0f);
}

/** @brief Window point (canvas px) of a mesh-local point on `id`, or nullopt. */
inline std::optional<glm::vec2> screen_of(toy::core::Engine& engine, EditorApp& app, ObjectId id, const glm::vec3& local,
                                          float scale) {
    glm::vec2 px;
    if (!engine.world_to_window(glm::vec3(world_of(app, id) * glm::vec4(local, 1.0f)), px)) return std::nullopt;
    return px / scale;
}

/** @brief The cube corner (x, y) nearest the camera, so its vertical edge is visible. */
inline glm::vec2 visible_corner(EditorApp& app, ObjectId cube) {
    const glm::mat4 view = coopa::gfx::engine::components::CameraComponent::main()->get_view_matrix();
    const glm::vec3 eye = glm::vec3(glm::inverse(view)[3]);
    const glm::mat4 w = world_of(app, cube);
    glm::vec2 best(0.5f);
    float bd = 1e30f;
    for (float x : {-0.5f, 0.5f}) for (float y : {-0.5f, 0.5f}) {
        const float d = glm::distance(glm::vec3(w * glm::vec4(x, y, 0, 1)), eye);
        if (d < bd) { bd = d; best = {x, y}; }
    }
    return best;
}

/**
 * @brief Copies toyengine's animation test rigs into a project's assets/, untagged: the rig
 *        object assets (objects/animation/ -> objects/), their clips, and the meshes they use.
 */
inline void copy_rig_assets(const fs::path& dst) {
    const fs::path src = fs::path(ROOT_DIR) / "assets";
    fs::create_directories(dst / "objects");
    for (const auto& e : fs::directory_iterator(src / "objects" / "animation")) {
        fs::copy_file(e.path(), dst / "objects" / e.path().filename(), fs::copy_options::overwrite_existing);
    }
    fs::copy(src / "animations", dst / "animations", fs::copy_options::recursive);
    for (const char* m : {"animation/tentacle.yaml", "primitives/ball.yaml"}) {
        fs::copy_file(src / "meshes" / m, dst / "meshes" / fs::path(m).filename(), fs::copy_options::overwrite_existing);
    }
}

} // namespace toy::editor::testing
