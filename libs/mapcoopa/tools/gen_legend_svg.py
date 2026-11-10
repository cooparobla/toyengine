"""Generates the colour swatches and the legend tables in this repo's README.

Standalone generator, not part of the C++ build -- run it manually
(`python3 tools/gen_legend_svg.py`) whenever `BiomePalette` in
coopa/maps/map_config.h changes colour. Requires nothing but the standard library.

It writes two things from one source, which is the whole point of it existing: the
43 swatch files under assets/svg/, and the two markdown tables in README.md that
reference them. A legend is only useful if it is true, and keeping three artefacts
(palette, swatches, README) in agreement by hand across 33 biomes and 10 overlays
is not something anyone does reliably.

`legend/readme_legend_matches_the_palette` (tests/legend_test.cpp) is the other half of the
arrangement: this script makes the three agree, that test proves they still do and
fails the build if they have drifted. Running this on an unchanged palette must
leave the tree byte-identical -- it is deliberately idempotent, so `git diff` after
a run is the check that what is committed matches what the palette says.

The palette is read out of the C++ header by regular expression rather than through
a compiled helper. That is the trade this repo chose: a tiny script with no build
step, against a parser that would break if the header's formatting changed. The
checks in `_parse_*` below exist to make such a break loud and immediate rather
than silent -- a partial run would leave a stale README that the C++ test then
blames on the palette.
"""

import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
"""This repository's root; every path below is resolved from it, not the caller's cwd."""

CONFIG_HEADER = REPO_ROOT / "coopa" / "maps" / "map_config.h"
"""Declares `BiomePalette` -- the biome colour array and the overlay colour fields."""

BIOME_HEADER = REPO_ROOT / "coopa" / "maps" / "biome.h"
"""Declares `biome_name()`, which maps each `Biome` to its stable `snake_case` identity."""

SWATCH_DIR = REPO_ROOT / "assets" / "svg"
"""Where the swatches are written; one file per palette entry, named after its key."""

README = REPO_ROOT / "README.md"
"""Carries the two legend tables this script rewrites in place."""

BIOME_COUNT = 33
"""Expected `k_biome_count`. Asserted rather than inferred: finding fewer colours than
this means the regex stopped matching partway through the array, and writing a short
legend is worse than refusing to write one."""

SWATCH = (
    '<svg xmlns="http://www.w3.org/2000/svg" width="46" height="18" role="img" '
    'aria-label="{label}">'
    '<rect x="0.5" y="0.5" width="45" height="17" rx="3" fill="{hex}" '
    'stroke="#808080" stroke-opacity="0.55"/></svg>\n'
)
"""One swatch.

The outline is a 55%-opacity grey rather than any fixed colour because the same file
has to read on both of GitHub's themes. Without it `snow` (#FFFAFA), `salt_flat`
(#EEEEE6) and `background` (#FFFFFF) are invisible against a white README, and
`bridge` (#706C66) disappears against GitHub's dark background. A translucent
grey darkens the pale ones and lightens the dark ones, so every swatch keeps an edge
either way.

Colours are NOT rendered as inline HTML or as a `#RRGGBB` code alone, which would need
no files at all: GitHub renders its colour chips only in issues, pull requests and
discussions -- never in a README -- and its markdown sanitiser strips both the `style`
attribute and `data:` URIs. Real image files are the only thing that actually shows a
colour in a repository's front page.
"""

OVERLAYS = [
    ("river_color", "river", "River",
     "5 m plus 2 m per unit of volume, along a smoothed centreline"),
    ("trail_color", "trail", "Trail", "3 m wide; a spur off the network"),
    ("road_color", "road", "Road", "6 m wide"),
    ("highway_color", "highway", "Highway", "10 m wide; the busiest stretches"),
    ("bridge_color", "bridge", "Bridge",
     "Stone parapet drawn square across the road"),
    ("building_color", "building", "Building", "Rotated quad, 7-14 m per side, one per dwelling"),
    ("town_color", "settlement", "Settlement",
     "Square marker; 25 m capital, 17 m town, 11 m village"),
    ("landmark_natural_color", "landmark-natural", "Natural landmark",
     "Diamond; 21 m for a region's wonder, 13 m otherwise"),
    ("landmark_built_color", "landmark-built", "Built landmark", "Square, 13 m"),
    ("background_color", "background", "Background", "Whatever no cell covers"),
]
"""The overlay rows, in the order `MapLayers` draws them.

`(palette field, swatch name, display label, shape description)`. Every row here has its own
palette field and its own swatch.

The shape column is prose that cannot be derived from the palette, so it lives here.
Sizes are in METRES, which at the default one pixel per metre is also the pixel
count. Marker sizes come from `MapLayers::marker_radius_()` and `draw_landmark_()`, which size
themselves in metres and draw a square of side `2r+1`.
"""


def _hex(red: int, green: int, blue: int) -> str:
    """Formats an 0-255 RGB triple the way the README legend spells it.

    Args:
        red: Red component, 0-255.
        green: Green component, 0-255.
        blue: Blue component, 0-255.

    Returns:
        str: An uppercase `#RRGGBB` string.
    """
    return "#{:02X}{:02X}{:02X}".format(red, green, blue)


def _parse_biome_colors(source: str) -> list:
    """Reads `BiomePalette::biome_colors` out of map_config.h.

    Args:
        source: The full text of coopa/maps/map_config.h.

    Returns:
        list: `(enum_key, (r, g, b))` pairs, in declaration order.

    Raises:
        SystemExit: If the array cannot be found, or holds other than `BIOME_COUNT`
            entries -- see `BIOME_COUNT`'s own note on why this is fatal.
    """
    if "biome_colors = {" not in source:
        sys.exit("gen_legend_svg: could not find BiomePalette::biome_colors in "
                 "{}".format(CONFIG_HEADER))
    block = source.split("biome_colors = {", 1)[1].split("};", 1)[0]

    # Every entry is `glm::vec3(r, g, b),  // EnumName` -- the trailing comment is
    # what ties a colour to its biome, since the array is positional.
    entries = re.findall(r"glm::vec3\((\d+),\s*(\d+),\s*(\d+)\)[,\s]*//\s*(\w+)", block)
    if len(entries) != BIOME_COUNT:
        sys.exit("gen_legend_svg: expected {} biome colours, matched {}. The array's "
                 "formatting has changed -- fix the pattern rather than writing a "
                 "short legend.".format(BIOME_COUNT, len(entries)))

    return [(key, (int(r), int(g), int(b))) for r, g, b, key in entries]


def _parse_biome_names(source: str) -> dict:
    """Reads `biome_name()`'s enum-to-identifier mapping out of biome.h.

    Args:
        source: The full text of coopa/maps/biome.h.

    Returns:
        dict: Enum key (e.g. `TemperateDesert`) to `snake_case` name.

    Raises:
        SystemExit: If fewer than `BIOME_COUNT` cases are found.
    """
    names = dict(re.findall(r'case Biome::(\w+):\s*return "([a-z_]+)";', source))
    if len(names) < BIOME_COUNT:
        sys.exit("gen_legend_svg: expected {} cases in biome_name(), matched "
                 "{}.".format(BIOME_COUNT, len(names)))
    return names


def _parse_overlay_color(source: str, field: str) -> tuple:
    """Reads one named `glm::vec3` colour field of `BiomePalette`.

    Args:
        source: The full text of coopa/maps/map_config.h.
        field: The member name, e.g. `river_color`.

    Returns:
        tuple: The `(r, g, b)` components as ints.

    Raises:
        SystemExit: If the field is absent or not a plain three-component literal.
    """
    found = re.search(r"\b{} = glm::vec3\((\d+),\s*(\d+),\s*(\d+)\);".format(field), source)
    if not found:
        sys.exit("gen_legend_svg: could not read BiomePalette::{} from {}".format(
            field, CONFIG_HEADER))
    return int(found.group(1)), int(found.group(2)), int(found.group(3))


def _display_name(slug: str) -> str:
    """Turns a `snake_case` identifier into the legend's display label.

    Only the first word is capitalised, so the column reads as prose
    (`Temperate deciduous forest`) rather than as a type name.

    Args:
        slug: A `snake_case` biome name.

    Returns:
        str: The display label.
    """
    words = slug.split("_")
    return " ".join([words[0].capitalize()] + words[1:])


def _write_swatch(name: str, hex_value: str) -> None:
    """Writes one swatch file.

    Args:
        name: The file's stem, which is also what the README links to.
        hex_value: The fill, as `#RRGGBB`.
    """
    label = "{} {}".format(name.replace("_", " ").replace("-", " "), hex_value)
    path = SWATCH_DIR / "{}.svg".format(name)
    path.write_text(SWATCH.format(hex=hex_value, label=label))


def _replace_between(text: str, header: str, terminator: str, replacement: str) -> str:
    """Swaps the span from a table's header row up to the paragraph that follows it.

    Anchored on the README's own prose rather than on line numbers, so editing the
    surrounding text does not silently move the target.

    Args:
        text: The README's full contents.
        header: The table's header row, matched exactly.
        terminator: The first prose line after the table.
        replacement: The rebuilt table, without a trailing newline.

    Returns:
        str: The README with that table replaced.

    Raises:
        SystemExit: If either anchor is missing or they are out of order.
    """
    start = text.find(header)
    end = text.find(terminator)
    if start < 0 or end < 0 or end < start:
        sys.exit("gen_legend_svg: could not locate the legend table anchored on "
                 "'{}' in {}".format(header, README))
    return text[:start] + replacement + text[end:]


def main() -> None:
    """Regenerates every swatch and both README legend tables."""
    config_source = CONFIG_HEADER.read_text()
    biome_source = BIOME_HEADER.read_text()

    biome_colors = _parse_biome_colors(config_source)
    biome_names = _parse_biome_names(biome_source)

    SWATCH_DIR.mkdir(parents=True, exist_ok=True)

    biome_rows = ["| Colour | Biome | YAML name | Hex | RGB |", "|---|---|---|---|---|"]
    for key, (red, green, blue) in biome_colors:
        if key not in biome_names:
            sys.exit("gen_legend_svg: Biome::{} has a palette colour but no "
                     "biome_name() case".format(key))
        slug = biome_names[key]
        hex_value = _hex(red, green, blue)
        _write_swatch(slug, hex_value)
        biome_rows.append("| ![](assets/svg/{}.svg) | {} | `{}` | `{}` | {}, {}, {} |".format(
            slug, _display_name(slug), slug, hex_value, red, green, blue))

    overlay_rows = ["| Colour | Overlay | Hex | RGB | Shape |", "|---|---|---|---|---|"]
    for field, slug, label, shape in OVERLAYS:
        red, green, blue = _parse_overlay_color(config_source, field)
        hex_value = _hex(red, green, blue)
        _write_swatch(slug, hex_value)
        overlay_rows.append("| ![](assets/svg/{}.svg) | {} | `{}` | {}, {}, {} | {} |".format(
            slug, label, hex_value, red, green, blue, shape))

    readme = README.read_text()
    readme = _replace_between(readme, biome_rows[0], "\n\n`ocean` and `lake` share a colour",
                              "\n".join(biome_rows))
    readme = _replace_between(readme, overlay_rows[0], "\n\nThe three road tiers are told apart",
                              "\n".join(overlay_rows))
    README.write_text(readme)

    written = len({slug for _, slug, _, _ in OVERLAYS}) + len(biome_colors)
    print("Wrote {} swatches to {}".format(written, SWATCH_DIR.relative_to(REPO_ROOT)))
    print("Rewrote both legend tables in {}".format(README.relative_to(REPO_ROOT)))


if __name__ == "__main__":
    main()
