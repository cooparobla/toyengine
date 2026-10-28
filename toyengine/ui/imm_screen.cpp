#include <toyengine/ui/imm_screen.h>

#include <cstdio>
#include <exception>
#include <memory>
#include <filesystem>
#include <vector>

#include <coopa/job/context.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_commands.h>
#include <coopa/scene/scene_object.h>
#include <uicoopa/audio/ui_audio.h>
#include <uicoopa/immediate/imm_theme.h>
#include <uicoopa/text/font_defaults.h>

namespace toy::ui {
namespace {

/// The stock UI face, when a screen names none.
constexpr const char* k_default_font = "fonts/inter_regular.ttf";

/**
 * @brief A Font for `ref`, without needing the Engine.
 *
 * The process-wide fallback first (a host that already loaded a UI font has published it),
 * then `ref` against each asset root the engine put in FontDefaults::search_dirs. Loading goes
 * through FontDefaults::resolve_font, which the engine wires to its own shared font cache --
 * so two screens naming the same face share one atlas.
 */
coopa::ui::Font* resolve_font(const std::string& ref) {
    if (ref.empty() && coopa::ui::FontDefaults::font) return coopa::ui::FontDefaults::font;
    if (!coopa::ui::FontDefaults::resolve_font) return coopa::ui::FontDefaults::font;
    const std::string want = ref.empty() ? k_default_font : ref;
    for (const std::string& root : coopa::ui::FontDefaults::search_dirs) {
        const std::filesystem::path p = std::filesystem::path(root) / want;
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return coopa::ui::FontDefaults::resolve_font(p.string());
    }
    // An absolute or already-resolvable path, so a screen can name a face outside the roots.
    std::error_code ec;
    if (std::filesystem::exists(want, ec)) return coopa::ui::FontDefaults::resolve_font(want);
    return coopa::ui::FontDefaults::font;
}

/**
 * @brief Resolves a theme reference against the asset roots.
 *
 * `.yaml` is optional and `ui/themes/` is implied, so "game_dark", "ui/themes/game_dark" and
 * "ui/themes/game_dark.yaml" all name the same file -- a reference in a scene should not have to
 * know where themes are kept.
 */
std::string find_theme(const std::string& ref) {
    if (ref.empty()) return {};
    std::vector<std::string> tries;
    const bool has_ext = ref.size() > 5 && ref.rfind(".yaml") == ref.size() - 5;
    const std::string with_ext = has_ext ? ref : ref + ".yaml";
    tries.push_back(with_ext);
    if (with_ext.find('/') == std::string::npos) tries.push_back("ui/themes/" + with_ext);
    for (const std::string& rel : tries) {
        for (const std::string& root : coopa::ui::FontDefaults::search_dirs) {
            const std::filesystem::path p = std::filesystem::path(root) / rel;
            std::error_code ec;
            if (std::filesystem::exists(p, ec)) return p.string();
        }
    }
    std::error_code ec;
    if (std::filesystem::exists(with_ext, ec)) return with_ext;
    return {};
}

}  // namespace

coopa::ui::imm::Context* ImmScreen::context() {
    return canvas_ ? &canvas_->context() : nullptr;
}

ImmScreen::ImmScreen() : alive_(std::make_shared<ImmScreen*>(this)) {}

ImmScreen::~ImmScreen() {
    // Both the canvas callback and any still-queued build read through this, so clearing it
    // is what makes them safe -- the canvas can outlive the component when only the component
    // is removed.
    if (alive_) *alive_ = nullptr;
    canvas_ = nullptr;
}

void ImmScreen::start() {
    // Deliberately empty of scene mutation -- see the header. Subclasses still override this
    // to set their own fields (space, world_size) before the canvas is built.
}

void ImmScreen::update(float delta_time) {
    if (canvas_) {
        canvas_->frame_dt = delta_time;
        on_update(delta_time);
        return;
    }
    on_update(delta_time);
    // Building the canvas means adding a scene object, and every walk that could call us --
    // Scene::start(), BehaviourSystem::execute() -- iterates the very vector that would
    // reallocate. So ask the scene to do it at its next sync point instead. The flush applies
    // ops in order against root_objects_, not against the command queue, so the builder is
    // safe to run from inside it.
    if (pending_ || !owner || !scene) return;
    pending_ = true;
    scene->commands_for(coopa::job::k_main_thread_index).enqueue(
        [tok = alive_](coopa::scene::Scene&) {
            // A destroy queued after this one in the same buffer runs after it, so the screen
            // can already be gone by the time the build is applied.
            if (ImmScreen* self = *tok) self->ensure_canvas_();
        });
}

void ImmScreen::ensure_canvas_() {
    // Cleared on every exit without a canvas, so a screen that is detached and re-adopted
    // queues another build instead of staying blank for the rest of the process.
    pending_ = false;
    if (canvas_ || !owner || !scene) return;

    if (space == ImmSpace::World) {
        // Parented to this component's own object, so the plate inherits its transform and is
        // destroyed with it.
        canvas_ = coopa::ui::build_world_immediate_canvas(*scene, owner, type_name() + "UI", world_size,
                                                          pixels_per_unit, world_text_supersample,
                                                          world_billboard, world_occlude);
    } else {
        // Parented to this component's object too: a root-object canvas outlives its owner
        // and keeps calling on_draw. A screen-space canvas ignores the parent's 3D transform
        // (add_child() does not link it), so it stays window-relative.
        canvas_ = coopa::ui::build_immediate_canvas(*scene, type_name() + "UI", sort_order, owner);
    }
    if (!canvas_) return;

    // `hidden` is checked here rather than by the caller so toggling it takes effect on the
    // next frame with no rebuild: the callback stays installed and simply declares nothing.
    // Game UI is reachable without a mouse: Tab walks the widgets, Enter/Space activates.
    // Opt-in per Context, because a host with its own keymap (the editor binds Tab and Enter)
    // must not have those keys taken from it.
    canvas_->context().keyboard_focus = true;

    canvas_->on_draw = [tok = alive_](coopa::ui::imm::Context& ctx) {
        ImmScreen* self = *tok;
        if (self && !self->hidden) self->draw(ctx);
    };

    // The theme first: it may name the font, and font_ref overrides it rather than the other
    // way round, so a screen can borrow a theme's look with a face of its own.
    std::string font_want = font_ref;
    if (!theme_ref.empty()) {
        const std::string path = find_theme(theme_ref);
        if (path.empty()) {
            std::fprintf(stderr, "[toy::ui] %s: theme '%s' not found -- using the built-in look\n",
                         type_name().c_str(), theme_ref.c_str());
        } else {
            try {
                const coopa::ui::imm::Theme theme = coopa::ui::imm::load_theme(path);
                canvas_->context().style = theme.style;
                theme_ = game_theme_from(theme);
                // load_theme() has already resolved `font:` against the theme's own folder,
                // so this is an absolute path -- resolve_font() passes it straight through.
                if (font_want.empty() && !theme.font.empty()) font_want = theme.font;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[toy::ui] %s: theme '%s' failed to load (%s)\n",
                             type_name().c_str(), theme_ref.c_str(), e.what());
            }
        }
    }

    if (coopa::ui::Font* font = resolve_font(font_want)) {
        canvas_->context().text.set_font(font);
    } else {
        std::fprintf(stderr, "[toy::ui] %s: no UI font found (wanted '%s') -- it will draw no text\n",
                     type_name().c_str(), font_want.empty() ? k_default_font : font_want.c_str());
    }

    tunables_.load(tunables_ref.empty() ? "ui/" + tune_file_stem(type_name()) : tunables_ref);

#ifdef UICOOPA_HAS_AUDIO
    // UI cues, wired once for the whole screen: the layer reports what a widget just did and
    // this maps it to the host's named sounds. Silent when no UI audio is up.
    //
    // Hover is rate-limited for the reason the retained scheme limits it: sweeping the pointer
    // across a column of rows would otherwise fire a tick per row and read as a buzz. The
    // sound names are the ones the shipped library already defines.
    if (ui_sounds) {
        auto last_hover = std::make_shared<double>(-1.0);
        coopa::ui::imm::Context* ctx = &canvas_->context();
        ctx->on_sound = [ctx, last_hover](coopa::ui::imm::UiSound cue) {
            coopa::ui::UiAudio* audio = coopa::ui::UiAudio::active();
            if (!audio) return;
            switch (cue) {
                case coopa::ui::imm::UiSound::Hover: {
                    constexpr double k_min_interval = 0.04;
                    if (ctx->time() - *last_hover < k_min_interval) return;
                    *last_hover = ctx->time();
                    audio->play("ui_hover_tick");
                    break;
                }
                case coopa::ui::imm::UiSound::Press:   audio->play("ui_click_soft"); break;
                case coopa::ui::imm::UiSound::Release: break;   // the press already spoke
            }
        };
    }
#endif

    on_ready();
}

}  // namespace toy::ui
