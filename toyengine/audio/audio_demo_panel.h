/**
 * @file audio_demo_panel.h
 * @brief The audio demo's control panel, declared per frame in immediate mode.
 *
 * Four pages over the audio system: music transport and a playlist, spatial sources you can
 * switch on and walk around, the named UI sounds, and the mixer's buses. It exists to make
 * every audio feature reachable without a keyboard shortcut per feature.
 *
 * It replaces a 432-line UI asset and the 59 signal reactors that wired it: a reactor pattern
 * exists so authored YAML can reach game code by widget NAME, and a panel declared in code
 * calls the audio API directly instead -- `if (ctx.button("Next")) music.next(fade)`. That is
 * the whole difference, and it is why this is one file.
 *
 * Add it to any object in a scene:
 * @code{.yaml}
 * - type: AudioDemoPanel
 *   playlist_object: MusicDirector   # the object carrying a MusicPlaylistComponent
 *   theme_ref: ui/themes/game_dark
 * @endcode
 */

#ifndef TOYENGINE_AUDIO_AUDIO_DEMO_PANEL_H
#define TOYENGINE_AUDIO_AUDIO_DEMO_PANEL_H

#include <string>
#include <vector>

#include <toyengine/ui/imm_screen.h>

namespace toy::audio {

/**
 * @class AudioDemoPanel
 * @brief A tabbed panel over the audio system, for the audio demo scene.
 */
class AudioDemoPanel : public toy::ui::ImmScreen {
public:
    std::string type_name() const override { return "AudioDemoPanel"; }

    /** @brief Object carrying the MusicPlaylistComponent the "Restart playlist" button drives. */
    std::string playlist_object;

    /** @brief Crossfade for the direct play buttons, in seconds. */
    float fade_seconds = 2.5f;

    /** @brief Page shown first: 0 Music, 1 Spatial, 2 UI Sounds, 3 Mixer. */
    int start_page = 0;

    void draw(coopa::ui::imm::Context& ctx) override;

    /** @brief One entry of a name table: what the panel calls it, and what it drives. */
    struct Entry {
        const char* label;
        const char* name;   ///< Scene object (sources, pings), sound name, or bus name.
    };

    // The panel's tables, exposed so a test can check every object it drives actually exists
    // in the demo scene. A dead button used to be caught by matching reactor names against
    // the authored UI; with the panel in code, this is what replaces that guarantee.

    /** @brief Spatial sources the Spatial page toggles, by scene object name. */
    static const std::vector<Entry>& sources();
    /** @brief One-shot directional pings, by scene object name. */
    static const std::vector<Entry>& pings();
    /** @brief Named UI sounds the UI Sounds page plays, by SoundLibrary name. */
    static const std::vector<Entry>& sounds();
    /** @brief Mixer buses, by bus name. */
    static const std::vector<Entry>& buses();

protected:
    void on_ready() override { page_ = start_page; }

private:
    void draw_music_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& body);
    void draw_spatial_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& body);
    void draw_sounds_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& body);
    void draw_mixer_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& body);

    /**
     * @brief Toggles a named AudioSource, reading its own is_playing() as the switch state.
     *
     * The source is the state -- there is no mirror to keep in step, so a source stopped by
     * anything else shows as off here on the next frame.
     */
    void source_toggle_(coopa::ui::imm::Context& ctx, const char* label, const char* object);

    /** @brief A button that plays a named AudioSource once. */
    void source_button_(coopa::ui::imm::Context& ctx, const coopa::ui::imm::Box& b,
                        const char* label, const char* object);

    int page_ = 0;
};

}  // namespace toy::audio

#endif  // TOYENGINE_AUDIO_AUDIO_DEMO_PANEL_H
