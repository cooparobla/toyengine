/**
 * @file engine_contract_test.cpp
 * @brief What the editor offers, the engine must load: every component schema with every field at its
 * default, every shader the material editor's dropdown lists (in the pass the editor assumes),
 * and the three spellings of a material (reference, base + override, inline).
 */

#include <coopa/testing/test.h>

#include <coopa/scene/scene_loader.h>

#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("engine_contract");

namespace toy::editor::testing {

/**
 * @brief Every component the inspector can write loads in the engine: for each schema, a
 *        component with EVERY field at its schema default (what editing each field writes) goes
 *        through the engine's own scene loader without error and creates the component.
 */
COOPA_TEST(every_component_schema_loads_in_the_engine) {
    setenv("NO_INPUT", "1", 1);
    const Project project = new_project();
    const fs::path root = project.root();
    toy::core::Engine engine(shell_config(project), shell_options(project));   // registers every component parser
    int checked = 0;
    for (const auto& [type, schema] : schemas()) {
        Node comp = default_component(type);
        for (const auto& f : schema.fields) {
            if (comp.contains(f.key)) continue;
            switch (f.kind) {
                case FieldKind::Enum:  if (!f.options.empty() && !f.options.front().empty()) comp[f.key] = Node(f.options.front()); break;
                case FieldKind::String:
                case FieldKind::AssetRef:
                case FieldKind::ChildRef: if (!f.default_string.empty()) comp[f.key] = Node(f.default_string); break;
                case FieldKind::Material: break;   // default_component() writes it when the schema has one
                default: comp[f.key] = field_default_node(f); break;
            }
        }
        Node obj = Node::mapping();
        obj["name"] = Node(std::string("Probe"));
        Node comps = Node::sequence();
        Node t = Node::mapping();
        t["type"] = Node(std::string("Transform"));
        comps.as_seq().push_back(t);
        if (type != "Transform") comps.as_seq().push_back(comp);
        obj["components"] = comps;
        Node doc = Node::mapping();
        doc["format"] = Node(std::string("toyengine"));
        Node sc = Node::mapping();
        sc["scene_name"] = Node(std::string("SchemaProbe"));
        Node roots = Node::sequence();
        roots.as_seq().push_back(obj);
        sc["root_objects"] = roots;
        doc["scene"] = sc;
        std::string error;
        bool has = false;
        try {
            coopa::scene::Scene scene = coopa::scene::SceneLoader::load_from_node(doc, (root / "assets" / "scenes" / "probe.yaml").string());
            auto* o = scene.find_object("Probe");
            has = o && (type == "Transform" || o->get_component_by_type_name(type) != nullptr ||
                        o->get_component_by_type_name(type + "Component") != nullptr);
            if (!has && o) {
                for (const auto& c : o->components()) error += " " + c->type_name();
            }
        } catch (const std::exception& e) {
            error = e.what();
        }
        // A Theme with no file loads nothing (by design); one with a file is covered by the UI tests.
        if (type == "Theme") { ++checked; continue; }
        expect(error.empty() || has, "schema " + type + ": every field at its default loads in the engine (" + error + ")");
        expect(has, "schema " + type + ": ...and creates the component (has:" + error + ")");
        ++checked;
    }
    expect(checked >= 20, "checked every component schema (" + std::to_string(checked) + ")");
}

/** @brief The material editor's Shader dropdown only offers shaders the engine registers, in
 *         the pass (domain) the catalogue claims -- a mismatch would silently render stock PBR. */
COOPA_TEST(material_shader_catalogue_matches_the_engine_registry) {
    setenv("NO_INPUT", "1", 1);
    const Project project = new_project();
    toy::core::Engine engine(shell_config(project), shell_options(project));
    const auto& registry = engine.render_config().surface_shaders;
    for (const auto& s : surface_shaders()) {
        if (s.name.empty()) continue;
        const auto* desc = registry.find(s.name);
        expect(desc != nullptr, "the engine registers the '" + s.name + "' shader the dropdown offers");
        if (!desc) continue;
        const bool transparent = desc->domain == coopa::gfx::pipeline::SurfaceShaderDomain::Transparent;
        expect(transparent == s.transparent, "'" + s.name + "' is in the pass the editor assumes");
        expect(s.params.size() <= 4, "'" + s.name + "' labels at most the four shader_params slots");
    }
}

/** @brief A material by reference, as base + override, and inline: the engine parses the
 *         reference and the same values inline identically, and an override changes only its key. */
COOPA_TEST(material_reference_override_and_inline_forms_agree) {
    setenv("NO_INPUT", "1", 1);
    Project project = new_project();
    Node mat = Node::mapping();
    mat["albedo"] = make_color({0.1f, 0.2f, 0.3f});
    mat["roughness"] = make_float(0.9);
    coopa::yaml::save_document(project.assets() / "materials/stone.yaml", mat);
    SceneDocument doc;
    doc.reset("Forms");
    auto add = [&](const std::string& name, Node material) {
        Node o = doc.make_object(name);
        Node mr = Node::mapping();
        mr["type"] = Node(std::string("MeshRenderer"));
        mr["mesh_path"] = Node(std::string("cube"));
        mr["material"] = material;
        o["components"].as_seq().push_back(mr);
        doc.add_object(o);
    };
    add("Ref", Node(std::string("materials/stone")));
    Node over = Node::mapping();
    over["base"] = Node(std::string("materials/stone"));
    over["roughness"] = make_float(0.2);
    add("Override", over);
    Node inl = Node::mapping();
    inl["albedo"] = make_color({0.1f, 0.2f, 0.3f});
    inl["roughness"] = make_float(0.9);
    add("Inline", inl);
    const fs::path scene = project.assets() / "scenes/forms/scene.yaml";
    doc.save(scene);

    toy::core::AppConfig cfg = shell_config(project);
    cfg.scene.default_scene = scene.string();
    toy::core::EngineOptions o;
    o.project_root = project.root();
    toy::core::Engine engine(cfg, o);
    auto mat_of = [&](const char* n) {
        return engine.scene().find_object(n)->get_component<coopa::gfx::engine::components::MeshRenderer>()->material;
    };
    const auto r = mat_of("Ref"), ov = mat_of("Override"), in = mat_of("Inline");
    expect(r.albedo == in.albedo && r.roughness == in.roughness, "a reference and the same values inline parse identically");
    expect(ov.albedo == in.albedo && std::abs(ov.roughness - 0.2f) < 1e-5f, "base + override keeps the base, overrides the key");
}

} // namespace toy::editor::testing
