/**
 * @file ui_panel.h
 * @brief A screen authored as YAML and loaded like any other asset.
 *
 * Most interfaces are one shape: a panel, optionally tabbed, holding a stack of labelled
 * rows, with a few buttons at the bottom. Settings, pause menus, stat readouts, debug panels.
 * Nothing about that shape needs a recompile, so it is data:
 *
 * @code{.yaml}
 * # assets/ui/settings.yaml
 * title: Settings
 * anchor: Center
 * size: [720, 560]
 * tabs:
 *   - name: Audio
 *     rows:
 *       - { kind: slider, label: Master volume, bind: audio.master, min: 0, max: 1 }
 *       - { kind: toggle, label: Subtitles,     bind: ui.subtitles }
 *       - { kind: combo,  label: Quality,       bind: render.quality, options: [Low, High] }
 * buttons:
 *   - { label: Close, action: close }
 * @endcode
 *
 * Draw it with PanelScreen (`- type: PanelScreen / panel: ui/settings`), or hand the loaded
 * UiPanel to `draw_panel()` from a C++ screen that wants to mix authored rows with its own.
 *
 * ## Where a row's value lives
 *
 * `bind` is a key, not a pointer. By default it reads and writes
 * toy::core::UserSettings -- which already persists -- so a settings screen works with no
 * game code at all. A game that wants a row to drive something else registers a provider for
 * that key (PanelBindings::provide), and the row goes through it instead.
 *
 * That indirection is the whole reason this can be data: a YAML file cannot hold a pointer to
 * a float, but it can hold the NAME of one.
 *
 * ## What this deliberately is not
 *
 * A general layout language. There are no nested containers, no anchoring per element, no
 * arbitrary composition -- a panel is a stack of rows, and anything that is not that shape
 * (a hotbar, a transport deck, a creature designer) is a C++ ImmScreen, which stays the
 * better tool for it. Reaching for arbitrary nesting here is how a UI format turns into a
 * worse programming language.
 */

#ifndef TOYENGINE_UI_UI_PANEL_H
#define TOYENGINE_UI_UI_PANEL_H

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <uicoopa/immediate/imm.h>

namespace coopa::asset { class AssetManager; }

namespace toy::ui {

/** @brief What a row shows. */
enum class RowKind {
    Label,    ///< Text only.
    Slider,   ///< A float between `min` and `max`.
    Toggle,   ///< A bool.
    Int,      ///< An integer drag between `min` and `max`.
    Combo,    ///< One of `options`, stored by NAME.
    Button,   ///< Fires `action`; no value.
    Separator,///< A rule between groups.
    Spacer,   ///< Blank space.
};

/** @brief One line of a panel. */
struct UiRow {
    RowKind kind = RowKind::Label;
    std::string label;
    std::string bind;                   ///< Value key; empty for Label/Button/Separator/Spacer.
    std::string action;                 ///< Button only: the action name reported to the host.
    std::string tooltip;
    float min = 0.0f;
    float max = 1.0f;
    /**
     * @brief Quantum the value snaps to -- Slider and Int. 0 means don't quantize.
     *
     * One meaning everywhere it appears. It is deliberately NOT a drag speed: `drag_int` takes
     * one, and feeding `step` to it made a row with the schema default (0.01) need ~20 px of
     * drag per unit, which reads as a stuck widget.
     */
    float step = 0.0f;
    std::vector<std::string> options;   ///< Combo only.

    /**
     * @brief What the row shows before anything has written its key -- `default:` in YAML.
     *
     * Without it a slider would fall back to `min`, so an unwritten `audio.master` reads 0.00
     * against a real volume of 1.0 and the first nudge persists silence; and a toggle would
     * fall back to off, so one could never default to on. Exactly one of these is used, chosen
     * by `kind`; unset means min (Slider/Int), off (Toggle) or the first option (Combo).
     */
    std::optional<float> default_num;       ///< Slider, Int.
    std::optional<bool> default_on;         ///< Toggle.
    std::optional<std::string> default_option;  ///< Combo: an option NAME.
};

/** @brief A named group of rows, drawn as one tab. A panel with no tabs has exactly one. */
struct UiTab {
    std::string name;
    std::vector<UiRow> rows;
};

/**
 * @struct UiPanel
 * @brief A whole authored screen. The payload the asset system publishes.
 */
struct UiPanel {
    std::string title;                      ///< Frame heading; empty draws no frame.
    coopa::ui::imm::Anchor anchor = coopa::ui::imm::Anchor::Center;
    glm::vec2 offset{0.0f};
    glm::vec2 size{640.0f, 480.0f};
    std::string theme;                      ///< Theme ref; empty uses the host's.
    bool closable = false;                  ///< Draw a close button; reports the "close" action.
    std::vector<UiTab> tabs;
    std::vector<UiRow> buttons;             ///< A row of buttons along the bottom.
};

/**
 * @class PanelBindings
 * @brief Resolves a row's `bind` key to a value, and its `action` to something that happens.
 *
 * Unbound keys fall through to UserSettings, so an authored panel is useful before any game
 * code exists. A game overrides the ones it cares about.
 */
class PanelBindings {
public:
    using FloatFn = std::function<float()>;
    using SetFloatFn = std::function<void(float)>;
    using BoolFn = std::function<bool()>;
    using SetBoolFn = std::function<void(bool)>;
    using IntFn = std::function<int()>;
    using SetIntFn = std::function<void(int)>;
    using TextFn = std::function<std::string()>;
    using SetTextFn = std::function<void(const std::string&)>;
    using ActionFn = std::function<void()>;

    // One provide()/get()/set() per type, mirroring UserSettings' own typed accessors --
    // because a key there KEEPS the type it was written with: get_bool() on a key written by
    // set_float() returns nothing. A float-only binding therefore stored every toggle and
    // every combo as a number, which any engine code reading it with get_bool()/get_string()
    // silently saw as unset.

    /** @brief Routes a Slider row's `key` to a game value instead of UserSettings. */
    void provide(const std::string& key, FloatFn get, SetFloatFn set);

    /** @brief Routes a Toggle row's `key`. */
    void provide_bool(const std::string& key, BoolFn get, SetBoolFn set);

    /** @brief Routes an Int row's `key`. */
    void provide_int(const std::string& key, IntFn get, SetIntFn set);

    /** @brief Routes a Combo row's `key`, whose value is the chosen option's NAME. */
    void provide_text(const std::string& key, TextFn get, SetTextFn set);

    /** @brief What a Button row with this `action` does. */
    void on_action(const std::string& action, ActionFn fn);

    /** @brief The value for `key`: a provider if one is registered, else UserSettings. */
    float get(const std::string& key, float fallback = 0.0f) const;
    bool get_bool(const std::string& key, bool fallback = false) const;
    int get_int(const std::string& key, int fallback = 0) const;
    std::string get_text(const std::string& key, const std::string& fallback = {}) const;

    /** @brief Writes `key` through its provider, else to UserSettings (which persists it). */
    void set(const std::string& key, float value);
    void set_bool(const std::string& key, bool value);
    void set_int(const std::string& key, int value);
    void set_text(const std::string& key, const std::string& value);

    /** @brief Runs `action`'s handler. Returns false if nothing is registered for it. */
    bool invoke(const std::string& action) const;

    /** @brief Whether anything is registered for `key` -- what distinguishes a live binding
     *         from a typo, since UserSettings will happily invent either. */
    bool provided(const std::string& key) const;

private:
    struct Binding {
        FloatFn get_f; SetFloatFn set_f;
        BoolFn get_b; SetBoolFn set_b;
        IntFn get_i; SetIntFn set_i;
        TextFn get_t; SetTextFn set_t;
    };
    std::unordered_map<std::string, Binding> values_;
    std::unordered_map<std::string, ActionFn> actions_;
};

/**
 * @brief Draws `panel` into `b` and routes every edit through `bindings`.
 *
 * @param active_tab Which tab is shown; the caller owns it so the panel keeps its page
 *                   between frames without the asset being mutable.
 * @return The action name a button fired this frame, or empty.
 */
std::string draw_panel(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& b,
                       const UiPanel& panel, PanelBindings& bindings, int* active_tab);

/** @brief Parses a panel from YAML text. Throws std::runtime_error on malformed input. */
UiPanel parse_panel(const std::string& yaml_text);

/**
 * @brief The `bind` keys in `panel` that nothing will consume.
 *
 * A key with no provider and no engine consumer still reads and writes UserSettings, so the
 * row looks like it works and silently does nothing -- while permanently adding the key to the
 * player's settings file. This is what the editor shows in the Panel tab and what the shipped
 * panels are tested against, since neither can be caught by parsing alone.
 */
std::vector<std::string> unbound_keys(const UiPanel& panel, const PanelBindings& bindings);

/** @brief Registers the UiPanel asset loader, so `assets.load<UiPanel>("ui/settings")` works. */
void register_panel_loader(coopa::asset::AssetManager& assets);

/**
 * @brief The AssetManager a PanelScreen loads its panel through. Engine sets it at startup.
 *
 * A component has no route to the asset system -- ParseContext carries none, and the
 * installer-function pattern (`install_x_system(scene, assets, jobs)`) does not fit a plain
 * behaviour -- so this is the seam. With it set, a panel is cached (N screens on one file
 * parse it once) and hot-reloads while the editor previews it; without it, PanelScreen reads
 * the file directly, which is what the headless unit tests rely on.
 *
 * Non-owning: the manager must outlive every PanelScreen, which Engine's ordering guarantees.
 */
void set_panel_assets(coopa::asset::AssetManager* assets);

/** @brief See set_panel_assets(). Null until a host sets one. */
coopa::asset::AssetManager* panel_assets();

}  // namespace toy::ui

#endif  // TOYENGINE_UI_UI_PANEL_H
