[Editor manual](README.md) > The 3D viewport

The viewport is the large area in the middle of the window where you look at your scene and
arrange it. It uses Blender's controls: the middle mouse button moves the view, the left button
selects, and single keys such as **G**, **R** and **S** move, rotate and scale.

![The viewport in Solid shading, showing a cube on the ground, the toolbar on the left, the
navigation gizmo on the right and the Viewport Shading popover open in the
header](../images/editor/viewport_solid.jpg)

*The Viewport Shading popover is open in the top-right corner, with Ambient Occlusion on.*

What you see:

1. **The header**, the bar along the top. From the left: the mode menu (**Object Mode**), the
   **View**, **Select**, **Add** and **Object** menus, and on the right the **Transform
   Orientation** dropdown (**Global**), the **Snap** button, **Colliders**, **Overlays**, **Toggle X-Ray** and
   the four shading buttons.
2. **The toolbar**, down the left edge: **Select Box**, **Cursor**, **Move**, **Rotate** and
   **Scale**.
3. **The view name**, in the top-left corner (here **User Perspective**), with the scene name
   and the active object under it.
4. **The navigation gizmo**, the coloured axis ball in the top-right corner, with zoom, pan,
   camera and projection buttons below it.
5. **The 3D cursor**, the red-and-white ring on the ground. New objects appear there.

Viewport keys work only while the mouse pointer is over the viewport. They are ignored while
you type in a text field or while a menu is open.

Editing a mesh's vertices is covered in [Modeling](modeling.md), and brushes in
[Sculpt and paint](sculpt-and-paint.md).

## Move around the view

The view turns around a focus point in the scene. Use the mouse, a trackpad or the keyboard.

| To | Mouse | Trackpad |
|---|---|---|
| Orbit | Drag with the middle button | Two-finger swipe |
| Pan | **Shift** + middle-drag | **Shift** + two-finger swipe |
| Zoom | Scroll wheel, or **Ctrl** + middle-drag | Pinch, or **Ctrl** + two-finger swipe |
| Pan up and down | **Shift** + scroll wheel | |
| Pan left and right | **Ctrl** + scroll wheel | |

No middle button? Hold **Alt** and drag with the left button to orbit. Add **Shift** to pan or
**Ctrl** to zoom. On a Mac, **Cmd** works the same as **Ctrl** for these mouse moves.

With the keyboard:

| Key | Action |
|---|---|
| **Numpad 4** / **Numpad 6** | Orbit left / right |
| **Numpad 8** / **Numpad 2** | Orbit up / down |
| **Numpad +** or **=** | Zoom in |
| **Numpad -** or **-** | Zoom out |
| **F**, **.** or **Numpad .** | Frame the selected objects (**F** only in Object Mode) |
| **Home** | Frame everything |

Framing fits the view around the selection. With nothing selected, it frames everything.

### Use the navigation gizmo

The axis ball in the top-right corner shows which way you are looking.

- Drag the ball to orbit.
- Click an axis ball to look straight along that axis. The balls labelled **X**, **Y** and
  **Z** are the positive ends.
- Drag the magnifier button up and down to zoom, and the hand button to pan.
- Click the camera button to look through the scene camera.
- Click the bottom button to switch between perspective and orthographic.

## Look from the front, top or side

1. Point at the viewport.
2. Press **Numpad 1** (front), **Numpad 3** (right) or **Numpad 7** (top). Add **Ctrl** for the
   opposite side: back, left or bottom.

The view switches to orthographic, which shows the scene without perspective. When you orbit
away, it goes back to perspective. Press **Numpad 5** to switch between perspective and
orthographic yourself; the view then stays the way you set it.

The same views are in **View > Viewpoint**, and in a menu that the backtick key (**`**) opens
under the pointer.

### Look through the scene camera

- Press **Numpad 0** to see what the scene camera sees. If the scene has several cameras, the
  editor uses the main camera.
- To move the camera to where you are looking, press **Ctrl+Alt+Numpad 0**. You can undo this.

If the scene has no camera, the Console says so.

## Change how the scene is drawn

The four round buttons on the right of the header set the shading. You can also press **Z** for a
menu under the pointer, or **Shift+Z** to switch between **Wireframe** and **Solid**.

| Shading | What you see |
|---|---|
| **Wireframe** | Edges only |
| **Solid** | Plain shading that doesn't depend on the scene's lights; best for modelling |
| **Material Preview** | Material colours and textures, without lighting |
| **Rendered** | Exactly what the game draws |

Each kind of asset remembers its own shading. A material, for example, opens in the shading you
last used for materials.

The small arrow right of the shading buttons opens the **Viewport Shading** popover:

- In **Solid** and **Material Preview**, **Ambient Occlusion** adds soft shadows in corners and
  creases. It stays off when ambient occlusion (**SSAO**) is turned off in the project's
  [render settings](project-settings.md#render-tab).
- In **Rendered**, you can turn **Shadows**, **Contact Shadows** and **Soft Lighting** on and
  off for this session. The features under **Set at startup** are listed but can't be changed
  here; change them in [Project settings](project-settings.md#apply-settings-that-need-a-restart), then choose **Render > Restart
  Renderer**.

## See through surfaces with X-Ray

![A cube in Edit Mode with X-Ray on, its faces translucent so the vertices on the back are
visible](../images/editor/xray_edit.jpg)

*With X-Ray on, the back vertices of the cube show through, and box selection picks them up.*

X-Ray makes surfaces see-through, so you can see and select what is behind them.

1. Press **Alt+Z**, or click **Toggle X-Ray** in the header (the button left of the shading
   buttons).
2. To change how see-through surfaces are, open the **Viewport Shading** popover and drag
   **X-Ray Alpha**.

X-Ray works in **Solid** and **Material Preview** shading. It is off in Sculpt and paint modes.

> [!TIP]
> In Edit Mode, box selection only picks vertices that face you. Turn on X-Ray (or use
> **Wireframe**) to select the ones behind as well.

## Choose what the viewport shows

The **Overlays** button in the header turns all the helper drawings on or off. Click the arrow
next to it to pick which ones show:

| Overlay | Shows |
|---|---|
| **Grid** | The floor grid |
| **Origins** | The origin glyph on every object with no mesh of its own, and a dot on the selection |
| **Cameras / Lights** | Camera frustums and light symbols |
| **3D Cursor** | The 3D cursor |
| **Statistics** | Object, vertex and face counts in the corner |

The grid lies on the ground. Its spacing changes as you zoom, so the lines never get too dense
or too sparse. In the front, back and side views it turns to face you. The red and green lines
are the X and Y axes.

### Object origins

A mesh-less object -- a marker, a parent you group things under, an IK target or attach
socket -- draws a glyph at its origin so you can see and click it.

How an origin draws has a viewer-wide default you can play with, which any object may
override. The default is the **Object Origins** section of the Overlays popover, and
**Edit > Preferences > Viewport**; the override is **Properties > Object > Origin Gizmo**.

| Style | Looks like |
|---|---|
| **Axes** | A line through the origin along each of the object's own axes. The default |
| **Orb** | A small ball with a short tick down each axis -- the least cluttered when several overlap |

A character's bones are **not** objects, and never draw an origin glyph of their own: a
skeleton is a `Rig` component's own bone list (`libs/libcoopa/toyengine/rig/README.md`), posed by
a `SkeletalAnimator` and the bone-space IK components in `src/toyengine/rig/`, not by Transforms parented
under an `Animator`. This is deliberate -- a mesh-less object near a rig (an IK target empty, an
attach socket) is a plain marker, never a clickable bone of its own.

## Pose a rig

Select a `Rig`-based character and pick **Pose Mode** from the mode menu in the viewport header.
Its bones draw as octahedra; click one to select it, Shift-click to add, drag a box to select
several. **G** / **R** / **S** move, rotate and scale the selected bones' pose (with the usual
axis locks, typed values and Ctrl snapping), **Alt+G** / **Alt+R** / **Alt+S** clear a channel
back to rest, **Ctrl+C** / **Ctrl+V** copy and paste a pose by bone name, **Ctrl+M** mirrors a
posed `_l` limb onto its `_r` partner, and **I** keys the selected bones into the open
`SkeletalAnimator` state's clip at the playhead. A pose edits the live pose only -- the rest
skeleton is edited in Edit Mode, below.

## Edit a rig's skeleton

**Tab** on a rigged object enters **Edit Mode on its skeleton** -- the same key that edits a
mesh's vertices, because editing a rig means editing its bones. (On an object that has both a rig
and a mesh of its own, Tab cycles Object -> Rig -> Mesh -> Object.) A new rig comes from
**Add > Armature**, which creates the object, its `Rig` component and a one-bone
`assets/rigs/<name>.yaml`, then drops straight into Edit Mode on it.

A bone has three selectable parts, as in Blender: its **head**, its **tail**, and its **body**.
Clicking the body selects both endpoints; clicking a handle selects just that one. What is
selected is what moves -- grabbing only a tail lengthens the bone and changes its direction,
while grabbing both slides it rigidly. A bone marked **connected** shares its head with its
parent's tail, so moving either moves that one joint.

| Key | Does |
|---|---|
| Click / Shift-click / drag | select a head, tail or body; extend; box select |
| `A` / `Alt+A` / `Ctrl+I` | select all / none / invert (per endpoint) |
| `G` / `R` / `S` | move / rotate / scale the selected endpoints |
| `Ctrl+R` | roll (the bone's twist about its own axis) |
| `E` | extrude a new connected child from every selected tail |
| `Shift+A` | add a free bone at the 3D cursor |
| `Shift+D` | duplicate the selected bones |
| `F` | fill a new bone between two selected endpoints |
| `Alt+M` | merge a connected run of selected bones into one |
| `Ctrl+P` / `Alt+P` | parent the selection to the active bone / clear parent |
| `Ctrl+N` | recalculate roll (to an axis, the 3D cursor, or the active bone) |
| `Ctrl+Alt+A` | align the selection's roll to the active bone |
| `X` / `Delete` | delete the selected bones (children reparent) |
| `W` or right-click | the Armature menu, including **Symmetrize** and **X-Axis Mirror** |

**Symmetrize** copies every fully-selected `_l`/`_r` bone onto its opposite-side partner,
creating it if needed; **X-Axis Mirror** keeps the two sides in step live, so dragging one arm
moves the other as one undoable step. Renaming a bone (the sidebar's **N** panel, Item > Bone)
repoints every referrer the project can find -- clips, IK fields, `FootIK` legs, `Ragdoll` bones
and `BoneAttachment`s -- because a bone name is a contract, not a label.

## Pose a property-animation rig

A few assets (`robot_arm`, `bouncing_ball`) use the older rig style, where a joint is an ordinary
object with an **Animator** at or above it, posed with its own Transform. Such a rig has no `Rig`
component and therefore no Edit Mode on its skeleton -- its "bones" are objects, moved in Object
Mode. Select the object with the **Animator**, or a joint under one, and pick **Pose Mode** from
the mode menu in the viewport header.

In it:

- Every joint draws as a **bone** reaching its child joints, so you can see the skeleton --
  Pose mode's own display: it draws regardless of the **Origins** overlay checkbox, even on a
  rig whose own origin style is Axes or Orb.
- Click a bone anywhere along its body to select that joint. Clicking the same place again
  steps past it to whatever is behind.
- **G**, **R** and **S** (and the gizmos) move the **pose**, not the object. The pose stays
  where you put it -- the clip no longer snaps it back the moment you let go -- and the rest
  transform saved in the scene is left alone.
- **I** keys the selected joints at the playhead. Turn on **Auto-Key** (the red button in the
  Timeline) and every edit keys itself as you make it.
- You can pose with no clip open at all. Keys need somewhere to go, so the first **I** makes
  a clip if the rig has none.
- Leaving Pose mode hands the rig back: the Timeline previews the clip again, and the scene's
  rest pose is untouched underneath.

**Size** is how big the glyph is in metres, up to 0.5. Turn it down on a dense rig.

An object's settings apply to **every selected object**, as one undo step, and they are
**inherited**. What an object actually draws with is, in order:

**Style:**

1. its own **Style**, or the nearest **ancestor's**, when it is Axes or Orb;
2. failing that, the **viewer-wide default**, which starts at Axes.

**Size:**

1. its own **Size**, or the nearest ancestor's, when it is not **0**;
2. the **viewer-wide default**, which starts at 0.5 m.

The panel shows what is in use (`In use: Orb at 0.200 m`) whenever a value is inherited
rather than the object's own.

The settings are editor-only: the game ignores `origin_style` and `origin_size` on a Transform.

A glyph is **clickable where it is drawn**: click anywhere along a bone's body to select
that joint, anywhere along an axis line, or on an orb's ball. Clicking the origin itself
always works too. A glyph that is hidden takes no clicks.

Where things overlap, **clicking the same place again steps past what is selected** to the
next thing under the pointer, as Blender does. So a bone buried in a character's chest takes
two clicks: the first picks the mesh in front, the second the bone inside it. Keep clicking
to go round the rest and back to the start; click somewhere else and it begins again with
whatever is in front.

The **Origins** checkbox in the Overlays popover is what shows and hides the glyphs;
**Cameras / Lights** only covers camera frustums and light symbols.

## Snap to the grid

Snapping makes a transform move in fixed steps.

- Hold **Ctrl** (or **Cmd** on a Mac) while you drag a gizmo or use **G**, **R** or **S**.
- To snap all the time, click the **Snap** button (the magnet) in the header. Holding **Ctrl**
  then turns snapping off.

| Transform | Step |
|---|---|
| Move | The grid spacing you can see, which gets finer as you zoom in |
| Rotate | 15 degrees with the gizmo, 5 degrees with **R** |
| Scale | 0.1 |

To change the steps, open **Properties > Tool > Active Tool**. Turn off **Increment Follows
Grid** to use a fixed move step, and set **Rotate Increment** for the gizmo.

## Place the 3D cursor

The **3D cursor** is the red-and-white ring that marks where new objects appear.

- **Shift** + right-click puts it on the surface under the pointer, or on the ground.
- With the **Cursor** tool, a click puts it there.
- **Shift+C** puts it back at the centre of the world and frames everything.

Press **Shift+S** for the Snap menu:

| Command | Does |
|---|---|
| **Cursor to Selected** | Moves the cursor to the selection |
| **Cursor to World Origin** | Moves the cursor to the centre of the world |
| **Cursor to Grid** | Moves the cursor to the nearest grid point |
| **Selection to Cursor** | Moves the selected objects to the cursor (Object Mode only) |

## Hide and show objects

| Key | Action |
|---|---|
| **H** | Hide the selected objects |
| **Shift+H** | Hide everything except the selection |
| **Alt+H** | Show everything you hid |

Hidden objects are skipped by box select and **A**. The same commands are in the **Object**
menu.

### Focus on the object you're editing

When you go into Edit Mode or Sculpt Mode on an object, the editor hides the other objects
(lights stay on) and frames that object. When you go back to Object Mode, everything reappears
and the view returns to where it was. To keep the other objects visible, click the **Isolate in
Edit Mode** button in the header to turn it off. The editor remembers your choice.

## Add objects and drop assets

1. Press **Shift+A** with the pointer over the viewport, or click **Add** in the header.
2. Choose **Mesh**, **Light**, **Camera**, **Empty**, **Reflection Probe** or **Terrain**.

The new object appears at the 3D cursor. Lights and cameras appear a little above it. See
[Scenes and objects](scenes-and-objects.md#add-an-object) for more.

You can also drag an asset from the Asset panel and drop it on the viewport:

| Drop | Result |
|---|---|
| An object asset or a mesh | Placed on the ground under the pointer |
| A material | Given to the object under the pointer |
| A UI asset | Added to the scene as a HUD or menu |

## Use the right-click menu

Right-click in the viewport in Object Mode for a menu of common commands: **Edit Mode**,
**Duplicate**, **Delete**, **Rename**, **Parent to Active**, **Clear Parent (keep
transform)**, **Hide**, **Unhide All**, **Frame Selected** and **Set 3D Cursor Here**.

In Edit Mode, right-click opens the mesh tools menu (see [Modeling](modeling.md#other-mesh-tools)).

## Fill the window with the viewport

Press **Ctrl+Space**, or choose **View > Toggle Maximize Area**. The viewport fills the window.
Press it again to bring the other panels back.

## Save a rendered image

1. Choose **Render > Render Image**.

The editor saves the current full render as a PNG in the `renders` folder of your project, and
the Console shows the file name.

> [!NOTE]
> The menu shows **F12**, but the key doesn't work yet (not yet available). Use the menu.

## Use the viewport while the game runs

When you press **Play** or **F5** (see [Scenes and objects](scenes-and-objects.md#test-the-scene-with-play)), the header
changes colour and the corner shows **PLAYING** or **PAUSED**.

1. Click in the viewport to give the game your mouse and keyboard. The banner says **Click to
   control the game**.
2. Press **Esc** to take them back. The game keeps running.
3. Press **F5** or the stop button to end the game.

While the game has control, the editor only reacts to **Esc** and **F5**.

---
Sources: `src/editor/viewport/editor_camera.h`, `src/editor/viewport/gizmo.h`, `src/editor/viewport/modal_transform.h`, `editor/app/ui/viewport_chrome.inl`, `src/editor/app/trackpad.h`, `src/editor/app/editor_app.h`, `editor/app/ui/topbar.inl`, `editor/app/ui/properties.inl`, `editor/app/ui/statusbar.inl`
Previous: [Scenes and objects](scenes-and-objects.md) | Next: [Modeling](modeling.md)
