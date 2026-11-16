/**
 * @file item_list_test.cpp
 * @brief The naming and defaulting rules behind an inspector list (FieldKind::ItemList): one
 *        item's noun, how an item titles its own header, and what a fresh "Add" writes.
 *
 * Pure schema and YAML, no UI context -- the drawer's own behaviour is inspector.
 */

#include <coopa/testing/test.h>

#include <string>
#include <vector>

#include "editor/schema/item_list.h"
#include "editor/support/fixtures.h"

COOPA_TEST_SUITE("item_list");

namespace toy::editor::testing {

namespace {

/** @brief One item, as a mapping of key to text. */
Node item_of(const std::vector<std::pair<std::string, std::string>>& kv) {
    Node n = Node::mapping();
    for (const auto& [k, v] : kv) n[k] = Node(v);
    return n;
}

} // namespace

COOPA_TEST(a_noun_comes_from_the_schema_or_the_plural) {
    expect(item_noun(f_items("wheels", {f_float("radius", 0.3f)})) == "Wheel",
           "a trailing s is dropped to make the singular");
    expect(item_noun(f_items("bursts", {f_float("time", 0.0f)})) == "Burst", "...for any plural");
    expect(item_noun(with_item_noun(f_items("lods", {f_int("field", 0)}), "LOD")) == "LOD",
           "an explicit noun wins, for a plural that is not one");
    expect(item_noun(f_items("s", {f_int("a", 0)})) == "S", "a one-letter name is left alone");
}

COOPA_TEST(an_item_titles_itself_from_its_own_content) {
    const FieldDesc wheels = f_items("wheels", {f_string("object"), f_float("radius", 0.3f)});

    expect(item_title(wheels, item_of({{"object", "wheel_fl"}}), 0) == "wheel_fl",
           "the first of the item's own text fields names it");
    expect(item_title(wheels, item_of({{"name", "Front Left"}, {"object", "wheel_fl"}}), 0) == "Front Left",
           "a 'name' key outranks an arbitrary text field");
    expect(item_title(wheels, item_of({{"label", "FL"}, {"name", "Front Left"}}), 0) == "FL",
           "and 'label' outranks 'name'");

    const FieldDesc titled = with_item_title(wheels, "object");
    expect(item_title(titled, item_of({{"label", "FL"}, {"object", "wheel_fl"}}), 0) == "wheel_fl",
           "an explicit title key outranks every fallback");

    expect(item_title(wheels, item_of({}), 2) == "Wheel 3",
           "an item with nothing to go on is numbered from 1");
    expect(item_title(wheels, item_of({{"object", ""}}), 0) == "Wheel 1",
           "an empty value counts as absent");
}

COOPA_TEST(a_fixed_order_list_shows_each_items_index) {
    const FieldDesc layers = fixed_order(with_item_title(f_items("layers", {f_string("name")}), "name"));
    expect(item_title(layers, item_of({{"name", "Rock"}}), 2) == "2  Rock",
           "where the index IS the meaning, it is part of the title");
    expect(item_title(layers, item_of({}), 2) == "Layer 3",
           "an invented name is not numbered twice");
    expect(!layers.items_reorderable, "and the list declares itself unorderable");
}

COOPA_TEST(a_fresh_item_starts_at_every_keys_own_default) {
    const FieldDesc list = f_items("wheels", {
        f_string("object"),
        f_float("radius", 0.35f),
        f_int("axle", 2),
        f_bool("powered", true),
        f_enum("side", {"left", "right"}),
    });
    const Node m = make_default_item(list, 0);
    expect(m.is_mapping(), "the item is a mapping");
    expect(m.contains("object") && m.contains("radius") && m.contains("axle") &&
           m.contains("powered") && m.contains("side"), "with every key of the item schema");
    expect(m.at("radius").get_value<double>() > 0.34 && m.at("radius").get_value<double>() < 0.36,
           "a float starts at its schema default, not at 0");
    expect(m.at("axle").get_value<int64_t>() == 2, "an int starts at its schema default");
    expect(m.at("powered").get_value<bool>(), "a bool starts at its schema default");
    expect(m.at("side").get_value<std::string>() == "left", "an enum starts at its first option");
    expect(m.at("object").get_value<std::string>().empty(), "a plain string starts empty");
}

COOPA_TEST(fresh_items_with_a_name_key_are_tellable_apart) {
    const FieldDesc named = f_items("layers", {f_string("name"), f_float("scale", 1.0f)});
    expect(make_default_item(named, 0).at("name").get_value<std::string>() == "Item1",
           "the first fresh item is named for its position");
    expect(make_default_item(named, 1).at("name").get_value<std::string>() == "Item2",
           "and the second gets a different name");

    const FieldDesc labelled = f_items("rows", {f_string("label")});
    expect(make_default_item(labelled, 2).at("label").get_value<std::string>() == "Item 3",
           "a 'label' key is spelled for reading, with its space");
}

COOPA_TEST(an_empty_list_has_one_wording) {
    expect(item_list_empty_text(f_items("wheels", {f_string("object")})) == "No wheels yet.",
           "the plural display name, lowercased");
    expect(item_list_empty_text(with_label(f_items("on_death", {f_string("target")}), "Sub-Emitters")) ==
               "No sub-emitters yet.",
           "an explicit label is used as given");
}

} // namespace toy::editor::testing
