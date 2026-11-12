#include <toyengine/weather/weather_profile.h>

namespace toy {
namespace weather {

const Condition* Settings::find(const std::string& n) const {
    for (const auto& c : conditions) if (c.name == n) return &c;
    return nullptr;
}

int Settings::index_of(const std::string& n) const {
    for (size_t i = 0; i < conditions.size(); ++i) if (conditions[i].name == n) return static_cast<int>(i);
    return -1;
}

const std::vector<std::string>& controlled_render_keys() {
    static const std::vector<std::string> keys = {
        "ambient_intensity", "sky_intensity", "sky_zenith", "sky_horizon", "sky_ground",
        "fog_mode", "fog_density", "fog_linear_start", "fog_linear_end", "fog_color", "fog_sky_blend",
        "fog_max_opacity", "fog_height_falloff", "fog_sun_amount", "cloud_coverage",
    };
    return keys;
}

bool controls_render_key(const std::string& key) {
    const auto& k = controlled_render_keys();
    return std::find(k.begin(), k.end(), key) != k.end();
}

} // namespace weather
} // namespace toy

namespace toy {
namespace weather {
namespace detail {

EffectSpec effect(std::string prefab, float intensity, EffectAnchor follow, glm::vec3 offset, float wind) {
    EffectSpec e;
    e.prefab = std::move(prefab);
    e.intensity = intensity;
    e.follow = follow;
    e.offset = offset;
    e.wind_influence = wind;
    return e;
}

} // namespace detail
} // namespace weather
} // namespace toy

namespace toy {
namespace weather {

std::vector<Condition> default_conditions() {
    using detail::effect;
    std::vector<Condition> out;
    {
        Condition c; c.name = "clear";
        c.weight = 3.0f; c.min_minutes = 5; c.max_minutes = 12; c.transition = 30;
        c.cloud_cover = 0.05f; c.sun = 1.0f; c.ambient = 1.0f;
        c.fog_density = 0.0026f; c.fog_color = {0.70f, 0.78f, 0.88f}; c.fog_sky_blend = 0.7f; c.fog_sun_amount = 0.5f;
        c.wind_strength = 1.5f; c.wind_gust = 0.2f; c.temperature = 21;
        out.push_back(c);
    }
    {
        Condition c; c.name = "cloudy";
        c.weight = 2.0f; c.min_minutes = 4; c.max_minutes = 9; c.transition = 30;
        c.cloud_cover = 0.4f; c.sun = 0.8f; c.ambient = 0.95f;
        c.fog_density = 0.0043f; c.fog_color = {0.68f, 0.73f, 0.80f}; c.fog_sky_blend = 0.6f; c.fog_sun_amount = 0.35f;
        c.wind_strength = 3.0f; c.wind_gust = 0.3f; c.temperature = 17;
        out.push_back(c);
    }
    {
        Condition c; c.name = "overcast";
        c.weight = 1.5f; c.min_minutes = 3; c.max_minutes = 8; c.transition = 35;
        c.next = {"cloudy", "rain", "fog", "overcast"};
        c.cloud_cover = 0.85f; c.sun = 0.35f; c.ambient = 0.85f; c.sky_tint = {0.92f, 0.94f, 0.97f};
        c.fog_density = 0.0068f; c.fog_color = {0.60f, 0.63f, 0.67f}; c.fog_sky_blend = 0.5f; c.fog_sun_amount = 0.1f;
        c.wind_strength = 4.0f; c.wind_gust = 0.35f; c.temperature = 14; c.wetness = 0.05f;
        out.push_back(c);
    }
    {
        Condition c; c.name = "fog";
        c.weight = 0.8f; c.min_minutes = 3; c.max_minutes = 6; c.transition = 45;
        c.next = {"overcast", "cloudy", "clear"};
        c.cloud_cover = 0.7f; c.sun = 0.45f; c.ambient = 0.9f; c.sky_tint = {0.95f, 0.96f, 0.98f};
        c.fog_density = 0.0382f; c.fog_color = {0.70f, 0.72f, 0.75f}; c.fog_height_falloff = 12.0f;
        c.fog_sky_blend = 0.2f; c.fog_sun_amount = 0.6f;
        c.wind_strength = 0.6f; c.wind_gust = 0.1f; c.temperature = 10; c.wetness = 0.25f;
        c.effects.push_back(effect("objects/weather_ground_mist", 1.0f, EffectAnchor::Ground, {0, 0, 0}, 0.5f));
        out.push_back(c);
    }
    {
        Condition c; c.name = "rain";
        c.weight = 1.2f; c.min_minutes = 3; c.max_minutes = 7; c.transition = 25;
        c.next = {"overcast", "storm", "cloudy", "rain"};
        c.cloud_cover = 0.9f; c.sun = 0.3f; c.ambient = 0.75f; c.sky_tint = {0.85f, 0.88f, 0.92f};
        c.fog_density = 0.0127f; c.fog_color = {0.50f, 0.54f, 0.58f}; c.fog_sky_blend = 0.4f; c.fog_sun_amount = 0.05f;
        c.wind_strength = 5.0f; c.wind_gust = 0.4f; c.temperature = 12; c.precipitation = 0.6f; c.wetness = 0.8f;
        c.effects.push_back(effect("objects/weather_rain", 1.0f, EffectAnchor::Camera, {0, 0, 4}, 0.6f));
        out.push_back(c);
    }
    {
        Condition c; c.name = "storm";
        c.weight = 0.5f; c.min_minutes = 2; c.max_minutes = 5; c.transition = 20;
        c.next = {"rain", "overcast"};
        c.cloud_cover = 1.0f; c.sun = 0.15f; c.ambient = 0.55f; c.sky_tint = {0.70f, 0.73f, 0.80f};
        c.fog_density = 0.0187f; c.fog_color = {0.36f, 0.39f, 0.44f}; c.fog_sky_blend = 0.3f; c.fog_sun_amount = 0.0f;
        c.wind_strength = 11.0f; c.wind_gust = 0.6f; c.temperature = 11; c.precipitation = 1.0f; c.wetness = 1.0f;
        c.lightning = 6.0f;
        c.effects.push_back(effect("objects/weather_rain", 2.4f, EffectAnchor::Camera, {0, 0, 4}, 0.8f));
        c.effects.push_back(effect("objects/weather_ground_mist", 0.5f, EffectAnchor::Ground, {0, 0, 0}, 1.0f));
        out.push_back(c);
    }
    {
        Condition c; c.name = "snow";
        c.weight = 0.0f; c.min_minutes = 4; c.max_minutes = 10; c.transition = 40;
        c.next = {"snow", "overcast", "blizzard"};
        c.cloud_cover = 0.85f; c.sun = 0.4f; c.ambient = 0.95f; c.sky_tint = {0.95f, 0.97f, 1.02f};
        c.fog_density = 0.0102f; c.fog_color = {0.82f, 0.85f, 0.90f}; c.fog_sky_blend = 0.5f; c.fog_sun_amount = 0.2f;
        c.wind_strength = 2.0f; c.wind_gust = 0.3f; c.temperature = -4; c.precipitation = 0.5f; c.wetness = 0.2f;
        c.effects.push_back(effect("objects/weather_snow", 1.0f, EffectAnchor::Camera, {0, 0, 3}, 0.8f));
        out.push_back(c);
    }
    {
        Condition c; c.name = "blizzard";
        c.weight = 0.0f; c.min_minutes = 2; c.max_minutes = 4; c.transition = 25;
        c.next = {"snow"};
        c.cloud_cover = 1.0f; c.sun = 0.2f; c.ambient = 0.75f; c.sky_tint = {0.90f, 0.93f, 0.98f};
        c.fog_density = 0.0425f; c.fog_color = {0.80f, 0.83f, 0.88f}; c.fog_sky_blend = 0.2f; c.fog_sun_amount = 0.1f;
        c.wind_strength = 14.0f; c.wind_gust = 0.5f; c.temperature = -15; c.precipitation = 1.0f; c.wetness = 0.3f;
        c.effects.push_back(effect("objects/weather_snow", 2.5f, EffectAnchor::Camera, {0, 0, 3}, 1.0f));
        out.push_back(c);
    }
    {
        Condition c; c.name = "sandstorm";
        c.weight = 0.0f; c.min_minutes = 2; c.max_minutes = 5; c.transition = 30;
        c.next = {"clear", "cloudy"};
        c.cloud_cover = 0.5f; c.sun = 0.5f; c.ambient = 0.85f; c.sky_tint = {1.10f, 0.92f, 0.70f};
        c.fog_density = 0.034f; c.fog_color = {0.72f, 0.58f, 0.40f}; c.fog_sky_blend = 0.15f; c.fog_sun_amount = 0.5f;
        c.wind_strength = 12.0f; c.wind_gust = 0.5f; c.temperature = 34;
        c.effects.push_back(effect("objects/weather_dust", 1.0f, EffectAnchor::Camera, {0, 0, 1}, 1.0f));
        out.push_back(c);
    }
    return out;
}

} // namespace weather
} // namespace toy

namespace toy {
namespace weather {
namespace yaml_detail {

float num(const fkyaml::node& n, float def) {
    if (n.is_float_number()) return static_cast<float>(n.get_value<double>());
    if (n.is_integer()) return static_cast<float>(n.get_value<int64_t>());
    return def;
}

bool b(const fkyaml::node& m, const char* k, bool def) {
    if (!m.contains(k)) return def;
    const fkyaml::node& n = m.at(k);
    if (n.is_boolean()) return n.get_value<bool>();
    if (n.is_integer()) return n.get_value<int64_t>() != 0;
    return def;
}

glm::vec3 rgb(const fkyaml::node& m, const char* k, glm::vec3 def) {
    if (!m.contains(k)) return def;
    const fkyaml::node& n = m.at(k);
    if (n.is_sequence() && n.size() >= 3) return {num(n[0], def.x), num(n[1], def.y), num(n[2], def.z)};
    if (!n.is_mapping()) return def;
    return {f(n, "r", def.x), f(n, "g", def.y), f(n, "b", def.z)};
}

glm::vec3 xyz(const fkyaml::node& m, const char* k, glm::vec3 def) {
    if (!m.contains(k)) return def;
    const fkyaml::node& n = m.at(k);
    if (n.is_sequence() && n.size() >= 3) return {num(n[0], def.x), num(n[1], def.y), num(n[2], def.z)};
    if (!n.is_mapping()) return def;
    return {f(n, "x", def.x), f(n, "y", def.y), f(n, "z", def.z)};
}

fkyaml::node out_rgb(glm::vec3 c) {
    fkyaml::node n = fkyaml::node::mapping();
    n["r"] = static_cast<double>(c.r); n["g"] = static_cast<double>(c.g); n["b"] = static_cast<double>(c.b);
    return n;
}

fkyaml::node out_xyz(glm::vec3 v) {
    fkyaml::node n = fkyaml::node::mapping();
    n["x"] = static_cast<double>(v.x); n["y"] = static_cast<double>(v.y); n["z"] = static_cast<double>(v.z);
    return n;
}

EffectAnchor anchor_of(const std::string& s) {
    return s == "ground" ? EffectAnchor::Ground : s == "world" ? EffectAnchor::World : EffectAnchor::Camera;
}

} // namespace yaml_detail
} // namespace weather
} // namespace toy

namespace toy {
namespace weather {

EffectSpec parse_effect(const fkyaml::node& n) {
    using namespace yaml_detail;
    EffectSpec e;
    if (n.is_string()) { e.prefab = n.get_value<std::string>(); return e; }
    if (!n.is_mapping()) return e;
    e.prefab = s(n, "prefab", e.prefab);
    e.intensity = f(n, "intensity", e.intensity);
    e.follow = anchor_of(s(n, "follow", "camera"));
    e.offset = xyz(n, "offset", e.offset);
    e.wind_influence = f(n, "wind_influence", e.wind_influence);
    return e;
}

Condition parse_condition(const fkyaml::node& n) {
    using namespace yaml_detail;
    Condition c;
    if (!n.is_mapping()) return c;
    c.name = s(n, "name", c.name);
    c.weight = f(n, "weight", c.weight);
    if (n.contains("duration")) {
        const fkyaml::node& d = n.at("duration");
        if (d.is_sequence() && d.size() >= 2) { c.min_minutes = num(d[0], c.min_minutes); c.max_minutes = num(d[1], c.max_minutes); }
        else c.min_minutes = c.max_minutes = num(d, c.min_minutes);
    }
    c.min_minutes = std::max(0.05f, c.min_minutes);
    c.max_minutes = std::max(c.min_minutes, c.max_minutes);
    c.transition = std::max(0.0f, f(n, "transition", c.transition));
    if (n.contains("next") && n.at("next").is_sequence()) {
        for (const auto& x : n.at("next")) if (x.is_string()) c.next.push_back(x.get_value<std::string>());
    }
    c.cloud_cover = f(n, "cloud_cover", c.cloud_cover);
    c.sun = f(n, "sun", c.sun);
    c.ambient = f(n, "ambient", c.ambient);
    c.sky_tint = rgb(n, "sky_tint", c.sky_tint);
    c.fog_density = f(n, "fog_density", c.fog_density);
    c.fog_color = rgb(n, "fog_color", c.fog_color);
    c.fog_height_falloff = f(n, "fog_height_falloff", c.fog_height_falloff);
    c.fog_sky_blend = f(n, "fog_sky_blend", c.fog_sky_blend);
    c.fog_max_opacity = f(n, "fog_max_opacity", c.fog_max_opacity);
    c.fog_sun_amount = f(n, "fog_sun_amount", c.fog_sun_amount);
    c.wind_strength = f(n, "wind_strength", c.wind_strength);
    c.wind_heading = f(n, "wind_heading", c.wind_heading);
    c.wind_gust = f(n, "wind_gust", c.wind_gust);
    c.temperature = f(n, "temperature", c.temperature);
    c.precipitation = f(n, "precipitation", c.precipitation);
    c.wetness = f(n, "wetness", c.wetness);
    c.lightning = f(n, "lightning", c.lightning);
    if (n.contains("effects") && n.at("effects").is_sequence()) {
        for (const auto& e : n.at("effects")) {
            EffectSpec spec = parse_effect(e);
            if (!spec.prefab.empty()) c.effects.push_back(std::move(spec));
        }
    }
    return c;
}

Settings parse_settings(const fkyaml::node& n) {
    using namespace yaml_detail;
    Settings st;
    if (n.is_mapping()) {
        st.enabled = b(n, "enabled", st.enabled);
        st.time_of_day = f(n, "time_of_day", st.time_of_day);
        st.day_length_minutes = std::max(0.0f, f(n, "day_length_minutes", st.day_length_minutes));
        st.latitude = std::clamp(f(n, "latitude", st.latitude), -89.0f, 89.0f);
        st.north_offset = f(n, "north_offset", st.north_offset);
        st.drive_sun = b(n, "drive_sun", st.drive_sun);
        st.sun_intensity = f(n, "sun_intensity", st.sun_intensity);
        st.moon_intensity = f(n, "moon_intensity", st.moon_intensity);
        st.moon_color = rgb(n, "moon_color", st.moon_color);
        st.day_zenith = rgb(n, "day_zenith", st.day_zenith);
        st.day_horizon = rgb(n, "day_horizon", st.day_horizon);
        st.day_ground = rgb(n, "day_ground", st.day_ground);
        st.twilight_zenith = rgb(n, "twilight_zenith", st.twilight_zenith);
        st.twilight_horizon = rgb(n, "twilight_horizon", st.twilight_horizon);
        st.night_zenith = rgb(n, "night_zenith", st.night_zenith);
        st.night_horizon = rgb(n, "night_horizon", st.night_horizon);
        st.night_ground = rgb(n, "night_ground", st.night_ground);
        st.ambient_day = f(n, "ambient_day", st.ambient_day);
        st.ambient_night = f(n, "ambient_night", st.ambient_night);
        st.night_exposure = f(n, "night_exposure", st.night_exposure);
        st.condition = s(n, "condition", st.condition);
        st.schedule = schedule_of(s(n, "schedule", "fixed"));
        st.seed = static_cast<uint32_t>(std::max(0.0f, f(n, "seed", 0.0f)));
        st.ground_height = f(n, "ground_height", st.ground_height);
        st.surface_collision = b(n, "surface_collision", st.surface_collision);
        st.ground_effects = b(n, "ground_effects", st.ground_effects);
        st.ground_height_splashes = b(n, "ground_height_splashes", st.ground_height_splashes);
        st.snow_accumulate_time = std::max(1.0f, f(n, "snow_accumulate_time", st.snow_accumulate_time));
        st.snow_melt_time = std::max(1.0f, f(n, "snow_melt_time", st.snow_melt_time));
        st.snow_max_depth = std::max(0.0f, f(n, "snow_max_depth", st.snow_max_depth));
        st.initial_snow_cover = std::clamp(f(n, "initial_snow_cover", st.initial_snow_cover), 0.0f, 1.0f);
        st.snow_auto_deformers = b(n, "snow_auto_deformers", st.snow_auto_deformers);
        st.snow_trench_recover_time = std::max(0.0f, f(n, "snow_trench_recover_time", st.snow_trench_recover_time));
        st.snow_patch_hard = s(n, "snow_patch_style", st.snow_patch_hard ? "hard" : "soft") == "hard";
        st.snow_patch_size = std::max(0.1f, f(n, "snow_patch_size", st.snow_patch_size));
        if (n.contains("conditions") && n.at("conditions").is_sequence()) {
            for (const auto& c : n.at("conditions")) {
                Condition cond = parse_condition(c);
                if (!cond.name.empty() && !st.find(cond.name)) st.conditions.push_back(std::move(cond));
            }
        }
    }
    st.time_of_day = std::fmod(std::fmod(st.time_of_day, 24.0f) + 24.0f, 24.0f);
    if (st.conditions.empty()) st.conditions = default_conditions();
    if (!st.find(st.condition)) st.condition = st.conditions.front().name;
    return st;
}

fkyaml::node to_node(const EffectSpec& e) {
    using namespace yaml_detail;
    fkyaml::node n = fkyaml::node::mapping();
    n["prefab"] = e.prefab;
    n["intensity"] = out_f(e.intensity);
    n["follow"] = std::string(anchor_name(e.follow));
    n["offset"] = out_xyz(e.offset);
    n["wind_influence"] = out_f(e.wind_influence);
    return n;
}

fkyaml::node to_node(const Condition& c) {
    using namespace yaml_detail;
    fkyaml::node n = fkyaml::node::mapping();
    n["name"] = c.name;
    n["weight"] = out_f(c.weight);
    fkyaml::node d = fkyaml::node::sequence();
    d.get_value_ref<fkyaml::node::sequence_type&>().push_back(fkyaml::node(out_f(c.min_minutes)));
    d.get_value_ref<fkyaml::node::sequence_type&>().push_back(fkyaml::node(out_f(c.max_minutes)));
    n["duration"] = d;
    n["transition"] = out_f(c.transition);
    fkyaml::node nx = fkyaml::node::sequence();
    for (const auto& s : c.next) nx.get_value_ref<fkyaml::node::sequence_type&>().push_back(fkyaml::node(s));
    n["next"] = nx;
    n["cloud_cover"] = out_f(c.cloud_cover);
    n["sun"] = out_f(c.sun);
    n["ambient"] = out_f(c.ambient);
    n["sky_tint"] = out_rgb(c.sky_tint);
    n["fog_density"] = out_f(c.fog_density);
    n["fog_color"] = out_rgb(c.fog_color);
    n["fog_height_falloff"] = out_f(c.fog_height_falloff);
    n["fog_sky_blend"] = out_f(c.fog_sky_blend);
    n["fog_max_opacity"] = out_f(c.fog_max_opacity);
    n["fog_sun_amount"] = out_f(c.fog_sun_amount);
    n["wind_strength"] = out_f(c.wind_strength);
    n["wind_heading"] = out_f(c.wind_heading);
    n["wind_gust"] = out_f(c.wind_gust);
    n["temperature"] = out_f(c.temperature);
    n["precipitation"] = out_f(c.precipitation);
    n["wetness"] = out_f(c.wetness);
    n["lightning"] = out_f(c.lightning);
    fkyaml::node fx = fkyaml::node::sequence();
    for (const auto& e : c.effects) fx.get_value_ref<fkyaml::node::sequence_type&>().push_back(to_node(e));
    n["effects"] = fx;
    return n;
}

fkyaml::node to_node(const Settings& st) {
    using namespace yaml_detail;
    fkyaml::node n = fkyaml::node::mapping();
    n["enabled"] = st.enabled;
    n["time_of_day"] = out_f(st.time_of_day);
    n["day_length_minutes"] = out_f(st.day_length_minutes);
    n["latitude"] = out_f(st.latitude);
    n["north_offset"] = out_f(st.north_offset);
    n["drive_sun"] = st.drive_sun;
    n["sun_intensity"] = out_f(st.sun_intensity);
    n["moon_intensity"] = out_f(st.moon_intensity);
    n["moon_color"] = out_rgb(st.moon_color);
    n["day_zenith"] = out_rgb(st.day_zenith);
    n["day_horizon"] = out_rgb(st.day_horizon);
    n["day_ground"] = out_rgb(st.day_ground);
    n["twilight_zenith"] = out_rgb(st.twilight_zenith);
    n["twilight_horizon"] = out_rgb(st.twilight_horizon);
    n["night_zenith"] = out_rgb(st.night_zenith);
    n["night_horizon"] = out_rgb(st.night_horizon);
    n["night_ground"] = out_rgb(st.night_ground);
    n["ambient_day"] = out_f(st.ambient_day);
    n["ambient_night"] = out_f(st.ambient_night);
    n["night_exposure"] = out_f(st.night_exposure);
    n["condition"] = st.condition;
    n["schedule"] = std::string(schedule_name(st.schedule));
    n["seed"] = static_cast<int64_t>(st.seed);
    n["ground_height"] = out_f(st.ground_height);
    n["surface_collision"] = st.surface_collision;
    n["ground_effects"] = st.ground_effects;
    n["ground_height_splashes"] = st.ground_height_splashes;
    n["snow_accumulate_time"] = out_f(st.snow_accumulate_time);
    n["snow_melt_time"] = out_f(st.snow_melt_time);
    n["snow_max_depth"] = out_f(st.snow_max_depth);
    n["initial_snow_cover"] = out_f(st.initial_snow_cover);
    n["snow_auto_deformers"] = st.snow_auto_deformers;
    n["snow_trench_recover_time"] = out_f(st.snow_trench_recover_time);
    n["snow_patch_style"] = std::string(st.snow_patch_hard ? "hard" : "soft");
    n["snow_patch_size"] = out_f(st.snow_patch_size);
    fkyaml::node cs = fkyaml::node::sequence();
    for (const auto& c : st.conditions) cs.get_value_ref<fkyaml::node::sequence_type&>().push_back(to_node(c));
    n["conditions"] = cs;
    return n;
}

} // namespace weather
} // namespace toy
