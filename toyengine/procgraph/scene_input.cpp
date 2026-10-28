/**
 * @file scene_input.cpp
 * @brief The built-in binding resolvers, the variable table, and the snapshot pass.
 */

#include <toyengine/procgraph/scene_input.h>

#include <algorithm>
#include <mutex>
#include <shared_mutex>

#include <glm/gtc/matrix_transform.hpp>

#include <coopa/animation/animated_property.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/data/mesh.h>

#include <toyengine/core/yaml_read.h>

namespace toy {
namespace procgraph {
namespace {

using coopa::nodegraph::Severity;
using coopa::nodegraph::TypeRegistry;
using coopa::nodegraph::Value;
using coopa::scene::SceneObject;

std::shared_mutex& resolver_mutex_() {
    static std::shared_mutex mutex;
    return mutex;
}

std::unordered_map<std::string, InputResolver>& resolvers_() {
    static std::unordered_map<std::string, InputResolver> table;
    return table;
}

/// @brief Decomposes an object's world matrix into the fields an ObjectRef carries.
void fill_transform_(const SceneObject& obj, ObjectRef& out) {
    auto* tc = obj.get_transform();
    if (tc == nullptr) return;
    const glm::mat4 world = tc->get_world_matrix();
    out.position = glm::vec3(world[3]);
    // Scale is each basis vector's length; the rotation is what is left once it is divided out.
    glm::vec3 basis_x(world[0]);
    glm::vec3 basis_y(world[1]);
    glm::vec3 basis_z(world[2]);
    out.scale = glm::vec3(glm::length(basis_x), glm::length(basis_y), glm::length(basis_z));
    if (out.scale.x > 1e-8f) basis_x /= out.scale.x;
    if (out.scale.y > 1e-8f) basis_y /= out.scale.y;
    if (out.scale.z > 1e-8f) basis_z /= out.scale.z;
    glm::mat3 rotation(basis_x, basis_y, basis_z);
    out.rotation = glm::quat_cast(rotation);
}

/**
 * @brief World-space bounds from the object's renderer, if it has one with a loaded mesh.
 *
 * Bounds come from the GPU mesh's own recorded extents rather than by walking vertices: the
 * extents are already there, and a renderer whose mesh is still loading simply reports none,
 * which a graph can test with the `valid` flag.
 */
void fill_bounds_(const SceneObject& obj, ObjectRef& out) {
    auto* mr = obj.get_component<coopa::gfx::engine::components::MeshRenderer>();
    if (mr == nullptr) return;
    const auto& handle = mr->get_mesh();
    const auto* mesh = handle.get();
    if (mesh == nullptr) return;
    const glm::vec3 lo = mesh->bounds_min();
    const glm::vec3 hi = mesh->bounds_max();
    auto* tc = obj.get_transform();
    const glm::mat4 world = tc ? tc->get_world_matrix() : glm::mat4(1.0f);
    // Every corner is transformed and re-bounded: transforming only min and max is wrong the
    // moment there is any rotation in the matrix.
    bool first = true;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
        const glm::vec3 world_corner = glm::vec3(world * glm::vec4(corner, 1.0f));
        if (first) {
            out.bounds_min = out.bounds_max = world_corner;
            first = false;
        } else {
            out.bounds_min = glm::min(out.bounds_min, world_corner);
            out.bounds_max = glm::max(out.bounds_max, world_corner);
        }
    }
}

/// @brief Copies the object's mesh into a GeometrySet, in the object's own local space.
void fill_geometry_(const SceneObject& obj, ObjectRef& out, const ResolveContext& ctx) {
    auto* mr = obj.get_component<coopa::gfx::engine::components::MeshRenderer>();
    if (mr == nullptr) {
        ctx.warn("object '" + out.name + "' has no MeshRenderer, so `geometry: true` yields nothing");
        return;
    }
    const auto* mesh = mr->get_mesh().get();
    if (mesh == nullptr) {
        ctx.warn("object '" + out.name + "' has a MeshRenderer whose mesh is not loaded yet");
        return;
    }
    // A GPU mesh keeps no CPU copy of its vertices -- the staging buffer is released after the
    // upload. So the honest answer is bounds, not triangles, and a graph that wants real geometry
    // to work on should reference the mesh ASSET rather than an instance of it. Saying so here is
    // better than returning an empty set and letting an author wonder.
    ctx.warn("object '" + out.name + "' has no CPU-side mesh to read: an uploaded mesh keeps no "
             "vertex copy. Use a primitive node, or a Mesh Asset node, instead of `geometry: true`");
}

ObjectRef snapshot_object_(const SceneObject& obj, bool want_geometry, const ResolveContext& ctx) {
    ObjectRef ref;
    ref.name  = obj.name();
    ref.valid = true;
    fill_transform_(obj, ref);
    fill_bounds_(obj, ref);
    if (want_geometry) fill_geometry_(obj, ref, ctx);
    return ref;
}

// --- The built-in resolvers -------------------------------------------------------------------

Value resolve_object_(const fkyaml::node& args, const ResolveContext& ctx) {
    if (ctx.scene == nullptr) return Value();
    std::string name;
    bool want_geometry = false;
    if (args.is_string()) {
        name = args.get_value<std::string>();
    } else if (args.is_mapping()) {
        core::yaml::read(args, "object", name);
        core::yaml::read(args, "geometry", want_geometry);
    }
    if (name.empty()) {
        ctx.error("an `object:` binding needs an object name");
        return Value();
    }
    SceneObject* obj = ctx.scene->find_object(name);
    if (obj == nullptr) {
        ctx.warn("no object named '" + name + "' in the scene");
        ObjectRef missing;
        missing.name = name;
        return make_object(std::move(missing));   // valid == false, so a graph can branch on it.
    }
    return make_object(snapshot_object_(*obj, want_geometry, ctx));
}

Value resolve_children_of_(const fkyaml::node& args, const ResolveContext& ctx) {
    if (ctx.scene == nullptr) return Value();
    std::string name;
    bool want_geometry = false;
    if (args.is_string()) {
        name = args.get_value<std::string>();
    } else if (args.is_mapping()) {
        core::yaml::read(args, "children_of", name);
        core::yaml::read(args, "geometry", want_geometry);
    }
    if (name.empty()) {
        ctx.error("a `children_of:` binding needs an object name");
        return Value();
    }
    SceneObject* parent = ctx.scene->find_object(name);
    if (parent == nullptr) {
        ctx.warn("no object named '" + name + "' to take children from");
        return make_object_set(ObjectSet::from({}));
    }
    std::vector<ObjectRef> refs;
    refs.reserve(parent->children().size());
    for (const auto& child : parent->children()) {
        if (child) refs.push_back(snapshot_object_(*child, want_geometry, ctx));
    }
    return make_object_set(ObjectSet::from(std::move(refs)));
}

/// @brief Depth-first walk collecting every object whose name matches.
void collect_matching_(SceneObject& obj, const std::string& pattern, bool want_geometry,
                       const ResolveContext& ctx, std::vector<ObjectRef>& out) {
    if (glob_match(pattern, obj.name())) out.push_back(snapshot_object_(obj, want_geometry, ctx));
    for (const auto& child : obj.children()) {
        if (child) collect_matching_(*child, pattern, want_geometry, ctx, out);
    }
}

Value resolve_name_like_(const fkyaml::node& args, const ResolveContext& ctx) {
    if (ctx.scene == nullptr) return Value();
    std::string pattern;
    bool want_geometry = false;
    if (args.is_string()) {
        pattern = args.get_value<std::string>();
    } else if (args.is_mapping()) {
        core::yaml::read(args, "name_like", pattern);
        core::yaml::read(args, "geometry", want_geometry);
    }
    if (pattern.empty()) {
        ctx.error("a `name_like:` binding needs a pattern");
        return Value();
    }
    std::vector<ObjectRef> refs;
    for (const auto& root : ctx.scene->root_objects()) {
        if (root) collect_matching_(*root, pattern, want_geometry, ctx, refs);
    }
    // Sorted by name so the snapshot is stable: a graph that instances over this set must not
    // reshuffle just because the scene's internal object order changed.
    std::sort(refs.begin(), refs.end(),
              [](const ObjectRef& a, const ObjectRef& b) { return a.name < b.name; });
    return make_object_set(ObjectSet::from(std::move(refs)));
}

Value resolve_component_(const fkyaml::node& args, const ResolveContext& ctx) {
    if (ctx.scene == nullptr || !args.is_mapping()) return Value();
    std::string object_name;
    std::string component_type;
    std::string key;
    core::yaml::read(args, "object", object_name);
    core::yaml::read(args, "component", component_type);
    core::yaml::read(args, "key", key);
    if (component_type.empty() || key.empty()) {
        ctx.error("a `component:` binding needs both `component:` and `key:`");
        return Value();
    }

    SceneObject* obj = object_name.empty() ? ctx.owner : ctx.scene->find_object(object_name);
    if (obj == nullptr) {
        ctx.warn("no object named '" + object_name + "' for a component binding");
        return Value();
    }

    // The engine has no reflection; AnimatedPropertyRegistry is its one generic component-property
    // path, and it is the same one the animation system drives -- so this works for any component
    // that registered its properties, a game project's own included.
    const auto* prop = coopa::anim::AnimatedPropertyRegistry::instance().find(component_type, key);
    if (prop == nullptr) {
        ctx.warn("component '" + component_type + "' has no animatable property '" + key +
                 "'; register it with AnimatedPropertyRegistry to bind it");
        return Value();
    }

    coopa::scene::Component* found = nullptr;
    for (const auto& component : obj->components()) {
        if (component && component->type_name() == component_type) {
            found = component.get();
            break;
        }
    }
    if (found == nullptr) {
        ctx.warn("object '" + obj->name() + "' has no " + component_type + " component");
        return Value();
    }

    void* target = prop->cast ? prop->cast(*found) : nullptr;
    if (target == nullptr || !prop->get) {
        ctx.warn("could not read '" + component_type + "." + key + "'");
        return Value();
    }
    float channels[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    prop->get(target, channels);

    switch (prop->component_count) {
        case 1: return make_float(channels[0]);
        case 2: return make_vector(glm::vec3(channels[0], channels[1], 0.0f));
        case 3: return make_vector(glm::vec3(channels[0], channels[1], channels[2]));
        default: return make_color(glm::vec4(channels[0], channels[1], channels[2], channels[3]));
    }
}

Value resolve_var_(const fkyaml::node& args, const ResolveContext& ctx) {
    std::string name;
    if (args.is_string()) {
        name = args.get_value<std::string>();
    } else if (args.is_mapping()) {
        core::yaml::read(args, "var", name);
    }
    if (name.empty()) {
        ctx.error("a `var:` binding needs a variable name");
        return Value();
    }
    if (ctx.variables == nullptr || !ctx.variables->contains(name)) {
        ctx.warn("variable '" + name + "' is not set; the socket default is used instead");
        return Value();
    }
    return ctx.variables->get(name);
}

} // namespace

void ResolveContext::error(const std::string& message) const {
    if (out) out->add(Severity::Error, 0, "input:" + socket, message);
}
void ResolveContext::warn(const std::string& message) const {
    if (out) out->add(Severity::Warning, 0, "input:" + socket, message);
}

bool glob_match(const std::string& pattern, const std::string& name) {
    // Iterative backtracking rather than recursion: a pathological pattern over a scene with
    // thousands of objects should not be able to put a stack frame per character on the stack.
    size_t p = 0, n = 0, star = std::string::npos, mark = 0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = n;
        } else if (star != std::string::npos) {
            p = star + 1;
            n = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

void VariableTable::load_from_node(const fkyaml::node& node) {
    if (!node.is_mapping()) return;
    for (auto it = node.begin(); it != node.end(); ++it) {
        const std::string name = it.key().get_value<std::string>();
        const fkyaml::node& v = it.value();
        if (v.is_boolean()) {
            set(name, make_bool(v.get_value<bool>()));
        } else if (v.is_integer()) {
            set(name, make_int(static_cast<int32_t>(v.get_value<int64_t>())));
        } else if (v.is_float_number()) {
            set(name, make_float(static_cast<float>(v.get_value<double>())));
        } else if (v.is_string()) {
            set(name, make_string(v.get_value<std::string>()));
        } else if (v.is_sequence() && v.size() >= 3) {
            set(name, make_vector(core::yaml::as_vec3(v)));
        }
        // Anything else (a nested mapping) is skipped: a variable is a scalar by design, and a
        // structured variable would need a type the graph could name.
    }
}

void register_input_resolver(std::string kind, InputResolver fn) {
    std::unique_lock<std::shared_mutex> lock(resolver_mutex_());
    resolvers_()[std::move(kind)] = std::move(fn);
}

bool has_input_resolver(const std::string& kind) {
    std::shared_lock<std::shared_mutex> lock(resolver_mutex_());
    return resolvers_().find(kind) != resolvers_().end();
}

std::vector<std::string> input_resolver_kinds() {
    std::shared_lock<std::shared_mutex> lock(resolver_mutex_());
    std::vector<std::string> out;
    out.reserve(resolvers_().size());
    for (const auto& [kind, fn] : resolvers_()) out.push_back(kind);
    std::sort(out.begin(), out.end());
    return out;
}

void register_builtin_input_resolvers() {
    static bool done = [] {
        register_input_resolver("object", resolve_object_);
        register_input_resolver("children_of", resolve_children_of_);
        register_input_resolver("name_like", resolve_name_like_);
        register_input_resolver("component", resolve_component_);
        register_input_resolver("var", resolve_var_);
        return true;
    }();
    (void)done;
}

InputBinding parse_input_binding(const fkyaml::node& node) {
    InputBinding binding;
    binding.args = node;
    if (!node.is_mapping()) return binding;   // A scalar or sequence literal.
    register_builtin_input_resolvers();
    // The binding kind is whichever key names a registered resolver. Checking every key rather
    // than only the first means key order in the file does not matter.
    for (auto it = node.begin(); it != node.end(); ++it) {
        const std::string key = it.key().get_value<std::string>();
        if (has_input_resolver(key)) {
            binding.kind = key;
            return binding;
        }
    }
    return binding;   // A mapping naming no resolver is a literal, not an error.
}

std::vector<coopa::nodegraph::Value> resolve_inputs(
    const std::unordered_map<std::string, InputBinding>& bindings,
    const coopa::nodegraph::CompiledGraph& compiled,
    const ResolveContext& ctx,
    uint64_t& out_hash) {
    register_builtin_input_resolvers();

    std::vector<Value> out;
    out.reserve(compiled.inputs().size());
    uint64_t seed = 1469598103934665603ull;   // FNV-1a 64 basis.

    for (const auto& socket : compiled.inputs()) {
        ResolveContext local = ctx;
        local.wanted = socket.type;
        local.socket = socket.identifier;

        auto it = bindings.find(socket.identifier);
        if (it == bindings.end()) {
            out.emplace_back();   // Unbound: the evaluator uses the declared default.
            seed ^= 0x9e3779b97f4a7c15ull;
            seed *= 1099511628211ull;
            continue;
        }

        Value value;
        if (it->second.is_literal()) {
            const auto& info = TypeRegistry::instance().info(socket.type);
            if (info.from_yaml == nullptr) {
                local.warn("socket type '" + info.name + "' has no YAML hook, so a literal cannot "
                           "be read for it");
            } else {
                Value parsed = Value::make_default(socket.type);
                if (!parsed.empty() && info.from_yaml(it->second.args, parsed.mutable_data())) {
                    value = std::move(parsed);
                } else {
                    local.warn("could not read a literal for '" + socket.identifier + "'");
                }
            }
        } else {
            InputResolver fn;
            {
                std::shared_lock<std::shared_mutex> lock(resolver_mutex_());
                auto found = resolvers_().find(it->second.kind);
                // Copied out and called unlocked, so a resolver that itself registers one (or
                // simply takes a long time) cannot deadlock against the table's lock.
                if (found != resolvers_().end()) fn = found->second;
            }
            if (!fn) {
                local.error("no resolver registered for binding kind '" + it->second.kind + "'");
            } else {
                value = fn(it->second.args, local);
            }
        }

        // A resolved value of a convertible type is converted here rather than being refused, so
        // binding an Int variable to a Float parameter works the way an author expects.
        if (!value.empty() && value.type() != socket.type) {
            Value converted = value.converted_to(socket.type);
            if (converted.empty()) {
                const auto& reg = TypeRegistry::instance();
                local.warn("binding produced a " + reg.name_of(value.type()) + " but '" +
                           socket.identifier + "' wants a " + reg.name_of(socket.type) +
                           "; the default is used instead");
                value.reset();
            } else {
                value = std::move(converted);
            }
        }

        value.hash_into(seed);
        out.push_back(std::move(value));
    }

    // The compiled graph's own revision is folded in, so editing the graph re-bakes even when
    // every input is untouched.
    const uint64_t revision = compiled.revision();
    seed ^= revision;
    seed *= 1099511628211ull;
    out_hash = seed;
    return out;
}

namespace {

/// @brief Whether `candidate` is `root` or a descendant of it.
bool is_within_(const SceneObject& root, const SceneObject& candidate) {
    if (&root == &candidate) return true;
    for (const auto& child : root.children()) {
        if (child && is_within_(*child, candidate)) return true;
    }
    return false;
}

/// @brief Every object name a binding could resolve to, without snapshotting anything.
void collect_binding_names_(const InputBinding& binding, std::vector<std::string>& out) {
    if (binding.is_literal()) return;
    const fkyaml::node& a = binding.args;
    for (const char* key : {"object", "children_of"}) {
        if (a.is_mapping() && a.contains(key) && a.at(key).is_string()) {
            out.push_back(a.at(key).get_value<std::string>());
        }
    }
    if (a.is_string() && (binding.kind == "object" || binding.kind == "children_of")) {
        out.push_back(a.get_value<std::string>());
    }
}

} // namespace

bool bindings_reference_subtree(const std::unordered_map<std::string, InputBinding>& bindings,
                                const coopa::scene::Scene& scene,
                                const coopa::scene::SceneObject& root) {
    std::vector<std::string> names;
    for (const auto& [socket, binding] : bindings) collect_binding_names_(binding, names);
    for (const std::string& name : names) {
        SceneObject* obj = scene.find_object(name);
        if (obj != nullptr && is_within_(root, *obj)) return true;
    }
    // A `name_like` pattern is checked against the subtree's own names, since it could sweep one
    // up without naming it.
    for (const auto& [socket, binding] : bindings) {
        if (binding.kind != "name_like") continue;
        std::string pattern;
        if (binding.args.is_string()) pattern = binding.args.get_value<std::string>();
        else core::yaml::read(binding.args, "name_like", pattern);
        if (pattern.empty()) continue;
        if (glob_match(pattern, root.name())) return true;
        for (const auto& child : root.children()) {
            if (child && glob_match(pattern, child->name())) return true;
        }
    }
    return false;
}

} // namespace procgraph
} // namespace toy
