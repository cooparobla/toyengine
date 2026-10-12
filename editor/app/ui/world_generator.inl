// editor/app/ui/world_generator.inl -- included inside EditorApp's class body.
//
// The World Generator: a TileTerrain's mapcoopa settings in one window -- the settings by
// category on the left and middle, a live preview of the map they make on the right, and
// Generate World, which writes them to the terrain (its `generator:` block) and re-bakes its
// tiles. A new terrain starts as mapcoopa's standard map (what a bare `mapcoopa` run makes).

public:
    /** @brief Opens the World Generator on TileTerrain object `id` (0: the selected object). */
    void open_world_generator(ObjectId id = 0);
    /** @brief Closes it without applying anything. */
    void close_world_generator() { wgen_close_ = true; }
    bool world_generator_open() const { return wgen_object_ != 0; }
    /** @brief The settings being edited (authored values), for an edit: the preview follows. Test hook. */
    coopa::maps::MapConfig& world_generator_config() {
        ++wgen_requested_;
        wgen_changed_at_ = 0.0;
        return wgen_config_;
    }
    /** @brief Whether the world being edited is a planet. Test hook. */
    bool& world_generator_planet() {
        ++wgen_requested_;
        wgen_changed_at_ = 0.0;
        return wgen_planet_;
    }
    /** @brief The world size being edited, in tiles per side. Test hook. */
    int& world_generator_world_tiles() { return wgen_world_tiles_; }
    /** @brief The preview's serial: bumps each time a preview lands. Test hook. */
    std::uint64_t world_generator_preview_serial() const { return wgen_preview_landed_; }
    /** @brief Writes the settings to the terrain and re-bakes it, as the Generate World button does. */
    void world_generator_generate() { wgen_generate_ = true; }

    /** @brief One map preview: generated and rendered off the main thread. */
    struct WorldPreview {
        int width = 0, height = 0;
        std::vector<std::uint8_t> rgb;   ///< width * height * 3, top row first.
        int land_percent = 0, lakes = 0, rivers = 0, towns = 0;
        std::uint64_t serial = 0;
    };

private:

    void draw_world_generator_modal_(imm::Context& ctx);
    /** @brief Starts a preview of the current settings when they changed (debounced), and lands a finished one. */
    void wgen_tick_preview_();
    /** @brief Writes the settings to the terrain's component (one undo step) and queues the re-bake. */
    void wgen_apply_();
    /** @brief The preview map, drawn in `box`. */
    void wgen_draw_preview_(imm::Context& ctx, const imm::Box& box);

    ObjectId wgen_object_ = 0;            ///< The terrain being edited; 0 when closed.
    bool wgen_close_ = false;             ///< close_world_generator(): close on the next draw.
    bool wgen_generate_ = false;          ///< world_generator_generate(): apply on the next draw.
    coopa::maps::MapConfig wgen_config_;  ///< Authored settings being edited.
    int wgen_world_tiles_ = 320;          ///< The terrain's own size in tiles per side, edited alongside.
    float wgen_tile_size_ = 1.0f;         ///< ...and its tiles' size in world units.
    float wgen_height_scale_ = 16.0f;
    float wgen_water_inset_ = 0.5f;
    float wgen_river_inset_ = 0.2f;
    bool wgen_planet_ = false;            ///< A planet: the map on a sphere, the tiles on a cube-sphere.
    int wgen_face_tiles_ = 128;           ///< ...its tiles along one cube face's edge.
    float wgen_gravity_ = 9.81f;          ///< ...and its pull at the surface, m/s^2.
    int wgen_category_ = 0;
    int wgen_layer_ = 0;                  ///< 0 composite, 1 biomes, 2 elevation, 3 water.
    std::uint64_t wgen_requested_ = 0;    ///< Bumped by every edit.
    std::uint64_t wgen_started_ = 0;      ///< The request the preview in flight (or last) was made for.
    double wgen_changed_at_ = 0.0;        ///< When the last edit happened (the debounce).
    std::uint64_t wgen_preview_landed_ = 0;
    std::future<WorldPreview> wgen_job_;  ///< The preview in flight.
    WorldPreview wgen_preview_;           ///< The last preview that landed.
    /// Generate World waits for the re-parsed terrain to carry the new settings before baking.
    ObjectId wgen_regen_object_ = 0;
    std::string wgen_regen_expect_;
    int wgen_regen_frames_ = 0;
