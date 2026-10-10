// editor/app/ui/assets.inl -- included inside EditorApp's class body.
//
// The asset side of the editor -- what it is for: building a toyengine project's assets.
//   - The Asset panel (left): a tab per asset type (Scenes, Objects, Meshes, Materials,
//     Textures) listing the project's assets of that type. Clicking one OPENS it -- one asset
//     at a time (unsaved changes prompt first) -- and the viewer and Properties follow.
//   - Opening each type: scenes and object assets load into the scene document (object
//     assets as a one-object scene, see SceneDocument::is_object_asset()); meshes, materials
//     and textures show in the private preview ("lookdev") scene.
//   - Object assets (objects/*.yaml, the engine's `prefab:` references): create one from a
//     mesh or from a scene selection, and place instances in scenes.

    // =================================================================================
    // Asset types and listing
    // =================================================================================

    struct AssetTypeInfo {
        AssetType type;
        const char* label;      ///< tab label (plural)
        const char* singular;
        imm::Icon icon;
        const char* dir;        ///< folder under assets/
    };
    static const std::vector<AssetTypeInfo>& asset_types_();
    static const AssetTypeInfo& asset_type_info_(AssetType t);

    /** @brief The project's assets of a type (assets-relative paths, sorted). */
    std::vector<std::string> list_assets_(AssetType t);

    /**
     * @brief toyengine's own assets of a type that the project doesn't have a copy of -- the
     *        read-only layer the Asset panel shows below the project's (empty when the project is
     *        toyengine itself, or while the panel's toyengine toggle is off).
     */
    std::vector<std::string> list_engine_assets_(AssetType t);

    /** @brief Materials a picker offers: the project's, then toyengine's (while shown). */
    std::vector<std::string> material_choices_();

public:
    /** @brief Shows / hides toyengine's assets in the Asset panel and pickers (remembered). */
    void set_show_engine_assets(bool on);
    bool show_engine_assets() const { return show_engine_assets_; }
    /** @brief The Asset panel's engine rows for a type, as listed (tests). */
    std::vector<std::string> engine_assets_listed(AssetType t) { return list_engine_assets_(t); }

    /**
     * @brief Copies a read-only toyengine asset into the project (same path, so the copy now
     *        resolves instead of toyengine's) and opens it -- the way to edit one. A scene copies
     *        its whole folder (scene-local meshes and textures come along).
     */
    bool copy_engine_asset_to_project(AssetType t, const std::string& rel);

private:
    /** @brief Refuses to open / edit a toyengine asset outside toyengine itself (logging why). */
    bool refuse_engine_asset_(const fs::path& abs);

    /** @brief Audio clip extensions sfxcoopa decodes. */
    static bool is_audio_ext_(std::string ext);

    /** @brief The asset type of an assets-relative path (by folder / extension). */
    static AssetType asset_type_of_(const std::string& rel);

    /** @brief A display name for an asset: scenes show their folder, others the file stem. */
    static std::string asset_display_name_(AssetType t, const std::string& rel);

    // =================================================================================
    // Tags, ordering and filtering
    // =================================================================================
    //
    // An asset's tags are the folders between its type folder and the file (project.h
    // asset_tags()): assets/materials/metal/brick.yaml is the material `brick` tagged `metal`.
    // References name only the type and the asset (`materials/brick`) and the engine finds it
    // by name (coopa::asset::AssetIndex), so re-tagging -- moving the file -- breaks nothing;
    // which is also why names are unique per type (asset_name_taken_()).

    /** @brief The type folder an asset type lists from (`ui/themes` for themes). */
    static std::string asset_type_folder_(AssetType t) { return asset_type_info_(t).dir; }

    /** @brief True when an asset of type folder `type` (`materials`, `scenes`...) is named `name`. */
    bool asset_name_taken_(const std::string& type, const std::string& name, const std::string& except_rel = {});

    /** @brief The folder a new asset of type folder `dir` is created in: dir/<tags being created with>. */
    std::string new_asset_dir_(const std::string& dir) const;
    /** @brief Runs a create (new_mesh(), create_material()...) with its file under `tags`. */
    template <class F>
    void with_tags_(const std::vector<std::string>& tags, F&& create) {
        creating_tags_ = tags;
        create();
        creating_tags_.clear();
    }

    /** @brief A typed tag as a folder name (snake_case), or "" when it can't be one. */
    static std::string clean_tag_(const std::string& raw);

    /** @brief Every tag on `items`, with how many assets carry it (sorted by name). */
    static std::map<std::string, int> tag_counts_(const std::vector<std::string>& items);

    /** @brief Does `rel` pass the tab's tag filter (none selected: everything does)? */
    bool passes_tag_filter_(const std::string& rel) const;
    std::vector<std::string>& tag_filter_() { return asset_tag_filter_[asset_tab_]; }
    void toggle_tag_filter_(const std::string& tag);

    /** @brief Last-modified time and size of an asset (a scene: its document), cached briefly. */
    AssetStat asset_stat_(const std::string& rel);

    static const char* asset_sort_name_(AssetSort s);
    static AssetSort asset_sort_from_name_(const std::string& n);
    void set_asset_sort_(AssetSort s, bool reverse);

    /**
     * @brief Orders a tab's assets: by name (A-Z), last modified (newest first), tags (untagged
     *        first, then by tag path) or size (largest first); Reverse flips any of them.
     */
    void sort_assets_(AssetType t, std::vector<std::string>& items);

    /** @brief A tag's chip colour: a muted hue picked from its name, so a tag looks the same everywhere. */
    static glm::vec4 tag_color_(const std::string& tag);

    /**
     * @brief Draws `tags` as chips right-aligned in `row`, never left of `min_x` (the rest
     *        collapse into a "+N" chip). @return The tag whose chip was clicked this frame, or "".
     */
    std::string draw_tag_chips_(imm::Context& ctx, const imm::Box& row, const std::vector<std::string>& tags, float min_x, bool clicked);

    /**
     * @brief Edits a tag list in place: the tags it has (each removable), the tab's other known
     *        tags to add, and a field for a new one. Tags are folders, nested in list order.
     */
    void draw_tag_editor_(imm::Context& ctx, std::vector<std::string>& tags, const std::vector<std::string>& known);

    /**
     * @brief Re-tags an asset: moves it (a scene: its folder) under `tags`, rewriting any
     *        full-path references to it; short references (`materials/brick`) need nothing.
     *        Unsaved edits are settled first; an open asset reopens from its new place.
     */
    void retag_asset_(AssetType t, const std::string& rel, const std::vector<std::string>& tags);

public:
    /** @brief Re-tags an asset (tests; the panel's Tags... dialog). */
    void set_asset_tags(AssetType t, const std::string& rel, const std::vector<std::string>& tags) { retag_asset_(t, rel, tags); }
    /** @brief Sets the panel's tag filter for the current tab (tests). */
    void set_asset_tag_filter(const std::vector<std::string>& tags, bool match_all = false);
    /** @brief The current tab's assets as the panel lists them: filtered by tags, sorted (tests). */
    std::vector<std::string> assets_listed();
    void set_asset_sort(AssetSort s, bool reverse = false) { set_asset_sort_(s, reverse); }

private:
    /** @brief Is this (assets-relative) asset the one that's open? */
    bool asset_is_open_(AssetType t, const std::string& rel) const;
    /** @brief Unsaved changes on the open asset (for the list's dot). */
    bool open_asset_dirty_() const;

    // =================================================================================
    // Opening (one asset at a time)
    // =================================================================================

public:
    /**
     * @brief Opens an asset (assets-relative or absolute path) -- the one way in. Asks to
     *        save unsaved changes first; the viewer and Properties switch to the asset's type.
     */
    void open_asset(AssetType t, const std::string& item);

private:
    void open_asset_now_(AssetType t, const fs::path& abs);

    /** @brief Legacy string kinds (drops, menus) -> open_asset(). */
    void open_asset_(const std::string& kind, const std::string& item);

public:
    /** @brief Opens an object asset (objects/*.yaml) as a one-object scene. */
    bool open_object_asset(const fs::path& path);

    /** @brief Creates objects/<name>.yaml (an empty object) and opens it. */
    bool new_object_asset(const std::string& name);

    /** @brief Opens a sound: Properties shows its import settings and a preview player. */
    bool open_audio(const fs::path& path);

    /** @brief Plays the open sound once on the UI bus (the editor's engine is silent otherwise). */
    void preview_audio();
    void stop_audio_preview();

    /**
     * @brief Copies sound files into assets/audio/ (keeping an .import sidecar beside each).
     * @return How many were imported.
     */
    int import_audio(const std::vector<fs::path>& files);

    /** @brief Shows a texture asset in the viewer (texture editing is a TODO). */
    bool open_texture(const fs::path& path);

private:
    // =================================================================================
    // Object assets (prefabs)
    // =================================================================================

    /** @brief A clean object node: Transform at identity, no private keys. */
    static Node object_asset_node_(Node obj);

    bool write_object_asset_(const fs::path& path, const Node& obj);

    /** @brief objects/<mesh>.yaml: an object with a MeshRenderer using `mesh_rel`; opens it. */
    void create_object_asset_from_mesh_(const std::string& mesh_rel);

public:
    /**
     * @brief Scene view: turns the selected object into objects/<name>.yaml and replaces it
     *        with an instance (`prefab:`) at the same place.
     */
    bool create_object_asset_from_selection();

    /** @brief Places an instance of an object asset (objects/x.yaml) in the open scene / object. */
    ObjectId place_object_asset(const std::string& object_rel, std::optional<glm::vec3> at = std::nullopt);

private:
    // =================================================================================
    // The Asset panel (left)
    // =================================================================================

    void draw_asset_panel_(imm::Context& ctx, const imm::Box& area);

    void draw_asset_context_menu_(imm::Context& ctx, AssetType t, const std::string& rel);

    /** @brief True when New for this type is a choice (mesh primitives, UI templates). */
    static bool asset_new_has_choices_(AssetType t) { return t == AssetType::Mesh || t == AssetType::UI; }

    /** @brief The New items of an asset type -- shared by the + button and the list's Add menu. */
    void draw_new_asset_items_(imm::Context& ctx, AssetType t);

    /** @brief Creates a new asset of a type under the tags picked for it (new_asset_tags_). */
    void new_asset_(AssetType t);

    /** @brief scenes/<name>/scene.yaml: the starter scene, saved, then opened. */
    void new_scene_asset_(const std::string& base);

    void duplicate_asset_(AssetType t, const std::string& rel);

public:
    /**
     * @brief Copies a tile set -- the project's or a built-in toyengine one -- into the project
     *        as a new, independent look: objects/<name>_copy.yaml plus its own copy of every
     *        piece mesh (duplicate_tile_set_pieces_()).
     * @return The copy's project-relative path, or "" if it could not be made.
     */
    std::string duplicate_tile_set(const std::string& rel);

private:
    /** @brief True when the object asset `rel` is a tile set (`tile_set: true` on its object). */
    bool is_tile_set_(const std::string& rel) const;

    /**
     * @brief If the object asset at `copy` is a TILE SET (`tile_set: true`, see
     *        assets/objects/tileset_*.yaml), gives the copy its own piece meshes: each child's
     *        mesh is copied to meshes/<copy stem>_<child name>.yaml and the child pointed at
     *        it. Without this a duplicate would share -- and editing it would change -- the
     *        original's pieces, which is the opposite of what duplicating a look is for.
     *        Pieces come from the project, or from toyengine's own assets for a built-in set.
     */
    void duplicate_tile_set_pieces_(const fs::path& copy);

    /** @brief Renames an asset and every reference to it; reloads open documents that changed. */
    void rename_asset_(const std::string& from, const std::string& to);

    void draw_asset_modals_(imm::Context& ctx);

    // =================================================================================
    // Console (under the viewer)
    // =================================================================================

    /** @brief The bottom area: Console and Timeline tabs. */
    void draw_console_area_(imm::Context& ctx, const imm::Box& area);

    // =================================================================================
    // The preview ("lookdev") scene: meshes, materials, textures
    // =================================================================================

    /** @brief Lookdev settings (Material Properties > Preview). */
    struct Lookdev {
        int shape = 0;            ///< 0 shader ball, 1 sphere, 2 rounded cube, 3 plane, 4 cylinder
        bool turntable = false;
        bool ground = true;
        float angle = 0.0f;
        uint64_t revision = 1;    ///< bumped when the shape changes
    };

    static Node lookdev_object_(const std::string& name);

    /**
     * @brief The preview scene: a studio rig (key sun with shadows, fill and rim point lights,
     *        an environment light), a ground disc and the preview object.
     */
    void ensure_preview_();

    /** @brief The lookdev shape for a material (object space, resting on z = 0 after placement). */
    EditMesh lookdev_shape_() const;

    /** @brief Places the preview object so it rests on the ground disc (z = 0). */
    void place_preview_(const EditMesh& m, bool on_ground);

    /** @brief Per frame while a preview asset is open: keep the preview object current. */
    void update_preview_();

    /**
     * @brief How the project uses a texture: "linear" when a material, scene or object asset names
     *        it as a normal / metallic-roughness / alpha-mask map (data, not colour), "srgb" when
     *        as an albedo map; with no reference, guessed from the file name (_normal, _mr...).
     *
     * The preview must declare the SAME color space the project's materials will: the texture
     * loader keeps the first declaration for a file, so previewing a normal map as sRGB would
     * mis-decode it for every material that uses it for the rest of the session.
     */
    std::string texture_color_space_(const std::string& rel);

    /** @brief The texture view's material: the image as albedo (decoded in the color space the
     *         project uses it in -- see texture_color_space_()), fully rough. */
    void refresh_texture_preview_();

    /** @brief A distinct preview colour per mesh slot (slot 0 stays the neutral grey). */
    static glm::vec3 slot_color_(uint32_t slot);

    /** @brief The mesh viewer's per-slot materials: a picked material asset, else a palette colour. */
    void apply_slot_preview_materials_();

    /** @brief Studio lights for an object asset's view (live scene, never saved). */
    void add_object_view_lights_();
