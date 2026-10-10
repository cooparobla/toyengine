// editor/app/ui/timeline.inl -- included inside EditorApp's class body.
//
// Animation: the Timeline (a dope sheet) in the bottom area, next to the Console.
//
// A rig is an object hierarchy whose root carries an Animator (coopa::anim). Selecting the root
// or anything under it makes it the Timeline's rig. Its clips are the Animator's `states`, each a
// separate clip file under `<scene>/animations/<rig>/` -- created, listed and deleted from here
// only, never from the asset browser: a clip belongs to its object. The panel edits one clip at
// a time (a ClipModel, mesh-document style: own undo, written through to the file on every edit).
//
// Poses. The scene document holds the REST pose (what a skinned mesh binds against). While the
// Timeline shows a clip, the live rig is posed by it at the playhead (anim/clip_pose.h); leaving
// it (another tab, Rest, Play) puts the rest pose back.
//
// Animating (Blender / Unity style):
//  - Record (the red dot; the viewport gets a red border): transforms of rig objects -- gizmo,
//    G/R/S, the sidebar, the inspector -- change the live pose only, never the rest pose, and
//    AUTO-KEY: whatever channel changed (position, rotation, scale) is keyed at the playhead.
//  - I (viewport or Timeline) keys position, rotation (quaternion) and scale of the selected rig
//    objects; the inspector's diamonds key one channel (filled = keyed at this frame).
//  - Keys: click / Shift-click diamonds (Summary row: every object at that frame), drag to move
//    (snapped to frames), X / Delete removes them, right-click for interpolation (Linear, Step,
//    Ease In / Out / In-Out). Expand an object (its arrow) for one row per channel.
//  - Events (the lane under Summary): clip events the Animator fires as the playhead crosses them
//    (footsteps, sounds, hit frames -- Animator::on_event / the scene's "anim_event" signal).
//    Right-click the lane to add one, drag a flag to move it (snapped to frames), right-click it
//    to rename it, set its string / float payload or delete it; X / Delete removes the selected one.
//  - Space plays, Left / Right step a frame, Up / Down jump to the previous / next key, Home goes
//    to the start; the wheel zooms the frame range, Shift-wheel or a middle-drag pans it.

    static constexpr float kAnimFps = 30.0f;   ///< The Timeline's frame grid (keys snap to it).

    /** @brief The clip being edited: its file, the model, and its own undo history. */
    struct ClipDocument {
        std::string state;        ///< The Animator state naming it.
        fs::path path;
        ClipModel model;
        UndoStack<ClipModel> undo{200};
        uint64_t revision = 1;    ///< Bumps on every change (the preview re-parses).
        bool open() const { return !path.empty(); }
    };
    struct KeyDrag { bool active = false; float start_x = 0.0f; float dt = 0.0f; };
    /** @brief The dope sheet's visible frame range (0/0 = fit the clip). */
    struct TimelineView { float start = 0.0f, end = 0.0f; };

    // =================================================================================
    // Rig and clip context
    // =================================================================================

    /** @brief The rig root `id` belongs to: itself or its nearest ancestor with an Animator (0 if none). */
    ObjectId rig_of_(ObjectId id) const;

    /** @brief `id`'s track path from rig root `rig` ("" for the root, "Arm/Hand" below it). */
    std::string rig_path_of_(ObjectId rig, ObjectId id) const;

    /** @brief The rig root and every object under it, depth first (the Timeline's rows). */
    std::vector<std::pair<ObjectId, int>> rig_objects_(ObjectId rig) const;

    bool in_anim_rig_(ObjectId id) const { return anim_rig_ && (id == anim_rig_ || doc_.is_ancestor(anim_rig_, id)); }

    fs::path anim_scene_dir_() const { return (doc_.path().empty() ? sync_.fallback_path : doc_.path()).parent_path(); }

    /** @brief Where a state's clip path points, resolved as the engine resolves it: next to the
     *         document first, then the project's assets/ (shared clips of object assets). */
    fs::path anim_resolve_clip_(const std::string& rel) const;
    /** @brief New clips of a scene's rig live next to the scene; an object asset's in assets/ (shared by every scene placing it). */
    fs::path anim_new_clip_base_() const { return doc_.is_object_asset() ? project_.assets() : anim_scene_dir_(); }

    /** @brief An object's name in the document, or `fallback` when the id is not (or no longer) in it. */
    std::string anim_name_of_(ObjectId id, const std::string& fallback = std::string()) const;

    /** @brief The object asset (objects/x) a placed instance at or above `id` comes from, or "". */
    std::string anim_instance_asset_(ObjectId id) const;

    /** @brief The rig's Animator states: (state name, clip path as written). */
    std::vector<std::pair<std::string, std::string>> anim_states_() const;

    /** @brief Opens state `name`'s clip file (creating an empty clip if the file is missing). */
    void anim_open_clip_(const std::string& name);

    /**
     * @brief One undoable clip edit. Edits sharing `merge_key` while the mouse stays down (one
     *        gizmo drag auto-keying every frame) are one undo step. The file is written once at
     *        the end of the frame (timeline_frame_()), not per edit.
     */
    void anim_clip_edit_(const std::string& label, const std::function<void(ClipModel&)>& fn, const std::string& merge_key = {});

    void anim_save_clip_();

    /** @brief Adds an Animator to `id`: it becomes a rig root. */
    void anim_add_animator_(ObjectId id);

    /** @brief A new clip on the rig: a file under animations/<rig>/, a state, and (first clip) auto_play. */
    void anim_new_clip_(std::string name = "clip");

    /** @brief Removes the open clip's state from the Animator and deletes its file. */
    void anim_delete_clip_();

    // =================================================================================
    // Poses
    // =================================================================================

    /** @brief Rig objects follow the live pose for transform edits (Record), not the document. */
    bool anim_live_edit_(ObjectId id) const { return anim_record_ && anim_posed_ && in_anim_rig_(id); }

    /** @brief An object's transform as edits should start from: the live pose while recording, else the document. */
    bool get_object_transform_(ObjectId id, glm::vec3& p, glm::vec3& r, glm::vec3& s) const;

    /** @brief Sets an object's transform: on the live pose while recording a rig object, else as a document edit. */
    void set_object_transform_(ObjectId id, const glm::vec3& p, const glm::vec3& r, const glm::vec3& s, const std::string& label,
                               const std::string& merge_key = {});

    /** @brief Puts the document's rest pose back on the live rig. */
    void anim_restore_rest_pose_();

    /** @brief Should the live rig show the open clip at the playhead? */
    bool anim_preview_wanted_() const;

    /** @brief Per frame (pre-render): follow the selection's rig, play, and pose the live rig. */
    void timeline_frame_(float dt);

    // =================================================================================
    // Keys
    // =================================================================================

    static float anim_snap_(float t) { return std::max(0.0f, std::round(t * kAnimFps) / kAnimFps); }

    /** @brief I: keys position, rotation and scale of the selected rig objects (the root if none) at the playhead. */
    void anim_insert_keys_();

    void anim_delete_selected_keys_();

    /** @brief The inspector's diamonds: keys one channel (position / rotation_quat / scale) of `id` now. */
    void anim_key_channel_(ObjectId id, const std::string& property);

    /** @brief Whether `id`'s `property` has a key at the playhead (the inspector's filled diamond). */
    bool anim_keyed_now_(ObjectId id, const std::string& property) const;

    /** @brief Renames the open clip: its state, the auto_play reference, its name and its file. */
    void anim_rename_clip_(const std::string& to);

    // =================================================================================
    // UI
    // =================================================================================

    /** @brief The bottom area's Timeline tab: header controls in `hb`, the dope sheet in `body`. */
    void draw_timeline_(imm::Context& ctx, const imm::Box& hb, const imm::Box& body, float x);

    /** @brief A channel's row label. */
    static std::string anim_channel_label_(const std::string& property);

    /**
     * @brief The dope sheet: a frame ruler, a Summary row, one row per rig object (expandable into
     *        one row per animated channel), key diamonds and the playhead.
     */
    void draw_dope_sheet_(imm::Context& ctx, const imm::Box& body);

    /**
     * @brief The Events lane: one flag per clip event (selected: orange), hover for its name.
     *        Click selects and starts a drag, right-click opens its menu; right-click empty lane
     *        space to add one there.
     */
    template<typename XOf, typename TOf>
    void draw_events_lane_(imm::Context& ctx, const imm::Box& track, float y, float row_h, const XOf& x_of, const TOf& t_of,
                           float pps, bool in_track, bool& on_key) {
        const auto& in = ctx.input();
        const glm::vec2 m = ctx.mouse();
        const auto& evs = anim_clip_.model.events;
        if (anim_sel_event_ >= static_cast<int>(evs.size())) anim_sel_event_ = -1;
        ctx.push_clip(track);
        for (size_t i = 0; i < evs.size(); ++i) {
            const bool sel = static_cast<int>(i) == anim_sel_event_;
            const float ex = x_of(evs[i].time) + (anim_event_drag_.active && sel ? anim_event_drag_.dt * pps : 0.0f);
            const glm::vec4 col = sel ? glm::vec4(1.0f, 0.62f, 0.18f, 1) : glm::vec4(0.55f, 0.8f, 0.95f, 1);
            ctx.line({ex, y + 3}, {ex, y + row_h - 3}, col, 1.5f);
            ctx.triangle({ex, y + 3}, {ex + 9, y + 6.5f}, {ex, y + 10}, col);
            const imm::Box hit{ex - 4, y, 15, row_h};
            if (!ctx.is_hovered(hit) || on_key) continue;
            ctx.text_in({ex + 11, y, 160, row_h}, evs[i].name + (evs[i].string_value.empty() ? "" : " (" + evs[i].string_value + ")"),
                        ctx.style.text, 0.0f);
            if (in.pressed[0]) {
                on_key = true;
                anim_sel_event_ = static_cast<int>(i);
                anim_sel_keys_.clear();
                anim_event_drag_ = {true, m.x, 0.0f};
            } else if (in.pressed[1]) {
                on_key = true;
                anim_sel_event_ = static_cast<int>(i);
                ctx.open_popup("event_menu", m);
            }
        }
        ctx.pop_clip();
        const imm::Box lane{track.x, y, track.w, row_h};
        if (in_track && !on_key && ctx.is_hovered(lane)) {
            if (in.pressed[0]) { anim_sel_event_ = -1; anim_sel_keys_.clear(); on_key = true; }   // empty lane: deselect
            if (in.pressed[1]) {
                on_key = true;
                anim_menu_time_ = anim_snap_(t_of(m.x));
                ctx.open_popup("events_lane_menu", m);
            }
        }
    }

    /** @brief Adds a clip event (undoable) and selects it. */
    void anim_add_event_(float time, const std::string& name);

    void anim_delete_selected_event_();

    void anim_select_all_keys_();

public:
    // --- Animation (tests, menus) ---
    ObjectId animation_rig() const { return anim_rig_; }
    const ClipModel* animation_clip() const { return anim_clip_.open() ? &anim_clip_.model : nullptr; }
    std::string animation_clip_state() const { return anim_clip_.state; }
    float animation_time() const { return anim_time_; }
    void set_animation_time(float t) { anim_time_ = std::max(0.0f, t); }
    void set_animation_record(bool on) { anim_record_ = on; }
    bool animation_record() const { return anim_record_; }
    void show_timeline();
    void set_timeline_rest_pose(bool on) { anim_show_rest_ = on; }
    /** @brief The object asset a placed instance at or above `id` comes from ("" if none). */
    std::string animation_instance_asset(ObjectId id) const { return anim_instance_asset_(id); }
    /** @brief The project's assets of a type, as the browser lists them. */
    std::vector<std::string> list_assets(AssetType t) { return list_assets_(t); }
    void key_channel(ObjectId id, const std::string& property) { anim_key_channel_(id, property); }
    bool keyed_now(ObjectId id, const std::string& property) const { return anim_keyed_now_(id, property); }
    void select_all_keys() { anim_select_all_keys_(); }
    void set_selected_key_interpolation(const std::string& ease);
    void rename_animation_clip(const std::string& to) { anim_rename_clip_(to); }
    void add_animator(ObjectId id) { anim_add_animator_(id); }
    void new_animation_clip(const std::string& name = "clip") { anim_new_clip_(name); }
    void insert_keyframes() { anim_insert_keys_(); }
    /** @brief The Events lane's operations, as its mouse actions perform them (all undoable). */
    void add_animation_event(float time, const std::string& name) { anim_add_event_(time, name); }
    int selected_animation_event() const { return anim_sel_event_; }
    void select_animation_event(int index) { anim_sel_event_ = index; }
    void move_selected_animation_event(float to_time);
    void rename_selected_animation_event(const std::string& name);
    void delete_selected_animation_event() { anim_delete_selected_event_(); }
    /** @brief Moves an object the way the gizmo does (the live pose while recording a rig object). */
    void set_object_transform(ObjectId id, const glm::vec3& p, const glm::vec3& r, const glm::vec3& s) {
        set_object_transform_(id, p, r, s, "Move");
    }

private:
