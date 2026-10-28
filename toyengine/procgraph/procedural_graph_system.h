/**
 * @file procedural_graph_system.h
 * @brief `ProceduralGraphSystem` -- order **63**: resolve, bake, publish.
 *
 * ### The frame is three steps, and the split between them is the thread-safety contract
 *
 * 1. **Resolve** (main thread) -- every binding becomes an immutable `Value`. This is the one
 *    place the scene is read.
 * 2. **Bake** (worker, `Priority::Low`) -- the graph is evaluated over the job engine. It reads
 *    nothing but the snapshot, so it cannot race the scene.
 * 3. **Publish** (main thread) -- finished bakes become real objects.
 *
 * A re-bake therefore never hitches a frame, and `cpu.scene.procgraph` in the steady state is the
 * cost of a few transform reads for `on_change` graphs and nothing at all for `on_load` ones.
 *
 * ### Order 63, and what that costs
 *
 * The first free slot after the terrain systems (60, 61) and the instance painter (62): after
 * terrain so a graph could sample ground height, and before Physics (100) so a baked collider is
 * stepped the same frame it appears. It is *before* `TransformResolve` (350), so a binding reads
 * the owner's world matrix as the previous frame resolved it -- identical for a static authored
 * object, one frame stale for one animated this frame. That is the same contract every system
 * below 350 lives with.
 *
 * `on_load` graphs bake during **warm-up** rather than during `execute()`, so the loading screen
 * holds until they are done (`warm_up_state()`, as `TerrainSystem` and `WaterSystem` use). That is
 * also why being ordered after the instance painter costs nothing: a load-time bake is complete
 * before the first steady-state frame.
 *
 * ### Derived state does not live on the component
 *
 * The editor destroys and recreates a component on every inspector keystroke, so everything the
 * system remembers is keyed by `ProceduralGraph::state_key()` -- the owner's name plus the graph
 * path -- rather than by a component pointer.
 */

#ifndef TOYENGINE_PROCGRAPH_PROCEDURAL_GRAPH_SYSTEM_H
#define TOYENGINE_PROCGRAPH_PROCEDURAL_GRAPH_SYSTEM_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <coopa/job/handle.h>
#include <coopa/nodegraph/compile.h>
#include <coopa/nodegraph/graph.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_system.h>

#include <toyengine/procgraph/bake_result.h>
#include <toyengine/procgraph/proc_publish.h>
#include <toyengine/procgraph/procedural_graph.h>
#include <toyengine/procgraph/scene_input.h>

namespace coopa {
namespace asset {
class AssetManager;
}
namespace gfx {
namespace core {
class Device;
}
namespace memory {
class Allocator;
}
}
}

namespace toy {
namespace procgraph {

/// @brief Order 63. See the @file doc for why.
inline constexpr int k_procgraph_system_order = 63;

/**
 * @class ProceduralGraphSystem
 * @brief Bakes every `ProceduralGraph` in the scene and publishes the result.
 */
class ProceduralGraphSystem : public coopa::scene::ISceneSystem {
public:
    ProceduralGraphSystem(coopa::asset::AssetManager* assets,
                          coopa::gfx::core::Device* device,
                          coopa::gfx::memory::Allocator* allocator)
        : assets_(assets), device_(device), allocator_(allocator) {}

    const char* system_name() const override { return "ProceduralGraph"; }
    /// @brief True, so a dragged parameter -- or a moved input object -- re-bakes live.
    bool runs_in_edit_mode() const override { return true; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override;
    void on_detach(coopa::scene::Scene& scene) override;

    /// @brief Holds the loading screen until every `bake: on_load` graph has baked.
    coopa::scene::WarmupState warm_up_state() const override;

    /// @brief Queues a re-bake of one component, whatever its bake mode. The editor calls this.
    void rebake(const ProceduralGraph& component);
    /// @brief Queues a re-bake of every graph in the scene.
    void rebake_all();

    /// @brief Sets a variable every `{ var: name }` binding can read. Main thread.
    void set_variable(std::string name, coopa::nodegraph::Value value);
    const coopa::nodegraph::Value& variable(const std::string& name) const {
        return variables_.get(name);
    }
    /// @brief Reads a `variables:` mapping, as `scene.settings.procgraph.variables` holds.
    void load_variables(const fkyaml::node& node);

    /// @brief Where the graph assets live, so `graphs/railing` resolves. Set by the installer.
    void set_asset_roots(std::vector<std::string> roots) { asset_roots_ = std::move(roots); }
    /// @brief The owning scene's file path, for relative asset paths inside emitted components.
    void set_scene_path(std::string path) { scene_path_ = std::move(path); }

    /// @brief The messages from one component's last bake, for an inspector panel.
    const BakeResult* last_result(const ProceduralGraph& component) const;

    /// @brief How many graphs baked since the system was installed. For a test.
    uint64_t bake_count() const { return bake_count_; }

private:
    /// @brief Everything derived for one component, keyed by its state_key().
    struct Entry {
        std::shared_ptr<coopa::nodegraph::Graph>         source;
        std::shared_ptr<coopa::nodegraph::CompiledGraph> compiled;
        PublishedGraph published;
        BakeResult     last_result;
        uint64_t       input_hash = 0;
        int64_t        asset_mtime = 0;
        bool           baked = false;
        bool           force = false;      ///< A rebake() request, regardless of the hash.
        bool           failed_compile = false;
    };

    Entry& entry_for_(const std::string& key) { return entries_[key]; }

    /// @brief Loads and compiles a graph, caching by asset path. Returns null on failure.
    std::shared_ptr<coopa::nodegraph::CompiledGraph> compile_graph_(
        const std::string& graph_path, Entry& entry, BakeResult& out);

    /// @brief The resolved path of a graph asset, or empty when it cannot be found.
    std::string resolve_graph_path_(const std::string& graph) const;

    /// @brief Resolve, bake and publish one component. Main thread.
    void process_(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx,
                  ProceduralGraph& component);

    coopa::asset::AssetManager*    assets_ = nullptr;
    coopa::gfx::core::Device*      device_ = nullptr;
    coopa::gfx::memory::Allocator* allocator_ = nullptr;
    std::vector<std::string>       asset_roots_;
    std::string                    scene_path_;

    VariableTable                            variables_;
    std::unordered_map<std::string, Entry>   entries_;
    /// @brief Compiled graphs shared by every component referencing the same asset.
    std::unordered_map<std::string, std::shared_ptr<coopa::nodegraph::CompiledGraph>> compiled_cache_;
    std::unordered_map<std::string, std::shared_ptr<coopa::nodegraph::Graph>>         source_cache_;

    uint64_t bake_count_ = 0;
    /// @brief Set when a variable changes, so `on_change` graphs re-resolve even if nothing moved.
    bool     variables_dirty_ = false;
    /// @brief Graphs still owing a first bake, so warm_up_state() can report progress.
    mutable int pending_warmup_ = 0;
};

/// @brief The installed system, or nullptr.
inline ProceduralGraphSystem* find_procgraph(const coopa::scene::Scene& scene) {
    return dynamic_cast<ProceduralGraphSystem*>(scene.find_system("ProceduralGraph"));
}

/**
 * @brief Installs the system, or returns the one already there.
 * @param settings The scene's `settings.procgraph` block, for `variables:`.
 */
ProceduralGraphSystem* install_procgraph_system(coopa::scene::Scene& scene,
                                                coopa::asset::AssetManager* assets,
                                                coopa::gfx::core::Device* device,
                                                coopa::gfx::memory::Allocator* allocator,
                                                const fkyaml::node& settings,
                                                int order = k_procgraph_system_order);

} // namespace procgraph
} // namespace toy

#endif // TOYENGINE_PROCGRAPH_PROCEDURAL_GRAPH_SYSTEM_H
