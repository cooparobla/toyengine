# libcoopa

**A header-only C++20 runtime library: jobs, scenes, assets, coroutines and gameplay basics.**

libcoopa is the shared base that the other coopa libraries build on. It has a work-stealing
job system, a scene graph with an ordered per-frame system pipeline, a typed asset manager
with async loading and hot reload, Unity-style coroutines and animation, and small gameplay
models such as input maps, inventories and health bars. It has no graphics or windowing code,
so it works in tools, servers and tests as well as games. It is developed on Linux and macOS.

The library is header-only. It depends on nothing outside the standard library except three
vendored headers in `includes/`: [glm](https://github.com/g-truc/glm),
[fkYAML](https://github.com/fktn-k/fkYAML) and
[parallel-hashmap](https://github.com/greg7mdp/parallel-hashmap). It does not depend on any
other coopa library. The others (for example the Vulkan renderer gfxcoopa and the UI toolkit
uicoopa) depend on it and plug into its extension points. The game engine toyengine is one
consumer.

## Features

### Concurrency
- **Job system** (`coopa/job/`). `JobEngine` runs jobs on worker threads with lock-free
  Chase-Lev work-stealing deques and three priority levels. Handles come from a fixed pool,
  so submitting a job does not allocate a handle on the heap. A job can depend on any number of other
  handles; dependents are released when their dependencies finish, without polling.
  `parallel_for()` splits a range into chunks. A worker that calls `wait_for()` keeps
  running other jobs, so jobs can wait on jobs without deadlocking.
- **Hazard-based scheduling.** `JobScheduler` works out dependencies for you from declared
  reads and writes (read-after-write, write-after-read, write-after-write).
- **Thread-safe containers.** `ParallelQueue`, `ParallelVector` and `ParallelMap` (over
  `phmap`).
- **Node graphs** (`coopa/nodegraph/`). A generic graph evaluator that knows no types of its own: a
  consumer registers its socket types and node types, and the module validates, compiles and
  evaluates the graph. Compilation flattens node groups, inserts implicit conversions, prunes
  whatever cannot reach a requested output and fuses runs of cheap nodes into chains; evaluation
  runs one job per chain over the job system, or inline on the calling thread when no `JobEngine`
  is given. Both paths produce identical results.

### Scenes
- **Scene graph** (`coopa/scene/`). `Scene` owns `SceneObject`s, which own `Component`s
  with `start()`, `update()` and `late_update()`. `TransformComponent` wraps a hierarchical
  transform with lazy world-matrix updates.
- **Ordered system pipeline.** `Scene::update()` and `late_update()` run a list of
  `ISceneSystem`s sorted by `UpdatePhase` (Behaviour, Routine, Animation, TransformResolve,
  LateBehaviour) or by any integer order. A system can spread its work over a `JobEngine` and
  queue structural changes in per-worker `SceneCommandBuffer`s.
- **SceneManager.** Runs several scenes at once, one job per scene, when a `JobEngine` is
  installed.
- **YAML scenes.** `SceneLoader` reads scene files. Every component type except `Transform`
  is parsed by a function you register with `register_component_parser()`. Objects and
  whole scenes can `inherit_from` other files (with `prefab:` as a shorthand), merge
  components by type, and `remove: true` entries.

### Assets and data
- **Asset manager** (`coopa/asset/`). Register a loader per type, then `load()` or
  `load_async()` by virtual path. Assets are cached and refcounted, and `AssetHandle`s stay
  valid across hot reloads. Loaders decode off-thread and finalize on the main thread.
  A reference that misses its exact path is found by type folder and file name
  (`materials/brick` finds `materials/metal/brick.yaml`), so assets can be re-filed.
  Hot reload checks file modification times. `create()` publishes runtime-built assets, and
  idle eviction frees unused ones.
- **YAML I/O** (`coopa/yaml/`, `coopa/collections/`). `read_text()` and `load_document()`
  read plain YAML, or an encoded format through a decoder you register by magic bytes and
  extension. The writer emits stable YAML with canonical key order and short flow-style
  collections, so files survive a load-save round trip unchanged. `YAMLMap` is a simple
  key-value wrapper for config files.

### Gameplay
- **Coroutines** (`coopa/routine/`). A `Routine` is a C++20 coroutine that yields
  `next_frame()`, `frames(n)`, `seconds(s)`, `seconds_realtime(s)`, `wait_until(pred)` or
  `wait_while(pred)`. It can also wait for a `JobHandle` (`wait_for`) or run a step on a
  worker thread (`on_worker`) and resume on the main thread. Routines can nest, and
  `RoutineScope` stops them when their owner is destroyed.
- **Animation** (`coopa/animation/`). An `Animator` component plays named states with
  crossfades. `AnimationClip`s hold keyframed tracks (linear and eased) and procedural tracks
  (`orbit`, `sine`, `spin`, `constant`, or your own). Any component can expose animatable
  properties through `AnimatedPropertyRegistry`.
- **Input** (`coopa/input/`). `Input` holds keyboard and mouse state (levels, press and
  release edges, held time, cursor deltas). `InputMap` binds named actions, axes and vectors,
  with modifier chords. A windowing library feeds it and implements `IInputBackend`.
- **Items** (`coopa/item/`). `ItemDatabase` (from C++ or YAML), `Inventory` with
  move, merge, swap and split rules and change signals, and a `Hotbar` over its slots.
- **Stats** (`coopa/stat/`). `Resource` is a clamped meter (health, stamina) with delayed
  regeneration and `on_changed` / `on_depleted` signals. `StatBlock` groups them by name.

### Utilities
- **Events** (`coopa/event/`). `Signal<Args...>` with RAII `Connection` and
  `ScopedConnection` tokens; slots can connect and disconnect during `emit()`. `EventBus` adds
  pub/sub by (object name, signal name), plus wildcard listeners by signal name.
- **Logging** (`coopa/debug/`). A thread-safe `Logger` with level, tag and microsecond
  timestamps, and queue-based collectors that sort logs from many threads before printing.
- **Helpers** (`coopa/util/`). Unique IDs, string functions, a 4x4 matrix type that
  converts to `glm::mat4`, transforms, frame timing and path resolution.

## Getting started

### 1. Get the code

```bash
git clone git@github.com:cooparobla/libcoopa.git
```

### 2. Add it to your CMake project

You need CMake 3.20+ and a C++20 compiler. Add the library as a subdirectory and link the
`coopa::lib` interface target. It sets up the include paths, the
`GLM_FORCE_DEPTH_ZERO_TO_ONE` and `GLM_FORCE_RADIANS` defines, and links `Threads::Threads`.

```cmake
add_subdirectory(libs/libcoopa)
target_link_libraries(my_app PRIVATE coopa::lib)
```

Two options add debugging checks. Both are off by default:

| Option | Effect |
|---|---|
| `COOPA_JOB_DIAGNOSTICS` | Per-type job counters and per-thread queue-depth snapshots on `JobEngine` |
| `COOPA_SCENE_THREAD_CHECKS` | Asserts that each `Scene` is never used from two threads at once |

### 3. Use it

This program squares a million floats on every core, then runs a scene with one component
that starts a coroutine:

```cpp
#include <coopa/job/engine.h>
#include <coopa/job/parallel_for.h>
#include <coopa/scene/scene.h>
#include <coopa/routine/routine_system.h>
#include <coopa/routine/yield.h>
#include <cstdio>
#include <memory>
#include <vector>

using namespace coopa;

class Greeter : public scene::Component {
public:
    std::string type_name() const override { return "Greeter"; }
    void start() override { routine::start_routine(*this, run()); }
private:
    routine::Routine run() {
        co_yield routine::seconds(1.0f);
        std::printf("one second later\n");
    }
};

int main() {
    job::JobEngine jobs;  // one worker per hardware thread

    std::vector<float> values(1'000'000, 2.0f);
    jobs.parallel_for_blocking(values.size(), 0, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) values[i] *= values[i];
    });

    scene::Scene world("Hello");
    world.set_job_engine(&jobs);
    world.add_system(std::make_unique<routine::RoutineSystem>(), scene::UpdatePhase::Routine);
    auto* obj = world.add_root_object(std::make_unique<scene::SceneObject>("greeter"));
    obj->add_component<Greeter>();

    world.start();
    for (int frame = 0; frame < 90; ++frame) {   // 1.5 s at 60 Hz
        world.update(1.0f / 60.0f);
        world.late_update(1.0f / 60.0f);
    }
}
```

Each module folder has a README with more examples. Start with
[scene](coopa/scene/README.md), [job](coopa/job/README.md) and
[asset](coopa/asset/README.md).

## Testing

The tests live in [tests/](tests/), one suite per system in `tests/<suite>_test.cpp`
(`job_engine`, `dependency_graph`, `signal`, `asset_manager`, `scene_loader`, `animator`,
`routine`, ...), with shared fixtures in `tests/support/`. They are written against the coopa
test framework ([coopa/testing/test.h](coopa/testing/test.h)) and need no window or GPU. The
`libcoopa` test executable is only built when libcoopa is the top-level CMake project.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target libcoopa -j
ctest --test-dir build -j8 --output-on-failure   # one ctest entry per suite (libcoopa_<suite>)
./build/libcoopa --suite signal                  # one suite; --list, -v and name filters also work
```

The test build always turns on `COOPA_SCENE_THREAD_CHECKS`. The job, deque, dependency-graph,
signal and routine suites include concurrency and lifetime regression tests; they are most
useful under ASan/TSan but assert their invariants (every job runs exactly once, nothing is
touched after destruction) unconditionally.

## Project layout

```
coopa/
├── job/          JobEngine, JobScheduler, handles, dependency graph, parallel_for
│   └── collections/  work-stealing deque, ParallelQueue/Vector/Map
├── nodegraph/    socket types, Graph, compile(), job-parallel evaluate(), diagnostics
├── scene/        Scene, SceneObject, Component, systems, SceneLoader, SceneManager
├── asset/        AssetManager, handles, loaders, search roots, name index
├── animation/    Animator, AnimationClip, curves, procedural tracks, AnimationSystem
├── routine/      Routine, yield instructions, RoutineRunner, RoutineSystem
├── input/        Input, InputMap, key vocabulary, backend interface
├── item/         ItemDatabase, Inventory, Hotbar
├── stat/         Resource, StatBlock
├── event/        Signal, EventBus
├── yaml/         document reading and decoder registry, YAML writer
├── collections/  YAMLMap
├── debug/        Logger, DebugManager, DebugBucket
└── util/         ids, strings, math, transforms, time, file paths
includes/         vendored glm, fkYAML, parallel_hashmap
configuration/    root_directory.h.in (generated into the build tree)
tests/            test suites (one per system) and shared fixtures
plans/            design notes
```

## Notes

- **Extension points.** libcoopa only knows about `Transform`. Other component types, asset
  types and animatable properties are registered at startup by the code that defines them,
  through `SceneLoader::register_component_parser()`, `AssetManager::register_loader()` and
  `AnimatedPropertyRegistry`.
- **Opt-in systems.** A new `Scene` only runs the Behaviour and LateBehaviour walks. Add
  `RoutineSystem`, `AnimationSystem` or `TransformSystem` with `add_system()` if you need them.
- **Threading rules.** A `Scene` must be used by one thread at a time; different scenes can
  run in parallel. `Signal` is not thread-safe. `JobEngine` cannot be copied or moved, so
  hold it in a `std::unique_ptr` if it lives in a container.
- **`parallel_for`** is declared in `job/engine.h` but defined in `job/parallel_for.h`.
  Include the second header when you call it.
- **Root paths.** CMake writes `root_directory.h` with `ROOT_DIR` set to the *top-level*
  project's source directory. `FileUtil` uses it to find files, and the environment variables
  `LOGL_ROOT_PATH` and `LOGL_PROJ_PATH` override it at run time.
- **YAML comments** are dropped on load (fkYAML does not keep them), so re-saving a file
  with the writer removes its comments.
- **Logging.** `JobEngine` logs its thread start-up and shutdown through `Logger`.

## Documentation

- Module READMEs: [job](coopa/job/README.md), [job collections](coopa/job/collections/README.md),
  [nodegraph](coopa/nodegraph/README.md),
  [scene](coopa/scene/README.md), [scene components](coopa/scene/components/README.md),
  [asset](coopa/asset/README.md), [animation](coopa/animation/README.md),
  [routine](coopa/routine/README.md), [input](coopa/input/README.md),
  [item](coopa/item/README.md), [stat](coopa/stat/README.md),
  [collections](coopa/collections/README.md), [debug](coopa/debug/README.md),
  [util](coopa/util/README.md).
- Every header has doc comments. `.coopadocs` configures `coopadocs build`, which generates
  an HTML API reference into `.docs/`.
