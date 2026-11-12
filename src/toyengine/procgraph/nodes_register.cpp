/**
 * @file nodes_register.cpp
 * @brief One entry point that registers every category.
 */

#include <toyengine/procgraph/nodes.h>

namespace toy {
namespace procgraph {

void register_procgraph_nodes() {
    // Guarded rather than merely idempotent per-category: registering a node type REPLACES the
    // previous entry, which is harmless but pointless work on a second scene load.
    static bool done = [] {
        register_proc_types();   // Socket types must exist before any declare() runs.
        register_input_nodes();
        register_scene_nodes();
        register_primitive_nodes();
        register_geometry_nodes();
        register_utility_nodes();
        register_output_nodes();
        return true;
    }();
    (void)done;
}

} // namespace procgraph
} // namespace toy
