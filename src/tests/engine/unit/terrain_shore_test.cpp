/**
 * @file terrain_shore_test.cpp
 * @brief grade_tile_shore() (toyengine/world/terrain_shore.h) on the FLAT grid: that it grades a
 *        terraced shoreline without building a face the ground did not already have, that it
 *        leaves a cliff standing, and that re-grading graded ground changes nothing.
 *
 * Every test builds a `TerraData` by hand and passes no topology, which is the flat grid -- the
 * path an ordinary tile world takes. The cosmetic dither is off (`jitter_steps = 0`) so each
 * assertion is against an exact result rather than one depending on which lattice cell a tile
 * falls into, and `tile_size == height_step == 1` so a slope in steps per tile is a plain tangent.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/world/terrain_field.h>
#include <toyengine/world/terrain_params.h>
#include <toyengine/world/terrain_shore.h>
#include <toyengine/world/terrain_water.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("terrain_shore");

using toy::world::borrow;
using toy::world::IRect;
using toy::world::ShoreProfile;
using toy::world::TerraData;
using toy::world::TerraGeometry;
using toy::world::TerrainField;
using toy::world::TerrainParams;
using toy::world::TileCell;
using toy::world::TileKind;
using toy::world::WaterKind;
using toy::world::side_kind_for;

namespace {

/** @brief A flat tile field, `w` by `h`, 1 m tiles and 1 m steps. */
struct Fixture {
    std::int32_t w, h;
    TerraData data;
    TerrainParams params;

    Fixture(std::int32_t w_, std::int32_t h_) : w(w_), h(h_) {
        TerraGeometry g;
        g.tiles_x = w_;
        g.tiles_y = h_;
        g.tile_size = 1.0f;
        g.height_step = 1.0f;
        data = TerraData(g);
        params.tile_size = 1.0f;
        params.height_step = 1.0f;
        params.shore.enabled = true;
        params.shore.jitter_steps = 0.0f;
        for (std::int32_t y = 0; y < h_; ++y)
            for (std::int32_t x = 0; x < w_; ++x) set_dry(x, y, 40, TileKind::Grass);
    }

    const TileCell& at(std::int32_t x, std::int32_t y) const { return data.at(x, y); }

    void set_dry(std::int32_t x, std::int32_t y, int steps, TileKind top) {
        TileCell& c = data.at(x, y);
        c.water_kind = 0;
        c.water_level = 0.0f;
        c.steps = static_cast<std::int16_t>(steps);
        c.set_top_kind(top);
        c.set_side_kind(side_kind_for(top));
    }

    void set_wet(std::int32_t x, std::int32_t y, int bed_steps, float terrace, WaterKind kind) {
        TileCell& c = data.at(x, y);
        c.water_kind = static_cast<std::uint8_t>(kind);
        c.water_level = terrace;  // height_step == 1, so metres == steps
        c.steps = static_cast<std::int16_t>(bed_steps);
        c.set_top_kind(TileKind::Stone);
    }

    /** @brief Grades the whole field on the flat grid -- no topology, as a tile world has none. */
    void grade() {
        TerrainField field = borrow(data);
        toy::world::grade_tile_shore(field, IRect{0, 0, w, h}, params, nullptr);
    }
};

/**
 * @brief The worst riser the grading ADDED: over every 8-adjacent dry pair, how far the graded
 *        rise exceeds `max(bound, the rise the baked ground already had)`.
 *
 * This, and not a distance, is the property the pass owes. A cone that stops where it meets the
 * ground leaves `min(baked, cone) == baked` at the join, so every riser in the result is either
 * one the terrain already had or one of the cone's own bounded steps -- and checking that needs
 * no knowledge of where a cone stopped, which is exactly why it catches a wall wherever one is.
 */
int worst_added_rise(const Fixture& f, const std::vector<TileCell>& baked, float bound,
                     std::string& worst) {
    int over = 0;
    for (std::int32_t y = 0; y < f.h; ++y) {
        for (std::int32_t x = 0; x < f.w; ++x) {
            const TileCell& c = f.at(x, y);
            if (c.wet()) continue;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    const std::int32_t nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= f.w || ny >= f.h) continue;
                    const TileCell& o = f.at(nx, ny);
                    if (o.wet()) continue;
                    const int rise = static_cast<int>(o.steps) - static_cast<int>(c.steps);
                    if (rise <= 0) continue;
                    const float step = (dx != 0 && dy != 0) ? 1.41421356f : 1.0f;
                    const int baked_rise =
                        static_cast<int>(baked[static_cast<std::size_t>(ny) * f.w + nx].steps) -
                        static_cast<int>(baked[static_cast<std::size_t>(y) * f.w + x].steps);
                    const int allowed = std::max(static_cast<int>(std::ceil(bound * step)), baked_rise);
                    if (rise <= allowed) continue;
                    if (rise - allowed > over) {
                        worst = std::to_string(x) + "," + std::to_string(y) + " -> " +
                                std::to_string(nx) + "," + std::to_string(ny) + " (graded " +
                                std::to_string(c.steps) + " -> " + std::to_string(o.steps) + ")";
                    }
                    over = std::max(over, rise - allowed);
                }
            }
        }
    }
    return over;
}

/** @brief No dry tile may stand under the water terrace of any of its 8 neighbours. */
int tiles_under_their_banks(const Fixture& f) {
    int over = 0;
    for (std::int32_t y = 0; y < f.h; ++y) {
        for (std::int32_t x = 0; x < f.w; ++x) {
            const TileCell& c = f.at(x, y);
            if (c.wet()) continue;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    const std::int32_t nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= f.w || ny >= f.h) continue;
                    const TileCell& o = f.at(nx, ny);
                    if (o.wet() && static_cast<float>(c.steps) < o.water_level) ++over;
                }
            }
        }
    }
    return over;
}

int tiles_moved(const Fixture& f, const std::vector<TileCell>& baked) {
    int moved = 0;
    for (std::size_t i = 0; i < baked.size(); ++i)
        if (f.data.tiles()[i].steps != baked[i].steps) ++moved;
    return moved;
}

/** @brief A lake down the left edge at `terrace`, then a `rim`-step bank climbing 1 step a tile. */
void coast_with_rim(Fixture& f, int terrace, int rim) {
    for (std::int32_t y = 0; y < f.h; ++y) {
        f.set_wet(0, y, terrace - 1, static_cast<float>(terrace), WaterKind::Lake);
        for (std::int32_t x = 1; x < f.w; ++x) f.set_dry(x, y, terrace + rim + (x - 1), TileKind::Grass);
    }
}

} // namespace

COOPA_TEST(a_disabled_profile_changes_nothing) {
    Fixture f(48, 8);
    coast_with_rim(f, 40, 6);
    f.params.shore.enabled = false;
    const std::vector<TileCell> before = f.data.tiles();
    f.grade();
    expect(f.data.tiles() == before, "off by default: a terrain that wants bare terraces keeps them");
}

COOPA_TEST(grading_never_builds_a_riser_the_baked_ground_did_not_have) {
    Fixture f(48, 8);
    // A 6-step bank at the water's edge -- under cut_max_steps, so the cone attacks it rather than
    // refusing it as a cliff -- then ground climbing 1 step a tile, which is gentler than the
    // taper and so must be left exactly alone by a cone that respects the ground.
    coast_with_rim(f, 40, 6);
    const std::vector<TileCell> baked = f.data.tiles();
    f.grade();
    require(tiles_moved(f, baked) > 0, "the grading moved something, so the bound is not vacuous");
    std::string worst;
    const int over = worst_added_rise(f, baked, f.params.shore.cut_taper_slope_steps, worst);
    expect(over == 0, "no dry riser steeper than the ground already was, by " + std::to_string(over) +
                          " steps at " + worst);
    expect(tiles_under_their_banks(f) == 0, "and no dry tile ends up under the water beside it");
}

COOPA_TEST(a_cliff_at_the_waterline_keeps_its_height) {
    Fixture f(48, 8);
    // A 30-step wall straight out of the water: far over cut_max_steps, so the budget refuses it
    // and the crown stays put rather than the cone chewing the coast back into a ramp.
    for (std::int32_t y = 0; y < f.h; ++y) {
        f.set_wet(0, y, 19, 20.0f, WaterKind::Lake);
        for (std::int32_t x = 1; x < f.w; ++x) f.set_dry(x, y, 50, TileKind::Grass);
    }
    f.grade();
    expect(f.at(f.w - 1, 4).steps == 50, "the crown, far inland, is untouched");
    expect(static_cast<int>(f.at(1, 4).steps) >= 50 - static_cast<int>(f.params.shore.cut_max_steps),
          "and the face is cut by at most its budget, not demolished (" +
              std::to_string(f.at(1, 4).steps) + ")");
}

COOPA_TEST(grading_is_idempotent) {
    Fixture f(48, 8);
    coast_with_rim(f, 40, 6);
    f.grade();
    const std::vector<TileCell> once = f.data.tiles();
    f.grade();
    expect(f.data.tiles() == once, "a second pass over already-graded ground changes nothing");
}

COOPA_TEST(the_dither_is_stable_across_passes_too) {
    Fixture f(48, 8);
    coast_with_rim(f, 40, 6);
    f.params.shore.jitter_steps = 1.0f;  // the shipped default, which the other tests switch off
    f.grade();
    const std::vector<TileCell> once = f.data.tiles();
    f.grade();
    expect(f.data.tiles() == once, "the dither is a pure function of position, so it re-grades the same");
}

COOPA_TEST(a_bed_is_shelved_up_toward_its_margin) {
    Fixture f(24, 8);
    // A lake 12 tiles across with a slot bed: one step under its surface at the margins, falling
    // 2 a tile to 11 at the centre -- the submerged cross-section a cut channel actually leaves,
    // and steeper than the bed cone's own descent, which is what gives it something to shelve.
    for (std::int32_t y = 0; y < f.h; ++y) {
        for (std::int32_t x = 0; x < f.w; ++x) {
            if (x < 6 || x >= 18) { f.set_dry(x, y, 40, TileKind::Grass); continue; }
            const int d = std::min(x - 6, 17 - x);
            f.set_wet(x, y, 40 - 2 * d, 41.0f, WaterKind::Lake);
        }
    }
    const int deepest_before = 41 - f.at(12, 4).steps;
    f.grade();
    const int deepest_after = 41 - f.at(12, 4).steps;
    expect(deepest_after < deepest_before, "the centre of the slot is shallower after grading (" +
                                               std::to_string(deepest_before) + " -> " +
                                               std::to_string(deepest_after) + ")");
    int worst = 0;
    for (std::int32_t x = 7; x < 17; ++x)
        worst = std::max(worst, std::abs(static_cast<int>(f.at(x, 4).steps) -
                                         static_cast<int>(f.at(x + 1, 4).steps)));
    expect(worst <= 3, "the bed rises gradually rather than in one jump (" + std::to_string(worst) + ")");
    expect(tiles_under_their_banks(f) == 0, "and the bed never rose above its own water");
}

/**
 * @brief A long straight coast, so the only thing that can make the sand band vary along it is
 *        `beach_variation` -- and with it at 0 the band is the same width end to end.
 */
namespace {
/** @brief Width of the sand run at row `y`, walking inland from the water at x = 0. */
std::int32_t sand_width_(const Fixture& f, std::int32_t y) {
    std::int32_t n = 0;
    for (std::int32_t x = 1; x < f.w; ++x) {
        if (f.at(x, y).top_kind() != TileKind::Sand) break;
        ++n;
    }
    return n;
}

/** @brief A flat coast `h` rows long: `kind` down the x = 0 column, grass at its level inland. */
Fixture flat_coast_of_(std::int32_t w, std::int32_t h, WaterKind kind) {
    Fixture f(w, h);
    for (std::int32_t y = 0; y < h; ++y) {
        f.set_wet(0, y, 39, 40.0f, kind);
        for (std::int32_t x = 1; x < w; ++x) f.set_dry(x, y, 40, TileKind::Grass);
    }
    return f;
}

/** @brief The same, against a lake -- the shape every beach test below a map would see. */
Fixture flat_coast_(std::int32_t w, std::int32_t h) {
    return flat_coast_of_(w, h, WaterKind::Lake);
}
} // namespace

COOPA_TEST(a_uniform_profile_gives_one_width_of_sand_the_whole_way_along) {
    Fixture f = flat_coast_(32, 192);
    f.params.shore.beach_variation = 0.0f;
    f.grade();
    const std::int32_t first = sand_width_(f, 0);
    expect(first > 0, "a flat shore sands over at all (" + std::to_string(first) + ")");
    bool same = true;
    for (std::int32_t y = 1; y < f.h; ++y) same &= sand_width_(f, y) == first;
    expect(same, "and with no variation every row is the same width");
}

COOPA_TEST(beach_variation_makes_some_stretches_broad_and_others_bare) {
    Fixture f = flat_coast_(32, 192);
    f.params.shore.beach_variation = 0.6f;
    f.params.shore.beach_variation_tiles = 32.0f;
    f.grade();

    std::int32_t lo = 1 << 20, hi = 0;
    for (std::int32_t y = 0; y < f.h; ++y) {
        const std::int32_t n = sand_width_(f, y);
        lo = std::min(lo, n);
        hi = std::max(hi, n);
    }
    // The uniform band is beach_tiles + beach_jitter_tiles = 9 tiles; 0.6 of variation runs the
    // scale over [0.4, 1.6], so the widest stretches must beat the narrowest by a clear margin.
    expect(hi > lo + 2, "the band's width varies along the coast (" + std::to_string(lo) + ".." +
                            std::to_string(hi) + " tiles)");
    expect(hi > 9, "the broadest bays are wider than the uniform band (" + std::to_string(hi) + ")");
    expect(lo < 9, "and the barest stretches are narrower (" + std::to_string(lo) + ")");
}

COOPA_TEST(the_variation_is_smooth_rather_than_stepping_between_stretches) {
    Fixture f = flat_coast_(32, 192);
    f.params.shore.beach_variation = 0.6f;
    f.params.shore.beach_variation_tiles = 32.0f;
    f.grade();
    // A hard lattice would cut the band at a cell boundary -- a straight edge across the sand.
    // Blended, the width walks: no single row may jump by more than a tile or two against its
    // neighbour, over a period thirty-two tiles long.
    std::int32_t worst = 0;
    for (std::int32_t y = 1; y < f.h; ++y) {
        worst = std::max(worst, std::abs(sand_width_(f, y) - sand_width_(f, y - 1)));
    }
    expect(worst <= 2, "the width walks rather than stepping (worst jump " + std::to_string(worst) + ")");
}

COOPA_TEST(a_low_soft_shore_sands_over_but_a_high_one_does_not) {
    Fixture f(32, 8);
    for (std::int32_t y = 0; y < f.h; ++y) {
        f.set_wet(0, y, 39, 40.0f, WaterKind::Lake);
        for (std::int32_t x = 1; x < f.w; ++x) {
            // Flat grass at the water's own level, and a snow peak four tiles in.
            const bool peak = x >= 4 && x < 8;
            f.set_dry(x, y, peak ? 50 : 40, peak ? TileKind::Snow : TileKind::Grass);
        }
    }
    f.grade();
    expect(f.at(1, 4).top_kind() == TileKind::Sand, "low grass right beside the water sands over");
    expect(f.at(5, 4).top_kind() == TileKind::Snow, "snow well above the water never does");
    expect(f.at(f.w - 1, 4).top_kind() == TileKind::Grass, "and grass outside the band stays grass");
}


/**
 * @brief `beach_repair_only` leaves ground this pass did not move to whatever classified it.
 *
 * A dead-flat shore at the water's own level gives every cone nothing to do, so the pass moves
 * no tile on it -- and a terrain baked from a map has already had its beaches drawn there by the
 * map. Restricted, the band adds nothing; unrestricted it sands the same ground regardless,
 * which over a classified coast is the floor that flattens it.
 */
COOPA_TEST(beach_repair_only_leaves_ground_the_pass_did_not_move_alone) {
    Fixture restricted = flat_coast_(32, 32);
    restricted.params.shore.beach_repair_only = true;
    restricted.grade();
    std::int32_t widest = 0;
    for (std::int32_t y = 0; y < restricted.h; ++y) {
        widest = std::max(widest, sand_width_(restricted, y));
    }
    expect(widest == 0, "a shore the pass never touched keeps the classification it came with (" +
                            std::to_string(widest) + " tiles of sand)");

    Fixture drawn = flat_coast_(32, 32);
    drawn.grade();
    expect(sand_width_(drawn, 0) > 0, "and the same shore DOES sand over with the band unrestricted");
}

/**
 * @brief A river bank sands over even when the band is restricted to repaired ground.
 *
 * A map classifies a shore from the cells that hold water, and a river runs along the edge
 * between two cells rather than filling one -- so no cell beside it is ever a water cell and its
 * banks are never classified. The band is the only thing that sands them, restricted or not.
 */
COOPA_TEST(a_river_bank_sands_over_even_when_the_band_is_restricted) {
    Fixture f = flat_coast_of_(32, 32, WaterKind::River);
    f.params.shore.beach_repair_only = true;
    f.grade();
    expect(sand_width_(f, 0) > 0, "the river's bank still gets its sand (" +
                                      std::to_string(sand_width_(f, 0)) + " tiles)");
}
