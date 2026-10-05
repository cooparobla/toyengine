[Editor manual](README.md) > Animation

You animate objects in the **Timeline**, which shares the bottom of the window with the
**Console**. You pose objects in the viewport, the editor turns each pose into *keys* (saved
values at a frame), and the Timeline plays the motion between them.

![The Timeline open under the viewport, showing the wave clip of a robot arm with keys on the
shoulder and elbow rows](../images/editor/timeline.jpg)

*The Timeline at frame 18 of the `wave` clip. The blue line is the playhead; each diamond is a
key.*

What you see in the Timeline, from top to bottom:

- **The header.** From left to right: the clip list (here `wave`), **Jump to Start** (the
  circular arrow), **Play / Pause**, the **Frame** number, **Record** (the red dot),
  **Insert Keyframe** (**+**), **Rest Pose**, **All Objects**, **Length** and the repeat mode
  (here **loop**). Hover any of them for a tooltip.
- **The ruler.** Frame numbers. The Timeline runs at 30 frames a second.
- **The dope sheet.** A **Summary** row, then one row per object, with a diamond for every key.

Two words used on this page:

- A **rig** is an object with an **Animator** component plus every object parented under it.
  Here `robot_arm` is the rig, and `shoulder` and `elbow` are parts of it.
- A **clip** is one named animation of a rig, such as `wave` or `walk`. A rig can have many
  clips.

## Open the Timeline

1. Click the **Timeline** tab at the bottom of the window, next to **Console**.
   If the bottom area is hidden, turn on **Window > Console** first.
2. Select an object in the rig you want to animate.

The Timeline shows the rig of the selected object. It keeps showing that rig while you click
objects outside it, and clears when you open another scene.

If the Timeline shows a message instead of keys:

| Message | What to do |
|---|---|
| **Select an object to animate** | Select an object. |
| **No Animator on this object or its parents** | The object isn't part of a rig yet. See [Make an object animatable](#make-an-object-animatable). |
| **Placed from ...: animate it in its object asset** | The object is a copy of an [object asset](scenes-and-objects.md#reuse-an-object-in-many-places). Click **Open Object Asset** and animate it there; every placed copy plays the same clips. |
| **Rig ...: add a clip to start animating** | The rig has no clips yet. See [Create a clip](#create-a-clip). |

## Make an object animatable

1. Select the object that should be the top of the animation, such as a character's root
   object.
2. Click **Add Animator** in the Timeline.

The object is now a rig. You can animate it and everything parented under it.

You can also add the **Animator** component from **Add Component** in the Properties panel (see
[Scenes and objects](scenes-and-objects.md#add-and-edit-components)). Its settings are:

| Setting | Meaning |
|---|---|
| **Auto play** | The clip the game plays when it starts. Your first clip is filled in for you. |
| **Speed** | How fast clips play (1 is normal speed). |
| **Default crossfade** | How long, in seconds, the game blends when it switches from one clip to another. |

> [!WARNING]
> Keys remember objects by their name and place in the rig. If you rename an object inside a
> rig, or move it under a different parent, its existing keys stop applying to it.

## Create a clip

1. Click the clip list at the left of the Timeline header. It says **No Clip** when the rig has
   none.
2. Choose **New Clip**.

The new clip is named **Clip** (then **Clip.1**, **Clip.2** and so on) and opens straight away.
To switch to another clip, open the list and click its name.

Pressing **I** on a rig with no clips also creates a clip, and keys the pose into it.

## Rename or delete a clip

1. Open the clip you want to change.
2. Click the clip list.
3. Do one of these:
   - Type a new name in the **Rename** field.
   - Choose **Delete Clip**.

Your keys are saved as you make them, so a clip never needs saving on its own. Creating,
renaming and deleting clips does change the scene, though: save the scene afterwards with
**Ctrl+S**.

## Record a pose

The quickest way to animate is to turn on **Record** and pose the rig. Every move you make is
keyed at the playhead.

![The viewport with a red border and the label "Recording: moves become keys at frame 12", the
robot arm bent at the elbow](../images/editor/record_autokey.jpg)

*While recording, a red border surrounds the viewport and its label tells you which frame the
keys go to.*

1. Click the red **Record** button in the Timeline header.
2. Move the playhead to a frame: click the ruler, or type in the **Frame** field.
3. Move, rotate or scale an object in the rig, any way you like: the gizmo, **G** / **R** /
   **S**, or the fields in the Properties panel.
4. Repeat steps 2 and 3 for each pose.
5. Click **Record** again to stop.

Each change becomes a key on the frame under the playhead, for just what you changed (position,
rotation or scale). One drag is one step to undo.

> [!NOTE]
> Without **Record**, moving an object in a rig changes its *rest pose*: how it sits in the
> scene when no animation is playing. The clip still drives anything it has keys for, so the
> move may not show until you turn on **Rest Pose** in the Timeline header.

## Insert keys by hand

To key without recording, put the playhead on a frame and use any of these:

- Press **I** with the mouse over the viewport or the Timeline. This keys the position,
  rotation and scale of the selected objects in the rig, or of the rig's top object if none of
  its objects is selected.
- Click **Insert Keyframe** (**+**) in the Timeline header. It does the same as **I**.
- Right-click an empty spot on the dope sheet and choose **Insert Keyframe Here**. The playhead
  jumps to that frame and keys it.
- Click the small diamond next to **Location**, **Rotation** or **Scale** in the Properties
  panel. This keys only that one value. A filled diamond means it already has a key on this
  frame; click it again to update the key.

Keys always land on whole frames.

## Edit keys on the dope sheet

Each object in the rig has a row. Click the arrow next to an object's name to show separate
**Position**, **Rotation** and **Scale** rows under it. Clicking an object's name selects it in
the scene.

| To | Do this |
|---|---|
| Select a key | Click its diamond. A diamond on the **Summary** row selects every key on that frame. Selected keys turn orange. |
| Add to the selection | **Shift**+click more diamonds. |
| Select every key | Press **A**, or right-click an empty spot and choose **Select All Keys**. |
| Deselect | Click an empty spot. |
| Move keys in time | Drag a selected diamond left or right. Dropping it onto another key on the same row replaces that key. |
| Delete keys | Press **X** or **Delete**, or right-click a key and choose **Delete Keyframes**. |
| Move the playhead to a key | Right-click the key and choose **Jump to Key**. |

To see only some objects, turn off **All Objects** in the header: the dope sheet then lists just
the rig's top object and the objects that have keys or are selected.

## Change how motion eases between keys

By default an object moves at a steady speed from one key to the next. To change that:

1. Select one or more keys.
2. Right-click one of them.
3. Choose an option under **Interpolation**.

| Option | The motion from this key to the next |
|---|---|
| **Linear** | Moves at a steady speed. |
| **Constant (Step)** | Holds still, then jumps at the next key. |
| **Ease In** | Starts slowly and speeds up. |
| **Ease Out** | Slows down at the end. |
| **Ease In-Out** | Starts and ends slowly. |

A tick in the menu shows the option that all the selected keys already share.

## Play and scrub through a clip

| To | Do this |
|---|---|
| Play or pause | Click **Play / Pause**, or press **Space** with the mouse over the Timeline. |
| Scrub | Click or drag on the ruler. The playhead snaps to whole frames; hold **Shift** to scrub smoothly. |
| Step one frame | Press **Left** or **Right**. |
| Jump to the next or previous key | Press **Up** or **Down**. |
| Go back to frame 0 | Click **Jump to Start**, or press **Home** (this also fits the whole clip in view). |
| Fit the whole clip in view | Right-click an empty spot and choose **Frame All**. |
| Zoom in or out on frames | Scroll the mouse wheel over the keys. |
| Scroll sideways | **Shift**+scroll, a sideways swipe, or drag with the middle mouse button. |
| Scroll the list of objects | Scroll the mouse wheel over the object names. |

The viewport shows the clip at the playhead whenever the Timeline tab is open. This preview is
never saved into your scene. Your rig goes back to its rest pose when you switch to the
**Console** tab, turn on **Rest Pose**, or press Play to run the game.

## Set the clip's length and looping

- **Length** is the clip's length in frames. Drag it or type a number. Keys you insert after the
  end make the clip longer. The frames past the end are shaded on the dope sheet.
- The button after **Length** sets what happens at the end. Click it to switch between:
  - **loop**: start again from the beginning.
  - **once**: play once and hold the last frame.
  - **pingpong**: play forwards, then backwards, and repeat.

## Undo animation edits

Press **Ctrl+Z** to undo and **Ctrl+Shift+Z** to redo. While the mouse is over the Timeline, or
while **Record** is on, these undo your key edits, interpolation, length and looping changes.
Anywhere else they undo changes to the scene.

---

Sources: `editor/app/ui/timeline.inl`, `editor/anim/clip_model.h`, `editor/anim/clip_pose.h`,
`editor/app/ui/assets.inl`, `editor/app/ui/properties.inl`, `editor/app/editor_app.h`,
`editor/app/ui/statusbar.inl`, `editor/schema/component_schema.h`

Previous: [Materials and textures](materials-and-textures.md) | Next: [UI designer](ui-designer.md)
