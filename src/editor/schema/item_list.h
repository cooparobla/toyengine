#pragma once

/**
 * @file item_list.h
 * @brief What a `FieldKind::ItemList` row needs decided before it is drawn: one item's noun,
 *        one item's title, and the item a fresh "Add" produces.
 *
 * Pure functions over the schema and the YAML, so the naming rules are unit-testable without
 * a UI context -- the inspector's drawer then only lays them out.
 */

#include <cstddef>
#include <string>

#include "editor/schema/component_schema.h"

namespace toy {
namespace editor {

/**
 * @brief The singular noun for one of `list`'s items, for its header and its Add button.
 *
 * `item_noun` when the schema sets one, else the list's display name with a single trailing
 * "s" dropped ("Wheels" -> "Wheel").
 */
std::string item_noun(const FieldDesc& list);

/**
 * @brief The title of `list`'s `index`-th item, naming it by its own content where it can.
 *
 * In order: the key named by `item_title_key`; the first of `label`, `name`, `title`, `id`
 * the item carries; the first of the item's own fields that holds text (a string, an asset
 * reference, a child reference or an enum); and failing all of those, the noun and the
 * item's 1-based position.
 *
 * A list that declares `fixed_order()` is prefixed with the item's index, because there the
 * position is not an accident of ordering but the meaning -- a splat channel, a face slot.
 */
std::string item_title(const FieldDesc& list, const Node& item, std::size_t index);

/**
 * @brief The item an "Add" appends: every key of `list`'s item schema at its own default.
 *
 * Each key starts at its own default rather than at an empty string, because a blank in a
 * numeric key is not something a scene parser can read back, and a list can be saved before
 * the user has touched the new row. A `label` or `name` key instead gets a distinguishable
 * placeholder, so two fresh items are tellable apart in the list.
 */
Node make_default_item(const FieldDesc& list, std::size_t index);

/**
 * @brief What an empty list says where its items would be ("No wheels yet.").
 *
 * One wording for every list, so an empty inspector list, an empty palette and an empty
 * panel list do not each invent their own sentence.
 */
std::string item_list_empty_text(const FieldDesc& list);

} // namespace editor
} // namespace toy
