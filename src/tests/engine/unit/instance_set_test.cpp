/**
 * @file instance_set_test.cpp
 * @brief The painted-instance store (toyengine/instance/instance_set.h): chunk routing, radius
 *        queries, the revision journal every consumer watches, and the `.instances` file.
 */

#include <coopa/testing/test.h>

#include <fstream>

#include <toyengine/instance/instance_set.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("instance_set");

using toy::instance::ChunkCoord;
using toy::instance::InstanceRecord;
using toy::instance::InstanceRef;
using toy::instance::InstanceSetData;

namespace {

InstanceRecord at(float x, float y, float z = 0.0f, std::uint16_t proto = 0) {
    InstanceRecord r;
    r.position = glm::vec3(x, y, z);
    r.prototype = proto;
    r.seed = static_cast<std::uint32_t>(x * 131.0f + y * 17.0f) | 1u;
    return r;
}

} // namespace

COOPA_TEST(records_route_to_the_chunk_their_position_falls_in) {
    InstanceSetData d;
    d.set_chunk_size(32.0f);
    d.add(at(1.0f, 1.0f));
    d.add(at(31.9f, 0.5f));
    d.add(at(33.0f, 1.0f));
    d.add(at(-1.0f, -1.0f));

    expect(d.size() == 4, "instance_set: four added records are four records");
    expect(d.chunk_of(glm::vec3(1.0f, 1.0f, 0.0f)) == ChunkCoord(0, 0),
           "instance_set: a position inside the first chunk maps to (0, 0)");
    expect(d.chunk_of(glm::vec3(33.0f, 1.0f, 0.0f)) == ChunkCoord(1, 0),
           "instance_set: one chunk past the origin maps to (1, 0)");
    expect(d.chunk_of(glm::vec3(-1.0f, -1.0f, 0.0f)) == ChunkCoord(-1, -1),
           "instance_set: a negative position floors, so the grid is symmetric about the origin");
    const auto* c00 = d.records(ChunkCoord(0, 0));
    require(c00 != nullptr, "instance_set: chunk (0, 0) holds records");
    expect(c00->size() == 2, "instance_set: both sub-32 m records land in the same chunk");
    expect(d.chunk_count() == 3, "instance_set: three distinct chunks were touched");
}

COOPA_TEST(changing_the_chunk_size_reroutes_without_moving_anything) {
    InstanceSetData d;
    d.set_chunk_size(32.0f);
    d.add(at(10.0f, 10.0f));
    d.add(at(40.0f, 10.0f));
    expect(d.chunk_count() == 2, "instance_set: at 32 m the two records are in different chunks");

    d.set_chunk_size(64.0f);
    expect(d.size() == 2, "instance_set: re-chunking loses no records");
    expect(d.chunk_count() == 1, "instance_set: at 64 m both records share one chunk");
    const auto* c = d.records(ChunkCoord(0, 0));
    require(c != nullptr && c->size() == 2, "instance_set: the merged chunk holds both");
    // The whole point: chunking is storage, so the positions are untouched.
    bool found_40 = false;
    for (const InstanceRecord& r : *c) found_40 = found_40 || r.position.x == 40.0f;
    expect(found_40, "instance_set: re-chunking never moves an instance, only which chunk holds it");
}

COOPA_TEST(radius_queries_and_spacing_see_across_chunk_borders) {
    InstanceSetData d;
    d.set_chunk_size(32.0f);
    d.add(at(31.0f, 16.0f));   // chunk (0, 0)
    d.add(at(33.0f, 16.0f));   // chunk (1, 0)

    std::vector<InstanceRef> hits;
    d.query_radius(glm::vec3(32.0f, 16.0f, 0.0f), 2.0f, hits);
    expect(hits.size() == 2,
           "instance_set: a query straddling a chunk border finds instances on both sides -- "
           "otherwise a brush would pile instances along every border");
    expect(d.any_within(glm::vec3(32.5f, 16.0f, 0.0f), 1.0f),
           "instance_set: any_within sees the neighbouring chunk too (this is the brush's minimum spacing)");
    expect(!d.any_within(glm::vec3(50.0f, 16.0f, 0.0f), 1.0f),
           "instance_set: any_within is false where nothing has been painted");
    // z is deliberately ignored: a brush spaces instances on the ground, not through it.
    d.add(at(31.0f, 16.0f, 100.0f));
    expect(d.any_within(glm::vec3(31.0f, 16.0f, 0.0f), 0.5f),
           "instance_set: spacing is measured in xy, so something 100 m overhead still counts as occupied");
}

COOPA_TEST(erase_in_radius_can_be_limited_to_one_prototype) {
    InstanceSetData d;
    d.add(at(0.0f, 0.0f, 0.0f, /*proto=*/0));
    d.add(at(1.0f, 0.0f, 0.0f, /*proto=*/1));
    d.add(at(20.0f, 0.0f, 0.0f, /*proto=*/0));

    expect(d.erase_in_radius(glm::vec3(0.0f), 5.0f, /*prototype=*/1) == 1,
           "instance_set: erasing prototype 1 in the radius removes only that one");
    expect(d.size() == 2, "instance_set: the other two survive");
    expect(d.erase_in_radius(glm::vec3(0.0f), 5.0f, /*prototype=*/-1) == 1,
           "instance_set: prototype -1 erases whatever is in the radius");
    expect(d.size() == 1, "instance_set: the record 20 m away was outside the radius");
}

COOPA_TEST(every_edit_bumps_the_sets_and_the_chunks_revision) {
    InstanceSetData d;
    d.set_chunk_size(32.0f);
    const std::uint64_t r0 = d.revision();
    expect(d.chunk_revision(ChunkCoord(0, 0)) == 0,
           "instance_set: a chunk nothing was painted in has revision 0");

    d.add(at(1.0f, 1.0f));
    const std::uint64_t r1 = d.revision(), c1 = d.chunk_revision(ChunkCoord(0, 0));
    expect(r1 > r0, "instance_set: adding a record bumps the set's revision");
    expect(c1 != 0, "instance_set: ...and its chunk's");

    d.add(at(100.0f, 100.0f));
    expect(d.revision() > r1, "instance_set: a second edit bumps the set again");
    expect(d.chunk_revision(ChunkCoord(0, 0)) == c1,
           "instance_set: editing one chunk leaves the others' revisions alone -- which is what "
           "lets the render cache rebuild only what moved");

    // A chunk that empties must still report a CHANGED revision, or a cache watching it would
    // read "revision 0, never painted" and keep drawing what is no longer there.
    d.set_records(ChunkCoord(0, 0), {});
    expect(d.chunk_revision(ChunkCoord(0, 0)) != c1 && d.chunk_revision(ChunkCoord(0, 0)) != 0,
           "instance_set: emptying a chunk bumps its revision rather than resetting it to 0");
}

COOPA_TEST(the_instances_file_round_trips_exactly) {
    const std::filesystem::path path = coopa::test::scratch_dir() / "forest.instances";
    InstanceSetData saved;
    saved.set_chunk_size(16.0f);
    for (int i = 0; i < 400; ++i) {
        InstanceRecord r = at(static_cast<float>(i % 20) * 3.1f, static_cast<float>(i / 20) * 2.7f,
                              static_cast<float>(i) * 0.01f, static_cast<std::uint16_t>(i % 3));
        r.rotation = glm::normalize(glm::quat(1.0f, 0.1f * static_cast<float>(i), 0.0f, 0.2f));
        r.scale = glm::vec3(0.8f + 0.001f * static_cast<float>(i));
        r.flags = (i % 7 == 0) ? InstanceRecord::Hidden : 0;
        saved.add(r);
    }
    expect(saved.unsaved(), "instance_set: a set with edits reports them as unsaved");
    require(saved.save(path.string()), "instance_set: save() writes the file");
    expect(!saved.unsaved(), "instance_set: saving clears the unsaved flag");

    InstanceSetData loaded;
    require(loaded.load(path.string()), "instance_set: load() reads it back");
    expect_near(loaded.chunk_size(), 16.0f, 1e-6f, "instance_set: the chunk size round-trips");
    expect(loaded.size() == saved.size(), "instance_set: every record round-trips");
    expect(loaded.chunk_count() == saved.chunk_count(), "instance_set: so does the chunking");
    expect(!loaded.unsaved(), "instance_set: a freshly loaded set has nothing unsaved");

    // Field by field, in one chunk: the file packs records field-major, so a byte landing in the
    // wrong plane would show up here and nowhere else.
    for (ChunkCoord c : saved.chunks()) {
        const auto* a = saved.records(c);
        const auto* b = loaded.records(c);
        require(a != nullptr && b != nullptr && a->size() == b->size(),
                "instance_set: each chunk comes back with the same record count");
        for (std::size_t i = 0; i < a->size(); ++i) {
            expect_near(glm::length((*a)[i].position - (*b)[i].position), 0.0f, 1e-6f,
                        "instance_set: positions survive the field-major packing exactly");
            expect_near(glm::length((*a)[i].scale - (*b)[i].scale), 0.0f, 1e-6f,
                        "instance_set: so do scales");
            expect_near(std::abs(glm::dot((*a)[i].rotation, (*b)[i].rotation)), 1.0f, 1e-6f,
                        "instance_set: so do rotations");
            expect((*a)[i].prototype == (*b)[i].prototype && (*a)[i].seed == (*b)[i].seed &&
                       (*a)[i].flags == (*b)[i].flags,
                   "instance_set: so do the prototype index, seed and flags");
        }
    }
}

COOPA_TEST(saving_twice_writes_identical_bytes) {
    const std::filesystem::path dir = coopa::test::scratch_dir();
    InstanceSetData d;
    for (int i = 0; i < 50; ++i) d.add(at(static_cast<float>(i), static_cast<float>(i % 7)));
    require(d.save((dir / "a.instances").string()), "instance_set: the first save succeeds");
    require(d.save((dir / "b.instances").string()), "instance_set: a second save of the same set succeeds");

    auto read_all = [](const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    // The chunk index is written in sorted order rather than hash order, so an unchanged set is
    // byte-identical on every save -- which is what keeps a scene's diff quiet.
    expect(read_all(dir / "a.instances") == read_all(dir / "b.instances"),
           "instance_set: saving an unchanged set twice produces the same bytes");
}

COOPA_TEST(a_corrupt_or_foreign_file_is_refused_and_changes_nothing) {
    const std::filesystem::path path = coopa::test::scratch_dir() / "junk.instances";
    {
        std::ofstream out(path, std::ios::binary);
        out << "not an instances file at all, really not";
    }
    InstanceSetData d;
    d.add(at(5.0f, 5.0f));
    expect(!d.load(path.string()), "instance_set: a file without the magic is refused");
    expect(d.size() == 1, "instance_set: a refused load leaves what was already held intact");
    expect(!d.load((coopa::test::scratch_dir() / "nothing_here.instances").string()),
           "instance_set: a missing file is refused rather than throwing");
}

COOPA_TEST(the_store_hands_back_one_live_set_per_file) {
    using toy::instance::InstanceSetStore;
    const std::filesystem::path path = coopa::test::scratch_dir() / "shared.instances";
    InstanceSetStore::instance().clear();

    auto a = InstanceSetStore::instance().acquire(path.string());
    a->add(at(1.0f, 2.0f));
    // The editor re-parses a component on every keystroke; the second acquire is that re-parse.
    auto b = InstanceSetStore::instance().acquire(path.string());
    expect(a.get() == b.get(),
           "instance_set: acquiring the same file twice hands back the SAME live set, so unsaved "
           "paint survives a component re-parse");
    expect(b->size() == 1, "instance_set: ...with its unsaved records still in it");

    expect(InstanceSetStore::instance().any_dirty(), "instance_set: the store reports the unsaved set");
    expect(InstanceSetStore::instance().save_dirty() == 1, "instance_set: save_dirty writes it");
    expect(!InstanceSetStore::instance().any_dirty(), "instance_set: and nothing is dirty afterwards");
    expect(InstanceSetStore::instance().path_of(*a) == path.string(),
           "instance_set: the store knows which file a set came from");
    InstanceSetStore::instance().clear();
}
