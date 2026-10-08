// editor/app/ui/weather.inl -- included inside EditorApp's class body.
//
// The scene's weather and time of day (toyengine/weather/) in the World tab, and the editor's
// side of "runtime objects" (toy::scene::RuntimeObject).
//
//   Weather     the scene's `scene.settings.weather` block: the switch in the section header,
//               then Preview (live only, never saved), Clock, Sky, Schedule and Conditions --
//               a list of the scene's conditions with an editor for the selected one's profile.
//               Every edit rewrites the block (one undo step; drags merge) and reaches the live
//               WeatherSystem at once (ChangeScope::Settings: nothing rebuilds).
//   Locked rows while the weather is on, the render keys it drives (weather::controlled_render_keys())
//               draw as locked rows in Render and World, and a DirectionalLight it drives says so.
//   Runtime     objects a system created (weather effects, its sun) are listed under a locked
//               "Runtime" row in the Hierarchy and shown read-only in Properties.

    // ---------------------------------------------------------------------------------
    // Weather block
    // ---------------------------------------------------------------------------------

    /** @brief The open scene's weather block (`scene.settings.weather`), or an empty mapping. */
    Node weather_block_() const {
        const Node st = doc_.scene_settings();
        if (st.is_mapping() && st.contains("weather") && st.at("weather").is_mapping()) return st.at("weather");
        return Node::mapping();
    }
    bool weather_scene_() const { return active_type_ == AssetType::Scene && !doc_.is_object_asset(); }
    /** @brief True while the open scene's weather is switched on. */
    bool weather_on_() const { return weather_scene_() && get_bool(weather_block_(), "enabled", false); }
    /** @brief True if the weather drives render `key` right now (its row is locked). */
    bool weather_locks_(const std::string& section, const std::string& key) const {
        return section == "render" && weather_on_() && toy::weather::controls_render_key(key);
    }
    /** @brief True if the weather drives the scene's directional light. */
    bool weather_drives_sun_() const { return weather_on_() && get_bool(weather_block_(), "drive_sun", true); }

    /** @brief The scene the viewport shows: the play scene while playing, else the edit scene. */
    coopa::scene::Scene* viewed_live_scene_() const { return play_scene_ ? play_scene_ : sync_.scene(); }
    /** @brief The live weather of the viewed scene, or null. */
    toy::weather::WeatherSystem* live_weather_() const {
        coopa::scene::Scene* s = viewed_live_scene_();
        return s ? toy::weather::find(*s) : nullptr;
    }

    /** @brief Replaces the weather block (null: removes it) as one undoable scene-settings edit. */
    void set_weather_block_(const Node* block, const std::string& label, const std::string& merge = {}) {
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

    /**
     * @brief Turns the weather on. The first time, the block is written out in full -- clock,
     *        palette and the stock conditions -- with its day sky and sun taken from the scene's
     *        current look, so switching it on at noon changes as little as possible.
     */
    void enable_weather_() {
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

    /** @brief A sub-heading inside the weather section (as settings groups draw theirs). */
    void weather_subhead_(imm::Context& ctx, const std::string& title) {
        ctx.spacing(3);
        const imm::Box hb = ctx.next_box(ctx.style.row_height - 4);
        ctx.text_in(hb, title, ctx.style.text_dim, 0.0f);
        const float tw = ctx.text_width(title) + 8;
        ctx.fill({hb.x + tw, hb.y + hb.h * 0.5f, std::max(0.0f, hb.w - tw), 1.0f}, ctx.style.separator);
    }

    /** @brief Draws `fields` against the block; any edit rewrites the block. */
    void draw_weather_fields_(imm::Context& ctx, const std::vector<FieldDesc>& fields, const InspectorEnv& env) {
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

    // --- field tables -----------------------------------------------------------------

    static const std::vector<FieldDesc>& weather_clock_fields_() {
        static const std::vector<FieldDesc> f = {
            with_label(with_tip(f_float("time_of_day", 10.0f, 0.02f, 0.0f, 24.0f), "Hour the clock starts at when the scene loads (0-24). Changing it jumps the live clock there"), "Start Time"),
            with_label(with_tip(f_float("day_length_minutes", 24.0f, 0.1f, 0.0f, 100000.0f), "Real minutes per game day. 0 stops time at the start time"), "Day Length (min)"),
            with_label(with_tip(f_bool("drive_sun", true), "Aim and colour the scene's directional light: the sun by day, the moon by night. "
                                                           "A scene without one gets a runtime Sun"), "Drive Sun"),
            with_label(with_tip(f_float("sun_intensity", 1.2f, 0.01f, 0.0f, 100.0f), "The sun's intensity at noon in clear weather"), "Sun Intensity"),
            with_label(with_tip(f_float("moon_intensity", 0.12f, 0.005f, 0.0f, 100.0f), "The moon's intensity at its highest"), "Moon Intensity"),
            with_label(f_color("moon_color", glm::vec3(0.55f, 0.65f, 0.9f)), "Moon Colour"),
            with_label(with_tip(f_float("latitude", 35.0f, 0.25f, -89.0f, 89.0f), "Tilts the sun's path: 0 = straight overhead at noon, higher = lower sun and longer shadows"), "Latitude"),
            with_label(with_tip(f_float("north_offset", 0.0f, 0.5f, -360.0f, 360.0f), "Turns the sun's path about Z. 0: sunrise toward +X"), "Sun Path Heading"),
        };
        return f;
    }
    static const std::vector<FieldDesc>& weather_sky_fields_() {
        static const std::vector<FieldDesc> f = {
            with_label(f_color("day_zenith", glm::vec3(0.05f, 0.18f, 0.55f)), "Day Zenith"),
            with_label(f_color("day_horizon", glm::vec3(0.25f, 0.35f, 0.45f)), "Day Horizon"),
            with_label(f_color("day_ground", glm::vec3(0.05f, 0.045f, 0.04f)), "Day Ground"),
            with_label(with_tip(f_color("twilight_zenith", glm::vec3(0.12f, 0.12f, 0.3f)), "Mixed in while the sun is near the horizon"), "Twilight Zenith"),
            with_label(with_tip(f_color("twilight_horizon", glm::vec3(0.85f, 0.42f, 0.2f)), "The sunrise / sunset glow; also warms the fog at twilight"), "Twilight Horizon"),
            with_label(f_color("night_zenith", glm::vec3(0.004f, 0.007f, 0.02f)), "Night Zenith"),
            with_label(f_color("night_horizon", glm::vec3(0.015f, 0.022f, 0.045f)), "Night Horizon"),
            with_label(f_color("night_ground", glm::vec3(0.006f, 0.006f, 0.008f)), "Night Ground"),
            with_label(with_tip(f_float("ambient_day", 1.0f, 0.01f, 0.0f, 16.0f), "Ambient and sky-reflection intensity at noon (render ambient_intensity / sky_intensity)"), "Ambient (Day)"),
            with_label(with_tip(f_float("ambient_night", 0.25f, 0.01f, 0.0f, 16.0f), "...at midnight"), "Ambient (Night)"),
            with_label(with_tip(f_float("night_exposure", 0.35f, 0.01f, 0.0f, 4.0f), "Multiplies the render exposure at midnight, so auto exposure "
                                                                                     "doesn't lift the night back to day. 1 = off"), "Night Exposure"),
        };
        return f;
    }
    std::vector<FieldDesc> weather_schedule_fields_(const std::vector<std::string>& names) const {
        FieldDesc start = with_label(with_tip(f_enum("condition", names), "The condition the scene starts in. Changing it switches the live weather at once"), "Start Condition");
        return {
            start,
            with_label(with_tip(f_enum("schedule", {"fixed", "random", "cycle"}),
                                "fixed: stays on the start condition (code calls set_condition()). random: when a condition's duration runs out, "
                                "a weighted pick among its Next list (or all). cycle: the next one in the list"), "Schedule"),
            with_tip(f_int("seed", 0, 0, 1000000), "Random schedule, lightning and gusts; 0 = a fixed default"),
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
    }
    static const std::vector<FieldDesc>& weather_condition_fields_(int part) {
        static const std::vector<FieldDesc> schedule = {
            with_tip(f_string("name", "condition"), "What code and the Start Condition call it (unique)"),
            with_tip(f_float("weight", 1.0f, 0.01f, 0.0f, 1000.0f), "Random schedule: how likely it is picked. 0 = only on request"),
            with_label(with_tip(f_float("duration_min", 4.0f, 0.05f, 0.05f, 100000.0f), "Real minutes it lasts before the schedule moves on: at least..."), "Lasts (min)"),
            with_label(with_tip(f_float("duration_max", 10.0f, 0.05f, 0.05f, 100000.0f), "...at most"), "Lasts (max)"),
            with_label(with_tip(f_float("transition", 20.0f, 0.1f, 0.0f, 3600.0f), "Seconds to blend into it from whatever was showing"), "Transition (s)"),
            with_tip(f_strings("next"), "Random schedule: the conditions that may follow it (empty = any). List itself to let it repeat"),
        };
        static const std::vector<FieldDesc> sky = {
            with_tip(f_float("cloud_cover", 0.0f, 0.01f, 0.0f, 1.0f), "Greys and darkens the sky"),
            with_label(with_tip(f_float("sun", 1.0f, 0.01f, 0.0f, 4.0f), "Multiplier on the clock's sun / moon light"), "Sunlight"),
            with_tip(f_float("ambient", 1.0f, 0.01f, 0.0f, 4.0f), "Multiplier on the sky's ambient light"),
            with_label(f_color("sky_tint", glm::vec3(1.0f)), "Sky Tint"),
        };
        static const std::vector<FieldDesc> fog = {
            with_label(with_tip(f_float("fog_density", 0.004f, 0.0005f, 0.0f, 10.0f), "Global fog (exponential squared)"), "Density"),
            with_label(with_tip(f_color("fog_color", glm::vec3(0.68f, 0.75f, 0.84f)), "Its daylight colour; the clock darkens it at night"), "Colour"),
            with_label(with_tip(f_float("fog_height_falloff", 0.0f, 0.05f, 0.0f, 1000.0f), "> 0: fog thins above render fog_height_base over this many metres"), "Height Falloff"),
            with_label(f_float("fog_sky_blend", 0.6f, 0.01f, 0.0f, 1.0f), "Sky Blend"),
            with_label(f_float("fog_max_opacity", 1.0f, 0.01f, 0.0f, 1.0f), "Max Opacity"),
            with_label(f_float("fog_sun_amount", 0.4f, 0.01f, 0.0f, 16.0f), "Sun Glow"),
        };
        static const std::vector<FieldDesc> air = {
            with_label(with_tip(f_float("wind_strength", 1.5f, 0.05f, 0.0f, 200.0f), "m/s; drifts rain, snow and mist, and is in WeatherState::wind for gameplay"), "Wind (m/s)"),
            with_label(with_tip(f_float("wind_heading", 30.0f, 0.5f, -360.0f, 360.0f), "Degrees the wind blows toward, counter-clockwise from +X"), "Wind Heading"),
            with_label(with_tip(f_float("wind_gust", 0.2f, 0.01f, 0.0f, 1.0f), "How much the wind varies"), "Gusts"),
            with_label(with_tip(f_float("temperature", 18.0f, 0.1f, -100.0f, 100.0f), "Degrees C, for gameplay"), "Temperature"),
            with_tip(f_float("precipitation", 0.0f, 0.01f, 0.0f, 1.0f), "0..1 how hard it rains / snows, for gameplay (WeatherReactor min_precipitation)"),
            with_tip(f_float("wetness", 0.0f, 0.01f, 0.0f, 1.0f), "How wet surfaces get; WeatherState::wetness creeps toward it"),
            with_label(with_tip(f_float("lightning", 0.0f, 0.1f, 0.0f, 600.0f), "Flashes per minute"), "Lightning"),
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
            case 0: return schedule;
            case 1: return sky;
            case 2: return fog;
            case 3: return air;
            default: return effects;
        }
    }

    // ---------------------------------------------------------------------------------
    // World tab: the Weather section
    // ---------------------------------------------------------------------------------

    void draw_weather_section_(imm::Context& ctx) {
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
        ctx.label_dim("Drives sky, ambient and fog (locked below) and the sun.");
        const InspectorEnv env = inspector_env_();
        const toy::weather::Settings parsed = toy::weather::parse_settings(block);
        std::vector<std::string> names;
        for (const auto& c : parsed.conditions) names.push_back(c.name);

        draw_weather_preview_(ctx, names);

        weather_subhead_(ctx, "Clock");
        draw_weather_fields_(ctx, weather_clock_fields_(), env);
        weather_subhead_(ctx, "Sky");
        draw_weather_fields_(ctx, weather_sky_fields_(), env);
        weather_subhead_(ctx, "Schedule");
        draw_weather_fields_(ctx, weather_schedule_fields_(names), env);
        draw_weather_conditions_(ctx, env);
        ctx.unindent(6);
        ctx.spacing(4);
    }

    /** @brief Live controls on the running WeatherSystem: never saved, gone on the next rebuild. */
    void draw_weather_preview_(imm::Context& ctx, const std::vector<std::string>& names) {
        using I = imm::Icon;
        weather_subhead_(ctx, "Preview (not saved)");
        toy::weather::WeatherSystem* w = live_weather_();
        if (!w || !w->enabled()) { ctx.label_dim("Starting..."); return; }
        const toy::weather::WeatherState& s = w->state();
        // Now: 16:42  day 0  dusk  |  rain -> storm 45%
        char clock[64];
        const int hh = static_cast<int>(s.hour), mm = static_cast<int>((s.hour - hh) * 60.0f);
        std::snprintf(clock, sizeof(clock), "%02d:%02d  day %d  %s", hh, mm, s.day, toy::weather::phase_name(s.phase));
        std::string cond = s.condition;
        if (s.transition < 1.0f) cond = (s.previous.empty() ? std::string("?") : s.previous) + " -> " + s.condition + "  " +
                                       std::to_string(static_cast<int>(s.transition * 100.0f)) + "%";
        ctx.label(std::string(clock) + "   " + cond);
        char air[96];
        std::snprintf(air, sizeof(air), "wind %.1f m/s  %.0f C  rain %.0f%%  wet %.0f%%  cloud %.0f%%", s.wind_speed(), s.temperature,
                      s.precipitation * 100.0f, s.wetness * 100.0f, s.cloud_cover * 100.0f);
        ctx.label_dim(air);

        float hour = w->time_of_day();
        if (ctx.slider_float("Time##wpreview", &hour, 0.0f, 24.0f, "%.2f h")) w->set_time(hour);
        ctx.tooltip("Time\nScrub the live clock (the scene's Start Time is unchanged)");
        int idx = 0;
        for (size_t i = 0; i < names.size(); ++i) if (names[i] == w->condition()) idx = static_cast<int>(i);
        if (ctx.combo("Condition##wpreview", &idx, names) && idx >= 0 && idx < static_cast<int>(names.size())) {
            w->set_condition(names[static_cast<size_t>(idx)]);
        }
        ctx.tooltip("Condition\nBlend the live weather into this condition over its transition (not saved)");
        // Fast-forward: transitions are tens of seconds, which is a long wait to judge a blend.
        static const std::vector<std::string> speeds = {"1x (as authored)", "4x", "16x", "Instant"};
        static const float speed_values[] = {1.0f, 4.0f, 16.0f, 0.0f};
        int speed = std::clamp(weather_preview_speed_, 0, 3);
        if (ctx.combo("Transition Speed##wpreview", &speed, speeds) && speed >= 0 && speed < 4) weather_preview_speed_ = speed;
        // Re-applied every frame: a scene rebuild makes a fresh system at 1x.
        w->set_transition_speed(speed_values[std::clamp(weather_preview_speed_, 0, 3)]);
        ctx.tooltip("Transition Speed\nFast-forwards blends between conditions in the editor (not saved; the game uses each "
                    "condition's own Transition)");
        test_rects_["weather_transition_speed"] = ctx.last_rect();
        if (ctx.button("Finish Transition", -1, w->transitioning(), I::ArrowRight)) w->finish_transition();
        ctx.tooltip("Finish Transition\nJump the running blend to its target condition now");
        bool run = w->editor_preview();
        if (ctx.property_bool("Run Clock & Schedule##wpreview", &run)) w->set_editor_preview(run);
        ctx.tooltip("Run Clock & Schedule\nIn edit mode the clock and the schedule stand still; this runs them as Play would");
        if (ctx.button("Reset to Scene Start", -1, true, I::Restart)) {
            const toy::weather::Settings st = toy::weather::parse_settings(weather_block_());
            w->set_time(st.time_of_day);
            w->set_condition(st.condition, 0.0f);
        }
    }

    /** @brief The condition list (select / add / duplicate / remove / reorder) and the selected one's profile. */
    void draw_weather_conditions_(imm::Context& ctx, const InspectorEnv& env) {
        using I = imm::Icon;
        weather_subhead_(ctx, "Conditions");
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
        for (int i = 0; i < count; ++i) {
            const Node& c = list.as_seq()[static_cast<size_t>(i)];
            const std::string name = get_string(c, "name", "condition");
            std::string label = name;
            if (name == start) label += "  (start)";
            if (live && live->condition() == name) label += "  *";
            char w[32];
            std::snprintf(w, sizeof(w), "w %.2g", get_float(c, "weight", 1.0f));
            ctx.push_id(static_cast<int64_t>(i));
            if (ctx.selectable(label, i == weather_sel_, 0.0f, I::World)) weather_sel_ = i;
            const imm::Box row = ctx.last_rect();
            ctx.text_in({row.right() - 60, row.y, 56, row.h}, w, ctx.style.text_dim, 1.0f);
            ctx.tooltip(name + "\n" + (live && live->condition() == name ? "* the live weather is in this condition\n" : "") +
                        "weight " + w + " (random schedule)");
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
        ctx.spacing(4);
        ctx.push_id("wcond");
        static const char* parts[] = {"Profile: Schedule", "Sky & Light", "Fog", "Atmosphere", "Effects"};
        EditResult total;
        for (int p = 0; p < 5; ++p) {
            weather_subhead_(ctx, p == 0 ? "\"" + old_name + "\" -- schedule" : parts[p]);
            for (const auto& f : weather_condition_fields_(p)) {
                ctx.push_id(f.key);
                total.absorb(draw_field(ctx, f, item, env));
                ctx.pop_id();
            }
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

    // ---------------------------------------------------------------------------------
    // Locked rows
    // ---------------------------------------------------------------------------------

    /** @brief A settings row the weather drives: greyed, a lock, not editable. */
    void draw_weather_locked_row_(imm::Context& ctx, const FieldDesc& f, const std::string& section_name) {
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

    // ---------------------------------------------------------------------------------
    // Runtime objects (toy::scene::RuntimeObject)
    // ---------------------------------------------------------------------------------

    /** @brief The viewed scene's marked root objects. */
    std::vector<coopa::scene::SceneObject*> runtime_roots_() const {
        std::vector<coopa::scene::SceneObject*> out;
        coopa::scene::Scene* s = viewed_live_scene_();
        if (!s) return out;
        for (const auto& r : s->root_objects()) if (r->get_component<toy::scene::RuntimeObject>()) out.push_back(r.get());
        return out;
    }
    static std::string runtime_path_(const coopa::scene::SceneObject& o) {
        std::string p = o.name();
        for (const coopa::scene::SceneObject* a = o.parent(); a; a = a->parent()) p = a->name() + ":" + p;
        return p;
    }
    /** @brief The selected runtime object, re-found by path each frame (it may come and go). */
    coopa::scene::SceneObject* runtime_selected_() const {
        if (runtime_sel_.empty()) return nullptr;
        coopa::scene::Scene* s = viewed_live_scene_();
        coopa::scene::SceneObject* o = s ? s->find_object_by_path(runtime_sel_) : nullptr;
        return o && toy::scene::is_runtime_object(*o) ? o : nullptr;
    }

    /** @brief The Hierarchy's "Runtime" row and its locked tree (nothing when there is none). */
    void draw_outliner_runtime_(imm::Context& ctx) {
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

    void draw_outliner_runtime_row_(imm::Context& ctx, coopa::scene::SceneObject& o) {
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

    /** @brief Properties for a runtime object: what made it, where it is, its components -- all read-only. */
    void draw_runtime_object_props_(imm::Context& ctx, coopa::scene::SceneObject& o) {
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
