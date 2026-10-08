#!/usr/bin/env python3
"""Writes scenes/tests/navigation/nav_open_world/scene.yaml -- the open-world flow-field bench:
hierarchical flow fields that integrate exactly only where their followers are.

A 400 x 400 m world (0.5 m cells, 25 x 25 tiles of 16 m). Two flow fields:
  * `player` circles slowly near the centre; six packs (216 mobs) spread over the map chase it:
    two behind a long wall across the north (three gaps), one on top of a 4 m mesa (two ramps),
    one in the far west, one in the far south, one close by.
  * `outpost` sits in the south-west corner; 30 scouts start in the far north-east corner and
    cross the whole map to it (through a wall gap).
Each field's coarse pass routes the whole world over tile-border portals in a millisecond or
two; only the tiles each pack occupies (plus two tiles ahead of it, and the goal's surroundings)
are integrated exactly. The overlay outlines those tiles: watch them roll along with the packs.

Run:  python3 tools/gen_nav_open_world_scene.py   then   ./build/toyengine nav_open_world
"""

import math
import os
import random

ASSETS = os.path.join(os.path.dirname(__file__), "..", "assets")
OUT = os.path.join(ASSETS, "scenes", "tests", "navigation", "nav_open_world", "scene.yaml")

HALF = 200.0                    # world half-size, metres
PLAYER_CENTER = (0.0, -20.0)
PLAYER_RADIUS = 35.0
WALL_Y = 60.0
WALL_GAPS = [(-100.0, 6.0), (20.0, 6.0), (150.0, 6.0)]   # (centre x, width)
MESA = (140.0, 40.0, 40.0, 40.0, 4.0)                   # centre x, y, size x, y, height
PACKS = [                                               # (centre, count, target)
    ((-120.0, 150.0, 0.0), 36, "player"),
    ((60.0, 160.0, 0.0), 36, "player"),
    ((-170.0, -60.0, 0.0), 36, "player"),
    ((40.0, -170.0, 0.0), 36, "player"),
    ((140.0, 40.0, 4.0), 36, "player"),    # on the mesa
    ((-35.0, 20.0, 0.0), 36, "player"),    # near
    ((165.0, 170.0, 0.0), 30, "outpost"),  # scouts
]
OUTPOST = (-160.0, -165.0)


def fmt(f):
    s = "%.4f" % f
    s = s.rstrip("0")
    return s + "0" if s.endswith(".") else s


def v(x, y, z):
    return "{ x: %s, y: %s, z: %s }" % (fmt(x), fmt(y), fmt(z))


def transform(pos, rot=(0, 0, 0), scale=(1, 1, 1)):
    return ("        - type: Transform\n"
            f"          position: {v(*pos)}\n"
            f"          rotation: {v(*rot)}\n"
            f"          scale: {v(*scale)}\n")


def block(name, pos, size, material, rot=(0, 0, 0)):
    return (f"    - name: {name}\n"
            f"      active: true\n"
            f"      components:\n"
            + transform(pos, rot, size) +
            f"        - type: MeshRenderer\n"
            f"          mesh_path: cube\n"
            f"          material: {material}\n"
            f"        - type: BoxCollider\n"
            f"          size: {v(1, 1, 1)}\n"
            f"          material: concrete\n"
            f"      children: []\n\n")


def mob(name, pos, color, target, scale=1.7):
    h = 0.65 * scale
    return (f"    - name: {name}\n"
            f"      active: true\n"
            f"      components:\n"
            + transform((pos[0], pos[1], pos[2] + h), (0, 0, 0), (scale, scale, scale)) +
            f"        - type: MeshRenderer\n"
            f"          mesh_path: barrel\n"
            f"          material: {{ albedo: {{ r: {color[0]:.3f}, g: {color[1]:.3f}, b: {color[2]:.3f} }}, metallic: 0.0, roughness: 0.6, ao: 1.0 }}\n"
            f"        - type: NavAgent\n"
            f"          base_offset: {fmt(h)}\n"
            f"          speed: 4.0\n"
            f"          flow_target: {target}\n"
            f"      children: []\n\n")


def marker(name, pos, color, scale, mover=""):
    return (f"    - name: {name}\n"
            f"      active: true\n"
            f"      components:\n"
            + transform(pos, (0, 0, 0), (scale, scale, scale)) +
            f"        - type: MeshRenderer\n"
            f"          mesh_path: sphere\n"
            f"          material: {{ albedo: {{ r: {color[0]}, g: {color[1]}, b: {color[2]} }}, emissive: {{ r: {color[0]}, g: {color[1]}, b: {color[2]} }}, emissive_strength: 4.0, metallic: 0.0, roughness: 0.4, ao: 1.0 }}\n"
            + mover +
            f"      children: []\n\n")


def in_mesa(x, y, margin):
    mx, my, sx, sy, _ = MESA
    return abs(x - mx) < sx / 2 + margin and abs(y - my) < sy / 2 + margin


def keep_clear(x, y, r):
    """True if a rock of radius r at (x, y) would block something the scene relies on."""
    d = math.hypot(x - PLAYER_CENTER[0], y - PLAYER_CENTER[1])
    if abs(d - PLAYER_RADIUS) < 6.0 + r:
        return True                                  # the player's orbit
    if abs(y - WALL_Y) < 5.0 + r:
        return True                                  # the wall and its gaps
    if in_mesa(x, y, 18.0 + r):
        return True                                  # the mesa and its ramps
    for (c, _, _) in PACKS:
        if math.hypot(x - c[0], y - c[1]) < 12.0 + r:
            return True                              # spawn areas
    if math.hypot(x - OUTPOST[0], y - OUTPOST[1]) < 10.0 + r:
        return True
    return False


def main():
    rng = random.Random(17)
    out = []
    out.append("""# nav_open_world -- open-world flow fields: a 400 x 400 m world where hierarchical flow
# fields integrate exactly only where their followers are.
#
# Run: ./build/toyengine nav_open_world        (or SCENE=nav_open_world)
# Generated by tools/gen_nav_open_world_scene.py -- edit that, not this file.
#
# Map (Z up; ground x/y in [-200, 200]; 0.5 m cells, 16 m tiles):
#   centre  `player` circles (0, -20) at 35 m. Six packs (216 mobs) chase it:
#           two north of the wall (y = 60, gaps at x = -100, 20, 150), one on the 4 m mesa
#           (x 120..160, y 20..60; ramps on its west and south sides), one far west, one far
#           south, one close by.
#   SW      `outpost`; 30 scouts start in the NE corner and cross the map to it.
#
# Overlay (`navigation.debug_draw: [Flow, FlowTiles, Links]`): boxes = the tiles a field
# integrates exactly (they roll along with the packs), small arrows inside = exact directions,
# large arrows = the coarse layer everywhere else (one per region, toward its best portal).
# Each field has its own colours (by target name): outpost = green boxes / ochre arrows,
# player = blue boxes / violet arrows.
# Worth sweeping: navigation.flow.mode (auto / exact / hierarchical), flow.lookahead_tiles,
# flow.near_radius, flow.exact_tile_budget.
format: blender
scene:
  scene_name: nav_open_world
  settings:
    navigation:
      cell_size: 0.5
      cell_height: 0.1
      tile_size: 32
      agents:
        - { name: Humanoid, radius: 0.6, height: 1.8, max_climb: 0.4 }
      max_path_requests_per_frame: 32
      flow:
        mode: auto
        exact_tile_budget: 64
        near_radius: 16
        lookahead_tiles: 2
        rebuild_distance: 1.0
        rebuild_interval: 0.25
        wall_penalty: 1.0
      debug_draw: [Flow, FlowTiles, Links]
      debug_flow_stride: 4
  root_objects:
    - name: ground
      active: true
      components:
""")
    out.append(transform((0, 0, 0), (0, 0, 0), (HALF, HALF, 1)))
    out.append("""        - type: MeshRenderer
          mesh_path: plane
          material: materials/prototype_grid
        - type: BoxCollider
          size: { x: 2.0, y: 2.0, z: 0.2 }
          center: { x: 0.0, y: 0.0, z: -0.1 }
          material: concrete
      children: []

    # ============================================================================
    # THE WALL -- across the north at y = 60, three 6 m gaps.
    # ============================================================================
""")
    xs = [-HALF]
    for (gx, gw) in sorted(WALL_GAPS):
        xs += [gx - gw / 2, gx + gw / 2]
    xs.append(HALF)
    for i in range(0, len(xs), 2):
        a, b = xs[i], xs[i + 1]
        out.append(block("wall_%d" % (i // 2), ((a + b) / 2, WALL_Y, 1.5), (b - a, 1.0, 3.0), "materials/brick"))

    out.append("""    # ============================================================================
    # THE MESA -- 40 x 40 m, 4 m high; ramps on its west and south sides.
    # ============================================================================
""")
    mx, my, sx, sy, mh = MESA
    out.append(block("mesa", (mx, my, mh / 2), (sx, sy, mh), "materials/sand"))
    run = 16.0
    theta = math.atan2(mh, run)
    length = math.hypot(run, mh) + 0.4
    thick = 0.5
    # West ramp, rising +X into the mesa's west face (x = mx - sx/2).
    x0 = mx - sx / 2 - run
    n = (-math.sin(theta), 0.0, math.cos(theta))
    c = (x0 + run / 2 - thick / 2 * n[0], my, mh / 2 - thick / 2 * n[2])
    out.append(block("ramp_west", c, (length, 8.0, thick), "materials/stone", rot=(0, -math.degrees(theta), 0)))
    # South ramp, rising +Y into the south face (y = my - sy/2).
    y0 = my - sy / 2 - run
    n = (0.0, -math.sin(theta), math.cos(theta))
    c = (mx, y0 + run / 2 - thick / 2 * n[1], mh / 2 - thick / 2 * n[2])
    out.append(block("ramp_south", c, (8.0, length, thick), "materials/stone", rot=(math.degrees(theta), 0, 0)))

    out.append("""    # ============================================================================
    # ROCKS -- scattered fields, clear of the orbit, the wall, the mesa and the spawns.
    # ============================================================================
""")
    rocks = 0
    tries = 0
    while rocks < 500 and tries < 20000:
        tries += 1
        # Clumped: pick a cluster centre, then jitter.
        if rocks % 8 == 0:
            cx, cy = rng.uniform(-HALF + 10, HALF - 10), rng.uniform(-HALF + 10, HALF - 10)
        x = cx + rng.gauss(0.0, 9.0)
        y = cy + rng.gauss(0.0, 9.0)
        sx_, sy_ = rng.uniform(1.0, 6.0), rng.uniform(1.0, 6.0)
        h = rng.uniform(1.0, 3.5)
        r = max(sx_, sy_) / 2
        if abs(x) > HALF - r - 1 or abs(y) > HALF - r - 1 or keep_clear(x, y, r):
            continue
        out.append(block("rock_%03d" % rocks, (x, y, h / 2), (sx_, sy_, h), "materials/stone",
                         rot=(0, 0, rng.uniform(0, 90))))
        rocks += 1

    out.append("""    # ============================================================================
    # TARGETS
    # ============================================================================
""")
    out.append(marker("player", (PLAYER_CENTER[0] + PLAYER_RADIUS, PLAYER_CENTER[1], 0.8), (1.0, 0.25, 0.2), 0.6,
                      "        - type: KinematicMover\n          mode: orbit\n"
                      f"          orbit_center: {v(PLAYER_CENTER[0], PLAYER_CENTER[1], 0.8)}\n"
                      f"          orbit_radius: {fmt(PLAYER_RADIUS)}\n          speed: 6.0\n"))
    out.append(marker("outpost", (OUTPOST[0], OUTPOST[1], 0.8), (0.3, 0.6, 1.0), 0.8))

    out.append("""    # ============================================================================
    # PACKS
    # ============================================================================
""")
    idx = 0
    for p, (centre, count, target) in enumerate(PACKS):
        cols = 6
        for k in range(count):
            x = centre[0] + (k % cols - cols / 2) * 1.4
            y = centre[1] + (k // cols - count / cols / 2) * 1.4
            if target == "player":
                hue = [(0.85, 0.25, 0.2), (0.9, 0.5, 0.15), (0.75, 0.2, 0.45), (0.85, 0.35, 0.3),
                       (0.95, 0.6, 0.25), (0.7, 0.3, 0.2)][p % 6]
            else:
                hue = (0.25, 0.55, 0.9)
            out.append(mob("mob_%03d" % idx, (x, y, centre[2]), hue, target))
            idx += 1

    out.append("""    # ============================================================================
    # CAMERA + LIGHT -- an overview: the whole map, with the overlay's exact tiles visible.
    # ============================================================================
    - name: camera
      active: true
      components:
""" + transform((0, -270, 380), (34, 0, 0)) + """        - type: Camera
          main: true
          projection: Perspective
          fov: 55.0
          near_clip_plane: 0.5
          far_clip_plane: 2000.0
        - type: CameraController
          mode: orbit
          target: { x: 0.0, y: 0.0, z: 0.0 }
          min_distance: 20.0
          max_distance: 800.0
      children: []

    - name: sun
      active: true
      components:
""" + transform((0, 0, 50)) + """        - type: DirectionalLight
          direction: { x: -0.35, y: 0.4, z: -0.85 }
          color: { r: 1.0, g: 0.96, b: 0.9 }
          intensity: 1.0
          cast_shadows: true
      children: []
""")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as f:
        f.write("".join(out))
    print("wrote", os.path.relpath(OUT, ASSETS), "--", rocks, "rocks,", idx, "mobs")


if __name__ == "__main__":
    main()
