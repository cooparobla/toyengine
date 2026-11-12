/**
 * @file transform_test.cpp
 * @brief coopa::util::Transform rotation conventions (Euler <-> quaternion are exact inverses;
 *        quaternion rotation matches glm::mat4_cast) and the TransformSystem: its eager resolve
 *        matches the lazy per-object path, and resolved matrices are race-free to read.
 */
#include <coopa/testing/test.h>

#include <coopa/job/engine.h>
#include <coopa/job/parallel_for.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/systems/transform_system.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

COOPA_TEST_SUITE("transform");

namespace {

/** @brief Largest |a - b| over every matrix element. */
float max_abs_diff(const glm::mat4& a, const glm::mat4& b) {
    float m = 0.0f;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m = std::max(m, std::abs(a[r][c] - b[r][c]));
    return m;
}

/** @brief A root with a Transform at (1,2,3) and `children` parented children at (i,0,0). */
void build_hierarchy(coopa::scene::Scene& scene, int children) {
    using namespace coopa::scene;
    auto root = std::make_unique<SceneObject>("Root");
    auto* root_tc = root->add_component<TransformComponent>();
    root_tc->transform().set_position({1.0f, 2.0f, 3.0f});
    for (int i = 0; i < children; ++i) {
        auto child = std::make_unique<SceneObject>("Child" + std::to_string(i));
        auto* child_tc = child->add_component<TransformComponent>();
        child_tc->set_parent_transform(&root_tc->transform());
        child_tc->transform().set_position({static_cast<float>(i), 0.0f, 0.0f});
        root->add_child(std::move(child));
    }
    scene.add_root_object(std::move(root));
}

} // namespace

COOPA_TEST(euler_to_quaternion_and_back_reproduces_the_matrix) {
    coopa::util::Transform t;
    t.set_rotation({15.0f, 30.0f, 45.0f});
    const glm::mat4 before = t.get_world_matrix();

    // Round-trip through the quaternion path and back to Euler must reproduce the same
    // matrix -- set_rotation_quat()'s Euler re-derivation is the exact analytic inverse of
    // the eulerAngleZYX composition recompute_() uses.
    t.set_rotation_quat(t.rotation_quat());
    t.set_rotation(t.rotation_degrees());
    EXPECT_LT(max_abs_diff(before, t.get_world_matrix()), 1e-4f);
}

COOPA_TEST(quaternion_rotation_matches_mat4_cast_and_euler_set_clears_it) {
    coopa::util::Transform t;
    const glm::quat q = glm::angleAxis(glm::radians(37.0f), glm::normalize(glm::vec3(1.0f, 2.0f, 3.0f)));
    t.set_rotation_quat(q);
    // No position/scale set -> pure rotation matrix.
    EXPECT_LT(max_abs_diff(glm::mat4_cast(q), t.get_world_matrix()), 1e-4f);

    // set_rotation(vec3) must clear use_quat_, reverting to ordinary Euler authoring.
    t.set_rotation({0.0f, 0.0f, 0.0f});
    EXPECT_LT(max_abs_diff(glm::mat4(1.0f), t.get_world_matrix()), 1e-4f);
}

COOPA_TEST(transform_system_resolve_matches_lazy_world_matrices) {
    using namespace coopa::scene;

    Scene lazy_scene("Lazy");
    build_hierarchy(lazy_scene, 5);
    std::vector<glm::mat4> lazy_matrices;
    for (auto& child : lazy_scene.root_objects()[0]->children()) {
        lazy_matrices.push_back(child->get_transform()->get_world_matrix());
    }

    coopa::job::JobEngine engine(4);
    Scene resolved_scene("Resolved");
    build_hierarchy(resolved_scene, 5);
    resolved_scene.set_job_engine(&engine);
    install_transform_system(resolved_scene);
    resolved_scene.start();
    resolved_scene.update(0.016f); // runs TransformSystem
    resolved_scene.late_update(0.016f);

    size_t i = 0;
    for (auto& child : resolved_scene.root_objects()[0]->children()) {
        EXPECT_FALSE(child->get_transform()->transform().is_dirty());
        EXPECT_LT(max_abs_diff(child->get_transform()->transform().world_matrix(), lazy_matrices[i++]), 1e-6f);
    }
    engine.shutdown();
}

COOPA_TEST(resolved_world_matrices_are_race_free_to_read_concurrently) {
    using namespace coopa::scene;

    Scene scene("ConcurrentReads");
    build_hierarchy(scene, 64);

    coopa::job::JobEngine engine(4);
    scene.set_job_engine(&engine);
    install_transform_system(scene);
    scene.start();
    scene.update(0.016f);
    scene.late_update(0.016f);

    auto& children = scene.root_objects()[0]->children();
    std::vector<float> expected_x(children.size());
    for (size_t i = 0; i < children.size(); ++i) {
        expected_x[i] = children[i]->get_transform()->transform().world_matrix()[3].x;
    }

    // Every transform is clean after the resolve pass (world_matrix() itself
    // asserts this in debug builds) -- read every child's world_matrix()
    // concurrently, many times over, from several jobs at once. A race in
    // the underlying cache would corrupt a value or trip that assert;
    // recording a mismatch here (rather than asserting from inside the job
    // body) keeps a failure from becoming an uncaught-exception abort on a
    // worker thread.
    std::atomic<bool> mismatch{false};
    engine.parallel_for_blocking(children.size(), 1, [&](size_t start, size_t end) {
        for (int rep = 0; rep < 50; ++rep) {
            for (size_t i = start; i < end; ++i) {
                float got = children[i]->get_transform()->transform().world_matrix()[3].x;
                if (std::abs(got - expected_x[i]) >= 1e-6f) mismatch.store(true, std::memory_order_relaxed);
            }
        }
    });
    EXPECT_FALSE(mismatch.load());
    engine.shutdown();
}
