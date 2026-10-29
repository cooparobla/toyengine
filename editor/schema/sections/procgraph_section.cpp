/**
 * @file procgraph_section.cpp
 * @brief The `ProceduralGraph` inspector's Inputs block: a row per input the chosen graph declares.
 *
 * This cannot be a schema. A schema is a fixed list of fields built once at start-up, and which
 * inputs a `ProceduralGraph` has is decided by the graph asset its `graph:` key names -- a
 * different file, choosable per component, editable while the inspector is open. So the rows are
 * built here, each frame, from the graph's own interface: its declared type picks the widget, and
 * its `min`/`max` become the widget's range, which is the whole reason `InterfaceSocket` carries
 * them.
 *
 * An input is authored either as a literal (`steps: 5`) or as a binding that fetches something
 * from the scene (`markers: { children_of: Markers }`), and the kind dropdown switches between
 * the two. Types that cannot be written down -- Object, ObjectSet, Geometry -- offer only
 * bindings, since a literal for them has no spelling.
 */

#include "editor/schema/inspector_sections.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <coopa/nodegraph/graph.h>
#include <coopa/nodegraph/graph_yaml.h>
#include <coopa/nodegraph/type_registry.h>
#include <toyengine/procgraph/nodes.h>
#include <toyengine/procgraph/scene_input.h>

#include "editor/schema/component_schema.h"

namespace toy {
namespace editor {
namespace {

namespace ng = coopa::nodegraph;
namespace fs = std::filesystem;

/// @brief A graph's interface, reloaded only when the file's timestamp moves.
struct CachedInterface {
    fs::file_time_type stamp{};
    std::vector<ng::InterfaceSocket> inputs;
    bool loaded = false;
};

/**
 * @brief The interface of the graph at `abs`, parsed at most once per change on disk.
 *
 * The inspector redraws every frame; re-parsing a YAML file at frame rate to answer "what inputs
 * does this have" would be the single most expensive thing in the panel.
 */
const CachedInterface& interface_of_(const std::string& abs) {
    static std::unordered_map<std::string, CachedInterface> cache;
    CachedInterface& entry = cache[abs];

    std::error_code ec;
    const fs::file_time_type now = fs::last_write_time(abs, ec);
    if (ec) return entry;
    if (entry.loaded && now == entry.stamp) return entry;

    procgraph::register_procgraph_nodes();   // Socket type names resolve through the registry.
    ng::Diagnostics diag;
    ng::Graph g = ng::load_graph(abs, diag);
    entry.stamp  = now;
    entry.loaded = true;
    entry.inputs = g.inputs();
    return entry;
}

/**
 * @brief The kind dropdown's entry for "a value typed here", beside the resolver kinds.
 *
 * snake_case like every other name in the engine, and like the resolver kinds it sits next to
 * (`object`, `children_of`, `var`). A capitalised entry in that list reads as a different kind
 * of thing than it is.
 */
constexpr const char* k_literal_kind = "value";

/// @brief `graphs/stair` -> `graphs/stair.yaml`; an explicit extension is left alone.
std::string with_yaml_(std::string ref) {
    if (ref.size() < 5 || ref.compare(ref.size() - 5, 5, ".yaml") != 0) ref += ".yaml";
    return ref;
}

/// @brief The binding kind authored for `v`, or empty when it is a literal.
std::string binding_kind_(const Node& v, const std::vector<std::string>& kinds) {
    if (!v.is_mapping()) return {};
    for (const auto& k : kinds) if (v.contains(k)) return k;
    return {};
}

/// @brief Whether a value of `type` can be written down in YAML at all.
bool literal_capable_(ng::TypeId type) {
    const std::string n = ng::TypeRegistry::instance().name_of(type);
    return n == "Float" || n == "Int" || n == "Bool" || n == "Vector" ||
           n == "Color" || n == "String";
}

/**
 * @brief The field used to edit a literal of this socket, ranged by the interface's min/max.
 *
 * `min`/`max` are optional in the file. An absent one leaves the widget unclamped rather than
 * clamping to zero, which would make every unranged float un-draggable upward.
 */
FieldDesc literal_field_(const ng::InterfaceSocket& s) {
    const std::string n = ng::TypeRegistry::instance().name_of(s.type);
    const std::string& k = s.identifier;
    if (n == "Float") {
        const float lo = s.min_value.empty() ? -1e30f : s.min_value.as<float>(0.0f);
        const float hi = s.max_value.empty() ? 1e30f : s.max_value.as<float>(0.0f);
        const float sp = (lo > -1e29f && hi < 1e29f) ? std::max(0.001f, (hi - lo) * 0.005f) : 0.01f;
        return f_float(k, s.default_value.as<float>(0.0f), sp, lo, hi);
    }
    if (n == "Int") {
        const int lo = s.min_value.empty() ? -1000000 : s.min_value.as<int32_t>(0);
        const int hi = s.max_value.empty() ? 1000000 : s.max_value.as<int32_t>(0);
        return f_int(k, s.default_value.as<int32_t>(0), lo, hi);
    }
    if (n == "Bool")   return f_bool(k, s.default_value.as<bool>(false));
    if (n == "Vector") return f_vec3(k, s.default_value.as<glm::vec3>(glm::vec3(0.0f)));
    if (n == "Color")  return f_color4(k, s.default_value.as<glm::vec4>(glm::vec4(1.0f)));
    return f_string(k, s.default_value.as<std::string>(std::string()));
}

/// @brief Writes `block[key]` to the literal default for `s`, so switching to `value` is editable.
void seed_literal_(Node& block, const ng::InterfaceSocket& s) {
    const std::string n = ng::TypeRegistry::instance().name_of(s.type);
    if (n == "Float")       block[s.identifier] = Node(s.default_value.as<float>(0.0f));
    else if (n == "Int")    block[s.identifier] = Node(s.default_value.as<int32_t>(0));
    else if (n == "Bool")   block[s.identifier] = Node(s.default_value.as<bool>(false));
    else if (n == "String") block[s.identifier] = Node(s.default_value.as<std::string>(std::string()));
    else if (n == "Vector") block[s.identifier] = make_vec3(s.default_value.as<glm::vec3>(glm::vec3(0.0f)));
    // Anything else has no literal spelling; callers gate on literal_capable_() before asking.
}

EditResult draw_procgraph_inputs_(imm::Context& ctx, SectionContext& sc) {
    EditResult out;
    sc.handled("inputs");

    const std::string graph_ref = get_string(sc.comp, "graph");
    if (graph_ref.empty()) {
        ctx.label("Choose a graph above; its inputs appear here.");
        return out;
    }
    const std::string abs = sc.env.asset_path ? sc.env.asset_path(with_yaml_(graph_ref)) : std::string();
    if (abs.empty()) {
        ctx.label(graph_ref + " not found");
        return out;
    }
    const CachedInterface& iface = interface_of_(abs);
    if (iface.inputs.empty()) {
        ctx.label(iface.loaded ? "This graph declares no inputs." : "Could not read " + graph_ref);
        return out;
    }

    if (!sc.comp.contains("inputs") || !sc.comp.at("inputs").is_mapping()) {
        sc.comp["inputs"] = Node::mapping();
    }
    Node& block = sc.comp["inputs"];
    const std::vector<std::string> kinds = procgraph::input_resolver_kinds();

    for (const auto& s : iface.inputs) {
        ctx.push_id(s.identifier);

        const bool can_literal = literal_capable_(s.type);
        const Node* cur = block.contains(s.identifier) ? &block.at(s.identifier) : nullptr;
        const std::string kind = cur ? binding_kind_(*cur, kinds) : std::string();

        // The kind dropdown. `value` is only an option for a type a literal can spell.
        std::vector<std::string> opts;
        if (can_literal) opts.push_back(k_literal_kind);
        for (const auto& k : kinds) opts.push_back(k);
        int idx = 0;

        // What the dropdown shows when nothing is authored: a value for a type that can spell
        // one, else the first binding kind. Shown, not stored.
        std::string kind_now = kind;
        if (!cur && kind_now.empty() && !can_literal && !kinds.empty()) kind_now = kinds.front();

        for (std::size_t i = 0; i < opts.size(); ++i) {
            if (opts[i] == kind_now) idx = static_cast<int>(i);
        }

        const std::string title = s.display() + "  (" +
                                  ng::TypeRegistry::instance().name_of(s.type) + ")";
        // `opts` is empty only when the type has no literal spelling AND no resolver is
        // registered yet. Indexing it then is an out-of-bounds read on a click.
        if (!opts.empty() && ctx.combo(title, &idx, opts) &&
            idx >= 0 && idx < static_cast<int>(opts.size())) {
            const std::string picked = opts[static_cast<std::size_t>(idx)];
            if (picked == k_literal_kind) seed_literal_(block, s);
            else {
                Node b = Node::mapping();
                b[picked] = Node(std::string());
                block[s.identifier] = std::move(b);
            }
            out.changed = true;
            out.finished = true;
            out.key = "inputs." + s.identifier;
        }

        // Drawing must not WRITE. An input the author never set is simply absent, and an
        // absent one is not an error -- the evaluator falls back to the interface default. If
        // merely opening the inspector seeded a value, opening a scene would dirty it, and for
        // a bound-only type it would seed an EMPTY binding, which the resolver rightly reports
        // as "a `children_of:` binding needs an object name" on the very next bake.
        //
        // So an absent input is drawn into a scratch node, and only a row the user actually
        // changed is copied back.
        const bool present = block.contains(s.identifier);
        Node scratch = present ? block.at(s.identifier) : Node();
        if (!present) {
            if (!kind_now.empty()) {
                scratch = Node::mapping();
                scratch[kind_now] = Node(std::string());
            } else if (can_literal) {
                scratch = Node::mapping();
                seed_literal_(scratch, s);
                scratch = scratch.at(s.identifier);
            }
        }

        // `draw_field` works on a block keyed by the field name, so the value is wrapped for the
        // call and unwrapped after.
        Node holder = Node::mapping();
        holder[s.identifier] = scratch;
        EditResult row;
        if (kind_now.empty()) {
            if (can_literal) row = draw_field(ctx, literal_field_(s), holder, sc.env);
        } else {
            // A binding's own keys: the resolver's argument, plus `key` for `component`.
            // `object`, `children_of` and `component` all name a scene object, so they get the
            // inspector's object picker rather than a text box you have to spell a name into.
            Node& b = holder[s.identifier];
            if (!b.is_mapping()) { b = Node::mapping(); b[kind_now] = Node(std::string()); }
            const bool names_object = kind_now == "object" || kind_now == "children_of" ||
                                      kind_now == "component";
            FieldDesc arg_field = names_object ? f_child(kind_now) : f_string(kind_now);
            // The KEY stays the resolver's own snake_case name (it is what lands in the YAML);
            // only the row's label is prose. `children_of` names the parent, not the thing.
            arg_field.label = kind_now == "children_of" ? "Parent"
                              : names_object            ? "Object"
                              : kind_now == "var"       ? "Variable"
                                                        : "Pattern";
            row.absorb(draw_field(ctx, arg_field, b, sc.env));
            if (kind_now == "component") {
                FieldDesc key_field = f_string("key");
                key_field.label = "Property";
                row.absorb(draw_field(ctx, key_field, b, sc.env));
            }
        }
        if (row.changed) {
            if (holder.contains(s.identifier)) block[s.identifier] = holder.at(s.identifier);
            else erase_key(block, s.identifier);
        }
        out.absorb(row);

        if (!s.tooltip.empty()) ctx.label("  " + s.tooltip);
        ctx.pop_id();
    }

    // An input the graph no longer declares is dead weight in the file, and silently ignored at
    // bake time -- so say so rather than leaving it invisible.
    std::vector<std::string> stale;
    for (const auto& kv : block.as_map()) {
        const std::string key = kv.first.get_value<std::string>();
        const bool known = std::any_of(iface.inputs.begin(), iface.inputs.end(),
                                       [&](const ng::InterfaceSocket& s) { return s.identifier == key; });
        if (!known) stale.push_back(key);
    }
    if (!stale.empty()) {
        ctx.separator();
        std::string list;
        for (const auto& k : stale) list += (list.empty() ? "" : ", ") + k;
        ctx.label("Not declared by this graph: " + list);
        if (ctx.button("Remove unused")) {
            for (const auto& k : stale) erase_key(block, k);
            out.changed = true; out.finished = true; out.key = "inputs";
        }
    }
    return out;
}

} // namespace

void register_procgraph_section() {
    register_inspector_section("ProceduralGraph.inputs", draw_procgraph_inputs_);
}

} // namespace editor
} // namespace toy
