/**
 * @file outliner.h
 * @brief The Hierarchy: exactly what the open file holds. For a scene, the root row is the file's
 *        `scene:` block (named by scene_name) and its rows are root_objects in file order, nested
 *        children, then every component in order (Transform included). For an object asset the
 *        root row is the object itself. Prefab instances (`prefab:`) show a link icon; the
 *        children they inherit from the object asset are rows like any other (ui/instances.h),
 *        dimmed until they override something, and an accent bar marks a row with overrides. Rows
 *        have eye (viewport visibility) and monitor (enabled in the game) toggles, driven like
 *        Blender's: a click acts on that object, Shift+click on it and every descendant, and
 *        holding the button while the pointer crosses other rows copies the same state onto
 *        every toggle in that column.
 *
 * One class per panel, owning its own state (see editor/app/README.md). EditorApp holds it
 * behind a forward declaration, so editing this header recompiles this panel and
 * editor_app.cpp -- not every panel.
 */

#ifndef TOYEDITOR_APP_UI_OUTLINER_H
#define TOYEDITOR_APP_UI_OUTLINER_H

#include "editor/app/editor_panel.h"

#include <set>
#include <vector>

namespace toy::editor {

/** @brief See the file doc. */
class OutlinerPanel : public EditorPanel {
public:
    using EditorPanel::EditorPanel;

    /// EditorApp owns this panel and drives it; the declarations below keep the access
    /// level they had inside EditorApp's body.
    friend class EditorApp;

    void draw_outliner_(imm::Context& ctx, const imm::Box& area);

    void draw_outliner_list_(imm::Context& ctx, const Node& list);

    /**
     * @brief True for a row that is off screen and collapsed: the caller reserves its height
     *        instead of drawing it. A 1000-object scene then pays for the rows in view, not for
     *        every row's widgets, icons and strings each frame -- while scrolling, the content
     *        height and drop targets stay exactly as if every row were drawn. An open row is
     *        always drawn (its children's height isn't known without walking them), and so is
     *        the row being renamed.
     */
    bool outliner_row_skippable_(imm::Context& ctx, ObjectId id, bool flat);

    void draw_outliner_row_(imm::Context& ctx, ObjectId id, bool flat);

    /// Which column of row toggles a drag-paint is in (None: no drag in progress).
    enum class ToggleColumn { None, Eye, Monitor };

    /**
     * @brief A row's eye and monitor toggles, including the gestures the file doc describes.
     *
     * Both act on PRESS rather than on release, because that is what lets the same gesture
     * carry on into a drag across the column.
     *
     * @param eye       The eye (viewport visibility) box.
     * @param mon       The monitor (saved `active` flag) box.
     * @param hidden    The row is hidden in the viewport (isolation included).
     * @param isolated  Hidden by Edit / Sculpt Mode isolation: the eye is not the owner.
     * @param active    The row's effective `active` flag.
     */
    void draw_row_toggles_(imm::Context& ctx, ObjectId id, const imm::Box& eye, const imm::Box& mon,
                           bool hidden, bool isolated, bool active);

    /** @brief `id`, followed by every descendant in document order when `with_children`. */
    std::vector<ObjectId> row_subtree_(ObjectId id, bool with_children) const;

    /** @brief Hides or reveals rows in the viewport. Editor-only: the document is untouched. */
    void set_rows_hidden_(const std::vector<ObjectId>& ids, bool hide);

    /**
     * @brief Writes the saved `active` flag of every row in `ids` as ONE undoable step, merged
     *        with the rest of the gesture (end_merge() closes it when the button comes up).
     */
    void set_rows_active_(const std::vector<ObjectId>& ids, bool value);

    void draw_object_context_menu_(imm::Context& ctx, ObjectId target);

    ToggleColumn drag_column_ = ToggleColumn::None;   ///< the column the pointer is painting
    bool drag_value_ = false;                         ///< the state being copied onto the rows
    bool drag_children_ = false;                      ///< Shift was held at press: whole subtrees
    std::set<ObjectId> drag_painted_;                 ///< rows this drag has already set
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_UI_OUTLINER_H
