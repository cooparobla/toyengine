/**
 * @file component_schema_test.cpp
 * @brief Inspector component schemas: a default component carries its type and every in-default field,
 * a game project can register its own schema at runtime, and Vehicle's defaults match the preset
 * its component starts on.
 * 
 * Not here: that every schema's fields load in the engine (gpu engine_contract).
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <physxcoopa/vehicle/vehicle_presets.h>

#include "editor/schema/component_schema.h"
#include "editor/support/fixtures.h"

COOPA_TEST_SUITE("component_schema");

namespace toy::editor::testing {

COOPA_TEST(default_components_write_every_in_default_field) {
    for (const auto& [type, schema] : schemas()) {
        const Node c = default_component(type);
        expect(component_type(c) == type, "default " + type + " carries its type");
        for (const auto& f : schema.fields) {
            if (f.in_default && f.kind != FieldKind::AssetRef && f.kind != FieldKind::String && f.kind != FieldKind::Enum &&
                f.kind != FieldKind::ChildRef) {
                expect(c.contains(f.key), type + "." + f.key + " is written by default");
            }
        }
    }
    expect(find_schema("MeshRenderer") && find_schema("Rigidbody") && find_schema("Transform"), "core schemas exist");
}

COOPA_TEST(a_registered_project_schema_seeds_new_components) {
    expect(find_schema("TestProjectSpinner") == nullptr, "no schema before registration");
    register_component_schema({"TestProjectSpinner", "Gameplay", {f_float("speed", 90.0f, 1.0f, -3600.0f, 3600.0f, true)}});
    const ComponentSchema* s = find_schema("TestProjectSpinner");
    expect(s && s->category == "Gameplay", "a registered project schema is found");
    expect(schemas().count("TestProjectSpinner") == 1, "...and listed for Add Component");
    const Node c = default_component("TestProjectSpinner");
    expect(c.contains("speed") && std::abs(get_float(c, "speed") - 90.0) < 1e-6, "its in_default fields seed a new component");
    expect(find_schema("Transform") != nullptr, "built-in schemas are still there");
}

/**
 * @brief The Vehicle schema's defaults must equal the preset its component starts on.
 *
 * VehicleComponent::config is `pickup_4x4` until a scene says otherwise, and every inspector row
 * overrides one of that preset's values. A row whose default disagreed with the preset would write
 * its own number into the YAML the first time the inspector touched the component -- silently
 * pinning that key against the preset, so changing `preset:` afterwards would appear to do nothing
 * to it. Same reasoning as render_settings_schema_test's field-by-field parity check.
 */
COOPA_TEST(vehicle_schema_defaults_match_the_preset_it_starts_on) {
    const ComponentSchema* schema = find_schema("Vehicle");
    if (!expect(schema != nullptr, "the Vehicle schema exists")) return;

    const coopa::physx::vehicle::VehicleConfig c = coopa::physx::vehicle::pickup_4x4_preset();
    const std::vector<std::pair<std::string, float>> expected{
        {"max_steer_deg", c.max_steer_deg},
        {"min_steer_deg", c.min_steer_deg},
        {"steer_falloff_speed", c.steer_falloff_speed},
        {"steer_rate_deg", c.steer_rate_deg},
        {"steer_return_rate_deg", c.steer_return_rate_deg},
        {"brake_torque", c.brake_torque},
        {"handbrake_torque", c.handbrake_torque},
        {"anti_roll", c.anti_roll},
        {"drag", c.drag},
        {"downforce", c.downforce},
        {"air_righting", c.air_righting},
        {"air_damping", c.air_damping},
        {"nominal_wheel_load", c.nominal_wheel_load},
    };

    for (const auto& [key, value] : expected) {
        const FieldDesc* field = nullptr;
        for (const FieldDesc& f : schema->fields) {
            if (f.key == key) field = &f;
        }
        if (!expect(field != nullptr, "Vehicle." + key + " has a schema row")) continue;
        expect(std::abs(field->def.x - value) < 1e-4f,
               "Vehicle." + key + " defaults to pickup_4x4's " + std::to_string(value) +
                   " (schema says " + std::to_string(field->def.x) + ")");
        expect(value >= field->min && value <= field->max,
               "Vehicle." + key + "'s range contains the preset's value");
    }

    // The preset list must name every preset the library ships, or the inspector cannot pick one.
    const FieldDesc* preset_field = nullptr;
    for (const FieldDesc& f : schema->fields) {
        if (f.key == "preset") preset_field = &f;
    }
    if (!expect(preset_field != nullptr, "Vehicle.preset has a schema row")) return;
    expect(preset_field->options.size() == coopa::physx::vehicle::preset_names().size(),
           "the preset dropdown lists every shipped preset");
    for (const std::string& name : coopa::physx::vehicle::preset_names()) {
        bool found = false;
        for (const std::string& option : preset_field->options) {
            if (option == name) found = true;
        }
        expect(found, "the preset dropdown offers '" + name + "'");
    }
    // The dropdown's FIRST entry is what a new component gets, so it has to be the one
    // VehicleComponent::config is actually initialised to.
    expect(!preset_field->options.empty() && preset_field->options[0] == "pickup_4x4",
           "and offers pickup_4x4 first, which is the preset the component starts on");
}

} // namespace toy::editor::testing
