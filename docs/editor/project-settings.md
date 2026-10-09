[Editor manual](README.md) > Project settings

Project settings control how your game looks and runs: render quality, sky and fog, physics,
the game window and the scene the game starts with. They live in two places:

- **Edit > Project Settings...** holds the **project's** values (saved in `assets/config.yaml`).
  Every scene starts from them.
- The **Render**, **World** and **Scene** tabs of Properties hold the **open scene's**
  overrides of those values. Anything you change there applies to that scene only.

This page also shows how to package the project for shipping.

![The Render tab in Properties, organized by feature](../images/editor/render_settings.jpg)

*The scene's **Render** tab. At the top are **Project Settings...** and **Rebuild Renderer**,
then how many settings this scene overrides, then the setting groups.*

## Change the project's settings

1. Choose **Edit > Project Settings...** (or **Render > Project Settings...**, or click
   **Project Settings...** at the top of the **Render** tab).
2. Pick a category on the left: **General**, **Render Features**, **Stylize**, **Debug**,
   **Output** or **Physics**. Typing in **Search settings** searches every render category.
3. Change settings as on any tab. Most changes show in the viewport at once.
4. Click **Save config.yaml** at the bottom. The button shows a `*` while there are unsaved
   changes.

A scene that overrides a setting keeps its own value. Every other scene picks up the new
project value.

## Change a setting for one scene

1. Open the scene.
2. In Properties, click the **Render**, **World** or **Scene** tab icon. Hover over an icon to
   see its name. You can also choose **Render > Scene Render Settings** or
   **Render > World Settings**.
3. Change the setting. Every row shows the value the scene renders with, which is the project's
   value until you change it.

Click a group's header to fold or unfold it. Drag a number left or right, or click it and
type.

An overridden setting is easy to spot:

- Its row is tinted, with a coloured bar at its left edge. This applies to a feature's on/off
  checkbox in its section header too.
- A section that contains overrides shows **(N overridden)** in its header and the bar at its
  left edge, even while it is folded. Category headings show the same count.
- A dot appears on the **Render** tab icon (and on the **Scene** tab icon for physics) while the
  scene has overrides there.
- Hovering an overridden row shows both values: the scene's and the project's.

To manage an override, right-click the row or the section header:

| Item | What it does |
|---|---|
| **Revert to Project Setting** | Drops this scene's override, so the scene uses the project's value again |
| **Apply to Project Settings** | Makes this scene's value the project's value, for every scene |

Setting a value back to the project's value, by hand or by ticking a feature back, also removes
the override and its tint.

In **Project Settings**, right-clicking a row offers **Reset to Default** instead. It clears the
project's value, so the built-in default applies.

To drop every override at once, click **Revert All Scene Overrides** under the summary at the
top of the tab. One **Ctrl+Z** brings them all back.

> [!NOTE]
> **Texel AA** is fixed when the editor starts, so it is project-only. A scene shows it greyed
> out with **(project)**. Change it in Project Settings, then choose
> **Render > Restart Editor Engine...**.

## Save your settings

- Overrides are saved with the scene, when you save the scene.
- Project values are saved when you click **Save config.yaml** in Project Settings.
- **File > Save** (**Ctrl+S**) saves both.

To undo a settings change, point at Properties and press **Ctrl+Z**.

## Settings that rebuild the renderer

Some settings, such as turning a feature on or off, the render size and shadow map sizes, are
built into the renderer. Changing one rebuilds the renderer in place, about a tenth of a second
after you stop changing it. The window stays open and the scene, your view and any unsaved
changes stay as they were. The setting's tooltip says **Changing it rebuilds the renderer**.

A game does the same when it loads a scene that overrides one of these settings.

To force a rebuild, click **Rebuild Renderer** at the top of the **Render** tab or in Project
Settings, or choose **Render > Rebuild Renderer**. **Render > Restart Editor Engine...**
recreates the whole engine and window, which is only needed for **Texel AA**.

## Render tab

The Render tab is organized by feature. Every section starts closed, so the tab opens as a list:

| Heading | Sections |
|---|---|
| **General** | **Resolution & Detail** (render size, upscaling, mesh detail), **Lighting & Sky** (exposure, shading style, sky colours), **Anti-Aliasing** (the method, and only that method's tuning) |
| **Render Features** | **Shadows**, **Contact Shadows**, **Ambient Occlusion**, **Reflections**, **Global Illumination**, **Transparency & Refraction**, **Fog**, **Volumetrics**, **SDF Raymarching**, **Water**, **Bloom**, **Auto Exposure**, **Depth of Field**, **Tilt Shift**, **Color Grading**, **UI Canvases** |
| **Stylize** | **Outline**, **Palette**, **Dither**, **Pixel Stability** |
| **Debug** | **Debug View**: one part of the image on its own, such as normals or shadows |

- **Turn a feature on or off** with the checkbox in its section header. Click the name to open
  the section without changing it. An open section that is off says so at the top.
- **Everything about a feature is inside its section**: its quality tier, its tuning, and the
  sizes that need a restart. Sub-headings group the rows (for Shadows: Sun Cascades, Softness,
  Contact Hardening, Bias, Point & Spot Lights, Performance). Rows that belong to one mode show
  only in that mode, such as FXAA's rows only while **Method** is **fxaa**.
- **Hover any row** for what it does and its key in config.yaml.
- **Search settings** at the top narrows the tab to matching settings as you type. It matches
  names, keys and descriptions.
- Rows marked **Set by ... Quality** in their description follow the section's **Quality** tier
  until you change them. A value you set yourself always wins over the tier.
- **Unrecognized** appears as a category in Project Settings only when config.yaml's `render:`
  holds a key the editor doesn't know, such as a typo.

The editor's viewport always fills its area, so **Resolution & Detail** changes the running
game's image, not the editor's view. In the same way, the viewport's shading buttons decide
what the editor shows, not **Debug view** (see [Viewport](viewport.md#change-how-the-scene-is-drawn)).

## World tab

![The World tab with the Lighting & Sky group open](../images/editor/world_settings.jpg)

The **World** tab holds the sky, ambient light and fog.

- **Lighting & Sky**: **Exposure**, **Ambient intensity**, **Sky intensity**, and the three sky
  colours **Sky zenith** (overhead), **Sky horizon** and **Sky ground**. Click a colour swatch
  to pick a colour, or edit its red, green and blue numbers.
- **Fog**: the fog's mode, density, colour, height, sun glow and distance limits.

The fog is height fog: it is thickest at and below **Height Base** and thins out over
**Height Falloff** metres above it, so valleys fill with mist while hilltops and the sky
overhead stay clear. **Start Distance** keeps the fog off nearby objects, and **Cutoff Distance**
stops it from thickening past a point. Water, glass and particles are fogged at their own
distance. Under water, only what you see above the surface is fogged.

**Fog mode** has two choices:

| Mode | How the fog builds up |
|---|---|
| **Linear** | Evenly, from none at **Fog linear start** to full at **Fog linear end**. **Fog density** is ignored. |
| **Exponential** | Builds with distance the way real haze does, and thins with height. This is the default. |

To see fog at all, turn on **Fog** in **Render Features** on the **Render** tab.

### Weather & Time of Day

The first section of the World tab gives the open scene a clock and changing weather. It is
saved in the scene, so each scene can have its own.

1. Tick the checkbox in the **Weather & Time of Day** header. The first time, it fills in a day
   and night sky based on how the scene looks now, and a set of standard weather conditions:
   clear, cloudy, overcast, fog, rain, storm, snow, blizzard and sandstorm.
2. Set the clock under **Clock**: **Start Time**, **Day Length (min)** (real minutes per game
   day; 0 stops time) and how bright the sun and moon are.
3. Choose how the weather changes under **Schedule**: **fixed** stays on the **Start
   Condition**, **random** moves on when a condition's time is up, and **cycle** goes down the
   list.
4. Under **Conditions**, select a condition to edit what it does: how cloudy and dark the sky
   is, the fog, the wind, lightning, and the **Effects** it adds (rain, snow, mist). The buttons
   under the list add, duplicate, remove and reorder conditions, or reset them to the standard
   set.

**Preview** shows the time and weather right now, and lets you try things without changing the
scene. Drag **Time** to see another time of day. Pick a **Condition** to watch the weather blend
into it. To skip waiting for the blend, set **Transition Speed** to 4x, 16x or Instant, or click
**Finish Transition**. Tick **Run Clock & Schedule** to let time pass while you edit. Use **Full Render**
shading in the viewport to see it.

While the weather is on, it controls the sky colours, ambient light and fog. Those settings show
**Set by Weather** with a lock, on this tab and on the Render tab, and come back when you switch
the weather off. The scene's sun shows **Driven by Weather** in its components.

Rain splashes, and snow settles, only on objects you choose. Select the ground (or a street)
and **Add Component > Effects > WeatherSurface**. Rain still stops on every other surface,
such as a roof or an awning, but lands there without splashing.

The rain, snow and mist the weather creates appear in the Hierarchy under **Runtime**, greyed
out with a lock. You can select them to look at them, but not change them. They are never saved.

## Output settings

**Edit > Project Settings... > Output** holds:

- **Window**: the game window's **Title**, **Width**, **Height** and **Vsync**.
- **Jobs**: **Worker threads** (how many CPU threads the game uses; `0` uses them all) and
  **Parallel threshold**.
- **Output**: a screenshot the game can save when it closes. Turn on **Save on exit** and set
  **Filepath** to where it goes. **Save low res** saves the image at the render size instead of
  the window size.

Window and jobs settings apply the next time the game starts. The **Output** tab in Properties
has **Package Project...** and a shortcut to these settings.

## Scene tab

Open it from the tab icon, or click the scene's name at the top of the Hierarchy.

- **Scene**: the scene's **Name**, and **Auto Transform** (on by default), which makes sure
  every object has a position, rotation and scale when the scene loads. Below them is where the
  scene is saved.
- **Startup**: **Default Scene** is the scene the game opens first, and the one the editor
  opens when it starts. Click **Use This Scene at Startup** to pick the open scene.
- **Physics**: **Gravity** (in metres per second squared, straight down is negative Z) and
  **Fixed timestep** (how often physics updates, in seconds). Changes here override the
  project's physics for this scene (tinted, like render overrides). They apply when the scene
  starts or plays. The project's values are in **Project Settings > Physics**.

## Build the game

**Build** turns your project into a game that runs on its own on any computer with the same
operating system: a `.app` on macOS, or a folder on Linux. Players don't need toyengine,
Homebrew or the Vulkan SDK.

1. Choose **Build > Build Settings...** once. Set the **Product name**, **Version** and
   **Bundle identifier** (for example `com.yourstudio.yourgame`), and optionally an **Icon**
   (a square PNG). Click **Save**. The settings go in `build_settings.yaml` beside your project
   file, never into the game.
2. Choose **Build > Build (Development)** to make a version you can test and share with
   testers. It builds quickly and keeps debug options available.
3. Choose **Build > Build (Shipping)** for the version players get. It recompiles the game
   optimized, encodes your assets and turns debug options off.

A **Build Project** window shows progress. When the build finishes, click **Run** to start
it, or **Show in Finder** to find it. Builds go in `build/dist/development` or
`build/dist/shipping` inside your project. **Build and Run** does a Development build and
starts it.

### Signing on macOS

A Development build is signed ad-hoc: it runs on your Mac. For other people's Macs, a
Shipping build must be signed with a **Developer ID** and notarized by Apple:

1. Install a *Developer ID Application* certificate from your Apple Developer account.
2. Run `xcrun notarytool store-credentials toy-notary` once in Terminal, and follow its
   prompts.
3. In **Build Settings**, click **Detect identities**, pick yours (or leave **auto**), and set
   **Notary profile** to `toy-notary`.

The Shipping build then signs, notarizes and staples the app, and makes a `.zip` to hand out
(and a `.dmg` if you turn that on). Without an identity it still builds, signed ad-hoc, and
the Console warns that other Macs will block it.

> [!NOTE]
> A game writes its log and crash reports to `~/Library/Logs/<bundle id>/` on macOS
> (`~/.local/state/<name>/logs` on Linux). Players' saved settings, such as volume, go in
> `~/Library/Application Support/<bundle id>/`.

## Package the assets only

![The Package Project window with the Output folder and Keep .yaml copies options](../images/editor/package_dialog.jpg)

Packaging copies only your project's assets, with its scene and asset files encoded so they
can't be read as plain text. There is no program in it; use [Build](#build-the-game) to make a
game you can hand out.

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

Sources: `editor/schema/settings_schema.h`, `editor/app/ui/properties.inl`, `editor/app/ui/project_settings.inl`, `editor/app/ui/topbar.inl`, `editor/app/editor_app.h`, `editor/core/scene_document.h`, `editor/build/packager.h`, `editor/build/build_pipeline.h`, `editor/build/build_settings.h`, `editor/app/ui/build.inl`, `editor/app/project.h`

Previous: [UI designer](ui-designer.md) | Next: [Shortcuts](shortcuts.md)
