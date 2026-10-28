/**
 * @file bake_context.h
 * @brief `BakeContext` -- the per-bake information every node can reach, via EvalContext::user_data().
 *
 * The nodegraph library carries one opaque pointer from an evaluation request through to every
 * node, deliberately knowing nothing about what is on the other end. This is what this domain puts
 * there: the things a node may need that are neither an input socket nor a node setting.
 *
 * Everything here is a **snapshot taken on the main thread** before the bake started. In
 * particular `self` is an `ObjectRef` and not a `SceneObject*`, for the same reason every scene
 * binding is (see scene_input.h): the bake runs on a worker and `Scene` is main-thread-only, so a
 * node must not be able to reach the live scene even by accident.
 */

#ifndef TOYENGINE_PROCGRAPH_BAKE_CONTEXT_H
#define TOYENGINE_PROCGRAPH_BAKE_CONTEXT_H

#include <cstdint>
#include <string>
#include <unordered_map>

#include <toyengine/procgraph/proc_types.h>

namespace toy {
namespace procgraph {

/**
 * @struct BakeContext
 * @brief What a node can learn about the bake it is part of.
 */
struct BakeContext {
    /// @brief The component's `seed`. Every random node derives from this, so a bake is repeatable.
    uint32_t    seed = 0;
    /// @brief A snapshot of the object carrying the ProceduralGraph component.
    ObjectRef   self;
    /// @brief The graph asset's name, for a diagnostic that says which graph misbehaved.
    std::string graph_name;

    /**
     * @brief A COPY of the system's variable table, taken on the main thread with the bindings.
     *
     * A copy rather than a pointer, for the same reason every scene input is a snapshot: the bake
     * runs on a worker and `set_variable()` is called from the main thread, so sharing the live
     * table would be a race. The table is a handful of small Values, so copying it is cheaper
     * than the synchronisation would be.
     */
    std::unordered_map<std::string, coopa::nodegraph::Value> variables;

    /// @brief The variable named `name`, or an empty Value.
    const coopa::nodegraph::Value& variable(const std::string& name) const {
        static const coopa::nodegraph::Value nothing;
        auto it = variables.find(name);
        return it == variables.end() ? nothing : it->second;
    }

    /// @brief The context from an EvalContext, or nullptr when a node is evaluated bare.
    template <typename Ctx>
    static const BakeContext* from(const Ctx& ctx) {
        return static_cast<const BakeContext*>(ctx.user_data());
    }
};

/// @brief A shared default, so a node evaluated with no context still reads sane values.
inline const BakeContext& default_bake_context() {
    static const BakeContext none;
    return none;
}

/// @brief The bake context, or a shared default when a node runs outside a real bake.
template <typename Ctx>
const BakeContext& bake_context(const Ctx& ctx) {
    const BakeContext* found = BakeContext::from(ctx);
    return found ? *found : default_bake_context();
}

} // namespace procgraph
} // namespace toy

#endif // TOYENGINE_PROCGRAPH_BAKE_CONTEXT_H
