[Editor manual](README.md) > Keyboard shortcuts

Every keyboard and mouse shortcut in the editor, grouped by where you are. The keys follow
Blender, so if you know Blender most of them already work. Each group links to the page that
explains it.

![The Controls window open over the viewport, listing the main shortcuts by
group](../images/editor/controls_modal.jpg)

*For a short list inside the editor, choose **Help > Controls...**.*

Most keys act on the area under the mouse pointer: viewport keys need the pointer over the
viewport, Timeline keys over the Timeline, and so on. Keys are ignored while you type in a text
field or while a menu is open.

> [!NOTE]
> **On a Mac:** Save, Undo, Redo, New Scene, Open Scene and Quit accept **Cmd** as well as
> **Ctrl**. Every other **Ctrl** shortcut in this list needs the **Ctrl** key itself. When you
> hold **Ctrl** during a mouse drag (zoom, box select, snapping, brushes), **Cmd** works too.

## Anywhere

See [Interface](interface.md#use-the-menus) and [Scenes and objects](scenes-and-objects.md#undo-a-mistake).

| Key | Action |
|---|---|
| **Ctrl+S** | Save everything with unsaved changes |
| **Shift+Ctrl+S** | **Save Scene As...** |
| **Ctrl+Z** | Undo |
| **Shift+Ctrl+Z** or **Ctrl+Y** | Redo |
| **Ctrl+N** | New scene |
| **Ctrl+O** | Open a scene |
| **Shift+Ctrl+B** | **Build > Refresh**: rebuild the project's code |
| **Ctrl+Q** | Quit |
| **F5** | Play / stop the game (in the UI designer: switch Design / Interact) |
| **Esc** | While playing: take the mouse and keyboard back from the game |
| **F2** | Rename the active object |

## Hierarchy

With the pointer over the Hierarchy. See [Scenes and objects](scenes-and-objects.md#select-objects).

| Input | Action |
|---|---|
| Click | Select |
| **Shift**-click or **Cmd**-click (**Ctrl**-click on Windows / Linux) | Add to / remove from the selection |
| Double-click or **F2** | Rename |
| Drag a row onto another | Make it a child of that object |
| Right-click | Context menu |
| **X** or **Delete** | Delete |
| **A** / **Alt+A** | Select all / none |
| **H** / **Alt+H** | Hide / show hidden |
| **F**, **.** or **Numpad .** | Frame the selection in the viewport |

## Viewport: moving the view

Works in every mode. See [Move around the view](viewport.md#move-around-the-view) and
[Look from the front, top or side](viewport.md#look-from-the-front-top-or-side).

| Input | Action |
|---|---|
| Middle-drag | Orbit |
| **Shift** + middle-drag | Pan |
| Scroll wheel, or **Ctrl** + middle-drag | Zoom |
| **Alt** + left-drag | Orbit without a middle button (**Shift** pans, **Ctrl** zooms) |
| **Shift** + wheel / **Ctrl** + wheel | Pan up-down / left-right |
| Two-finger swipe | Orbit (**Shift** pans, **Ctrl** zooms) |
| Pinch | Zoom |
| **Numpad 2** / **4** / **6** / **8** | Orbit down / left / right / up |
| **Numpad +** / **Numpad -** (or **=** / **-**) | Zoom in / out |
| **Numpad 1** / **3** / **7** | Front / right / top view (add **Ctrl** for the opposite) |
| **Numpad 5** | Perspective / orthographic |
| **Numpad 0** | Look through the scene camera |
| **Ctrl+Alt+Numpad 0** | Move the scene camera to the current view |
| **.** or **Numpad .** | Frame the selection |
| **F** | Frame the selection (Object Mode) |
| **Home** | Frame everything |

## Viewport: display and tools

Works in every mode. See [The 3D viewport](viewport.md#change-how-the-scene-is-drawn).

| Key | Action |
|---|---|
| **`** (backtick) | View menu |
| **Z** | Shading menu |
| **Shift+Z** | Wireframe / Solid |
| **Alt+Z** | X-Ray |
| **T** / **N** | Toolbar / sidebar |
| **B** | Select Box tool |
| **Shift+Space** | Tool menu |
| **Ctrl+Space** | Maximize the viewport |
| **Ctrl+Tab** | Mode menu |
| **Shift+S** | Snap menu |
| **Shift+C** | 3D cursor to the world centre, frame everything |
| **Shift** + right-click | Place the 3D cursor |

## Object Mode

Pointer over the viewport. See [Select objects](viewport.md#select-objects) and
[Scenes and objects](scenes-and-objects.md#rename-copy-or-delete-an-object).

| Input | Action |
|---|---|
| Click | Select |
| **Shift**-click | Add to / remove from the selection |
| Drag | Box select (**Shift** adds, **Ctrl** removes) |
| Right-click | Context menu |
| **A** / **Alt+A** | Select all / none |
| **Ctrl+I** | Invert the selection |
| **G** / **R** / **S** | Move / rotate / scale |
| **Alt+G** / **Alt+R** / **Alt+S** | Reset location / rotation / scale |
| **Shift+D** | Duplicate |
| **X** | Delete (asks first) |
| **Delete** | Delete |
| **Shift+A** | Add menu |
| **H** / **Shift+H** / **Alt+H** | Hide selected / hide others / show all |
| **Ctrl+P** | Parent to the active object |
| **Alt+P** | Clear parent |
| **Tab** | Edit Mode |
| **I** | Insert animation keys (see [Animation](animation.md#insert-keys-by-hand)) |

## While moving, rotating or scaling

After **G**, **R** or **S**, in Object or Edit Mode. See
[Move, rotate and scale](viewport.md#move-rotate-and-scale).

| Input | Action |
|---|---|
| **X** / **Y** / **Z** | Lock to an axis (press again for local, again to unlock) |
| **Shift+X** / **Shift+Y** / **Shift+Z** | Lock to a plane |
| Hold the middle button | Lock to the axis you move towards |
| Digits, **.**, **-**, **Backspace** | Type an exact value |
| **Tab** | Next axis value |
| Hold **Ctrl** | Snap |
| Hold **Shift** | Fine adjustment |
| **G** (while moving in Edit Mode) | Edge Slide |
| Click or **Enter** | Confirm |
| Right-click or **Esc** | Cancel |

## Edit Mode

Pointer over the viewport while editing a mesh. See [Modeling](modeling.md#start-editing-a-mesh).

| Input | Action |
|---|---|
| **1** / **2** / **3** | Vertex / edge / face select |
| Click / **Shift**-click | Select / toggle |
| Drag | Box select (**Shift** adds, **Ctrl** removes) |
| **Alt**-click | Select an edge loop (**Shift+Alt** adds) |
| **Ctrl+Alt**-click | Select an edge ring (**Shift+Ctrl+Alt** adds) |
| **A** / **Alt+A** | Select all / none |
| **Ctrl+I** | Invert the selection |
| **L** / **Ctrl+L** | Select linked under the pointer / to the selection |
| **G** / **R** / **S** | Move / rotate / scale |
| **G** then **G** | Edge Slide |
| **E** | Extrude |
| **I** | Inset |
| **Ctrl+B** | Bevel |
| **F** | Make a face |
| **Shift+D** | Duplicate |
| **Ctrl+R** | Loop cut |
| **Ctrl+T** | Triangulate |
| **Alt+J** | Tris to quads |
| **Ctrl+E** | Bridge edge loops |
| **Shift+A** | Add a primitive |
| **W** or right-click | Context menu |
| **X** or **Delete** | Delete menu |
| **M** | Merge menu |
| **Ctrl+M** | Mirror menu |
| **U** | UV menu |
| **Alt+N** | Normals menu |
| **Tab** | Back to Object Mode |

During a loop cut (**Ctrl+R**):

| Input | Action |
|---|---|
| Scroll wheel, **+** / **-**, **Page Up** / **Page Down** | More / fewer cuts |
| **1** to **9** | Number of cuts |
| Click | Cut, then slide |
| Right-click or **Esc** | Cancel |

## Sculpt Mode

Pointer over the viewport. Moving the view and the display keys still work. See
[Sculpt and paint](sculpt-and-paint.md#sculpt).

| Input | Action |
|---|---|
| Drag | Sculpt |
| **Ctrl** + drag | Sculpt inverted |
| **Shift** + drag | Smooth |
| **X** / **S** / **I** / **G** / **Shift+T** | Draw / Smooth / Inflate / Grab / Flatten brush |
| **F** / **Shift+F** | Resize the brush / change its strength (click to confirm) |
| Right-click | Mode menu |
| **Tab** | Back to Object Mode |

## Vertex Paint and Weight Paint

Pointer over the viewport. Moving the view and the display keys still work. See
[Paint vertex colours](sculpt-and-paint.md#paint-vertex-colours) and
[Paint weights](sculpt-and-paint.md#paint-weights).

| Input | Action |
|---|---|
| Drag | Paint |
| **Ctrl** + drag | Paint the second colour / subtract weight |
| **Shift** + drag | Blur |
| **F** / **Shift+F** | Resize the brush / change its strength |
| **S** | Pick up the colour or weight under the pointer |
| **X** | Swap the two colours (Vertex Paint) |
| **Shift+K** | Fill the whole mesh |
| Right-click | Mode menu |
| **Tab** | Back to Object Mode |

## Timeline

Pointer over the Timeline. See [Animation](animation.md#edit-keys-on-the-dope-sheet).

| Input | Action |
|---|---|
| **Space** | Play / pause |
| **I** | Insert keys at the playhead |
| **X** or **Delete** | Delete the selected keys |
| **A** | Select all keys |
| **Left** / **Right** | Previous / next frame |
| **Down** / **Up** | Previous / next key |
| **Home** | Go to the start |
| Click / **Shift**-click a key | Select / toggle |
| Drag a key | Move the selected keys |
| Right-click | Context menu |
| Click or drag the ruler | Scrub (hold **Shift** to skip frame snapping) |
| Scroll wheel | Zoom in time |
| **Shift** + wheel, or middle-drag | Pan in time |

## UI designer

With a UI asset open and the pointer over the preview. See [Move around the canvas](ui-designer.md#move-around-the-canvas) and
[Select elements](ui-designer.md#select-elements).

| Input | Action |
|---|---|
| Scroll wheel or pinch | Zoom |
| Middle-drag, two-finger swipe, or **Space** + drag | Pan |
| Click / **Shift**-click | Select / add to the selection |
| **Alt**-click | Pick the next element under the pointer |
| Drag an element | Move it |
| Drag on empty space | Box select |
| Right-click | Context menu |
| **Shift+A** | Add a widget |
| **X**, **Delete** or **Backspace** | Delete |
| **Shift+D** or **Ctrl+D** (**Cmd+D** on a Mac) | Duplicate |
| **A** / **Alt+A** | Select all / none |
| **H** / **Alt+H** | Hide / show hidden |
| Arrow keys | Nudge 1 pixel (**Shift**: 10) |
| **]** / **[** | Draw lower / higher among siblings |
| **F2** | Rename |
| **Home** | Fit the preview |
| **F**, **.** or **Numpad .** | Frame the selection |
| **Tab** or **F5** | Switch to Interact (**Esc** also returns to Design) |

While dragging a handle: **Shift** keeps the shape (and snaps rotation to 15 degrees), **Alt**
resizes from the centre, and **Ctrl** turns snapping off.

## File browser

The Open / Save / folder picker (**Open Scene...**, **Save Scene As...**, **Open Project...**).
It works like the macOS Finder panels. **Back**, **Forward** and **Enclosing Folder** buttons sit
next to a clickable path bar and a search field. The sidebar lists **Favorites** (this project and
its `assets/` folder first), **Recent** folders and disks. The file list can be sorted by
**Name**, **Date Modified**, **Size** and **Kind**. Files the **Format** menu doesn't accept are
greyed out. On Windows / Linux, use **Ctrl** wherever **Cmd** appears.

| Input | Action |
|---|---|
| **Up** / **Down**, **Home** / **End** | Select |
| Type a name | Select the first match |
| Double-click, **Enter** or **Cmd+Down** | Open the folder or file (Save: save) |
| **Cmd+Up** | Enclosing folder |
| **Cmd+[** / **Cmd+]** | Back / forward |
| **/**, **~** or **Shift+Cmd+G** | Type a path to go to |
| **Cmd+F** | Search this folder |
| **Shift+Cmd+.** | Show / hide hidden files |
| **Shift+Cmd+H** / **Shift+Cmd+D** | Home / Desktop |
| **Esc** | Cancel (in the search field: clear it first) |

---

Sources: `editor/app/editor_app.h`, `editor/app/ui/topbar.inl`, `editor/app/ui/viewport_chrome.inl`, `editor/app/ui/outliner.inl`, `editor/app/ui/mesh_tools.inl`, `editor/app/ui/sculpt.inl`, `editor/app/ui/paint.inl`, `editor/app/ui/timeline.inl`, `editor/app/ui/ui_canvas.inl`, `editor/app/ui/statusbar.inl`, `libs/uicoopa/uicoopa/immediate/imm_file_dialog.h`, `editor/viewport/modal_transform.h`, `editor/viewport/rect_gizmo.h`, `editor/app/trackpad.h`, `libs/uicoopa/uicoopa/immediate/imm.h`
Previous: [Project settings and packaging](project-settings.md) | Next: [Editor manual](README.md)
