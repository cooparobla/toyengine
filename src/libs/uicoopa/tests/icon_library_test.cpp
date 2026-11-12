/**
 * @file icon_library_test.cpp
 * @brief IconLibrary: publishing a real sprite-sheet descriptor through coopa::asset (headless
 *        loader -- no PNG decode, no device), prefixed lookups, clear(), and the builder's
 *        icon-free fallback when no sheet was ever loaded.
 */

#include <coopa/testing/test.h>

#include "support/ui_test_support.h"
#include <fstream>
#include <sstream>
#include <coopa/asset/asset_manager.h>
#include <coopa/job/engine.h>

COOPA_TEST_SUITE("icon_library");

/** @brief Headless-safe coopa::asset loader for SpriteSheet: parses the descriptor
 *         (real filesystem read, real YAML parse) but skips the PNG decode/GPU upload
 *         entirely -- publishes a SpriteSheet with a null Texture, exactly like
 *         build_sprite_table(..., nullptr, ...) in sprite_sheet_test.cpp. Lets
 *         IconLibrary's add_sheet()/icon()/clear() be exercised end-to-end without a
 *         Device, using the real checked-in assets/icons/icons.yaml. */
class HeadlessSpriteSheetLoader : public coopa::asset::TypedAssetLoader<SpriteSheet, SpriteSheetDesc> {
public:
    std::shared_ptr<SpriteSheetDesc> decode_typed(const coopa::asset::AssetId&,
                                                  const coopa::asset::LoadContext& ctx) override {
        std::ifstream f(ctx.resolved_path);
        if (!f) throw std::runtime_error("HeadlessSpriteSheetLoader: cannot open " + ctx.resolved_path);
        std::stringstream buf;
        buf << f.rdbuf();
        return std::make_shared<SpriteSheetDesc>(parse_sprite_sheet_desc(buf.str()));
    }
    std::shared_ptr<SpriteSheet> finalize_typed(std::shared_ptr<SpriteSheetDesc> desc,
                                                const coopa::asset::AssetId&,
                                                const coopa::asset::LoadContext&) override {
        return std::make_shared<SpriteSheet>(nullptr, *desc);
    }
    const char* type_name() const override { return "SpriteSheet"; }
};

COOPA_TEST(add_sheet_publishes_prefixed_and_bare_names_until_clear) {
    // Shared engine, not the private fallback pool -- see the demos' own AssetManager
    // construction sites (demos/demo_window.cpp, demos/demo_settings_builder.cpp) for the same pattern.
    coopa::job::JobEngine jobs;
    coopa::asset::AssetManager assets(&jobs);
    assets.add_search_root(std::string(ROOT_DIR) + "/assets");
    assets.register_loader<SpriteSheet>(std::make_unique<HeadlessSpriteSheetLoader>());

    ASSERT_TRUE(!IconLibrary::instance().has_icons());

    bool ok = IconLibrary::instance().add_sheet(assets, "icons/icons.yaml");
    ASSERT_TRUE(ok);
    ASSERT_TRUE(IconLibrary::instance().has_icons());
    ASSERT_TRUE(IconLibrary::instance().icon("arrow_left") != nullptr);
    ASSERT_TRUE(IconLibrary::instance().icon("totally_missing_icon") == nullptr);

    // Prefixed publish: same entries also reachable under the prefix, original bare
    // names untouched.
    ok = IconLibrary::instance().add_sheet(assets, "icons/icons.yaml", "game/");
    ASSERT_TRUE(ok);
    ASSERT_TRUE(IconLibrary::instance().icon("game/arrow_left") != nullptr);
    ASSERT_TRUE(IconLibrary::instance().icon("arrow_left") != nullptr);

    IconLibrary::instance().clear();
    ASSERT_TRUE(!IconLibrary::instance().has_icons());
    ASSERT_TRUE(IconLibrary::instance().icon("arrow_left") == nullptr);
}

COOPA_TEST(builder_widgets_degrade_without_icons) {
    // No IconLibrary sheet loaded in this process (headless suite never touches a real
    // Device, so nothing could have loaded one) -- every icon-aware widget factory must
    // fall back to exactly its pre-icon look.
    ASSERT_TRUE(!IconLibrary::instance().has_icons());

    SceneObject root_obj("Root");
    root_obj.add_component<RectTransform>()->set_size_delta({800.0f, 600.0f});
    UIBuilder root(&root_obj);

    ComboBox* combo = root.add_dropdown("Combo", {"One", "Two"});
    SceneObject* arrow_obj = combo->owner->find_descendant("Arrow");
    ASSERT_TRUE(arrow_obj != nullptr);
    ASSERT_TRUE(arrow_obj->get_component<Text>() != nullptr);   // fallback "v" glyph
    ASSERT_TRUE(arrow_obj->get_component<Image>() == nullptr);  // not the icon path

    Toggle* toggle = root.add_toggle("Toggle");
    SceneObject* check_obj = toggle->owner->find_descendant("Checkmark");
    ASSERT_TRUE(check_obj != nullptr);
    Image* check_img = check_obj->get_component<Image>();
    ASSERT_TRUE(check_img != nullptr && check_img->sprite == nullptr); // plain tinted square

    SpinBox* spin = root.add_spinbox("Spin");
    SceneObject* dec_obj = spin->owner->find_descendant("DecBtn");
    ASSERT_TRUE(dec_obj != nullptr);
    SceneObject* dec_txt_obj = dec_obj->find_descendant("Txt");
    ASSERT_TRUE(dec_txt_obj != nullptr && dec_txt_obj->get_component<Text>() != nullptr);
}
