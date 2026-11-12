/**
 * @file weights.h
 * @brief Binding a mesh to a rig, Blender's "Armature Deform with Automatic Weights": the rig's
 *        joints become vertex groups and every vertex is weighted to the bones around it, so a
 *        rig put under a mesh deforms it without a single stroke of Weight Paint.
 *
 * The weights come from toy::scene::compute_auto_weights() through mesh/auto_weights.h; this
 * file resolves WHAT to bind -- which object is the skin, which is the rig, and where that
 * rig's bones run at its REST pose, since the rest pose is the bind pose. Object Mode's
 * Object > Parent menu binds (adding the SkinnedMeshRenderer as it goes); Weight Paint's
 * Weights menu and its Auto Weights panel re-run the generator on an already bound mesh.
 *
 * One class per panel, owning its own state (see editor/app/README.md). EditorApp holds it
 * behind a forward declaration, so editing this header recompiles this panel and
 * editor_app.cpp -- not every panel.
 */

#ifndef TOYEDITOR_APP_UI_WEIGHTS_H
#define TOYEDITOR_APP_UI_WEIGHTS_H

#include "editor/app/editor_panel.h"

#include "editor/mesh/auto_weights.h"   // AutoWeightReport, AutoWeightSettings

namespace toy::editor {

/** @brief See the file doc. */
class WeightsPanel : public EditorPanel {
public:
    using EditorPanel::EditorPanel;

    /// EditorApp owns this panel and drives it; the declarations below keep the access level
    /// they had inside EditorApp's body.
    friend class EditorApp;

    // Called by sibling panels, so part of this panel's interface.
    bool bind_targets_(ObjectId& skin, ObjectId& rig);
    bool bind_to_rig_(ObjectId skin, ObjectId rig, BindWeights kind);

    // Called by sibling panels, so part of this panel's interface.
    bool regenerate_weights_from_rig_(BindWeights kind);
    void draw_auto_weights_panel_(imm::Context& ctx);

public:
    /** @brief What a bind writes into the vertex groups (Blender's three parenting options). */

private:

    /**
     * @brief The skin mesh and the rig a bind would use, from the selection.
     *
     * The skin is the selected object with a mesh (the active one first). The rig is another
     * selected object that is an ancestor of it, else the nearest Animator above it, else its
     * parent -- the same order the engine's own SkinnedMeshRenderer::find_rig_() resolves in.
     *
     * @return False when the selection offers no mesh, or no rig with a joint in it.
     */

    /**
     * @brief `rig`'s deform bones, their segments in `skin`'s object space, at the rest pose.
     *
     * Every joint under the rig deforms (is_joint_object_(): not a mesh, a camera or a light),
     * except the rig root itself -- the skin already hangs off that, so weighting it would only
     * take weight from the first real bone. A `bones:` list on the SkinnedMeshRenderer narrows
     * the set to exactly those, as the engine's palette does. The segments are the ones the
     * viewport draws as bone glyphs: joint -> each child joint.
     */
    std::vector<BoneSegments> rig_bone_segments_(ObjectId rig, ObjectId skin);

    /** @brief The SkinnedMeshRenderer's `bones:` entries on `skin` (by name; empty = all joints). */
    std::vector<std::string> skinned_bone_names_(ObjectId skin);

    /**
     * @brief Binds `skin` to `rig`: weights its mesh file and makes the object a skinned one.
     *
     * Writes the weights as one undoable mesh edit, saves the mesh (Object Mode's usual
     * commit), then ensures the object carries `SkinnedMeshRenderer { mesh_path }` with its
     * MeshRenderer left holding the material alone -- and `rig:` only when neither of the
     * engine's fallbacks would find this rig on their own.
     */

    /** @brief Moves `skin`'s mesh onto a SkinnedMeshRenderer, adding one if it has none. */
    void ensure_skinned_renderer_(ObjectId skin, ObjectId rig, const std::string& mesh_key);

    /**
     * @brief Lets go of `path` as a static mesh, so the SkinnedMeshRenderer can claim it.
     *
     * An AssetManager slot is keyed by the file's path alone and refuses to reinterpret one C++
     * type's payload as another's, so while anything still holds the file as a
     * `data::Mesh`, loading it as a `SkinnedMeshSource` fails and the bound mesh never appears.
     * Binding is exactly the moment the object stops using the file statically: drop the live
     * MeshRenderer's handle and evict the slot before the rebuild asks for the skinned form.
     * A file some OTHER object still draws statically keeps its slot pinned, so the eviction is
     * a no-op and the AssetManager logs the type clash -- one mesh file cannot be both at once.
     */
    void release_static_mesh_(ObjectId skin, const fs::path& path);

    /** @brief Logs what a bind wrote, and what it could not. */
    void report_bind_(const AutoWeightReport& report, const std::string& rig_name, size_t bones);

    /** @brief Weight Paint's Weights menu: re-generates the open mesh's weights from its rig. */

    /** @brief Properties > Tool > Auto Weights (Weight Paint): the generator's settings and button. */

public:
    /** @brief Object > Parent > Bind to Rig: binds the selection's mesh to its rig. */
    bool bind_selection_to_rig(BindWeights kind) {
        ObjectId skin = 0, rig = 0;
        return bind_targets_(skin, rig) && bind_to_rig_(skin, rig, kind);
    }
    /** @brief Weights > Automatic Weights from Rig, on the mesh already open in Weight Paint. */
    bool regenerate_weights_from_rig(BindWeights kind) { return regenerate_weights_from_rig_(kind); }
    /** @brief The settings the bind commands and the Auto Weights panel share. */
    AutoWeightSettings& auto_weight_settings();
    /** @brief The skin and rig a bind would use (both 0 when the selection offers none). */
    bool bind_targets(ObjectId& skin, ObjectId& rig) { return bind_targets_(skin, rig); }

private:
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_UI_WEIGHTS_H
