/**
 * @file instance_painter_test.cpp
 * @brief instance_paint_demo end to end: the painter's records become instanced chunk batches
 *        that draw, prototypes resolve into their parts (an object asset into two), distance
 *        thinning shrinks a batch without re-uploading it, painting at runtime rebuilds only
 *        the chunk that changed, and collidable prototypes get streamed static colliders while
 *        collision-free ones cost nothing.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>

#include <glm/glm.hpp>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/core/engine.h>
#include <toyengine/instance/instance_painter.h>
#include <toyengine/instance/instance_painter_system.h>
#include <toyengine/instance/instance_set.h>
#include <toyengine/render/toy_render_pipeline.h>
#include <toyengine/scene/runtime_object.h>
#include <toyengine/world/terrain_component.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("instance_painter");

using namespace toy::test;
using toy::instance::ChunkCoord;
using toy::instance::InstancePainter;
using toy::instance::InstanceRecord;

namespace {

const char* k_demo = "assets/scenes/terrain/instance_paint_demo/scene.yaml";

std::string demo() { return std::string(ROOT_DIR) + "/" + k_demo; }

InstancePainter* painter_of(toy::core::Engine& engine) {
    return engine.scene().find_first_component<InstancePainter>();
}

toy::instance::InstancePainterSystem* system_of(toy::core::Engine& engine) {
    return toy::instance::find_instance_painter_system(engine.scene());
}

/** @brief Every runtime collider object the painter created, anywhere under the scene. */
int painter_collider_count(toy::core::Engine& engine) {
    int n = 0;
    for (const auto& root : engine.scene().root_objects()) {
        root->for_each_recursive([&](coopa::scene::SceneObject& o) {
            const auto* marker = o.get_component<toy::scene::RuntimeObject>();
            if (marker != nullptr && marker->owner_system == "InstancePainter") {
                n += static_cast<int>(o.children().size());
            }
        });
    }
    return n;
}

} // namespace

/** @brief The demo's painted world loads, resolves and draws. */
COOPA_TEST(demo_draws_its_painted_instances) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::instance::InstanceSetStore::instance().clear();
    toy::core::Engine engine(make_test_config(demo(), 320, 180, 320, 180));
    tick_frames(engine, 90);

    InstancePainter* p = painter_of(engine);
    require(p != nullptr && p->data != nullptr, "instance_painter: the demo's painter loaded");
    expect(p->data->size() > 1000,
           "instance_painter: the demo ships with its instances already painted (" +
               std::to_string(p->data->size()) + ") -- an empty painter would prove nothing");
    expect(p->data->chunk_count() > 4,
           "instance_painter: they are spread over several 32 m chunks, so culling and streaming "
           "have something to do");
    require(p->prototypes.size() == 4, "instance_painter: four prototypes in the palette");

    // An `object:` prototype must come apart into one part per MeshRenderer under it; the pine
    // has a trunk and a canopy, and a single-part resolution would silently drop the canopy.
    const auto& pine = p->prototypes[0];
    expect(pine.resolved, "instance_painter: the object-asset prototype resolved");
    expect(pine.parts.size() == 2,
           "instance_painter: objects/nature/pine resolves into TWO parts (trunk + canopy), each "
           "with its own local transform -- got " + std::to_string(pine.parts.size()));
    expect(pine.ready(), "instance_painter: both of its meshes loaded");
    expect(pine.bounds_max.z - pine.bounds_min.z > 3.0f,
           "instance_painter: its bounds span the whole tree, not just the trunk (" +
               std::to_string(pine.bounds_max.z - pine.bounds_min.z) + " m)");
    // A bare `mesh:` prototype is one part.
    expect(p->prototypes[1].parts.size() == 1, "instance_painter: a `mesh:` prototype is one part");

    const Frame f = engine.capture_image();
    expect(black_block_pixels(f) < 64, "instance_painter: no unwritten (black) blocks in the frame");
    dump_frame(f, "instance_paint_demo");
}

/** @brief Chunks become instanced batches whose bounds contain the instances in them. */
COOPA_TEST(chunks_become_instanced_batches_with_containing_bounds) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::instance::InstanceSetStore::instance().clear();
    toy::core::Engine engine(make_test_config(demo(), 320, 180, 320, 180));
    tick_frames(engine, 90);

    InstancePainter* p = painter_of(engine);
    auto* sys = system_of(engine);
    require(p != nullptr && sys != nullptr, "instance_painter: the painter and its system are present");
    expect(sys->instance_count() > 0,
           "instance_painter: the system built chunks around the camera (" +
               std::to_string(sys->instance_count()) + " instances)");
    std::size_t parts_total = 0;
    for (const auto& proto : p->prototypes) parts_total += std::max<std::size_t>(1, proto.parts.size());
    expect(sys->instance_count() <= p->data->size() * parts_total,
           "instance_painter: it builds one matrix per (instance, part) and no more -- the pine "
           "contributes two, the one-mesh prototypes one each");

    // Collect the batches the renderer would be handed this frame, with no frustum test, and
    // check each one's bounds actually hold the matrices it points at.
    toy::render::ParticleFrameState state;
    sys->collect_render(engine.scene(), glm::vec3(40.0f, 0.0f, 25.0f), {}, state);
    require(!state.meshes.empty(), "instance_painter: it hands the renderer batches");

    int checked = 0;
    bool bounds_ok = true, static_ok = true;
    std::set<std::uint64_t> keys;
    for (const auto& b : state.meshes) {
        if (b.proxy == nullptr || b.matrices == nullptr) continue;
        static_ok = static_ok && b.static_key != 0 && b.static_total >= b.count;
        keys.insert(b.static_key);
        for (std::uint32_t i = 0; i < b.count && bounds_ok; ++i) {
            const glm::vec3 pos = glm::vec3(b.matrices[i][3]);
            bounds_ok = pos.x >= b.bounds_min.x - 1e-2f && pos.x <= b.bounds_max.x + 1e-2f &&
                        pos.y >= b.bounds_min.y - 1e-2f && pos.y <= b.bounds_max.y + 1e-2f &&
                        pos.z >= b.bounds_min.z - 1e-2f && pos.z <= b.bounds_max.z + 1e-2f;
            ++checked;
        }
    }
    expect(checked > 100, "instance_painter: enough instances to be worth checking (" +
                              std::to_string(checked) + ")");
    expect(bounds_ok,
           "instance_painter: every batch's bounds contain the instances it draws -- a batch is "
           "one culling unit, so bounds that missed would pop instances in and out");
    expect(static_ok,
           "instance_painter: every batch is STATIC (a non-zero key, and a total at least its "
           "drawn count), which is what gives it a persistent GPU buffer instead of a per-frame copy");
    expect(keys.size() == state.meshes.size(),
           "instance_painter: each (chunk, prototype, part) has its OWN key -- two sharing one "
           "would overwrite each other's buffer");
}

/** @brief Distance thins a batch's drawn count without changing what is uploaded. */
COOPA_TEST(distance_thins_the_drawn_count_but_not_the_upload) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::instance::InstanceSetStore::instance().clear();
    toy::core::Engine engine(make_test_config(demo(), 160, 90, 160, 90));
    tick_frames(engine, 60);
    auto* sys = system_of(engine);
    InstancePainter* p = painter_of(engine);
    require(sys != nullptr && p != nullptr, "instance_painter: present");

    // The grass prototype has a 70 m draw distance that starts fading at 0.5 of it.
    const int grass = p->find_prototype("grass");
    require(grass >= 0, "instance_painter: the demo has a `grass` prototype");

    // Grass batches are the ones drawn through the grass prototype's own proxy renderer.
    require(!p->prototypes[static_cast<std::size_t>(grass)].parts.empty(),
            "instance_painter: the grass prototype resolved");
    const auto* grass_proxy = p->prototypes[static_cast<std::size_t>(grass)].parts.front().proxy;

    auto grass_counts = [&](const glm::vec3& camera) {
        toy::render::ParticleFrameState state;
        sys->collect_render(engine.scene(), camera, {}, state);
        std::uint32_t drawn = 0, total = 0;
        for (const auto& b : state.meshes) {
            if (b.proxy != grass_proxy) continue;
            drawn += b.count;
            total += b.static_total;
        }
        return std::pair<std::uint32_t, std::uint32_t>(drawn, total);
    };

    const auto near_cam = grass_counts(glm::vec3(0.0f, 0.0f, 10.0f));
    const auto far_cam = grass_counts(glm::vec3(0.0f, 300.0f, 10.0f));
    expect(near_cam.first > 0, "instance_painter: close to the field, grass draws");
    expect(near_cam.second >= near_cam.first,
           "instance_painter: the drawn count is a prefix of what is uploaded, never more");
    expect(far_cam.first < near_cam.first,
           "instance_painter: from 300 m away fewer tufts draw than from on top of them -- "
           "thinning is a prefix of the shuffled chunk, so it never rebuilds anything (" +
               std::to_string(far_cam.first) + " vs " + std::to_string(near_cam.first) + ")");
}

/** @brief Painting at runtime rebuilds the chunk that changed, and only that one. */
COOPA_TEST(a_runtime_edit_rebuilds_only_its_own_chunk) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::instance::InstanceSetStore::instance().clear();
    toy::core::Engine engine(make_test_config(demo(), 160, 90, 160, 90));
    tick_frames(engine, 60);
    InstancePainter* p = painter_of(engine);
    auto* sys = system_of(engine);
    require(p != nullptr && sys != nullptr, "instance_painter: present");

    // Snapshot each batch's upload version before the edit.
    auto versions = [&]() {
        toy::render::ParticleFrameState state;
        sys->collect_render(engine.scene(), glm::vec3(40.0f, 0.0f, 25.0f), {}, state);
        std::map<std::uint64_t, std::uint64_t> v;
        for (const auto& b : state.meshes) v[b.static_key] = b.static_version;
        return v;
    };
    const auto before = versions();
    require(!before.empty(), "instance_painter: there are batches to compare");

    // One new instance, right next to the camera, in a chunk that already has some.
    InstanceRecord r;
    r.position = glm::vec3(41.0f, 1.0f, 22.0f);
    r.prototype = 1;   // the boulder: a one-part mesh prototype
    r.seed = 4242u;
    const ChunkCoord edited = p->data->chunk_of(r.position);
    const std::size_t count_before = p->data->size();
    p->data->add(r);
    expect(p->data->size() == count_before + 1, "instance_painter: the record went in");
    tick_frames(engine, 10);   // the cache rebuilds a couple of chunks a frame

    const auto after = versions();
    int changed = 0, unchanged = 0;
    for (const auto& [key, version] : after) {
        const auto it = before.find(key);
        if (it == before.end()) { ++changed; continue; }   // a batch that did not exist before
        if (it->second != version) ++changed; else ++unchanged;
    }
    expect(changed > 0,
           "instance_painter: the edited chunk's batch got a new upload version, so the renderer "
           "re-uploads it");
    expect(unchanged > 0,
           "instance_painter: the OTHER chunks kept theirs -- a painted world would be unusable "
           "if one dab re-uploaded every chunk in it");
    expect(p->data->chunk_revision(edited) != 0,
           "instance_painter: the edited chunk's revision moved, which is what the cache watches");
}

/** @brief Collidable prototypes get streamed colliders; collision-free ones cost nothing. */
COOPA_TEST(collision_is_streamed_and_only_for_prototypes_that_asked) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::instance::InstanceSetStore::instance().clear();
    toy::core::Engine engine(make_test_config(demo(), 160, 90, 160, 90));
    tick_frames(engine, 90);
    InstancePainter* p = painter_of(engine);
    require(p != nullptr, "instance_painter: present");

    const int with_collision = painter_collider_count(engine);
    expect(with_collision > 0,
           "instance_painter: collidable instances near the camera have colliders (" +
               std::to_string(with_collision) + ")");
    // 1400 of the demo's instances are grass, which asked for none. If collision were applied
    // to everything, this count would be in the thousands rather than the hundreds.
    expect(with_collision < static_cast<int>(p->data->size()) / 2,
           "instance_painter: far fewer colliders than instances -- the 1400 collision: none "
           "grass tufts cost the broadphase nothing");

    // Turning the radius off drops every one of them.
    p->collision_radius = 0.0f;
    tick_frames(engine, 5);
    expect(painter_collider_count(engine) == 0,
           "instance_painter: collision_radius 0 removes every collider, whatever the prototypes ask");

    // ...and turning it back on brings them back.
    p->collision_radius = 80.0f;
    tick_frames(engine, 10);
    expect(painter_collider_count(engine) > 0,
           "instance_painter: restoring the radius streams them back in");
}

/** @brief A painted, collidable instance is solid: a physics ray finds it above the ground. */
COOPA_TEST(a_painted_tree_is_solid_to_physics) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::instance::InstanceSetStore::instance().clear();
    toy::core::Engine engine(make_test_config(demo(), 160, 90, 160, 90));
    tick_frames(engine, 60);
    InstancePainter* p = painter_of(engine);
    auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(engine.scene().find_system("Physics"));
    require(p != nullptr && physics != nullptr, "instance_painter: the painter and physics are present");

    // Clear a patch first: the demo is full of painted props, and a ray fired across it would
    // just as likely hit a boulder on the way as the tree under test.
    InstanceRecord tree;
    tree.position = glm::vec3(44.0f, 2.0f, 0.0f);
    p->data->erase_in_radius(tree.position, 10.0f, -1);
    // On the ground: the demo's terrain is the authority, so ask it.
    const auto views = engine.scene().get_components<toy::world::TerrainComponent>();
    require(!views.empty(), "instance_painter: the demo has a terrain to stand it on");
    views.front()->sync_field();
    tree.position.z = views.front()->field_view().height(tree.position);
    tree.prototype = 0;   // the pine, collision: auto -> a capsule
    tree.seed = 7u;
    p->data->add(tree);
    tick_frames(engine, 10);

    // The ray must START above the ground, and the demo's hillside descends eastward -- a fixed
    // offset from the trunk's own height begins underground and hits the heightfield instead.
    const glm::vec3 start_xy = tree.position + glm::vec3(-6.0f, 0.0f, 0.0f);
    const float start_ground = views.front()->field_view().height(start_xy);
    coopa::physx::geometry::Ray ray;
    ray.origin = glm::vec3(start_xy.x, start_xy.y, start_ground + 1.5f);
    ray.direction = glm::vec3(1.0f, 0.0f, 0.0f);
    ray.max_distance = 12.0f;
    coopa::physx::system::PhysicsSystem::RaycastHit hit;
    const bool found = physics->raycast(ray, hit);
    expect(found,
           "instance_painter: a horizontal ray at chest height hits the painted trunk -- this is "
           "what makes a painted forest something you walk into rather than through");
    if (found) {
        const std::string what = hit.object != nullptr ? hit.object->name() : std::string("<none>");
        expect(std::abs(hit.point.x - tree.position.x) < 1.5f,
               "instance_painter: ...at the trunk, not at something else (hit '" + what + "' at x=" +
                   std::to_string(hit.point.x) + ", the trunk is at x=" +
                   std::to_string(tree.position.x) + ")");
    }
}

/** @brief Disabling the painter stops it drawing and colliding, without touching its records. */
COOPA_TEST(disabling_a_painter_leaves_its_records_alone) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::instance::InstanceSetStore::instance().clear();
    toy::core::Engine engine(make_test_config(demo(), 160, 90, 160, 90));
    tick_frames(engine, 60);
    InstancePainter* p = painter_of(engine);
    auto* sys = system_of(engine);
    require(p != nullptr && sys != nullptr, "instance_painter: present");
    const std::size_t painted = p->data->size();

    p->enabled = false;
    tick_frames(engine, 5);
    toy::render::ParticleFrameState off;
    sys->collect_render(engine.scene(), glm::vec3(40.0f, 0.0f, 25.0f), {}, off);
    expect(off.meshes.empty(), "instance_painter: a disabled painter hands the renderer nothing");
    expect(painter_collider_count(engine) == 0, "instance_painter: ...and has no colliders");
    expect(p->data->size() == painted,
           "instance_painter: but its records are untouched -- `enabled` is a visibility switch, "
           "not a delete");

    p->enabled = true;
    tick_frames(engine, 20);
    toy::render::ParticleFrameState on;
    sys->collect_render(engine.scene(), glm::vec3(40.0f, 0.0f, 25.0f), {}, on);
    expect(!on.meshes.empty(), "instance_painter: re-enabling it brings the batches back");
}
