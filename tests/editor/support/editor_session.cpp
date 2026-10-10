#include "editor/support/editor_session.h"

namespace toy {
namespace editor {
namespace testing {

void install_codecs_once() {
    static const bool installed = [] { toy::core::install_caml_codec(); return true; }();
    (void)installed;
}

toy::core::AppConfig shell_config(const Project& p) {
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

toy::core::EngineOptions shell_options(const Project& p) {
    toy::core::EngineOptions o;
    o.project_root = p.root();
    o.load_default_scene = false;
    o.edit_mode = true;
    o.escape_quits = false;
    return o;
}

void tick(toy::core::Engine& e, int n) { for (int i = 0; i < n; ++i) e.tick(); }

void dump(toy::core::Engine& engine, const std::string& name) {
    const char* dir = std::getenv("EDITOR_DUMP_DIR");
    if (!dir) return;
    fs::create_directories(dir);
    engine.save_screenshot((fs::path(dir) / (name + ".png")).string(), false);
}

Project new_project(const std::string& name) {
    use_scratch_home();
    return Project::create(coopa::test::scratch_dir(name));
}

void InputDriver::move(glm::vec2 canvas, int frames) {
    at = canvas;
    e.set_cursor_override(canvas * scale);
    tick(e, frames);
}

void InputDriver::drag(glm::vec2 to, coopa::input::MouseButton b, int steps) {
    using coopa::input::KeyAction;
    e.queue_input([b](coopa::input::Input& in) { in.push_mouse_button(b, KeyAction::Press, coopa::input::Mods::None); });
    tick(e, 1);
    const glm::vec2 from = at;
    for (int i = 1; i <= steps; ++i) move(glm::mix(from, to, i / float(steps)));
    e.queue_input([b](coopa::input::Input& in) { in.push_mouse_button(b, KeyAction::Release, coopa::input::Mods::None); });
    tick(e, 1);
}

void InputDriver::click(glm::vec2 canvas, coopa::input::MouseButton b,
           coopa::input::Mods mods) {
    using coopa::input::KeyAction;
    move(canvas);
    e.queue_input([b, mods](coopa::input::Input& in) { in.push_mouse_button(b, KeyAction::Press, mods); });
    tick(e, 1);
    e.queue_input([b, mods](coopa::input::Input& in) { in.push_mouse_button(b, KeyAction::Release, mods); });
    tick(e, 2);
}

void InputDriver::key(coopa::input::Key k, coopa::input::Mods mods) {
    using coopa::input::KeyAction;
    e.queue_input([k, mods](coopa::input::Input& in) { in.push_key(k, 0, KeyAction::Press, mods); });
    tick(e, 1);
    e.queue_input([k, mods](coopa::input::Input& in) { in.push_key(k, 0, KeyAction::Release, coopa::input::Mods::None); });
    tick(e, 1);
}

void InputDriver::type(const std::string& text) {
    for (char ch : text) {
        e.queue_input([ch](coopa::input::Input& i) { i.push_char(static_cast<uint32_t>(ch)); });
        tick(e, 1);
    }
}

Project EditorSession::prepared(const SessionOptions& o) {
    setenv("FIXED_DT", o.fixed_dt.c_str(), 1);
    setenv("NO_INPUT", "1", 1);   // gameplay input drivers push zeros: the desktop can't perturb a run
    Project p = new_project();
    if (o.prepare) {
        o.prepare(p);
        p.refresh();
    }
    return p;
}

ObjectId object_named(EditorApp& app, const std::string& name) {
    for (ObjectId id : app.document().all_ids()) if (get_string(*app.document().find(id), "name") == name) return id;
    return 0;
}

glm::mat4 world_of(EditorApp& app, ObjectId id) {
    auto* live = app.sync().live(id);
    return live && live->get_transform() ? live->get_transform()->transform().get_world_matrix() : glm::mat4(1.0f);
}

std::optional<glm::vec2> screen_of(toy::core::Engine& engine, EditorApp& app, ObjectId id, const glm::vec3& local,
                                          float scale) {
    glm::vec2 px;
    if (!engine.world_to_window(glm::vec3(world_of(app, id) * glm::vec4(local, 1.0f)), px)) return std::nullopt;
    return px / scale;
}

glm::vec2 visible_corner(EditorApp& app, ObjectId cube) {
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

void copy_rig_assets(const fs::path& dst) {
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

} // namespace testing
} // namespace editor
} // namespace toy
