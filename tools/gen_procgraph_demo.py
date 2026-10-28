#!/usr/bin/env python3
"""Generates the procedural-graph demo's scene and the small meshes its graphs instance.

The graphs themselves are hand-authored under ``assets/graphs/``; this script writes only what a
graph needs to *point at* (a step box, a railing post, a lamp prefab) plus the scene that places
the ``ProceduralGraph`` components and their input markers.

Run from the repository root::

    python3 tools/gen_procgraph_demo.py
"""

import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mesh_yaml

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCENE_DIR = os.path.join(ROOT, "assets", "scenes", "procedural", "procgraph_demo")
MESH_DIR = os.path.join(ROOT, "assets", "meshes", "props")
OBJECT_DIR = os.path.join(ROOT, "assets", "objects", "props")


def box(size):
    """Returns (verts, faces, uvs, normals) for an axis-aligned box centred on the origin.

    Four vertices per face rather than eight shared corners: ``mesh_yaml.write_mesh`` takes one
    normal and one UV **per vertex**, so a box built from shared corners would get averaged
    corner normals and shade like a sphere. Each face owning its own four vertices is what makes
    it read flat.

    Args:
        size: An (x, y, z) triple of full extents.

    Returns:
        tuple: vertices, quad faces, per-vertex UVs and per-vertex normals.
    """
    hx, hy, hz = size[0] / 2.0, size[1] / 2.0, size[2] / 2.0
    # Each entry is (the face's four corners counter-clockwise from outside, its normal).
    sides = [
        ([(-hx, -hy, hz), (hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz)], (0.0, 0.0, 1.0)),
        ([(hx, -hy, -hz), (-hx, -hy, -hz), (-hx, hy, -hz), (hx, hy, -hz)], (0.0, 0.0, -1.0)),
        ([(-hx, -hy, -hz), (hx, -hy, -hz), (hx, -hy, hz), (-hx, -hy, hz)], (0.0, -1.0, 0.0)),
        ([(hx, hy, -hz), (-hx, hy, -hz), (-hx, hy, hz), (hx, hy, hz)], (0.0, 1.0, 0.0)),
        ([(hx, -hy, -hz), (hx, hy, -hz), (hx, hy, hz), (hx, -hy, hz)], (1.0, 0.0, 0.0)),
        ([(-hx, hy, -hz), (-hx, -hy, -hz), (-hx, -hy, hz), (-hx, hy, hz)], (-1.0, 0.0, 0.0)),
    ]
    verts, faces, uvs, normals = [], [], [], []
    for corners, normal in sides:
        base = len(verts)
        verts.extend(corners)
        normals.extend([normal] * 4)
        uvs.extend([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)])
        faces.append([base, base + 1, base + 2, base + 3])
    return verts, faces, uvs, normals


def write_meshes():
    """Writes the two boxes the demo graphs instance."""
    os.makedirs(MESH_DIR, exist_ok=True)

    # One stair tread. The graph scales nothing: it instances this at each point, so the mesh's
    # own size is the step size the graph's `width`/`run`/`rise` inputs describe at their defaults.
    verts, faces, uvs, normals = box((1.4, 0.3, 0.18))
    mesh_yaml.write_mesh(Path(MESH_DIR) / "procgraph_step.yaml", verts, faces, uvs,
                         normals, mesh_yaml.perpendicular_tangents(normals))

    # A railing post, sitting on z = 0 so it reads as standing on the ground.
    verts, faces, uvs, normals = box((0.08, 0.08, 1.0))
    verts = [(x, y, z + 0.5) for (x, y, z) in verts]
    mesh_yaml.write_mesh(Path(MESH_DIR) / "procgraph_post.yaml", verts, faces, uvs,
                         normals, mesh_yaml.perpendicular_tangents(normals))
    print("wrote 2 meshes to assets/meshes/")


LAMP_OBJECT = """\
# A lamp the marker_lights graph instances: a small emissive post plus the light itself.
# Instanced rather than built by the graph, because what that graph demonstrates is that it can
# place COMPONENTS -- the geometry is incidental.
name: ProcgraphLamp
active: true
components:
  - type: Transform
    position: { x: 0.0, y: 0.0, z: 0.0 }
  - type: MeshRenderer
    mesh_path: procgraph_post
    material:
      albedo: { r: 0.9, g: 0.85, b: 0.7 }
      emissive: { r: 1.0, g: 0.8, b: 0.4 }
      emissive_strength: 2.0
      roughness: 0.4
  - type: PointLight
    color: { r: 1.0, g: 0.82, b: 0.55 }
    intensity: 6.0
    range: 7.0
children: []
"""


def write_objects():
    """Writes the lamp prefab the no-geometry graph instances."""
    os.makedirs(OBJECT_DIR, exist_ok=True)
    path = os.path.join(OBJECT_DIR, "procgraph_lamp.yaml")
    with open(path, "w") as f:
        f.write(LAMP_OBJECT)
    print("wrote assets/objects/props/procgraph_lamp.yaml")


SCENE = """\
# procgraph_demo -- toyengine/procgraph/ end to end. Every element makes one claim checkable.
#
#   * STAIR_A/B/C: ONE graph (graphs/parametric_stair) at three input sets, so a graph is a
#     parameterized asset rather than a baked file.
#   * MARKER_LIGHTS: a graph that emits NO geometry at all -- only components, at authored
#     markers. This is why the system is a node-graph -> SCENE-graph converter and not
#     "geometry nodes".
#   * RAILING: built between two authored marker objects. Move RailStart or RailEnd and it
#     re-bakes, which is the scene-input contract made visible.
#
# Meshes and the lamp prefab: tools/gen_procgraph_demo.py.  Run: ./build/toyengine procgraph_demo
# World: Z up, metres.
scene:
  scene_name: procgraph_demo
  settings:
    procgraph:
      variables:
        demo.scale: 1.0
    render:
      ambient_intensity: 0.9
  root_objects:
    - name: camera
      active: true
      components:
        - type: Transform
          position: { x: 0.0, y: -16.0, z: 10.0 }
          rotation: { x: 62.0, y: 0.0, z: 0.0 }
        - type: Camera
          main: true
          projection: Perspective
          fov: 60.0
      children: []

    - name: sun
      active: true
      components:
        - type: Transform
          position: { x: 0.0, y: 0.0, z: 0.0 }
        - type: DirectionalLight
          direction: { x: -0.45, y: 0.6, z: -0.65 }
          color: { r: 1.0, g: 0.96, b: 0.9 }
          intensity: 2.0
          cast_shadows: true
      children: []

    - name: ground
      active: true
      components:
        - type: Transform
          position: { x: 0.0, y: 0.0, z: 0.0 }
          scale: { x: 40.0, y: 40.0, z: 1.0 }
        - type: MeshRenderer
          mesh_path: plane
          material: { albedo: { r: 0.28, g: 0.30, b: 0.27 }, roughness: 0.9 }
        - type: BoxCollider
          size: { x: 40.0, y: 40.0, z: 0.2 }
      children: []

    # --- One graph, three input sets ------------------------------------------------------
    - name: Stair_A
      active: true
      components:
        - type: Transform
          position: { x: -6.0, y: 0.0, z: 0.0 }
        - type: ProceduralGraph
          graph: graphs/parametric_stair
          seed: 1
          bake: on_load
          inputs:
            steps: 5
            rise: 0.18
            run: 0.30
            width: 1.4
      children: []

    - name: Stair_B
      active: true
      components:
        - type: Transform
          position: { x: -2.0, y: 0.0, z: 0.0 }
        - type: ProceduralGraph
          graph: graphs/parametric_stair
          seed: 1
          bake: on_load
          inputs:
            steps: 9
            rise: 0.14
            run: 0.26
            width: 1.4
      children: []

    - name: Stair_C
      active: true
      components:
        - type: Transform
          position: { x: 2.0, y: 0.0, z: 0.0 }
        - type: ProceduralGraph
          graph: graphs/parametric_stair
          seed: 1
          bake: on_load
          inputs:
            steps: 14
            rise: 0.10
            run: 0.22
            width: 1.4
      children: []

    # --- A graph that emits no geometry ---------------------------------------------------
    - name: Markers
      active: true
      components:
        - type: Transform
          position: { x: 8.0, y: 0.0, z: 0.0 }
      children:
        - name: Marker_0
          active: true
          components:
            - type: Transform
              position: { x: 0.0, y: 0.0, z: 0.0 }
          children: []
        - name: Marker_1
          active: true
          components:
            - type: Transform
              position: { x: 2.0, y: 2.0, z: 0.0 }
          children: []
        - name: Marker_2
          active: true
          components:
            - type: Transform
              position: { x: 4.0, y: -1.0, z: 0.0 }
          children: []

    - name: MarkerLights
      active: true
      components:
        - type: Transform
          position: { x: 0.0, y: 0.0, z: 0.0 }
        - type: ProceduralGraph
          graph: graphs/marker_lights
          bake: on_change
          inputs:
            markers: { children_of: Markers }
            intensity: 6.0
      children: []

    # --- A graph driven by authored markers -----------------------------------------------
    - name: RailStart
      active: true
      components:
        - type: Transform
          position: { x: -6.0, y: 7.0, z: 0.0 }
      children: []

    - name: RailEnd
      active: true
      components:
        - type: Transform
          position: { x: 4.0, y: 7.0, z: 0.0 }
      children: []

    - name: Railing
      active: true
      components:
        - type: Transform
          position: { x: 0.0, y: 0.0, z: 0.0 }
        - type: ProceduralGraph
          graph: graphs/spline_railing
          bake: on_change
          inputs:
            start: { object: RailStart }
            end: { object: RailEnd }
            posts: 8
            height: 1.0
      children: []
"""


def write_scene():
    """Writes the demo scene."""
    os.makedirs(SCENE_DIR, exist_ok=True)
    path = os.path.join(SCENE_DIR, "scene.yaml")
    with open(path, "w") as f:
        f.write(SCENE)
    print("wrote assets/scenes/procedural/procgraph_demo/scene.yaml")


def main():
    """Writes every demo asset."""
    write_meshes()
    write_objects()
    write_scene()


if __name__ == "__main__":
    main()
