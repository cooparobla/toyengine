// editor/app/ui/terrain.inl -- included inside EditorApp's class body.
//
// Terrain Mode: sculpting and painting a HeightmapTerrain (toyengine/world/heightmap_terrain.h).
// Entered from the mode menu (or the component's "Edit Terrain" button) with a terrain object
// active. Brushes are sized in metres and drawn as a circle draped over the ground; every tool
// writes the terrain's live HeightmapData, which the engine streams to the GPU each frame, so a
// stroke shows as it is painted. One undo step per stroke (or import / generate), in the scene's
// timeline. The terrain's files (heightmap and splat PNGs) are written with the scene (Ctrl S).
//
// Properties > Tool in this mode is the terrain panel: Tools, Brush, Layers, Terrain (size,
// height, chunk size, resolution -- applied with one button, since a change resamples the grid),
// Heightmap (import / export / generate) and Details (foliage scatter objects).

    // =================================================================================
    // Lifecycle
    // =================================================================================

    bool in_terrain_mode_() const { return terrain_mode_ && terrain_object_ != 0; }

    /** @brief The terrain component of the live terrain object (null when it is gone). */
    toy::world::HeightmapTerrain* terrain_live_() const;

    /** @brief True when `id` carries a HeightmapTerrain in the document. */
    bool is_terrain_object_(ObjectId id) const { return id != 0 && doc_.find_component(id, "HeightmapTerrain") >= 0; }

    void enter_terrain_mode_(ObjectId id);
    void exit_terrain_mode_();

    /** @brief Per frame: leaves the mode when the terrain went away or play started. */
    void terrain_frame_();

    // =================================================================================
    // Strokes
    // =================================================================================

    struct TerrainStroke {
        bool active = false;
        bool invert = false;
        TerrainTool tool = TerrainTool::Raise;
        float flatten_to = 0.0f;
        glm::vec2 last_px{0.0f};
        std::uint32_t seed = 1;
        std::shared_ptr<toy::world::HeightmapData> data;
        // The state before the stroke, to cut the undo patch from.
        std::vector<float> start_heights;
        std::vector<std::uint8_t> start_splat0, start_splat1;
        toy::world::IRect height_rect, splat_rect;
    };

    /** @brief Where the ray through `px` meets the terrain (world space). */
    bool terrain_hit_(const ViewProj& vp, glm::vec2 px, glm::vec3& world_hit) const;

    void terrain_dab_at_(const ViewProj& vp, glm::vec2 px);
    void terrain_begin_stroke_(const ViewProj& vp, glm::vec2 px, bool shift, bool ctrl);
    void terrain_continue_stroke_(const ViewProj& vp, glm::vec2 px);
    void terrain_end_stroke_();

    /** @brief Records an undo step that changed the whole terrain (import, generate). */
    void terrain_push_whole_(const std::string& label, const std::shared_ptr<toy::world::HeightmapData>& data,
                             const std::vector<float>& heights, const std::vector<std::uint8_t>& s0,
                             const std::vector<std::uint8_t>& s1);

    /** @brief scene_undo_() / scene_redo_()'s terrain half. */
    void terrain_apply_undo_(bool redo);

    // =================================================================================
    // Input / overlay / UI
    // =================================================================================

    void handle_terrain_input_(imm::Context& ctx, const ViewProj& vp, bool hovered);

    /** @brief The brush circle, draped over the ground, with the strength ring. */
    void draw_terrain_overlay_(imm::Context& ctx, const ViewProj& vp);

    /** @brief Header widgets in Terrain Mode: radius (m), strength. Returns the width used. */
    float draw_terrain_header_(imm::Context& ctx, float x, const imm::Box& hb);

    /** @brief The tool column of the toolbar (Terrain Mode). */
    void draw_terrain_toolbar_(imm::Context& ctx, const imm::Box& r);

    /** @brief Properties > Tool in Terrain Mode. */
    void draw_terrain_tool_panel_(imm::Context& ctx);

    void draw_terrain_layers_(imm::Context& ctx, toy::world::HeightmapTerrain& t);
    void draw_terrain_settings_(imm::Context& ctx, toy::world::HeightmapTerrain& t);
    void draw_terrain_heightmap_(imm::Context& ctx, toy::world::HeightmapTerrain& t);
    void draw_terrain_details_(imm::Context& ctx);

    /** @brief Edits the terrain's component node (one undo step; `merge` joins a drag). */
    void terrain_edit_component_(const std::string& label, const std::function<void(Node&)>& fn, const std::string& merge = {});

    /** @brief Adds a foliage scatter child (`kind`: grass, tree, rock) under the terrain. */
    void terrain_add_scatter_(const std::string& kind);

    /** @brief Writes every terrain with unsaved strokes (heightmap + splat PNGs). */
    void save_terrains_();

    TerrainBrushSettings terrain_brush_;
    TerrainStroke terrain_stroke_;
    SculptResize terrain_resize_;
    UndoStack<TerrainPatch> terrain_undo_{200};
    bool terrain_mode_ = false;
    ObjectId terrain_object_ = 0;
    // The Terrain section's staged dimensions (applied together; a change resamples the grid).
    struct TerrainStaged {
        ObjectId id = 0;
        glm::vec2 size{0.0f};
        float height = 0.0f, chunk = 0.0f;
        int resolution = 0, splat_resolution = 0;
    } terrain_staged_;
    int terrain_import_match_ = 1;   ///< Import: 1 = resize the grid to the image, 0 = resample the image.

public:
    TerrainBrushSettings& terrain_brush() { return terrain_brush_; }
    bool terrain_mode() const { return in_terrain_mode_(); }
    bool terrain_stroking() const { return terrain_stroke_.active; }
    void set_terrain_mode(bool on) { if (on) enter_terrain_mode_(doc_.primary()); else exit_terrain_mode_(); }
    const UndoStack<TerrainPatch>& terrain_history() const { return terrain_undo_; }

private:
