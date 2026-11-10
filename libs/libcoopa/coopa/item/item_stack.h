/**
 * @file item_stack.h
 * @brief A quantity of one item kind occupying a single inventory slot.
 */

#ifndef COOPA_ITEM_ITEM_STACK_H
#define COOPA_ITEM_ITEM_STACK_H

#include <coopa/item/item_id.h>

namespace coopa {
namespace item {

/**
 * @struct ItemStack
 * @brief One inventory slot's contents: which item, and how many.
 *
 * Deliberately holds nothing else -- name/icon/max_stack/etc. all live on the
 * shared ItemDef (item_def.h), looked up by `item` through an ItemDatabase.
 * A default-constructed ItemStack is empty().
 */
struct ItemStack {
    ItemId item;
    int    count = 0;

    /** @brief True if this slot has no item or a non-positive count. */
    bool empty() const { return !item.is_valid() || count <= 0; }
};

} // namespace item
} // namespace coopa

#endif // COOPA_ITEM_ITEM_STACK_H
