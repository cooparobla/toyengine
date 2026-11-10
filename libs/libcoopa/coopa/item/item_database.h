/**
 * @file item_database.h
 * @brief Owning collection of ItemDefs, keyed by ItemId.
 */

#ifndef COOPA_ITEM_ITEM_DATABASE_H
#define COOPA_ITEM_ITEM_DATABASE_H

#include <coopa/item/item_def.h>
#include <coopa/item/item_id.h>
#include <unordered_map>
#include <utility>

namespace coopa {
namespace item {

/**
 * @class ItemDatabase
 * @brief A lookup table of ItemDefs, the shared backing data every ItemStack
 *        (via its ItemId) resolves through.
 *
 * Plain value type -- copyable and movable, with no coopa::event::Signal
 * member (Signal is copy-disabled; a database is meant to be handed around
 * as a `shared_ptr<ItemDatabase>` by AssetManager/ItemDatabaseLoader, which
 * requires the pointee itself to be an ordinary object). Definitions can
 * come from C++ (define()) and from YAML (item_database_loader.h's
 * parse_item_database(), which merges into an existing instance) side by
 * side -- neither path is privileged.
 */
class ItemDatabase {
public:
    /** @brief Adds or replaces the definition for `def.id`. */
    void define(ItemDef def) {
        defs_[def.id] = std::move(def);
    }

    /** @brief Looks up a definition by id, or nullptr if not defined. */
    const ItemDef* find(const ItemId& id) const {
        auto it = defs_.find(id);
        return it != defs_.end() ? &it->second : nullptr;
    }

    /** @brief True if `id` has a registered definition. */
    bool contains(const ItemId& id) const { return defs_.count(id) != 0; }

    /** @brief The per-slot stack limit for `id`, or k_default_max_stack if undefined. */
    int max_stack_of(const ItemId& id) const {
        const ItemDef* def = find(id);
        return def ? def->max_stack : k_default_max_stack;
    }

    size_t size() const { return defs_.size(); }
    void clear() { defs_.clear(); }

    using const_iterator = std::unordered_map<ItemId, ItemDef>::const_iterator;
    const_iterator begin() const { return defs_.begin(); }
    const_iterator end() const { return defs_.end(); }

private:
    std::unordered_map<ItemId, ItemDef> defs_;
};

} // namespace item
} // namespace coopa

#endif // COOPA_ITEM_ITEM_DATABASE_H
