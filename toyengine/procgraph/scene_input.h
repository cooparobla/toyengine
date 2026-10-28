/**
 * @file scene_input.h
 * @brief Binding a graph's inputs to things in the surrounding scene, and the variable table.
 *
 * ### The snapshot seam
 *
 * A graph is far more useful if it can process authored content -- a spline someone drew, a
 * blockout mesh, a row of placed markers, a value a gameplay system owns. But the bake runs on a
 * worker thread and `coopa::scene::Scene` is single-owner, main-thread-only: component parsers are
 * not thread-safe, and the editor destroys and recreates components between a system's `execute()`
 * and the render hand-off. A node reaching into `Scene` during evaluation would be a data race
 * that surfaces as a rare crash somewhere else entirely.
 *
 * **So resolution and evaluation are separated by a snapshot.** `resolve_inputs()` runs on the
 * main thread and turns every binding into an immutable `Value`; only then does the bake start,
 * reading nothing but those values. No node type ever sees a `Scene*` or a `SceneObject*` -- the
 * snapshot types (`ObjectRef`, `ObjectSet`) carry a name, a transform, bounds and optionally
 * geometry, and no pointers. The absence of a pointer is what makes the contract enforceable
 * rather than a convention.
 *
 * The snapshot is also what makes re-baking on change cheap: re-resolving is a handful of
 * transform reads, hashed, and the bake only re-runs when the hash moves.
 *
 * ### Resolvers are registered, so this list is not the limit
 *
 * Each binding kind is a registered function, following the same idiom as
 * `SceneLoader::register_component_parser`. A game project adds its own kind with one
 * `register_input_resolver()` call and no edit here. The built-ins are:
 *
 * | Key | Yields | Notes |
 * |---|---|---|
 * | `object` | `Object` | Its transform and world bounds. With `geometry: true`, also its mesh. |
 * | `children_of` | `ObjectSet` | An object's direct children. |
 * | `name_like` | `ObjectSet` | Glob over object names (`"Rock_*"`). |
 * | `component` + `key` | a scalar / vector | One property off a component on an object. |
 * | `var` | whatever it holds | The escape hatch; see below. |
 *
 * There is deliberately **no `tag` resolver**: this engine's `SceneObject` has no tag concept, so
 * a tag binding would need one invented first. `name_like` covers the same authoring need.
 *
 * `component` + `key` goes through `coopa::anim::AnimatedPropertyRegistry` -- the engine's one
 * existing generic component-property path, the same one the animation system drives. So it works
 * for any component that registered its properties, including a game project's own, and needs no
 * reflection that does not exist.
 *
 * ### Variables are the general escape hatch
 *
 * Anything a scene query cannot express becomes a named variable, fed from two directions: the
 * scene's own `settings.procgraph.variables` block for authored constants, and
 * `ProceduralGraphSystem::set_variable()` for anything live. A graph reads one with
 * `{ var: world.season }`. That is how a gameplay value drives a graph without the component
 * knowing anything about gameplay.
 */

#ifndef TOYENGINE_PROCGRAPH_SCENE_INPUT_H
#define TOYENGINE_PROCGRAPH_SCENE_INPUT_H

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <fkYAML/node.hpp>

#include <coopa/nodegraph/compile.h>
#include <coopa/nodegraph/diagnostics.h>
#include <coopa/nodegraph/value.h>
#include <toyengine/procgraph/proc_types.h>

namespace coopa {
namespace asset {
class AssetManager;
}
namespace scene {
class Scene;
class SceneObject;
}
}

namespace toy {
namespace procgraph {

using coopa::nodegraph::Diagnostics;

/**
 * @struct InputBinding
 * @brief One `inputs:` entry: a literal, or a named way of fetching something from the scene.
 */
struct InputBinding {
    /// @brief Empty for a literal; otherwise a registered resolver name ("object", "var", ...).
    std::string  kind;
    /// @brief The authored node, verbatim. A literal's value, or the binding's own keys.
    fkyaml::node args;

    InputBinding() : args(fkyaml::node::mapping()) {}

    bool is_literal() const { return kind.empty(); }
};

/**
 * @class VariableTable
 * @brief Named values a `{ var: ... }` binding reads.
 */
class VariableTable {
public:
    void set(std::string name, coopa::nodegraph::Value v) { vars_[std::move(name)] = std::move(v); }

    /// @brief The value, or an empty one.
    const coopa::nodegraph::Value& get(const std::string& name) const {
        static const coopa::nodegraph::Value nothing;
        auto it = vars_.find(name);
        return it == vars_.end() ? nothing : it->second;
    }

    bool contains(const std::string& name) const { return vars_.find(name) != vars_.end(); }
    void clear() { vars_.clear(); }
    size_t size() const { return vars_.size(); }

    /// @brief The whole table, for taking the snapshot a bake carries.
    const std::unordered_map<std::string, coopa::nodegraph::Value>& all() const { return vars_; }

    /// @brief Reads a `variables:` mapping from scene settings. Scalars only; types are inferred.
    void load_from_node(const fkyaml::node& node);

private:
    std::unordered_map<std::string, coopa::nodegraph::Value> vars_;
};

/**
 * @struct ResolveContext
 * @brief What a resolver is handed. Main thread only -- this is the one place `scene` is legal.
 */
struct ResolveContext {
    coopa::scene::Scene*        scene = nullptr;
    /// @brief The object carrying the ProceduralGraph component, for relative lookups.
    coopa::scene::SceneObject*  owner = nullptr;
    const VariableTable*        variables = nullptr;
    coopa::asset::AssetManager* assets = nullptr;
    /// @brief The socket's declared type, so a resolver can produce the right thing or convert.
    coopa::nodegraph::TypeId    wanted = 0;
    /// @brief The input's identifier, for a message that names what failed.
    std::string                 socket;
    Diagnostics*                out = nullptr;

    void error(const std::string& message) const;
    void warn(const std::string& message) const;
};

/// @brief Turns a binding's args into a Value. Returns an empty Value to mean "could not".
using InputResolver = std::function<coopa::nodegraph::Value(const fkyaml::node& args,
                                                           const ResolveContext& ctx)>;

/**
 * @brief Registers a binding kind under the YAML key that selects it.
 *
 * Write at startup, read concurrently -- the same contract as every other coopa registry. A key
 * registered twice replaces the first, so a test can substitute one.
 */
void register_input_resolver(std::string kind, InputResolver fn);

/// @brief Whether `kind` has a resolver. Used by the component parser to reject a typo early.
bool has_input_resolver(const std::string& kind);

/// @brief Every registered kind, sorted. For an error message that lists the alternatives.
std::vector<std::string> input_resolver_kinds();

/// @brief Registers the built-in resolvers described in the @file doc. Idempotent.
void register_builtin_input_resolvers();

/**
 * @brief Parses one `inputs:` entry into a binding.
 *
 * A mapping whose first key names a registered resolver becomes that binding; anything else --
 * including a mapping that happens to look like one but names no resolver -- is treated as a
 * literal, so an unrecognised shape is authored data rather than an error.
 */
InputBinding parse_input_binding(const fkyaml::node& node);

/**
 * @brief Resolves every binding against the compiled graph's interface.
 *
 * Positional on the graph's `inputs()`: an interface input with no binding gets an empty Value and
 * the evaluator falls back to its declared default.
 *
 * @param bindings Keyed by interface input identifier.
 * @param compiled The graph whose interface decides the order and the wanted types.
 * @param ctx      Scene access. Main thread.
 * @param out_hash Receives a digest of every resolved value, for change detection.
 * @return One Value per interface input, in interface order.
 */
std::vector<coopa::nodegraph::Value> resolve_inputs(
    const std::unordered_map<std::string, InputBinding>& bindings,
    const coopa::nodegraph::CompiledGraph& compiled,
    const ResolveContext& ctx,
    uint64_t& out_hash);

/**
 * @brief Whether any binding resolves to an object at or under `root`.
 *
 * A graph that reads what it writes never settles: every bake would dirty the next one. The
 * system calls this before accepting a binding set and refuses it as a diagnostic rather than
 * looping forever.
 */
bool bindings_reference_subtree(const std::unordered_map<std::string, InputBinding>& bindings,
                                const coopa::scene::Scene& scene,
                                const coopa::scene::SceneObject& root);

/// @brief Whether `name` matches `pattern`, where `*` matches any run and `?` any one character.
bool glob_match(const std::string& pattern, const std::string& name);

} // namespace procgraph
} // namespace toy

#endif // TOYENGINE_PROCGRAPH_SCENE_INPUT_H
