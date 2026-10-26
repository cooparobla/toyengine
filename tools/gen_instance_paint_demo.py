#!/usr/bin/env python3
"""Generates the instance painter's demo scene, its object asset and its painted instances.

Run from the repo root:

    python3 tools/gen_instance_paint_demo.py

It writes four things:

* ``assets/objects/nature/pine.yaml`` -- a two-part object asset (trunk plus a canopy child,
  with a capsule collider). Two parts on purpose: a prototype that is an object asset must draw
  every MeshRenderer under it, each with its own local transform, and one mesh would not prove it.
* ``assets/scenes/terrain/instance_paint_demo/terrain/height.png`` -- the terrain's 16-bit
  height map, written here rather than sculpted so this script knows the ground height exactly
  and can place instances ON it.
* ``assets/scenes/terrain/instance_paint_demo/instances/scattered.instances`` -- the painted
  records, in the binary format ``toyengine/instance/instance_set.h`` reads.
* ``assets/scenes/terrain/instance_paint_demo/scene.yaml`` -- the scene itself.

The point of pre-painting here is that the demo has to *ship* with instances: a painter with an
empty set proves nothing about the renderer, the collision streaming or the file format. What a
person then does in Instance Paint Mode is add to it.
"""

import math
import pathlib
import struct
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCENE_DIR = ROOT / "assets" / "scenes" / "terrain" / "instance_paint_demo"

# --- the terrain ---------------------------------------------------------------------------

TERRAIN_SIZE = 256.0      # metres a side
TERRAIN_HEIGHT = 40.0     # metres at a full-white height sample
RESOLUTION = 257          # height samples a side (2^n + 1)

# Where the terrain's (0, 0) corner sits in the world, so the demo is centred on the origin.
ORIGIN = (-TERRAIN_SIZE / 2.0, -TERRAIN_SIZE / 2.0, 0.0)


def height_at(x: float, y: float) -> float:
    """Ground height in metres at terrain-local (x, y). The scene's one source of truth.

    A broad hill with a flat shelf cut into its east side, plus gentle ripples: enough relief to
    show instances following a slope, and a flat area where a grid-snapped fence reads clearly.
    """
    u = x / TERRAIN_SIZE
    v = y / TERRAIN_SIZE
    # The hill: a smooth bump centred a little south-west of the middle.
    dx, dy = u - 0.42, v - 0.46
    r = math.sqrt(dx * dx + dy * dy)
    hill = 20.0 * math.exp(-(r * 3.1) ** 2)
    # Ripples, so no two trees sit at quite the same height.
    ripple = 1.1 * math.sin(u * 17.0) * math.cos(v * 13.0)
    h = hill + ripple + 3.0
    # The shelf: a flat terrace to the east, blended in over four metres.
    shelf_h = 6.0
    t = _smoothstep(0.62, 0.70, u)
    return h * (1.0 - t) + shelf_h * t


def _smoothstep(a: float, b: float, x: float) -> float:
    t = max(0.0, min(1.0, (x - a) / (b - a)))
    return t * t * (3.0 - 2.0 * t)


def normal_at(x: float, y: float) -> tuple:
    """Unit surface normal at terrain-local (x, y), by central differences."""
    e = 0.5
    hx = height_at(x + e, y) - height_at(x - e, y)
    hy = height_at(x, y + e) - height_at(x, y - e)
    nx, ny, nz = -hx / (2.0 * e), -hy / (2.0 * e), 1.0
    inv = 1.0 / math.sqrt(nx * nx + ny * ny + nz * nz)
    return (nx * inv, ny * inv, nz * inv)


def write_png16_grey(path: pathlib.Path, samples, width: int, height: int) -> None:
    """A 16-bit greyscale PNG, the format toyengine/world/png16.h reads."""
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    raw = bytearray()
    for j in range(height):
        raw.append(0)   # filter: none
        row = samples[j * width:(j + 1) * width]
        raw += struct.pack(">%dH" % width, *row)
    ihdr = struct.pack(">IIBBBBB", width, height, 16, 0, 0, 0, 0)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
                     chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def write_heightmap() -> None:
    samples = []
    for j in range(RESOLUTION):
        for i in range(RESOLUTION):
            x = TERRAIN_SIZE * i / (RESOLUTION - 1)
            y = TERRAIN_SIZE * j / (RESOLUTION - 1)
            h = max(0.0, min(TERRAIN_HEIGHT, height_at(x, y)))
            samples.append(int(round(h / TERRAIN_HEIGHT * 65535.0)))
    write_png16_grey(SCENE_DIR / "terrain" / "height.png", samples, RESOLUTION, RESOLUTION)


# --- the painted instances ------------------------------------------------------------------

CHUNK_SIZE = 32.0
RECORD_BYTES = 48

PROTO_PINE, PROTO_BOULDER, PROTO_GRASS, PROTO_SLAB = 0, 1, 2, 3


class Rng:
    """A tiny LCG, so re-running this script reproduces the same world exactly."""

    def __init__(self, seed: int) -> None:
        self.state = seed & 0xFFFFFFFF

    def next_u32(self) -> int:
        self.state = (self.state * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.state

    def unit(self) -> float:
        return self.next_u32() / 4294967296.0

    def between(self, a: float, b: float) -> float:
        return a + (b - a) * self.unit()


def quat_from_yaw_and_normal(yaw: float, normal: tuple, align: float) -> tuple:
    """The rotation make_record() builds: a yaw about +Z, tilted onto the normal by `align`."""
    # Tilt: the shortest rotation from +Z to the normal, slerped from identity by `align`.
    nx, ny, nz = normal
    axis = (-ny, nx, 0.0)                       # +Z cross normal
    axis_len = math.sqrt(axis[0] ** 2 + axis[1] ** 2)
    angle = math.acos(max(-1.0, min(1.0, nz))) * align
    if axis_len < 1e-6 or abs(angle) < 1e-6:
        tilt = (0.0, 0.0, 0.0, 1.0)
    else:
        s = math.sin(angle * 0.5)
        tilt = (axis[0] / axis_len * s, axis[1] / axis_len * s, 0.0, math.cos(angle * 0.5))
    spin = (0.0, 0.0, math.sin(yaw * 0.5), math.cos(yaw * 0.5))
    return _quat_mul(tilt, spin)


def _quat_mul(a: tuple, b: tuple) -> tuple:
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def record(x: float, y: float, proto: int, scale: float, yaw: float, align: float,
           sink: float, seed: int) -> bytes:
    """One InstanceRecord: world position on the terrain, rotation, scale, prototype, seed."""
    local_x, local_y = x - ORIGIN[0], y - ORIGIN[1]
    n = normal_at(local_x, local_y)
    z = ORIGIN[2] + height_at(local_x, local_y) - sink * scale
    q = quat_from_yaw_and_normal(yaw, n, align)
    return struct.pack("<3f4f3fHHI", x, y, z, q[0], q[1], q[2], q[3],
                       scale, scale, scale, proto, 0, seed & 0xFFFFFFFF)


def paint() -> dict:
    """Every instance the demo ships with, grouped into the chunks that hold them."""
    rng = Rng(20261026)
    records = []

    # A copse of pines over the hill's north flank, thinning as the ground steepens.
    for i in range(170):
        x = rng.between(-110.0, 20.0)
        y = rng.between(-40.0, 95.0)
        nz = normal_at(x - ORIGIN[0], y - ORIGIN[1])[2]
        if nz < 0.86:                       # too steep for a tree to have taken
            continue
        records.append(record(x, y, PROTO_PINE, rng.between(0.85, 1.45),
                              rng.between(0.0, math.tau), 0.15, 0.25, rng.next_u32()))

    # Boulders, which lie back on the slope rather than standing up on it.
    for i in range(55):
        x = rng.between(-120.0, 60.0)
        y = rng.between(-110.0, 100.0)
        records.append(record(x, y, PROTO_BOULDER, rng.between(0.7, 2.4),
                              rng.between(0.0, math.tau), 0.75, 0.3, rng.next_u32()))

    # Grass, dense and collision-free, over the gentler ground.
    for i in range(1400):
        x = rng.between(-125.0, 100.0)
        y = rng.between(-125.0, 110.0)
        nz = normal_at(x - ORIGIN[0], y - ORIGIN[1])[2]
        if nz < 0.9:
            continue
        records.append(record(x, y, PROTO_GRASS, rng.between(0.6, 1.2),
                              rng.between(0.0, math.tau), 0.6, 0.05, rng.next_u32()))

    # A paved path across the eastern shelf, turning a corner: slabs grid-snapped to 2 m cells
    # with yaw quantised to 90 degrees. This is what Snap does in the editor, and it is here
    # beside the organic scatter above because the two are what the painter is for -- the same
    # brush lays a kit on a grid and scatters a forest.
    for i in range(24):
        x = 63.0
        y = -24.0 + 2.0 * i
        records.append(record(x, y, PROTO_SLAB, 1.0, 0.0, 0.0, 0.45, 0x1000 + i))
    for i in range(12):
        x = 63.0 - 2.0 * (i + 1)
        y = -24.0
        records.append(record(x, y, PROTO_SLAB, 1.0, math.pi * 0.5, 0.0, 0.45, 0x2000 + i))

    chunks = {}
    for r in records:
        x, y = struct.unpack_from("<2f", r, 0)
        key = (math.floor(x / CHUNK_SIZE), math.floor(y / CHUNK_SIZE))
        chunks.setdefault(key, []).append(r)
    return chunks


def write_instances(path: pathlib.Path, chunks: dict) -> int:
    """The `.instances` file: a header, a chunk index, then one deflated block per chunk.

    Records are packed FIELD-MAJOR inside a block (all the first bytes, then all the second...)
    because that is what lets deflate see the redundancy between neighbouring instances. The
    engine's reader expects exactly this.
    """
    index, blobs, offset = [], [], 0
    for (cx, cy) in sorted(chunks.keys(), key=lambda k: (k[1], k[0])):
        recs = chunks[(cx, cy)]
        plane = bytearray()
        for field in range(RECORD_BYTES):
            for r in recs:
                plane.append(r[field])
        blob = zlib.compress(bytes(plane), 8)
        index.append((cx, cy, len(recs), offset, len(blob)))
        blobs.append(blob)
        offset += len(blob)

    out = bytearray(b"TINS")
    out += struct.pack("<I", 1)                 # version
    out += struct.pack("<f", CHUNK_SIZE)
    out += struct.pack("<I", RECORD_BYTES)
    out += struct.pack("<I", len(index))
    for cx, cy, count, off, length in index:
        out += struct.pack("<iiIII", cx, cy, count, off, length)
    for blob in blobs:
        out += blob
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(out))
    return sum(len(v) for v in chunks.values())


# --- the object asset ----------------------------------------------------------------------

PINE_OBJECT = """\
# pine: a trunk and its canopy, as one object asset -- the thing an InstancePainter paints.
#
# TWO MeshRenderers on purpose. A painter resolves an `object:` prototype by walking it for every
# MeshRenderer and keeping each one's local transform, so a prototype with parts proves that path
# works; a single-mesh rock would not. The capsule is what `collision: auto` fits to a shape
# taller than it is wide, and what the painter's streamed colliders are built from.
#
# Generated by tools/gen_instance_paint_demo.py.
object:
  name: pine
  components:
    - type: Transform
      position: {x: 0, y: 0, z: 0}
    - type: MeshRenderer
      mesh_path: nature/pine_trunk
      material:
        albedo: {r: 0.33, g: 0.24, b: 0.17}
        roughness: 0.9
    - type: CapsuleCollider
      radius: 0.35
      height: 7.0
      center: {x: 0, y: 0, z: 3.5}
  children:
    - name: canopy
      components:
        - type: Transform
          position: {x: 0, y: 0, z: 0}
        - type: MeshRenderer
          mesh_path: nature/pine_canopy
          material:
            albedo: {r: 0.16, g: 0.3, b: 0.14}
            roughness: 0.85
      children: []
"""

# --- the scene -----------------------------------------------------------------------------
#
# Written in the EDITOR'S canonical form -- keys in the saver's order, no comments -- because
# tests/editor/unit/asset_roundtrip_test.cpp holds every scene in the repository to loading and
# saving byte-for-byte unchanged, and the editor's saver sorts keys and drops comments. What the
# scene is for is documented in toyengine/instance/README.md and in this script's docstring
# rather than inside the file, where the first Ctrl S would delete it.
#
# What it shows (toyengine/instance/, editor/app/ui/instance_paint.h):
#   * ~1700 instances of four prototypes over a 256 m hill, all of them RECORDS in
#     instances/scattered.instances rather than objects in the scene file -- which is the whole
#     point: a world's worth of props costs nothing there.
#   * an `object:` prototype with two parts (objects/nature/pine: a trunk and its canopy) beside
#     three bare `mesh:` ones, so both prototype kinds are exercised. An `object:` reference is
#     spelled the way `prefab:` is, with the asset folder.
#   * collision streamed per prototype: pines collide as authored capsules and boulders as boxes
#     within 80 m of the camera, while 1400 grass tufts cost nothing at all. Walk into a trunk.
#   * a grid-snapped paved path across the eastern shelf, next to the organic scatter, because
#     Snap is what makes the painter useful for kits and walls rather than only for foliage.
#   * two plain mesh objects (a plinth and a ramp) which are ALSO paintable: the brush raycasts
#     every surface in the scene, not just the terrain.

SCENE = """\
scene:
  scene_name: instance_paint_demo
  settings:
    weather:
      condition: clear
      drive_sun: true
      enabled: true
      ground_height: 0.0
      latitude: 45
      schedule: fixed
      sun_intensity: 1.2
      time_of_day: 9.0
  root_objects:
    - name: terrain
      components:
        - type: Transform
          position: { x: -128, y: -128, z: 0 }
        - type: HeightmapTerrain
          cast_shadows: true
          chunk_size: 32
          collision: true
          height: 40
          heightmap: terrain/height.png
          layers:
            - name: grass
              albedo: textures/terrain/layers/grass_albedo.png
              normal: textures/terrain/layers/grass_normal.png
              roughness: 0.9
              tiling: 4
          resolution: 257
          size: { x: 256, y: 256 }
        - { type: WeatherSurface }
      children: []
    - name: scattered
      components:
        - type: Transform
          position: { x: 0, y: 0, z: 0 }
        - type: InstancePainter
          cast_shadows: true
          chunk_size: 32
          collision_radius: 80
          data: instances/scattered.instances
          prototypes:
            - name: pine
              align_to_normal: 0.15
              collision: capsule
              collision_radius: 0.45
              object: objects/nature/pine
              scale_range: [0.85, 1.45]
              sink: 0.25
              yaw_range: [0, 360]
            - name: boulder
              align_to_normal: 0.75
              collision: box
              material:
                albedo: { b: 0.43, g: 0.46, r: 0.48 }
                roughness: 0.8
              mesh: nature/boulder
              scale_range: [0.7, 2.4]
              sink: 0.3
            - name: grass
              align_to_normal: 0.6
              cast_shadows: false
              collision: none
              draw_distance: 70
              fade_start: 0.5
              material:
                albedo: { b: 0.75, g: 1.0, r: 0.85 }
                alpha_cutoff: 0.4
                alpha_mode: CUTOUT
                roughness: 0.8
                texture_albedo: textures/terrain/layers/grass_blades.png
                texture_alpha_mask: textures/terrain/layers/grass_blades.png
              mesh: nature/grass_tuft
              scale_range: [0.6, 1.2]
              sink: 0.05
            - name: slab
              align_to_normal: 0
              collision: box
              material:
                albedo: { b: 0.52, g: 0.58, r: 0.62 }
                roughness: 0.9
              mesh: primitives/cube
              scale_range: [1.0, 1.0]
              sink: 0.45
              yaw_range: [0, 0]
      children: []
    - name: plinth
      components:
        - type: Transform
          position: { x: 30, y: -60, z: 7 }
          scale: { x: 12, y: 12, z: 1.5 }
        - type: MeshRenderer
          material:
            albedo: { b: 0.5, g: 0.52, r: 0.55 }
            roughness: 0.7
          mesh_path: primitives/cube
        - type: BoxCollider
          size: { x: 1, y: 1, z: 1 }
      children: []
    - name: ramp
      components:
        - type: Transform
          position: { x: 10, y: -60, z: 5 }
          rotation: { x: 0, y: -22, z: 0 }
          scale: { x: 14, y: 10, z: 0.6 }
        - type: MeshRenderer
          material:
            albedo: { b: 0.45, g: 0.47, r: 0.5 }
            roughness: 0.75
          mesh_path: primitives/cube
        - type: BoxCollider
          size: { x: 1, y: 1, z: 1 }
      children: []
    - name: player
      prefab: objects/characters/mannequin
      components:
        - type: Transform
          position: { x: 40, y: 0, z: 20 }
          rotation: { x: 0, y: 0, z: 180 }
        - type: CharacterController
          height: 1.8
          jump_height: 1.2
          move_speed: 3.0
          radius: 0.3
          slope_limit: 45.0
          sprint_multiplier: 3.0
          step_height: 0.35
          use_root_motion: true
        - { type: Animator, apply_root_motion: true }
        - { type: CharacterAnimDriver, crossfade: 0.2, run_speed: 4.0, walk_speed: 0.2 }
    - name: camera
      components:
        - type: Transform
          position: { x: 40, y: -14, z: 26 }
          rotation: { x: 68, y: 0, z: 0 }
        - type: Camera
          far_clip_plane: 1200
          fov: 60
          main: true
          near_clip_plane: 0.1
          projection: Perspective
        - type: CameraController
          collide: true
          collision_radius: 0.25
          distance: 14
          follow_smoothing: 12.0
          max_distance: 60
          min_distance: 2
          mode: orbit
          pitch_deg: 20
          target_offset: { x: 0, y: 0, z: 1.4 }
          tracker: player
          yaw_deg: 180
      children: []
    - name: sun
      components:
        - type: Transform
          position: { x: 0, y: 0, z: 200 }
        - type: DirectionalLight
          cast_shadows: true
          color: { b: 0.9, g: 0.96, r: 1.0 }
          direction: { x: -0.45, y: 0.55, z: -0.6 }
          intensity: 1.0
      children: []
"""


def main() -> None:
    write_heightmap()
    n = write_instances(SCENE_DIR / "instances" / "scattered.instances", paint())
    (ROOT / "assets" / "objects" / "nature").mkdir(parents=True, exist_ok=True)
    (ROOT / "assets" / "objects" / "nature" / "pine.yaml").write_text(PINE_OBJECT)
    SCENE_DIR.mkdir(parents=True, exist_ok=True)
    (SCENE_DIR / "scene.yaml").write_text(SCENE)
    print("wrote %s" % (SCENE_DIR / "scene.yaml"))
    print("wrote %s (%d instances)" % (SCENE_DIR / "instances" / "scattered.instances", n))
    print("wrote %s" % (SCENE_DIR / "terrain" / "height.png"))
    print("wrote %s" % (ROOT / "assets" / "objects" / "nature" / "pine.yaml"))


if __name__ == "__main__":
    main()
