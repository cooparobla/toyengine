/**
 * @file nodes.h
 * @brief The node library: one registration function, and the helpers its bodies share.
 *
 * Node types are registered once at startup, grouped by category into one translation unit each
 * (`nodes_input.cpp`, `nodes_scene.cpp`, ...), so adding a node touches one small file. Geometry is
 * the biggest category but it is one among several -- a graph that emits only components and no
 * geometry at all is an equally valid use of this system.
 *
 * `NodeType::declare` and `NodeType::evaluate` are plain function pointers rather than
 * `std::function`, so every body here is a non-capturing lambda or a free function. That keeps a
 * `NodeType` trivially copyable and keeps the per-evaluation cost to one indirect call.
 */

#ifndef TOYENGINE_PROCGRAPH_NODES_H
#define TOYENGINE_PROCGRAPH_NODES_H

#include <cstdint>
#include <string>

#include <coopa/nodegraph/node_type.h>
#include <toyengine/procgraph/bake_context.h>
#include <toyengine/procgraph/proc_types.h>

namespace toy {
namespace procgraph {

using coopa::nodegraph::DeclarationBuilder;
using coopa::nodegraph::EvalContext;
using coopa::nodegraph::NodeCost;
using coopa::nodegraph::NodeType;
using coopa::nodegraph::PropertyBag;

/// @brief Registers every node type this engine ships. Idempotent; call once at startup.
void register_procgraph_nodes();

// Each category's own registration, called by the above. Separate so a test can register one.
void register_input_nodes();
void register_scene_nodes();
void register_primitive_nodes();
void register_geometry_nodes();
void register_utility_nodes();
void register_output_nodes();

namespace detail {

/// @brief Reads a Geometry input, yielding an empty set rather than failing when unconnected.
inline GeometrySet geometry_in(EvalContext& ctx, const char* socket) {
    return ctx.input_value(socket).as<GeometrySet>(empty_geometry());
}

/// @brief Reads an ObjectRef input.
inline ObjectRef object_in(EvalContext& ctx, const char* socket) {
    static const ObjectRef none;
    return ctx.input_value(socket).as<ObjectRef>(none);
}

/// @brief Reads an ObjectSet input.
inline ObjectSet object_set_in(EvalContext& ctx, const char* socket) {
    static const ObjectSet none;
    return ctx.input_value(socket).as<ObjectSet>(none);
}

/// @brief Reads a String input.
inline std::string string_in(EvalContext& ctx, const char* socket) {
    static const std::string none;
    return ctx.input_value(socket).as<std::string>(none);
}

/**
 * @brief A stable per-element random value in [0, 1).
 *
 * Seeded from the bake's own seed plus an element index, never from a global or from time, so a
 * bake is byte-identical across runs and between the inline and parallel evaluators. This is the
 * same lowbias32 hash `toyengine/particles/particle_math.h` uses, so a graph and the engine's
 * own scatter agree on what "seed 7" means.
 */
inline uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

inline float hash01(uint32_t seed, uint32_t index) {
    return static_cast<float>(hash_u32(seed ^ hash_u32(index + 0x9e3779b9U))) /
           static_cast<float>(0xffffffffU);
}

} // namespace detail

} // namespace procgraph
} // namespace toy

#endif // TOYENGINE_PROCGRAPH_NODES_H
