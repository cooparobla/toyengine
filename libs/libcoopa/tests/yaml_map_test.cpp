/**
 * @file yaml_map_test.cpp
 * @brief coopa::collections::YAMLMap: typed get/set and a save/load round-trip through disk.
 */
#include <coopa/testing/test.h>

#include <coopa/collections/yaml_map.h>

#include <string>
#include <vector>

COOPA_TEST_SUITE("yaml_map");

COOPA_TEST(values_vectors_and_nested_maps_survive_save_and_load) {
    coopa::collections::YAMLMap map;
    map.set<int>("some_int", 42);
    map.set<std::string>("some_str", "hello");
    EXPECT_EQ(map.get<int>("some_int", 0), 42);
    EXPECT_EQ(map.get<std::string>("some_str", ""), "hello");

    const std::string path = (coopa::test::scratch_dir() / "config.yaml").string();
    map.save(path);

    coopa::collections::YAMLMap loaded = coopa::collections::YAMLMap::load(path);
    EXPECT_EQ(loaded.get<int>("some_int", 0), 42);
    EXPECT_EQ(loaded.get<std::string>("some_str", ""), "hello");
    EXPECT_TRUE(loaded.exists("some_int"));
    EXPECT_FALSE(loaded.exists("non_existent"));

    loaded.set_vector<int>("numbers", std::vector<int>{1, 2, 3});
    auto numbers = loaded.get_vector<int>("numbers");
    ASSERT_EQ(numbers.size(), 3u);
    EXPECT_EQ(numbers[0], 1);
    EXPECT_EQ(numbers[1], 2);
    EXPECT_EQ(numbers[2], 3);

    coopa::collections::YAMLMap nested;
    nested.set<double>("pi", 3.14159);
    loaded.set<fkyaml::node>("nested", nested.get_raw_node());
    EXPECT_EQ(loaded.get_node("nested").get<double>("pi", 0.0), 3.14159);
}
