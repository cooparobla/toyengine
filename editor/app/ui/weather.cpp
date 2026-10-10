#include "editor/app/editor_app.h"

#include <toyengine/scene/runtime_object.h>

namespace toy {
namespace editor {

Node EditorApp::weather_block_() const {
    const Node st = doc_.scene_settings();
    if (st.is_mapping() && st.contains("weather") && st.at("weather").is_mapping()) return st.at("weather");
    return Node::mapping();
}

toy::weather::WeatherSystem* EditorApp::live_weather_() const {
    coopa::scene::Scene* s = viewed_live_scene_();
    return s ? toy::weather::find(*s) : nullptr;
}

void EditorApp::set_weather_block_(const Node* block, const std::string& label, const std::string& merge) {
    if (!weather_scene_()) return;
    settings_edit_was_scene_ = true;
    apply_(doc_.edit(label, [&](Node& d) -> Change {
        Node& sc = d["scene"];
        Node settings = sc.contains("settings") && sc.at("settings").is_mapping() ? sc.at("settings") : Node::mapping();
        if (block) settings["weather"] = *block;
        else erase_key(settings, "weather");
        if (settings.size() > 0) sc["settings"] = settings;
        else erase_key(sc, "settings");
        return {ChangeScope::Settings, 0};
    }, merge));
}

void EditorApp::enable_weather_() {
    Node block = weather_block_();
    if (!block.contains("conditions")) {
        toy::weather::Settings st = toy::weather::parse_settings(block);
        Node& rs = config_.section("render");
        auto render_vec = [&](const char* key, glm::vec3 def) {
            const FieldDesc* f = render_field_(key);
            const Node* n = f ? setting_value_(rs, "render", *f) : nullptr;
            return n ? as_vec3(*n, def) : def;
        };
        st.day_zenith = render_vec("sky_zenith", st.day_zenith);
        st.day_horizon = render_vec("sky_horizon", st.day_horizon);
        st.day_ground = render_vec("sky_ground", st.day_ground);
        if (const FieldDesc* f = render_field_("ambient_intensity")) {
            if (const Node* n = setting_value_(rs, "render", *f)) st.ambient_day = as_float(*n, st.ambient_day);
        }
        bool found = false;
        doc_.for_each_object([&](const Node& o, int) {
            if (found || !o.contains("components")) return;
            for (const auto& c : o.at("components").as_seq()) {
                if (component_type(c) == "DirectionalLight") { st.sun_intensity = get_float(c, "intensity", st.sun_intensity); found = true; break; }
            }
        });
        block = toy::weather::to_node(st);
    }
    block["enabled"] = Node(true);
    set_weather_block_(&block, "Enable Weather");
}

void EditorApp::weather_subhead_(imm::Context& ctx, const std::string& title) {
    ctx.spacing(3);
    const imm::Box hb = ctx.next_box(ctx.style.row_height - 4);
    ctx.text_in(hb, title, ctx.style.text_dim, 0.0f);
    const float tw = ctx.text_width(title) + 8;
    ctx.fill({hb.x + tw, hb.y + hb.h * 0.5f, std::max(0.0f, hb.w - tw), 1.0f}, ctx.style.separator);
}

bool EditorApp::weather_group_(imm::Context& ctx, const std::string& title, imm::Icon icon, bool default_open, const std::string& tip) {
    ctx.spacing(2);
    const bool open = ctx.collapsing_header(title, default_open, nullptr, icon);
    ctx.tooltip(title + "\n" + tip);
    test_rects_["weather_group:" + title] = ctx.last_rect();
    return open;
}

void EditorApp::draw_weather_fields_(imm::Context& ctx, const std::vector<FieldDesc>& fields, const InspectorEnv& env) {
    Node block = weather_block_();
    for (const auto& f : fields) {
        ctx.push_id(f.key);
        const EditResult r = draw_field(ctx, f, block, env);
        ctx.pop_id();
        if (r.changed) {
            set_weather_block_(&block, "Edit weather." + f.key, r.active ? "weather:" + f.key : std::string());
            block = weather_block_();
        }
        if (r.finished) doc_.end_merge();
    }
}

glm::vec4 EditorApp::weather_display_(glm::vec3 c) {
    c = glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
    return {std::pow(c.r, 1.0f / 2.2f), std::pow(c.g, 1.0f / 2.2f), std::pow(c.b, 1.0f / 2.2f), 1.0f};
}

std::string EditorApp::weather_hhmm_(float hour) {
    hour = toy::weather::wrap_hour(hour);
    int hh = static_cast<int>(hour), mm = static_cast<int>(std::round((hour - hh) * 60.0f));
    if (mm == 60) { mm = 0; hh = (hh + 1) % 24; }
    char b[16];
    std::snprintf(b, sizeof(b), "%02d:%02d", hh, mm);
    return b;
}

toy::weather::SkyFrame EditorApp::weather_condition_sky_(const toy::weather::Settings& st, const toy::weather::Condition& c, float hour) {
    toy::weather::SkyFrame f = toy::weather::evaluate_sky(st, hour);
    auto cloudy = [&](glm::vec3 col) {
        const float lum = glm::dot(col, glm::vec3(0.2126f, 0.7152f, 0.0722f));
        return glm::mix(col, glm::vec3(lum * 0.85f), std::clamp(c.cloud_cover, 0.0f, 1.0f) * 0.85f) * c.sky_tint;
    };
    f.zenith = cloudy(f.zenith);
    f.horizon = cloudy(f.horizon);
    f.ground = cloudy(f.ground);
    f.light_intensity *= c.sun;
    return f;
}

void EditorApp::draw_weather_dome_(imm::Context& ctx, const imm::Box& b, glm::vec3 zenith, glm::vec3 horizon, glm::vec3 ground,
                        float sun_height, glm::vec3 disc) {
    const float mid = std::floor(b.y + b.h * 0.62f);
    const glm::vec4 z = weather_display_(zenith), h = weather_display_(horizon), g = weather_display_(ground);
    ctx.gradient({b.x, b.y, b.w, mid - b.y}, z, z, h, h);
    ctx.gradient({b.x, mid, b.w, b.bottom() - mid}, h, h, g, g);
    if (sun_height > -1.5f) {
        const float y = mid - std::clamp(sun_height, -0.3f, 1.0f) * (mid - b.y - 5.0f);
        if (y < mid) ctx.circle({b.x + b.w * 0.5f, y}, std::max(2.5f, b.h * 0.07f), weather_display_(disc));
    }
    ctx.fill({b.x, mid, b.w, 1.0f}, {1.0f, 1.0f, 1.0f, 0.12f});
    ctx.outline(b, ctx.style.border);
}

void EditorApp::draw_weather_day_strip_(imm::Context& ctx, const toy::weather::Settings& st, toy::weather::WeatherSystem* w) {
    const float lab_h = ctx.style.font_size + 4;
    const imm::Box b = ctx.next_box(40 + lab_h);
    const imm::Box sky{b.x, b.y, b.w, 40};
    const float mid = sky.y + sky.h * 0.5f, amp = sky.h * 0.42f;
    auto x_of = [&](float hour) { return sky.x + sky.w * hour / 24.0f; };
    constexpr int kCols = 48;
    for (int i = 0; i < kCols; ++i) {
        const float h0 = 24.0f * i / kCols, h1 = 24.0f * (i + 1) / kCols;
        const auto f0 = toy::weather::evaluate_sky(st, h0), f1 = toy::weather::evaluate_sky(st, h1);
        const float x0 = std::floor(x_of(h0)), x1 = std::floor(x_of(h1)) + 1.0f;
        ctx.gradient({x0, sky.y, x1 - x0, mid - sky.y}, weather_display_(f0.zenith), weather_display_(f1.zenith),
                     weather_display_(f1.horizon), weather_display_(f0.horizon));
        ctx.gradient({x0, mid, x1 - x0, sky.bottom() - mid}, weather_display_(f0.horizon), weather_display_(f1.horizon),
                     weather_display_(f1.ground), weather_display_(f0.ground));
    }
    ctx.fill({sky.x, mid, sky.w, 1.0f}, {1.0f, 1.0f, 1.0f, 0.18f});
    // The sun's height through the day: bright above the horizon, faint below.
    glm::vec2 prev{};
    for (int i = 0; i <= 96; ++i) {
        const float hour = 24.0f * i / 96.0f;
        const glm::vec2 p{x_of(hour), mid - toy::weather::sun_direction(hour, st.latitude, st.north_offset).z * amp};
        if (i > 0) {
            const bool up = p.y < mid || prev.y < mid;
            ctx.line(prev, p, up ? glm::vec4(1.0f, 0.85f, 0.45f, 0.9f) : glm::vec4(1.0f, 1.0f, 1.0f, 0.18f), up ? 1.5f : 1.0f);
        }
        prev = p;
    }
    ctx.outline(sky, ctx.style.border);
    // Hour labels.
    for (int hr = 0; hr <= 24; hr += 6) {
        const std::string t = std::to_string(hr);
        const float tw = ctx.text_width(t);
        const float x = std::clamp(x_of(static_cast<float>(hr)) - tw * 0.5f, sky.x, sky.right() - tw);
        ctx.draw_text({x, sky.bottom() + 2}, t, ctx.style.text_disabled);
        ctx.fill({x_of(static_cast<float>(hr)), sky.bottom(), 1.0f, 3.0f}, ctx.style.text_disabled);
    }
    // The scene's start time.
    {
        const float x = x_of(toy::weather::wrap_hour(st.time_of_day));
        ctx.triangle({x - 5, sky.bottom() + 8}, {x + 5, sky.bottom() + 8}, {x, sky.bottom() + 1}, ctx.style.accent);
    }
    // The live clock.
    if (w && w->enabled()) {
        const float hour = w->time_of_day();
        const float x = x_of(hour);
        const auto f = toy::weather::evaluate_sky(st, hour);
        ctx.fill({x - 0.5f, sky.y, 1.5f, sky.h}, {1.0f, 1.0f, 1.0f, 0.85f});
        const float y = mid - f.sun_height * amp;
        ctx.circle({x, y}, 4.5f, f.moon_light ? weather_display_(st.moon_color) : glm::vec4(1.0f, 0.9f, 0.5f, 1.0f));
        ctx.ring({x, y}, 4.5f, 1.0f, {0.0f, 0.0f, 0.0f, 0.6f});
    }
    bool hov = false, held = false;
    ctx.invisible_button("weather_day_strip", sky, &hov, &held);
    test_rects_["weather_day_strip"] = sky;
    const float mouse_hour = std::clamp((ctx.mouse().x - sky.x) / std::max(1.0f, sky.w) * 24.0f, 0.0f, 23.999f);
    if (held && w && w->enabled()) w->set_time(mouse_hour);
    if (hov && !held) {
        const auto f = toy::weather::evaluate_sky(st, mouse_hour);
        char line[96];
        std::snprintf(line, sizeof(line), "%s -- %s %.0f deg %s the horizon", weather_hhmm_(mouse_hour).c_str(),
                      f.moon_light ? "sun" : "sun", std::abs(glm::degrees(std::asin(std::clamp(f.sun_height, -1.0f, 1.0f)))),
                      f.sun_height >= 0.0f ? "above" : "below");
        ctx.tooltip(std::string("A day of this sky\n") + line +
                    "\nThe curve is the sun's height; the triangle marks the scene's Start Time.\nDrag to scrub the live clock (not saved).");
    }
}

void EditorApp::draw_weather_sun_path_(imm::Context& ctx, const toy::weather::Settings& st, const toy::weather::WeatherSystem* w) {
    const float size = 116;
    const imm::Box b = ctx.next_box(size);
    const glm::vec2 c{b.x + size * 0.5f + 4, b.y + size * 0.5f};
    const float R = size * 0.5f - 12;
    ctx.circle(c, R, ctx.style.field, 48);
    for (float elev : {30.0f, 60.0f}) ctx.ring(c, R * (1.0f - elev / 90.0f), 1.0f, {1.0f, 1.0f, 1.0f, 0.07f}, 40);
    ctx.ring(c, R, 1.0f, ctx.style.text_disabled, 48);
    ctx.line({c.x - R, c.y}, {c.x + R, c.y}, {1.0f, 1.0f, 1.0f, 0.06f});
    ctx.line({c.x, c.y - R}, {c.x, c.y + R}, {1.0f, 1.0f, 1.0f, 0.06f});
    ctx.draw_text({c.x + R + 2, c.y - ctx.style.font_size * 0.6f}, "X", ctx.style.axis_x);
    ctx.draw_text({c.x - ctx.text_width("Y") * 0.5f, c.y - R - ctx.style.font_size - 1}, "Y", ctx.style.axis_y);
    auto project = [&](glm::vec3 d) {
        const float elev = std::asin(std::clamp(d.z, -1.0f, 1.0f));
        const float r = R * (1.0f - elev / glm::half_pi<float>());
        const glm::vec2 xy(d.x, d.y);
        const float l = glm::length(xy);
        const glm::vec2 dir = l > 1e-5f ? xy / l : glm::vec2(0.0f);
        return glm::vec2(c.x + dir.x * r, c.y - dir.y * r);
    };
    const glm::vec4 sun_col{1.0f, 0.82f, 0.4f, 1.0f};
    glm::vec2 prev{};
    bool prev_up = false;
    for (int i = 0; i <= 96; ++i) {
        const glm::vec3 d = toy::weather::sun_direction(24.0f * i / 96.0f, st.latitude, st.north_offset);
        const bool up = d.z >= 0.0f;
        const glm::vec2 p = project(glm::vec3(d.x, d.y, std::max(0.0f, d.z)));
        if (i > 0 && up && prev_up) ctx.line(prev, p, sun_col, 1.5f);
        prev = p;
        prev_up = up;
    }
    for (float hr : {6.0f, 12.0f, 18.0f}) {
        const glm::vec3 d = toy::weather::sun_direction(hr, st.latitude, st.north_offset);
        const glm::vec2 p = project(glm::vec3(d.x, d.y, std::max(0.0f, d.z)));
        ctx.circle(p, 2.5f, sun_col);
        // The label sits toward the centre, clear of the dot and the axis letters.
        const std::string t = std::to_string(static_cast<int>(hr));
        const glm::vec2 in = glm::length(c - p) > 1e-3f ? glm::normalize(c - p) : glm::vec2(0.0f, -1.0f);
        const glm::vec2 lc = p + in * 11.0f;
        ctx.draw_text({lc.x - ctx.text_width(t) * 0.5f, lc.y - ctx.style.font_size * 0.6f}, t, ctx.style.text_dim);
    }
    if (w && w->enabled()) {
        const glm::vec3 d = toy::weather::sun_direction(w->time_of_day(), st.latitude, st.north_offset);
        if (d.z >= 0.0f) {
            ctx.circle(project(d), 5.0f, sun_col);
            ctx.ring(project(d), 5.0f, 1.0f, {0.0f, 0.0f, 0.0f, 0.6f});
        } else {
            ctx.circle(project(-d), 4.5f, weather_display_(st.moon_color));
            ctx.ring(project(-d), 4.5f, 1.0f, {0.0f, 0.0f, 0.0f, 0.6f});
        }
    }
    // Read-outs to the right.
    const glm::vec3 noon = toy::weather::sun_direction(12.0f, st.latitude, st.north_offset);
    const float noon_elev = glm::degrees(std::asin(std::clamp(noon.z, -1.0f, 1.0f)));
    const glm::vec3 rise = toy::weather::sun_direction(6.0f, st.latitude, st.north_offset);
    float rise_deg = glm::degrees(std::atan2(rise.y, rise.x));
    if (rise_deg < 0.0f) rise_deg += 360.0f;
    char l1[64], l2[64], l3[64];
    std::snprintf(l1, sizeof(l1), "Noon: %.0f deg up", noon_elev);
    std::snprintf(l2, sizeof(l2), "Noon shadow: %.1fx height", noon_elev > 0.5f ? 1.0f / std::tan(glm::radians(noon_elev)) : 99.0f);
    std::snprintf(l3, sizeof(l3), "Sunrise: %.0f deg from +X", rise_deg);
    const float tx = b.x + size + 14, lh = ctx.style.row_height - 2;
    float ty = b.y + 8;
    ctx.push_clip(b);
    for (const char* l : {l1, l2, l3}) { ctx.draw_text({tx, ty}, l, ctx.style.text_dim); ty += lh; }
    ctx.draw_text({tx, ty + 4}, "Seen from above;", ctx.style.text_disabled);
    ctx.draw_text({tx, ty + 4 + lh}, "centre = overhead", ctx.style.text_disabled);
    ctx.pop_clip();
    if (ctx.is_hovered({b.x, b.y, size + 8, size})) {
        ctx.tooltip("Sun Path\nThe sun's track across the sky, seen from above: the outer circle is the horizon, the centre is "
                    "straight overhead (faint rings at 30 and 60 deg). Dots mark 06:00, noon and 18:00; the big dot is the live sun "
                    "(or the moon at night).\nLatitude pulls the track toward the edge (lower sun, longer shadows); Sun Path "
                    "Heading turns it.");
    }
    test_rects_["weather_sun_path"] = b;
}

const char* EditorApp::weather_wind_name_(float ms) {
    if (ms < 0.5f) return "calm";
    if (ms < 3.4f) return "light breeze";
    if (ms < 8.0f) return "breeze";
    if (ms < 13.9f) return "strong wind";
    if (ms < 24.5f) return "gale";
    return "storm";
}

void EditorApp::draw_weather_wind_dial_(imm::Context& ctx, Node& item, EditResult& total) {
    const float size = 76;
    const imm::Box b = ctx.next_box(size);
    const glm::vec2 c{b.x + size * 0.5f + 4, b.y + size * 0.5f};
    const float R = size * 0.5f - 6;
    const imm::Box hit{c.x - R, c.y - R, 2 * R, 2 * R};
    bool hov = false, held = false;
    ctx.invisible_button("wind_dial", hit, &hov, &held);
    const bool released = ctx.last_deactivated();
    test_rects_["weather_wind_dial"] = hit;
    float heading = get_float(item, "wind_heading", 30.0f);
    const float strength = get_float(item, "wind_strength", 1.5f);
    if (held) {
        const glm::vec2 m = ctx.mouse() - c;
        if (glm::length(m) > 3.0f) {
            float deg = std::round(glm::degrees(std::atan2(-m.y, m.x)) / 5.0f) * 5.0f;
            if (deg < 0.0f) deg += 360.0f;
            if (std::abs(deg - heading) > 1e-3f) {
                heading = deg;
                item["wind_heading"] = make_float(deg);
                total.changed = true;
                if (total.key.empty()) total.key = "wind_heading";
            }
            total.active = true;
        }
    }
    if (released) total.finished = true;
    ctx.circle(c, R, hov || held ? ctx.style.field_hover : ctx.style.field, 40);
    ctx.ring(c, R, 1.0f, ctx.style.text_disabled, 40);
    for (int k = 0; k < 8; ++k) {
        const float a = glm::radians(45.0f * k);
        const glm::vec2 d{std::cos(a), -std::sin(a)};
        ctx.line(c + d * (R - (k % 2 ? 3.0f : 6.0f)), c + d * R, ctx.style.text_disabled);
    }
    ctx.draw_text({c.x + R + 2, c.y - ctx.style.font_size * 0.6f}, "X", ctx.style.axis_x);
    ctx.draw_text({c.x - ctx.text_width("Y") * 0.5f, c.y - R - ctx.style.font_size - 1}, "Y", ctx.style.axis_y);
    const float a = glm::radians(heading);
    const glm::vec2 d{std::cos(a), -std::sin(a)}, n{-d.y, d.x};
    const float len = R * (0.35f + 0.5f * std::clamp(std::log1p(strength) / std::log1p(25.0f), 0.0f, 1.0f));
    const glm::vec2 tail = c - d * len, tip = c + d * len;
    const glm::vec4 col = held ? ctx.style.accent : glm::vec4(0.55f, 0.8f, 1.0f, 1.0f);
    ctx.line(tail, tip - d * 6.0f, col, 2.0f);
    ctx.triangle(tip, tip - d * 9.0f + n * 5.0f, tip - d * 9.0f - n * 5.0f, col);
    char l1[48], l2[48];
    std::snprintf(l1, sizeof(l1), "Blows toward %.0f deg", heading);
    std::snprintf(l2, sizeof(l2), "%.1f m/s -- %s", strength, weather_wind_name_(strength));
    const float tx = b.x + size + 14, lh = ctx.style.row_height - 2;
    ctx.push_clip(b);
    ctx.draw_text({tx, b.y + 10}, l1, ctx.style.text_dim);
    ctx.draw_text({tx, b.y + 10 + lh}, l2, ctx.style.text_dim);
    ctx.draw_text({tx, b.y + 10 + lh * 2 + 4}, "Drag the dial to aim it", ctx.style.text_disabled);
    ctx.pop_clip();
    if (hov && !held) ctx.tooltip("Wind\nThe arrow points where the wind blows (counter-clockwise from +X, seen from above); "
                                  "its length grows with the strength. Drag to aim it.");
}

const std::vector<FieldDesc>& EditorApp::weather_time_fields_() {
    static const std::vector<FieldDesc> f = {
        with_label(with_tip(f_float("time_of_day", 10.0f, 0.02f, 0.0f, 24.0f), "Hour the clock starts at when the scene loads (0-24): the "
                                                                                 "triangle under the strip. Changing it jumps the live clock there"), "Start Time"),
        with_label(with_tip(f_float("day_length_minutes", 24.0f, 0.1f, 0.0f, 100000.0f), "Real minutes per game day. 0 stops time at the start time"), "Day Length (min)"),
    };
    return f;
}

const std::vector<FieldDesc>& EditorApp::weather_sun_fields_() {
    static const std::vector<FieldDesc> f = {
        with_label(with_tip(f_bool("drive_sun", true), "Aim and colour the scene's directional light: the sun by day, the moon by night. "
                                                       "A scene without one gets a runtime Sun"), "Drive Sun"),
        with_label(with_tip(f_float("sun_intensity", 1.2f, 0.01f, 0.0f, 100.0f), "The sun's intensity at noon in clear weather"), "Sun Intensity"),
        with_label(with_tip(f_float("latitude", 35.0f, 0.25f, -89.0f, 89.0f), "Tilts the sun's path: 0 = straight overhead at noon, higher = lower sun and longer shadows"), "Latitude"),
        with_label(with_tip(f_float("north_offset", 0.0f, 0.5f, -360.0f, 360.0f), "Turns the sun's path about Z. 0: sunrise toward +X"), "Sun Path Heading"),
    };
    return f;
}

const std::vector<FieldDesc>& EditorApp::weather_moon_fields_() {
    static const std::vector<FieldDesc> f = {
        with_label(with_tip(f_float("moon_intensity", 0.12f, 0.005f, 0.0f, 100.0f), "The moon's intensity at its highest"), "Moon Intensity"),
        with_label(with_tip(f_color("moon_color", glm::vec3(0.55f, 0.65f, 0.9f)), "The moonlight's colour"), "Moon Colour"),
    };
    return f;
}

const std::vector<FieldDesc>& EditorApp::weather_palette_fields_(int which) {
    static const std::vector<FieldDesc> day = {
        with_label(with_tip(f_color("day_zenith", glm::vec3(0.05f, 0.18f, 0.55f)), "Straight up, by day"), "Zenith (top)"),
        with_label(with_tip(f_color("day_horizon", glm::vec3(0.25f, 0.35f, 0.45f)), "Along the horizon, by day; also tints distant haze"), "Horizon"),
        with_label(with_tip(f_color("day_ground", glm::vec3(0.05f, 0.045f, 0.04f)), "Below the horizon, by day: lights surfaces from beneath"), "Ground (below)"),
    };
    static const std::vector<FieldDesc> twilight = {
        with_label(with_tip(f_color("twilight_zenith", glm::vec3(0.12f, 0.12f, 0.3f)), "Mixed (half strength) into the top of the sky while the sun is near the horizon"), "Zenith (top)"),
        with_label(with_tip(f_color("twilight_horizon", glm::vec3(0.85f, 0.42f, 0.2f)), "The sunrise / sunset glow along the horizon; also warms the fog at twilight"), "Horizon Glow"),
    };
    static const std::vector<FieldDesc> night = {
        with_label(with_tip(f_color("night_zenith", glm::vec3(0.004f, 0.007f, 0.02f)), "Straight up, at night"), "Zenith (top)"),
        with_label(with_tip(f_color("night_horizon", glm::vec3(0.015f, 0.022f, 0.045f)), "Along the horizon, at night"), "Horizon"),
        with_label(with_tip(f_color("night_ground", glm::vec3(0.006f, 0.006f, 0.008f)), "Below the horizon, at night"), "Ground (below)"),
    };
    return which == 0 ? day : which == 1 ? twilight : night;
}

const std::vector<FieldDesc>& EditorApp::weather_brightness_fields_() {
    static const std::vector<FieldDesc> f = {
        with_label(with_tip(f_float("ambient_day", 1.0f, 0.01f, 0.0f, 16.0f), "Ambient and sky-reflection intensity at noon (render ambient_intensity / sky_intensity)"), "Ambient (Day)"),
        with_label(with_tip(f_float("ambient_night", 0.25f, 0.01f, 0.0f, 16.0f), "...at midnight"), "Ambient (Night)"),
        with_label(with_tip(f_float("night_exposure", 0.35f, 0.01f, 0.0f, 4.0f), "Multiplies the render exposure at midnight, so auto exposure "
                                                                                 "doesn't lift the night back to day. 1 = off"), "Night Exposure"),
    };
    return f;
}

std::vector<FieldDesc> EditorApp::weather_schedule_fields_(const std::vector<std::string>& names) const {
    return {
        with_label(with_tip(f_enum("condition", names), "The condition the scene starts in. Changing it switches the live weather at once"), "Start Condition"),
        with_label(with_tip(f_enum("schedule", {"fixed", "random", "cycle"}),
                            "fixed: stays on the start condition (code calls set_condition()). random: when a condition's duration runs out, "
                            "a weighted pick among its Next list (or all). cycle: the next one in the list"), "Schedule"),
        with_tip(f_int("seed", 0, 0, 1000000), "Random schedule, lightning and gusts; 0 = a fixed default"),
    };
}

const std::vector<FieldDesc>& EditorApp::weather_ground_fields_() {
    static const std::vector<FieldDesc> f = {
        with_label(with_tip(f_float("ground_height", 0.0f, 0.05f), "World Z effects treat as the ground: mist sits on it, rain and snow land on it "
                                                                "where nothing else is below"), "Ground Height"),
        with_label(with_tip(f_bool("surface_collision", true), "Rain and snow stop on real surfaces -- roofs, terrain, water -- probed around the "
                                                               "camera (colliders while running, mesh bounds in the editor), so nothing falls "
                                                               "indoors. Off: they fall to the Ground Height plane"), "Surface Collision"),
        with_label(with_tip(f_bool("ground_effects", true), "Splashes and spray where rain lands, snow settling -- on objects with a "
                                                            "WeatherSurface component (and their children)"), "Ground Effects"),
        with_label(with_tip(f_bool("ground_height_splashes", false), "The Ground Height plane also takes splashes, where no object is below "
                                                                     "(scenes without colliders)"), "Plane Splashes"),
    };
    return f;
}

const std::vector<FieldDesc>& EditorApp::weather_snow_fields_() {
    static const std::vector<FieldDesc> f = {
        with_label(with_tip(f_float("initial_snow_cover", 0.0f, 0.01f, 0.0f, 1.0f), "Lying snow the scene starts with (0..1). A snowy "
                                                                                    "Start Condition starts fully covered anyway"), "Starting Snow"),
        with_label(with_tip(f_float("snow_accumulate_time", 180.0f, 1.0f, 1.0f, 100000.0f), "Seconds of full snowfall (below freezing) to full "
                                                                                              "cover on open, up-facing surfaces"), "Snow Builds (s)"),
        with_label(with_tip(f_float("snow_melt_time", 240.0f, 1.0f, 1.0f, 100000.0f), "Seconds full cover takes to melt at +5 C (faster when "
                                                                                        "warmer or raining)"), "Snow Melts (s)"),
        with_label(with_tip(f_float("snow_max_depth", 0.3f, 0.01f, 0.0f, 5.0f), "Metres of deep snow at full cover, on `snow`-shader surfaces"),
                   "Deep Snow (m)"),
        with_label(with_tip(f_bool("snow_auto_deformers", false), "Every Rigidbody leaves tracks in deep snow, not only objects with a "
                                                                  "SnowDeformer"), "Auto Snow Tracks"),
        with_label(with_tip(f_float("snow_trench_recover_time", 2.0f, 0.1f, 0.0f, 100000.0f), "Seconds a full-depth track takes to fill back "
                                                                                               "in. 0 = tracks only fill while it snows"), "Tracks Refill (s)"),
        with_label(with_tip(f_enum("snow_patch_style", {"soft", "hard"}), "soft: snow gathers with a soft, noisy edge. hard: round, "
                                                                              "crisp-edged patches (a stylized look) that grow and merge"),
                   "Snow Patches"),
        with_label(with_tip(f_float("snow_patch_size", 1.5f, 0.05f, 0.1f, 100.0f), "Hard patches: typical patch diameter (m)"), "Patch Size (m)"),
    };
    return f;
}

const std::vector<FieldDesc>& EditorApp::weather_condition_fields_(int part) {
    static const std::vector<FieldDesc> timing = {
        with_tip(f_string("name", "condition"), "What code and the Start Condition call it (unique)"),
        with_tip(f_float("weight", 1.0f, 0.01f, 0.0f, 1000.0f), "Random schedule: how likely it is picked. 0 = only on request"),
        with_label(with_tip(f_float("duration_min", 4.0f, 0.05f, 0.05f, 100000.0f), "Real minutes it lasts before the schedule moves on: at least..."), "Lasts (min)"),
        with_label(with_tip(f_float("duration_max", 10.0f, 0.05f, 0.05f, 100000.0f), "...at most"), "Lasts (max)"),
        with_label(with_tip(f_float("transition", 20.0f, 0.1f, 0.0f, 3600.0f), "Seconds to blend into it from whatever was showing"), "Transition (s)"),
        with_tip(f_strings("next"), "Random schedule: the conditions that may follow it (empty = any). List itself to let it repeat"),
    };
    static const std::vector<FieldDesc> sky = {
        with_tip(f_float("cloud_cover", 0.0f, 0.01f, 0.0f, 1.0f), "Greys and darkens the sky (see the swatches above)"),
        with_label(with_tip(f_float("sun", 1.0f, 0.01f, 0.0f, 4.0f), "Multiplier on the clock's sun / moon light"), "Sunlight"),
        with_tip(f_float("ambient", 1.0f, 0.01f, 0.0f, 4.0f), "Multiplier on the sky's ambient light"),
        with_label(with_tip(f_color("sky_tint", glm::vec3(1.0f)), "Multiplies the whole sky gradient"), "Sky Tint"),
    };
    static const std::vector<FieldDesc> fog = {
        with_label(with_tip(f_float("fog_density", 0.004f, 0.0005f, 0.0f, 10.0f), "Global fog (exponential squared)"), "Density"),
        with_label(with_tip(f_color("fog_color", glm::vec3(0.68f, 0.75f, 0.84f)), "Its daylight colour; the clock darkens it at night"), "Colour"),
        with_label(with_tip(f_float("fog_height_falloff", 0.0f, 0.05f, 0.0f, 1000.0f), "> 0: fog thins above render fog_height_base over this many metres"), "Height Falloff"),
        with_label(with_tip(f_float("fog_sky_blend", 0.6f, 0.01f, 0.0f, 1.0f), "How much the fog covers the sky"), "Sky Blend"),
        with_label(with_tip(f_float("fog_max_opacity", 1.0f, 0.01f, 0.0f, 1.0f), "Caps how opaque the fog gets in the distance"), "Max Opacity"),
        with_label(with_tip(f_float("fog_sun_amount", 0.4f, 0.01f, 0.0f, 16.0f), "Glow in the fog toward the sun"), "Sun Glow"),
    };
    static const std::vector<FieldDesc> wind = {
        with_label(with_tip(f_float("wind_strength", 1.5f, 0.05f, 0.0f, 200.0f), "m/s; drifts rain, snow and mist, and is in WeatherState::wind for gameplay"), "Wind (m/s)"),
        with_label(with_tip(f_float("wind_heading", 30.0f, 0.5f, -360.0f, 360.0f), "Degrees the wind blows toward, counter-clockwise from +X"), "Wind Heading"),
        with_label(with_tip(f_float("wind_gust", 0.2f, 0.01f, 0.0f, 1.0f), "How much the wind varies"), "Gusts"),
    };
    static const std::vector<FieldDesc> climate = {
        with_label(with_tip(f_float("temperature", 18.0f, 0.1f, -100.0f, 100.0f), "Degrees C. At or below 0 precipitation falls as snow and lies"), "Temperature (C)"),
        with_tip(f_float("precipitation", 0.0f, 0.01f, 0.0f, 1.0f), "0..1 how hard it rains / snows, for gameplay (WeatherReactor min_precipitation) and snow build-up"),
        with_tip(f_float("wetness", 0.0f, 0.01f, 0.0f, 1.0f), "How wet surfaces get; WeatherState::wetness creeps toward it"),
        with_label(with_tip(f_float("lightning", 0.0f, 0.1f, 0.0f, 600.0f), "Flashes per minute"), "Lightning (/min)"),
    };
    static const std::vector<FieldDesc> effects = {
        with_tip(f_items("effects", {
            with_tip(f_asset("prefab", "objects", ".yaml", true), "The object asset spawned: its particle rates and volume densities are scaled by the intensity"),
            with_tip(f_float("intensity", 1.0f, 0.01f, 0.0f, 100.0f), "Scales emission / density"),
            with_tip(f_enum("follow", {"camera", "ground", "world"}), "camera: around the viewer + offset; ground: under the viewer at ground height + offset.z; world: at offset"),
            f_vec3("offset", glm::vec3(0.0f), 0.05f),
            with_tip(f_float("wind_influence", 1.0f, 0.01f, 0.0f, 10.0f), "How much of the wind drifts it"),
        }), "Runtime effects while this condition is active, faded with its blend"),
    };
    switch (part) {
        case 0: return timing;
        case 1: return sky;
        case 2: return fog;
        case 3: return wind;
        case 4: return climate;
        default: return effects;
    }
}

void EditorApp::draw_weather_section_(imm::Context& ctx) {
    using I = imm::Icon;
    if (!weather_scene_()) return;
    Node block = weather_block_();
    bool on = get_bool(block, "enabled", false);
    const bool was = on;
    const bool open = ctx.collapsing_header("Weather & Time of Day", true, nullptr, I::Sun, &on);
    ctx.tooltip("Weather & Time of Day\nA clock (sun, moon, sky) and weather conditions that blend into each other, "
                "driving the sky, ambient light, fog, the sun and runtime effects (rain, snow, mist).\n"
                "Saved in this scene (scene.settings.weather). The checkbox switches it " + std::string(was ? "off" : "on"));
    test_rects_["weather_header"] = ctx.last_rect();
    if (on != was && !playing()) {
        if (on) enable_weather_();
        else { block["enabled"] = Node(false); set_weather_block_(&block, "Disable Weather"); }
    }
    if (!open) return;
    ctx.indent(6);
    if (!on) {
        ctx.label_dim("Off: the scene uses the Lighting & Sky and Fog settings.");
        ctx.label_dim("Switch it on to drive them from a clock and weather conditions.");
        ctx.unindent(6);
        ctx.spacing(4);
        return;
    }
    const InspectorEnv env = inspector_env_();
    const toy::weather::Settings parsed = toy::weather::parse_settings(block);
    std::vector<std::string> names;
    for (const auto& c : parsed.conditions) names.push_back(c.name);
    toy::weather::WeatherSystem* w = live_weather_();

    ctx.push_id("weather_ui");
    draw_weather_status_(ctx, parsed, w);
    if (weather_group_(ctx, "Live Preview", I::Play, true, "Try the weather out in the editor. Nothing here is saved.")) {
        ctx.indent(4);
        draw_weather_preview_(ctx, names, w);
        ctx.unindent(4);
    }
    if (weather_group_(ctx, "Time & Sun", I::Sun, true, "The clock, and the path the sun takes across the sky.")) {
        ctx.indent(4);
        draw_weather_fields_(ctx, weather_time_fields_(), env);
        if (parsed.day_length_minutes > 0.0f) {
            char hint[96];
            std::snprintf(hint, sizeof(hint), "1 game hour = %.3g real min; a full day = %.3g min", parsed.day_length_minutes / 24.0f,
                          parsed.day_length_minutes);
            ctx.label_dim(hint);
        } else {
            ctx.label_dim("Time stands still at the start time.");
        }
        weather_subhead_(ctx, "Sun Path");
        draw_weather_sun_path_(ctx, parsed, w);
        draw_weather_fields_(ctx, weather_sun_fields_(), env);
        weather_subhead_(ctx, "Moon");
        draw_weather_fields_(ctx, weather_moon_fields_(), env);
        ctx.unindent(4);
    }
    if (weather_group_(ctx, "Sky Colours", I::Palette, true, "The sky's gradient by day, at sunrise / sunset and at night; the clock "
                                                             "blends them by the sun's height.")) {
        ctx.indent(4);
        draw_weather_palettes_(ctx, parsed, env);
        ctx.unindent(4);
    }
    if (weather_group_(ctx, "Conditions", I::World, true, "The weather states (clear, rain, snow...) and how the scene moves between them.")) {
        ctx.indent(4);
        draw_weather_fields_(ctx, weather_schedule_fields_(names), env);
        draw_weather_conditions_(ctx, env, parsed);
        ctx.unindent(4);
    }
    if (weather_group_(ctx, "Rain & Snow Landing", I::Terrain, false, "Where precipitation stops, and the splashes it makes.")) {
        ctx.indent(4);
        draw_weather_fields_(ctx, weather_ground_fields_(), env);
        ctx.unindent(4);
    }
    if (weather_group_(ctx, "Lying Snow", I::Texture, false, "How snow builds up, melts and takes tracks.")) {
        ctx.indent(4);
        draw_weather_fields_(ctx, weather_snow_fields_(), env);
        ctx.unindent(4);
    }
    ctx.pop_id();
    ctx.unindent(6);
    ctx.spacing(4);
}

void EditorApp::draw_weather_status_(imm::Context& ctx, const toy::weather::Settings& st, toy::weather::WeatherSystem* w) {
    ctx.spacing(2);
    if (!w || !w->enabled()) {
        ctx.label_dim("Starting...");
        draw_weather_day_strip_(ctx, st, nullptr);
        return;
    }
    const toy::weather::WeatherState& s = w->state();
    // A swatch of the sky right now, then "16:42  dusk  /  rain -> storm".
    const float rh = ctx.style.row_height;
    const imm::Box row = ctx.next_box(rh * 2 + 2);
    const imm::Box sw{row.x, row.y, rh * 2 + 10, row.h};
    const toy::weather::Condition* cond = st.find(s.condition);
    const toy::weather::SkyFrame f = cond ? weather_condition_sky_(st, *cond, s.hour) : toy::weather::evaluate_sky(st, s.hour);
    draw_weather_dome_(ctx, sw, f.zenith, f.horizon, f.ground, f.sun_height, f.moon_light ? st.moon_color : glm::vec3(1.0f, 0.9f, 0.6f));
    const float tx = sw.right() + 8;
    std::string head = weather_hhmm_(s.hour) + "   " + toy::weather::phase_name(s.phase) + "   day " + std::to_string(s.day);
    ctx.text_in({tx, row.y, row.right() - tx, rh}, head, ctx.style.text, 0.0f);
    std::string c = s.condition;
    if (s.transition < 1.0f) c = (s.previous.empty() ? std::string("?") : s.previous) + "  ->  " + s.condition;
    ctx.text_in({tx, row.y + rh, row.right() - tx, rh}, c, ctx.style.text_dim, 0.0f);
    if (s.transition < 1.0f) {
        const imm::Box bar{tx, row.bottom() - 3, std::max(0.0f, row.right() - tx), 3};
        ctx.fill(bar, ctx.style.field);
        ctx.fill({bar.x, bar.y, bar.w * std::clamp(s.transition, 0.0f, 1.0f), bar.h}, ctx.style.accent);
    }
    char air[128];
    std::snprintf(air, sizeof(air), "%.0f C  wind %.1f  cloud %.0f%%  precip %.0f%%  wet %.0f%%  snow %.0f%%", s.temperature,
                  s.wind_speed(), s.cloud_cover * 100.0f, s.precipitation * 100.0f, s.wetness * 100.0f, s.snow_cover * 100.0f);
    ctx.spacing(2);
    const imm::Box ab = ctx.next_box(rh - 2);
    ctx.text_in(ab, air, ctx.style.text_disabled, 0.0f);
    ctx.tooltip("Right now\nThe live weather: temperature (C), wind (m/s), cloud cover, precipitation (rain or snow), how wet surfaces are, lying snow");
    ctx.spacing(2);
    draw_weather_day_strip_(ctx, st, w);
}

void EditorApp::draw_weather_preview_(imm::Context& ctx, const std::vector<std::string>& names, toy::weather::WeatherSystem* w) {
    using I = imm::Icon;
    if (!w || !w->enabled()) { ctx.label_dim("Starting..."); return; }
    const toy::weather::WeatherState& s = w->state();
    float hour = w->time_of_day();
    if (ctx.slider_float("Time##wpreview", &hour, 0.0f, 24.0f, "%.2f h")) w->set_time(hour);
    ctx.tooltip("Time\nScrub the live clock (the scene's Start Time is unchanged). The strip above does the same");
    int idx = 0;
    for (size_t i = 0; i < names.size(); ++i) if (names[i] == w->condition()) idx = static_cast<int>(i);
    if (ctx.combo("Condition##wpreview", &idx, names) && idx >= 0 && idx < static_cast<int>(names.size())) {
        w->set_condition(names[static_cast<size_t>(idx)]);
    }
    ctx.tooltip("Condition\nBlend the live weather into this condition over its transition (not saved)");
    static const std::vector<std::string> speeds = {"1x (as authored)", "4x", "16x", "Instant"};
    static const float speed_values[] = {1.0f, 4.0f, 16.0f, 0.0f};
    int speed = std::clamp(weather_preview_speed_, 0, 3);
    if (ctx.combo("Blend Speed##wpreview", &speed, speeds) && speed >= 0 && speed < 4) weather_preview_speed_ = speed;
    // Re-applied every frame: a scene rebuild makes a fresh system at 1x.
    w->set_transition_speed(speed_values[std::clamp(weather_preview_speed_, 0, 3)]);
    ctx.tooltip("Blend Speed\nFast-forwards blends between conditions in the editor (not saved; the game uses each "
                "condition's own Transition)");
    test_rects_["weather_transition_speed"] = ctx.last_rect();
    float cover = s.snow_cover;
    if (ctx.slider_float("Lying Snow##wpreview", &cover, 0.0f, 1.0f, "%.2f")) w->set_snow_cover(cover);
    ctx.tooltip("Lying Snow\nSet the snow on the ground now; the weather builds or melts it from here (not saved)");
    bool run = w->editor_preview();
    if (ctx.property_bool("Run Clock & Schedule##wpreview", &run)) w->set_editor_preview(run);
    ctx.tooltip("Run Clock & Schedule\nIn edit mode the clock and the schedule stand still; this runs them as Play would");
    if (ctx.property_bool("Show Effects##wpreview", &weather_preview_effects_)) w->set_preview_effects(weather_preview_effects_);
    ctx.tooltip("Show Effects\nRain, snow, mist and the rest in the editor. Off, they wait for Play "
                "(the sky, sun, fog and lying snow still show)");
    ctx.spacing(2);
    const float half = std::floor((ctx.available_width() - ctx.style.spacing * 2) * 0.5f);
    if (ctx.button("Finish Blend", half, w->transitioning(), I::ArrowRight)) w->finish_transition();
    ctx.tooltip("Finish Blend\nJump the running blend to its target condition now");
    ctx.same_line();
    if (ctx.button("Reset to Start", half, true, I::Restart)) {
        const toy::weather::Settings st = toy::weather::parse_settings(weather_block_());
        w->set_time(st.time_of_day);
        w->set_condition(st.condition, 0.0f);
    }
    ctx.tooltip("Reset to Start\nBack to the scene's Start Time and Start Condition");
}

void EditorApp::draw_weather_palettes_(imm::Context& ctx, const toy::weather::Settings& st, const InspectorEnv& env) {
    static const char* titles[] = {"Day", "Twilight", "Night"};
    const float gap = 6, lab = ctx.style.row_height - 2;
    const imm::Box row = ctx.next_box(58 + lab);
    const float cw = std::floor((row.w - gap * 2) / 3.0f);
    weather_palette_ = std::clamp(weather_palette_, 0, 2);
    for (int i = 0; i < 3; ++i) {
        const imm::Box cell{row.x + i * (cw + gap), row.y, cw, row.h};
        const imm::Box dome{cell.x + 3, cell.y + 3, cell.w - 6, 52};
        // Twilight: the day / night mix with the glow on top, as the sky looks with the sun on the horizon.
        glm::vec3 z, h, g;
        float sun = -2.0f;
        if (i == 0) { z = st.day_zenith; h = st.day_horizon; g = st.day_ground; sun = 0.6f; }
        else if (i == 2) { z = st.night_zenith; h = st.night_horizon; g = st.night_ground; }
        else {
            const float d = 0.42f;   // daylight with the sun on the horizon
            z = glm::mix(glm::mix(st.night_zenith, st.day_zenith, d), st.twilight_zenith, 0.5f);
            h = glm::mix(glm::mix(st.night_horizon, st.day_horizon, d), st.twilight_horizon, 0.85f);
            g = glm::mix(st.night_ground, st.day_ground, d);
            sun = 0.02f;
        }
        bool hov = false;
        ctx.push_id(static_cast<int64_t>(i));
        const bool click = ctx.invisible_button("palette", cell, &hov);
        ctx.pop_id();
        if (click) weather_palette_ = i;
        const bool sel = weather_palette_ == i;
        ctx.fill_rounded(cell, sel ? ctx.style.selection : hov ? ctx.style.row_hover : ctx.style.panel_alt, 4);
        draw_weather_dome_(ctx, dome, z, h, g, sun, i == 1 ? glm::vec3(1.0f, 0.55f, 0.25f) : glm::vec3(1.0f, 0.95f, 0.8f));
        ctx.text_in({cell.x, dome.bottom() + 1, cell.w, lab}, titles[i], sel ? ctx.style.text : ctx.style.text_dim, 0.0f, true);
        test_rects_[std::string("weather_palette:") + titles[i]] = cell;
        if (hov) ctx.tooltip(std::string(titles[i]) + " sky\nClick to edit its colours.\n" +
                             (i == 0 ? "Shown with the sun more than ~15 deg up."
                              : i == 1 ? "Mixed in while the sun is near the horizon: sunrise and sunset."
                                       : "Shown with the sun more than ~15 deg below the horizon."));
    }
    ctx.spacing(2);
    draw_weather_fields_(ctx, weather_palette_fields_(weather_palette_), env);
    if (weather_palette_ == 1) ctx.label_dim("The ground uses the day / night mix at twilight.");
    weather_subhead_(ctx, "Brightness");
    draw_weather_fields_(ctx, weather_brightness_fields_(), env);
}

std::string EditorApp::weather_condition_tags_(const toy::weather::Condition& c) {
    std::string t;
    auto add = [&](const char* s) { t += (t.empty() ? "" : ", ") + std::string(s); };
    if (c.precipitation > 0.01f) add(c.temperature <= 0.0f ? "snow" : "rain");
    if (c.lightning > 0.0f) add("lightning");
    if (c.fog_density > 0.015f) add("fog");
    if (c.cloud_cover > 0.5f && c.precipitation <= 0.01f) add("cloudy");
    if (c.wind_strength >= 8.0f) add("windy");
    return t;
}

void EditorApp::draw_weather_conditions_(imm::Context& ctx, const InspectorEnv& env, const toy::weather::Settings& st) {
    using I = imm::Icon;
    Node block = weather_block_();
    if (!block.contains("conditions") || !block.at("conditions").is_sequence()) {
        ctx.label_dim("Using the engine's stock conditions.");
        if (ctx.button("Customize Conditions", -1, !playing(), I::Plus)) {
            Node list = Node::sequence();
            for (const auto& c : toy::weather::default_conditions()) list.as_seq().push_back(toy::weather::to_node(c));
            block["conditions"] = list;
            set_weather_block_(&block, "Customize Weather Conditions");
        }
        return;
    }
    Node list = block.at("conditions");
    const int count = static_cast<int>(list.size());
    weather_sel_ = std::clamp(weather_sel_, 0, std::max(0, count - 1));
    const std::string start = get_string(block, "condition");
    const toy::weather::WeatherSystem* live = live_weather_();
    ctx.spacing(4);
    // One row per condition: a swatch of its sky at noon, its name, what it does, and start / live badges.
    const float rh = ctx.style.row_height + 4;
    for (int i = 0; i < count; ++i) {
        const Node& node = list.as_seq()[static_cast<size_t>(i)];
        const toy::weather::Condition c = toy::weather::parse_condition(node);
        ctx.push_id(static_cast<int64_t>(i));
        const imm::Box row = ctx.next_box(rh);
        bool hov = false;
        if (ctx.invisible_button("cond_row", row, &hov)) weather_sel_ = i;
        const bool sel = i == weather_sel_;
        if (sel) ctx.fill_rounded(row, ctx.style.selection, 3);
        else if (hov) ctx.fill_rounded(row, ctx.style.row_hover, 3);
        const toy::weather::SkyFrame f = weather_condition_sky_(st, c, 12.0f);   // at noon: the clearest difference
        draw_weather_dome_(ctx, {row.x + 3, row.y + 3, rh * 1.4f, rh - 6}, f.zenith, f.horizon, f.ground);
        const float tx = row.x + rh * 1.4f + 9;
        const std::string tags = weather_condition_tags_(c);
        std::string badge;
        if (live && live->condition() == c.name) badge = "live";
        if (c.name == start) badge += badge.empty() ? "start" : " / start";
        const float bw = badge.empty() ? 0.0f : ctx.text_width(badge) + 12;
        const float nw = ctx.text_width(c.name);
        ctx.text_in({tx, row.y, row.right() - tx - bw, rh}, c.name, ctx.style.text, 0.0f);
        if (!tags.empty()) ctx.text_in({tx + nw + 8, row.y, std::max(0.0f, row.right() - tx - nw - 8 - bw), rh}, tags, ctx.style.text_disabled, 0.0f);
        if (!badge.empty()) {
            const imm::Box bb{row.right() - bw - 2, row.y + 4, bw - 2, rh - 8};
            ctx.fill_rounded(bb, badge.rfind("live", 0) == 0 ? glm::vec4(0.25f, 0.55f, 0.3f, 1.0f) : ctx.style.button, 3);
            ctx.text_in(bb, badge, ctx.style.text, 0.0f, true);
        }
        test_rects_["weather_condition:" + c.name] = row;
        if (hov) {
            char wt[32];
            std::snprintf(wt, sizeof(wt), "%.2g", c.weight);
            ctx.tooltip(c.name + "\n" + (tags.empty() ? std::string("clear") : tags) + "\n" +
                        (badge.empty() ? std::string() : "(" + badge + ")\n") + "random-schedule weight " + wt + "\nClick to edit it below");
        }
        ctx.pop_id();
    }
    const bool ok = !playing();
    auto rewrite = [&](Node new_list, const std::string& label) {
        Node b = weather_block_();
        b["conditions"] = std::move(new_list);
        set_weather_block_(&b, label);
    };
    auto unique_name = [&](std::string base) {
        std::set<std::string> used;
        for (const auto& c : list.as_seq()) used.insert(get_string(c, "name"));
        std::string n = base;
        for (int k = 2; used.count(n); ++k) n = base + "_" + std::to_string(k);
        return n;
    };
    const float bw = 26;
    ctx.spacing(2);
    const imm::Box bar = ctx.next_box(bw);
    auto tool = [&](const char* id, I icon, const char* tip, int slot, bool enabled) {
        return ctx.icon_button(id, icon, tip, false, bw - 2, imm::Context::kAll, imm::Box{bar.x + slot * bw, bar.y, bw - 2, bw - 2}) && enabled;
    };
    if (tool("wc_add", I::Plus, "Add Condition\nA new clear-sky condition", 0, ok)) {
        toy::weather::Condition c;
        c.name = unique_name("condition");
        c.weight = 0.0f;
        Node nl = list;
        nl.as_seq().push_back(toy::weather::to_node(c));
        weather_sel_ = count;
        rewrite(nl, "Add Weather Condition");
    } else if (tool("wc_dup", I::Duplicate, "Duplicate Condition", 1, ok && count > 0)) {
        Node nl = list;
        Node copy = list.as_seq()[static_cast<size_t>(weather_sel_)];
        copy["name"] = Node(unique_name(get_string(copy, "name", "condition")));
        nl.as_seq().insert(nl.as_seq().begin() + weather_sel_ + 1, copy);
        ++weather_sel_;
        rewrite(nl, "Duplicate Weather Condition");
    } else if (tool("wc_del", I::Trash, "Remove Condition\nThe last one can't be removed", 2, ok && count > 1)) {
        Node nl = list;
        nl.as_seq().erase(nl.as_seq().begin() + weather_sel_);
        weather_sel_ = std::max(0, weather_sel_ - 1);
        rewrite(nl, "Remove Weather Condition");
    } else if (tool("wc_up", I::Undo, "Move Up", 3, ok && weather_sel_ > 0)) {
        Node nl = list;
        std::swap(nl.as_seq()[static_cast<size_t>(weather_sel_)], nl.as_seq()[static_cast<size_t>(weather_sel_ - 1)]);
        --weather_sel_;
        rewrite(nl, "Move Weather Condition");
    } else if (tool("wc_down", I::Redo, "Move Down", 4, ok && weather_sel_ + 1 < count)) {
        Node nl = list;
        std::swap(nl.as_seq()[static_cast<size_t>(weather_sel_)], nl.as_seq()[static_cast<size_t>(weather_sel_ + 1)]);
        ++weather_sel_;
        rewrite(nl, "Move Weather Condition");
    } else if (tool("wc_reset", I::Restart, "Reset to Stock Conditions\nReplaces the list with the engine's defaults", 5, ok)) {
        Node nl = Node::sequence();
        for (const auto& c : toy::weather::default_conditions()) nl.as_seq().push_back(toy::weather::to_node(c));
        weather_sel_ = 0;
        rewrite(nl, "Reset Weather Conditions");
    }
    if (live && count > 0 && ok) {
        const std::string sel_name = get_string(list.as_seq()[static_cast<size_t>(weather_sel_)], "name");
        const float pw = std::min(110.0f, std::max(0.0f, bar.w - 6 * bw - 4));
        if (pw > 40 && ctx.icon_button("wc_preview", I::Play, "Preview It\nBlend the live weather into the selected condition (not saved)",
                                       live->condition() == sel_name, bw - 2, imm::Context::kAll,
                                       imm::Box{bar.right() - bw + 2, bar.y, bw - 2, bw - 2})) {
            if (auto* lw = live_weather_()) lw->set_condition(sel_name);
        }
    }
    if (count == 0) return;
    test_rects_["weather_condition_bar"] = bar;

    // The selected condition's profile. `duration: [min, max]` edits as two rows.
    list = weather_block_().at("conditions");
    if (weather_sel_ >= static_cast<int>(list.size())) return;
    Node item = list.as_seq()[static_cast<size_t>(weather_sel_)];
    const std::string old_name = get_string(item, "name");
    if (item.contains("duration") && item.at("duration").is_sequence() && item.at("duration").size() >= 2) {
        item["duration_min"] = item.at("duration").as_seq()[0];
        item["duration_max"] = item.at("duration").as_seq()[1];
    } else if (item.contains("duration")) {
        item["duration_min"] = item["duration_max"] = item.at("duration");
    }
    const toy::weather::Condition cond = toy::weather::parse_condition(list.as_seq()[static_cast<size_t>(weather_sel_)]);
    ctx.spacing(4);
    {   // A banner naming what's being edited.
        const imm::Box hb = ctx.next_box(ctx.style.row_height + 2);
        ctx.fill_rounded(hb, ctx.style.subpanel, 3);
        ctx.fill({hb.x, hb.y + 2, 3, hb.h - 4}, ctx.style.accent);
        ctx.text_in({hb.x + 10, hb.y, hb.w - 10, hb.h}, "Editing \"" + old_name + "\"", ctx.style.text, 0.0f);
    }
    ctx.push_id("wcond");
    static const char* parts[] = {"Timing", "Sky & Light", "Fog", "Wind", "Temperature & Rain", "Effects"};
    static const I icons[] = {I::Restart, I::Sun, I::World, I::ArrowRight, I::Info, I::Component};
    static const char* tips[] = {
        "How long it lasts and what may follow (random / cycle schedules)",
        "Clouds, light and the sky's tint while this condition shows",
        "The global fog it brings",
        "Wind speed and direction: drifts rain, snow and mist",
        "Temperature, precipitation, wetness and lightning",
        "Runtime objects it spawns: rain, snow, mist...",
    };
    EditResult total;
    for (int p = 0; p < 6; ++p) {
        if (!weather_group_(ctx, parts[p], icons[p], p == 1 || p == 3, tips[p])) continue;
        ctx.indent(4);
        if (p == 1) {   // the condition's sky through the day
            static const float hours[] = {7.0f, 12.0f, 18.5f, 0.0f};
            static const char* labels[] = {"Morning", "Noon", "Dusk", "Night"};
            const float gap = 4, lab = ctx.style.row_height - 4;
            const imm::Box row = ctx.next_box(40 + lab);
            const float cw = std::floor((row.w - gap * 3) / 4.0f);
            for (int k = 0; k < 4; ++k) {
                const toy::weather::SkyFrame f = weather_condition_sky_(st, cond, hours[k]);
                const imm::Box d{row.x + k * (cw + gap), row.y, cw, 40};
                draw_weather_dome_(ctx, d, f.zenith, f.horizon, f.ground, f.sun_height,
                                   f.moon_light ? st.moon_color : glm::vec3(1.0f, 0.92f, 0.7f));
                ctx.text_in({d.x, d.bottom(), d.w, lab}, labels[k], ctx.style.text_disabled, 0.0f, true);
            }
            if (ctx.is_hovered(row)) ctx.tooltip("This condition's sky\nThe clock's sky at 07:00, noon, 18:30 and midnight, with this "
                                                 "condition's clouds and tint (before fog)");
        }
        if (p == 2) {   // fog colour as the clock darkens it
            const imm::Box sw = ctx.next_box(14);
            const glm::vec3 fc = cond.fog_color;
            ctx.gradient(sw, weather_display_(fc * 0.04f), weather_display_(fc), weather_display_(fc), weather_display_(fc * 0.04f));
            ctx.outline(sw, ctx.style.border);
            if (ctx.is_hovered(sw)) ctx.tooltip("Fog colour\nNight (left) to day (right): the clock darkens the daylight colour at night");
        }
        if (p == 3) draw_weather_wind_dial_(ctx, item, total);
        for (const auto& f : weather_condition_fields_(p)) {
            ctx.push_id(f.key);
            total.absorb(draw_field(ctx, f, item, env));
            ctx.pop_id();
        }
        if (p == 4 && cond.precipitation > 0.01f) {
            ctx.label_dim(cond.temperature <= 0.0f ? "Falls as snow (at or below 0 C) and lies." : "Falls as rain (above 0 C).");
        }
        ctx.unindent(4);
    }
    ctx.pop_id();
    if (total.changed && ok) {
        if (item.contains("duration_min") || item.contains("duration_max")) {
            const float lo = std::max(0.05f, get_float(item, "duration_min", 4.0f));
            const float hi = std::max(lo, get_float(item, "duration_max", lo));
            Node d = Node::sequence();
            d.as_seq().push_back(make_float(lo));
            d.as_seq().push_back(make_float(hi));
            item["duration"] = d;
            erase_key(item, "duration_min");
            erase_key(item, "duration_max");
        }
        std::string name = get_string(item, "name");
        Node b = weather_block_();
        Node& nl = b["conditions"];
        // A rename must stay unique; references to the old name (start, next lists) follow it.
        bool clash = false;
        for (int i = 0; i < static_cast<int>(nl.size()); ++i) {
            if (i != weather_sel_ && get_string(nl.as_seq()[static_cast<size_t>(i)], "name") == name) clash = true;
        }
        if (name.empty() || clash) item["name"] = Node(name = old_name);
        nl.as_seq()[static_cast<size_t>(weather_sel_)] = item;
        if (name != old_name) {
            if (get_string(b, "condition") == old_name) b["condition"] = Node(name);
            for (auto& c : nl.as_seq()) {
                if (!c.contains("next") || !c.at("next").is_sequence()) continue;
                for (auto& n : c["next"].as_seq()) if (n.is_string() && n.get_value<std::string>() == old_name) n = Node(name);
            }
        }
        set_weather_block_(&b, "Edit weather condition " + name, total.active ? "weather_cond:" + total.key : std::string());
    }
    if (total.finished) doc_.end_merge();
}

void EditorApp::draw_weather_locked_row_(imm::Context& ctx, const FieldDesc& f, const std::string& section_name) {
    using I = imm::Icon;
    const imm::Box row = ctx.property_row(f.display() + "##locked_" + f.key);
    ctx.fill_rounded(row, ctx.style.panel_alt);
    ctx.icon(I::Lock, {row.x + 4, row.y + 3, row.h - 6, row.h - 6}, ctx.style.text_disabled);
    ctx.text_in({row.x + row.h + 2, row.y, row.w - row.h - 4, row.h}, "Set by Weather", ctx.style.text_disabled, 0.0f);
    const imm::Box hit{ctx.last_label_box().x, row.y, row.right() - ctx.last_label_box().x, row.h};
    if (ctx.is_hovered(hit)) {
        ctx.tooltip(f.display() + "\nLocked: the scene's weather drives it every frame (World > Weather & Time of Day).\n"
                    "Its own value comes back when the weather is switched off.\nconfig key: " + section_name + "." + f.key);
    }
    test_rects_["locked:" + f.key] = row;
}

std::vector<coopa::scene::SceneObject*> EditorApp::runtime_roots_() const {
    std::vector<coopa::scene::SceneObject*> out;
    coopa::scene::Scene* s = viewed_live_scene_();
    if (!s) return out;
    for (const auto& r : s->root_objects()) if (r->get_component<toy::scene::RuntimeObject>()) out.push_back(r.get());
    return out;
}

std::string EditorApp::runtime_path_(const coopa::scene::SceneObject& o) {
    std::string p = o.name();
    for (const coopa::scene::SceneObject* a = o.parent(); a; a = a->parent()) p = a->name() + ":" + p;
    return p;
}

coopa::scene::SceneObject* EditorApp::runtime_selected_() const {
    if (runtime_sel_.empty()) return nullptr;
    coopa::scene::Scene* s = viewed_live_scene_();
    coopa::scene::SceneObject* o = s ? s->find_object_by_path(runtime_sel_) : nullptr;
    return o && toy::scene::is_runtime_object(*o) ? o : nullptr;
}

void EditorApp::draw_outliner_runtime_(imm::Context& ctx) {
    using I = imm::Icon;
    const auto roots = runtime_roots_();
    if (roots.empty()) return;
    const glm::vec4 dim = ctx.style.text_disabled;
    auto head = ctx.tree_node(ctx.get_id("runtime_root"), "Runtime", false, false, true, &dim, I::Lock);
    ctx.tooltip("Runtime\nObjects systems create while the scene runs (weather effects, its sun). "
                "Locked: they are not part of the scene file and can't be edited -- change the system that makes them");
    test_rects_["outliner_runtime"] = head.rect;
    if (!head.open) return;
    for (coopa::scene::SceneObject* r : roots) draw_outliner_runtime_row_(ctx, *r);
    ctx.tree_pop();
}

void EditorApp::draw_outliner_runtime_row_(imm::Context& ctx, coopa::scene::SceneObject& o) {
    using I = imm::Icon;
    const std::string path = runtime_path_(o);
    ctx.push_id(path);
    const bool leaf = o.children().empty();
    const glm::vec4 col = ctx.style.text_dim;
    const glm::vec4 tint = ctx.style.text_disabled;
    const bool sel = runtime_sel_ == path;
    I icon = I::Empty;
    if (o.get_component<toy::particles::ParticleSystem>()) icon = I::Component;
    else if (o.get_component<coopa::gfx::engine::components::VolumeComponent>()) icon = I::World;
    else if (o.get_component<coopa::gfx::engine::components::DirectionalLightComponent>()) icon = I::Sun;
    auto r = ctx.tree_node(ctx.get_id("rt"), o.name(), leaf, sel, false, &col, icon, 24.0f, false, &tint);
    test_rects_["runtime:" + path] = r.rect;
    const float s = ctx.style.row_height - 2;
    ctx.icon(I::Lock, {r.rect.right() - s - 4, r.rect.y + 2, s - 2, s - 2}, ctx.style.text_disabled);
    const toy::scene::RuntimeObject* m = toy::scene::runtime_marker(o);
    ctx.tooltip(o.name() + "\nLocked -- created at runtime by " + (m ? m->owner_system : std::string("a system")) +
                (m && !m->note.empty() ? "\n" + m->note : std::string()));
    if (r.clicked) {
        doc_.clear_selection();
        runtime_sel_ = path;
        prop_tab_ = PropTab::Object;
    }
    if (r.open) {
        for (const auto& c : o.children()) draw_outliner_runtime_row_(ctx, *c);
        ctx.tree_pop();
    }
    ctx.pop_id();
}

void EditorApp::draw_runtime_object_props_(imm::Context& ctx, coopa::scene::SceneObject& o) {
    using I = imm::Icon;
    const toy::scene::RuntimeObject* m = toy::scene::runtime_marker(o);
    {
        imm::Box row = ctx.next_box(ctx.style.row_height + 2);
        ctx.icon(I::Lock, {row.x + 2, row.y + 3, row.h - 6, row.h - 6}, ctx.style.text_dim);
        ctx.text_in({row.x + row.h + 2, row.y, row.w - row.h - 2, row.h}, o.name(), ctx.style.text, 0.0f);
    }
    test_rects_["runtime_props"] = ctx.last_rect();
    ctx.label_dim("Runtime object -- locked (created by " + (m ? m->owner_system : std::string("a system")) + ", never saved)");
    if (m && !m->note.empty()) ctx.paragraph(m->note, &ctx.style.text_dim);
    auto value = [&](const std::string& k, const std::string& v) {
        const imm::Box b = ctx.property_row(k + "##rt_" + k);
        ctx.text_in(b, v, ctx.style.text_disabled, 0.0f);
    };
    char buf[128];
    if (ctx.collapsing_header("Transform", true, nullptr, I::Orientation)) {
        if (auto* tc = o.get_transform()) {
            const glm::vec3 p = glm::vec3(tc->transform().get_world_matrix()[3]);
            std::snprintf(buf, sizeof(buf), "%.2f  %.2f  %.2f", p.x, p.y, p.z);
            value("World Position", buf);
        }
    }
    if (auto* ps = o.get_component<toy::particles::ParticleSystem>(); ps && ctx.collapsing_header("ParticleSystem", true, nullptr, I::Component)) {
        std::snprintf(buf, sizeof(buf), "%.1f / s", ps->settings.rate); value("Rate", buf);
        std::snprintf(buf, sizeof(buf), "%u of %u", ps->particle_count(), ps->settings.max_particles); value("Particles", buf);
        const glm::vec3 v = ps->settings.velocity;
        std::snprintf(buf, sizeof(buf), "%.2f  %.2f  %.2f", v.x, v.y, v.z); value("Velocity", buf);
        value("Playing", ps->is_playing() ? "yes" : "no");
    }
    if (auto* v = o.get_component<coopa::gfx::engine::components::VolumeComponent>(); v && ctx.collapsing_header("Volume", true, nullptr, I::World)) {
        std::snprintf(buf, sizeof(buf), "%.4f", v->density); value("Density", buf);
        std::snprintf(buf, sizeof(buf), "%.2f m/s", v->speed); value("Drift", buf);
        std::snprintf(buf, sizeof(buf), "%.2f  %.2f  %.2f", v->extent.x, v->extent.y, v->extent.z); value("Extent", buf);
    }
    if (auto* l = o.get_component<coopa::gfx::engine::components::DirectionalLightComponent>(); l && ctx.collapsing_header("DirectionalLight", true, nullptr, I::Sun)) {
        std::snprintf(buf, sizeof(buf), "%.2f  %.2f  %.2f", l->direction.x, l->direction.y, l->direction.z); value("Direction", buf);
        std::snprintf(buf, sizeof(buf), "%.3f", l->intensity); value("Intensity", buf);
    }
    if (o.components().size() > 0) {
        std::string types;
        for (const auto& c : o.components()) {
            if (c->type_name() == "RuntimeObject") continue;
            types += (types.empty() ? "" : ", ") + c->type_name();
        }
        if (!types.empty()) value("Components", types);
    }
    if (!o.children().empty()) value("Children", std::to_string(o.children().size()));
}

} // namespace editor
} // namespace toy
