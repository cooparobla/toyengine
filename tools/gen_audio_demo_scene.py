#!/usr/bin/env python3
"""Generates the audio_demo scene and its UI panel.

Run from the repo root:  python3 tools/gen_audio_demo_scene.py

Writes:
  assets/scenes/audio/audio_demo/scene.yaml
  assets/scenes/audio/audio_demo/ui/audio_panel.yaml

The clips it plays come from tools/gen_audio_demo_assets.py (assets/audio/).
"""

import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCENE_DIR = ROOT / "assets" / "scenes" / "audio" / "audio_demo"


def v3(x, y, z):
    return f"{{ x: {x}, y: {y}, z: {z} }}"


def rgb(r, g, b):
    return f"{{ r: {r}, g: {g}, b: {b} }}"


def transform(pos, rot=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0), ind=8):
    p = " " * ind
    return (f"{p}- type: Transform\n{p}  position: {v3(*pos)}\n{p}  rotation: {v3(*rot)}\n"
            f"{p}  scale: {v3(*scale)}\n")


def mesh(path, albedo, ind=8, emissive=None, strength=2.0, roughness=0.7, metallic=0.0):
    p = " " * ind
    s = (f"{p}- type: MeshRenderer\n{p}  mesh_path: {path}\n{p}  material:\n"
         f"{p}    albedo: {rgb(*albedo)}\n{p}    metallic: {metallic}\n{p}    roughness: {roughness}\n{p}    ao: 1.0\n")
    if emissive:
        s += f"{p}    emissive: {rgb(*emissive)}\n{p}    emissive_strength: {strength}\n"
    return s


def light(color, intensity, rng, ind=8):
    p = " " * ind
    return (f"{p}- type: PointLight\n{p}  color: {rgb(*color)}\n{p}  intensity: {intensity}\n"
            f"{p}  range: {rng}\n{p}  cast_shadows: false\n")


def source(clip, ind=8, volume=1.0, pitch=1.0, play=True, loop=True, min_d=1.0, max_d=40.0, curve="Inverse",
           doppler=1.0, bus="SFX"):
    p = " " * ind
    return (f"{p}- type: AudioSource\n{p}  clip: {clip}\n{p}  bus: {bus}\n{p}  volume: {volume}\n{p}  pitch: {pitch}\n"
            f"{p}  loop: {str(loop).lower()}\n{p}  play_on_start: {str(play).lower()}\n{p}  spatialize: true\n"
            f"{p}  min_distance: {min_d}\n{p}  max_distance: {max_d}\n{p}  curve: {curve}\n{p}  doppler: {doppler}\n")


def label(text, z, width=420, ind=8):
    """A camera-facing world-space caption as a child object."""
    p = " " * ind
    return (f"{p}- name: label\n{p}  components:\n"
            + transform((0.0, 0.0, z), ind=ind + 4) +
            f"{p}    - type: RectTransform\n{p}      anchor_preset: StretchAll\n"
            f"{p}    - type: Canvas\n{p}      render_mode: WorldSpace\n{p}      billboard: CameraFacing\n"
            f"{p}      world_size: {{ x: {width}, y: 60 }}\n{p}      pixels_per_unit: 70\n{p}      text_supersample: 2\n"
            f"{p}      occlude: true\n{p}    - type: Theme\n{p}      source: ui/themes/default.yaml\n"
            f"{p}  children:\n{p}    - name: text\n{p}      components:\n"
            f"{p}        - type: RectTransform\n{p}          anchor_preset: StretchAll\n"
            f"{p}        - {{type: ThemedText, text: \"{text}\", font_role: Heading, align: Center}}\n")


# --- scene ------------------------------------------------------------------------------------

BEACONS = [  # name, position, pitch, colour, caption
    ("beacon_north", (0.0, 10.0), 1.0, (1.0, 0.25, 0.2), "North beacon"),
    ("beacon_east", (10.0, 0.0), 1.26, (0.3, 1.0, 0.35), "East beacon"),
    ("beacon_south", (0.0, -10.0), 0.75, (0.3, 0.55, 1.0), "South beacon"),
    ("beacon_west", (-10.0, 0.0), 1.5, (1.0, 0.85, 0.2), "West beacon"),
]

PINGS = [  # child of the camera; the camera looks down its local -Z, +Y is up
    ("ping_left", (-4.0, 0.0, 0.0), 1.0),
    ("ping_right", (4.0, 0.0, 0.0), 1.0),
    ("ping_front", (0.0, 0.0, -4.0), 1.0),
    ("ping_behind", (0.0, 0.0, 4.0), 1.0),
    ("ping_above", (0.0, 4.0, 0.0), 1.0),
]

HEADER = """\
# audio_demo -- toyengine's audio end to end: background music with crossfades, 3D spatial sound,
# and UI sounds. Headphones make the 3D part obvious.
#
# You stand in the middle of a ring of stations (fly camera: WASD move, Q/E down/up, arrow keys
# turn; the mouse is free for the panel on the left).
#
#   * MUSIC (object `music`, a MusicPlaylist): meadow -> expedition, 4 s crossfades, repeating.
#     Each track hands off to the next on its own a few seconds before it ends -- the "natural"
#     transition. The panel's Music tab drives it from code paths (MusicOnSignal): previous /
#     next, pause (fades), stop, play a track directly (a 2.5 s crossfade), push a "boss" track
#     over it and pop back, restart the playlist. The status line (MusicStatusText) shows the
#     track, its clock and when a crossfade is running.
#   * MUSIC ZONE (south-west, violet floor): a MusicZone. Walk in and `nocturne` is pushed over
#     the playlist, which fades out and PAUSES; walk out and it fades back in where it stopped.
#   * COMPASS BEACONS (N/E/S/W, 10 m): a blip loop at a different pitch each, toggled from the
#     Spatial tab -- turn on one at a time and turn your head; it moves between the ears.
#   * DRONE: a buzzing sphere orbiting you at 6 m, 120 deg/s -- direction sweeps around your head
#     and doppler bends its pitch as it comes and goes.
#   * CRICKETS (north-west grass), CHIMES (north-east post): ambience with inverse rolloff.
#   * CAMPFIRE (south-east): crackle with LINEAR rolloff to 16 m -- walk away and it fades to
#     silence, unlike the inverse-curve sources.
#   * MUSIC BOX (far north, 28 m): too far to hear from the start; walk up to it.
#   * PINGS: five one-shot sources parented to the camera (left / right / front / behind /
#     above), fired from the Spatial tab -- they always come from that side of your head.
#   * UI: every widget plays the canvas' UiSoundPlayer cues (hover tick, press click, slider
#     ticks, tab switch); the UI Sounds tab's buttons each add a PlaySoundOnSignal sound on
#     release. The Mixer tab's sliders are VolumeBindings on the Master / Music / SFX / UI buses.
#
# Clips: tools/gen_audio_demo_assets.py. This file: tools/gen_audio_demo_scene.py.
#
# Run: ./build/toyengine audio_demo        (or SCENE=audio_demo)
"""


def scene_yaml():
    s = HEADER + """
scene:
  scene_name: audio_demo
  settings:
    render:
      sky_zenith:  [0.06, 0.1, 0.24]
      sky_horizon: [0.55, 0.42, 0.38]
      sky_ground:  [0.05, 0.05, 0.05]
      ambient_intensity: 0.9
      world_ui_enabled: true
  root_objects:
"""
    # Ground.
    s += "    - name: ground\n      active: true\n      components:\n"
    s += transform((0.0, 0.0, 0.0), scale=(32.0, 32.0, 1.0))
    s += "        - type: MeshRenderer\n          mesh_path: plane\n          material: materials/concrete\n"
    s += "      children: []\n\n"

    # Centre marker: where you start.
    s += "    - name: centre_marker\n      active: true\n      components:\n"
    s += transform((0.0, 0.0, 0.01), scale=(0.6, 0.6, 0.02))
    s += mesh("barrel", (0.08, 0.08, 0.1), emissive=(0.35, 0.5, 0.9), strength=0.35)
    s += "      children: []\n\n"

    # Music.
    s += """    # ==========================================================================================
    # MUSIC: the playlist, and the named tracks the panel and the zone play.
    # ==========================================================================================
    - name: music
      active: true
      components:
        - type: MusicPlaylist
          crossfade: 4.0
          repeat: true
          tracks:
            - { name: meadow, clip: audio/music/meadow.wav, volume: 0.9 }
            - { name: expedition, clip: audio/music/expedition.wav, volume: 0.8 }
      children:
        # Registers `nocturne` by name for the zone and the panel (not part of the playlist).
        - name: night_track
          components:
            - type: MusicPlaylist
              play_on_start: false
              tracks:
                - { name: nocturne, clip: audio/music/nocturne.wav, volume: 0.9 }

    - name: music_zone
      active: true
      components:
"""
    s += transform((-14.0, -12.0, 0.02), scale=(1.0, 1.0, 1.0))
    s += "        - type: MusicZone\n          track: nocturne\n          size: { x: 8.0, y: 8.0, z: 8.0 }\n          fade: 2.5\n"
    s += "      children:\n"
    s += "        - name: zone_floor\n          components:\n"
    s += transform((0.0, 0.0, 0.0), scale=(4.0, 4.0, 1.0), ind=12)
    s += mesh("plane", (0.05, 0.02, 0.08), ind=12, emissive=(0.55, 0.25, 1.0), strength=0.6)
    for i, (x, y) in enumerate(((-4, -4), (4, -4), (-4, 4), (4, 4))):
        s += f"        - name: zone_post_{i}\n          components:\n"
        s += transform((float(x), float(y), 1.0), scale=(0.12, 0.12, 2.0), ind=12)
        s += mesh("cube", (0.05, 0.02, 0.08), ind=12, emissive=(0.6, 0.3, 1.0), strength=2.5)
    s += "        - name: zone_light\n          components:\n"
    s += transform((0.0, 0.0, 3.0), ind=12)
    s += light((0.6, 0.35, 1.0), 40.0, 9.0, ind=12)
    s += label("Music zone: nocturne", 3.2, width=520, ind=8)
    s += "\n"

    # Beacons.
    s += """    # ==========================================================================================
    # COMPASS BEACONS (toggled from the panel's Spatial tab; start off)
    # ==========================================================================================
"""
    for name, (x, y), pitch, col, caption in BEACONS:
        s += f"    - name: {name}\n      active: true\n      components:\n"
        s += transform((x, y, 0.0))
        s += source("audio/sfx/beacon.wav", volume=0.8, pitch=pitch, play=False, min_d=2.0, max_d=60.0)
        s += "      children:\n"
        s += "        - name: post\n          components:\n"
        s += transform((0.0, 0.0, 0.75), scale=(0.25, 0.25, 0.75), ind=12)
        s += mesh("barrel", (0.15, 0.15, 0.17), ind=12, roughness=0.5, metallic=0.6)
        s += "        - name: lamp\n          components:\n"
        s += transform((0.0, 0.0, 1.75), scale=(0.3, 0.3, 0.3), ind=12)
        s += mesh("sphere", tuple(c * 0.1 for c in col), ind=12, emissive=col, strength=3.0)
        s += light(col, 30.0, 6.0, ind=12)
        s += label(f"{caption} (x{pitch})", 2.6, ind=8)
        s += "\n"

    # Drone.
    s += """    # ==========================================================================================
    # DRONE: orbits the listener -- direction sweeps, doppler bends the pitch
    # ==========================================================================================
    - name: drone
      active: true
      components:
"""
    s += transform((6.0, 0.0, 2.2), scale=(0.35, 0.35, 0.35))
    s += mesh("sphere", (0.1, 0.05, 0.02), emissive=(1.0, 0.5, 0.15), strength=3.0)
    s += light((1.0, 0.55, 0.2), 25.0, 5.0)
    s += source("audio/sfx/drone.wav", volume=0.45, min_d=1.5, max_d=50.0, doppler=1.5)
    s += ("        - type: KinematicMover\n          mode: orbit\n          orbit_center: { x: 0.0, y: 0.0, z: 0.0 }\n"
          "          orbit_radius: 6.0\n          speed: 120.0\n")
    s += "      children: []\n\n"

    # Ambience.
    s += """    # ==========================================================================================
    # AMBIENCE: crickets (north-west), chimes (north-east), campfire (south-east, linear rolloff)
    # ==========================================================================================
    - name: crickets
      active: true
      components:
"""
    s += transform((-13.0, 12.0, 0.0))
    s += source("audio/sfx/crickets.wav", volume=0.5, min_d=2.0, max_d=40.0)
    s += "      children:\n"
    for i, (x, y, sc) in enumerate(((0, 0, 1.0), (1.4, 0.6, 0.7), (-1.1, 0.9, 0.8), (0.4, -1.3, 0.6))):
        s += f"        - name: grass_{i}\n          components:\n"
        s += transform((float(x), float(y), 0.0), scale=(2.0 * sc, 2.0 * sc, 1.2 * sc), ind=12)
        s += "            - type: MeshRenderer\n              mesh_path: mossy_mound\n              material: materials/foliage\n"
    s += label("Crickets", 2.4, ind=8)
    s += "\n    - name: chimes\n      active: true\n      components:\n"
    s += transform((12.0, 12.0, 0.0))
    s += source("audio/sfx/chimes.wav", volume=0.55, min_d=2.0, max_d=40.0)
    s += "      children:\n"
    s += "        - name: chime_post\n          components:\n"
    s += transform((0.0, 0.0, 1.5), scale=(0.08, 0.08, 1.5), ind=12)
    s += "            - type: MeshRenderer\n              mesh_path: barrel\n              material: materials/wood\n"
    for i in range(5):
        s += f"        - name: chime_{i}\n          components:\n"
        s += transform((-0.4 + i * 0.2, 0.3, 2.3 - i * 0.08), scale=(0.03, 0.03, 0.35 + 0.06 * i), ind=12)
        s += "            - type: MeshRenderer\n              mesh_path: barrel\n              material: materials/copper\n"
    s += label("Wind chimes", 3.6, ind=8)
    s += "\n    - name: campfire\n      prefab: objects/campfire\n      components:\n"
    s += transform((12.0, -10.0, 0.0))
    s += source("audio/sfx/fire.wav", volume=0.9, min_d=1.0, max_d=16.0, curve="Linear")
    s += "\n    - name: campfire_label\n      active: true\n      components:\n"
    s += transform((12.0, -10.0, 0.0))
    s += "      children:\n" + label("Campfire (linear rolloff, 16 m)", 2.6, width=560, ind=8)

    # Music box, far away.
    s += """
    # ==========================================================================================
    # MUSIC BOX: 28 m north, out of earshot until you walk to it
    # ==========================================================================================
    - name: music_box
      active: true
      components:
"""
    s += transform((0.0, 28.0, 0.0))
    s += source("audio/sfx/music_box.wav", volume=0.9, min_d=1.0, max_d=14.0, curve="Linear")
    s += "      children:\n"
    s += "        - name: box\n          components:\n"
    s += transform((0.0, 0.0, 0.5), scale=(0.6, 0.45, 0.5), ind=12)
    s += "            - type: MeshRenderer\n              mesh_path: cube\n              material: materials/wood\n"
    s += "        - name: box_light\n          components:\n"
    s += transform((0.0, 0.0, 1.6), ind=12)
    s += light((1.0, 0.8, 0.5), 30.0, 5.0, ind=12)
    s += label("Music box", 1.8, ind=8)

    # Sun + camera (with the head-locked ping sources).
    s += """
    - name: sun
      active: true
      components:
"""
    s += transform((0.0, 0.0, 20.0))
    s += ("        - type: DirectionalLight\n          direction: { x: -0.5, y: 0.3, z: -0.55 }\n"
          "          color: { r: 1.0, g: 0.78, b: 0.6 }\n          intensity: 0.8\n          cast_shadows: true\n")
    s += "      children: []\n\n"
    s += """    # The listener: AudioSystem hears from the main camera. Fly mode, mouse look off so the
    # cursor is free for the panel.
    - name: camera
      active: true
      components:
"""
    s += transform((0.0, -3.0, 1.8), rot=(84.0, 0.0, 0.0))
    s += """        - type: Camera
          main: true
          projection: Perspective
          fov: 60.0
          near_clip_plane: 0.1
          far_clip_plane: 300.0
        - type: CameraController
          mode: fly
          move_speed: 5.0
          mouse_sensitivity: 0.0
          capture_cursor: false
      children:
"""
    for name, pos, pitch in PINGS:
        s += f"        - name: {name}\n          components:\n"
        s += transform(pos, ind=12)
        s += source("audio/sfx/ping.wav", ind=12, volume=0.8, pitch=pitch, play=False, loop=False, min_d=4.0,
                    max_d=40.0, doppler=0.0)
    s += """
    # The panel (ui/audio_panel, in this folder): music transport, spatial toggles, UI sounds, mixer.
    - name: audio_panel
      prefab: ui/audio_panel
"""
    return s


# --- UI panel ---------------------------------------------------------------------------------

def rect(w, h, ind):
    p = " " * ind
    return (f"{p}- type: RectTransform\n{p}  anchor_min: {{x: 0.5, y: 0.5}}\n{p}  anchor_max: {{x: 0.5, y: 0.5}}\n"
            f"{p}  pivot: {{x: 0.5, y: 0.5}}\n{p}  anchored_position: {{x: 0, y: 0}}\n{p}  size_delta: {{x: {w}, y: {h}}}\n")


def ui_obj(name, comp, w, h, ind):
    p = " " * ind
    return f"{p}- name: {name}\n{p}  components:\n" + rect(w, h, ind + 4) + f"{p}    - {comp}\n"


def text_row(name, text, ind, h=40, role="Body", wrap=False, extra=""):
    w = ", wrap: true" if wrap else ""
    s = ui_obj(name, f"{{type: ThemedText, text: \"{text}\", font_role: {role}{w}}}", 520, h, ind)
    if extra:
        s += " " * (ind + 4) + f"- {extra}\n"
    return s


def button_row(name, items, ind, h=52):
    its = ", ".join(f"{{name: {n}, label: \"{l}\"{', role: Primary' if r else ''}}}" for n, l, r in items)
    return ui_obj(name, f"{{type: ActionBar, align: MiddleLeft, button_height: {h - 4}, items: [{its}]}}", 520, h, ind)


def toggle_row(name, label_, on, ind):
    return ui_obj(name, f"{{type: SettingRow, kind: toggle, label: \"{label_}\", is_on: {str(on).lower()}, label_width: 300}}",
                  520, 36, ind)


def page(name, rows, ind=12):
    p = " " * ind
    return (f"{p}- name: {name}\n{p}  components:\n" + rect(540, 700, ind + 4) +
            f"{p}    - {{type: VerticalLayoutGroup, spacing: 8, child_force_expand_height: false}}\n"
            f"{p}  children:\n" + "".join(rows))


SPATIAL_TOGGLES = [  # toggle name, label, target, starts on
    ("BeaconNorth", "North beacon (front)", "beacon_north", False),
    ("BeaconEast", "East beacon (right)", "beacon_east", False),
    ("BeaconSouth", "South beacon (behind)", "beacon_south", False),
    ("BeaconWest", "West beacon (left)", "beacon_west", False),
    ("Drone", "Orbiting drone (doppler)", "drone", True),
    ("Crickets", "Crickets", "crickets", True),
    ("Chimes", "Wind chimes", "chimes", True),
    ("Campfire", "Campfire", "campfire", True),
    ("MusicBox", "Music box (far)", "music_box", True),
]

UI_SOUND_BUTTONS = [  # button name, label, library sound
    [("SoundConfirm", "Confirm", "confirm"), ("SoundDenied", "Denied", "denied"), ("SoundCoin", "Coin", "coin")],
    [("SoundLevelUp", "Level up", "level_up"), ("SoundError", "Error", "error"), ("SoundNotify", "Notify", "notify")],
    [("SoundPageTurn", "Page turn", "page_turn"), ("SoundPowerup", "Power-up", "powerup"), ("SoundWhoosh", "Whoosh", "whoosh_short")],
]


def panel_yaml(first_page=None):
    ind = 24
    music_rows = [
        text_row("NowPlaying", "", ind, h=44, role="Heading", extra="{type: MusicStatusText, prefix: \"Now playing: \"}"),
        text_row("PlaylistCaption", "Playlist (crossfades on its own as a track ends)", ind, role="Label"),
        button_row("Transport", [("MusicPrevious", "Prev", False), ("MusicPause", "Pause", False),
                                 ("MusicNext", "Next", False), ("MusicStop", "Stop", False)], ind),
        button_row("PlaylistRow", [("MusicPlaylist", "Restart playlist", True)], ind),
        text_row("DirectCaption", "Play a track from code (2.5 s crossfade)", ind, role="Label"),
        button_row("DirectRow", [("PlayMeadow", "Meadow", False), ("PlayExpedition", "Expedition", False),
                                 ("PlayNocturne", "Nocturne", False)], ind),
        text_row("StackCaption", "Push a track over the music, then pop back", ind, role="Label"),
        button_row("StackRow", [("MusicPush", "Push boss (0.4 s)", False), ("MusicPop", "Pop", False)], ind),
        text_row("MusicHint", "The violet zone (south-west) pushes the night track while you stand in it; "
                 "the paused playlist resumes where it stopped when you leave.", ind, h=96, role="Caption", wrap=True),
    ]
    spatial_rows = [text_row("SpatialHint", "WASD move, Q/E down/up, arrow keys turn. Use headphones.", ind,
                             role="Caption")]
    spatial_rows += [toggle_row(n, l, on, ind) for n, l, _, on in SPATIAL_TOGGLES]
    spatial_rows += [
        text_row("PingCaption", "One-shot pings locked to your head", ind, role="Label"),
        button_row("PingRow1", [("PingLeft", "Left", False), ("PingFront", "Front", False), ("PingRight", "Right", False)], ind),
        button_row("PingRow2", [("PingBehind", "Behind", False), ("PingAbove", "Above", False)], ind),
    ]
    ui_rows = [text_row("UiHint", "Every widget plays the canvas' cues: hover tick, press click, slider ticks, "
                        "tab switch. These buttons add their own sound on release.", ind, h=96, role="Caption", wrap=True)]
    ui_rows += [button_row(f"SoundRow{i}", [(n, l, False) for n, l, _ in row], ind) for i, row in enumerate(UI_SOUND_BUTTONS)]
    ui_rows += [
        ui_obj("UiToggleTest", "{type: SettingRow, kind: toggle, label: \"Toggle\", is_on: true, label_width: 200}", 520, 40, ind),
        ui_obj("UiSliderTest", "{type: SettingRow, kind: slider, label: \"Slider\", min: 0, max: 1, value: 0.5, label_width: 200}",
               520, 40, ind),
        ui_obj("UiDropdownTest", "{type: SettingRow, kind: dropdown, label: \"Dropdown\", items: [One, Two, Three], "
               "selected_index: 0, label_width: 200}", 520, 40, ind),
    ]
    mixer_rows = [text_row("MixerHint", "Bus volumes (remembered per player)", ind, role="Label")]
    for name, label_, bus in (("MasterVolume", "Master", "Master"), ("MusicVolume", "Music", "Music"),
                              ("SfxVolume", "Effects", "SFX"), ("UiVolume", "UI", "UI")):
        mixer_rows.append(ui_obj(name, f"{{type: SettingRow, kind: slider, label: {label_}, min: 0, max: 1, value: 0.8, "
                                       f"label_width: 160}}", 520, 44, ind) + " " * (ind + 4) + f"- {{type: VolumeBinding, bus: {bus}}}\n")

    reactors = []
    music_actions = [("MusicPrevious", "previous", ""), ("MusicPause", "toggle_pause", ""), ("MusicNext", "next", ""),
                     ("MusicStop", "stop", ""), ("PlayMeadow", "play", "meadow"), ("PlayExpedition", "play", "expedition"),
                     ("PlayNocturne", "play", "nocturne"), ("MusicPop", "pop", "")]
    for obj, action, track in music_actions:
        t = f", track: {track}, fade: 2.5" if track else ""
        reactors.append(f"{{type: MusicOnSignal, listen_object: {obj}, listen_signal: click, action: {action}{t}}}")
    reactors.append("{type: MusicOnSignal, listen_object: MusicPush, listen_signal: click, action: push, track: expedition, fade: 0.4}")
    reactors.append("{type: MusicOnSignal, listen_object: MusicPlaylist, listen_signal: click, action: playlist, target: music}")
    for n, _, target, _ in SPATIAL_TOGGLES:
        reactors.append(f"{{type: AudioSourceOnSignal, listen_object: {n}, listen_signal: value_changed, action: follow, target: {target}}}")
    for name, _, _ in PINGS:
        btn = "Ping" + name.split("_")[1].capitalize()
        reactors.append(f"{{type: AudioSourceOnSignal, listen_object: {btn}, listen_signal: click, action: play, target: {name}}}")
    for row in UI_SOUND_BUTTONS:
        for n, _, sound in row:
            reactors.append(f"{{type: PlaySoundOnSignal, listen_object: {n}, listen_signal: click, sound: {sound}}}")

    s = """\
# audio_panel -- the audio_demo control panel: Music / Spatial / UI Sounds / Mixer tabs.
#
# Everything is wired in YAML with reactors on the root (MusicOnSignal, AudioSourceOnSignal,
# PlaySoundOnSignal) listening to the buttons and toggles by name, and a UiSoundPlayer giving
# every widget hover / press / value / tab sounds. Generated by tools/gen_audio_demo_scene.py.
object:
  name: audio_panel
  components:
    - type: RectTransform
      anchor_min: {x: 0, y: 0}
      anchor_max: {x: 1, y: 1}
      pivot: {x: 0.5, y: 0.5}
      anchored_position: {x: 0, y: 0}
      size_delta: {x: 0, y: 0}
    - type: Canvas
      mode: ScaleWithScreenSize
      reference_resolution: {x: 1920, y: 1080}
      match_width_or_height: 0.5
      sort_order: 10
    - {type: Theme, source: ui/themes/default.yaml}
    - type: UiSoundPlayer
      cues:
        - {signal: hover_enter, sound: ui_hover_tick, min_interval: 0.04}
        - {signal: press, sound: ui_click_soft}
        - {signal: value_changed, sound: ui_slider_tick, min_interval: 0.03}
        - {signal: selection_changed, sound: ui_tab_switch}
"""
    s += "".join(f"    - {r}\n" for r in reactors)
    s += """  children:
    - name: Panel
      components:
        - type: RectTransform
          anchor_min: {x: 0, y: 0.5}
          anchor_max: {x: 0, y: 0.5}
          pivot: {x: 0, y: 0.5}
          anchored_position: {x: 32, y: 0}
          size_delta: {x: 620, y: 960}
        - {type: Window, title: Audio, padding: 24}
      children:
        - name: Tabs
          components:
""" + rect(572, 860, 12) + """            - type: TabView
              tabs: [Music, Spatial, UI Sounds, Mixer]
          children:
"""
    pages = [("Music", page("MusicPage", music_rows)), ("Spatial", page("SpatialPage", spatial_rows)),
             ("UI Sounds", page("UiPage", ui_rows)), ("Mixer", page("MixerPage", mixer_rows))]
    if first_page:   # verification renders only: the TabView shows its first page
        pages.sort(key=lambda p: p[0] != first_page)
        s = s.replace("tabs: [Music, Spatial, UI Sounds, Mixer]", "tabs: [" + ", ".join(p[0] for p in pages) + "]")
    s += "".join(p[1] for p in pages)
    return s


def main():
    (SCENE_DIR / "ui").mkdir(parents=True, exist_ok=True)
    (SCENE_DIR / "scene.yaml").write_text(scene_yaml())
    (SCENE_DIR / "ui" / "audio_panel.yaml").write_text(panel_yaml())
    print("wrote", (SCENE_DIR / "scene.yaml").relative_to(ROOT), "and ui/audio_panel.yaml")


if __name__ == "__main__":
    main()
