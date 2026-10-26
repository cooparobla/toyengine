[Editor manual](README.md) > Painting instances

An **Instance Painter** is how you fill a world with things: trees over a hillside, rocks and
grass across a valley, a paved path laid on a grid, clutter around a camp. You paint them with a
brush, exactly as you paint a terrain's layers — but what the brush puts down are copies of an
object you already made.

The copies are cheap on purpose. A painted instance is a *record*, not an object in your scene:
it has a position, a rotation, a scale and nothing else. That is what makes it reasonable to
cover a world with hundreds of thousands of them, where a few thousand real objects would already
be unmanageable. When one of them needs to be more than scenery — a door that opens, a chest with
loot — you turn that one into a real object (see [Convert to Object](#turn-an-instance-into-an-object)).

Painting is for **placing things you chose**. If you want grass scattered over a whole landscape
by rules rather than by hand, use a terrain's foliage scatter instead
([Terrain > Grass, trees and rocks](terrain.md)). Most worlds use both.

## Add a painter

1. Choose **Add > Instance Painter** in the viewport header. (You can have as many as you like —
   one for a forest, one for a village's props, one for rubble.)
2. Select it and click **Paint Instances** in **Properties > Tool**, or pick **Instance Paint**
   in the mode dropdown at the left of the viewport header.

A new painter has nothing in its palette yet, so the first thing to do is add something to paint.

## Build the palette

**Properties > Tool > Prototypes** is the list of what this brush can place. Use **Add** to pick
an object asset, or **Add Mesh** for a single mesh.

- An **object asset** brings everything it has along: several meshes, its materials, its levels
  of detail. A pine made of a trunk and a canopy paints as a pine, not as a trunk.
- A **mesh** is the shortcut for something simple — a rock, a tuft of grass — and you give it a
  material here.

Click a prototype to select it; the brush places whichever one is selected. Open its
**Placement** section to say how its copies should vary:

| | |
|---|---|
| **Scale** | Each copy takes a size somewhere in this range. `[0.85, 1.3]` keeps a forest from looking stamped. |
| **Yaw** | How far each copy is turned. Leave it at `0 – 360` for anything organic. |
| **Align to Normal** | `0` stands every copy straight up whatever the ground does; `1` lies it flat on the slope. Trees want a little, rocks want a lot. |
| **Sink** | How far into the ground it is pushed, so a trunk is not perched on a hillside. |
| **Collision** | Whether you can walk into it — see [Making them solid](#making-them-solid). |
| **Draw Distance** | How far away it stops being drawn. Leave it at `0` and the editor works it out from how tall the thing is. |

## Paint

The four tools are in the toolbar down the left of the viewport, and in **Properties > Tool**:

| Tool | What it does |
|---|---|
| **Paint** (1) | Scatters copies under the brush as you drag. **Ctrl** erases instead. |
| **Erase** (2) | Removes whatever is under the brush. |
| **Place** (3) | Drops exactly one copy where you click. |
| **Cluster** (4) | Drops a clump per click — a copse of trees, a pile of rocks. |
| **Select** (5) | Picks instances you already placed, to fix them — see [Fixing what you placed](#fixing-what-you-placed). |

The brush is a circle draped over whatever is beneath it, so it reads correctly over a slope, a
step or a boulder. Its settings are in **Properties > Tool > Brush**:

- **Radius** — how wide it is, in metres. **F** drags it; **[** and **]** step it.
- **Density** — how many copies per square metre. **Shift+F** drags it.
- **Falloff** — how much the edge thins out. The inner ring shows the part at full density. A
  stroke never ends in a hard circle of props.
- **Min Spacing** — how far copies keep clear of each other. Without it, dragging slowly piles
  everything on one spot.

**You can paint on anything.** Terrain of either kind, a rock, a wall, the roof of a building —
the brush finds whatever surface is under each point it is about to place on. One stroke can run
off a hillside and up onto a plinth without changing tools.

## Snap to a grid

Press **S**, or click the grid button in the viewport header, to turn **Snap** on. Placement then
lands on a grid and rotation is quantised — which is what makes the painter useful for fences,
walls, paths and modular kits rather than only for foliage. The grid is drawn under the brush.

In **Properties > Tool > Snap**:

- **Cell** — the grid's spacing in metres.
- **Origin** — shifts the grid, so a new row lines up with something already placed.
- **Yaw Step** — rotation is rounded to this. `90` for walls; `0` leaves rotation alone.
- **One Per Cell** — a cell that already holds something takes no more.

Snap works with Place, Paint and Cluster, so you can lay a path in one drag.

## Clumps

With **Cluster** selected, **Properties > Tool > Brush** gains its own settings: **Count Min** and
**Count Max** (how many per click), **Spread** (how far they reach), and **Centre Bias** (`0`
spreads the clump evenly, `1` crowds it in the middle). Turn on **Mix Prototypes** and each clump
draws at random from the first few entries in your palette — a copse of three kinds of tree from
one click.

## Fixing what you placed

Painting is fast and approximate; **Select** is how you go back and get a particular tree right.

- **Click** an instance to select it. Click empty space to clear.
- **Drag a box** over several to select all of them. **Shift** adds to the selection, **Ctrl**
  removes from it.

Selected instances are ringed at the base with a line up their height. What you can do to them is
in **Properties > Tool > Selection**, and it changes depending on how many you have:

| One instance | A group |
|---|---|
| **Yaw** sets which way it faces, in degrees | **Turn** swings the whole group about its centre, so the arrangement holds instead of each one spinning where it stands |
| **Scale** sets its size | **Spin Each 90** turns every one in place; **Randomize Yaw** gives each a different facing |
| | **Scale ×1.1 / ×0.9** grow and shrink them together |

Both get:

- **Raise** nudges the selection up or down. It springs back to zero — it is a nudge, not a height.
- **Drop to Surface** re-places each one on whatever is under it *now*. This is what you want
  after sculpting ground that already had things standing on it.
- **Change To** swaps the selection for another prototype, where they stand.
- **Delete Selected** (or **X**) removes them.

Each of these is one undo step, however many instances it touched.

> **Painting or erasing clears the selection.** A selection remembers *which* instances by their
> position in the painter's storage, and adding or removing any instance shifts the rest along.
> Rather than risk turning the wrong tree, the editor drops the selection whenever the set
> changes under it. Select, edit, then carry on painting.

## Making them solid

Painted instances have no collision until you ask for one, per prototype, under **Placement >
Collision**. Grass and flowers should stay at **none**: they then cost the physics engine nothing
at all, which is what lets you paint a field of them.

| | |
|---|---|
| **none** | Scenery. You walk through it. |
| **auto** | Fits a shape to the whole thing — a capsule if it is taller than it is wide, otherwise a box. |
| **box**, **sphere**, **capsule** | The same shapes, but you choose. |
| **mesh** | The thing's own shape, for something you must not be able to clip into. |

> **A tree is the case where `auto` is wrong.** It fits the shape to the whole prototype, canopy
> included, so you bump into the branches a couple of metres before you reach the trunk. Choose
> **capsule** and set its radius to the trunk instead.

Colliders are only created near the camera — **Properties > Tool > Painter > Collision Radius**
says how near. Set it to `0` to turn collision off entirely for this painter.

## Turn an instance into an object

A painted copy cannot do anything by itself. When one of them needs to — a crate that breaks, a
lamp that lights — hover it and press **Ctrl+C**: the editor places a real object there, with the
same position and rotation, and removes the record. It is an ordinary object from then on, with
its own components, and you can undo it like anything else.

(This only works for prototypes added as object assets, since that is what the new object is an
instance of.)

## Saving

Painted instances are stored in their own file beside the scene, not inside it — so a scene with
a hundred thousand of them is no bigger than one with none. **Ctrl+S** saves the scene and writes
that file at the same time, exactly as it writes a terrain's heightmap.

## Undo

Every stroke is one undo step, however many copies it put down, and it shares the one timeline
with everything else you do in the scene. **Ctrl+Z** steps back through painting, sculpting and
object edits in the order you made them.

## Leaving the mode

Press **Tab** or **Esc**, pick **Object Mode** in the mode dropdown, or click **Exit Instance
Paint Mode** in **Properties > Tool**.
