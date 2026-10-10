// editor/app/ui/instances.inl -- included inside EditorApp's class body.
//
// Prefab instances (`prefab: objects/crate`) and their overrides.
//
// The document holds an instance as its root node plus, for every child the instance inherits
// from its object asset, a placeholder node (SceneDocument::sync_placeholders()) carrying that
// child's overrides. Placeholders give inherited children ids, so selection, the Hierarchy,
// the gizmo and picking treat them like any object. What the editor SHOWS for a node inside an
// instance is its resolved node (asset + overrides, instance_view_()); what it WRITES is the
// smallest override that turns the asset's value into the edited one, and nothing at all when
// they agree -- the object asset itself is never touched, except by Apply to Object Asset.
//
//   Overrides are matched as SceneInheritance merges them: components by type (+ `id`),
//   children by name, mappings key by key.

    // --- resolving ----------------------------------------------------------------------

    /** @brief Documents read while resolving, reused while their files are unchanged. */
    struct CachedDoc { fs::file_time_type time{}; Node node; };
    mutable std::map<std::string, CachedDoc> inherit_docs_;

    coopa::scene::SceneInheritance::DocumentLoader inherit_loader_() const;

    std::string inherit_anchor_() const { return (doc_.path().empty() ? sync_.fallback_path : doc_.path()).string(); }

    /** @brief `obj` with its prefab expanded, keeping editor ids (null if it can't be resolved). */
    std::optional<Node> resolve_keep_ids_(const Node& obj) const;

    /** @brief The object asset an instance node refers to, as the asset has it (no overrides). */
    std::optional<Node> bare_asset_of_(const Node& inst) const;

    /** @brief Placeholders for every inherited child (see the file doc). Before each rebuild. */
    void sync_instance_placeholders_();

    /** @brief One outermost instance, resolved: its nodes merged (`resolved`) and as its asset has them (`base`). */
    struct InstanceEntry {
        Node src;   ///< The instance's document node this was resolved from.
        std::unordered_map<ObjectId, Node> resolved, base;
    };
    /** @brief Every instance in the document, resolved; re-resolved per instance only when its node changed. */
    struct InstanceView {
        uint64_t rev = ~0ull, gen = ~0ull;
        std::map<ObjectId, InstanceEntry> roots;
        std::unordered_map<ObjectId, const Node*> resolved, base;   ///< Into `roots`.
    };
    mutable InstanceView inst_view_;
    uint64_t scene_gen_ = 0;   ///< Bumped by every rebuild: asset files may have changed since.

    InstanceEntry resolve_instance_entry_(const Node& inst) const;

    const InstanceView& instance_view_() const;

    /** @brief The merged node of an object inside an instance, or null for a plain object. */
    const Node* resolved_of_(ObjectId id) const;
    /** @brief The object asset's own node for an object inside an instance, or null. */
    const Node* base_of_(ObjectId id) const;
    /** @brief What the editor shows for `obj`: its merged node inside an instance, else itself. */
    const Node& effective_(const Node& obj) const;
    /** @brief True for an instance root or a child it inherits: edits there are overrides. */
    bool in_instance_(ObjectId id) const { return doc_.instance_root_of(id) != 0; }

    // --- reading and writing overrides ----------------------------------------------------

    /** @brief Index in `list` of the component matching `comp` by type and `id`, or -1. */
    static int match_component_(const Node& list, const Node& comp);

    /** @brief `id`'s own override entry for `comp` (null if it overrides nothing there). */
    const Node* own_override_(ObjectId id, const Node& comp) const;

    /** @brief The asset's component matching `comp` for an object inside an instance (null if added). */
    const Node* base_component_(ObjectId id, const Node& comp) const;

    /** @brief A mapping whose values are all scalars (a vector, a colour): compared whole. */
    static bool leafy_(const Node& n);

    /** @brief The keys of `edited` that differ from `base` (mappings recurse, key by key). */
    static Node diff_map_(const Node& base, const Node& edited);

    /** @brief The smallest override turning the asset's component into `edited` (type + id kept). */
    Node minimal_override_(ObjectId id, const Node& edited) const;

    /** @brief True when an override entry holds nothing but its type / id. */
    static bool override_empty_(const Node& ov) { return ov.size() <= (ov.contains("id") ? 2u : 1u); }

    /** @brief Writes (or, when it overrides nothing, removes) `id`'s override entry for `match`. */
    void write_override_(ObjectId id, const Node& match, const Node& ov, const std::string& merge = {});

    /** @brief Drops one key of `id`'s override for `comp` (the component's whole entry if empty after). */
    void revert_override_field_(ObjectId id, const Node& comp, const std::string& key);

    /** @brief The resolved Transform of an object inside an instance (false if it has none). */
    bool instance_transform_(ObjectId id, glm::vec3& p, glm::vec3& r, glm::vec3& s) const;

    /**
     * @brief Moves an inherited child: its Transform override keeps only the parts that differ
     *        from the asset's, and the live object is set directly (an override key that goes
     *        away would otherwise leave the live value where it was).
     */
    void set_inherited_transform_(ObjectId id, const glm::vec3& p, const glm::vec3& r, const glm::vec3& s,
                                  const std::string& label, const std::string& merge_key);

    /** @brief True when `id` (inside an instance) carries any override of its own. */
    bool has_overrides_(ObjectId id) const;

    /** @brief Removes every override `id` (an instance root or inherited child) holds -- and below it. */
    void revert_all_overrides_(ObjectId id);

    /** @brief `id`'s first `type` component as the editor shows it (resolved inside an instance). */
    bool shown_component_(ObjectId id, const std::string& type, Node& out) const;

    /** @brief Saves an edited component the editor showed: in place, or inside an instance as its smallest override. */
    void edit_component_(ObjectId id, const Node& edited, const std::string& label, const std::string& merge = {});

    // --- Apply to Object Asset --------------------------------------------------------------

    /** @brief The object asset file instance root `root` refers to (empty if it can't be found). */
    fs::path instance_asset_file_(ObjectId root) const;

    /** @brief The asset path of instance root `root` as the Asset panel names it ("objects/crate.yaml"). */
    std::string instance_asset_item_(ObjectId root) const;

    /** @brief SceneInheritance's merge of one override onto a base value: mappings key by key. */
    static void merge_into_(Node& base, const Node& over);

    /**
     * @brief Writes overrides into the object asset itself: `comps` (override entries) and
     *        `extra` (other keys, e.g. `active`) onto the asset's object at `path` (child names
     *        from its root), and `added` children appended there. A path into an instance the
     *        asset itself contains becomes an override node in the asset -- the same merge.
     * @return False (logged) if the asset can't be written.
     */
    bool write_to_asset_(ObjectId root, const std::vector<std::string>& path, const std::vector<Node>& comps,
                         const Node& extra, const std::vector<Node>& added);

    /** @brief Apply one field (`key`) of `id`'s override for `comp` to the asset, then drop it here. */
    void apply_field_to_asset_(ObjectId id, const Node& comp, const std::string& key);

    /** @brief Apply `id`'s whole override for `comp` to the asset, then drop it here. */
    void apply_component_to_asset_(ObjectId id, const Node& comp);

    /** @brief Apply every override of `id` and below (an instance root: the whole instance) to the asset. */
    void apply_all_to_asset_(ObjectId id);

    // --- selection ---------------------------------------------------------------------------

    /**
     * @brief What a click on `hit` selects. A part of an instance selects the whole instance
     *        first; once the instance (or something in it) is selected, the part itself --
     *        Unity's prefab selection.
     */
    ObjectId click_target_(ObjectId hit) const;

    /** @brief Box select picks whole instances, not their parts. */
    ObjectId box_target_(ObjectId id) const;

    /** @brief Instance commands for a context menu on `id` (shown only for an object in an instance). */
    void draw_instance_menu_items_(imm::Context& ctx, ObjectId id);

    // --- renaming a part of an object asset ---------------------------------------------------

    /// The open object asset's objects (by id) and their name paths, as of opening / saving it.
    std::map<ObjectId, std::vector<std::string>> asset_name_paths_;

    std::map<ObjectId, std::vector<std::string>> asset_name_paths_now_() const;
    void remember_asset_names_() { asset_name_paths_ = asset_name_paths_now_(); }

    /**
     * @brief After saving object asset `asset_file`: overrides find its children by name, so a
     *        child renamed here is renamed in every scene and object asset that overrides it.
     * @return The files rewritten.
     */
    std::vector<fs::path> follow_asset_renames_(const fs::path& asset_file);
