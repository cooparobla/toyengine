# toyengine editor manual

The toyengine editor is where you build your game's content: the scenes players walk around
in, the objects that fill them, their shapes, materials and animations, and the menus and HUD
drawn on screen. What you see in the editor's viewport is drawn by the game's own renderer, so
it looks the way it will in the game.

![The editor with the water_demo scene open: the asset list on the left, the 3D viewport in
the middle, the Hierarchy and Properties on the right](../images/editor/overview.jpg)

If you have used Blender, the viewport, its keys and its modes will feel familiar. If you have
used Unity, so will the way objects are built from components.

## Open the editor

Start the editor from a terminal in the toyengine folder:

```sh
./build/toyengine_editor                     # reopens the project you used last
./build/toyengine_editor path/to/my_game     # opens a specific project
./build/toyengine_editor --new-project ~/my_game   # creates a new project and opens it
```

A **project** is a folder holding everything your game uses. Once the editor is open you can
switch projects without restarting it:

- **File > New Project...** creates a project with a starter scene (a camera, a sun, a ground
  plane and a cube). Pick a folder and click **Create**.
- **File > Open Project...** opens another project folder.
- **File > Recent Projects** lists the last 8 projects you opened.

If you have unsaved changes, the editor asks whether to save them first.

## What you can make

The tabs at the top of the left-hand panel list your project's assets, one tab per kind:

| Tab | What it holds | Where to learn more |
|---|---|---|
| **Scenes** | Levels and other places, filled with objects | [Scenes and objects](scenes-and-objects.md) |
| **Objects** | Reusable objects (a crate, an enemy) you place in many scenes | [Scenes and objects](scenes-and-objects.md) |
| **Meshes** | 3D shapes you model, sculpt and paint | [Modeling](modeling.md), [Sculpt and paint](sculpt-and-paint.md) |
| **Materials** | How surfaces look: colour, shine, glow, glass | [Materials and textures](materials-and-textures.md) |
| **Textures** | Images used by materials | [Materials and textures](materials-and-textures.md) |
| **UI** | Menus, HUDs and other screens | [UI designer](ui-designer.md) |
| **Themes** | The look shared by your game's UI | [UI designer](ui-designer.md) |

Click an asset to open it. One asset is open at a time.

## Contents

1. [The interface](interface.md): the parts of the window, the menus, and changing the
   editor's colours.
2. [Scenes and objects](scenes-and-objects.md): adding, arranging and setting up objects,
   reusable objects, and playing your scene.
3. [The 3D viewport](viewport.md): looking around, selecting, and moving, rotating and scaling
   things.
4. [Modeling](modeling.md): shaping meshes in Edit Mode.
5. [Sculpt and paint](sculpt-and-paint.md): sculpting with brushes and painting colours and
   weights.
6. [Materials and textures](materials-and-textures.md): making surfaces look right.
7. [Animation](animation.md): recording movement on the Timeline.
8. [UI designer](ui-designer.md): building menus and HUDs.
9. [Project settings](project-settings.md): rendering, lighting and packaging the game.
10. [Keyboard shortcuts](shortcuts.md): every shortcut in one place.

## Your first steps

1. Read [The interface](interface.md) to learn the parts of the window.
2. Learn to move around and select things in [The 3D viewport](viewport.md).
3. Build something in [Scenes and objects](scenes-and-objects.md): add a few objects, give
   them materials, and press **Play** to try it.

> [!TIP]
> Press **Ctrl+S** to save everything you have changed. On macOS, **Cmd+S** works too, as do
> **Cmd** versions of the other main commands (Undo, Redo, New Scene, Open Scene, Quit).

---
Sources: `editor/editor.cpp`, `editor/app/project.h`, `editor/app/ui/assets.inl`,
`editor/app/ui/topbar.inl`
Next: [The interface](interface.md)
