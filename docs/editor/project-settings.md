[Editor manual](README.md) > Project settings

Project settings control how your game looks and runs: render quality, sky and fog, physics,
the game window and the scene the game starts with. You change them on four tabs of
Properties, **Render**, **World**, **Output** and **Scene**, which appear while a scene is
open. This page also shows how to package the project for shipping.

![The Render tab in Properties, showing the Viewport & Resolution and Features groups](../images/editor/render_settings.jpg)

*The **Render** tab. At the top are **Save config.yaml** and **Restart Renderer**, then a short
reminder of how edits are stored, then the setting groups. A `*` after a setting's name means
it takes effect only after a restart.*

## Open the settings tabs

1. Open a scene.
2. In Properties, click the **Render**, **Output**, **Scene** or **World** tab icon (the second
   to fifth icons). Hover over an icon to see its name.

You can also choose **Render > Render Settings** or **Render > World Settings** in the top bar.

Click a group's header to fold or unfold it. Drag a number left or right, or click it and
type. Most changes show in the viewport at once.

## Change a setting for one scene or for the whole project

Settings belong to the project, so every scene uses the same values. A scene can **override**
some of them for itself, for example to give one level thicker fog.

> [!NOTE]
> While a scene is open, a change on the **Render** or **World** tab, or in **Physics** on the
> **Scene** tab, applies only to that scene. The row turns tinted, with a coloured bar at its
> left edge, to show it is overridden. Settings marked `*` are the exception: they always apply
> to the whole project.

To manage an override, right-click the row:

| Item | What it does |
|---|---|
| **Revert to Project Setting** | Drops this scene's override, so the scene uses the project's value again |
| **Apply to Project Settings** | Makes this scene's value the project's value, for every scene |
| **Reset to Default** | Clears the project's value, so the built-in default applies |

## Save your settings

- Overrides are saved with the scene, when you save the scene.
- Project-wide values are saved when you click **Save config.yaml** on the **Render** or
  **Output** tab. The button shows a `*` while there are unsaved changes.
- **File > Save** (**Ctrl+S**) saves both.

To undo a settings change, point at Properties and press **Ctrl+Z**.

## Apply settings that need a restart

Settings with a `*` after their name can't change while the renderer is running. When you
change one, the Console says the change applies on restart. To apply it:

1. Click **Restart Renderer** at the top of the **Render** tab, or choose
   **Render > Restart Renderer**.

The renderer starts again with your changes. The open scene, your view and any unsaved changes
stay as they were.

## Render tab

| Group | What it controls |
|---|---|
| **Viewport & Resolution** | The game's render size and how it is scaled to the window, and the anti-aliasing (smoothing of jagged edges) |
| **Quality Tiers** | One **low** to **ultra** level each for shadows, ambient occlusion, reflections, indirect light, depth of field, volumetric light, SDF shapes and water |
| **Features** | Switches for each effect: shadows, ambient occlusion, reflections, transparency, refraction, bloom, fog, volumetric light, depth of field, tilt-shift, auto exposure, colour grading, outlines and more |
| **Shadows** | Shadow distance, number of cascades, bias, softness and contact shadows |
| **Bloom & Exposure** | How bright things must be to glow, how strong and wide the glow is, and exposure compensation |
| **Stylize** | Outline thickness and colour, edge detection, dithering, a colour palette image and a colour grading image |
| **Debug** | **Debug view**, which shows one part of the image on its own, such as normals or shadows |
| **Other Render Keys** | Any other render setting in the project, as a plain field |

A quality tier sets several related values at once. A setting you change yourself always wins
over the tier.

The editor's viewport always fills its area, so **Viewport & Resolution** changes the running
game's image, not the editor's view. In the same way, the viewport's shading buttons decide
what the editor shows, not **Debug view** (see [Viewport](viewport.md#change-how-the-scene-is-drawn)).

## World tab

![The World tab with the Lighting & Sky group open](../images/editor/world_settings.jpg)

The **World** tab holds the sky, ambient light and fog.

- **Lighting & Sky**: **Exposure**, **Ambient intensity**, **Sky intensity**, and the three sky
  colours **Sky zenith** (overhead), **Sky horizon** and **Sky ground**. Click a colour swatch
  to pick a colour, or edit its red, green and blue numbers.
- **Fog**: the fog's mode, density, colour, height and distance limits.

**Fog mode** has three choices:

| Mode | How the fog builds up |
|---|---|
| **Linear** | Evenly, from none at **Fog linear start** to full at **Fog linear end**. **Fog density** is ignored. |
| **Exponential** | Starts right away and fades out softly |
| **Exponential Squared** | Clear up close, then closes in quickly. This is the default. |

To see fog at all, turn on **Fog enabled** in **Features** on the **Render** tab and restart
the renderer.

## Output tab

The **Output** tab has **Save config.yaml** and **Package Project...** at the top, then:

- **Window**: the game window's **Title**, **Width**, **Height** and **Vsync**.
- **Jobs**: **Worker threads** (how many CPU threads the game uses; `0` uses them all) and
  **Parallel threshold**.
- **Output**: a screenshot the game can save when it closes. Turn on **Save on exit** and set
  **Filepath** to where it goes. **Save low res** saves the image at the render size instead of
  the window size.

Window and jobs settings apply the next time the game starts.

## Scene tab

Open it from the tab icon, or click the scene's name at the top of the Hierarchy.

- **Scene**: the scene's **Name**, and **Auto Transform** (on by default), which makes sure
  every object has a position, rotation and scale when the scene loads. Below them is where the
  scene is saved.
- **Startup**: **Default Scene** is the scene the game opens first, and the one the editor
  opens when it starts. Click **Use This Scene at Startup** to pick the open scene.
- **Physics**: **Gravity** (in metres per second squared, straight down is negative Z) and
  **Fixed timestep** (how often physics updates, in seconds). A scene's own values apply when
  it starts or plays.

## Package the project

![The Package Project window with the Output folder and Keep .yaml copies options](../images/editor/package_dialog.jpg)

Packaging makes a copy of your project that is ready to ship, with its scene and asset files
encoded so they can't be read as plain text.

1. Choose **File > Package Project (.caml)...**, or click **Package Project...** on the
   **Output** tab.
2. Check **Output folder**. It starts as the `build/package` folder inside your project.
3. Turn on **Keep .yaml copies** only if you also want readable copies of the files, for
   example to track down a problem.
4. Click **Package**.

The Console reports how many files were packaged, or lists what went wrong. The output folder
holds a copy of your assets and, if the game program has been built, the game itself.

> [!TIP]
> You don't need to change anything in your scenes before packaging. The packaged game finds
> the encoded files on its own.

---

Sources: `editor/schema/settings_schema.h`, `editor/app/ui/properties.inl`, `editor/app/ui/topbar.inl`, `editor/app/editor_app.h`, `editor/core/scene_document.h`, `editor/build/packager.h`, `editor/app/project.h`

Previous: [UI designer](ui-designer.md) | Next: [Shortcuts](shortcuts.md)
