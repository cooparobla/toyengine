/**
 * @file config_document_test.cpp
 * @brief ConfigDocument writes config.yaml without pinning presets: an untouched save changes nothing,
 * an edit adds only its own key, and a reset removes it.
 */

#include <coopa/testing/test.h>

#include <toyengine/core/config.h>

#include "editor/app/asset_documents.h"
#include "editor/support/fixtures.h"

COOPA_TEST_SUITE("config_document");

namespace toy::editor::testing {

COOPA_TEST(untouched_saves_change_nothing_and_edits_stay_minimal) {
    const fs::path dir = coopa::test::scratch_dir("config_write");
    fs::copy_file(fs::path(ROOT_DIR) / "assets/config.yaml", dir / "config.yaml");
    ConfigDocument cfg;
    cfg.load(dir / "config.yaml");
    const Node original = cfg.node;
    cfg.save();
    expect(coopa::yaml::load_document(dir / "config.yaml") == original, "an untouched save changes no values");

    const size_t keys_before = cfg.node.at("render").size();
    const Node before = cfg.node;
    cfg.section("render")["shadow_quality"] = Node(std::string("low"));
    cfg.commit("q", before, {});
    cfg.save();
    const Node saved = coopa::yaml::load_document(dir / "config.yaml");
    expect(saved.at("render").size() == keys_before + (original.at("render").contains("shadow_quality") ? 0 : 1),
           "changing a quality tier adds no pinned preset keys");
    const toy::core::AppConfig parsed = toy::core::AppConfig::load((dir / "config.yaml").string());
    expect(parsed.render.shadow_quality == toy::render::RenderQuality::Low, "the written tier loads back");

    erase_key(cfg.section("render"), "exposure");
    cfg.save();
    expect(!coopa::yaml::load_document(dir / "config.yaml").at("render").contains("exposure"), "reset removes the key");
    expect(cfg.dirty() == false, "saving clears the dirty flag");
}

} // namespace toy::editor::testing
