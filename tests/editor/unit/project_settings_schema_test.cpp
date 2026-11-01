/**
 * @file project_settings_schema_test.cpp
 * @brief The Project Settings "Loading" group's schema defaults against LoadingConfig's own
 *        engine defaults, so the panel never shows a value the engine would not actually start
 *        with (the panel shows a field's schema default while its key is absent from
 *        config.yaml).
 */

#include <coopa/testing/test.h>

#include <optional>

#include <glm/gtc/epsilon.hpp>
#include <toyengine/core/loading_config.h>

#include "editor/schema/settings_schema.h"

COOPA_TEST_SUITE("project_settings_schema");

namespace toy::editor::testing {

using coopa::test::expect;

namespace {

std::vector<FieldDesc> loading_fields() {
    for (const auto& g : project_settings_groups()) {
        if (g.title == "Loading") return g.all_fields();
    }
    return {};
}

std::optional<FieldDesc> field(const std::vector<FieldDesc>& fields, const std::string& key) {
    for (const auto& f : fields) {
        if (f.key == key) return f;
    }
    return std::nullopt;
}

}  // namespace

COOPA_TEST(loading_group_defaults_match_the_engine) {
    const toy::core::LoadingConfig eng;  // engine defaults, not read from any file
    const std::vector<FieldDesc> fields = loading_fields();
    expect(!fields.empty(), "a Loading group exists in Project Settings");
    if (fields.empty()) return;

    if (const auto f = field(fields, "enabled")) {
        expect((f->def.x != 0.0f) == eng.enabled, "enabled default matches the engine");
    } else {
        expect(false, "Loading has 'enabled'");
    }
    if (const auto f = field(fields, "spinner")) {
        expect((f->def.x != 0.0f) == eng.spinner, "spinner default matches the engine");
    } else {
        expect(false, "Loading has 'spinner'");
    }

    auto expect_float = [&](const char* key, float want) {
        const auto f = field(fields, key);
        expect(f.has_value(), std::string("Loading has '") + key + "'");
        if (f.has_value()) {
            expect(glm::epsilonEqual(f->def.x, want, 1e-5f), std::string(key) + " default matches the engine");
        }
    };
    expect_float("min_display_time", eng.min_display_time);
    expect_float("fade_in", eng.fade_in);
    expect_float("fade_out", eng.fade_out);
    expect_float("warm_up_timeout", eng.warm_up_timeout);
    expect_float("warm_up_budget_ms", eng.warm_up_budget_ms);

    if (const auto f = field(fields, "background")) {
        expect(glm::epsilonEqual(glm::vec3(f->def), eng.background, glm::vec3(1e-5f)) == glm::bvec3(true),
               "background default matches the engine");
    } else {
        expect(false, "Loading has 'background'");
    }
    if (const auto f = field(fields, "spinner_color")) {
        expect(glm::epsilonEqual(glm::vec3(f->def), eng.spinner_color, glm::vec3(1e-5f)) == glm::bvec3(true),
               "spinner_color default matches the engine");
    } else {
        expect(false, "Loading has 'spinner_color'");
    }
    if (const auto f = field(fields, "corner")) {
        expect(!f->options.empty() && f->options.front() == toy::core::loading_corner_name(eng.corner),
               "corner default matches the engine");
    } else {
        expect(false, "Loading has 'corner'");
    }
    if (const auto f = field(fields, "overlay_ui")) {
        expect(f->default_string == eng.overlay_ui, "overlay_ui default matches the engine (blank)");
    } else {
        expect(false, "Loading has 'overlay_ui'");
    }
}

}  // namespace toy::editor::testing
