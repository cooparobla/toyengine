/**
 * @file asset_browser_test.cpp
 * @brief The Asset panel: each asset type opens its own view (switching asks to save first), the tag
 * filter and Set Tags, right-click Delete / Add (a scene deletes as its folder), texture previews
 * declare the colour space the project uses, and toyengine's own assets are listed, usable and
 * read-only.
 */

#include <coopa/testing/test.h>

#include <coopa/asset/asset_index.h>
#include <gfxcoopa/util/image_readback.h>

#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("asset_browser");

namespace toy::editor::testing {

/** @brief The asset-focused flow: each asset type opens its view; switching asks to save. */
COOPA_TEST(each_asset_type_opens_its_view_and_switching_asks_to_save) {
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4, .prepare = [](Project& project) {
        coopa::gfx::util::ImageData img;   // a small texture asset
        img.width = img.height = 4;
        img.channels = 4;
        img.pixels.assign(4 * 4 * 4, 200);
        coopa::gfx::util::save_image_png(img, (project.assets() / "textures" / "checker.png").string());
    }});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& project = session.project;
    expect(app.active_asset_type() == AssetType::Scene, "the project opens its default scene");
    expect(fs::is_directory(project.assets() / "objects"), "new projects have an objects/ folder");

    app.open_asset(AssetType::Material, "materials/default.yaml");
    tick(engine, 4);
    expect(app.active_asset_type() == AssetType::Material && app.material_document().open(), "a material opens in the material view");
    auto* po = engine.scene().find_object("PreviewObject");
    auto* ground = engine.scene().find_object("PreviewGround");
    auto* key = engine.scene().find_object("LookdevKey");
    auto* pmr = po ? po->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
    expect(pmr && pmr->is_ready() && ground && ground->active() && key, "the lookdev scene shows the material on a shape, on a ground disc, lit");
    expect(app.shading() == Shading::Full, "materials open in the game's renderer");
    dump(engine, "16_lookdev");

    app.open_asset(AssetType::Mesh, "meshes/cube.yaml");
    tick(engine, 4);
    expect(app.active_asset_type() == AssetType::Mesh && app.interaction_mode() == InteractionMode::Object, "a mesh opens in the mesh viewer");
    app.set_interaction_mode(InteractionMode::Edit);
    expect(app.edit_mode_active(), "the mode dropdown enters Edit Mode on the mesh asset");
    app.set_interaction_mode(InteractionMode::Sculpt);
    tick(engine, 2);
    expect(app.interaction_mode() == InteractionMode::Sculpt, "and Sculpt Mode works on a standalone mesh asset");
    app.set_interaction_mode(InteractionMode::Object);

    app.open_asset(AssetType::Texture, "textures/checker.png");
    tick(engine, 4);
    po = engine.scene().find_object("PreviewObject");
    pmr = po ? po->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
    expect(app.active_asset_type() == AssetType::Texture && pmr && !pmr->material.texture_albedo.empty(), "a texture shows on a plane");

    app.open_asset(AssetType::Scene, "scenes/main/scene.yaml");
    tick(engine, 4);
    expect(app.active_asset_type() == AssetType::Scene && engine.scene().find_object("cube"), "back to the scene");
    // Unsaved changes: switching asks first and does nothing until answered.
    const ObjectId cube = object_named(app, "cube");
    app.document().set_object_key(cube, "name", Node(std::string("Renamed")), "Rename");
    app.open_asset(AssetType::Material, "materials/default.yaml");
    tick(engine, 3);
    expect(app.active_asset_type() == AssetType::Scene && app.ui().any_popup_open(), "switching with unsaved changes opens the save prompt");
    tick(engine, 2);
    dump(engine, "24_unsaved_prompt");
}

/**
 * @brief Tags in the Asset panel: the tag filter keeps tagged assets, a new asset's name is unique
 *        per type whatever the tags, Set Tags moves the file into its tag folder (short
 *        references still find it), and Sort by Tags lists untagged assets first.
 */
COOPA_TEST(tag_filter_unique_names_and_set_tags) {
    EditorSession session({.fixed_dt = "0", .settle_frames = 2});
    auto& engine = session.engine;
    auto& app = session.app;
    Project& made = session.project;
    app.set_show_engine_assets(false);
    tick(engine, 2);
    coopa::yaml::save_document(made.assets() / "materials" / "metal" / "material.yaml", Node::mapping());
    app.project().refresh();
    app.set_asset_tab(AssetType::Material);
    app.set_asset_tag_filter({"metal"});
    expect(app.assets_listed() == std::vector<std::string>{"materials/metal/material.yaml"}, "the tag filter keeps tagged assets");
    app.set_asset_tag_filter({});
    expect(app.assets_listed().size() == 2, "no tag picked: everything");
    expect(app.create_material("material"), "creating a material");
    tick(engine, 2);
    expect(coopa::yaml::document_exists(made.assets() / "materials" / "material_01.yaml"),
           "a new material avoids a name taken in another tag folder");
    app.set_asset_tags(AssetType::Material, "materials/default.yaml", {"basic"});
    tick(engine, 3);
    expect(fs::exists(made.assets() / "materials/basic/default.yaml") && !fs::exists(made.assets() / "materials/default.yaml"),
           "set_asset_tags moves the asset into its tag folder");
    const Node scene = coopa::yaml::load_document(made.assets() / "scenes/main/scene.yaml");
    bool short_kept = false;
    for (const auto& o : scene.at("scene").at("root_objects").as_seq()) {
        if (get_string(o, "name") != "ground") continue;
        for (const auto& c : o.at("components").as_seq()) short_kept |= get_string(c, "material") == "materials/default";
    }
    expect(short_kept, "the starter scene's materials/default still names it (found by name)");
    app.set_asset_sort(EditorApp::AssetSort::Tags);
    const auto by_tags = app.assets_listed();
    expect(!by_tags.empty() && by_tags.front().rfind("materials/material", 0) == 0 && by_tags.back() == "materials/metal/material.yaml",
           "sort by tags: untagged first, then by tag");
    app.set_asset_sort(EditorApp::AssetSort::Name);
}

/**
 * @brief The Asset panel's right-click menus, through real input: right-clicking an asset offers
 *        Delete, and the confirmation deletes the file (a scene: its whole folder, scenes/<name>/,
 *        not just its scene.yaml); right-clicking empty space in the list offers Add, with what
 *        the + button offers for that tab.
 */
COOPA_TEST(context_menus_delete_and_add_assets) {
    using coopa::input::MouseButton;
    EditorSession session({.fixed_dt = "0", .prepare = [](Project& project) {
        for (const char* n : {"stone", "brick"}) {
            Node m = Node::mapping();
            m["albedo"] = make_color({0.5f, 0.5f, 0.5f});
            coopa::yaml::save_document(project.assets() / "materials" / (std::string(n) + ".yaml"), m);
        }
        coopa::yaml::save_document(project.assets() / "scenes/extra/scene.yaml", Project::default_scene_node("extra"));
    }});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;
    auto& project = session.project;
    app.set_show_engine_assets(false);   // the project's list only: "empty space" below it must stay empty
    app.project().refresh();
    app.set_asset_tab(AssetType::Material);
    tick(engine, 3);

    // Right-click an asset: its menu, with Delete...; confirming deletes the asset (and only it).
    auto delete_row = [&](const std::string& rel) {
        auto row = app.test_rect("asset_row:" + rel);
        expect(row.has_value(), rel + " is listed");
        if (!row) return false;
        in.click(row->center(), MouseButton::Right);
        expect(app.ui().any_popup_open(), "right-clicking " + rel + " opens its context menu");
        auto del = app.test_rect("asset_delete");
        expect(del.has_value(), "the menu has Delete...");
        if (!del) return false;
        in.click(del->center());
        tick(engine, 2);
        auto confirm = app.test_rect("asset_delete_confirm");
        expect(confirm.has_value(), "Delete... asks for confirmation");
        if (!confirm) return false;
        in.click(confirm->center());
        tick(engine, 2);
        return true;
    };
    if (delete_row("materials/stone.yaml")) {
        expect(!coopa::yaml::document_exists(project.assets() / "materials" / "stone.yaml") &&
               coopa::yaml::document_exists(project.assets() / "materials" / "brick.yaml"),
               "confirming deletes that asset's file (and only it)");
    }
    app.set_asset_tab(AssetType::Scene);
    tick(engine, 3);
    if (delete_row("scenes/extra/scene.yaml")) {
        expect(!fs::exists(project.assets() / "scenes" / "extra"), "deleting a scene deletes its folder");
        expect(fs::exists(project.assets() / "scenes" / "main" / "scene.yaml"), "the other scene is untouched");
    }
    app.set_asset_tab(AssetType::Material);
    tick(engine, 3);

    // Right-click empty space: Add, the same as + (Materials: creates one directly).
    auto brick = app.test_rect("asset_row:materials/brick.yaml");
    expect(brick.has_value(), "the list shows the remaining material");
    if (!brick) return;
    const glm::vec2 empty = brick->center() + glm::vec2(0.0f, 120.0f);
    in.click(empty, MouseButton::Right);
    expect(app.ui().any_popup_open(), "right-clicking empty space in the list opens the Add menu");
    auto add = app.test_rect("asset_add");
    expect(add.has_value(), "the menu has Add");
    if (!add) return;
    const size_t before = app.project().list("materials", ".yaml").size();
    in.click(add->center());
    tick(engine, 3);
    app.project().refresh();
    expect(app.project().list("materials", ".yaml").size() == before + 1, "Add creates a new material, as + does");

    // A tab whose + offers choices (meshes) shows them under Add.
    app.set_asset_tab(AssetType::Mesh);
    tick(engine, 3);
    in.click(empty, MouseButton::Right);
    add = app.test_rect("asset_add");
    expect(app.ui().any_popup_open() && add.has_value(), "the Meshes tab's empty-space menu has Add");
    if (!add) return;
    in.move(add->center(), 3);   // hovering opens the submenu
    auto cube = app.test_rect("asset_new:Cube");
    expect(cube.has_value(), "Add lists the mesh primitives the + button offers");
    if (cube) {
        in.click(cube->center());
        tick(engine, 3);
        expect(app.active_asset_type() == AssetType::Mesh && coopa::yaml::document_exists(project.assets() / "meshes" / "cube.yaml"),
               "picking Cube creates (and opens) a cube mesh asset");
    }
}

/**
 * @brief Previewing a texture in the Textures tab declares the color space the project uses it
 *        in, not always sRGB (the preview shows it as albedo): the loader keeps the first
 *        declaration, so an sRGB one from viewing a normal map would mis-decode it for every
 *        material using it ("declare_color_space ... conflicts" on the console).
 */
COOPA_TEST(texture_previews_declare_the_colour_space_in_use) {
    EditorSession session({.fixed_dt = "0", .prepare = [](Project& project) {
        for (const char* t : {"brick_normal.png", "brick_albedo.png", "noise_mask.png"}) {
            // toyengine keeps them in tag folders (textures/brick/, textures/masks/); found by name.
            const auto from = coopa::asset::AssetIndex::find(fs::path(ROOT_DIR) / "assets", std::string("textures/") + t);
            fs::copy_file(from.value_or(fs::path(ROOT_DIR) / "assets" / "textures" / t), project.assets() / "textures" / t);
        }
        Node mat = Node::mapping();
        mat["albedo"] = make_color({1, 1, 1});
        mat["texture_albedo"] = Node(std::string("textures/brick_albedo.png"));
        mat["texture_normal"] = Node(std::string("textures/brick_normal.png"));
        coopa::yaml::save_document(project.assets() / "materials" / "brick.yaml", mat);
    }});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& project = session.project;
    auto* loader = dynamic_cast<coopa::gfx::engine::loaders::TextureLoader*>(
        engine.assets().loader<coopa::gfx::engine::data::Texture>());
    expect(loader != nullptr, "the engine has a texture loader");
    if (!loader) return;
    auto space_of = [&](const char* rel) {
        return loader->declared_color_space(engine.assets().source().resolve(rel, project.assets().string()));
    };
    using coopa::gfx::ColorSpace;
    app.open_asset(AssetType::Texture, "textures/brick_normal.png");
    tick(engine, 4);
    expect(space_of("textures/brick_normal.png") == ColorSpace::Linear,
           "previewing a normal map (used as texture_normal) declares it linear, not sRGB");
    app.open_asset(AssetType::Texture, "textures/noise_mask.png");
    tick(engine, 4);
    expect(space_of("textures/noise_mask.png") == ColorSpace::Linear, "an unreferenced *_mask texture is treated as data (linear)");
    app.open_asset(AssetType::Texture, "textures/brick_albedo.png");
    tick(engine, 4);
    expect(space_of("textures/brick_albedo.png") == ColorSpace::Srgb, "an albedo map previews as sRGB");
}

/**
 * @brief toyengine's assets/ is a read-only layer under a game project's: listed (toggle, on by
 *        default), usable by drag and drop, never edited -- Copy to Project makes an editable copy.
 */
COOPA_TEST(engine_assets_are_listed_usable_and_read_only) {
    using coopa::input::MouseButton;
    EditorSession session({.fixed_dt = "0"});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;
    auto& project = session.project;
    const fs::path root = project.root();
    expect(!project.is_engine() && Project(fs::path(ROOT_DIR)).is_engine(), "a game project is not toyengine; the checkout is");
    expect(project.is_engine_asset("materials/brick.yaml") && !project.is_engine_asset("materials/default.yaml"),
           "a file only toyengine has resolves to toyengine's; one the project has stays the project's");
    expect(project.absolute("materials/brick.yaml") == Project::engine_assets() / "materials/building/brick.yaml" &&
           project.relative(project.absolute("materials/brick.yaml")) == "materials/building/brick.yaml",
           "absolute() finds toyengine's asset by name (in its tag folder); relative() round-trips");
    expect(Project(fs::path(ROOT_DIR)).list_engine("materials", ".yaml").empty(), "inside toyengine nothing is a separate layer");

    expect(app.show_engine_assets(), "toyengine's assets are shown by default");
    auto has = [](const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); };
    const auto meshes = app.engine_assets_listed(AssetType::Mesh);
    expect(has(meshes, "meshes/primitives/barrel.yaml") && !has(meshes, "meshes/primitives/cube.yaml"),
           "the Asset panel lists toyengine's meshes, minus those the project has its own copy of");

    // Not editable: opening is refused, so nothing can be saved back into toyengine.
    app.open_asset(AssetType::Material, "materials/brick.yaml");
    tick(engine, 3);
    expect(app.active_asset_type() == AssetType::Scene, "a toyengine material doesn't open for editing");

    // Usable: drag a toyengine mesh from the panel into the viewport.
    app.set_asset_tab(AssetType::Mesh);
    tick(engine, 3);
    const auto row = app.test_rect("asset_row:engine:meshes/primitives/barrel.yaml");
    expect(row.has_value(), "the barrel row is drawn in the toyengine section");
    if (row) {
        const imm::Box vb = app.viewport_box();
        in.move({row->x + row->w * 0.5f, row->y + row->h * 0.5f});
        in.drag({vb.x + vb.w * 0.5f, vb.y + vb.h * 0.5f}, MouseButton::Left, 8);
        tick(engine, 4);
    }
    const ObjectId barrel = object_named(app, "barrel");
    expect(barrel != 0, "dropping it adds a barrel to the scene");
    expect(engine.scene().find_object("barrel") != nullptr, "...which loads (toyengine's mesh resolves under the project)");
    dump(engine, "engine_assets_drop");

    // ...but its mesh can't be edited in place.
    if (barrel) {
        app.document().clear_selection();
        app.document().select(barrel);
        tick(engine, 1);
        expect(!app.set_interaction_mode(InteractionMode::Edit) && app.interaction_mode() == InteractionMode::Object,
               "Edit Mode on toyengine's mesh is refused");
    }

    // Copy to Project: the project's own, editable copy now resolves instead.
    expect(app.copy_engine_asset_to_project(AssetType::Material, "materials/brick.yaml"), "Copy to Project");
    tick(engine, 4);
    expect(fs::exists(root / "assets" / "materials" / "building" / "brick.yaml") && !app.project().is_engine_asset("materials/brick.yaml"),
           "the copy is the project's");
    // It opens for editing -- after the usual save prompt, since the barrel left the scene unsaved.
    expect(app.active_asset_type() == AssetType::Material || app.ui().is_popup_open("Unsaved Changes"),
           "...and opens for editing (after the unsaved-changes prompt)");
    expect(!has(app.engine_assets_listed(AssetType::Material), "materials/building/brick.yaml"), "...and leaves the toyengine section");

    // The toggle hides the layer, and is remembered.
    app.set_show_engine_assets(false);
    expect(app.engine_assets_listed(AssetType::Mesh).empty(), "the toggle hides toyengine's assets");
    expect(Project::load_prefs().contains("show_engine_assets") && !Project::load_prefs().at("show_engine_assets").get_value<bool>(),
           "...and is remembered");
}

} // namespace toy::editor::testing
