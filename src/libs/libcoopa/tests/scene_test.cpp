/**
 * @file scene_test.cpp
 * @brief coopa::scene::Scene: the ordered ISceneSystem pipeline behind update()/late_update()
 *        for every calling shape consumers use, system add/remove/move, the per-worker
 *        SceneCommandBuffers, and object lookup (find_child / find_object_by_path).
 *
 * Shapes covered: update() only (blendy/toyengine), late_update() standalone (several uicoopa
 * tests), and both together. Multi-scene processing is in scene_manager_test.cpp; the
 * TransformSystem in transform_test.cpp.
 */
#include <coopa/testing/test.h>

#include "support/scene_fixtures.h"

#include <coopa/job/engine.h>
#include <coopa/job/handle.h>
#include <coopa/job/parallel_for.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_commands.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/scene_system.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

COOPA_TEST_SUITE("scene");

using libcoopa_test::CountingComponent;
using libcoopa_test::RecordingSystem;

namespace {

/** @brief Records whether FrameContext::jobs was null the last time it executed. */
class JobsNullCheckSystem : public coopa::scene::ISceneSystem {
public:
    explicit JobsNullCheckSystem(bool* jobs_was_null) : jobs_was_null_(jobs_was_null) {}
    void execute(coopa::scene::Scene&, const coopa::scene::FrameContext& ctx) override {
        *jobs_was_null_ = (ctx.jobs == nullptr);
    }
    const char* system_name() const override { return "JobsNullCheck"; }

private:
    bool* jobs_was_null_;
};

/**
 * @brief Every execute() call creates a handle, submits one trivial job onto
 *        it, waits for it, and closes it -- exercising the exact JobEngine
 *        usage pattern AnimationSystem follows, without ever calling
 *        begin_frame()/end_frame() itself (that boundary is diagnostics-only
 *        now and optional -- see coopa/job/handle.h).
 */
class JobUsingSystem : public coopa::scene::ISceneSystem {
public:
    explicit JobUsingSystem(bool* all_valid) : all_valid_(all_valid) {}
    void execute(coopa::scene::Scene&, const coopa::scene::FrameContext& ctx) override {
        if (!ctx.jobs) { *all_valid_ = false; return; }
        coopa::job::JobHandle h = ctx.jobs->create_handle();
        if (!h.is_valid()) { *all_valid_ = false; return; }
        ctx.jobs->submit([] {}, 0, h);
        ctx.jobs->wait_for(h);
        h.close();
    }
    const char* system_name() const override { return "JobUsing"; }

private:
    bool* all_valid_;
};

/** @brief Increments on_attach()/on_detach() counters, for verifying add_system()/remove_system(). */
class AttachDetachSystem : public coopa::scene::ISceneSystem {
public:
    AttachDetachSystem(int* attach_count, int* detach_count)
        : attach_count_(attach_count), detach_count_(detach_count) {}
    void on_attach(coopa::scene::Scene&) override { ++(*attach_count_); }
    void on_detach(coopa::scene::Scene&) override { ++(*detach_count_); }
    void execute(coopa::scene::Scene&, const coopa::scene::FrameContext&) override {}
    const char* system_name() const override { return "AttachDetach"; }

private:
    int* attach_count_;
    int* detach_count_;
};

/// @brief Fans out one chunk per JobEngine worker, each recording an add_root_object()
/// into its OWN SceneCommandBuffer -- exercises FrameContext::commands/worker_index.
class SpawnPerWorkerSystem : public coopa::scene::ISceneSystem {
public:
    const char* system_name() const override { return "SpawnPerWorker"; }
    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override {
        if (!ctx.jobs || ctx.jobs->worker_count() == 0) return;
        ctx.jobs->parallel_for_blocking(ctx.jobs->worker_count(), 1,
            [&scene](size_t start, size_t end, const coopa::job::JobContext& jctx) {
                for (size_t i = start; i < end; ++i) {
                    auto obj = std::make_unique<coopa::scene::SceneObject>(
                        "FromWorker" + std::to_string(jctx.worker_index));
                    scene.commands_for(jctx.worker_index).add_root_object(std::move(obj));
                }
            });
    }
};

} // namespace

COOPA_TEST(systems_run_in_ascending_phase_order_and_ties_keep_insertion_order) {
    coopa::scene::Scene scene("PhaseOrder");
    std::vector<std::string> log;
    scene.add_system(std::make_unique<RecordingSystem>("mid", &log), 250);
    scene.add_system(std::make_unique<RecordingSystem>("early", &log), 150);
    scene.add_system(std::make_unique<RecordingSystem>("late", &log), 350);
    scene.update(0.016f); // all three are < LateBehaviour(400), so all run here, in ascending order.
    ASSERT_EQ(log.size(), 3u);
    EXPECT_EQ(log[0], std::string("early"));
    EXPECT_EQ(log[1], std::string("mid"));
    EXPECT_EQ(log[2], std::string("late"));

    coopa::scene::Scene scene2("EqualOrder");
    std::vector<std::string> log2;
    scene2.add_system(std::make_unique<RecordingSystem>("first", &log2), 250);
    scene2.add_system(std::make_unique<RecordingSystem>("second", &log2), 250);
    scene2.update(0.016f);
    ASSERT_EQ(log2.size(), 2u);
    EXPECT_EQ(log2[0], std::string("first"));
    EXPECT_EQ(log2[1], std::string("second"));
}

COOPA_TEST(update_runs_phases_below_late_behaviour_and_late_update_the_rest) {
    coopa::scene::Scene scene("Split");
    std::vector<std::string> log;
    scene.add_system(std::make_unique<RecordingSystem>("before_late", &log), 350); // < LateBehaviour(400)
    scene.add_system(std::make_unique<RecordingSystem>("after_late", &log), 450);  // >= LateBehaviour(400)

    scene.update(0.016f);
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0], std::string("before_late"));

    scene.late_update(0.016f);
    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log[1], std::string("after_late"));

    // late_update() standalone, with no preceding update() this frame.
    coopa::scene::Scene standalone("StandaloneLate");
    std::vector<std::string> log2;
    standalone.add_system(std::make_unique<RecordingSystem>("late_only", &log2), 450);
    standalone.late_update(0.016f);
    ASSERT_EQ(log2.size(), 1u);
    EXPECT_EQ(log2[0], std::string("late_only"));
    EXPECT_TRUE(standalone.job_engine() == nullptr); // never touched, since none was ever installed
}

COOPA_TEST(systems_see_null_jobs_when_no_engine_is_installed) {
    coopa::scene::Scene scene("NoEngine");
    bool jobs_was_null = false;
    scene.add_system(std::make_unique<JobsNullCheckSystem>(&jobs_was_null), 250);
    scene.update(0.016f);
    EXPECT_TRUE(jobs_was_null);
}

COOPA_TEST(update_only_callers_never_exhaust_the_handle_pool) {
    // Scene never touches the engine's frame boundary, including for a caller
    // that only ever calls update() and never late_update() (the
    // blendy/toyengine shape). Handles are reclaimed individually (see
    // coopa/job/handle.h), so this runs far more iterations than the tiny
    // handle pool capacity (4) could ever hold at once, relying entirely on
    // JobUsingSystem's own handle.close() each call to keep the pool from
    // exhausting.
    coopa::job::JobEngine engine(2, 64, 4);
    coopa::scene::Scene scene("FrameBoundaryUpdateOnly");
    scene.set_job_engine(&engine);
    bool all_valid = true;
    scene.add_system(std::make_unique<JobUsingSystem>(&all_valid), 250);

    for (int i = 0; i < 200; ++i) scene.update(0.016f);
    EXPECT_TRUE(all_valid);
    engine.shutdown();
}

COOPA_TEST(add_and_remove_system_attach_detach_and_behaviour_is_removable) {
    coopa::scene::Scene scene("AddRemove");
    int attach = 0, detach = 0;
    scene.add_system(std::make_unique<AttachDetachSystem>(&attach, &detach), 250);
    EXPECT_EQ(attach, 1);
    EXPECT_EQ(detach, 0);
    EXPECT_TRUE(scene.remove_system("AttachDetach"));
    EXPECT_EQ(detach, 1);
    EXPECT_TRUE(scene.find_system("AttachDetach") == nullptr);
    EXPECT_FALSE(scene.remove_system("AttachDetach")); // already removed

    auto obj = std::make_unique<coopa::scene::SceneObject>("Obj");
    auto* counter = obj->add_component<CountingComponent>();
    scene.add_root_object(std::move(obj));
    scene.update(0.016f);
    EXPECT_EQ(counter->update_count, 1);

    EXPECT_TRUE(scene.remove_system("Behaviour"));
    scene.update(0.016f);
    EXPECT_EQ(counter->update_count, 1); // removing BehaviourSystem stops the Component::update() walk
}

COOPA_TEST(moved_scene_keeps_its_systems_and_engine) {
    coopa::job::JobEngine engine(2);
    std::vector<std::string> log;
    coopa::scene::Scene scene("MoveSrc");
    scene.set_job_engine(&engine);
    scene.add_system(std::make_unique<RecordingSystem>("sys", &log), 250);

    coopa::scene::Scene moved(std::move(scene));
    EXPECT_TRUE(moved.job_engine() == &engine);
    moved.update(0.016f);
    EXPECT_EQ(log.size(), 1u);
    moved.late_update(0.016f);
    engine.shutdown();
}

COOPA_TEST(command_buffers_flush_in_ascending_worker_index_order) {
    // Deterministic: records directly into each worker's buffer (any thread
    // may call commands_for(i) -- only the CONVENTION is that worker i
    // writes its own buffer, nothing stops a test setting them up directly)
    // in REVERSE index order, to prove flush_commands() applies them in
    // ascending buffer-index order rather than recording order.
    coopa::job::JobEngine engine(4);
    coopa::scene::Scene scene("CommandBufferOrder");
    scene.set_job_engine(&engine); // sizes the per-worker buffers

    for (uint32_t i = engine.worker_count(); i-- > 0;) {
        auto obj = std::make_unique<coopa::scene::SceneObject>("FromWorker" + std::to_string(i));
        scene.commands_for(i).add_root_object(std::move(obj));
    }

    const size_t before = scene.root_objects().size();
    scene.flush_commands();
    ASSERT_EQ(scene.root_objects().size() - before, static_cast<size_t>(engine.worker_count()));
    for (uint32_t i = 0; i < engine.worker_count(); ++i) {
        EXPECT_EQ(scene.root_objects()[before + i]->name(), std::string("FromWorker") + std::to_string(i));
    }
    engine.shutdown();
}

COOPA_TEST(parallel_command_buffer_writes_all_land_exactly_once) {
    // Realistic usage: a system fans out via parallel_for(), each chunk
    // writing into its OWN buffer via commands_for(jctx.worker_index). Which
    // physical worker ends up running a given chunk is NOT guaranteed
    // one-to-one with the chunk's logical index (work-stealing may let one
    // fast worker grab more than one chunk) -- so this only asserts the
    // aggregate invariant that matters: every recorded command lands exactly
    // once, with none lost or duplicated, regardless of that mapping.
    coopa::job::JobEngine engine(4);
    coopa::scene::Scene scene("CommandBufferParallel");
    scene.set_job_engine(&engine);
    scene.add_system(std::make_unique<SpawnPerWorkerSystem>(), 250);

    const size_t before = scene.root_objects().size();
    scene.update(0.016f);
    scene.late_update(0.016f); // flush_commands() runs here

    size_t spawned_count = 0;
    for (auto& root : scene.root_objects()) {
        if (root->name().rfind("FromWorker", 0) == 0) ++spawned_count;
    }
    EXPECT_EQ(spawned_count, static_cast<size_t>(engine.worker_count()));
    EXPECT_EQ(scene.root_objects().size(), before + spawned_count);

    engine.shutdown();
}

COOPA_TEST(find_child_is_direct_only_and_find_descendant_recurses) {
    using coopa::scene::SceneObject;
    auto root  = std::make_unique<SceneObject>("Root");
    auto child = std::make_unique<SceneObject>("Child");
    child->add_child(std::make_unique<SceneObject>("Grandchild"));
    root->add_child(std::move(child));

    EXPECT_TRUE(root->find_child("Child") != nullptr);
    EXPECT_TRUE(root->find_child("Grandchild") == nullptr); // non-recursive
    EXPECT_TRUE(root->find_descendant("Grandchild") != nullptr);
    EXPECT_TRUE(root->find_child("Nope") == nullptr);
}

COOPA_TEST(find_object_by_path_walks_direct_children_and_fails_closed) {
    using coopa::scene::SceneObject;
    coopa::scene::Scene scene("Path");
    auto root = std::make_unique<SceneObject>("sdf_blob");
    auto mid  = std::make_unique<SceneObject>("sdf_blob_sphere");
    mid->add_child(std::make_unique<SceneObject>("inner"));
    root->add_child(std::move(mid));
    scene.add_root_object(std::move(root));

    // Single segment is exactly find_object().
    EXPECT_TRUE(scene.find_object_by_path("sdf_blob") == scene.find_object("sdf_blob"));

    // Two- and three-deep paths resolve through direct children.
    SceneObject* two_deep = scene.find_object_by_path("sdf_blob:sdf_blob_sphere");
    EXPECT_TRUE(two_deep != nullptr && two_deep == scene.find_object("sdf_blob_sphere"));
    SceneObject* three_deep = scene.find_object_by_path("sdf_blob:sdf_blob_sphere:inner");
    EXPECT_TRUE(three_deep != nullptr && three_deep == scene.find_object("inner"));

    // "inner" is only a grandchild of sdf_blob, not a direct child -- skipping the middle
    // segment must fail rather than falling back to a recursive search.
    EXPECT_TRUE(scene.find_object_by_path("sdf_blob:inner") == nullptr);

    // Malformed paths and unknown segments fail closed rather than crashing.
    for (const char* bad : {"", ":", "sdf_blob:", ":sdf_blob", "sdf_blob::inner", "nope", "sdf_blob:nope"}) {
        coopa::test::expect(scene.find_object_by_path(bad) == nullptr,
                            std::string("malformed/unknown path '") + bad + "' resolves to nothing");
    }
}
