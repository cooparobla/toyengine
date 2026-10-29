/**
 * @file procedural_graph_system.cpp
 * @brief The resolve -> bake -> publish loop, and graph asset loading.
 */

#include <toyengine/procgraph/procedural_graph_system.h>

#include <filesystem>
#include <iostream>
#include <set>
#include <utility>

#include <coopa/asset/asset_manager.h>
#include <coopa/nodegraph/graph_yaml.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

#include <toyengine/procgraph/nodes.h>
#include <toyengine/procgraph/proc_bake.h>

namespace toy {
namespace procgraph {
namespace {

using coopa::nodegraph::CompiledGraph;
using coopa::nodegraph::Diagnostics;
using coopa::nodegraph::Graph;
using coopa::nodegraph::Value;

/// @brief One line per distinct message, so a broken graph cannot spam every frame.
void warn_once_(const std::string& line) {
    static std::set<std::string> seen;
    if (!seen.insert(line).second) return;
    std::cerr << line << "\n";
}

int64_t mtime_of_(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<int64_t>(t.time_since_epoch().count());
}

} // namespace

std::string ProceduralGraphSystem::resolve_graph_path_(const std::string& graph) const {
    if (graph.empty()) return {};
    // An explicit extension is taken as given; otherwise `.yaml` is appended. Both forms are
    // tried under every asset root, and `resolve_variant` lets a packaged `.caml` stand in.
    std::vector<std::string> candidates;
    if (graph.find('.') != std::string::npos) {
        candidates.push_back(graph);
    } else {
        candidates.push_back(graph + ".yaml");
    }

    for (const std::string& candidate : candidates) {
        if (std::filesystem::exists(candidate)) return candidate;
        for (const std::string& root : asset_roots_) {
            const std::string joined = (std::filesystem::path(root) / candidate).string();
            if (std::filesystem::exists(joined)) return joined;
            const std::string variant = coopa::yaml::resolve_variant(joined);
            if (!variant.empty() && std::filesystem::exists(variant)) return variant;
        }
    }
    return {};
}

std::shared_ptr<CompiledGraph> ProceduralGraphSystem::compile_graph_(
    const std::string& graph_path, Entry& entry, BakeResult& out) {
    const std::string resolved = resolve_graph_path_(graph_path);
    if (resolved.empty()) {
        out.errors.push_back("graph asset '" + graph_path + "' was not found under any asset root");
        return nullptr;
    }

    const int64_t mtime = mtime_of_(resolved);
    // The cache is keyed by resolved path AND timestamp, so an edited graph recompiles while an
    // untouched one is shared by every component referencing it.
    const std::string cache_key = resolved + "@" + std::to_string(mtime);
    auto cached = compiled_cache_.find(cache_key);
    if (cached != compiled_cache_.end()) {
        entry.asset_mtime = mtime;
        entry.source      = source_cache_[cache_key];
        return cached->second;
    }

    Diagnostics diag;
    auto source = std::make_shared<Graph>(coopa::nodegraph::load_graph(resolved, diag));
    if (!diag.ok()) {
        for (const auto& e : diag.entries()) out.errors.push_back(e.message);
        return nullptr;
    }

    Diagnostics compile_diag;
    // Groups resolve through the same path lookup, so `group: graphs/post` works inside a graph.
    auto loader = [this](const std::string& ref) -> const Graph* {
        const std::string path = resolve_graph_path_(ref);
        if (path.empty()) return nullptr;
        const int64_t m = mtime_of_(path);
        const std::string key = path + "@" + std::to_string(m);
        auto it = source_cache_.find(key);
        if (it != source_cache_.end()) return it->second.get();
        Diagnostics sub;
        auto loaded = std::make_shared<Graph>(coopa::nodegraph::load_graph(path, sub));
        if (!sub.ok()) return nullptr;
        source_cache_[key] = loaded;
        return loaded.get();
    };

    auto compiled = std::make_shared<CompiledGraph>(
        coopa::nodegraph::compile(*source, loader, {}, compile_diag));
    for (const auto& e : compile_diag.entries()) {
        std::string line = e.message;
        if (!e.node_label.empty()) {
            line = e.node_label + " (#" + std::to_string(e.node) + "): " + line;
        }
        if (e.severity == coopa::nodegraph::Severity::Error) out.errors.push_back(line);
        else out.warnings.push_back(line);
    }
    if (!compile_diag.ok()) return nullptr;

    entry.asset_mtime         = mtime;
    entry.source              = source;
    source_cache_[cache_key]  = source;
    compiled_cache_[cache_key] = compiled;
    return compiled;
}

void ProceduralGraphSystem::process_(coopa::scene::Scene& scene,
                                     const coopa::scene::FrameContext& ctx,
                                     ProceduralGraph& component) {
    const std::string key = component.state_key();
    Entry& entry = entry_for_(key);

    const bool first_bake = !entry.baked;
    const bool forced     = entry.force;

    // Manual graphs bake once (so the scene is not empty on load) and then only on request.
    if (!first_bake && !forced && component.bake == BakeMode::Manual) return;

    // A compile failure is remembered so a broken graph is not recompiled every single frame; a
    // forced rebake or a changed file clears it.
    if (entry.failed_compile && !forced) {
        if (!component.refresh_on_asset_change) return;
        const std::string resolved = resolve_graph_path_(component.graph);
        if (resolved.empty() || mtime_of_(resolved) == entry.asset_mtime) return;
        entry.failed_compile = false;
    }

    BakeResult pending;
    pending.root.name = component.graph.empty() ? "Generated" : component.graph;

    if (component.refresh_on_asset_change && entry.baked && !forced) {
        const std::string resolved = resolve_graph_path_(component.graph);
        if (!resolved.empty() && mtime_of_(resolved) != entry.asset_mtime) {
            entry.compiled.reset();   // The file moved: recompile before re-baking.
        }
    }

    if (!entry.compiled) {
        entry.compiled = compile_graph_(component.graph, entry, pending);
        if (!entry.compiled) {
            entry.failed_compile = true;
            entry.baked          = true;   // Do not retry every frame.
            entry.last_result    = std::move(pending);
            warn_once_("[ProceduralGraph] " + key + ": " + entry.last_result.messages());
            return;
        }
    }

    // --- Step 1: resolve, on the main thread. The one place the scene is read. ---
    ResolveContext resolve;
    resolve.scene     = &scene;
    resolve.owner     = component.owner;
    resolve.variables = &variables_;
    resolve.assets    = assets_;

    Diagnostics resolve_diag;
    resolve.out = &resolve_diag;

    // A graph bound to an object inside its own output would dirty itself every bake and never
    // settle, so it is refused as a diagnostic rather than looped on.
    //
    // `live_root()`, never `published.root`: the editor destroys and recreates the component's
    // object on every inspector edit, which takes the published subtree with it, while THIS
    // entry survives (it is keyed to outlive the component by design). Dereferencing the stored
    // pointer then reads freed memory -- a segfault one frame after dragging a value.
    coopa::scene::SceneObject* published_root = entry.published.live_root(component.owner);
    if (published_root == nullptr) entry.published.root = nullptr;
    if (component.owner != nullptr && published_root != nullptr &&
        bindings_reference_subtree(component.inputs, scene, *published_root)) {
        pending.errors.push_back(
            "a binding resolves to an object inside this graph's own output; that would re-dirty "
            "the bake every frame and never settle");
        entry.failed_compile = true;
        entry.baked          = true;
        entry.last_result    = std::move(pending);
        warn_once_("[ProceduralGraph] " + key + ": " + entry.last_result.messages());
        return;
    }

    uint64_t hash = 0;
    std::vector<Value> inputs = resolve_inputs(component.inputs, *entry.compiled, resolve, hash);
    for (const auto& e : resolve_diag.entries()) {
        if (e.severity == coopa::nodegraph::Severity::Error) pending.errors.push_back(e.message);
        else pending.warnings.push_back(e.message);
    }

    // --- The change gate. This is what makes `on_change` affordable. ---
    if (!first_bake && !forced && hash == entry.input_hash) return;

    // --- Step 2: bake. On the job engine when there is one, inline otherwise. ---
    BakeRequest request;
    request.graph              = entry.compiled.get();
    request.inputs             = inputs;
    request.jobs               = ctx.jobs;
    request.input_hash         = hash;
    request.context.seed       = static_cast<uint32_t>(component.seed);
    request.context.graph_name = component.graph;
    // Copied here, with the bindings, so a Variable node reads a snapshot rather than racing a
    // set_variable() call from the main thread while the bake runs on a worker.
    request.context.variables  = variables_.all();
    if (component.owner != nullptr) {
        request.context.self.name  = component.owner->name();
        request.context.self.valid = true;
        if (auto* tc = component.owner->get_transform()) {
            const glm::mat4 world = tc->get_world_matrix();
            request.context.self.position = glm::vec3(world[3]);
        }
    }

    BakeResult result = bake(request);
    for (auto& w : pending.warnings) result.warnings.push_back(std::move(w));
    for (auto& e : pending.errors) result.errors.push_back(std::move(e));
    ++bake_count_;

    // --- Step 3: publish, on the main thread. ---
    if (component.owner != nullptr) {
        PublishContext publish_ctx;
        publish_ctx.scene      = &scene;
        publish_ctx.owner      = component.owner;
        publish_ctx.assets     = assets_;
        publish_ctx.device     = device_;
        publish_ctx.allocator  = allocator_;
        publish_ctx.id_prefix  = "runtime/procgraph/" + key;
        publish_ctx.scene_path = scene_path_;

        PublishedGraph published = publish(result, publish_ctx, entry.published);
        for (const auto& e : published.errors) result.errors.push_back(e);
        entry.published = std::move(published);
    }

    entry.input_hash = hash;
    entry.baked      = true;
    entry.force      = false;
    entry.last_result = std::move(result);

    if (!entry.last_result.ok()) {
        warn_once_("[ProceduralGraph] " + key + ": " + entry.last_result.messages());
    }
}

void ProceduralGraphSystem::execute(coopa::scene::Scene& scene,
                                    const coopa::scene::FrameContext& ctx) {
    register_procgraph_nodes();
    register_builtin_input_resolvers();

    const auto components = scene.get_components<ProceduralGraph>();
    int pending = 0;
    for (ProceduralGraph* component : components) {
        if (component == nullptr) continue;
        const std::string key = component->state_key();
        auto it = entries_.find(key);
        const bool needs_first_bake = it == entries_.end() || !it->second.baked;

        // An `on_change` graph re-resolves every frame; the others only when something asked.
        const bool should_run = needs_first_bake || it->second.force || variables_dirty_ ||
                                component->bake == BakeMode::OnChange ||
                                component->refresh_on_asset_change;
        if (should_run) process_(scene, ctx, *component);

        auto after = entries_.find(key);
        if (after != entries_.end() && !after->second.baked) ++pending;
    }
    variables_dirty_ = false;
    pending_warmup_  = pending;

    // A component removed from the scene takes its published output with it. Without this, editing
    // a scene in the editor would leave orphaned baked objects and leaked synthetic assets behind.
    if (entries_.size() > components.size()) {
        std::set<std::string> live;
        for (ProceduralGraph* component : components) {
            if (component != nullptr) live.insert(component->state_key());
        }
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (live.count(it->first) != 0) { ++it; continue; }
            PublishContext publish_ctx;
            publish_ctx.scene     = &scene;
            publish_ctx.owner     = nullptr;   // The owner is gone; only assets need releasing.
            publish_ctx.assets    = assets_;
            unpublish(it->second.published, publish_ctx);
            it = entries_.erase(it);
        }
    }
}

coopa::scene::WarmupState ProceduralGraphSystem::warm_up_state() const {
    coopa::scene::WarmupState ws;
    ws.participates = pending_warmup_ > 0;
    if (!ws.participates) return ws;
    ws.what      = "procedural graphs";
    ws.warm      = pending_warmup_ == 0;
    ws.remaining = pending_warmup_;
    ws.progress  = 0.0f;
    return ws;
}

void ProceduralGraphSystem::on_detach(coopa::scene::Scene& scene) {
    PublishContext publish_ctx;
    publish_ctx.scene  = &scene;
    publish_ctx.assets = assets_;
    for (auto& [key, entry] : entries_) {
        // The owner is left null: the scene is going away, so its objects destroy themselves.
        // Only the synthetic assets and instance sets, which outlive the scene, need releasing.
        unpublish(entry.published, publish_ctx);
    }
    entries_.clear();
    compiled_cache_.clear();
    source_cache_.clear();
}

void ProceduralGraphSystem::rebake(const ProceduralGraph& component) {
    Entry& entry = entry_for_(component.state_key());
    entry.force          = true;
    entry.failed_compile = false;
    entry.compiled.reset();   // Pick up an edited graph file as well as changed inputs.
}

void ProceduralGraphSystem::rebake_all() {
    for (auto& [key, entry] : entries_) {
        entry.force          = true;
        entry.failed_compile = false;
        entry.compiled.reset();
    }
    compiled_cache_.clear();
    source_cache_.clear();
}

void ProceduralGraphSystem::set_variable(std::string name, Value value) {
    const Value& existing = variables_.get(name);
    // Only an actual change dirties the graphs: a system pushing the same value every frame must
    // not trigger a re-bake every frame.
    if (!existing.empty() && existing.equals(value)) return;
    variables_.set(std::move(name), std::move(value));
    variables_dirty_ = true;
}

void ProceduralGraphSystem::load_variables(const fkyaml::node& node) {
    variables_.load_from_node(node);
    variables_dirty_ = true;
}

const BakeResult* ProceduralGraphSystem::last_result(const ProceduralGraph& component) const {
    auto it = entries_.find(component.state_key());
    return it == entries_.end() ? nullptr : &it->second.last_result;
}

ProceduralGraphSystem* install_procgraph_system(coopa::scene::Scene& scene,
                                                coopa::asset::AssetManager* assets,
                                                coopa::gfx::core::Device* device,
                                                coopa::gfx::memory::Allocator* allocator,
                                                const fkyaml::node& settings,
                                                int order) {
    if (auto* existing = find_procgraph(scene)) {
        if (settings.is_mapping() && settings.contains("variables")) {
            existing->load_variables(settings.at("variables"));
        }
        return existing;
    }
    auto system = std::make_unique<ProceduralGraphSystem>(assets, device, allocator);
    ProceduralGraphSystem* raw = system.get();
    if (settings.is_mapping() && settings.contains("variables")) {
        raw->load_variables(settings.at("variables"));
    }
    scene.add_system(std::move(system), order);
    return raw;
}

} // namespace procgraph
} // namespace toy
