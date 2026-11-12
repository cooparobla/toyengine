# editor/anim

The animation Timeline's model: one clip file, and posing a live rig from it.

| File | Purpose |
|---|---|
| [`clip_model.h`](clip_model.h) | The editor's model of one animation clip file (toy::anim's YAML schema) and its undo. |
| [`clip_pose.h`](clip_pose.h) | Poses a live rig at a time — the Timeline's preview in edit mode, where no Animator is running. |

The Timeline panel itself is `../app/ui/timeline.cpp`. Clip authoring from the asset side is
the `create-animation` skill.

**Scope boundary, as of the `toy::rig` split:** this module and the Timeline panel still only
know about `toy::anim`'s `Animator` — a "rig" here means an object with an `Animator`
component plus its SceneObject children (`TimelinePanel::rig_of_()` walks ancestors looking for
exactly that component type). A `toy::rig::Rig` + `SkeletalAnimator` character (bones are not
SceneObjects at all) is invisible to the Timeline: `rig_of_()` never finds it, so Pose mode,
the dope sheet and clip authoring do not work on `mannequin`, `mannequin_ik`, `horse`,
`tentacle`, `ik_watcher` or `ik_reacher` today. A parallel `editor/rig/` module (bone-index
selection, a `BoneClipModel`, a rig-aware bone drawer/picker in
`../app/ui/viewport_chrome.cpp`) is the planned fix and has not been started — see the
`right-now-we-have-bubbly-glacier` plan's Editor section for the target design.
