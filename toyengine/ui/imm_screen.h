/**
 * @file imm_screen.h
 * @brief Component base for a game interface drawn in immediate mode: a HUD, a menu, a plate
 *        floating over a creature.
 *
 * A screen is one override. `draw()` runs once per frame and declares the whole interface from
 * whatever the game's state happens to be right now, the way the editor's own panels do:
 *
 * @code
 * class CombatHud : public toy::ui::ImmScreen {
 * public:
 *     std::string type_name() const override { return "CombatHud"; }
 *     void draw(coopa::ui::imm::Context& ctx) override {
 *         using coopa::ui::imm::Anchor;
 *         const auto b = tune().box(ctx, "health", Anchor::TopLeft, {20.0f, 20.0f}, {260.0f, 28.0f});
 *         ctx.stat_bar("hp", b, "Health", player_->health, player_->max_health,
 *                      bar_style(theme(), theme().bars.health));
 *     }
 * };
 * @endcode
 *
 * There is nothing to bind and no widget names: a control sits directly beside the value it
 * edits, and a screen that changes shape between frames costs nothing extra. Compare
 * UiController (ui_assets.h), which binds game logic to a retained widget tree by name.
 *
 * ## Screen or world
 *
 * `space` picks which. A Screen panel is laid out in canvas pixels over the whole window; a
 * World one is a quad in the 3D scene, sized in canvas pixels and placed by its own object's
 * transform -- so parent the object to the thing the panel labels and it follows the body with
 * no per-frame projection to keep in step. Either way the canvas object is a CHILD of this
 * component's object and is destroyed with it; a screen-space canvas ignores the parent's 3D
 * transform and stays window-relative.
 *
 * World panels are not clickable: a host deliberately skips world canvases when deciding
 * whether the interface owns the pointer, because nameplates and damage numbers are not
 * buttons. Keep them to bars and labels; a dropdown on a small plate is clamped to the plate
 * and clipped rather than relocated.
 *
 * ## The font
 *
 * Resolved once in start() with no Engine pointer: the process-wide fallback if one is set,
 * else `fonts/inter_regular.ttf` from the asset roots the engine publishes through
 * FontDefaults::search_dirs. Override font_ref to name a different face. Without a font a
 * screen draws its boxes and no text at all, which is a confusing way to fail, so it says so
 * once.
 */

#ifndef TOYENGINE_UI_IMM_SCREEN_H
#define TOYENGINE_UI_IMM_SCREEN_H

#include <memory>
#include <string>

#include <coopa/scene/component.h>
#include <uicoopa/immediate/imm_canvas.h>

#include <toyengine/ui/game_theme.h>
#include <toyengine/ui/ui_tunables.h>

namespace toy::ui {

/** @brief Where an ImmScreen's canvas lives. */
enum class ImmSpace {
    Screen,   ///< Canvas pixels over the whole window, scaled by the player's interface scale.
    World,    ///< A quad in the 3D scene, placed by the owner object's transform.
};

/**
 * @class ImmScreen
 * @brief A game interface declared per frame in immediate mode.
 *
 * Add it to any object. start() builds the canvas and resolves a font; draw() is called once
 * per frame from the canvas's emit pass, so anything it reads is this frame's state and
 * anything it returns (a clicked button) can be acted on immediately.
 *
 * Restructuring the scene from inside draw() is not safe -- the scene is mid-walk. Queue it
 * (the scene's command buffer) and let it happen after the frame, exactly as the editor does.
 */
class ImmScreen : public coopa::scene::Component {
public:
    /** @brief Screen-space or in the world. Set before start(). */
    ImmSpace space = ImmSpace::Screen;

    /**
     * @brief Canvas draw order; higher draws over lower.
     *
     * The stock ladder leaves room between screens: hud 0, main menu 10, dialog 20, inventory
     * 30, settings 40, pause 50. The engine's own layers sit well above (the stats overlay at
     * 100, a transition fade below everything at -1000).
     */
    int sort_order = 0;

    /** @brief World space only: the authored design size in CANVAS PIXELS, not world units. */
    glm::vec2 world_size{320.0f, 64.0f};

    /** @brief World space only: world_size / this = the plate's extent in world units. */
    float pixels_per_unit = 120.0f;

    /**
     * @brief World space only: glyph-atlas density.
     *
     * A world plate has no fixed screen scale, so this is what keeps its text crisp when the
     * camera is close. Roughly the magnification you expect at the closest comfortable range.
     */
    float world_text_supersample = 2.0f;

    /**
     * @brief World space only: face the camera, or sit in the scene at the object's rotation.
     *
     * Facing is what a nameplate wants -- always square-on and always legible. Transform is
     * for a plate that is physically somewhere: a sign on a wall, readable from in front and
     * edge-on from the side.
     */
    coopa::ui::CanvasBillboard world_billboard = coopa::ui::CanvasBillboard::CameraFacing;

    /**
     * @brief World space only: let scene geometry hide the plate.
     *
     * Off for a nameplate, which should stay readable through a hill -- a label you cannot
     * see is worse than one that cheats. On for a plate that is physically on a surface.
     */
    bool world_occlude = false;

    /** @brief Font to draw with, relative to an asset root. Empty uses the stock UI face. */
    std::string font_ref;

    /**
     * @brief Theme to style this screen with ("ui/themes/game_dark"; extension optional).
     *
     * Loaded once in start(): its widget colours and metrics become the Context's Style, and
     * its `game:` section becomes theme(). Empty leaves the built-in Style, which is the
     * editor's own look -- fine for a debug panel, rarely what a game wants.
     *
     * A theme also names a font, which wins over font_ref when font_ref is empty.
     */
    std::string theme_ref;

    /** @brief Hide the screen without destroying it; draw() is not called while set. */
    bool hidden = false;

    /**
     * @brief Play UI cues ("ui_hover_tick", "ui_click_soft") as widgets are hovered and pressed.
     *
     * Resolved through the host's sound library. Silent if it defines neither, or if there is
     * no UI audio at all. Hover is rate-limited so sweeping across a column of rows ticks once
     * rather than once per row.
     */
    bool ui_sounds = true;

    /**
     * @brief Placement file for this screen ("ui/combat_hud"), or empty to derive it.
     *
     * Empty uses "ui/<type_name()>", which is what a screen wants unless two of them share a
     * layout. The file need not exist: every box a screen declares carries a code default,
     * and the file only ever holds what someone moved. See ui_tunables.h.
     */
    std::string tunables_ref;

    /** @brief Declares this frame's interface. The one thing a screen has to implement. */
    virtual void draw(coopa::ui::imm::Context& ctx) = 0;

    /** @brief The Context the callback receives -- style (the theme) and font live here. */
    coopa::ui::imm::Context* context();

    /** @brief The hosting canvas, for the few things that are set on it rather than per frame. */
    coopa::ui::ImmediateCanvas* canvas() { return canvas_; }

    /**
     * @brief This screen's game roles -- which colour health is, what an empty slot looks like.
     *
     * The defaults if theme_ref named nothing, so draw() can always use it. Pair it with
     * bar_style()/slot_style() rather than restating the mapping per screen.
     */
    const GameTheme& theme() const { return theme_; }

    /**
     * @brief This screen's draggable placement.
     *
     * Ask it for every panel's box -- `tune().box(ctx, "health", Anchor::TopLeft, {20, 20},
     * {260, 28})` -- rather than computing one inline. It costs the same, and it is what makes
     * the panel appear in the designer, draggable, with the position its author gave it.
     */
    Tunables& tune() { return tunables_; }
    const Tunables& tune() const { return tunables_; }

    /**
     * @brief Tears the screen down: nothing can call back into it after this returns.
     *
     * The canvas holds an `on_draw` callback into this screen, and for a screen-space canvas
     * that callback outlives a plain component removal. So both the callback and the queued
     * build hold a liveness token this clears, which turns them into no-ops rather than calls
     * into freed memory. The canvas OBJECT is a child of `owner` and is destroyed with it;
     * removing only the component therefore leaves an inert canvas behind that draws nothing
     * and dies with the object.
     */
    ~ImmScreen() override;

    ImmScreen(const ImmScreen&) = delete;
    ImmScreen& operator=(const ImmScreen&) = delete;

    /**
     * @brief Marks the screen as needing a canvas. Does NOT build one.
     *
     * Building it here would be a use-after-free: Scene::start() walks `root_objects_` by
     * reference and SceneObject::start() walks its children the same way, so adding the
     * canvas object mid-walk reallocates the very vector being iterated. The canvas is built
     * at the scene's next command flush instead -- the end of Scene::late_update(), which is
     * one whole frame later, since this frame's emit happens earlier in that same walk.
     */
    void start() override;

    /**
     * @brief Builds the canvas on the first call, feeds the Context this frame's dt, then
     *        calls on_update().
     *
     * FINAL on purpose. This is where the canvas gets built, so a subclass that overrode it
     * and forgot to chain would compile, run, and silently draw nothing -- which is exactly
     * what happened to EnvPresetSwitcher. Per-frame work goes in on_update() instead.
     */
    void update(float delta_time) final;

protected:
    /**
     * @brief Called once after the canvas, theme and font exist, before the first draw().
     *
     * Where a screen reads anything that needs context() -- it is not valid during start().
     */
    virtual void on_ready() {}

    /** @brief Per-frame work for a subclass. Runs after the canvas is up. */
    virtual void on_update(float delta_time) { (void)delta_time; }

protected:
    /** @brief Mints the liveness token the canvas callback and the queued build hold. */
    ImmScreen();

private:
    /** @brief Builds the canvas and applies the theme, font and sound. Idempotent. */
    void ensure_canvas_();

    coopa::ui::ImmediateCanvas* canvas_ = nullptr;
    /// A build has been queued with the scene but not yet flushed; stops queueing it twice.
    bool pending_ = false;
    /**
     * @brief Points at this screen while it lives, null once it is destroyed.
     *
     * Held by copy in the canvas's `on_draw` and in the queued build, so neither needs a raw
     * `this`. A raw capture is what made destroying a screen's object a pure-virtual call
     * through a canvas that had outlived it.
     */
    std::shared_ptr<ImmScreen*> alive_;
    GameTheme theme_;
    Tunables tunables_;
};

}  // namespace toy::ui

#endif  // TOYENGINE_UI_IMM_SCREEN_H
