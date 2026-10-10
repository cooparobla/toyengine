/**
 * @file yaml_writer_test.cpp
 * @brief coopa/yaml/writer.h as the editor saves with it: scalar typing survives emit -> parse, and keys
 * come out in the editor's order with short vectors in flow style.
 * 
 * Not here: the byte-stability of every file in assets/ (asset_roundtrip).
 */

#include <coopa/testing/test.h>

#include <coopa/yaml/writer.h>

#include "editor/support/fixtures.h"

COOPA_TEST_SUITE("yaml_writer");

namespace toy::editor::testing {

COOPA_TEST(every_scalar_kind_survives_emit_and_parse) {
    Node n = Node::mapping();
    n["f1"] = Node(1.0);
    n["f2"] = Node(0.1);
    n["f3"] = Node(-123.456789);
    n["f4"] = Node(1e-7);
    n["f5"] = Node(3.0e20);
    n["i"] = Node(int64_t(42));
    n["b"] = Node(true);
    n["s_true"] = Node(std::string("true"));
    n["s_num"] = Node(std::string("1.5"));
    n["s_empty"] = Node(std::string(""));
    n["s_colon"] = Node(std::string("a: b"));
    n["s_hash"] = Node(std::string("#x"));
    n["s_path"] = Node(std::string("assets/scenes/a b/scene.yaml"));
    n["s_quote"] = Node(std::string("say \"hi\""));
    const std::string text = coopa::yaml::emit(n);
    const Node back = Node::deserialize(text);
    expect(back == n, "every scalar kind survives emit -> parse");
    expect(back.at("f1").is_float_number(), "1.0 stays a float, not an int");
    expect(back.at("i").is_integer(), "an int stays an int");
    expect(back.at("s_true").is_string() && back.at("s_num").is_string(), "ambiguous strings stay strings");
    expect(coopa::yaml::emit(back) == text, "emit is stable across a round trip");
}

COOPA_TEST(identity_keys_come_first_and_short_vectors_are_flow) {
    Node obj = Node::mapping();
    obj["children"] = Node::sequence();
    obj["components"] = Node::sequence();
    obj["active"] = Node(true);
    obj["name"] = Node(std::string("Cube"));
    Node t = Node::mapping();
    t["z"] = Node(1.0); t["x"] = Node(0.0); t["y"] = Node(2.0);
    obj["position"] = t;
    const std::string text = coopa::yaml::emit(obj);
    expect(text.find("name:") < text.find("active:"), "name comes before active");
    expect(text.find("active:") < text.find("components:"), "identity keys before structure");
    expect(text.find("components:") < text.find("children:"), "children last");
    expect(text.find("position: { x: 0.0, y: 2.0, z: 1.0 }") != std::string::npos, "short vectors are flow style:\n" + text);
}

} // namespace toy::editor::testing
