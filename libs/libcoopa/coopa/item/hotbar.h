/**
 * @file hotbar.h
 * @brief A selectable window of contiguous slots within an Inventory.
 */

#ifndef COOPA_ITEM_HOTBAR_H
#define COOPA_ITEM_HOTBAR_H

#include <coopa/event/signal.h>
#include <coopa/item/inventory.h>
#include <coopa/item/item_stack.h>
#include <algorithm>

namespace coopa {
namespace item {

/**
 * @class Hotbar
 * @brief Tracks which of a contiguous range of an Inventory's slots is
 *        "selected" -- the model behind a typical bottom-of-screen quick-slot
 *        bar. Does not own or duplicate storage: `selected_stack()` always
 *        reads through to the backing Inventory.
 *
 * Move-only (holds a coopa::event::Signal member).
 */
class Hotbar {
public:
    Hotbar() = default;

    /**
     * @param inv Non-owning; must outlive this Hotbar.
     * @param first_slot Index into `inv` that this hotbar's slot 0 maps to.
     * @param count Number of slots this hotbar spans, starting at first_slot.
     */
    Hotbar(Inventory* inv, int first_slot, int count)
        : inventory_(inv), first_slot_(first_slot), count_(std::max(0, count)) {}

    Hotbar(const Hotbar&) = delete;
    Hotbar& operator=(const Hotbar&) = delete;
    Hotbar(Hotbar&&) = default;
    Hotbar& operator=(Hotbar&&) = default;

    Inventory* inventory() const { return inventory_; }
    int first_slot() const { return first_slot_; }
    int count() const { return count_; }

    /** @brief Currently selected slot, relative to first_slot() (0-based), or -1 if none. */
    int selected() const { return selected_; }

    /** @brief Absolute Inventory slot index of the current selection, or -1 if none. */
    int selected_slot() const { return selected_ < 0 ? -1 : first_slot_ + selected_; }

    /** @brief Selects relative index `i` (clamped into [0, count())); -1 clears selection.
     *         Emits on_selection_changed only when the selection actually changes. */
    void select(int i) {
        int clamped = (i < 0) ? -1 : std::min(i, count_ - 1);
        if (clamped == selected_) return;
        selected_ = clamped;
        on_selection_changed.emit(selected_);
    }

    /** @brief Selects the next slot, wrapping to 0 past the end. No-op if count() == 0. */
    void next() {
        if (count_ <= 0) return;
        int base = selected_ < 0 ? -1 : selected_;
        select((base + 1) % count_);
    }

    /** @brief Selects the previous slot, wrapping to count()-1 before the start. No-op if count() == 0. */
    void prev() {
        if (count_ <= 0) return;
        int base = selected_ < 0 ? 0 : selected_;
        select((base - 1 + count_) % count_);
    }

    /** @brief The ItemStack in the currently selected slot, read through to the
     *         backing Inventory. Empty if nothing is selected or inventory() is null. */
    const ItemStack& selected_stack() const {
        static ItemStack s_empty;
        if (!inventory_ || selected_ < 0) return s_empty;
        return inventory_->at(selected_slot());
    }

    coopa::event::Signal<int> on_selection_changed;

private:
    Inventory* inventory_   = nullptr;
    int        first_slot_  = 0;
    int        count_       = 0;
    int        selected_    = -1;
};

} // namespace item
} // namespace coopa

#endif // COOPA_ITEM_HOTBAR_H
