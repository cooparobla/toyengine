# toyengine/fire

Fire, as a property of an **object**.

There is no world fire state here: no field, no grid, no scene-level fire block, nothing
spreading between objects, and no coupling to the weather or the environment field. A thing is
either alight or it is not.

| File | What it is |
|---|---|
| [`fire.h`](fire.h) | `Burnable`, `FireIgniter` |
| [`fire_system.h`](fire_system.h) | order **44**: advances each burn front, burns objects out |

## The two components

| Author | For |
|---|---|
| `Burnable` | an object that catches at a POINT and burns outward across itself |
| `FireIgniter` | opts a scene into click-to-ignite |

```yaml
- type: Burnable
  spread_rate: 0.14      # m/s the front travels out over the surface
  burn_time: 18          # s from ignition to consumed
  on_burnt: char
```

## A visual fire is not a component

A candle or a campfire is a **particle prefab** -- `objects/props/campfire` -- not anything in
here. There was briefly a `Fire` component that spawned emitters, a light and a flicker from a
look asset; it was removed because a prefab is shorter to place, fully editable in the editor,
tunable per instance, and does not give the engine an opinion about what fire looks like. Two
ways to make a campfire is worse than one.

Nothing authored decides what a BURNING SURFACE looks like either. Char is the surface's **own
albedo** crushed toward black, so a pale chair and a dark crate both char plausibly without
either being authored for it, and the edge runs a black-body ramp.

## How a burn front works

Ignition takes a world point -- the point a click landed on -- and stores it in the object's
LOCAL space, so a burning object that is then moved or turned carries its burn with it. A front
then grows outward from it at `spread_rate`.

The front reaches the renderer through the material, and **the system owns those two fields for
as long as an object is alight**:

| Field | Carries |
|---|---|
| `PBRMaterial::burning` | `surface_ext.z` bit 2 -- switches the burn layer on at all |
| `PBRMaterial::shader_params` | `(origin_ws.xyz, front_radius)` |

A material that uses `shader_params` for its own surface shader must therefore not also be
burnable.

`assets/shaders/gfx/surface/burn.glsl` draws it: char behind, an **advancing incandescent rim**
at the edge, untouched material ahead. The rim is the whole effect -- a burning object is not
uniformly glowing, and a travelling edge is what reads as "on fire" rather than "tinted orange".
It runs a black-body ramp rather than one flat ember colour, and crawls with `gfx_time`.

## Known limits

- **The front is a sphere in local space, not a geodesic over the surface.** It crosses the gap
  between a chair's seat and its leg rather than travelling the material between them. True
  surface spread needs mesh connectivity; at flame scale nobody can tell.
- **The pick is ray-vs-bounding-box, per PART.** Exact for anything built from boxes; a click
  near the silhouette of a curved mesh can land slightly off it. Per part, not per object,
  because a chair is mostly empty space inside its overall bounds -- one box would report a
  click on a leg as a hit above the seat.
- **Fire does no damage and has no gameplay meaning.** Burning is a world state; what it does to
  a character is a game's business.
