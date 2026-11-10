# Scene Components Submodule (`coopa::scene`)

This directory now holds only the one component type libcoopa's scene system
defines directly. Every renderer-specific component (mesh renderer, camera,
lights, GI/reflection probes) moved to gfxcoopa's `engine/components/`, and
every UI component (RectTransform, Canvas, Image, Text, Button, layout
groups, ...) lives in uicoopa's `uicoopa/`. Both register their component
YAML parsers with `coopa::scene::SceneLoader::register_component_parser()`
instead of this module knowing about them — see the parent
[Scene Module README](../README.md).

The scene-wide `Animator` component lives in the sibling `coopa::anim` module
— see [Animation Module README](../../animation/README.md).
It is registered the same way as any external component (via
`register_component_parser("Animator", ...)`, see
`coopa::anim::register_animation_components()`), not built into `SceneLoader`.

---

## Component Inheritance Graph

```text
┌──────────────────────────────────────────────────────────────────────────────────────────┐
│                            COMPONENT INHERITANCE HIERARCHY                               │
└──────────────────────────────────────────────────────────────────────────────────────────┘
                                           │
                                ┌──────────┴──────────┐
                                │      Component      │
                                │     (Abstract)      │
                                └──────────┬──────────┘
                                           │
                                           ▼
                                   ┌───────────────┐
                                   │   Transform   │
                                   │   Component   │
                                   ├───────────────┤
                                   │ position      │
                                   │ rotation      │
                                   │ scale         │
                                   └───────────────┘

  (MeshRenderer, Camera, DirectionalLight, PointLight, SpotLight, EnvironmentLight,
   GiProbeVolume, ReflectionProbe -> gfxcoopa/gfxcoopa/engine/components/)
  (RectTransform, Canvas, Image, Text, Button, layout groups -> uicoopa/uicoopa/)
  (Animator -> coopa/animation/animator.h, registered via register_component_parser)
```

---

## Component Breakdown

### [`transform_component.h`](transform_component.h)

Wraps `coopa::util::Transform`, linking it into the SceneObject hierarchy for
world-matrix propagation:
- `transform()`: Mutable/const access to the underlying `coopa::util::Transform`
  (`position`, `rotation_degrees`/`rotation_quat`, `scale`, local/world matrices).
- `get_world_matrix() -> glm::mat4`: Lazily recomputed from the parent chain.
- `set_parent_transform(...)`: Links this transform under a parent's transform.
