/**
 * @file caml_codec_test.cpp
 * @brief The .caml codec's contracts with coopa::yaml: detection by magic bytes (not extension),
 *        a missing codec naming its fix, and x.yaml <-> x.caml variant resolution through paths and
 *        AssetSource. Every-asset round-tripping is the assets suite's.
 */

#include <coopa/testing/test.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <coopa/asset/asset_source.h>
#include <coopa/yaml/document.h>
#include <toyengine/core/caml_codec.h>

#include "engine/support/checks.h"
#include "engine/support/scratch_files.h"

COOPA_TEST_SUITE("caml_codec");

using namespace toy::test;

/** @brief Detection is by magic bytes: an encoded file named .yaml still decodes, plain text passes through. */
COOPA_TEST(detected_by_magic_not_extension) {
    toy::core::install_caml_codec();   // what the game's main() does before loading anything
    const std::filesystem::path dir = coopa::test::scratch_dir("caml_magic");
    const std::string yaml = "a: 1\nlist: [1, 2, 3]\nname: hello\n";
    const std::vector<uint8_t> bytes = toy::core::encode_caml_text(yaml);
    {
        std::ofstream out(dir / "misnamed.yaml", std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    write_text_file(dir / "plain.yaml", yaml);

    expect(coopa::yaml::read_text(dir / "misnamed.yaml") == yaml, "an encoded file named .yaml decodes by its magic");
    expect(coopa::yaml::read_text(dir / "plain.yaml") == yaml, "plain YAML passes through unchanged");
    expect(coopa::yaml::load_document(dir / "misnamed.yaml").at("name").get_value<std::string>() == "hello",
           "load_document parses the decoded text");
}

/** @brief Without the codec, a .caml fails with a message naming the fix rather than a parse error. */
COOPA_TEST(missing_codec_error_names_the_fix) {
    toy::core::install_caml_codec();   // what the game's main() does before loading anything
    const std::filesystem::path dir = coopa::test::scratch_dir("caml_no_codec");
    toy::core::encode_caml_file(std::string(ROOT_DIR) + "/assets/physics_materials/rubber.yaml", dir / "rubber.caml");

    coopa::yaml::clear_decoders();
    std::string message;
    try {
        coopa::yaml::load_document(dir / "rubber.caml");
    } catch (const std::exception& e) {
        message = e.what();
    }
    toy::core::install_caml_codec();

    expect(message.find("install_caml_codec") != std::string::npos,
           "loading .caml with no decoder names install_caml_codec() (got: '" + message + "')");
    expect(coopa::yaml::load_document(dir / "rubber.caml").contains("restitution"), "reinstalling the codec restores loading");
}

/** @brief "x.yaml" references find "x.caml" (and back), including multi-dot sidecar names and AssetSource lookups. */
COOPA_TEST(yaml_references_resolve_to_caml_twins) {
    toy::core::install_caml_codec();   // what the game's main() does before loading anything
    const std::filesystem::path dir = coopa::test::scratch_dir("caml_variants");
    std::filesystem::create_directories(dir / "meshes");
    write_text_file(dir / "plain.yaml", "k: 1\n");
    toy::core::encode_caml_file(dir / "plain.yaml", dir / "meshes" / "sphere.lod.caml");
    toy::core::encode_caml_file(dir / "plain.yaml", dir / "packed.caml");

    using coopa::yaml::resolve_variant;
    expect(resolve_variant(dir / "plain.yaml") == dir / "plain.yaml", "an existing path resolves to itself");
    expect(resolve_variant(dir / "packed.yaml") == dir / "packed.caml", "x.yaml finds x.caml");
    expect(resolve_variant(dir / "plain.caml") == dir / "plain.yaml", "x.caml finds x.yaml");
    expect(resolve_variant(dir / "meshes" / "sphere.lod.yaml") == dir / "meshes" / "sphere.lod.caml",
           "multi-dot sidecar names keep their stem");
    expect(resolve_variant(dir / "missing.yaml") == dir / "missing.yaml", "no twin: the path comes back unchanged");
    expect(resolve_variant(dir / "image.png") == dir / "image.png", "non-document paths are never rewritten");

    coopa::asset::AssetSource source;
    source.add_search_root(dir.string());
    expect(std::filesystem::path(source.resolve("meshes/sphere.lod.yaml")) == dir / "meshes" / "sphere.lod.caml",
           "AssetSource search roots resolve a .yaml reference to its .caml twin");
    expect(std::filesystem::path(source.resolve("packed.yaml", (dir / "meshes").string())) == dir / "packed.caml",
           "AssetSource falls through base_dir to the search root, variant-aware");
}
