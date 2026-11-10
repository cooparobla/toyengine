/**
 * @file item_database_loader.h
 * @brief Parses ItemDatabase from YAML and adapts that into an asset loader.
 */

#ifndef COOPA_ITEM_ITEM_DATABASE_LOADER_H
#define COOPA_ITEM_ITEM_DATABASE_LOADER_H

#include <coopa/asset/asset_loader.h>
#include <coopa/item/item_database.h>
#include <coopa/item/item_def.h>
#include <coopa/item/item_id.h>
#include <fkYAML/node.hpp>
#include <coopa/yaml/document.h>
#include <glm/glm.hpp>

#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

namespace coopa {
namespace item {

/**
 * @brief Parses one ItemDef from its YAML mapping node. Missing keys fall back
 *        to ItemDef's own in-class defaults; `category`/`rarity` strings that
 *        don't match a known name fall back to Misc/Common (see
 *        parse_item_category()/parse_item_rarity()) rather than throwing --
 *        an author typo degrades to a plain-looking item, not a load failure.
 *        Public so tests can parse a fixture node directly.
 * @throws std::runtime_error if the node has no `id:` key.
 */
inline ItemDef parse_item_def(const fkyaml::node& node) {
    if (!node.contains("id")) {
        throw std::runtime_error("[ItemDatabase] item entry has no 'id:' key");
    }

    ItemDef def;
    def.id = ItemId::from_name(node.at("id").get_value<std::string>());
    if (node.contains("name"))        def.name        = node.at("name").get_value<std::string>();
    if (node.contains("description")) def.description = node.at("description").get_value<std::string>();
    if (node.contains("icon"))        def.icon        = node.at("icon").get_value<std::string>();
    if (node.contains("max_stack"))   def.max_stack   = node.at("max_stack").get_value<int>();
    if (node.contains("category"))    def.category    = parse_item_category(node.at("category").get_value<std::string>());
    if (node.contains("rarity"))      def.rarity      = parse_item_rarity(node.at("rarity").get_value<std::string>());

    if (node.contains("tint")) {
        const auto& t = node.at("tint");
        if (t.contains("r")) def.tint.r = t.at("r").get_value<float>();
        if (t.contains("g")) def.tint.g = t.at("g").get_value<float>();
        if (t.contains("b")) def.tint.b = t.at("b").get_value<float>();
        if (t.contains("a")) def.tint.a = t.at("a").get_value<float>();
    }

    return def;
}

/**
 * @brief Parses every entry under `items:` and defines each one on `out`,
 *        merging into whatever `out` already contains (so a C++-defined
 *        item and a YAML-defined item catalog can coexist -- neither path
 *        is privileged; the last one to define a given id wins).
 * @throws std::runtime_error if the root has no `items:` key.
 */
inline void parse_item_database(const fkyaml::node& root, ItemDatabase& out) {
    if (!root.contains("items")) {
        throw std::runtime_error("[ItemDatabase] root node has no 'items:' key");
    }
    for (const auto& item_node : root.at("items")) {
        out.define(parse_item_def(item_node));
    }
}

/**
 * @class ItemDatabaseLoader
 * @brief coopa::asset TypedAssetLoader for ItemDatabase -- a pure-CPU asset.
 *
 * decode_typed() runs entirely on the asset IO worker thread, safe because an
 * ItemDatabase stores only strings/numbers/ids -- no registry lookup, no
 * SceneObject or Component pointer to resolve on the main thread.
 * finalize_typed() is therefore a pass-through, mirroring
 * coopa::anim::AnimationClipLoader.
 */
class ItemDatabaseLoader : public coopa::asset::TypedAssetLoader<ItemDatabase> {
public:
    std::shared_ptr<ItemDatabase> decode_typed(const coopa::asset::AssetId& id,
                                               const coopa::asset::LoadContext& ctx) override {
        fkyaml::node root = coopa::yaml::load_document(ctx.resolved_path);
        auto db = std::make_shared<ItemDatabase>();
        parse_item_database(root, *db);
        return db;
    }

    std::shared_ptr<ItemDatabase> finalize_typed(std::shared_ptr<ItemDatabase> decoded,
                                                 const coopa::asset::AssetId&,
                                                 const coopa::asset::LoadContext&) override {
        return decoded;
    }

    const char* type_name() const override { return "ItemDatabase"; }
};

} // namespace item
} // namespace coopa

#endif // COOPA_ITEM_ITEM_DATABASE_LOADER_H
