#include <toyengine/audio/audio_components.h>

#include <coopa/scene/component.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <toyengine/core/user_settings.h>

namespace toy {
namespace audio {

std::string volume_setting_key(const std::string& bus) {
    std::string k = "audio.";
    for (char c : bus) k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return k;
}

void VolumeBinding::update(float) {
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

void register_audio_components() {
    coopa::scene::SceneLoader::register_component_parser(
        "VolumeBinding", [](const fkyaml::node& node, coopa::scene::SceneObject& obj, const coopa::scene::SceneLoader::ParseContext&) {
            auto* vb = obj.add_component<VolumeBinding>();
            if (node.contains("bus") && node.at("bus").is_string()) vb->bus = node.at("bus").get_value<std::string>();
        });
}

} // namespace audio
} // namespace toy
