// editor/app/ui/tile_mode.inl -- included inside EditorApp's class body.
//
// Tile Mode: editing a TileTerrain's stored tiles (toyengine/world/tile_map_data.h). Entered from
// the mode menu (or the TileTerrain panel's "Edit Tiles") with a tile terrain active. Unlike
// Terrain Mode's round, metre-sized brushes, everything here snaps to tiles:
//
//   - square brushes of N x N tiles: Raise / Lower (whole steps, once per tile per stroke),
//     Flatten (to the clicked tile's height), Set Height, Paint (surface kind), Smooth
//     (terrace-aware) and Water (flood to a level / drain to the bed);
//   - Select: drag a rectangle (Shift adds, Ctrl removes), double-click selects the connected
//     tiles of the same height (Alt: same surface), A / Esc clear -- then Properties > Tool
//     edits every selected tile at once: height, surfaces, biome, climate, water, region;
//   - hover info: everything stored about the tile under the cursor.
//
// Every stroke and selection edit is one undo step in the scene's timeline; the tiles are
// written to their .tiles file with the scene (Ctrl S). Regenerate World re-bakes from the
// generator settings, undoably.

    // =================================================================================
    // Lifecycle
    // =================================================================================

    bool in_tile_mode_() const { return tile_mode_ && tile_object_ != 0; }

    /** @brief The live TileTerrain of the tile-mode object (null when it is gone). */
    toy::world::TerrainComponent* tile_live_() const;

    /** @brief True when `id` carries a TileTerrain (or legacy Terrain) in the document. */
    bool is_tile_terrain_object_(ObjectId id) const {
        return id != 0 && (doc_.find_component(id, "TileTerrain") >= 0 || doc_.find_component(id, "Terrain") >= 0);
    }

    void enter_tile_mode_(ObjectId id);
    void exit_tile_mode_();

    /** @brief Per frame: leaves the mode when the terrain went away or play started; lands a pending Regenerate's undo step. */
    void tile_frame_();

    // =================================================================================
    // Strokes
    // =================================================================================

    struct TileStroke {
        bool active = false;
        bool invert = false;
        TileTool tool = TileTool::Raise;
        int flatten_to = 0;
        std::shared_ptr<toy::world::TileMapData> data;
        std::vector<toy::world::TileRecord> start;   ///< The whole grid before the stroke (the undo patch is cut from it).
        std::vector<std::uint8_t> touched;           ///< Tiles already raised this stroke: each once.
        toy::world::IRect rect;                      ///< Everything the stroke changed.
        glm::ivec2 last_tile{-1};
        // Select drags.
        bool selecting = false;
        bool remove = false;
        glm::ivec2 sel_start{0};
    };

    /** @brief The tile under `px` (and the hit), from the live terrain's tiles. */
    bool tile_hit_(const ViewProj& vp, glm::vec2 px, glm::ivec2& tile, glm::vec3* world = nullptr) const;
    /** @brief World position of a tile corner at a height (in steps), for the overlay. */
    glm::vec3 tile_corner_(const toy::world::TerrainComponent& t, int x, int y, float steps) const;

    void tile_begin_stroke_(const ViewProj& vp, glm::vec2 px, bool shift, bool ctrl, bool alt, bool double_click);
    void tile_continue_stroke_(const ViewProj& vp, glm::vec2 px);
    void tile_end_stroke_();
    void tile_dab_(const glm::ivec2& tile);

    /** @brief Records an undo step from `before` (cut from a whole-grid copy) to the current tiles over `rect`. */
    void tile_push_undo_(const std::string& label, const std::shared_ptr<toy::world::TileMapData>& data,
                         const std::vector<toy::world::TileRecord>& whole_before, const toy::world::IRect& rect);
    /** @brief scene_undo_() / scene_redo_()'s tile half. */
    void tile_apply_undo_(bool redo);

    /** @brief Applies `edit` to every selected tile, as one undo step (`merge`: a drag's frames join one step). */
    void tile_edit_selection_(const std::string& label, const std::function<void(toy::world::TileRecord&)>& edit,
                              const std::string& merge = {});

    /** @brief Asks object `id`'s live terrain (0: the Tile Mode one) to re-bake its world from the generator settings (undoable once it lands). */
    void tile_regenerate_(ObjectId id = 0);

    /** @brief Writes every tile terrain with unsaved edits (their .tiles files). */
    void save_tile_maps_();

    // =================================================================================
    // Input / overlay / UI
    // =================================================================================

    void handle_tile_input_(imm::Context& ctx, const ViewProj& vp, bool hovered);
    /** @brief Hover / brush squares and the selection outline, on the tile tops. */
    void draw_tile_overlay_(imm::Context& ctx, const ViewProj& vp);
    /** @brief Header widgets in Tile Mode: brush size, strength. Returns the width used. */
    float draw_tile_header_(imm::Context& ctx, float x, const imm::Box& hb);
    /** @brief The tool column of the toolbar (Tile Mode). */
    void draw_tile_toolbar_(imm::Context& ctx, const imm::Box& r);
    /** @brief Properties > Tool in Tile Mode. */
    void draw_tile_tool_panel_(imm::Context& ctx);
    void draw_tile_selection_section_(imm::Context& ctx, toy::world::TerrainComponent& t);

    TileBrushSettings tile_brush_;
    TileStroke tile_stroke_;
    TileSelection tile_sel_;
    UndoStack<TilePatch> tile_undo_{200};
    bool tile_mode_ = false;
    ObjectId tile_object_ = 0;
    glm::ivec2 tile_hover_{-1};
    bool tile_hover_valid_ = false;
    double tile_last_click_ = -1.0;        ///< For double-click (select similar).
    // Regenerate: the grid before, until the bake lands and the undo step can be recorded.
    std::shared_ptr<toy::world::TileMapData> tile_regen_data_;
    std::vector<toy::world::TileRecord> tile_regen_before_;
    int tile_regen_w_ = 0, tile_regen_h_ = 0;
    std::uint64_t tile_regen_wait_ = 0;    ///< bakes_done() to wait past.

public:
    TileBrushSettings& tile_brush() { return tile_brush_; }
    TileSelection& tile_selection() { return tile_sel_; }
    bool tile_mode() const { return in_tile_mode_(); }
    void set_tile_mode(bool on) { if (on) enter_tile_mode_(doc_.primary()); else exit_tile_mode_(); }
    const UndoStack<TilePatch>& tile_history() const { return tile_undo_; }
    /** @brief Test hook: edit the selection as the panel does. */
    void tile_edit_selection(const std::string& label, const std::function<void(toy::world::TileRecord&)>& edit) {
        tile_edit_selection_(label, edit);
    }

private:
