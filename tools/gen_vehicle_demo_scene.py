"""Generates assets/scenes/physics/vehicle_demo/scene.yaml -- the driving course.

The course is laid out RADIALLY around a central depot, with each station on its own surface, so
the whole thing reads in one glance from the demo's top-down camera (pitch 55 degrees, the rig
habitat and cubeworld share). Driving it is the acceptance test for physxcoopa/vehicle/: every
station exists to separate the six vehicles from each other.

    NORTH  tarmac skidpad, slalom, jump ramp     -- grip, steering, air
    EAST   gravel straight, ford, deep mud pit   -- surface grip and rolling resistance
    WEST   kerbs and ledges, 0.12 m to 0.45 m    -- suspension travel and ground clearance
    SOUTH  12 / 22 / 32 degree climbs            -- torque, gearing and differentials
    S-E    seesaw bridge, crate yard             -- a moving surface and loose obstacles

The ground is a TILED floor: adjacent boxes whose top faces all sit at exactly z = 0, each with
its own physics material. Laying surfaces as slabs on top of a shared floor instead would leave a
lip at every patch edge, and a wheel raycast landing on two coincident surfaces has no defined
winner. One apron tile 1 cm lower catches everything outside the course, so nothing can drive off
into the void.

Run from the repository root:

    python3 tools/gen_vehicle_demo_scene.py
"""

import math
import pathlib

OUT = pathlib.Path(__file__).parent.parent / "assets" / "scenes" / "physics" / "vehicle_demo"

# Surface colours, chosen so the materials are told apart from directly above -- which is the only
# view the demo's camera offers.
SURFACES = {
    "tarmac": (0.20, 0.21, 0.23),
    "gravel": (0.52, 0.49, 0.43),
    "mud": (0.26, 0.19, 0.12),
    "snow": (0.88, 0.90, 0.94),
}


def vec(x, y, z):
    return f"{{ x: {x:.3f}, y: {y:.3f}, z: {z:.3f} }}"


def rgb(c):
    return f"{{ r: {c[0]:.3f}, g: {c[1]:.3f}, b: {c[2]:.3f} }}"


def obj(name, body, children="", indent="    "):
    """One scene object, with `body` already indented for the components list."""
    kids = children if children else " []"
    if children:
        kids = "\n" + children
    return (f"{indent}- name: {name}\n"
            f"{indent}  active: true\n"
            f"{indent}  components:\n{body}"
            f"{indent}  children:{kids}")


def surface_tile(name, x0, x1, y0, y1, surface, top=0.0, thickness=1.0, roughness=0.9):
    """A floor slab whose TOP face sits at `top`, with the physics material of `surface`."""
    w, d = x1 - x0, y1 - y0
    cx, cy = (x0 + x1) * 0.5, (y0 + y1) * 0.5
    colour = SURFACES[surface]
    body = (f"        - type: Transform\n"
            f"          position: {vec(cx, cy, top - thickness * 0.5)}\n"
            f"          scale: {vec(w, d, thickness)}\n"
            f"        - type: MeshRenderer\n"
            f"          mesh_path: cube\n"
            f"          material: {{ albedo: {rgb(colour)}, metallic: 0.0, roughness: {roughness}, ao: 1.0 }}\n"
            f"        - type: BoxCollider\n"
            f"          size: {vec(1.0, 1.0, 1.0)}\n"
            f"          material: {surface}\n")
    return obj(name, body)


def static_box(name, center, size, colour, material="concrete", rotation=None, mesh="cube",
               roughness=0.85):
    """A static obstacle: render mesh plus a matching BoxCollider, no Rigidbody."""
    rot = f"          rotation: {vec(*rotation)}\n" if rotation else ""
    body = (f"        - type: Transform\n"
            f"          position: {vec(*center)}\n"
            f"{rot}"
            f"          scale: {vec(*size)}\n"
            f"        - type: MeshRenderer\n"
            f"          mesh_path: {mesh}\n"
            f"          material: {{ albedo: {rgb(colour)}, metallic: 0.0, roughness: {roughness}, ao: 1.0 }}\n"
            f"        - type: BoxCollider\n"
            f"          size: {vec(1.0, 1.0, 1.0)}\n"
            f"          material: {material}\n")
    return obj(name, body)


def dynamic_box(name, center, size, colour, mass, material="wood", angular_drag=0.05):
    """A loose obstacle: the same as static_box plus a Rigidbody."""
    body = (f"        - type: Transform\n"
            f"          position: {vec(*center)}\n"
            f"          scale: {vec(*size)}\n"
            f"        - type: MeshRenderer\n"
            f"          mesh_path: cube\n"
            f"          material: {{ albedo: {rgb(colour)}, metallic: 0.0, roughness: 0.8, ao: 1.0 }}\n"
            f"        - type: BoxCollider\n"
            f"          size: {vec(1.0, 1.0, 1.0)}\n"
            f"          material: {material}\n"
            f"        - type: Rigidbody\n"
            f"          mass: {mass}\n"
            f"          angular_drag: {angular_drag}\n")
    return obj(name, body)


def ramp(name, cx, cy, half_width, half_length, angle_deg, colour, material="tarmac",
         thickness=0.25):
    """A ramp rising toward +Y, with its low end's top face exactly at ground level.

    Rotating a slab about +X by `angle_deg` lifts its +Y end. Its low corner would then sink or
    float unless the centre height compensates, which is what the z below does: the top face at
    local (0, -half_length, +thickness) lands at exactly z = 0.

    Returns:
        tuple: (yaml, crest_height, crest_y) -- so the caller can put a plateau at the top.
    """
    a = math.radians(angle_deg)
    cz = half_length * math.sin(a) - thickness * math.cos(a)
    crest = 2.0 * half_length * math.sin(a)
    crest_y = cy + half_length * math.cos(a)
    yaml = static_box(name, (cx, cy, cz), (half_width * 2, half_length * 2, thickness * 2), colour,
                      material, rotation=(angle_deg, 0.0, 0.0))
    return yaml, crest, crest_y


def main():
    """Writes the scene."""
    OUT.mkdir(parents=True, exist_ok=True)
    parts = []

    # --- Floor -------------------------------------------------------------------------------
    parts.append(surface_tile("apron", -120, 120, -90, 110, "gravel", top=-0.01, thickness=2.0))
    parts.append(surface_tile("depot_pad", -20, 20, -15, 15, "tarmac", roughness=0.8))
    parts.append(surface_tile("skidpad", -36, 36, 15, 76, "tarmac", roughness=0.8))
    parts.append(surface_tile("gravel_straight", 20, 60, -14, 14, "gravel"))
    parts.append(surface_tile("ford_bed", 60, 74, -14, 14, "tarmac", roughness=0.7))
    parts.append(surface_tile("mud_pit", 74, 108, -20, 20, "mud", roughness=0.95))
    parts.append(surface_tile("rock_yard", -60, -20, -15, 15, "gravel"))
    parts.append(surface_tile("hill_base", -20, 20, -50, -15, "tarmac", roughness=0.8))
    parts.append(surface_tile("snow_patch", -56, -20, -50, -16, "snow", roughness=0.75))
    parts.append(surface_tile("bridge_yard", 20, 58, -50, -16, "gravel"))

    # --- NORTH: slalom and a jump ------------------------------------------------------------
    # Knock-over-able, not static. A 9-tonne machine stopping dead against an immovable 0.5 m post
    # is not a slalom, it is a wall -- and a static cone line across the skidpad turns the fastest
    # part of the course into a crash barrier. 8 kg each, so they scatter and can be replaced by
    # reloading the scene.
    for i in range(8):
        y = 24.0 + i * 6.0
        x = 7.0 if i % 2 == 0 else -7.0
        parts.append(dynamic_box(f"cone_{i}", (x, y, 0.45), (0.55, 0.55, 0.9),
                                 (0.86, 0.33, 0.07), 8.0, "rubber"))
    jump, jump_crest, jump_crest_y = ramp("jump_ramp", 0.0, 64.0, 4.0, 4.0, 18.0,
                                          (0.38, 0.36, 0.34))
    parts.append(jump)
    parts.append(static_box("jump_marker", (0.0, jump_crest_y + 9.0, 0.06),
                            (9.0, 14.0, 0.12), (0.92, 0.88, 0.25), "tarmac"))

    # --- EAST: the ford, then the mud pit ----------------------------------------------------
    water = (f"        - type: Transform\n"
             f"          position: {vec(67.0, 0.0, 0.26)}\n"
             f"        - type: MeshRenderer\n"
             f"          affects_reflection_probes: false\n"
             f"          material:\n"
             f"            albedo: {{ r: 0.05, g: 0.14, b: 0.17 }}\n"
             f"            metallic: 0.0\n"
             f"            roughness: 0.06\n"
             f"            ao: 1.0\n"
             f"            alpha: 0.45\n"
             f"            alpha_mode: BLEND\n"
             f"            refraction: true\n"
             f"            ior: 1.33\n"
             f"            refraction_tint: {{ r: 0.42, g: 0.70, b: 0.74 }}\n"
             f"        - type: WaterBody\n"
             f"          mode: planar\n"
             f"          size: {{ x: 14.0, y: 28.0 }}\n"
             f"          resolution: 48\n"
             f"          wave_amplitude: 0.04\n"
             f"          wave_length: 3.0\n"
             f"          clarity: 1.2\n"
             f"          shore_foam_depth: 0.35\n"
             f"          edge_fade_depth: 0.1\n")
    # A shallow ford: 26 cm of water over a hard bed, so a vehicle drives THROUGH it. None of the
    # vehicles carries a Buoyancy component, so the water is a surface effect here, not a float.
    parts.append(obj("ford_water", water))
    for i, x in enumerate((78.0, 88.0, 98.0)):
        parts.append(static_box(f"mud_marker_{i}", (x, 18.5, 0.6), (0.4, 0.4, 1.2),
                                (0.9, 0.85, 0.2), "wood"))

    # --- WEST: kerbs and ledges of rising height ---------------------------------------------
    for i, h in enumerate((0.12, 0.22, 0.32, 0.45)):
        x = -26.0 - i * 7.0
        parts.append(static_box(f"kerb_{i}", (x, 0.0, h * 0.5), (1.4, 16.0, h),
                                (0.46, 0.44, 0.42), "concrete"))
    # A pair of logs to straddle, and a lone 0.4 m step with nothing to run up on.
    for i, y in enumerate((-9.0, -11.5)):
        parts.append(static_box(f"log_{i}", (-40.0, y, 0.28), (14.0, 0.56, 0.56),
                                (0.33, 0.24, 0.15), "wood"))
    parts.append(static_box("ledge", (-52.0, 8.0, 0.20), (12.0, 6.0, 0.40),
                            (0.40, 0.38, 0.36), "concrete"))

    # --- SOUTH: three graded climbs ----------------------------------------------------------
    for i, (angle, x) in enumerate(((12.0, -13.0), (22.0, 0.0), (32.0, 13.0))):
        # Each ramp faces -Y (driven at from the depot), so it is generated rising toward +Y and
        # then placed with its low end nearest the depot.
        yaml, crest, crest_y = ramp(f"climb_{int(angle)}", x, -34.0, 4.5, 5.0, angle,
                                    (0.24, 0.25, 0.27))
        parts.append(yaml)
        parts.append(static_box(f"climb_{int(angle)}_plateau", (x, crest_y + 3.0, crest - 0.25),
                                (9.0, 6.0, 0.5), (0.30, 0.42, 0.24), "tarmac"))

    # --- SOUTH-EAST: the seesaw bridge and a crate yard --------------------------------------
    parts.append(static_box("seesaw_post", (34.0, -32.0, 0.35), (3.4, 1.0, 0.7),
                            (0.34, 0.33, 0.32), "concrete"))
    seesaw = (f"        - type: Transform\n"
              f"          position: {vec(34.0, -32.0, 0.82)}\n"
              f"        - type: MeshRenderer\n"
              f"          mesh_path: cube\n"
              f"          scale: {vec(1.0, 1.0, 1.0)}\n"
              f"          material: {{ albedo: {rgb((0.42, 0.30, 0.18))}, metallic: 0.0, roughness: 0.8, ao: 1.0 }}\n"
              f"        - type: BoxCollider\n"
              f"          size: {vec(1.0, 1.0, 1.0)}\n"
              f"          material: wood\n"
              f"        - type: Rigidbody\n"
              f"          mass: 600.0\n"
              f"        - type: HingeJoint\n"
              f"          anchor: {vec(0.0, 0.0, 0.0)}\n"
              f"          axis: {vec(1.0, 0.0, 0.0)}\n"
              # `use_limits` + `min_angle`/`max_angle` is the spelling physx_yaml.h reads (and the
              # editor writes); `limits: {min, max}` is the other accepted form. A `limit_min` key
              # would simply be ignored and the plank would tip until it fell off its post.
              f"          use_limits: true\n"
              f"          min_angle: -14.0\n"
              f"          max_angle: 14.0\n")
    # The plank's Transform carries the scale, so the BoxCollider's unit size picks it up.
    seesaw = seesaw.replace(f"          scale: {vec(1.0, 1.0, 1.0)}\n", "")
    seesaw = seesaw.replace(f"          position: {vec(34.0, -32.0, 0.82)}\n",
                            f"          position: {vec(34.0, -32.0, 0.82)}\n"
                            f"          scale: {vec(4.5, 14.0, 0.24)}\n")
    parts.append(obj("seesaw_plank", seesaw))

    for i in range(6):
        parts.append(dynamic_box(f"crate_{i}", (48.0 + (i % 3) * 1.4, -44.0 + (i // 3) * 1.4,
                                                0.5 + (i // 3) * 1.05),
                                 (1.0, 1.0, 1.0), (0.55, 0.42, 0.24), 60.0))

    # --- The lineup --------------------------------------------------------------------------
    # Parked in a row across the depot, in preset order. The pickup starts active because it is
    # the forgiving one; V cycles to the next. The trailer sits behind the hauler, hitched to it.
    lineup = [
        ("sports_car", -14.0, 0.81),
        ("pickup_4x4", -7.0, 1.04),
        ("hauler_6x6", 0.0, 1.58),
        ("buggy", 8.0, 1.09),
        ("dozer", 15.0, 1.17),
    ]
    for name, x, z in lineup:
        active = "true" if name == "pickup_4x4" else "false"
        body = (f"        - type: Transform\n"
                f"          position: {vec(x, -8.0, z)}\n"
                f"        - type: Vehicle\n"
                f"          active: {active}\n")
        # No `children:` key: on a prefab instance that list means child OVERRIDES, and the
        # editor drops an empty one when it saves -- which would make this scene fail
        # tests/editor/unit/asset_roundtrip_test.cpp's load-save-compare.
        parts.append(f"    - name: {name}\n"
                     f"      active: true\n"
                     f"      prefab: objects/vehicles/{name}\n"
                     f"      components:\n{body}".rstrip("\n"))

    # The trailer, hitched to the hauler's tail by a hinge about the vertical axis. Its anchor is
    # in the TRAILER's local frame, at the end of its drawbar.
    # Parked so its drawbar tip already reaches the hauler's tail (half-length 3.40 from y = -8,
    # so the tail is at y = -11.40, and the hitch anchor sits 2.50 ahead of the trailer's origin).
    # Spawning it slack makes the joint yank both bodies together on the first step.
    trailer = (f"        - type: Transform\n"
               f"          position: {vec(0.0, -13.9, 1.00)}\n"
               f"        - type: HingeJoint\n"
               f"          connected_object: hauler_6x6\n"
               f"          anchor: {vec(0.0, 2.50, -0.30)}\n"
               f"          axis: {vec(0.0, 0.0, 1.0)}\n"
               f"          use_limits: true\n"
               f"          min_angle: -80.0\n"
               f"          max_angle: 80.0\n")
    parts.append(f"    - name: tipper_trailer\n"
                 f"      active: true\n"
                 f"      prefab: objects/vehicles/trailer\n"
                 f"      components:\n{trailer}".rstrip("\n"))

    # --- Camera and sun ----------------------------------------------------------------------
    # The top-down ARPG rig habitat and cubeworld share: steeply down, a limited pitch band the
    # player can tilt within, right mouse to orbit, and a long zoom range so the whole course can
    # be seen at once. `tracker` is rewritten by Engine::drive_vehicles_() when V cycles vehicles.
    camera = (f"        - type: Transform\n"
              f"          position: {vec(0.0, -30.0, 22.0)}\n"
              f"          rotation: {vec(55.0, 0.0, 0.0)}\n"
              f"        - type: Camera\n"
              f"          main: true\n"
              f"          projection: Perspective\n"
              f"          fov: 60.0\n"
              f"          near_clip_plane: 0.3\n"
              f"          far_clip_plane: 1000.0\n"
              f"        - type: CameraController\n"
              f"          mode: orbit\n"
              f"          tracker: pickup_4x4\n"
              f"          capture_cursor: false\n"
              f"          look_button: right\n"
              f"          target_offset: {vec(0.0, 0.0, 1.0)}\n"
              f"          follow_smoothing: 5.0\n"
              f"          movement_smoothing: 0.25\n"
              f"          mouse_sensitivity: 0.12\n"
              f"          distance: 24.0\n"
              f"          yaw_deg: 0.0\n"
              f"          pitch_deg: 55.0\n"
              f"          min_pitch_deg: 44.0\n"
              f"          max_pitch_deg: 66.0\n"
              f"          min_distance: 5.0\n"
              f"          max_distance: 90.0\n"
              f"          zoom_speed: 2.0\n"
              f"          collide: true\n"
              f"          collision_radius: 0.25\n")
    parts.append(obj("camera", camera))

    sun = (f"        - type: Transform\n"
           f"          position: {vec(0.0, 0.0, 0.0)}\n"
           f"        - type: DirectionalLight\n"
           f"          direction: {vec(-0.38, -0.42, -0.82)}\n"
           f"          color: {{ r: 1.0, g: 0.97, b: 0.9 }}\n"
           f"          intensity: 1.0\n"
           f"          cast_shadows: true\n")
    parts.append(obj("sun", sun))

    header = f'''# vehicle_demo -- the driving course for physxcoopa/vehicle/.
#
# GENERATED by tools/gen_vehicle_demo_scene.py. Edit the generator, not this file.
#
# Controls: W/S throttle and brake, A/D steer, Space handbrake, Shift/Ctrl manual gears, V to
# cycle vehicles, R to right one that has ended up on its roof (it floats clear, turns smoothly
# back upright about the heading it had, and lands on its suspension). Steering is in the
# VEHICLE's frame, not the camera's, so orbiting the top-down camera never inverts it. Hold the
# right mouse button to look around; the wheel zooms.
#
# The lineup, parked across the depot west to east, and what each one is here to show:
#
#   sports_car   1300 kg  RWD   0.13 m travel  -- fast on the skidpad, stopped by a 0.22 m kerb
#   pickup_4x4   2100 kg  AWD   0.26 m travel  -- crosses gravel, bogs in the deep mud
#   hauler_6x6   9000 kg  6WD   0.40 m travel  -- crosses the mud and steps over the 0.45 m kerb
#   buggy         700 kg  AWD   0.42 m travel  -- light enough to skate over mud; lands the jump
#   dozer       12000 kg  track 0.12 m travel  -- pivots on the spot, holds the 32 degree climb
#   tipper_trailer        towed on a hinge behind the hauler
#
# Stations, radially from the depot (see the generator's doc for why the ground is tiled):
#
#   NORTH  skidpad, 8-cone slalom, 18 degree jump ramp
#   EAST   gravel straight -> 26 cm ford -> deep mud pit
#   WEST   kerbs at 0.12 / 0.22 / 0.32 / 0.45 m, two logs, a 0.4 m ledge
#   SOUTH  12 / 22 / 32 degree climbs, each with a plateau to prove you made it
#   S-E    a hinged seesaw bridge and a crate stack to shove
scene:
  scene_name: vehicle_demo
  settings:
    render:
      # The shipped config's fog is tuned for a scene tens of metres across; this course is 200 m
      # corner to corner, and at 0.008/m the far stations wash out to flat sky before you can see
      # what surface they are. A quarter of that keeps the depth cue and lets the mud, gravel and
      # tarmac still be told apart from the far end of the skidpad.
      fog_density: 0.002
    physics:
      solver:
        # Nothing on this course is allowed to fall asleep. physxcoopa does not WAKE a sleeping
        # body when something new comes into contact with it (dynamics/solver.h's sleep pass skips
        # any body that is already asleep), so a settled 8 kg traffic cone behaves exactly like a
        # static post -- a truck hits it at 16 m/s and stops dead while the cone does not move a
        # millimetre. Measured: identical results at 8 kg, 200 kg and fully static.
        #
        # Keeping every body awake is a scene-level workaround, not a fix. It costs a solver pass
        # on ~60 idle bodies, which is nothing here, and it is what makes the cones scatter and
        # the crates shove the way a driving course needs them to.
        # Spelled in full: `1.0e9` does not survive a YAML load/save round-trip as the same
        # scalar, and tests/editor/unit/asset_roundtrip_test.cpp holds every scene to that.
        sleep_time: 1000000.0
  root_objects:
'''
    text = header + "\n".join(parts) + "\n"
    (OUT / "scene.yaml").write_text(text)
    print(f"wrote {OUT / 'scene.yaml'} ({len(parts)} root objects, {len(text.splitlines())} lines)")


if __name__ == "__main__":
    main()
