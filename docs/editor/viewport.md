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
   Orientation** dropdown (**Global**), the **Snap** button, **Overlays**, **Toggle X-Ray** and
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
| **Origins** | A dot at each object's origin |
| **Cameras / Lights** | Shapes for cameras, lights and empties |
| **3D Cursor** | The 3D cursor |
| **Statistics** | Object, vertex and face counts in the corner |

The grid lies on the ground. Its spacing changes as you zoom, so the lines never get too dense
or too sparse. In the front, back and side views it turns to face you. The red and green lines
are the X and Y axes.

## Show the toolbar and sidebar

- Press **T** to show or hide the toolbar on the left edge.
- Press **N** to show or hide the sidebar on the right edge.

The toolbar holds the tools for the current mode. In Object Mode:

| Tool | Use |
|---|---|
| **Select Box** | Click or drag to select. **B** picks this tool. |
| **Cursor** | Click to place the 3D cursor |
| **Move**, **Rotate**, **Scale** | Show a handle (a gizmo) on the selection that you drag |

Press **Shift+Space** for a menu of these tools under the pointer. Edit Mode adds modelling
tools, and Sculpt and paint modes show brushes instead.

The sidebar has two tabs:

- **Item** shows the active object's **Location**, **Rotation** and **Scale**, which you can
  type into or drag. In Edit Mode it shows the centre of the selected vertices instead.
- **View** shows the viewport's **Field of View** and the 3D cursor's exact **Location**.

## Select objects

| Do this | To |
|---|---|
| Click an object | Select it (click empty space to deselect everything) |
| **Shift**-click | Add an object to the selection; Shift-click it again to remove it |
| Drag on empty space | Select everything inside the box |
| **Shift**-drag / **Ctrl**-drag | Add to / remove from the selection with a box |
| **A** | Select all |
| **Alt+A** | Select none |
| **Ctrl+I** | Invert the selection |

The last object you click is the **active object**. Its name shows in the viewport corner and
its settings show in the Properties panel. The **Select** menu in the header has the same
commands. You can also select in the Hierarchy; see [Scenes and objects](scenes-and-objects.md#select-objects).

## Move, rotate and scale

![A selected cube being moved along the X axis, with a red guide line through it and the
distance shown at the bottom of the viewport](../images/editor/gizmo_move.jpg)

*Moving with **G** then **X**: the red line shows the locked axis, and the bottom-left readout
shows the distance moved.*

### With the keyboard

1. Select the object.
2. With the pointer over the viewport, press **G** to move, **R** to rotate or **S** to scale.
3. Move the mouse.
4. Click, or press **Enter**, to finish. Right-click or press **Esc** to cancel.

While you move the mouse, you can:

| Press | To |
|---|---|
| **X**, **Y** or **Z** | Lock to that axis. Press again to use the object's own axis, a third time to unlock. |
| **Shift+X**, **Shift+Y** or **Shift+Z** | Keep that axis fixed and move along the other two |
| A number, then **Enter** | Set an exact value (units, degrees or a scale factor). Type **-** to flip the sign. |
| **Tab** | Type the next axis's value |
| Hold **Ctrl** | Snap to steps (see [Snap to the grid](#snap-to-the-grid)) |
| Hold **Shift** | Make fine adjustments |

Each move, rotate or scale is one undo step. The same commands are in **Object > Transform**.

### With the gizmo

1. Pick **Move**, **Rotate** or **Scale** in the toolbar.
2. Drag a handle:
   - **Move**: an arrow moves along that axis; a small square moves across that plane.
   - **Rotate**: a ring turns around that axis; the outer ring turns around your view.
   - **Scale**: an axis handle stretches along that axis; the centre scales evenly.

The **Transform Orientation** dropdown in the header sets which way the gizmo points:
**Global** (the world's axes), **Local** (the object's own) or **Normal** (in Edit Mode only).

### Reset a transform

| Key | Resets (also in **Object > Clear**) |
|---|---|
| **Alt+G** | **Location** to 0 |
| **Alt+R** | **Rotation** to 0 |
| **Alt+S** | **Scale** to 1 |

**Shift+D** duplicates the selection and starts moving the copy.

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
Sources: `editor/viewport/editor_camera.h`, `editor/viewport/gizmo.h`, `editor/viewport/modal_transform.h`, `editor/app/ui/viewport_chrome.inl`, `editor/app/trackpad.h`, `editor/app/editor_app.h`, `editor/app/ui/topbar.inl`, `editor/app/ui/properties.inl`, `editor/app/ui/statusbar.inl`
Previous: [Scenes and objects](scenes-and-objects.md) | Next: [Modeling](modeling.md)
