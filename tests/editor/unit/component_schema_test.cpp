/**
 * @file component_schema_test.cpp
 * @brief Inspector component schemas: a default component carries its type and every in-default field,
 * and a game project can register its own schema at runtime.
 * 
 * Not here: that every schema's fields load in the engine (gpu engine_contract).
 */

#include <coopa/testing/test.h>

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

} // namespace toy::editor::testing
