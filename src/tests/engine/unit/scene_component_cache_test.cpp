/**
 * @file scene_component_cache_test.cpp
 * @brief The Scene's cached component queries: get_components() / find_objects_with() /
 *        find_first_component() stay correct across every structural change that invalidates
 *        them -- component add/remove, child attach/detach, active toggles, root add/remove --
 *        and keep their documented pre-order, per-object-active semantics.
 *
 * The cache itself is invisible by design (the queries' signatures and results are unchanged);
 * what these tests pin is the invalidation contract. Every mutation path below must be observed
 * by the very next query -- a stale entry after remove_component() would be a dangling pointer,
 * not just a wrong answer.
 */

#include <coopa/testing/test.h>

#include <memory>
#include <vector>

#include <coopa/scene/scene.h>
#include <toyengine/scene/free_mover.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("scene_component_cache");

using coopa::scene::Scene;
using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;
using toy::scene::FreeMover;

namespace {

/** @brief A root object carrying a Transform, added to the scene; returns the raw object. */
SceneObject* add_root(Scene& scene, const std::string& name) {
    auto obj = std::make_unique<SceneObject>(name);
    obj->add_component<TransformComponent>();
    return scene.add_root_object(std::move(obj));
}

std::vector<std::string> object_names(const std::vector<SceneObject*>& objects) {
    std::vector<std::string> names;
    names.reserve(objects.size());
    for (const SceneObject* o : objects) names.push_back(o->name());
    return names;
}

} // namespace

COOPA_TEST(repeated_queries_return_identical_results) {
    Scene scene("cache");
    SceneObject* a = add_root(scene, "a");
    add_root(scene, "b");
    a->add_component<FreeMover>();

    const auto first  = scene.get_components<TransformComponent>();
    const auto second = scene.get_components<TransformComponent>();
    expect(first.size() == 2 && first == second, "an unchanged scene answers the same twice");
    expect(scene.get_components<FreeMover>().size() == 1, "each type is tracked on its own");
}

COOPA_TEST(component_add_and_remove_are_seen_by_the_next_query) {
    Scene scene("cache");
    SceneObject* a = add_root(scene, "a");
    expect(scene.get_components<FreeMover>().empty(), "no FreeMover yet");

    FreeMover* fm = a->add_component<FreeMover>();
    const auto with = scene.get_components<FreeMover>();
    expect(with.size() == 1 && with[0] == fm, "a component added at runtime appears at once");
    expect(scene.find_first_component<FreeMover>() == fm, "find_first agrees");

    a->remove_component(fm);
    expect(scene.get_components<FreeMover>().empty(),
           "a removed (destroyed) component is gone from the very next query");
    expect(scene.find_first_component<FreeMover>() == nullptr, "find_first agrees it is gone");
}

COOPA_TEST(child_attach_and_detach_are_seen_by_the_next_query) {
    Scene scene("cache");
    SceneObject* parent = add_root(scene, "parent");
    scene.get_components<FreeMover>(); // prime the cache before the structural change

    // Attached via SceneObject::add_child alone -- no Scene::adopt() -- so this also covers the
    // hook propagating from the parent onto the added subtree.
    auto spawned = std::make_unique<SceneObject>("spawned");
    spawned->add_component<TransformComponent>();
    FreeMover* fm = spawned->add_component<FreeMover>();
    SceneObject* child = parent->add_child(std::move(spawned));

    const auto found = scene.get_components<FreeMover>();
    expect(found.size() == 1 && found[0] == fm, "a subtree attached with add_child() appears");

    // A further edit INSIDE the attached subtree must also invalidate (the propagated hook).
    child->remove_component(fm);
    expect(scene.get_components<FreeMover>().empty(), "an edit inside the spawned subtree is seen");

    FreeMover* again = child->add_component<FreeMover>();
    expect(scene.find_first_component<FreeMover>() == again, "and so is the one after it");

    std::unique_ptr<SceneObject> detached = parent->detach_child(child);
    expect(scene.get_components<FreeMover>().empty(), "a detached subtree's components are gone");
    expect(detached != nullptr, "the detached subtree is returned to the caller");
}

COOPA_TEST(active_toggles_follow_the_documented_per_object_rule) {
    Scene scene("cache");
    SceneObject* parent = add_root(scene, "parent");
    auto child_owned = std::make_unique<SceneObject>("child");
    child_owned->add_component<TransformComponent>();
    FreeMover* child_fm = child_owned->add_component<FreeMover>();
    parent->add_child(std::move(child_owned));
    FreeMover* parent_fm = parent->add_component<FreeMover>();

    expect(scene.get_components<FreeMover>().size() == 2, "both start active");

    parent->set_active(false);
    const auto filtered = scene.get_components<FreeMover>();
    // The queries filter each object's OWN active flag only -- an inactive parent's active
    // child is still returned. This is long-standing query behaviour the cache must not change.
    expect(filtered.size() == 1 && filtered[0] == child_fm,
           "an inactive object is excluded; its active child is not");

    parent->set_active(true);
    const auto restored = scene.get_components<FreeMover>();
    expect(restored.size() == 2 && restored[0] == parent_fm,
           "re-activating restores it, in pre-order");
}

COOPA_TEST(results_keep_pre_order_across_rebuilds) {
    Scene scene("cache");
    SceneObject* a = add_root(scene, "a");
    auto b_owned = std::make_unique<SceneObject>("b");
    b_owned->add_component<TransformComponent>();
    a->add_child(std::move(b_owned));
    add_root(scene, "c");

    auto names = object_names(scene.find_objects_with<TransformComponent>());
    expect(names == std::vector<std::string>({"a", "b", "c"}), "pre-order: a, then its child b, then c");

    add_root(scene, "d"); // invalidates; the rebuilt order must still be pre-order
    names = object_names(scene.find_objects_with<TransformComponent>());
    expect(names == std::vector<std::string>({"a", "b", "c", "d"}), "still pre-order after a rebuild");
    expect(scene.find_first_component<TransformComponent>() == a->get_transform(),
           "find_first returns the pre-order head");
}

COOPA_TEST(root_removal_and_manual_bump_invalidate) {
    Scene scene("cache");
    SceneObject* a = add_root(scene, "a");
    add_root(scene, "b");
    expect(scene.get_components<TransformComponent>().size() == 2, "two roots");

    scene.remove_root_object(a);
    expect(scene.get_components<TransformComponent>().size() == 1,
           "a destroyed root's components are gone from the next query");

    // Direct container surgery is the one path the Scene cannot observe; the documented
    // escape hatch is bump_structure_revision().
    scene.root_objects().clear();
    scene.bump_structure_revision();
    expect(scene.get_components<TransformComponent>().empty(), "a manual bump invalidates");
}
