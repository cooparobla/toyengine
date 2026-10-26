/**
 * @file instance_select_test.cpp
 * @brief The instance selection model (editor/instance/instance_select.h): what a ref survives,
 *        what editing and erasing a selection do to the set, and the per-record edits a group
 *        operation is built from.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <memory>

#include <glm/gtc/quaternion.hpp>

#include "editor/instance/instance_select.h"

COOPA_TEST_SUITE("instance_select");

namespace toy::editor::testing {

using coopa::test::expect;
using coopa::test::expect_near;
using coopa::test::require;
using toy::instance::ChunkCoord;
using toy::instance::InstanceRecord;
using toy::instance::InstanceRef;
using toy::instance::InstanceSetData;

namespace {

InstanceRecord at(float x, float y, std::uint16_t proto = 0, std::uint32_t seed = 1) {
    InstanceRecord r;
    r.position = glm::vec3(x, y, 0.0f);
    r.prototype = proto;
    r.seed = seed;
    return r;
}

/** @brief A set with `n` instances in chunk (0, 0), a metre apart along +x. */
InstanceSetData row(int n) {
    InstanceSetData d;
    d.set_chunk_size(32.0f);
    for (int i = 0; i < n; ++i) d.add(at(static_cast<float>(i) + 0.5f, 0.5f, 0, static_cast<std::uint32_t>(i + 1)));
    return d;
}

InstanceRef ref(int index, ChunkCoord c = ChunkCoord(0, 0)) {
    return InstanceRef{c, static_cast<std::uint32_t>(index)};
}

} // namespace

COOPA_TEST(a_selection_is_a_set_add_remove_and_toggle_never_duplicate) {
    InstanceSelection sel;
    expect(sel.empty(), "instance_select: a new selection is empty");

    sel.add(ref(1));
    sel.add(ref(1));
    expect(sel.size() == 1, "instance_select: adding the same instance twice selects it once");
    expect(sel.contains(ref(1)), "instance_select: ...and it is in");
    expect(!sel.contains(ref(2)), "instance_select: ...while its neighbour is not");

    sel.toggle(ref(2));
    expect(sel.size() == 2, "instance_select: toggling an unselected instance adds it");
    sel.toggle(ref(2));
    expect(sel.size() == 1, "instance_select: toggling it again removes it");
    sel.remove(ref(99));
    expect(sel.size() == 1, "instance_select: removing something not selected is a no-op");

    // Chunk is part of the address: index 1 of two different chunks is two instances.
    sel.add(ref(1, ChunkCoord(1, 0)));
    expect(sel.size() == 2,
           "instance_select: the same index in another chunk is a different instance");
    sel.clear();
    expect(sel.empty(), "instance_select: clear() empties it");
}

COOPA_TEST(validate_drops_refs_that_address_nothing) {
    InstanceSetData d = row(5);
    InstanceSelection sel;
    for (int i = 0; i < 5; ++i) sel.add(ref(i));
    sel.add(ref(0, ChunkCoord(9, 9)));   // a chunk that was never painted
    sel.validate(d);
    expect(sel.size() == 5, "instance_select: a ref into an empty chunk is dropped");

    // Shrink the chunk behind the selection's back, as an erase or an undo would.
    std::vector<InstanceRecord> two(d.records(ChunkCoord(0, 0))->begin(),
                                    d.records(ChunkCoord(0, 0))->begin() + 2);
    d.set_records(ChunkCoord(0, 0), two);
    sel.validate(d);
    expect(sel.size() == 2,
           "instance_select: refs past the end are dropped -- an index is only as good as the "
           "records behind it");
}

COOPA_TEST(editing_a_selection_touches_only_what_is_selected) {
    InstanceSetData d = row(6);
    InstanceSelection sel;
    sel.add(ref(1));
    sel.add(ref(3));
    sel.add(ref(5));

    const std::size_t edited = sel.edit(d, [](InstanceRecord& r) { r.position.z = 10.0f; });
    expect(edited == 3, "instance_select: every selected record was edited");
    const auto* records = d.records(ChunkCoord(0, 0));
    require(records != nullptr && records->size() == 6, "instance_select: the chunk still holds six");
    for (std::size_t i = 0; i < records->size(); ++i) {
        const bool selected = (i % 2) == 1;
        expect_near((*records)[i].position.z, selected ? 10.0f : 0.0f, 1e-5f,
                    "instance_select: the unselected instances between them are untouched");
    }
}

COOPA_TEST(editing_a_selection_bumps_each_chunk_once) {
    InstanceSetData d;
    d.set_chunk_size(32.0f);
    d.add(at(1.0f, 1.0f));
    d.add(at(2.0f, 1.0f));
    d.add(at(40.0f, 1.0f));   // chunk (1, 0)

    InstanceSelection sel;
    sel.add(ref(0));
    sel.add(ref(1));
    sel.add(ref(0, ChunkCoord(1, 0)));

    const std::uint64_t before_a = d.chunk_revision(ChunkCoord(0, 0));
    const std::uint64_t before_b = d.chunk_revision(ChunkCoord(1, 0));
    sel.edit(d, [](InstanceRecord& r) { r.position.z += 1.0f; });
    expect(d.chunk_revision(ChunkCoord(0, 0)) != before_a, "instance_select: the first chunk changed");
    expect(d.chunk_revision(ChunkCoord(1, 0)) != before_b, "instance_select: so did the second");
    // Two instances in one chunk must not read as two separate changes to it: a consumer
    // watching the chunk should rebuild it once for the whole edit.
    expect(d.chunk_revision(ChunkCoord(0, 0)) + 1 == d.revision() ||
               d.chunk_revision(ChunkCoord(1, 0)) + 1 == d.revision(),
           "instance_select: each chunk is written back exactly once, however many of its "
           "instances the selection held");
}

COOPA_TEST(erasing_a_selection_removes_exactly_those_and_empties_it) {
    InstanceSetData d = row(6);
    InstanceSelection sel;
    sel.add(ref(0));
    sel.add(ref(2));
    sel.add(ref(4));

    const std::size_t erased = sel.erase(d);
    expect(erased == 3, "instance_select: three were erased");
    expect(d.size() == 3, "instance_select: three are left");
    expect(sel.empty(),
           "instance_select: the selection empties itself -- its refs addressed the records that "
           "are now gone, and keeping them would point at whatever slid into their place");

    // Erasing high-index-first is what makes this safe: the survivors are the odd ones.
    const auto* records = d.records(ChunkCoord(0, 0));
    require(records != nullptr && records->size() == 3, "instance_select: three records remain");
    for (std::size_t i = 0; i < 3; ++i) {
        expect_near((*records)[i].position.x, static_cast<float>(2 * i + 1) + 0.5f, 1e-5f,
                    "instance_select: the right ones survived -- erasing by descending index "
                    "never shifts an instance still to be removed");
    }
}

COOPA_TEST(the_centroid_is_the_mean_of_the_selected_positions) {
    InstanceSetData d = row(5);
    InstanceSelection sel;
    sel.add(ref(0));
    sel.add(ref(4));
    const glm::vec3 c = sel.centroid(d);
    expect_near(c.x, 2.5f, 1e-4f, "instance_select: the centroid sits between the two selected");
    expect_near(c.y, 0.5f, 1e-4f, "instance_select: ...on their shared y");
    expect_near(glm::length(InstanceSelection{}.centroid(d)), 0.0f, 1e-6f,
                "instance_select: an empty selection has no centre, and reports the origin");
}

COOPA_TEST(yaw_round_trips_through_turning_and_setting) {
    InstanceRecord r = at(0.0f, 0.0f);
    expect_near(instance_yaw(r), 0.0f, 1e-4f, "instance_select: an unrotated instance faces +x");

    turn_instance(r, glm::radians(90.0f));
    expect_near(instance_yaw(r), glm::radians(90.0f), 1e-4f, "instance_select: turning 90 reads back as 90");
    turn_instance(r, glm::radians(-90.0f));
    expect_near(instance_yaw(r), 0.0f, 1e-4f, "instance_select: turning back reads back as 0");

    set_instance_yaw(r, glm::radians(135.0f));
    expect_near(instance_yaw(r), glm::radians(135.0f), 1e-4f, "instance_select: setting a yaw lands on it");
    expect_near(glm::length(r.rotation), 1.0f, 1e-4f, "instance_select: the rotation stays unit length");
}

COOPA_TEST(turning_a_tilted_instance_spins_it_about_the_world_up_axis) {
    // An instance leaning 45 degrees off the vertical: its own up is not the world's.
    InstanceRecord r = at(0.0f, 0.0f);
    r.rotation = glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 up_before = r.rotation * glm::vec3(0.0f, 0.0f, 1.0f);

    turn_instance(r, glm::radians(180.0f));
    const glm::vec3 up_after = r.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    expect_near(up_after.z, up_before.z, 1e-4f,
                "instance_select: turning keeps the instance leaning the same amount -- it spins "
                "about the WORLD vertical, not about its own tilted axis");
    expect_near(up_after.x, -up_before.x, 1e-4f, "instance_select: ...with the lean swung round to face back");
}

COOPA_TEST(orbiting_swings_a_group_about_its_pivot_without_scrambling_it) {
    InstanceRecord a = at(2.0f, 0.0f), b = at(4.0f, 0.0f);
    const glm::vec3 pivot(3.0f, 0.0f, 0.0f);
    const float gap_before = glm::distance(a.position, b.position);

    orbit_instance(a, pivot, glm::radians(90.0f));
    orbit_instance(b, pivot, glm::radians(90.0f));
    expect_near(glm::distance(a.position, b.position), gap_before, 1e-4f,
                "instance_select: orbiting preserves the spacing -- turning a camp moves it, it "
                "does not scatter it");
    expect_near(a.position.x, 3.0f, 1e-4f, "instance_select: both swung onto the pivot's x...");
    expect_near(a.position.y, -1.0f, 1e-4f, "instance_select: ...one either side of it");
    expect_near(b.position.y, 1.0f, 1e-4f, "instance_select: ...the far one opposite");
    expect_near(instance_yaw(a), glm::radians(90.0f), 1e-4f,
                "instance_select: and each instance turned with the group, so it still faces the "
                "same way relative to it");
}

COOPA_TEST(scale_is_uniform_and_clamped) {
    InstanceRecord r = at(0.0f, 0.0f);
    scale_instance(r, 2.0f);
    expect_near(r.scale.x, 2.0f, 1e-5f, "instance_select: scaling multiplies");
    expect(r.scale.x == r.scale.y && r.scale.y == r.scale.z, "instance_select: ...uniformly");
    set_instance_scale(r, 0.0f);
    expect(r.scale.x > 0.0f,
           "instance_select: a zero scale is clamped away -- an instance scaled to nothing is an "
           "invisible thing you cannot click to fix");
    set_instance_scale(r, 1e6f);
    expect(r.scale.x <= 100.0f, "instance_select: and an absurd one is clamped too");
}

COOPA_TEST(offsetting_moves_without_rotating) {
    InstanceRecord r = at(1.0f, 2.0f);
    set_instance_yaw(r, glm::radians(45.0f));
    const float yaw = instance_yaw(r);
    offset_instance(r, glm::vec3(0.0f, 0.0f, 3.0f));
    expect_near(r.position.z, 3.0f, 1e-5f, "instance_select: raising moves it up");
    expect_near(r.position.x, 1.0f, 1e-5f, "instance_select: ...and nowhere else");
    expect_near(instance_yaw(r), yaw, 1e-5f, "instance_select: ...leaving its facing alone");
}

} // namespace toy::editor::testing
