// editor/app/ui/properties.inl -- included inside EditorApp's class body.
//
// The Properties editor: a vertical strip of icon tabs that depends on the open asset.
//   Scene    Tool, Render, Output, Scene, World (config.yaml / the scene block), then the
//            selected object's Object, Components (Unity's list), Physics, Mesh, Material.
//   Object   Tool, then the selected object's tabs (the asset's root is selected on open).
//   Mesh     Tool (mesh tools / sculpt brushes) and Mesh (data, material slots).
//   Material the material editor and its lookdev preview settings.
//   Texture  the texture's info (a texture editor is a TODO).

    void draw_properties_(imm::Context& ctx, const imm::Box& area);

    /** @brief "main (scene)", "crate (object)", "cube (mesh)"... for headers and the top bar. */
    std::string active_asset_label_() const;

    // --- per-asset panels --------------------------------------------------------------

    /** @brief Mesh asset: data, LOD info and the material slots (submeshes). */
    void draw_mesh_asset_props_(imm::Context& ctx);

    /**
     * @brief Material slots (submeshes): each face belongs to one slot; a MeshRenderer gives
     *        every slot its own material. Here they are named, added and removed, faces are
     *        assigned in Edit Mode, and a preview material can be shown per slot.
     */
    void draw_material_slots_(imm::Context& ctx);

    /** @brief Material asset: the editor and the lookdev preview's settings. */
    void draw_material_asset_props_(imm::Context& ctx);

    /** @brief Texture asset: info. */
    void draw_texture_properties_(imm::Context& ctx);

    /** @brief A sound: file info, Preview / Stop, and its .import sidecar (how it plays by default). */
    void draw_audio_properties_(imm::Context& ctx);

    // --- global tabs -----------------------------------------------------------------

    void draw_tool_tab_(imm::Context& ctx);

    // --- settings groups (settings_schema.h) ------------------------------------------

    /** @brief The render settings field for `key` (toggles included), or null. */
    static const FieldDesc* render_field_(const std::string& key);

    /** @brief A setting's value as the renderer sees it: this scene's override, else config.yaml's; null = the default. */
    const Node* setting_value_(Node& sec, const std::string& section, const FieldDesc& f) const;
    bool setting_bool_(Node& sec, const std::string& section, const FieldDesc& f) const;
    std::string setting_string_(Node& sec, const std::string& section, const FieldDesc& f) const;
    /** @brief Writes a setting the way its row would: a scene override where allowed, else config.yaml. */
    void set_setting_(Node& sec, const std::string& section, const FieldDesc& f, const Node& value);

    /** @brief Does `text` contain `needle` (lowercase), ignoring case? */
    static bool contains_ci_(std::string text, const std::string& needle);
    static bool field_matches_(const FieldDesc& f, const std::string& needle) {
        return contains_ci_(f.display() + " " + f.key + " " + f.tooltip, needle);
    }
    /** @brief Under a search: which of a group's rows match (all of them when its title does); none = hide the group. */
    static std::vector<bool> group_matches_(const SettingsGroup& g, const std::string& needle, bool& any);

    /**
     * @brief One settings group as a collapsible section: the feature's switch in the header,
     *        then its rows under their sub-headings (a mode's rows only while that mode is on).
     *        Under a search (`needle`, lowercase) only matching rows show, and the group opens.
     */
    void draw_setting_group_(imm::Context& ctx, const SettingsGroup& g, const std::string& section, const InspectorEnv& env,
                             const std::string& needle = {});

    /** @brief Draws the named settings groups (all when `only` is empty) against a config.yaml section. */
    void draw_setting_groups_(imm::Context& ctx, const std::vector<SettingsGroup>& groups, const std::string& section,
                              const std::vector<std::string>& only);

    /**
     * @brief The render settings groups under their category headings (General, Render
     *        Features -- one section per feature, its switch in the header -- Stylize, Debug),
     *        filtered by `needle` (lowercase) and, when set, to one `category_only`. Drawn by the
     *        scene's Render tab (overrides) and the Project Settings modal (config.yaml).
     * @return False if no group matched.
     */
    bool draw_render_groups_(imm::Context& ctx, const std::string& needle, const std::string& category_only = {});

    /** @brief A search box bound to `filter`; returns its live text, lowercase (it only commits on Enter). */
    std::string settings_search_box_(imm::Context& ctx, const char* id, std::string& filter);

    /** @brief The Rebuild Renderer button: applies settings fixed at pipeline construction in place. */
    void rebuild_renderer_button_(imm::Context& ctx, float w);

    /**
     * @brief The scene's Render tab: this scene's overrides of config.yaml's render settings.
     *        Every row shows the value the scene renders with; editing one overrides it for this
     *        scene only (tinted). The project's own values live in Edit > Project Settings.
     */
    void draw_render_props_(imm::Context& ctx);

    /**
     * @brief The "this scene overrides N settings" line with Revert All, or the hint that edits
     *        here override the project's values.
     */
    void draw_scene_override_summary_(imm::Context& ctx, const std::string& section, const std::string& what);

    void draw_world_props_(imm::Context& ctx);

    void draw_output_props_(imm::Context& ctx);

    void draw_scene_props_(imm::Context& ctx);

    // --- object tabs -----------------------------------------------------------------

    void draw_object_props_(imm::Context& ctx, ObjectId id);

    /**
     * @brief Unity's component list: every component (Transform aside) as a panel with its
     *        icon, a remove button and a context menu; "Add Component" with search.
     * @param physics True: only physics components (the Physics tab); false: all others.
     */
    void draw_component_list_(imm::Context& ctx, ObjectId id, bool physics);

    void draw_data_props_(imm::Context& ctx, ObjectId id);

    void draw_material_props_(imm::Context& ctx, ObjectId id);

    /**
     * @brief One material per mesh slot (submesh): the MeshRenderer's `materials:` map, keyed
     *        by the mesh's slot names. Slot 0 uses `material:` above.
     */
    void draw_slot_materials_(imm::Context& ctx, ObjectId id);

    /** @brief The colour a material slot's preview ball shows (inline albedo, or the asset's). */
    glm::vec3 material_preview_color_(const Node& comp);

    /** @brief The Shading workspace's Properties: the open material asset. */

