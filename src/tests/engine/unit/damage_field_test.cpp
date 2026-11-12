/**
 * @file damage_field_test.cpp
 * @brief toyengine/destruct/damage_field.h: a dot saturates only what it should, repeated hits
 *        on one spot plateau rather than exceeding 1, and saturated_plane() tells a flat slash
 *        streak (the cleave case) apart from a scattered set of isolated hits (the reject case).
 */

#include <coopa/testing/test.h>

#include <glm/glm.hpp>
#include <toyengine/destruct/damage_field.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("damage_field");

using toy::destruct::DamageField;
using coopa::physx::damage::ResolvedDamage;

COOPA_TEST(a_single_deposit_saturates_only_cells_inside_its_radius) {
    DamageField field;
    field.reset(glm::vec3(-1.0f), glm::vec3(1.0f), 10); // cell size 0.2

    ResolvedDamage dmg;
    dmg.radius = 0.3f;
    field.deposit(glm::vec3(0.0f), glm::vec3(0, 0, 1), glm::vec3(0.0f), dmg);

    expect(field.at(glm::vec3(0.0f)) > 0.0f, "damage_field: the hit point itself is saturated");
    expect(field.at(glm::vec3(0.9f, 0.9f, 0.9f)) == 0.0f,
          "damage_field: a far corner, well outside the radius, is untouched");
}

COOPA_TEST(repeated_deposits_on_one_spot_saturate_and_then_stop_growing) {
    DamageField field;
    field.reset(glm::vec3(-1.0f), glm::vec3(1.0f), 10);

    ResolvedDamage dmg;
    dmg.radius = 0.25f;
    const glm::vec3 point(0.1f, 0.1f, 0.1f);

    float previous = 0.0f;
    for (int i = 0; i < 6; ++i) {
        field.deposit(point, glm::vec3(0, 0, 1), glm::vec3(0.0f), dmg);
        const float now = field.at(point);
        expect(now >= previous, "damage_field: saturation never decreases across repeated hits");
        expect(now <= 1.0f, "damage_field: ...and never exceeds 1.0");
        previous = now;
    }
    expect(previous >= 0.999f, "damage_field: enough repeated hits fully saturate the spot");
}

COOPA_TEST(flatness_tells_a_flat_slash_streak_from_a_scattered_set_of_hits) {
    // A thin plank: a slash drawn along its length (edge = Y) at a fixed X saturates a near-flat
    // slab spanning Y and the plank's full (thin) Z -- the cleave case.
    DamageField slash_field;
    slash_field.reset(glm::vec3(-1.0f, -1.0f, -0.1f), glm::vec3(1.0f, 1.0f, 0.1f), 20);
    ResolvedDamage slash;
    slash.radius = 0.15f;
    slash.along_edge = 0.9f;
    const glm::vec3 edge(0.0f, 1.0f, 0.0f);
    // Dead-centre on a cell in X and Z (half a cell off the grid's own origin) so the whole
    // capsule's cross-section lands exactly on dist == 0, not straddling a cell boundary.
    slash_field.deposit(glm::vec3(0.05f, 0.0f, 0.05f), glm::vec3(1, 0, 0), edge, slash);

    glm::vec3 slash_normal(0.0f);
    float slash_distance = 0.0f, slash_flatness = 0.0f;
    require(slash_field.saturated_plane(slash_normal, slash_distance, slash_flatness),
           "damage_field: the slash saturates at least 4 cells, enough to fit a plane");
    expect(std::abs(glm::dot(slash_normal, edge)) < 0.3f,
          "damage_field: the fitted normal is roughly perpendicular to the slash's own edge");

    // The same field's worth of cells, but hit as four isolated, well-separated dots instead --
    // no flat streak, just a 3D scatter. The reject case.
    DamageField scatter_field;
    scatter_field.reset(glm::vec3(-1.0f), glm::vec3(1.0f), 10);
    ResolvedDamage dot;
    dot.radius = 0.05f; // small: each hit saturates one cell, not a blob that bleeds into the next
    const glm::vec3 points[4] = {{0.7f, 0.7f, 0.7f}, {-0.7f, 0.7f, -0.7f},
                                 {0.7f, -0.7f, -0.7f}, {-0.7f, -0.7f, 0.7f}};
    for (const glm::vec3& p : points) scatter_field.deposit(p, glm::vec3(0, 0, 1), glm::vec3(0.0f), dot);

    glm::vec3 scatter_normal(0.0f);
    float scatter_distance = 0.0f, scatter_flatness = 0.0f;
    require(scatter_field.saturated_plane(scatter_normal, scatter_distance, scatter_flatness),
           "damage_field: the scatter also saturates at least 4 cells");

    expect(slash_flatness > scatter_flatness,
          "damage_field: the flat streak reads flatter than the 3D scatter (" +
              std::to_string(slash_flatness) + " vs " + std::to_string(scatter_flatness) + ")");
}
