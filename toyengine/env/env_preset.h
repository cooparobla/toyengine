/**
 * @file env_preset.h
 * @brief `EnvPresetSwitcher` -- a panel of UI buttons that switch the scene between authored
 *        environment presets, one at a time.
 *
 * A preset is two things: a subtree of the scene holding whatever that environment needs (its
 * `EnvInfluence`s, particle effects, props), and optionally a weather condition to blend the
 * global layer to. Picking one activates its subtree, deactivates every other preset's, and
 * moves the weather -- so a scene can offer "calm / rotor downwash / gale / sheltered" with
 * the content authored as plain objects.
 *
 * @code
 * - type: EnvPresetSwitcher
 *   start: calm                  # which preset is live on load (a name below)
 *   label_object: PresetLabel    # optional: a Text widget to show the active preset's blurb
 *   presets:
 *     - name: calm
 *       button: BtnCalm          # the UI object whose click selects this preset
 *       object: preset_calm      # the scene subtree to activate (empty = none)
 *       condition: still         # optional: blend the weather to this condition
 *       transition: 1.5          # optional: seconds (omit = the condition's own)
 *       blurb: "Nothing but the scene's own still air."
 *     - {name: gale, button: BtnGale, object: preset_none, condition: gale, blurb: "..."}
 * @endcode
 *
 * Buttons are addressed by the NAME of their UI object, through the scene's event bus (the
 * same channel `SignalReactor` listens on), so the panel can be a separate UI asset and the
 * switcher can sit anywhere in the scene.
 *
 * **Switching REPLAYS the preset.** Picking one restarts every Animator, ParticleSystem and
 * KinematicMover under it, so the effect always runs from its beginning rather than resuming
 * wherever it was left -- picking "Sword Swing" twice shows the swing twice.
 *
 * A preset authored `active: false` has never had its components started (SceneObject::start()
 * returns early on an inactive object, and set_active() does not run it later), so the first
 * activation starts the subtree before replaying it. Without that an authored-off preset is
 * inert: its influence works, but nothing in it animates.
 *
 * Why one component rather than a `SetActiveOnSignal` per button: presets are EXCLUSIVE.
 * Expressing "turn this one on and the other five off" with per-button reactors needs one
 * instance per (button, object) pair, and nothing would keep the weather in step.
 */

#ifndef TOYENGINE_ENV_ENV_PRESET_H
#define TOYENGINE_ENV_ENV_PRESET_H

#include <map>
#include <set>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/event/signal.h>
#include <coopa/scene/component.h>

namespace toy {
namespace env {

/** @brief One switchable environment. */
struct EnvPreset {
    std::string name;        ///< Identifies it (`start`, select_by_name(), the signal payload).
    std::string button;      ///< UI object whose "click" selects it; empty = no button.
    std::string object;      ///< Scene subtree to activate; empty = nothing to activate.
    std::string condition;   ///< Weather condition to blend to; empty = leave the weather alone.
    float transition = -1.0f;///< Seconds for that blend; < 0 = the condition's own.
    std::string blurb;       ///< One line shown in `label_object` while it is active.
};

/**
 * @class EnvPresetSwitcher
 * @brief See the file doc.
 */
class EnvPresetSwitcher : public coopa::scene::Component {
public:
    std::vector<EnvPreset> presets;
    std::string start_preset;  ///< Preset live on load; empty = the first.
    std::string label_object;  ///< Optional Text/ThemedText widget showing the active blurb.

    std::string type_name() const override { return "EnvPresetSwitcher"; }

    void start() override;

    /// Applies the initial preset on the first frame rather than in start(): the panel's
    /// buttons are built by a UI composite's own start(), which runs after this component's,
    /// so in start() there is nothing yet to highlight or write a blurb into.
    void update(float delta_time) override;

    /**
     * @brief Activates preset `index`, deactivating every other preset's subtree.
     *
     * Selecting the preset that is already live is NOT a no-op: it replays it. That is what
     * makes a button a "play this again" control rather than a dead one.
     */
    void select(int index);

    /**
     * @brief Activates the preset called `name`.
     * @param name The preset's name.
     * @return bool False when no preset has that name.
     */
    bool select_by_name(const std::string& name);

    /** @brief Index of the live preset, or -1 before start(). */
    int selected() const { return selected_; }

    /** @brief Fires when the live preset changes: its index and name. */
    coopa::event::Signal<int, const std::string&> on_selected;

private:
    void apply_(int index);
    /** @brief Starts `root`'s components the first time it is ever activated. */
    void ensure_started_(coopa::scene::SceneObject& root);
    /** @brief Replays every time-driven component under `root` from the beginning. */
    static void restart_subtree_(coopa::scene::SceneObject& root);

    int selected_ = -1;
    int initial_ = 0;
    bool pending_initial_ = true;
    std::vector<coopa::event::ScopedConnection> clicks_;
    std::set<const coopa::scene::SceneObject*> started_;
    /// Each button's authored `normal` colour, so the live one can be tinted and the rest
    /// restored without hardcoding a palette the theme owns.
    std::map<std::string, glm::vec4> button_base_;
};

} // namespace env
} // namespace toy

#endif // TOYENGINE_ENV_ENV_PRESET_H
