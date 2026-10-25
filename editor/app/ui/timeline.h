/**
 * @file timeline.h
 * @brief Animation: the Timeline (a dope sheet) in the bottom area, next to the Console.
 *
 * A rig is an object hierarchy whose root carries an Animator (coopa::anim). Selecting the root
 * or anything under it makes it the Timeline's rig. Its clips are the Animator's `states`, each a
 * separate clip file under `<scene>/animations/<rig>/` -- created, listed and deleted from here
 * only, never from the asset browser: a clip belongs to its object. The panel edits one clip at
 * a time (a ClipModel, mesh-document style: own undo, written through to the file on every edit).
 *
 * Poses. The scene document holds the REST pose (what a skinned mesh binds against). While the
 * Timeline shows a clip, the live rig is posed by it at the playhead (anim/clip_pose.h); leaving
 * it (another tab, Rest, Play) puts the rest pose back.
 *
 * Animating (Blender / Unity style):
 * - Record (the red dot; the viewport gets a red border): transforms of rig objects -- gizmo,
 * G/R/S, the sidebar, the inspector -- change the live pose only, never the rest pose, and
 * AUTO-KEY: whatever channel changed (position, rotation, scale) is keyed at the playhead.
 * - I (viewport or Timeline) keys position, rotation (quaternion) and scale of the selected rig
 * objects; the inspector's diamonds key one channel (filled = keyed at this frame).
 * - Keys: click / Shift-click diamonds (Summary row: every object at that frame), drag to move
 * (snapped to frames), X / Delete removes them, right-click for interpolation (Linear, Step,
 * Ease In / Out / In-Out). Expand an object (its arrow) for one row per channel.
 * - Events (the lane under Summary): clip events the Animator fires as the playhead crosses them
 * (footsteps, sounds, hit frames -- Animator::on_event / the scene's "anim_event" signal).
 * Right-click the lane to add one, drag a flag to move it (snapped to frames), right-click it
 * to rename it, set its string / float payload or delete it; X / Delete removes the selected one.
 * - Space plays, Left / Right step a frame, Up / Down jump to the previous / next key, Home goes
 * to the start; the wheel zooms the frame range, Shift-wheel or a middle-drag pans it.
 *
 * One class per panel, owning its own state (see editor/app/README.md). EditorApp holds it
 * behind a forward declaration, so editing this header recompiles this panel and
 * editor_app.cpp -- not every panel.
 */

#ifndef TOYEDITOR_APP_UI_TIMELINE_H
#define TOYEDITOR_APP_UI_TIMELINE_H

#include "editor/app/editor_panel.h"

// Types this panel's declarations name (pre-seeded from the repo's type index;
// prune anything the declarations do not actually need).
#include "editor/anim/clip_model.h"
#include "editor/core/undo.h"
#include <coopa/animation/animator.h>

namespace toy::editor {

/** @brief See the file doc. */
class TimelinePanel : public EditorPanel {
public:
    using EditorPanel::EditorPanel;

    /// EditorApp owns this panel and drives it; the declarations below keep the access level
    /// they had inside EditorApp's body.
    friend class EditorApp;

    static constexpr float kAnimFps = 30.0f;   ///< The Timeline's frame grid (keys snap to it).

    /** @brief The clip being edited: its file, the model, and its own undo history. */


    /** @brief The dope sheet's visible frame range (0/0 = fit the clip). */


    // =================================================================================
    // Rig and clip context
    // =================================================================================

    /** @brief The rig root `id` belongs to: itself or its nearest ancestor with an Animator (0 if none). */
    ObjectId rig_of_(ObjectId id) const;

    /** @brief `id`'s track path from rig root `rig` ("" for the root, "Arm/Hand" below it). */
    std::string rig_path_of_(ObjectId rig, ObjectId id) const;

    /** @brief The rig root and every object under it, depth first (the Timeline's rows). */
    std::vector<std::pair<ObjectId, int>> rig_objects_(ObjectId rig) const;

    bool in_anim_rig_(ObjectId id) const;

    fs::path anim_scene_dir_() const;

    /** @brief Where a state's clip path points, resolved as the engine resolves it: next to the
     *         document first, then the project's assets/ (shared clips of object assets). */
    fs::path anim_resolve_clip_(const std::string& rel) const;
    /** @brief New clips of a scene's rig live next to the scene; an object asset's in assets/ (shared by every scene placing it). */
    fs::path anim_new_clip_base_() const;

    /** @brief An object's name in the document, or `fallback` when the id is not (or no longer) in it. */
    std::string anim_name_of_(ObjectId id, const std::string& fallback = std::string()) const;

    /** @brief The object asset (objects/x) a placed instance at or above `id` comes from, or "". */
    std::string anim_instance_asset_(ObjectId id) const;

    /** @brief Where a listed state's entry is declared, for a rig that inherits its Animator. */
    enum class AnimStateOrigin { Own, Inherited, Overridden };

    /** @brief One state on the rig: its name, its clip path as written, and where it is declared. */
    struct AnimStateRef {
        std::string name;
        std::string clip;
        AnimStateOrigin origin = AnimStateOrigin::Own;
    };

    /** @brief The rig's Animator states, as the editor shows them (the object asset's merged in). */
    std::vector<AnimStateRef> anim_states_() const;

    /** @brief `id`'s node as the editor shows it: merged with its object asset inside an instance. */
    const Node* anim_effective_node_(ObjectId id) const;

    /** @brief The Animator component on `id` as the editor shows it, or null if it has none. */
    const Node* anim_animator_of_(ObjectId id) const;

    /** @brief The rig's OWN Animator `states` (what this document declares), as a sequence. */
    Node anim_own_states_() const;

    /** @brief The object asset's Animator `states` for the rig, as a sequence (empty if it owns one). */
    Node anim_asset_states_() const;

    /** @brief True when the rig plays a state its object asset declares rather than this document. */
    bool anim_state_is_inherited_(const std::string& name) const;

    /** @brief Edits the rig's OWN Animator node, adding it as an override when the rig inherits one. */
    void anim_edit_animator_(const std::function<void(Node&)>& fn, const std::string& label);

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
    /** @brief Drops the open clip from the rig. `keep_file` leaves the clip file for its other users. */
    void anim_delete_clip_(bool keep_file = false);

    /** @brief The folder this rig's clips live in ("animations/<shared set>"), for a new clip. */
    std::string anim_clip_dir_() const;

    // --- sharing: one clip file is routinely played by many rigs ---------------------------

    /** @brief Documents OTHER than the open one whose Animator plays the open clip. */
    std::vector<std::string> anim_clip_sharers_();

    /** @brief The "Shared by N" pill and its list of the other rigs; advances `x`. */
    void draw_clip_sharing_(imm::Context& ctx, const imm::Box& hb, float& x);

    /** @brief The confirm shown before a delete or rename that would reach another rig's clip. */
    void draw_clip_share_modal_(imm::Context& ctx);

    /** @brief Delete the open clip, asking first when other rigs play its file. */
    void anim_request_delete_clip_();

    /** @brief Rename the open clip, asking first when other rigs play its file. */
    void anim_request_rename_clip_(const std::string& to);

    /** @brief What a pending clip-share confirm is about. */
    enum class ClipShareAction { None, Delete, Rename };
    struct ClipSharePrompt {
        ClipShareAction action = ClipShareAction::None;
        std::string state;                  ///< The state the action was invoked on.
        std::string rename_to;              ///< Rename only.
        std::vector<std::string> others;    ///< The documents that also play this clip.
    };
    ClipSharePrompt clip_share_prompt_;

    // =================================================================================
    // Poses
    // =================================================================================

    /** @brief Rig objects follow the live pose for transform edits (Pose mode / Record), not the document. */
    bool anim_live_edit_(ObjectId id) const;

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
    /** @brief Renames the open clip's state and file. `repoint_others` rewrites the other rigs that play it. */
    void anim_rename_clip_(const std::string& to, bool repoint_others = false);

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
                           float pps, bool in_track, bool& on_key);

    /** @brief Adds a clip event (undoable) and selects it. */
    void anim_add_event_(float time, const std::string& name);

    void anim_delete_selected_event_();

    void anim_select_all_keys_();

public:
    // --- Animation (tests, menus) ---
    ObjectId animation_rig() const;
    const ClipModel* animation_clip() const;
    std::string animation_clip_state() const;
    float animation_time() const;
    void set_animation_time(float t);
    void set_animation_record(bool on);
    bool animation_record() const;
    void show_timeline();
    void set_timeline_rest_pose(bool on);
    /** @brief The object asset a placed instance at or above `id` comes from ("" if none). */
    std::string animation_instance_asset(ObjectId id) const { return anim_instance_asset_(id); }
    /** @brief Documents other than the open one that play the open clip (the "Shared by N" list). */
    std::vector<std::string> animation_clip_sharers() { return anim_clip_sharers_(); }
    /** @brief The rig's state names in order, as the Timeline lists them. */
    std::vector<std::string> animation_state_names() const {
        std::vector<std::string> out;
        for (const AnimStateRef& st : anim_states_()) out.push_back(st.name);
        return out;
    }
    /** @brief True when the open rig plays this state from its object asset rather than itself. */
    bool animation_state_inherited(const std::string& name) const { return anim_state_is_inherited_(name); }
    void delete_animation_clip(bool keep_file) { anim_delete_clip_(keep_file); }
    void open_animation_clip(const std::string& state) { anim_open_clip_(state); }
    /** @brief The project's assets of a type, as the browser lists them. */
    std::vector<std::string> list_assets(AssetType t);
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
    int selected_animation_event() const;
    void select_animation_event(int index);
    void move_selected_animation_event(float to_time);
    void rename_selected_animation_event(const std::string& name);
    void delete_selected_animation_event() { anim_delete_selected_event_(); }
    /** @brief Moves an object the way the gizmo does (the live pose while recording a rig object). */
    void set_object_transform(ObjectId id, const glm::vec3& p, const glm::vec3& r, const glm::vec3& s) {
        set_object_transform_(id, p, r, s, "Move");
    }

private:
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_UI_TIMELINE_H
