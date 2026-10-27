/**
 * @file inventory.h
 * @brief Inventory screens over coopa::item: the grid, and the categorised list.
 *
 * Two shapes, because inventories come in two. A GRID (Diablo, Minecraft) is cells you drag
 * between; a categorised LIST (Fallout, Skyrim) is a category column, aligned numeric columns
 * and a detail card, and it is the one that stays readable at hundreds of items.
 *
 * Both are thin: the widgets are generic (`imm::Context::slot_grid`, `item_list`,
 * `category_list`, `item_detail`) and take only what a caller hands them, so a game with its
 * own item model uses those directly. These functions are the bridge for a game that uses
 * coopa::item -- they read `Inventory` and `ItemDatabase`, and route a completed drag to the
 * model's own `move_or_merge()` / `split()` rather than reimplementing stacking rules in UI
 * code. That is the whole reason to prefer them: the merge arithmetic already exists and is
 * already tested.
 */

#ifndef TOYENGINE_UI_INVENTORY_H
#define TOYENGINE_UI_INVENTORY_H

#include <string>
#include <vector>

#include <coopa/item/inventory.h>
#include <coopa/item/item_database.h>
#include <uicoopa/immediate/imm.h>

#include <toyengine/ui/game_theme.h>

namespace toy::ui {

/**
 * @struct InventoryAction
 * @brief What the player did to an inventory this frame. Every field is -1 for "nothing".
 *
 * Reported rather than handled: "use this item", "equip this item" and "show a context menu"
 * are game decisions, and the only one of them the UI can make on its own is moving a stack,
 * which it has already done through the model by the time this returns.
 */
struct InventoryAction {
    int clicked = -1;        ///< Slot (grid) or row (list) the player clicked.
    int right_clicked = -1;  ///< Slot or row right-clicked: use, equip, a menu.
    int moved_from = -1;     ///< A drag completed this frame, and the model has applied it.
    int moved_to = -1;
};

/** @brief The colour a rarity tier draws its name in, from `theme`'s feedback roles. */
glm::vec4 rarity_tint(const GameTheme& theme, coopa::item::ItemRarity rarity);

/**
 * @brief An inventory as a grid of cells, with drag-to-move.
 *
 * Icons come from each `ItemDef::icon` (an IconLibrary name), counts from the stack, and the
 * name colour from `ItemDef::tint` when it sets one, else the rarity. Dragging a cell onto
 * another calls `Inventory::move_or_merge()`, so stacking, partial merges and swaps behave
 * exactly as they do everywhere else in the game; Ctrl-dragging calls `split()` to peel one off.
 *
 * @param cols Cells per row. The grid takes as many rows as the inventory's capacity needs.
 */
InventoryAction inventory_grid(coopa::ui::imm::Context& ctx, std::string_view id,
                               const coopa::ui::imm::Box& b, coopa::item::Inventory& inv,
                               const coopa::item::ItemDatabase& db, int cols,
                               const GameTheme& theme, bool allow_split = true);

/**
 * @brief An inventory as a category column, a list and a detail card.
 *
 * `*category` indexes the categories `coopa::item::ItemCategory` defines, with 0 meaning All;
 * `*selected` is the highlighted row within whatever that filter leaves. Both are the caller's
 * so the screen keeps its place between frames without the inventory being mutable.
 */
InventoryAction inventory_list(coopa::ui::imm::Context& ctx, std::string_view id,
                               const coopa::ui::imm::Box& b, coopa::item::Inventory& inv,
                               const coopa::item::ItemDatabase& db, int* category,
                               int* selected, const GameTheme& theme);

/** @brief The category names `inventory_list()` offers, "All" first. */
const std::vector<std::string>& inventory_categories();

}  // namespace toy::ui

#endif  // TOYENGINE_UI_INVENTORY_H
