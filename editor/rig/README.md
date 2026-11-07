# editor/rig — bone editing, as pure functions

Everything the editor does *to* a skeleton, with no `EditorApp`, no undo stack and no GPU: the
panels in `editor/app/` supply a selection, call one of these, and wrap the result in a single
undoable edit. Same split as `editor/mesh/` and `editor/terrain/`, and the reason this directory
carries real test coverage while `app/ui/` carries almost none.

| File | Holds |
|---|---|
| `rig_model.h/.cpp` | `RigModel` / `RigModelBone` — the editable rest skeleton, its YAML round trip (through the *runtime's* `coopa::rig::parse_rig`, so the format cannot drift), the authoring primitives (`add_bone`, `extrude_bone`, `set_head`/`set_tail`, `rename_bone`, `reparent_bone`, `delete_bone`, `sort_parent_first`) and `roll_to_align_z` |
| `rig_selection.h` | Blender's edit-armature selection — per-bone head/tail flags plus an active bone — with click/box/all/none/invert, the median pivot, and `transform_endpoints`, which is what `G`/`R`/`S` actually apply |
| `rig_edit_ops.h/.cpp` | The armature operators: Subdivide, Fill, Merge, Recalculate Roll, Align, Symmetrize, and the live X-mirror map |
| `bone_names.h` | `mirrored_bone_name` — the `_l`/`_r` side flip every mirror operation keys on |
| `bone_pick.h` | Projecting an octahedral bone and hit-testing it: body, endpoints, or the combined `pick_bone_or_endpoint` |
| `bone_pose_edit.h` | Pose mode's side: world-delta → pose-channel math, clear, mirror, copy/paste |

## Two things that will bite you

**`set_head` on a connected bone moves its PARENT'S TAIL.** That is deliberate and correct — the
two are one joint, and it is the joint Blender moves. It means you cannot shorten a parent
without its connected child's head following; disconnecting is the explicit way out. Every
endpoint transform relies on this rather than special-casing it, which is why
`transform_endpoints` writes *absolute* positions computed from a base snapshot: a joint reached
twice gets written twice with the same value, so order never matters.

**A mirrored roll is not `-roll`.** `coopa::rig::bone_rest_matrix` seeds its basis from world +Z,
*except* for a bone running along Z, where it switches to +X — and mirroring negates that seed,
making the answer `180 - roll` there instead. `mirrored_roll` therefore derives it numerically
(reflect the bone's rest Z, ask `roll_to_align_z`), correct in both branches by construction.
`rig_edit_ops_test.cpp` checks a fan of directions including ±Z for exactly this reason.

## Tests

`tests/editor/unit/rig_model_test.cpp`, `rig_selection_test.cpp`, `rig_edit_ops_test.cpp`, and
`bone_pick_test.cpp` / `bone_pose_edit_test.cpp`; end to end through real input in
`tests/editor/gpu/rig_edit_mode_test.cpp` and `rig_pose_mode_test.cpp`.

```bash
./build/tests/toyengine_editor_tests --suite rig_selection   # or rig_edit_ops, rig_model
```
