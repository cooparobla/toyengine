"""Objects and components: the model a scene and an object asset both use.

A scene's `root_objects` and an object asset's `object:` hold the same shape -- a name, an
`active` flag, a list of components and a list of children -- so one builder serves both. An
`Obj` is that node, and `Component` is one entry in its `components:` list.

## There is no reflection, here or in the engine

Every component's keys are parsed by hand in C++ (`src/toyengine/scene/register.cpp` or the
area's own `*_yaml.cpp`), so this module cannot validate a component's keys against the engine
and does not pretend to. `component()` takes the type name and whatever keys you pass, which
means a **typo in a key is silent** -- the engine ignores keys it does not know, exactly as it
ignores a key for a component that was renamed. The authority on what a component accepts is
the key reference in the `create-scene` skill
(`.claude/skills/create-scene/reference/components.md`); the convenience methods below cover
the handful that appear in nearly every scene, and spell their keys correctly by construction.
"""

from ..yamlwrite import Flow


def vec3(value):
    """An inline `{ x: .., y: .., z: .. }` mapping, the way every asset spells a vector.

    Args:
        value (tuple | list | dict | Flow): Three numbers in x, y, z order, a mapping already
            carrying `x`, `y` and `z`, or the result of an earlier `vec3()` -- passing one
            straight back through is a no-op, so a caller may hand either form to any method
            that takes a vector.

    Returns:
        Flow: The inline mapping.

    Raises:
        ValueError: If a sequence does not hold exactly three numbers.
    """
    if isinstance(value, Flow):
        return value
    if isinstance(value, dict):
        return Flow({axis: float(value[axis]) for axis in ("x", "y", "z")})
    items = list(value)
    if len(items) != 3:
        raise ValueError(f"a vec3 needs exactly three numbers, got {items!r}")
    # Coerced to float so an integer argument still writes `x: 0.0`: a position is a float
    # field in the engine, and every committed asset spells one with its decimal point.
    return Flow({"x": float(items[0]), "y": float(items[1]), "z": float(items[2])})


def rgb(value):
    """An inline `{ r: .., g: .., b: .. }` colour, optionally with `a`.

    Args:
        value (tuple | list | dict | Flow): Three or four numbers in r, g, b(, a) order, a
            mapping already carrying those keys, or the result of an earlier `rgb()` -- passing
            one straight back through is a no-op, as with `vec3()`.

    Returns:
        Flow: The inline mapping.

    Raises:
        ValueError: If a sequence does not hold three or four numbers.
    """
    if isinstance(value, Flow):
        return value
    if isinstance(value, dict):
        out = {k: float(value[k]) for k in ("r", "g", "b") if k in value}
        if "a" in value:
            out["a"] = float(value["a"])
        return Flow(out)
    items = list(value)
    if len(items) not in (3, 4):
        raise ValueError(f"a colour needs three or four numbers, got {items!r}")
    # Float-coerced for the same reason as vec3(): `r: 1` would be the only `1` in a tree of
    # `1.0`s.
    out = {"r": float(items[0]), "g": float(items[1]), "b": float(items[2])}
    if len(items) == 4:
        out["a"] = float(items[3])
    return Flow(out)


class Component:
    """One entry in an object's `components:` list.

    Args:
        type_name (str): The component's `type:`, e.g. `"MeshRenderer"`. This is the string
            the engine registered a parser under, and it is case-sensitive.
        **keys: The component's keys, in the order they should appear in the file. Values may
            be scalars, lists, dicts, or `Flow`-wrapped collections for inline rendering.
    """

    def __init__(self, type_name, **keys):
        self.type_name = type_name
        self.keys = dict(keys)

    def to_yaml(self):
        """The component as a mapping ready for `yamlwrite.emit`.

        Returns:
            dict: `type:` first, then every key in insertion order.
        """
        return {"type": self.type_name, **self.keys}


class Obj:
    """A scene object or the root of an object asset.

    The convenience methods each return `self`, so a node and its components read as one
    expression:

        Obj("ground").transform(scale=(10, 10, 1)).mesh("plane", material="materials/stone")

    Args:
        name (str): The object's name, as the outliner and object paths show it.
        active (bool): Whether the object starts enabled.
        prefab (str): An object asset to instance here, e.g. `"objects/campfire"`. This is an
            **object-level key, not a component** -- a `Transform` beside it overrides the
            prefab's own, which is how the committed scenes place many copies of one asset.
            Getting this wrong is instructive: `component("prefab", path=...)` writes a
            component type nothing has registered, and the engine ignores it silently.
    """

    def __init__(self, name, active=True, prefab=None):
        self.name = name
        self.active = active
        self.prefab = prefab
        self.components = []
        self.children = []

    def add(self, component):
        """Appends an already-built `Component`.

        Args:
            component (Component): The component to add.

        Returns:
            Obj: self.
        """
        self.components.append(component)
        return self

    def component(self, type_name, **keys):
        """Appends a component by type name and keys.

        The general escape hatch: any component the engine parses can be written with this,
        including one a project registers itself through `TOY_MODULE`.

        Args:
            type_name (str): The component's `type:`.
            **keys: Its keys, in output order.

        Returns:
            Obj: self.
        """
        return self.add(Component(type_name, **keys))

    def child(self, obj):
        """Appends a child object.

        Args:
            obj (Obj): The child.

        Returns:
            Obj: self.
        """
        self.children.append(obj)
        return self

    def transform(self, position=None, rotation=None, scale=None):
        """Appends a `Transform`, writing only the keys given.

        Rotation is in degrees. Omitting a key leaves the engine's default for it, which is
        why an object asset usually writes `position` alone; a scene object conventionally
        writes all three.

        Args:
            position (tuple): World or parent-relative position.
            rotation (tuple): Euler angles in degrees.
            scale (tuple): Per-axis scale.

        Returns:
            Obj: self.

        Raises:
            ValueError: If every argument is omitted -- an empty `Transform` says nothing, and
                a node that wants the default transform should simply not have one.
        """
        keys = {}
        if position is not None:
            keys["position"] = vec3(position)
        if rotation is not None:
            keys["rotation"] = vec3(rotation)
        if scale is not None:
            keys["scale"] = vec3(scale)
        if not keys:
            raise ValueError(
                "transform() needs at least one of position, rotation or scale; omit the "
                "Transform entirely to accept every default")
        return self.component("Transform", **keys)

    def mesh(self, mesh_path, material=None, **keys):
        """Appends a `MeshRenderer`.

        Args:
            mesh_path (str): A mesh asset path (`props/crate`) or one of the engine's built-in
                primitives (`cube`, `plane`, `sphere`).
            material (str | dict): A shared material path (`materials/basic/plastic`, which
                resolves by name through the materials tree), or a mapping for an inline
                material -- `{"base": "materials/basic/plastic", "albedo": rgb((1, 0, 0))}`
                tints a shared one per object.
            **keys: Any further `MeshRenderer` keys.

        Returns:
            Obj: self.
        """
        out = {"mesh_path": mesh_path}
        if material is not None:
            out["material"] = Flow(material) if isinstance(material, dict) else material
        out.update(keys)
        return self.component("MeshRenderer", **out)

    def camera(self, main=True, projection="Perspective", fov=55.0,
               near_clip_plane=0.1, far_clip_plane=500.0, **keys):
        """Appends a `Camera`.

        Args:
            main (bool): Whether this is the scene's main camera.
            projection (str): `"Perspective"` or `"Orthographic"`.
            fov (float): Vertical field of view in degrees.
            near_clip_plane (float): Near plane distance.
            far_clip_plane (float): Far plane distance.
            **keys: Any further `Camera` keys.

        Returns:
            Obj: self.
        """
        return self.component("Camera", main=main, projection=projection, fov=fov,
                              near_clip_plane=near_clip_plane,
                              far_clip_plane=far_clip_plane, **keys)

    def directional_light(self, direction=(-0.4, 0.35, -0.85), color=(1.0, 0.96, 0.9),
                          intensity=1.0, cast_shadows=True, **keys):
        """Appends a `DirectionalLight` -- the sun.

        Args:
            direction (tuple): The direction the light travels.
            color (tuple): Its colour.
            intensity (float): Its intensity.
            cast_shadows (bool): Whether it casts the shadow cascades.
            **keys: Any further `DirectionalLight` keys.

        Returns:
            Obj: self.
        """
        return self.component("DirectionalLight", direction=vec3(direction), color=rgb(color),
                              intensity=intensity, cast_shadows=cast_shadows, **keys)

    def to_yaml(self):
        """The object as a mapping ready for `yamlwrite.emit`.

        A prefab instance is written the way the committed scenes write one -- `name`, `prefab`,
        then the overriding components, with no `active` and no empty `children`, because what
        it does not say it inherits from the asset. Everything else gets the full form, with
        `children: []` spelled out as the committed scenes do.

        Returns:
            dict: The object's keys, in the order the committed scenes use.
        """
        if self.prefab is not None:
            out = {"name": self.name, "prefab": self.prefab,
                   "components": [c.to_yaml() for c in self.components]}
            if self.children:
                out["children"] = [c.to_yaml() for c in self.children]
            return out
        return {
            "name": self.name,
            "active": self.active,
            "components": [c.to_yaml() for c in self.components],
            "children": [c.to_yaml() for c in self.children],
        }
