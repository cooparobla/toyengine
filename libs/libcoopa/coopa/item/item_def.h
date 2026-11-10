/**
 * @file item_def.h
 * @brief Immutable definition of an item kind -- name, icon, stacking, category, rarity, tint.
 */

#ifndef COOPA_ITEM_ITEM_DEF_H
#define COOPA_ITEM_ITEM_DEF_H

#include <coopa/item/item_id.h>
#include <glm/glm.hpp>
#include <string>
#include <string_view>

namespace coopa {
namespace item {

/** @brief Default per-slot stack limit for an ItemDef that doesn't specify its own. */
inline constexpr int k_default_max_stack = 99;

/** @enum ItemCategory
 *  @brief Broad authoring/gameplay grouping for an ItemDef. */
enum class ItemCategory {
    Misc,
    Consumable,
    Weapon,
    Armor,
    Material,
    Quest,
};

/** @enum ItemRarity
 *  @brief Authoring/gameplay rarity tier for an ItemDef, typically driving a UI tint. */
enum class ItemRarity {
    Common,
    Uncommon,
    Rare,
    Epic,
    Legendary,
};

/**
 * @brief Parses a lowercase category name ("consumable", "weapon", ...) into an
 *        ItemCategory, falling back to ItemCategory::Misc for anything unrecognized --
 *        callers (parse_item_def(), item_database_loader.h) never need to validate
 *        this input themselves.
 */
inline ItemCategory parse_item_category(std::string_view name) {
    if (name == "consumable") return ItemCategory::Consumable;
    if (name == "weapon")     return ItemCategory::Weapon;
    if (name == "armor")      return ItemCategory::Armor;
    if (name == "material")   return ItemCategory::Material;
    if (name == "quest")      return ItemCategory::Quest;
    return ItemCategory::Misc;
}

/**
 * @brief Parses a lowercase rarity name ("uncommon", "rare", ...) into an
 *        ItemRarity, falling back to ItemRarity::Common for anything unrecognized.
 */
inline ItemRarity parse_item_rarity(std::string_view name) {
    if (name == "uncommon")  return ItemRarity::Uncommon;
    if (name == "rare")      return ItemRarity::Rare;
    if (name == "epic")      return ItemRarity::Epic;
    if (name == "legendary") return ItemRarity::Legendary;
    return ItemRarity::Common;
}

/**
 * @struct ItemDef
 * @brief The authoritative, immutable description of one kind of item.
 *
 * ItemDatabase owns a collection of these, keyed by `id`. Everything that
 * varies per *instance* of an item (how many are in a slot) lives in
 * ItemStack instead -- this struct is shared by every stack of the same id.
 */
struct ItemDef {
    ItemId      id;
    std::string name;
    std::string description;
    std::string icon;                    /**< IconLibrary name; empty means no icon. */
    int         max_stack = k_default_max_stack;
    ItemCategory category = ItemCategory::Misc;
    ItemRarity   rarity   = ItemRarity::Common;

    /** @brief UI tint override. Alpha < 0 (the default) means "unset" -- consumers
     *         fall back to their own rarity/id-based coloring in that case, mirroring
     *         uicoopa's InventoryItem::color convention. */
    glm::vec4   tint{1.0f, 1.0f, 1.0f, -1.0f};
};

} // namespace item
} // namespace coopa

#endif // COOPA_ITEM_ITEM_DEF_H
