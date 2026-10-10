#!/usr/bin/env python3
"""Writes the UI designer's templates and the sample project's UI: game-ready RPG screens built
from uicoopa's themed composites (Window, MenuList, StatBar, Hotbar, SettingRow...).

Outputs:
  * editor/templates/ui/<name>.yaml -- what the editor's "New UI" menu offers (UI tab, +). A new
    UI copies one into the project's assets/ui/ and renames its root.
  * editor/templates/ui/themes/default.yaml and editor/templates/ui/fonts/*.ttf -- installed into
    a project (assets/ui/themes/, assets/fonts/) the first time a template is used.
  * assets/ui/*.yaml, assets/ui/themes/default.yaml, assets/fonts/inter_semibold.ttf -- the same,
    in this repository's own project.
The scene that shows them in use, assets/scenes/ui/ui_demo, is hand-maintained (it places the
HUD as `prefab: ui/hud` beside world-space canvases).

Every interactive widget has a NAME game code binds to (coopa::ui::UiHandle); the editor's
Bindings tab lists them. Layout is authored for 1920 x 1080 and scales with the window
(Canvas mode ScaleWithScreenSize).

Run:  python3 tools/gen_ui_templates.py
"""

import copy
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, ".."))
TEMPLATES = os.path.join(REPO, "editor", "templates", "ui")
ASSETS = os.path.join(REPO, "assets")
UICOOPA_FONTS = os.path.join(REPO, "libs", "uicoopa", "assets", "fonts")


# ---------------------------------------------------------------------------------------------
# A small YAML writer: block maps / sequences, short maps of scalars inline ({x: 1, y: 2}).
# ---------------------------------------------------------------------------------------------

def scalar(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return f"{v:g}" if isinstance(v, float) else str(v)
    s = str(v)
    plain = s and all(c.isalnum() or c in "_./ -" for c in s) and not s[0].isdigit() and s.strip() == s \
        and s.lower() not in ("true", "false", "null", "yes", "no", "on", "off")
    return s if plain else '"' + s.replace('"', '\\"') + '"'


def is_flat(v):
    return isinstance(v, dict) and all(not isinstance(x, (dict, list)) for x in v.values()) and len(v) <= 6


def flow(v):
    if isinstance(v, dict):
        return "{" + ", ".join(f"{k}: {flow(x)}" for k, x in v.items()) + "}"
    if isinstance(v, list):
        return "[" + ", ".join(flow(x) for x in v) + "]"
    return scalar(v)


def emit(v, indent=0):
    pad = " " * indent
    out = []
    if isinstance(v, dict):
        for k, x in v.items():
            if isinstance(x, dict) and not is_flat(x):
                out.append(f"{pad}{k}:")
                out.extend(emit(x, indent + 2))
            elif isinstance(x, list) and x and any(isinstance(e, dict) and not is_flat(e) for e in x):
                out.append(f"{pad}{k}:")
                out.extend(emit(x, indent + 2))
            elif isinstance(x, list) and len(flow(x)) > 90:
                out.append(f"{pad}{k}:")
                out.extend(emit(x, indent + 2))
            else:
                out.append(f"{pad}{k}: {flow(x)}")
    elif isinstance(v, list):
        for e in v:
            if isinstance(e, dict) and not is_flat(e):
                lines = emit(e, indent + 2)
                lines[0] = pad + "- " + lines[0][indent + 2:]
                out.extend(lines)
            else:
                out.append(f"{pad}- {flow(e)}")
    return out


def write(path, doc, header=""):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        if header:
            f.write("".join(f"# {l}\n" if l else "#\n" for l in header.strip().split("\n")))
        f.write("\n".join(emit(doc)) + "\n")
    print("wrote", os.path.relpath(path, REPO))


# ---------------------------------------------------------------------------------------------
# Building blocks
# ---------------------------------------------------------------------------------------------

PRESETS = {
    "TopLeft": ((0, 1), (0, 1), (0, 1)), "TopCenter": ((0.5, 1), (0.5, 1), (0.5, 1)), "TopRight": ((1, 1), (1, 1), (1, 1)),
    "MiddleLeft": ((0, 0.5), (0, 0.5), (0, 0.5)), "MiddleCenter": ((0.5, 0.5), (0.5, 0.5), (0.5, 0.5)),
    "MiddleRight": ((1, 0.5), (1, 0.5), (1, 0.5)), "BottomLeft": ((0, 0), (0, 0), (0, 0)),
    "BottomCenter": ((0.5, 0), (0.5, 0), (0.5, 0)), "BottomRight": ((1, 0), (1, 0), (1, 0)),
    "StretchAll": ((0, 0), (1, 1), (0.5, 0.5)), "StretchTop": ((0, 1), (1, 1), (0.5, 1)),
    "StretchBottom": ((0, 0), (1, 0), (0.5, 0)), "StretchHorizontal": ((0, 0.5), (1, 0.5), (0.5, 0.5)),
}


def v2(x, y):
    return {"x": x, "y": y}


def rect(preset="MiddleCenter", pos=(0, 0), size=(100, 100), **extra):
    amin, amax, pivot = PRESETS[preset]
    r = {"type": "RectTransform", "anchor_min": v2(*amin), "anchor_max": v2(*amax), "pivot": v2(*pivot),
         "anchored_position": v2(*pos), "size_delta": v2(*size)}
    r.update(extra)
    return r


def comp(type_, **kw):
    c = {"type": type_}
    c.update(kw)
    return c


def obj(name, *components, children=None, active=True):
    o = {"name": name}
    if not active:
        o["active"] = False
    o["components"] = list(components)
    if children:
        o["children"] = children
    return o


def canvas_root(name, children, sort_order=0, theme=True):
    comps = [rect("StretchAll", size=(0, 0)),
             comp("Canvas", mode="ScaleWithScreenSize", reference_resolution=v2(1920, 1080), match_width_or_height=0.5,
                  sort_order=sort_order)]
    if theme:
        comps.append(comp("Theme", source="ui/themes/default.yaml"))
    return {"object": obj(name, *comps, children=children)}


def items(*pairs, primary=None):
    out = []
    for name, label in pairs:
        it = {"name": name, "label": label}
        if name == primary:
            it["role"] = "Primary"
        out.append(it)
    return out


def rgba(r, g, b, a=1.0):
    return {"r": r, "g": g, "b": b, "a": a}


# ---------------------------------------------------------------------------------------------
# The theme: a full-resolution dark slate look with a warm gold accent
# ---------------------------------------------------------------------------------------------

THEME = {
    "name": "default",
    "panel": {
        "background": rgba(0.05, 0.06, 0.08, 0.92), "panel": rgba(0.09, 0.10, 0.14, 0.94),
        "panel_alt": rgba(0.07, 0.08, 0.11, 0.96), "header_bar": rgba(0.13, 0.15, 0.20, 1.0), "border": rgba(0.32, 0.30, 0.26, 1.0),
    },
    "text": {
        "primary": rgba(0.94, 0.93, 0.90), "secondary": rgba(0.66, 0.67, 0.72), "muted": rgba(0.48, 0.50, 0.55),
        "accent": rgba(0.96, 0.78, 0.40), "success": rgba(0.45, 0.85, 0.55), "warning": rgba(0.98, 0.80, 0.35),
        "info": rgba(0.45, 0.70, 0.98), "selection": rgba(0.80, 0.58, 0.20),
        "size_title": 40.0, "size_heading": 26.0, "size_body": 20.0, "size_label": 22.0, "size_small": 16.0,
        "font_path": "../../fonts/inter_regular.ttf",
        "fonts": {
            "title": {"path": "../../fonts/inter_semibold.ttf"}, "heading": {"path": "../../fonts/inter_semibold.ttf"},
            "body": {"path": "../../fonts/inter_regular.ttf"}, "label": {"path": "../../fonts/inter_regular.ttf"},
            "caption": {"path": "../../fonts/inter_regular.ttf"}, "numeric": {"path": "../../fonts/inter_semibold.ttf"},
        },
    },
    "button": {"normal": rgba(0.16, 0.18, 0.23), "hover": rgba(0.23, 0.26, 0.33), "press": rgba(0.12, 0.13, 0.17),
               "disabled": rgba(0.12, 0.13, 0.16, 0.5)},
    "button_primary": {"normal": rgba(0.78, 0.56, 0.22), "hover": rgba(0.90, 0.67, 0.30), "press": rgba(0.62, 0.44, 0.16),
                       "disabled": rgba(0.12, 0.13, 0.16, 0.5)},
    "button_success": {"normal": rgba(0.22, 0.50, 0.36), "hover": rgba(0.28, 0.62, 0.44), "press": rgba(0.17, 0.40, 0.28),
                       "disabled": rgba(0.12, 0.13, 0.16, 0.5)},
    "slider": {"track": rgba(0.14, 0.16, 0.20), "fill": rgba(0.90, 0.68, 0.30), "handle": rgba(0.95, 0.94, 0.90),
               "handle_hover": rgba(1, 1, 1), "handle_press": rgba(0.75, 0.74, 0.70), "handle_disabled": rgba(0.9, 0.9, 0.9, 0.4),
               "height": 28.0, "handle_width": 18.0},
    "toggle": {"bg": rgba(0.14, 0.16, 0.20), "bg_hover": rgba(0.20, 0.23, 0.29), "bg_press": rgba(0.11, 0.12, 0.15),
               "bg_disabled": rgba(0.14, 0.16, 0.20, 0.5), "check": rgba(0.90, 0.68, 0.30), "size": 30.0},
    "spinbox": {"bg": rgba(0.12, 0.14, 0.18), "height": 40.0, "btn_width": 36.0},
    "combobox": {"bg": rgba(0.16, 0.18, 0.23), "popup_bg": rgba(0.10, 0.11, 0.15, 0.98), "height": 40.0},
    "slot": {"bg": rgba(0.11, 0.12, 0.16, 0.92), "border": rgba(0.30, 0.29, 0.26), "hover": rgba(0.80, 0.62, 0.32),
             "selected": rgba(0.96, 0.78, 0.40), "tooltip_bg": rgba(0.04, 0.05, 0.07, 0.96), "tooltip_text": rgba(0.94, 0.93, 0.90)},
    "focus": {"color": rgba(0.96, 0.78, 0.40), "fill": rgba(0.96, 0.78, 0.40, 0.0), "thickness": 3.0, "padding": 4.0,
              "move_duration": 0.08, "fade_duration": 0.12},
    "metrics": {"row_height": 44.0, "row_spacing": 12.0, "label_width": 300.0, "header_height": 38.0, "card_header_height": 56.0,
                "scrollbar_thickness": 10.0, "scroll_frame_padding": 10.0, "button_padding_x": 28.0, "button_min_width": 180.0,
                "section_spacing": 16.0, "dialog_padding": 24.0, "tab_height": 50.0, "tab_spacing": 6.0,
                "tab_indicator_height": 4.0, "scrim_alpha": 0.6},
    "tab": {"normal": rgba(0.10, 0.11, 0.15), "hover": rgba(0.16, 0.18, 0.23), "press": rgba(0.08, 0.09, 0.12),
            "selected": rgba(0.20, 0.22, 0.28), "selected_hover": rgba(0.24, 0.27, 0.34), "indicator": rgba(0.96, 0.78, 0.40)},
    "hud": {"bar_bg": rgba(0.04, 0.05, 0.07, 0.80), "bar_border": rgba(0.30, 0.29, 0.26), "bar_text": rgba(0.96, 0.95, 0.92),
            "health_fill": rgba(0.82, 0.20, 0.22), "health_ghost": rgba(0.98, 0.85, 0.70), "stamina_fill": rgba(0.42, 0.78, 0.36),
            "stamina_ghost": rgba(0.24, 0.42, 0.20), "hotbar_key": rgba(0.80, 0.80, 0.78, 0.9), "hotbar_selected": rgba(0.96, 0.78, 0.40),
            "log_bg": rgba(0.0, 0.0, 0.0, 0.0), "log_text": rgba(0.92, 0.91, 0.88), "bar_width": 360.0, "bar_height": 22.0,
            "bar_spacing": 10.0, "ghost_delay": 0.35, "ghost_speed": 45.0, "log_line_height": 28.0, "log_hold_seconds": 7.0,
            "log_fade_seconds": 1.2, "corner_margin": 32.0},
    # Soft, modern shapes: rounded panels with a hairline gold-grey outline that float on a
    # shadow, pill-shaped slider tracks.
    "shape": {"panel_radius": 16.0, "button_radius": 10.0, "control_radius": 8.0, "bar_radius": 6.0, "slot_radius": 10.0,
              "border_width": 1.5, "shadow_size": 28.0, "shadow_color": rgba(0.0, 0.0, 0.0, 0.55)},
    "tooltip": {"bg": rgba(0.04, 0.05, 0.07, 0.96), "text": rgba(0.94, 0.93, 0.90), "padding_x": 14.0, "padding_y": 10.0,
                "max_width": 420.0, "delay": 0.4, "cursor_offset": 22.0},
}


def hx(code, a=None):
    """'#rrggbb' or '#rrggbbaa' -> rgba(); `a` overrides the alpha."""
    c = code.lstrip("#")
    r, g, b = (round(int(c[i:i + 2], 16) / 255.0, 3) for i in (0, 2, 4))
    alpha = a if a is not None else (round(int(c[6:8], 16) / 255.0, 3) if len(c) == 8 else 1.0)
    return rgba(r, g, b, alpha)


def variant(base, overrides):
    """`base` with `overrides` merged in, mapping by mapping (a deep copy; base is untouched)."""
    out = copy.deepcopy(base)
    for k, v in overrides.items():
        out[k] = variant(out[k], v) if isinstance(v, dict) and isinstance(out.get(k), dict) and "r" not in v else copy.deepcopy(v)
    return out


# Blender's default dark UI: neutral greys, #4772b3 blue for selection / active widgets, compact
# rows, Inter -- the same palette as the editor's own editor/themes/blender_dark.yaml, so a game
# styled with it reads as Blender.
BLENDER_THEME = variant(THEME, {
    "name": "blender",
    "panel": {"background": hx("#1d1d1d", 0.92), "panel": hx("#303030", 0.97), "panel_alt": hx("#282828", 0.97),
              "header_bar": hx("#3d3d3d"), "border": hx("#171717")},
    "text": {"primary": hx("#e6e6e6"), "secondary": hx("#a6a6a6"), "muted": hx("#707070"), "accent": hx("#71a8ff"),
             "success": hx("#78c968"), "warning": hx("#facc4c"), "info": hx("#71a8ff"), "selection": hx("#4772b3"),
             "size_title": 30.0, "size_heading": 21.0, "size_body": 17.0, "size_label": 17.0, "size_small": 14.0},
    "button": {"normal": hx("#545454"), "hover": hx("#656565"), "press": hx("#4772b3"), "disabled": hx("#545454", 0.45)},
    "button_primary": {"normal": hx("#4772b3"), "hover": hx("#5680c2"), "press": hx("#3a5f99"), "disabled": hx("#545454", 0.45)},
    "button_success": {"normal": hx("#4e8a43"), "hover": hx("#5c9c50"), "press": hx("#3f7036"), "disabled": hx("#545454", 0.45)},
    "slider": {"track": hx("#1d1d1d"), "fill": hx("#4772b3"), "handle": hx("#e6e6e6"), "handle_hover": hx("#ffffff"),
               "handle_press": hx("#c8c8c8"), "handle_disabled": hx("#e6e6e6", 0.4), "height": 22.0, "handle_width": 12.0},
    "toggle": {"bg": hx("#545454"), "bg_hover": hx("#656565"), "bg_press": hx("#4772b3"), "bg_disabled": hx("#545454", 0.45),
               "check": hx("#ffffff"), "size": 20.0},
    "spinbox": {"bg": hx("#545454"), "height": 28.0, "btn_width": 24.0},
    "combobox": {"bg": hx("#282828"), "popup_bg": hx("#181818", 0.97), "height": 28.0},
    "slot": {"bg": hx("#282828", 0.95), "border": hx("#171717"), "hover": hx("#656565"), "selected": hx("#4772b3"),
             "tooltip_bg": hx("#181818", 0.97), "tooltip_text": hx("#e6e6e6")},
    "focus": {"color": hx("#4772b3"), "fill": hx("#4772b3", 0.0), "thickness": 2.0, "padding": 2.0},
    "metrics": {"row_height": 30.0, "row_spacing": 6.0, "label_width": 220.0, "header_height": 28.0, "card_header_height": 40.0,
                "scrollbar_thickness": 8.0, "scroll_frame_padding": 6.0, "button_padding_x": 16.0, "button_min_width": 120.0,
                "section_spacing": 10.0, "dialog_padding": 14.0, "tab_height": 32.0, "tab_spacing": 2.0,
                "tab_indicator_height": 2.0, "scrim_alpha": 0.45},
    "tab": {"normal": hx("#282828"), "hover": hx("#3d3d3d"), "press": hx("#1d1d1d"), "selected": hx("#4772b3"),
            "selected_hover": hx("#5680c2"), "indicator": hx("#71a8ff")},
    "collapsible": {"header": hx("#3d3d3d"), "hover": hx("#4a4a4a"), "press": hx("#303030"), "rail_width": 2.0, "indent": 14.0},
    "menu": {"bar": hx("#232323"), "title_normal": hx("#232323"), "title_hover": hx("#4a4a4a"), "title_press": hx("#4772b3"),
             "popup_bg": hx("#181818", 0.97), "item_normal": hx("#181818", 0.0), "item_hover": hx("#4772b3"),
             "item_press": hx("#3a5f99"), "separator": hx("#000000", 0.35), "bar_height": 28.0, "item_height": 26.0,
             "title_padding_x": 10.0},
    "hud": {"bar_bg": hx("#1d1d1d", 0.85), "bar_border": hx("#171717"), "bar_text": hx("#e6e6e6"),
            "health_fill": hx("#e04a4a"), "health_ghost": hx("#f2c6a0"), "stamina_fill": hx("#78c968"),
            "stamina_ghost": hx("#3f6e36"), "hotbar_key": hx("#a6a6a6", 0.9), "hotbar_selected": hx("#4772b3"),
            "log_text": hx("#e6e6e6"), "bar_width": 300.0, "bar_height": 16.0, "bar_spacing": 6.0, "log_line_height": 22.0,
            "corner_margin": 20.0},
    "tooltip": {"bg": hx("#181818", 0.97), "text": hx("#e6e6e6"), "padding_x": 10.0, "padding_y": 6.0, "max_width": 360.0},
    # Blender's widget roundness: softly rounded buttons and fields, rounder panels and
    # popovers, the near-black widget outline, a modest drop shadow under floating regions.
    "shape": {"panel_radius": 12.0, "button_radius": 7.0, "control_radius": 7.0, "bar_radius": 5.0, "slot_radius": 7.0,
              "border_width": 1.0, "shadow_size": 16.0, "shadow_color": rgba(0.0, 0.0, 0.0, 0.5)},
})

# Unity's dark (Pro) editor skin: mid greys, #3a79bb blue, flat 2.5px-rounded controls, compact
# rows -- the palette of editor/themes/unity_dark.yaml.
UNITY_THEME = variant(BLENDER_THEME, {
    "name": "unity",
    "panel": {"background": hx("#191919", 0.92), "panel": hx("#383838", 0.97), "panel_alt": hx("#2e2e2e", 0.97),
              "header_bar": hx("#3e3e3e"), "border": hx("#232323")},
    "text": {"primary": hx("#d2d2d2"), "secondary": hx("#9a9a9a"), "muted": hx("#6b6b6b"), "accent": hx("#4c97e6"),
             "info": hx("#4c97e6"), "selection": hx("#2c5d87")},
    "button": {"normal": hx("#585858"), "hover": hx("#676767"), "press": hx("#46607c"), "disabled": hx("#585858", 0.45)},
    "button_primary": {"normal": hx("#3a79bb"), "hover": hx("#4687cc"), "press": hx("#2c5d87"), "disabled": hx("#585858", 0.45)},
    "button_success": {"normal": hx("#3f8a4f"), "hover": hx("#4b9c5c"), "press": hx("#326e3f"), "disabled": hx("#585858", 0.45)},
    "slider": {"track": hx("#5e5e5e"), "fill": hx("#3a79bb"), "handle": hx("#999999"), "handle_hover": hx("#b4b4b4"),
               "handle_press": hx("#c8c8c8"), "handle_disabled": hx("#999999", 0.4), "height": 20.0, "handle_width": 10.0},
    "toggle": {"bg": hx("#2a2a2a"), "bg_hover": hx("#303030"), "bg_press": hx("#46607c"), "bg_disabled": hx("#2a2a2a", 0.45),
               "check": hx("#d2d2d2"), "size": 18.0},
    "spinbox": {"bg": hx("#2a2a2a")},
    "combobox": {"bg": hx("#515151"), "popup_bg": hx("#282828", 0.97)},
    "slot": {"bg": hx("#2e2e2e", 0.95), "border": hx("#232323"), "hover": hx("#676767"), "selected": hx("#3a79bb"),
             "tooltip_bg": hx("#282828", 0.97), "tooltip_text": hx("#d2d2d2")},
    "focus": {"color": hx("#3a79bb"), "fill": hx("#3a79bb", 0.0)},
    "metrics": {"row_height": 28.0, "tab_height": 30.0, "tab_spacing": 0.0, "tab_indicator_height": 2.0},
    "tab": {"normal": hx("#282828"), "hover": hx("#303030"), "press": hx("#191919"), "selected": hx("#3c3c3c"),
            "selected_hover": hx("#474747"), "indicator": hx("#3a79bb")},
    "collapsible": {"header": hx("#3e3e3e"), "hover": hx("#474747"), "press": hx("#383838")},
    "menu": {"bar": hx("#191919"), "title_normal": hx("#191919"), "title_hover": hx("#474747"), "title_press": hx("#2c5d87"),
             "popup_bg": hx("#282828", 0.97), "item_normal": hx("#282828", 0.0), "item_hover": hx("#2c5d87"),
             "item_press": hx("#46607c"), "separator": hx("#000000", 0.35)},
    "hud": {"bar_bg": hx("#191919", 0.85), "bar_border": hx("#232323"), "bar_text": hx("#d2d2d2"),
            "hotbar_key": hx("#9a9a9a", 0.9), "hotbar_selected": hx("#3a79bb"), "log_text": hx("#d2d2d2")},
    "tooltip": {"bg": hx("#282828", 0.97), "text": hx("#d2d2d2")},
    # Unity's flat skin: barely-rounded 3 px corners everywhere, a 1 px dark outline, a tight shadow.
    "shape": {"panel_radius": 4.0, "button_radius": 3.0, "control_radius": 3.0, "bar_radius": 2.0, "slot_radius": 3.0,
              "border_width": 1.0, "shadow_size": 8.0, "shadow_color": rgba(0.0, 0.0, 0.0, 0.4)},
})

THEMES = {
    "default": (THEME, "The default game UI theme (full-resolution, dark slate with a gold accent)."),
    "blender": (BLENDER_THEME, "Blender's dark UI: neutral greys, #4772b3 blue, compact rows."),
    "unity": (UNITY_THEME, "Unity's dark (Pro) editor skin: mid greys, #3a79bb blue, compact rows."),
}


# ---------------------------------------------------------------------------------------------
# Screens
# ---------------------------------------------------------------------------------------------

def hud():
    return canvas_root("hud", [
        obj("Vitals", rect("TopLeft", (40, -40), (420, 120)), comp("HudCorner", flow="vertical", spacing=10), children=[
            obj("Health", rect(size=(420, 28)), comp("StatBar", role="health", max=100, value=100, bar_height=24)),
            obj("Stamina", rect(size=(420, 20)), comp("StatBar", role="stamina", max=100, value=100, bar_height=16, show_label=False)),
            obj("Mana", rect(size=(420, 20)), comp("StatBar", role="mana", max=60, value=60, bar_height=16, show_label=False)),
        ]),
        obj("Quest", rect("TopRight", (-40, -40), (480, 110)), comp("HudCorner", flow="vertical", spacing=6), children=[
            obj("QuestTitle", rect(size=(480, 36)), comp("ThemedText", text="The Lost Lantern", font_role="Heading",
                                                         text_role="Accent", align="Right")),
            obj("QuestStep", rect(size=(480, 30)), comp("ThemedText", text="Search the old mill for clues", font_role="Body",
                                                        text_role="Primary", align="Right")),
        ]),
        obj("MessageLog", rect("BottomLeft", (40, 140), (560, 200)), comp("MessageLog", max_lines=6, boxed=False)),
        obj("Hotbar", rect("BottomCenter", (0, 36), (660, 60)), comp("Hotbar", count=10, slot_size=60, spacing=6, key_labels=True)),
        obj("Prompts", rect("BottomRight", (-40, 40), (560, 44)), comp("PromptBar", prompts=[
            {"action": "Confirm", "label": "Interact"}, {"action": "Menu", "label": "Map"}, {"action": "Back", "label": "Menu"}])),
    ])


def main_menu():
    return canvas_root("main_menu", [
        obj("Backdrop", rect("StretchAll", size=(0, 0)), comp("ThemedPanel", style="background")),
        obj("Title", rect("TopCenter", (0, -180), (1200, 120)), comp("ThemedText", text="Ashes of the Old Kingdom",
                                                                   font_role="Title", size=72, text_role="Accent", align="Center")),
        obj("Subtitle", rect("TopCenter", (0, -290), (1200, 40)), comp("ThemedText", text="A toyengine RPG", font_role="Body",
                                                                      text_role="Secondary", align="Center")),
        obj("Menu", rect("MiddleCenter", (0, -120), (420, 330)),
            comp("MenuList", items=items(("NewGame", "New Game"), ("Continue", "Continue"), ("Settings", "Settings"),
                                         ("Quit", "Quit"), primary="NewGame"), button_height=64, spacing=14)),
        obj("Version", rect("BottomRight", (-24, 20), (300, 30)), comp("ThemedText", text="v0.1", font_role="Caption",
                                                                       text_role="Muted", align="Right")),
    ], sort_order=10)


def pause_menu():
    return canvas_root("pause_menu", [
        obj("Dimmer", rect("StretchAll", size=(0, 0)), comp("Image", color=rgba(0, 0, 0, 0.6), raycast_target=True)),
        obj("Pause", rect("MiddleCenter", size=(520, 470)), comp("Window", title="Paused", title_align="Center", padding=32), children=[
            obj("Menu", rect(size=(456, 300)),
                comp("MenuList", items=items(("Resume", "Resume"), ("Settings", "Settings"), ("SaveGame", "Save Game"),
                                             ("QuitToMenu", "Quit to Menu"), primary="Resume"), button_height=58, spacing=12)),
        ]),
    ], sort_order=50)


def dialog_box():
    return canvas_root("dialog_box", [
        obj("Box", rect("BottomCenter", (0, 40), (1500, 300)), comp("ThemedPanel", style="panel"), children=[
            obj("Portrait", rect("MiddleLeft", (24, 0), (240, 240)), comp("Image", color=rgba(0.24, 0.26, 0.32))),
            obj("Speaker", rect("TopLeft", (290, -24), (700, 44)), comp("ThemedText", text="Elder Maren", font_role="Heading",
                                                                       text_role="Accent")),
            obj("Line", rect("TopLeft", (290, -78), (760, 190)), comp("ThemedText", wrap=True, valign="Top", font_role="Body",
                text="The mill has been quiet since the lantern went out. Find it, and the river spirits may listen again.")),
            obj("Choices", rect("MiddleRight", (-24, 0), (400, 240)),
                comp("MenuList", items=items(("Accept", "I'll find it."), ("AskMore", "Tell me more."), ("Leave", "Not now.")),
                     button_height=56, spacing=12)),
            obj("Continue", rect("BottomRight", (-440, 16), (220, 30)), comp("ThemedText", text="Choose a reply", font_role="Caption",
                                                                             text_role="Muted", align="Right")),
        ]),
    ], sort_order=20)


def inventory():
    return canvas_root("inventory", [
        obj("Inventory", rect("MiddleCenter", size=(1240, 760)),
            comp("Window", title="Inventory", close_button=True, layout="none", padding=24), children=[
                obj("Tabs", rect("StretchAll", (-200, 30), (-400, -60)), comp("TabView", tabs=["Bag", "Equipment"]), children=[
                    obj("Bag", rect(size=(740, 560)), comp("ItemGrid", rows=6, cols=10, slot_size=64, spacing=8, align="top")),
                    obj("Equipment", rect(size=(740, 560)), comp("ItemGrid", rows=2, cols=4, slot_size=96, spacing=16, align="top")),
                ]),
                obj("Details", rect("StretchAll", (420, 30), (-848, -60)), comp("ThemedPanel", style="panel_alt"), children=[
                    obj("ItemName", rect("TopLeft", (20, -20), (340, 40)), comp("ThemedText", text="Rusty Lantern", font_role="Heading",
                                                                              text_role="Accent")),
                    obj("ItemDescription", rect("TopLeft", (20, -72), (340, 300)), comp("ThemedText", wrap=True, valign="Top",
                        text="Cold iron and cracked glass. It still smells of lamp oil.", font_role="Body", text_role="Secondary")),
                    obj("Use", rect("BottomCenter", (0, 24), (300, 56)), comp("ThemedButton", label="Use", role="Primary")),
                ]),
                obj("Gold", rect("BottomLeft", (0, 0), (400, 40)), comp("ThemedText", text="Gold  1,240", font_role="Numeric",
                                                                       text_role="Accent")),
            ]),
    ], sort_order=30)


def settings():
    def row(name, kind, label, **kw):
        return obj(name, rect(size=(760, 44)), comp("SettingRow", kind=kind, label=label, **kw))
    return canvas_root("settings", [
        obj("Settings", rect("MiddleCenter", size=(880, 760)), comp("Window", title="Settings", close_button=True, padding=32), children=[
            obj("Tabs", rect(size=(816, 560)), comp("TabView", tabs=["Audio", "Video", "Gameplay"]), children=[
                obj("Audio", rect(size=(800, 400)), comp("VerticalLayoutGroup", spacing=12, child_force_expand_height=False), children=[
                    row("MasterVolume", "slider", "Master Volume", min=0, max=1, value=0.8),
                    row("MusicVolume", "slider", "Music", min=0, max=1, value=0.6),
                    row("EffectsVolume", "slider", "Effects", min=0, max=1, value=0.9),
                ]),
                obj("Video", rect(size=(800, 400)), comp("VerticalLayoutGroup", spacing=12, child_force_expand_height=False), children=[
                    row("Fullscreen", "toggle", "Fullscreen", is_on=True),
                    row("VSync", "toggle", "VSync", is_on=True),
                    row("Quality", "dropdown", "Quality", items=["Low", "Medium", "High", "Ultra"], selected_index=2),
                    row("Resolution", "dropdown", "Resolution", items=["1280 x 720", "1920 x 1080", "2560 x 1440"], selected_index=1),
                ]),
                obj("Gameplay", rect(size=(800, 400)), comp("VerticalLayoutGroup", spacing=12, child_force_expand_height=False), children=[
                    row("Difficulty", "dropdown", "Difficulty", items=["Story", "Normal", "Hard"], selected_index=1),
                    row("Subtitles", "toggle", "Subtitles", is_on=True),
                    row("CameraShake", "slider", "Camera Shake", min=0, max=1, value=0.5),
                ]),
            ]),
            obj("Footer", rect(size=(816, 64)), comp("ActionBar", items=items(("Back", "Back"), ("Apply", "Apply"), primary="Apply"),
                                                     button_height=56)),
        ]),
    ], sort_order=40)


SCREENS = {
    "hud": (hud, "The in-game HUD: vitals (top left), quest tracker (top right), message log,\nhotbar and controller prompts."),
    "main_menu": (main_menu, "The title screen: NewGame / Continue / Settings / Quit buttons."),
    "pause_menu": (pause_menu, "The pause screen: Resume / Settings / SaveGame / QuitToMenu buttons over a dimmer."),
    "dialog_box": (dialog_box, "NPC dialogue: Portrait, Speaker, Line, and Choices (Accept / AskMore / Leave)."),
    "inventory": (inventory, "The inventory: Bag and Equipment grids (bind_inventory), item Details, Gold."),
    "settings": (settings, "Settings: Audio / Video / Gameplay tabs of SettingRows, Back / Apply."),
}


def main():
    os.makedirs(os.path.join(ASSETS, "fonts"), exist_ok=True)
    for dest in (os.path.join(TEMPLATES, "fonts"), os.path.join(ASSETS, "fonts")):
        os.makedirs(dest, exist_ok=True)
        for src, dst in (("Inter-Regular.ttf", "inter_regular.ttf"), ("Inter-SemiBold.ttf", "inter_semibold.ttf")):
            target = os.path.join(dest, dst)
            if not os.path.exists(target):
                shutil.copyfile(os.path.join(UICOOPA_FONTS, src), target)
                print("wrote", os.path.relpath(target, REPO))
        lic = os.path.join(dest, "LICENSE-OFL.txt")
        if not os.path.exists(lic):
            shutil.copyfile(os.path.join(UICOOPA_FONTS, "LICENSE-OFL.txt"), lic)

    def theme_header(summary):
        return (summary + "\n"
                "Every themed widget -- Window, MenuList, StatBar, SettingRow... -- takes its colours, fonts\n"
                "and sizes from here. Generated by tools/gen_ui_templates.py; edit freely (open it in the\n"
                "editor's Themes tab). Fonts resolve relative to this file.")
    # In the repo's assets/, each screen sits in its tag folder (ui/menus/, ui/hud/); the editor's
    # templates stay flat (a template is copied into a project's ui/ under the new asset's tags).
    screen_tags = {"main_menu": "menus", "pause_menu": "menus", "settings": "menus",
                   "hud": "hud", "inventory": "hud", "dialog_box": "hud"}
    for base in (TEMPLATES, os.path.join(ASSETS, "ui")):
        for name, (theme, summary) in THEMES.items():
            write(os.path.join(base, "themes", name + ".yaml"), theme, theme_header(summary))
        for name, (fn, doc) in SCREENS.items():
            folder = os.path.join(base, screen_tags.get(name, "")) if base != TEMPLATES else base
            write(os.path.join(folder, name + ".yaml"), fn(),
                  f"{doc}\n\nA UI asset (open it in the editor's UI tab). Authored at 1920 x 1080.\n"
                  "Generated by tools/gen_ui_templates.py.")


if __name__ == "__main__":
    main()
