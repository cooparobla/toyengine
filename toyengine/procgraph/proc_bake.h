/**
 * @file proc_bake.h
 * @brief The pure bake: a compiled graph plus resolved inputs becomes a `BakeResult`.
 *
 * No Vulkan, no `coopa::scene::Scene`, no `AssetManager`. That is what lets a bake run on a worker
 * thread and be unit-tested without constructing a device -- the same split
 * `toyengine/destruct/fracture.h` and `toyengine/env/env_field.h` use, and the reason
 * `proc_publish` exists as a separate file for the half that does touch the engine.
 *
 * A graph's interface outputs are read in this order of preference:
 *
 * 1. An `ObjectSpec` output (from an Object Output node) becomes the baked tree directly.
 * 2. Failing that, a `Geometry` output is wrapped in a single object named after the graph.
 * 3. Failing that, any `Component` outputs are attached to a single empty object.
 *
 * Which means a graph that emits nothing but components -- lights at authored markers, say -- is
 * as valid as one that emits a mesh, and needs no special handling by the caller.
 */

#ifndef TOYENGINE_PROCGRAPH_PROC_BAKE_H
#define TOYENGINE_PROCGRAPH_PROC_BAKE_H

#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <coopa/nodegraph/compile.h>
#include <coopa/nodegraph/value.h>
#include <toyengine/procgraph/bake_context.h>
#include <toyengine/procgraph/bake_result.h>

namespace coopa {
namespace job {
class JobEngine;
}
}

namespace toy {
namespace procgraph {

/**
 * @struct BakeRequest
 * @brief Everything one bake needs. Every field is a snapshot; nothing here reaches the scene.
 */
struct BakeRequest {
    const coopa::nodegraph::CompiledGraph* graph = nullptr;
    /// @brief Positional on `graph->inputs()`, from `resolve_inputs()`.
    std::span<const coopa::nodegraph::Value> inputs;
    /// @brief Seed, the owner's snapshot, the graph's name.
    BakeContext            context;
    /// @brief The engine to spread the graph over, or nullptr to run it on the calling thread.
    coopa::job::JobEngine* jobs = nullptr;
    /// @brief Polled between chains and inside long nodes, so an abandoned bake stops promptly.
    const std::atomic<bool>* cancel = nullptr;
    /// @brief Folded into the result so the system can skip an unchanged republish.
    uint64_t               input_hash = 0;
};

/**
 * @brief Evaluates the graph and assembles its object tree.
 *
 * Never throws: a bad graph, a node that threw, and a cancelled run all come back as a
 * `BakeResult` whose `errors` explain it.
 */
BakeResult bake(const BakeRequest& request);

/**
 * @brief Turns one `ObjectSpec` into a `BakedObject`, recursively.
 *
 * Exposed for tests, which build an ObjectSpec directly rather than running a graph to get one.
 */
BakedObject object_from_spec(const ObjectSpec& spec, const std::string& fallback_name);

} // namespace procgraph
} // namespace toy

#endif // TOYENGINE_PROCGRAPH_PROC_BAKE_H
