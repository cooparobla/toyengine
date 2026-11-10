/**
 * @file inventory.h
 * @brief Fixed-capacity item storage: the authoritative model behind any slot-based UI.
 */

#ifndef COOPA_ITEM_INVENTORY_H
#define COOPA_ITEM_INVENTORY_H

#include <coopa/event/signal.h>
#include <coopa/item/item_database.h>
#include <coopa/item/item_id.h>
#include <coopa/item/item_stack.h>
#include <algorithm>
#include <vector>

namespace coopa {
namespace item {

/**
 * @class Inventory
 * @brief A fixed number of ItemStack slots, with add/remove/move/merge/swap
 *        operations and change notification.
 *
 * This is the model a UI grid (e.g. uicoopa's InventoryGrid, via
 * InventoryBinding) visualizes rather than owns -- every mutation here is
 * the single source of truth, and every UI-originated action (a drag-drop,
 * a gamepad pick-place) is expected to route back through move_or_merge()
 * or the other methods here rather than mutating a view-local copy.
 *
 * Holds a non-owning `const ItemDatabase*` used to resolve each item's
 * max_stack; a null database (or an id the database doesn't define) falls
 * back to item::k_default_max_stack, so an Inventory is usable in tests
 * without a populated database.
 *
 * Move-only: holds coopa::event::Signal members, which are copy-disabled
 * (see coopa/event/signal.h).
 */
class Inventory {
public:
    using SlotChangedSignal  = coopa::event::Signal<int, const ItemStack&>;
    using SlotsSwappedSignal = coopa::event::Signal<int, int>;

    Inventory() = default;

    /**
     * @param capacity Number of slots.
     * @param db Non-owning database used to resolve max_stack per item; may be
     *        null (every item then uses k_default_max_stack).
     */
    explicit Inventory(int capacity, const ItemDatabase* db = nullptr)
        : slots_(static_cast<size_t>(std::max(0, capacity))), database_(db) {}

    Inventory(const Inventory&) = delete;
    Inventory& operator=(const Inventory&) = delete;
    Inventory(Inventory&&) = default;
    Inventory& operator=(Inventory&&) = default;

    int capacity() const { return static_cast<int>(slots_.size()); }

    const ItemStack& at(int index) const {
        if (index >= 0 && index < capacity()) return slots_[static_cast<size_t>(index)];
        static ItemStack s_empty;
        return s_empty;
    }

    /** @brief Overwrites a slot outright (no merge/stack logic) and notifies. */
    void set(int index, ItemStack stack, bool notify = true) {
        if (index < 0 || index >= capacity()) return;
        slots_[static_cast<size_t>(index)] = std::move(stack);
        if (notify) on_slot_changed.emit(index, slots_[static_cast<size_t>(index)]);
    }

    void clear_slot(int index) { set(index, ItemStack{}); }

    /** @brief The first slot index with an empty stack, or -1 if none. */
    int first_empty() const {
        for (int i = 0; i < capacity(); ++i) {
            if (slots_[static_cast<size_t>(i)].empty()) return i;
        }
        return -1;
    }

    /** @brief Total count of `id` across every slot. */
    int count_of(const ItemId& id) const {
        int total = 0;
        for (const auto& s : slots_) {
            if (!s.empty() && s.item == id) total += s.count;
        }
        return total;
    }

    /**
     * @brief Adds `count` of `id`, filling partial stacks (in slot-index order)
     *        before spilling into empty slots.
     * @return The leftover count that didn't fit (0 if it all fit).
     */
    int add(const ItemId& id, int count) {
        if (!id.is_valid() || count <= 0) return count > 0 ? count : 0;
        int max_stack = max_stack_of_(id);
        int remaining = count;

        for (int i = 0; i < capacity() && remaining > 0; ++i) {
            ItemStack& s = slots_[static_cast<size_t>(i)];
            if (s.empty() || s.item != id) continue;
            int space = max_stack - s.count;
            if (space <= 0) continue;
            int transfer = std::min(space, remaining);
            s.count += transfer;
            remaining -= transfer;
            on_slot_changed.emit(i, s);
        }

        for (int i = 0; i < capacity() && remaining > 0; ++i) {
            ItemStack& s = slots_[static_cast<size_t>(i)];
            if (!s.empty()) continue;
            int transfer = std::min(max_stack, remaining);
            s = ItemStack{id, transfer};
            remaining -= transfer;
            on_slot_changed.emit(i, s);
        }

        return remaining;
    }

    /**
     * @brief Removes up to `count` of `id`, taking from slots in index order.
     * @return The number actually removed (may be less than requested).
     */
    int remove(const ItemId& id, int count) {
        if (!id.is_valid() || count <= 0) return 0;
        int remaining = count;
        int removed = 0;

        for (int i = 0; i < capacity() && remaining > 0; ++i) {
            ItemStack& s = slots_[static_cast<size_t>(i)];
            if (s.empty() || s.item != id) continue;
            int take = std::min(s.count, remaining);
            s.count -= take;
            remaining -= take;
            removed += take;
            if (s.count <= 0) s = ItemStack{};
            on_slot_changed.emit(i, s);
        }

        return removed;
    }

    /**
     * @brief The authoritative three-way transfer rule: move into an empty
     *        slot, merge stacks of the same item (up to max_stack, leaving
     *        any remainder in `from`), or swap two different items.
     * @return false if out of range, `from_slot == to_slot`, or `from_slot` is empty.
     */
    bool move_or_merge(int from_slot, int to_slot) {
        if (from_slot < 0 || from_slot >= capacity() ||
            to_slot < 0 || to_slot >= capacity() || from_slot == to_slot) {
            return false;
        }

        ItemStack& from = slots_[static_cast<size_t>(from_slot)];
        ItemStack& to   = slots_[static_cast<size_t>(to_slot)];
        if (from.empty()) return false;

        if (to.empty()) {
            to = from;
            from = ItemStack{};
        } else if (to.item == from.item) {
            int max_stack = max_stack_of_(to.item);
            int space = max_stack - to.count;
            int transfer = std::min(space, from.count);
            to.count += transfer;
            from.count -= transfer;
            if (from.count <= 0) from = ItemStack{};
        } else {
            std::swap(from, to);
        }

        on_slot_changed.emit(from_slot, slots_[static_cast<size_t>(from_slot)]);
        on_slot_changed.emit(to_slot, slots_[static_cast<size_t>(to_slot)]);
        on_slots_swapped.emit(from_slot, to_slot);
        return true;
    }

    /** @brief Unconditionally swaps two slots' contents (no merge logic), notifying both. */
    bool swap(int a, int b) {
        if (a < 0 || a >= capacity() || b < 0 || b >= capacity() || a == b) return false;
        std::swap(slots_[static_cast<size_t>(a)], slots_[static_cast<size_t>(b)]);
        on_slot_changed.emit(a, slots_[static_cast<size_t>(a)]);
        on_slot_changed.emit(b, slots_[static_cast<size_t>(b)]);
        on_slots_swapped.emit(a, b);
        return true;
    }

    /**
     * @brief Splits `count` off of slot `from` into slot `to` (which must be
     *        empty), leaving the remainder in `from`.
     * @return false if `from` is empty, `count` isn't strictly between 0 and
     *         `from`'s count, or `to` isn't empty.
     */
    bool split(int from, int count, int to) {
        if (from < 0 || from >= capacity() || to < 0 || to >= capacity() || from == to) return false;
        ItemStack& src = slots_[static_cast<size_t>(from)];
        ItemStack& dst = slots_[static_cast<size_t>(to)];
        if (src.empty() || !dst.empty() || count <= 0 || count >= src.count) return false;

        dst = ItemStack{src.item, count};
        src.count -= count;

        on_slot_changed.emit(from, src);
        on_slot_changed.emit(to, dst);
        return true;
    }

    SlotChangedSignal  on_slot_changed;
    SlotsSwappedSignal on_slots_swapped;

private:
    int max_stack_of_(const ItemId& id) const {
        return database_ ? database_->max_stack_of(id) : k_default_max_stack;
    }

    std::vector<ItemStack> slots_;
    const ItemDatabase*    database_ = nullptr;
};

} // namespace item
} // namespace coopa

#endif // COOPA_ITEM_INVENTORY_H
