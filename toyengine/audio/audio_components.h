/**
 * @file audio_components.h
 * @brief Game-side audio components that need the engine's AudioSystem (not just sfxcoopa).
 *
 *   VolumeBinding   ties a UI Slider (on this object or a descendant -- e.g. a SettingRow's)
 *                   to a mixer bus: the slider starts at the bus's current volume, moving it
 *                   sets the bus and remembers the value in the player's settings
 *                   (UserSettings "audio.<bus>"), so it is restored next launch.
 *
 * @code
 * - type: SettingRow
 *   kind: slider
 *   label: Music
 *   min: 0
 *   max: 1
 * - {type: VolumeBinding, bus: Music}
 * @endcode
 */

#ifndef TOYENGINE_AUDIO_AUDIO_COMPONENTS_H
#define TOYENGINE_AUDIO_AUDIO_COMPONENTS_H

#include <toyengine/audio/audio_system.h>


#include <uicoopa/widgets/slider.h>

#include <cctype>
#include <cmath>
#include <string>

namespace toy::audio {

/** @brief The UserSettings key a bus's volume is stored under ("audio.music"). */
std::string volume_setting_key(const std::string& bus);

class VolumeBinding : public coopa::scene::Component {
public:
    std::string bus = k_bus_master;

    std::string type_name() const override { return "VolumeBinding"; }

    void update(float) override;

    coopa::ui::Slider* slider() const { return slider_; }

private:
    coopa::ui::Slider* slider_ = nullptr;
    float last_ = -1.0f;
};

/** @brief Registers VolumeBinding's scene parser. */
void register_audio_components();

} // namespace toy::audio

#endif // TOYENGINE_AUDIO_AUDIO_COMPONENTS_H
