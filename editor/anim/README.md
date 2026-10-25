# editor/anim

The animation Timeline's model: one clip file, and posing a live rig from it.

| File | Purpose |
|---|---|
| [`clip_model.h`](clip_model.h) | The editor's model of one animation clip file (coopa::anim's YAML schema) and its undo. |
| [`clip_pose.h`](clip_pose.h) | Poses a live rig at a time — the Timeline's preview in edit mode, where no Animator is running. |

The Timeline panel itself is `../app/ui/timeline.cpp`. Clip authoring from the asset side is
the `create-animation` skill.
