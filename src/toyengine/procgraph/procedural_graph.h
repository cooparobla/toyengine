/**
 * @file procedural_graph.h
 * @brief The `ProceduralGraph` component: a graph, a seed, and what its inputs are bound to.
 *
 * @code{.yaml}
 * - name: Railing
 *   components:
 *     ProceduralGraph:
 *       graph: graphs/railing          # assets/graphs/railing.yaml
 *       seed: 7
 *       bake: on_change                # on_load | on_change | manual
 *       refresh_on_asset_change: true  # re-bake when the graph file changes on disk
 *       inputs:
 *         post_spacing: 1.5            # a literal
 *         path:    { object: RoadSpline }      # a scene binding
 *         markers: { children_of: Spawns }
 *         season:  { var: world.season }
 * @endcode
 *
 * **There are four keys and no more.** There is deliberately no `collider:`, no `output:` and no
 * `emit:`: what a graph produces is decided by the nodes in it, because a graph may be doing
 * something that has nothing to do with meshes. A collider is what a Make Collider node emits; so
 * is a light, a rigidbody, or a game project's own component.
 *
 * `inputs:` is free-form and validated against the compiled graph's interface, so a graph's own
 * parameters become inspector parameters with no schema edit per graph.
 *
 * ### No runtime state lives here
 *
 * The editor destroys and recreates a component on every inspector keystroke, between a system's
 * `execute()` and the render hand-off. So this component holds only authored data; everything
 * derived -- the compiled graph, the published objects, the last input hash -- lives on
 * `ProceduralGraphSystem`, keyed by something that outlives the component. Caching any of it here
 * would mean losing a bake on every keypress at best, and a dangling pointer at worst.
 */

#ifndef TOYENGINE_PROCGRAPH_PROCEDURAL_GRAPH_H
#define TOYENGINE_PROCGRAPH_PROCEDURAL_GRAPH_H

#include <cstdint>
#include <string>
#include <unordered_map>

#include <coopa/scene/component.h>
#include <toyengine/procgraph/scene_input.h>

namespace toy {
namespace procgraph {

/// @brief When a graph re-bakes.
enum class BakeMode : uint8_t {
    OnLoad,   ///< Once, during scene warm-up. The default: generated geometry is usually static.
    OnChange, ///< Whenever a bound input's resolved value changes. Costs a per-frame re-resolve.
    Manual,   ///< Only when something calls ProceduralGraphSystem::rebake().
};

/// @brief Parses a `bake:` value. Returns false and leaves `out` alone when unrecognised.
bool parse_bake_mode(const std::string& name, BakeMode& out);
/// @brief The YAML spelling of a mode, for the editor's save path.
const char* bake_mode_name(BakeMode mode);

/**
 * @class ProceduralGraph
 * @brief Attach a graph to an object; its baked output becomes that object's children.
 */
class ProceduralGraph : public coopa::scene::Component {
public:
    std::string type_name() const override { return "ProceduralGraph"; }

    /// @brief The graph asset, without an extension: `graphs/railing`.
    std::string graph;
    /// @brief Seeds every random node, so a bake is repeatable. Change it for a different result.
    int32_t     seed = 0;
    BakeMode    bake = BakeMode::OnLoad;
    /// @brief Re-bake when the graph file's timestamp moves. The editor wants this; a game does not.
    bool        refresh_on_asset_change = false;

    /// @brief Keyed by the graph's interface input identifier.
    std::unordered_map<std::string, InputBinding> inputs;

    /// @brief A stable key for this component's derived state, surviving a component rebuild.
    std::string state_key() const;
};

/// @brief Registers the "ProceduralGraph" scene parser. Called once at startup.
void register_procgraph_components();

} // namespace procgraph
} // namespace toy

#endif // TOYENGINE_PROCGRAPH_PROCEDURAL_GRAPH_H
