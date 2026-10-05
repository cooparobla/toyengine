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
#include <toyengine/core/user_settings.h>

#include <coopa/scene/component.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>

#include <uicoopa/widgets/slider.h>

#include <cctype>
#include <cmath>
#include <string>

namespace toy::audio {

/** @brief The UserSettings key a bus's volume is stored under ("audio.music"). */
inline std::string volume_setting_key(const std::string& bus) {
    std::string k = "audio.";
    for (char c : bus) k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return k;
}

class VolumeBinding : public coopa::scene::Component {
public:
    std::string bus = k_bus_master;

    std::string type_name() const override { return "VolumeBinding"; }

    void update(float) override {
        AudioSystem* audio = AudioSystem::active();
        if (!audio || !owner) return;
        if (!slider_) {
            // A SettingRow builds its Slider when it starts, so look until one exists.
            auto sliders = owner->get_components_in_children<coopa::ui::Slider>();
            if (sliders.empty()) return;
            slider_ = sliders.front();
            last_ = audio->bus_volume(bus);
            slider_->set_value(last_, false);
            return;
        }
        const float v = slider_->value();
        if (std::fabs(v - last_) < 1e-4f) return;
        last_ = v;
        audio->set_bus_volume(bus, v);
        core::UserSettings::instance().set_float(volume_setting_key(bus), v);
    }

    coopa::ui::Slider* slider() const { return slider_; }

private:
    coopa::ui::Slider* slider_ = nullptr;
    float last_ = -1.0f;
};

/** @brief Registers VolumeBinding's scene parser. */
inline void register_audio_components() {
    coopa::scene::SceneLoader::register_component_parser(
        "VolumeBinding", [](const fkyaml::node& node, coopa::scene::SceneObject& obj, const coopa::scene::SceneLoader::ParseContext&) {
            auto* vb = obj.add_component<VolumeBinding>();
            if (node.contains("bus") && node.at("bus").is_string()) vb->bus = node.at("bus").get_value<std::string>();
        });
}

} // namespace toy::audio

#endif // TOYENGINE_AUDIO_AUDIO_COMPONENTS_H
