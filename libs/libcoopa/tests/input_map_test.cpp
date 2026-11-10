/**
 * @file input_map_test.cpp
 * @brief coopa::input::InputMap: named actions over keys (any bound key counts, unbinding
 *        clears), modifier chords, mouse-button bindings, and axis/vector composition -- both
 *        through a KeyState predicate and against a real Input.
 */
#include <coopa/testing/test.h>

#include <coopa/input/input.h>
#include <coopa/input/input_map.h>
#include <coopa/input/keys.h>

#include <glm/glm.hpp>

COOPA_TEST_SUITE("input_map");

using namespace coopa::input;

COOPA_TEST(action_is_down_when_any_bound_key_is_down) {
    InputMap map;
    map.bind("jump", Key::Space);
    map.bind("jump", Key::W);

    // Every key bound to an action participates: with two keys bound, holding
    // either one must report the action as down. The dense Key enum is what
    // keeps this exhaustive -- there is no out-of-band keycode to miss.
    EXPECT_TRUE(map.is_down("jump", [](Key k) { return k == Key::W; }));
    EXPECT_FALSE(map.is_down("jump", [](Key) { return false; }));
}

COOPA_TEST(unbound_and_unbound_again_actions_are_never_down) {
    auto always_true = [](Key) { return true; };
    InputMap map;
    EXPECT_FALSE(map.is_down("nonexistent", always_true));
    EXPECT_TRUE(map.bindings("nonexistent").empty());

    map.bind("fire", Key::F1);
    EXPECT_EQ(map.bindings("fire").size(), 1u);
    map.unbind("fire");
    EXPECT_TRUE(map.bindings("fire").empty());
    EXPECT_FALSE(map.is_down("fire", always_true));
}

COOPA_TEST(chord_requires_its_modifier) {
    InputMap map;
    map.bind("save", Key::S, Mods::Control);

    Input input;
    input.begin_frame(0.016f);
    input.push_key(Key::S, 0, KeyAction::Press, Mods::None);
    EXPECT_FALSE(map.is_down("save", input)); // S alone, no Control -- must not fire

    input.begin_frame(0.016f);
    input.push_key(Key::S, 0, KeyAction::Press, Mods::Control);
    EXPECT_TRUE(map.is_down("save", input)); // S + Control -- fires
}

COOPA_TEST(mouse_button_binding_reports_down_and_pressed) {
    InputMap map;
    map.bind("attack", MouseButton::Left);

    Input input;
    input.begin_frame(0.016f);
    EXPECT_FALSE(map.is_down("attack", input));
    input.push_mouse_button(MouseButton::Left, KeyAction::Press, Mods::None);
    EXPECT_TRUE(map.is_down("attack", input));
    EXPECT_TRUE(map.is_pressed("attack", input));
}

COOPA_TEST(axis_and_vector_combine_opposing_keys) {
    InputMap map;
    map.bind_axis("move_x", Key::D, Key::A);
    map.bind_vector("move", Key::D, Key::A, Key::W, Key::S);

    Input input;
    input.begin_frame(0.016f);
    input.push_key(Key::D, 0, KeyAction::Press, Mods::None);
    EXPECT_NEAR(map.axis("move_x", input), 1.0f, 1e-6f);
    EXPECT_VEC_NEAR(map.vector("move", input), glm::vec2(1.0f, 0.0f), 1e-6f);

    input.push_key(Key::W, 0, KeyAction::Press, Mods::None);
    EXPECT_VEC_NEAR(map.vector("move", input), glm::vec2(1.0f, 1.0f), 1e-6f);

    // Both keys of an axis held -- they cancel to 0, not undefined behavior.
    input.push_key(Key::A, 0, KeyAction::Press, Mods::None);
    EXPECT_NEAR(map.axis("move_x", input), 0.0f, 1e-6f);
}
