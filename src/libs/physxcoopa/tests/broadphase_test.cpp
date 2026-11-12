/**
 * @file broadphase_test.cpp
 * @brief Pair finding and pair filtering: the dynamic AABB tree against a brute-force oracle (before
 *        and after moves), the static/kinematic pair skip, and the layer collision matrix.
 */
#include <coopa/testing/test.h>

#include <physxcoopa/world.h>
#include <physxcoopa/broadphase/aabb_tree.h>
#include <physxcoopa/broadphase/layer_matrix.h>
#include <random>
#include <set>

COOPA_TEST_SUITE("broadphase");

using namespace coopa::physx;

COOPA_TEST(aabb_tree_finds_every_brute_force_overlap) {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> pos_dist(-20.0f, 20.0f);
    std::uniform_real_distribution<float> size_dist(0.1f, 2.0f);

    const int n = 1000;
    std::vector<geometry::AABB> boxes(n);
    for (int i = 0; i < n; ++i) {
        glm::vec3 center(pos_dist(rng), pos_dist(rng), pos_dist(rng));
        glm::vec3 half(size_dist(rng), size_dist(rng), size_dist(rng));
        boxes[i].min = center - half;
        boxes[i].max = center + half;
    }

    broadphase::AABBTree tree;
    std::vector<int32_t> proxies(n);
    for (int i = 0; i < n; ++i) {
        proxies[i] = tree.create_proxy(boxes[i], reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
    }

    // Brute-force oracle.
    std::set<std::pair<int, int>> brute_force_pairs;
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            if (boxes[i].overlaps(boxes[j])) brute_force_pairs.insert({i, j});
        }
    }

    std::set<std::pair<int, int>> tree_pairs;
    for (int i = 0; i < n; ++i) {
        tree.query(boxes[i], [&](void* user_data) {
            int j = static_cast<int>(reinterpret_cast<uintptr_t>(user_data));
            if (j != i) tree_pairs.insert({std::min(i, j), std::max(i, j)});
        });
    }

    // The tree's fat AABBs make it a conservative OVER-approximation -- every true overlap
    // must be found, but a fat-margin near-miss may also appear.
    for (const auto& p : brute_force_pairs) {
        ASSERT_TRUE(tree_pairs.count(p) == 1);
    }

    // Move every proxy slightly and re-verify: still a superset of a fresh brute-force pass
    // over the NEW boxes, and every fat AABB still contains its own tight bounds.
    for (int i = 0; i < n; ++i) {
        glm::vec3 shift(0.05f, -0.03f, 0.02f);
        boxes[i].min += shift;
        boxes[i].max += shift;
        tree.move_proxy(proxies[i], boxes[i], shift);
        ASSERT_TRUE(tree.fat_bounds(proxies[i]).contains(boxes[i]));
    }
    std::set<std::pair<int, int>> brute_force_pairs2;
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            if (boxes[i].overlaps(boxes[j])) brute_force_pairs2.insert({i, j});
        }
    }
    std::set<std::pair<int, int>> tree_pairs2;
    for (int i = 0; i < n; ++i) {
        tree.query(boxes[i], [&](void* user_data) {
            int j = static_cast<int>(reinterpret_cast<uintptr_t>(user_data));
            if (j != i) tree_pairs2.insert({std::min(i, j), std::max(i, j)});
        });
    }
    for (const auto& p : brute_force_pairs2) {
        ASSERT_TRUE(tree_pairs2.count(p) == 1);
    }
}

COOPA_TEST(static_and_kinematic_pairs_generate_no_manifolds) {
    PhysicsWorld world;
    dynamics::Body s1;
    s1.type = dynamics::BodyType::Static;
    s1.position = glm::vec3(0.0f);
    dynamics::Body s2 = s1;
    world.add_body(s1, collision::Shape::make_sphere(1.0f));
    world.add_body(s2, collision::Shape::make_sphere(1.0f));

    dynamics::Body k1;
    k1.type = dynamics::BodyType::Kinematic;
    k1.position = glm::vec3(5.0f, 0.0f, 0.0f);
    dynamics::Body k2 = k1;
    world.add_body(k1, collision::Shape::make_sphere(1.0f));
    world.add_body(k2, collision::Shape::make_sphere(1.0f));

    world.step_fixed(util::k_default_fixed_dt);
    ASSERT_TRUE(world.manifolds().empty());
}

COOPA_TEST(layer_matrix_blocks_pair_before_narrowphase) {
    PhysicsWorld world;
    world.layers().set_layer_collision(0, 1, false);

    dynamics::Body a;
    a.type = dynamics::BodyType::Static;
    a.position = glm::vec3(0.0f);
    collision::Shape shape_a = collision::Shape::make_sphere(1.0f);
    shape_a.layer = 0;
    world.add_body(a, shape_a);

    dynamics::Body b;
    b.position = glm::vec3(0.5f, 0.0f, 0.0f); // deeply overlapping shape_a
    b.mass = 1.0f;
    b.inv_mass = 1.0f;
    collision::Shape shape_b = collision::Shape::make_sphere(1.0f);
    shape_b.layer = 1;
    dynamics::BodyId id_b = world.add_body(b, shape_b);

    world.step_fixed(util::k_default_fixed_dt);
    ASSERT_TRUE(world.manifolds().empty());

    // Re-enable and confirm the same overlap now DOES produce a manifold (proves the
    // previous empty result was the layer filter, not a broadphase/narrowphase miss).
    world.layers().set_layer_collision(0, 1, true);
    world.step_fixed(util::k_default_fixed_dt);
    ASSERT_TRUE(!world.manifolds().empty());
    (void)id_b;
}
