// editor/app/ui/weights.inl -- included inside EditorApp's class body.
//
// Binding a mesh to a rig, Blender's "Armature Deform with Automatic Weights": the rig's joints
// become vertex groups and every vertex is weighted to the bones around it, so a rig put under
// a mesh deforms it without a single stroke of Weight Paint.
//
// The weights come from toy::scene::compute_auto_weights() through mesh/auto_weights.h; this
// file resolves WHAT to bind -- which object is the skin, which is the rig, and where that
// rig's bones run at its REST pose, since the rest pose is the bind pose. Object Mode's
// Object > Parent menu binds (adding the SkinnedMeshRenderer as it goes); Weight Paint's
// Weights menu and its Auto Weights panel re-run the generator on an already bound mesh.

public:
    /** @brief What a bind writes into the vertex groups (Blender's three parenting options). */
    enum class BindWeights { Automatic = 0, Envelope, EmptyGroups };

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
    bool bind_targets_(ObjectId& skin, ObjectId& rig);

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
    bool bind_to_rig_(ObjectId skin, ObjectId rig, BindWeights kind);

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
    bool regenerate_weights_from_rig_(BindWeights kind);

    /** @brief Properties > Tool > Auto Weights (Weight Paint): the generator's settings and button. */
    void draw_auto_weights_panel_(imm::Context& ctx);

public:
    /** @brief Object > Parent > Bind to Rig: binds the selection's mesh to its rig. */
    bool bind_selection_to_rig(BindWeights kind) {
        ObjectId skin = 0, rig = 0;
        return bind_targets_(skin, rig) && bind_to_rig_(skin, rig, kind);
    }
    /** @brief Weights > Automatic Weights from Rig, on the mesh already open in Weight Paint. */
    bool regenerate_weights_from_rig(BindWeights kind) { return regenerate_weights_from_rig_(kind); }
    /** @brief The settings the bind commands and the Auto Weights panel share. */
    AutoWeightSettings& auto_weight_settings() { return auto_weights_; }
    /** @brief The skin and rig a bind would use (both 0 when the selection offers none). */
    bool bind_targets(ObjectId& skin, ObjectId& rig) { return bind_targets_(skin, rig); }

private:
