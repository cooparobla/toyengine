[Editor manual](README.md) > UI designer

The UI designer is where you build your game's screens: HUDs, menus, dialogs, inventories. You
open a UI from the Asset panel, place widgets on it, and try it out right in the editor. What
you see in the designer is drawn exactly as the game will draw it.

![The UI designer with the hud UI open: widgets palette on the left, the HUD canvas in the
middle with the hotbar selected, and its Element settings on the right](../images/editor/ui_designer.jpg)

*The `hud` UI with its hotbar selected. The handles around the hotbar resize it; the label under
it shows its size.*

The designer reuses the editor's usual areas:

- **Left panel.** Your project's UIs at the top, and the **Widgets** palette below, with a
  **Search widgets** field.
- **Middle.** The *canvas*: a frame the size of a game window, with your UI inside it. The bar
  above it holds the preview controls (see [The designer header](#the-designer-header)).
- **Hierarchy** (top right). Every element of the UI, nested the way they're placed. An
  *element* is one piece of the UI: a panel, a button, a line of text.
- **Properties** (bottom right). Four tabs, shown as icons down the left edge: **Canvas**,
  **Bindings**, **Element** and **Components**. The last two appear when an element is
  selected.

## Create a UI

![The New UI menu open over the Asset panel, listing Blank Canvas, Blank Widget and six
templates](../images/editor/ui_new_menu.jpg)

1. In the Asset panel, click the **UI** tab.
2. Click **+** at the top right of the panel.
3. Choose what to start from.

| Choice | Gives you |
|---|---|
| **Blank Canvas** | An empty full-screen UI, such as a HUD, a menu or a title screen. |
| **Blank Widget** | An empty reusable piece, such as an item slot or a quest entry, to place inside other UIs. See [Reuse one UI inside another](#reuse-one-ui-inside-another). |
| **Dialog box** | An NPC conversation box: portrait, speaker name, a line of dialogue and three answer buttons. |
| **Hud** | Health, stamina and mana bars, a quest tracker, a message log, a hotbar and controller prompts. |
| **Inventory** | Bag and equipment grids, an item details panel and a gold count. |
| **Main menu** | A title screen with New Game, Continue, Settings and Quit buttons. |
| **Pause menu** | Resume, Settings, Save Game and Quit to Menu buttons over a dimmed background. |
| **Settings** | Audio, Video and Gameplay tabs of settings, with Back and Apply buttons. |

The new UI opens straight away. The templates come with ready-made themes and fonts, which are
added to your project the first time you use one.

## Open, save and rename a UI

- **Open:** click the UI in the **UI** tab of the Asset panel.
- **Save:** press **Ctrl+S**.
- **Undo / redo:** **Ctrl+Z** / **Ctrl+Shift+Z**.
- **Rename:** right-click the UI in the list and choose **Rename...**. This works only while
  that UI isn't the one open.

## The designer header

The bar above the canvas, from left to right:

| Control | What it does |
|---|---|
| **Design** / **Interact** (two icons) | **Design** is for editing. **Interact** runs the UI so you can click it. See [Try the UI in Interact mode](#try-the-ui-in-interact-mode). |
| Preview size | The game window size the UI is shown at. See [Check other screen sizes](#check-other-screen-sizes). |
| Zoom | The current zoom. Pick **Fit**, **25%**, **50%**, **100%** or **200%**. |
| **Snapping** (magnet) | Snaps elements to each other's edges and to the grid while you drag. On by default. |
| **Element Outlines** | Draws a faint outline around every element, handy for finding invisible ones. |
| **Safe Area** | Shows the TV and handheld safe frames, so nothing important sits too close to the screen edge. |
| **Grid 1** ... **Grid 16** | The snapping grid, in canvas pixels. |
| **Dark** / **Light** / **3D View** | What shows behind the UI. **3D View** shows your 3D scene behind it, as in the game. |
| **Add** | Opens the **Add Widget** menu. |

## Move around the canvas

| To | Do this |
|---|---|
| Zoom | Scroll the mouse wheel, or pinch on a trackpad. |
| Pan | Drag with the middle mouse button, hold **Space** and drag, or swipe with two fingers. |
| Fit the whole canvas | Press **Home**. |
| Zoom to the selection | Press **F**. |

## Add widgets

1. Select the element the new widget should go inside. Select nothing to add it to the canvas
   itself.
2. Click the widget in the **Widgets** palette on the left.

The widget appears inside the selected element. If the selection is a simple widget, such as a
button, the new widget goes beside it instead.

Other ways to add a widget:

- **Drag** it from the palette onto the canvas. It drops where you let go, inside the element
  under the mouse. Drop it onto a **Hierarchy** row to put it inside that element.
- Press **Shift+A** over the canvas, click **Add** in the header, or right-click the canvas and
  choose **Add Widget**. All three open the same menu, sorted into the palette's groups.

| Group | Widgets |
|---|---|
| Basics | **Panel**, **Text**, **Title**, **Button**, **Image**, **Plain Text**, **Slider**, **Progress Bar** |
| Layout | **Vertical Stack**, **Horizontal Row**, **Grid**, **Scroll View**, **Spacer**, **Empty Rect** |
| HUD | **HUD Corner**, **Health Bar**, **Stamina Bar**, **Mana Bar**, **Hotbar**, **Message Log**, **Button Prompts** |
| Windows & Menus | **Window**, **Dialog**, **Menu List**, **Action Bar**, **Tab View**, **Setting: Slider**, **Setting: Toggle**, **Setting: Dropdown**, **Collapsible**, **Item Grid** |
| Reactors | **Show On Signal**, **Color On Signal**, **Text On Signal** |

Hover a widget in the palette for a short description. Most widgets take their colours, fonts
and sizes from the UI's theme (see [Style the UI with a theme](#style-the-ui-with-a-theme)).
**Plain Text** and **Image** are the exceptions: you style each one yourself. Reactors are not
widgets you see; see [Make buttons do things without code](#make-buttons-do-things-without-code).

## Select elements

| To | Do this |
|---|---|
| Select an element | Click it on the canvas or in the **Hierarchy**. |
| Add to the selection | **Shift**+click. |
| Pick an element hidden under another | **Alt**+click. Click again at the same spot to go one deeper. |
| Select several by dragging | Drag a box from an empty spot. Hold **Alt** to start the box on top of an element. |
| Select all / nothing | **A** / **Alt+A**. |
| Select the parent element | Right-click and choose **Select Parent**. |

Clicking an empty part of the canvas selects the UI's top element.

## Move, resize and rotate an element

Select an element and drag the parts of the box around it:

| Drag | To |
|---|---|
| The middle of the element | Move it. You can also press on any element and drag straight away. |
| A square handle on a corner or edge | Resize it. Hold **Alt** to resize from the centre, or **Shift** on a corner to keep its shape. |
| Just outside a corner | Rotate it. Hold **Shift** to turn in 15-degree steps. |
| The blue ring | Move the *pivot*, the point it rotates and scales around. |

While **Snapping** is on, edges snap to the edges and centres of the parent and nearby elements,
and pink guide lines show what they snapped to. Hold **Ctrl** while dragging to turn snapping
off (or on) for that drag.

For exact values, type them into **Position**, **Size**, **Rotation** and **Scale** in the
**Element** tab.

Keyboard shortcuts over the canvas:

| Key | Action |
|---|---|
| Arrow keys | Nudge 1 pixel (**Shift**: 10 pixels). |
| **]** / **[** | **Bring Forward** / **Send Backward**. |
| **Shift+D** | Duplicate. |
| **X** or **Delete** | Delete. |
| **F2** | Rename. |
| **H** / **Alt+H** | Hide in the editor / show everything again. |

The same commands are on the right-click menu.

### Elements inside a stack, row or grid

An element inside a **Vertical Stack**, **Horizontal Row** or **Grid** is placed by that group,
and shows a small lock at its corner. Dragging it changes its place in the order, with a line
showing where it will land. Resizing it sets how much room it asks the group for.

## Pin an element to the screen edges

Screens come in different sizes. *Anchors* decide where an element stays when the screen changes:
pinned to a corner, centred, or stretched across.

1. Select the element.
2. Open the **Element** tab in Properties.
3. Click the square picture under **Rect Transform** to open the anchor presets.
4. Click a preset, such as the bottom centre for a hotbar or the top-right corner for a minimap.
   - Hold **Shift** while clicking to move the pivot there too.
   - Hold **Alt** while clicking to also move the element onto that spot.

You can also drag the small triangles around the element on the canvas to place the anchors by
hand. Pulling two of them apart makes the element stretch with the screen. When an element
stretches, **Position** and **Size** become **Left** / **Right** or **Top** / **Bottom**
margins.

## Check other screen sizes

1. Open the preview size list in the header.
2. Choose a size, such as **1280 x 720 (16:9)**, **2560 x 1080 (21:9)** or **1080 x 1920
   (portrait)**.

The canvas relays out at that size, the way the game would. For a size that isn't listed, type
it into **Width** and **Height** under **Preview** in the **Canvas** tab. The preview size is
remembered with the UI but doesn't affect the game.

> [!TIP]
> Check a HUD at every screen size you plan to support. Elements that overlap at one size are
> usually missing an anchor.

## Element settings

The **Element** tab of the selected element has:

- **Name**: what the element is called. Programmers use this name to reach buttons, sliders and
  text, so give interactive widgets clear, unique names.
- **Rect Transform**: the anchor presets, **Position**, **Size**, **Anchor Min**,
  **Anchor Max**, **Pivot**, **Rotation** and **Scale**. **Z Order** draws the element above or
  below its neighbours. Turn off **Hittable** to let clicks pass through it.
- **Visibility**: **Show in Editor** hides the element in the designer only. Turn off
  **Visible in Game** to make the element start hidden; something must then show it, such as a
  **Show On Signal** reactor.

The **Components** tab holds the widget's own settings, such as a button's label or a bar's
colour.

## Reuse one UI inside another

Build a piece once, such as an item slot, and place it in several UIs:

1. Create it with **Blank Widget** (see [Create a UI](#create-a-ui)) and save it.
2. Open the UI you want to place it in.
3. Do one of these:
   - Press **Shift+A** and choose it from **UI Asset**.
   - Drag it from the UI list onto the canvas or onto a **Hierarchy** row.
   - Right-click it in the UI list and choose **Place in this UI**.

The **Element** tab of the placed copy says **Instance of ...**. Changes you make to the copy
apply only to that copy. A UI can't be placed inside itself.

## Style the UI with a theme

A *theme* sets the colours, fonts and sizes of every themed widget in a UI. Change the theme and
the whole UI follows. These themes style your game; the editor's own colours are set elsewhere
(see [Interface](interface.md#change-the-editors-colours)).

To pick or make a theme for the open UI:

1. Open the **Canvas** tab in Properties.
2. Under **Theme**, do one of these:
   - Choose a theme from the **Theme** list.
   - Click **New Theme** to make a copy of the current theme and use it.
3. Click **Edit Theme** to change it.

The theme opens in a **Theme** tab in Properties, and your changes show on the canvas as you
make them. In that tab you can:

- Click **Save Theme** (or press **Ctrl+S**) to keep your changes. Nothing is written until you
  do.
- Click **Duplicate** to save a copy, with your changes, under a new name.
- Choose another UI under **Preview on** to see the theme there. If that UI uses a different
  theme, **Use This Theme in ...** switches it over.
- Pick a font for each kind of text under **Fonts**.
- Adjust each kind of widget in its own section: **Panels**, **Text**, **Buttons**, **Sliders**,
  **Tabs**, **HUD** and more.

You can also open a theme from the **Themes** tab of the Asset panel, or click **+** there to
create one.

## Make buttons do things without code

*Reactors* make an element respond when another element does something, with no programming.
For example, clicking an **Options** button can show an options panel.

1. Select the element that should react, such as the options panel.
2. Choose a reactor from the **Reactors** group of the palette or the **Add Widget** menu:

   | Reactor | When the other element signals, this element... |
   |---|---|
   | **Show On Signal** | Is shown (or hidden). |
   | **Color On Signal** | Changes colour. |
   | **Text On Signal** | Changes its text. |

3. Open the **Components** tab and fill in the reactor's settings:
   - **Listen object**: the name of the element to watch, such as the button.
   - **Listen signal**: what it has to do, such as **click** for a button or
     **value_changed** for a slider.
   - **Once**: react only the first time.
   - **Target**: the element to change. Leave it empty to change this element.

Try it in Interact mode (below). A panel that a reactor shows should usually start hidden: turn
off **Visible in Game** in its **Element** tab.

## Try the UI in Interact mode

![The main menu in Interact mode with an orange-tinted header, and the Console showing
the Continue button's click](../images/editor/ui_interact.jpg)

*In Interact mode the header turns orange. Clicking **Continue** printed `[UI] Continue click`
in the Console.*

1. Press **Tab** over the canvas, or click the **Interact** icon in the header.
2. Click buttons, drag sliders and switch tabs as a player would.
3. Watch the **Console**: each named element prints what it did, with any new value.
4. Press **Tab** or **Esc** over the canvas to go back to **Design**.

**F5** also switches between the two modes while a UI is open.

> [!NOTE]
> Anything you change by playing with the UI, such as a slider's position, is thrown away when
> you return to Design. The UI goes back to how you built it.

## See what programmers can use

![The Bindings tab listing the main menu's named elements and their kinds, with the Copy C++
Binding Stub button below](../images/editor/ui_bindings.jpg)

The **Bindings** tab (the link icon in Properties) lists every named element that a programmer
can hook up to the game, and what kind of widget each one is. Here the main menu's four buttons
and three texts are listed by name.

- Hover a row to see the signals that element sends, such as **click** for a button.
- Click a row to select that element.
- A warning icon means two elements share a name. Only the first one can be reached, so rename
  the other.
- **Copy C++ Binding Stub** copies a ready-made list of every named element to the clipboard,
  for a programmer to paste into the game's code.
- **No-Code Wiring** is a reminder that reactors can do simple jobs without a programmer.

## Show a UI in a scene

1. Open the scene.
2. Drag the UI from the **UI** tab of the Asset panel into the viewport, or right-click it and
   choose **Place in Scene**.

The UI is now part of the scene and appears when the scene starts. See
[Viewport](viewport.md#add-objects-and-drop-assets) for more on dragging assets in.

---

Sources: `editor/app/ui/ui_canvas.inl`, `editor/ui/ui_palette.h`, `editor/ui/ui_canvas_math.h`,
`editor/viewport/rect_gizmo.h`, `editor/schema/ui_schema.h`, `editor/schema/ui_theme_schema.h`,
`editor/app/ui/theme_editor.inl`, `editor/app/ui/assets.inl`, `editor/app/ui/properties.inl`,
`editor/app/ui/layout.inl`, `editor/app/editor_app.h`

Previous: [Animation](animation.md) | Next: [Project settings](project-settings.md)
