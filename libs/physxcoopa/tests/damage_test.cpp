/**
 * @file damage_test.cpp
 * @brief damage::resolve_damage() -- the policy that turns a typed blow plus a material into an
 *        HP cost and a geometric response (Dent/Chip/Hole/Cleave/Fracture).
 */
#include <coopa/testing/test.h>

#include <physxcoopa/damage/damage_material.h>
#include <physxcoopa/damage/damage_resolve.h>
#include <physxcoopa/damage/damage_types.h>

COOPA_TEST_SUITE("damage");

using namespace coopa::physx::damage;

namespace {

DamageMaterial wood() {
    DamageMaterial m;
    m.hardness = 0.3f;
    m.toughness = 0.55f;
    m.fibrous = 0.8f;
    m.hit_points = 120.0f;
    return m;
}

DamageMaterial glass() {
    DamageMaterial m;
    m.hardness = 0.95f;
    m.toughness = 0.05f;
    m.fibrous = 0.0f;
    m.hit_points = 20.0f;
    m.resistance[static_cast<std::size_t>(DamageType::Pierce)] = 0.7f;
    return m;
}

} // namespace

COOPA_TEST(a_strong_slash_on_wood_cleaves_it) {
    DamageEvent blow;
    blow.type = DamageType::Slash;
    blow.amount = 200.0f; // well past wood's hit_points * 0.5 cleave cost
    ResolvedDamage r = resolve_damage(blow, wood());
    ASSERT_TRUE(r.response == DamageResponse::Cleave);
    ASSERT_TRUE(r.hp_loss > 0.0f);
}

COOPA_TEST(the_same_amount_of_impact_on_wood_dents_it_instead) {
    DamageEvent blow;
    blow.type = DamageType::Impact;
    blow.amount = 200.0f; // wood is not brittle enough (toughness 0.55) to fracture here
    ResolvedDamage r = resolve_damage(blow, wood());
    ASSERT_TRUE(r.response == DamageResponse::Dent);
}

COOPA_TEST(a_weak_slash_on_wood_only_chips_it) {
    DamageEvent blow;
    blow.type = DamageType::Slash;
    blow.amount = 30.0f; // above the None floor, below the cleave cost
    ResolvedDamage r = resolve_damage(blow, wood());
    ASSERT_TRUE(r.response == DamageResponse::Chip);
}

COOPA_TEST(a_pierce_on_glass_spalls_rather_than_a_clean_hole) {
    DamageEvent blow;
    blow.type = DamageType::Pierce;
    blow.amount = 10.0f;
    ResolvedDamage r = resolve_damage(blow, glass());
    ASSERT_TRUE(r.response == DamageResponse::Chip); // glass's near-zero toughness spalls
}

COOPA_TEST(a_tough_metal_takes_a_pierce_as_a_clean_hole) {
    DamageMaterial metal;
    metal.hardness = 0.85f;
    metal.toughness = 0.8f;
    metal.hit_points = 250.0f;
    DamageEvent blow;
    blow.type = DamageType::Pierce;
    blow.amount = 10.0f;
    ResolvedDamage r = resolve_damage(blow, metal);
    ASSERT_TRUE(r.response == DamageResponse::Hole);
}

COOPA_TEST(a_strong_enough_impact_fractures_a_brittle_material) {
    DamageEvent blow;
    blow.type = DamageType::Impact;
    blow.amount = 500.0f; // well past glass's hit_points * 0.3 fracture threshold
    ResolvedDamage r = resolve_damage(blow, glass());
    ASSERT_TRUE(r.response == DamageResponse::Fracture);
}

COOPA_TEST(an_explosive_blow_always_fractures) {
    DamageEvent blow;
    blow.type = DamageType::Explosive;
    blow.amount = 50.0f;
    ResolvedDamage r = resolve_damage(blow, wood());
    ASSERT_TRUE(r.response == DamageResponse::Fracture);
}

COOPA_TEST(fire_never_marks_the_surface_but_still_costs_hp) {
    DamageEvent blow;
    blow.type = DamageType::Fire;
    blow.amount = 50.0f;
    ResolvedDamage r = resolve_damage(blow, wood());
    ASSERT_TRUE(r.response == DamageResponse::None);
    ASSERT_TRUE(r.hp_loss > 0.0f);
}

COOPA_TEST(resistance_scales_hp_loss_monotonically) {
    DamageEvent blow;
    blow.type = DamageType::Impact;
    blow.amount = 100.0f;

    DamageMaterial low_resist, high_resist;
    low_resist.resistance[static_cast<std::size_t>(DamageType::Impact)] = 0.5f;
    high_resist.resistance[static_cast<std::size_t>(DamageType::Impact)] = 2.0f;

    float hp_low = resolve_damage(blow, low_resist).hp_loss;
    float hp_mid = resolve_damage(blow, DamageMaterial{}).hp_loss; // resistance 1.0
    float hp_high = resolve_damage(blow, high_resist).hp_loss;

    ASSERT_TRUE(hp_low > hp_mid);
    ASSERT_TRUE(hp_mid > hp_high);
}

COOPA_TEST(a_settling_contact_impulse_resolves_to_none) {
    DamageEvent blow;
    blow.type = DamageType::Impact;
    blow.amount = 0.3f; // a resting crate's steady contact impulse, not a blow
    ResolvedDamage r = resolve_damage(blow, DamageMaterial{});
    ASSERT_TRUE(r.response == DamageResponse::None);
}
