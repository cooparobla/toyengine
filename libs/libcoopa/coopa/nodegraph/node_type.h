/**
 * @file node_type.h
 * @brief `NodeType` -- one kind of node, and the process-wide table of them.
 *
 * A node type is two functions and a cost hint. `declare` computes the node's sockets from its
 * settings (see declaration.h); `evaluate` reads inputs and writes outputs through an
 * `EvalContext` and is given nothing else. Both are plain function pointers rather than
 * `std::function`, because a node type is registered once and called millions of times, and
 * because it keeps `NodeType` trivially copyable.
 *
 * ### The cost hint, and why it is not optional
 *
 * Submitting a job costs a handle allocation, a deque push, possibly a condition-variable wake,
 * and a cache miss on the other end. A Math node costs a few nanoseconds. Scheduling one job per
 * node would therefore make a utility-heavy graph *slower* with threads than without, so the
 * compiler fuses runs of cheap nodes into a single job and `NodeCost` is how it knows which are
 * which.
 *
 * The hint is author-declared and therefore wrong sometimes: a node labelled `Cheap` that is
 * actually milliseconds will serialize the chain it is fused into. That is a performance bug, not
 * a correctness one, and the per-node timings in Diagnostics are there to find it.
 *
 * ### Registration
 *
 * Function-local-static table, write-at-startup and read-concurrently, keyed by name -- the same
 * shape as `SceneLoader::register_component_parser()` and `AnimatedPropertyRegistry`. A consumer
 * calls its own `register_*_nodes()` once before compiling any graph. Re-registering a name
 * replaces it, so a test can swap a node body out.
 */

#ifndef COOPA_NODEGRAPH_NODE_TYPE_H
#define COOPA_NODEGRAPH_NODE_TYPE_H

#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <coopa/nodegraph/declaration.h>
#include <coopa/nodegraph/eval_context.h>

namespace coopa {
namespace nodegraph {

/**
 * @enum NodeCost
 * @brief How expensive one evaluation is, which decides whether it gets its own job.
 */
enum class NodeCost : uint8_t {
    Trivial,  ///< Nanoseconds (Math, Combine XYZ). Always fused; never scheduled alone.
    Cheap,    ///< Microseconds. Fused into a run of cheap nodes.
    Heavy,    ///< Milliseconds. Gets its own job.
    Parallel, ///< Heavy, and parallelizes internally via EvalContext::jobs().
};

/// @brief Whether a cost is small enough to fuse with its neighbours.
inline bool is_fusable(NodeCost c) { return c == NodeCost::Trivial || c == NodeCost::Cheap; }

/// @brief Computes a node instance's sockets from its settings.
using DeclareFn = void (*)(DeclarationBuilder& builder, const PropertyBag& props);
/// @brief Reads declared inputs, writes declared outputs. Given nothing but the context.
using EvaluateFn = void (*)(EvalContext& ctx);

/**
 * @struct NodeType
 * @brief One registered kind of node.
 */
struct NodeType {
    std::string name;                    ///< The `type:` spelling in graph YAML. The registry key.
    std::string category;                ///< Groups the node in a palette ("Geometry", "Utility").
    std::string label;                   ///< Display name; falls back to `name`.
    std::string tooltip;
    NodeCost    cost = NodeCost::Cheap;
    DeclareFn   declare = nullptr;
    EvaluateFn  evaluate = nullptr;

    const std::string& display() const { return label.empty() ? name : label; }

    /// @brief Runs `declare` for a node carrying `props`. Empty when the type has no declare.
    NodeDeclaration declaration_for(const PropertyBag& props) const {
        NodeDeclaration out;
        if (declare != nullptr) {
            DeclarationBuilder builder(out);
            declare(builder, props);
        }
        return out;
    }
};

/**
 * @class NodeTypeRegistry
 * @brief The process-wide table of node types.
 */
class NodeTypeRegistry {
public:
    static NodeTypeRegistry& instance() {
        static NodeTypeRegistry registry;
        return registry;
    }

    /// @brief Registers (or replaces) a type. Call at startup, before compiling anything.
    void add(NodeType type) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        std::string key = type.name;
        types_[std::move(key)] = std::move(type);
    }

    /**
     * @brief The type registered under `name`, or nullptr.
     * @return A pointer that stays valid until the next `add()` for the same name. Compilation
     *         resolves every type up front and the registry is not written during evaluation, so
     *         a `CompiledGraph` holding these is safe for its lifetime.
     */
    const NodeType* find(std::string_view name) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = types_.find(std::string(name));
        return it == types_.end() ? nullptr : &it->second;
    }

    /// @brief Every type in `category`, for a UI palette. Sorted by name.
    std::vector<const NodeType*> by_category(std::string_view category) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::vector<const NodeType*> out;
        for (const auto& [name, type] : types_) {
            if (type.category == category) out.push_back(&type);
        }
        sort_by_name_(out);
        return out;
    }

    /// @brief Every registered type, sorted by name.
    std::vector<const NodeType*> all() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::vector<const NodeType*> out;
        out.reserve(types_.size());
        for (const auto& [name, type] : types_) out.push_back(&type);
        sort_by_name_(out);
        return out;
    }

    /// @brief Every distinct category, sorted.
    std::vector<std::string> categories() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::vector<std::string> out;
        for (const auto& [name, type] : types_) {
            bool seen = false;
            for (const auto& c : out) {
                if (c == type.category) { seen = true; break; }
            }
            if (!seen) out.push_back(type.category);
        }
        for (size_t i = 1; i < out.size(); ++i) {
            for (size_t j = i; j > 0 && out[j] < out[j - 1]; --j) std::swap(out[j], out[j - 1]);
        }
        return out;
    }

    size_t size() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return types_.size();
    }

    /// @brief Forgets every type. Tests only.
    void clear() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        types_.clear();
    }

private:
    NodeTypeRegistry() = default;

    static void sort_by_name_(std::vector<const NodeType*>& v) {
        for (size_t i = 1; i < v.size(); ++i) {
            for (size_t j = i; j > 0 && v[j]->name < v[j - 1]->name; --j) std::swap(v[j], v[j - 1]);
        }
    }

    mutable std::shared_mutex                     mutex_;
    std::unordered_map<std::string, NodeType>     types_;
};

/// @brief Convenience for a consumer's registration function.
inline void register_node_type(NodeType type) { NodeTypeRegistry::instance().add(std::move(type)); }

} // namespace nodegraph
} // namespace coopa

#endif // COOPA_NODEGRAPH_NODE_TYPE_H
