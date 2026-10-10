// editor/app/ui/weather.inl -- included inside EditorApp's class body.
//
// The scene's weather and time of day (toyengine/weather/) in the World tab, and the editor's
// side of "runtime objects" (toy::scene::RuntimeObject).
//
//   Weather     the scene's `scene.settings.weather` block: the switch in the section header,
//               then a live status card (the 24-hour sky strip scrubs the clock) and foldouts: Live Preview
//               (never saved), Time & Sun (with a sun-path diagram), Sky Colours (day / twilight /
//               night skies to pick from), Conditions (a list with sky swatches and the selected
//               one's profile, wind dial included), Rain & Snow Landing and Lying Snow.
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
    Node weather_block_() const;
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
    toy::weather::WeatherSystem* live_weather_() const;

    /** @brief Replaces the weather block (null: removes it) as one undoable scene-settings edit. */
    void set_weather_block_(const Node* block, const std::string& label, const std::string& merge = {});

    /**
     * @brief Turns the weather on. The first time, the block is written out in full -- clock,
     *        palette and the stock conditions -- with its day sky and sun taken from the scene's
     *        current look, so switching it on at noon changes as little as possible.
     */
    void enable_weather_();

    /** @brief A sub-heading inside a weather group (as settings groups draw theirs). */
    void weather_subhead_(imm::Context& ctx, const std::string& title);

    /** @brief A foldout inside the weather section; open state is kept per title. */
    bool weather_group_(imm::Context& ctx, const std::string& title, imm::Icon icon, bool default_open, const std::string& tip);

    /** @brief Draws `fields` against the block; any edit rewrites the block. */
    void draw_weather_fields_(imm::Context& ctx, const std::vector<FieldDesc>& fields, const InspectorEnv& env);

    // --- diagrams ---------------------------------------------------------------------

    /** @brief A linear sky colour as the swatches show it (clamped, gamma-encoded). */
    static glm::vec4 weather_display_(glm::vec3 c);
    static std::string weather_hhmm_(float hour);
    /** @brief The sky a condition shows at `hour`: the clock's gradient, greyed by its clouds and tinted (as WeatherSystem does). */
    static toy::weather::SkyFrame weather_condition_sky_(const toy::weather::Settings& st, const toy::weather::Condition& c, float hour);

    /** @brief A little sky: zenith at the top, the horizon across the middle, the ground below; an optional sun / moon disc. */
    void draw_weather_dome_(imm::Context& ctx, const imm::Box& b, glm::vec3 zenith, glm::vec3 horizon, glm::vec3 ground,
                            float sun_height = -2.0f, glm::vec3 disc = glm::vec3(1.0f));

    /**
     * @brief The 24-hour strip: the clock's sky every half hour, the sun's height as a curve,
     *        the scene's start time (a marker) and the live time (a line). Dragging scrubs the
     *        live clock; hovering says what that hour looks like.
     */
    void draw_weather_day_strip_(imm::Context& ctx, const toy::weather::Settings& st, toy::weather::WeatherSystem* w);

    /**
     * @brief The sun's path seen from above: the horizon is the circle, straight up is the
     *        centre. World +X / +Y are marked, so Latitude and Sun Path Heading read directly.
     */
    void draw_weather_sun_path_(imm::Context& ctx, const toy::weather::Settings& st, const toy::weather::WeatherSystem* w);

    /** @brief A Beaufort-ish name for a wind speed (m/s). */
    static const char* weather_wind_name_(float ms);

    /** @brief A dial for a condition's wind: the arrow is where it blows; dragging aims it (5 deg steps). */
    void draw_weather_wind_dial_(imm::Context& ctx, Node& item, EditResult& total);

    // --- field tables -----------------------------------------------------------------

    static const std::vector<FieldDesc>& weather_time_fields_();
    static const std::vector<FieldDesc>& weather_sun_fields_();
    static const std::vector<FieldDesc>& weather_moon_fields_();
    /** @brief One sky palette's colours: 0 day, 1 twilight, 2 night. */
    static const std::vector<FieldDesc>& weather_palette_fields_(int which);
    static const std::vector<FieldDesc>& weather_brightness_fields_();
    std::vector<FieldDesc> weather_schedule_fields_(const std::vector<std::string>& names) const;
    static const std::vector<FieldDesc>& weather_ground_fields_();
    static const std::vector<FieldDesc>& weather_snow_fields_();
    /** @brief A condition's profile, part by part: 0 timing, 1 sky, 2 fog, 3 wind, 4 climate, 5 effects. */
    static const std::vector<FieldDesc>& weather_condition_fields_(int part);

    // ---------------------------------------------------------------------------------
    // World tab: the Weather section
    // ---------------------------------------------------------------------------------

    void draw_weather_section_(imm::Context& ctx);

    /** @brief The "now" card: the live clock and condition, a few read-outs and the 24-hour strip. */
    void draw_weather_status_(imm::Context& ctx, const toy::weather::Settings& st, toy::weather::WeatherSystem* w);

    /** @brief Live controls on the running WeatherSystem: never saved, gone on the next rebuild. */
    void draw_weather_preview_(imm::Context& ctx, const std::vector<std::string>& names, toy::weather::WeatherSystem* w);

    /** @brief Day / Twilight / Night as three clickable skies; the picked one's colours below. */
    void draw_weather_palettes_(imm::Context& ctx, const toy::weather::Settings& st, const InspectorEnv& env);

    /** @brief Short tags for a condition's look: "rain", "snow", "storm", "fog", "cloudy". */
    static std::string weather_condition_tags_(const toy::weather::Condition& c);

    /** @brief The condition list (select / add / duplicate / remove / reorder) and the selected one's profile. */
    void draw_weather_conditions_(imm::Context& ctx, const InspectorEnv& env, const toy::weather::Settings& st);

    // ---------------------------------------------------------------------------------
    // Locked rows
    // ---------------------------------------------------------------------------------

    /** @brief A settings row the weather drives: greyed, a lock, not editable. */
    void draw_weather_locked_row_(imm::Context& ctx, const FieldDesc& f, const std::string& section_name);

    // ---------------------------------------------------------------------------------
    // Runtime objects (toy::scene::RuntimeObject)
    // ---------------------------------------------------------------------------------

    /** @brief The viewed scene's marked root objects. */
    std::vector<coopa::scene::SceneObject*> runtime_roots_() const;
    static std::string runtime_path_(const coopa::scene::SceneObject& o);
    /** @brief The selected runtime object, re-found by path each frame (it may come and go). */
    coopa::scene::SceneObject* runtime_selected_() const;

    /** @brief The Hierarchy's "Runtime" row and its locked tree (nothing when there is none). */
    void draw_outliner_runtime_(imm::Context& ctx);

    void draw_outliner_runtime_row_(imm::Context& ctx, coopa::scene::SceneObject& o);

    /** @brief Properties for a runtime object: what made it, where it is, its components -- all read-only. */
    void draw_runtime_object_props_(imm::Context& ctx, coopa::scene::SceneObject& o);
