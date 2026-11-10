/**
 * @file compile.h
 * @brief `CompiledGraph` and `compile()` -- the authored graph turned into something evaluable.
 *
 * Compilation is the expensive step, and it is cached: a `CompiledGraph` is immutable once built
 * and may be evaluated concurrently from any number of threads with different inputs. Everything
 * that can be decided without knowing the input *values* is decided here.
 *
 * ### The pipeline, in order -- and the order is load-bearing
 *
 * 1. **Declare.** Run each node type's `declare` against the node's settings. From here on
 *    nothing looks at node types for their socket lists, only at the resulting declarations.
 * 2. **Flatten groups**, recursively, tracking the reference chain so a group that includes
 *    itself is an error rather than a hang. Each inlining copies the sub-graph's nodes, so a
 *    group used twice becomes two independent sets of steps.
 * 3. **Bind inputs**, attaching a conversion function to any link whose endpoint types differ
 *    but are convertible.
 * 4. **Prune** by backward reachability from the requested outputs.
 * 5. **Order** the survivors topologically.
 * 6. **Fuse** runs of cheap nodes into chains, which are the unit of job submission.
 *
 * Pruning after flattening rather than before is what lets a group's unused half disappear;
 * ordering after pruning means a cycle in a dead branch cannot block a valid graph.
 *
 * ### Everything is a slot, including constants
 *
 * A slot holds one `Value`. Node outputs get slots, the graph's interface inputs get slots, and
 * so do unconnected inputs' defaults -- the last one is why there is no "is this linked?" branch
 * anywhere in the evaluator. Constants are pre-filled before the first step runs.
 *
 * ### Conversions are per-link, not per-step
 *
 * A converted link carries its conversion function on the binding rather than getting its own
 * synthesized node. That avoids an extra slot, an extra step and (under the parallel evaluator) a
 * possible extra job for what is a handful of instructions, and it keeps multi-inputs simple:
 * each incoming link converts independently, which a shared conversion step could not express.
 *
 * ### Slot release is reference-counted, not positional
 *
 * Each slot knows how many steps read it. A step, having run, decrements every slot it read, and
 * whichever decrement reaches zero clears that slot. Using "the last reader in topological order"
 * instead would be wrong under the parallel evaluator, where topological position says nothing
 * about which step finishes last. This is what keeps a chain of mesh operations from holding
 * every intermediate result alive until the end.
 */

#ifndef COOPA_NODEGRAPH_COMPILE_H
#define COOPA_NODEGRAPH_COMPILE_H

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <coopa/nodegraph/declaration.h>
#include <coopa/nodegraph/diagnostics.h>
#include <coopa/nodegraph/graph.h>
#include <coopa/nodegraph/node_type.h>
#include <coopa/nodegraph/type_registry.h>
#include <coopa/nodegraph/value.h>

namespace coopa {
namespace nodegraph {

/// @brief A slot index that means "nothing". Used where a binding could not be resolved.
inline constexpr uint32_t k_no_slot = 0xffffffffu;

/// @brief How deep group references may nest before compilation gives up.
inline constexpr int k_max_group_depth = 32;

/**
 * @struct SlotSource
 * @brief Where one value comes from: a slot, plus an optional conversion applied on read.
 */
struct SlotSource {
    uint32_t slot = k_no_slot;
    /// @brief Null when the types already agree. Applied into freshly default-constructed storage.
    void (*convert)(const void* src, void* dst) = nullptr;
    TypeId   convert_to = k_invalid_type;

    bool valid() const { return slot != k_no_slot; }
};

/**
 * @struct Step
 * @brief One node evaluation.
 */
struct Step {
    const NodeType*  type = nullptr;
    PropertyBag      props;        ///< Owned: a flattened group's nodes outlive their source Graph.
    NodeDeclaration  decl;         ///< Owned, for the same reason.
    NodeId           source_id = 0;   ///< The authored node, for diagnostics.
    std::string      label;

    /// @brief One entry per declared input; each entry is the links feeding it, in link order.
    std::vector<std::vector<SlotSource>> inputs;
    /// @brief One slot per declared output. k_no_slot when nothing reads it.
    std::vector<uint32_t>                out_slots;
    /// @brief Whether each declared output is reachable from a requested output.
    std::vector<bool>                    output_needed;

    /// @brief Step indices this one needs. Deduplicated.
    std::vector<uint32_t>                depends_on;
    /// @brief Distinct slots this step reads, each listed once, for the release counting.
    std::vector<uint32_t>                reads;
};

/**
 * @struct Chain
 * @brief A run of steps submitted as one unit of work. Steps run in listed order.
 */
struct Chain {
    std::vector<uint32_t> steps;       ///< Indices into CompiledGraph::steps().
    std::vector<uint32_t> depends_on;  ///< Chain indices this one waits for.
};

/**
 * @class CompiledGraph
 * @brief An evaluation-ready graph. Immutable; safe to evaluate from many threads at once.
 */
class CompiledGraph {
public:
    const std::vector<Step>&  steps() const { return steps_; }
    const std::vector<Chain>& chains() const { return chains_; }

    uint32_t slot_count() const { return slot_count_; }

    /// @brief Slot/value pairs to pre-fill before the first step: defaults and literals.
    const std::vector<std::pair<uint32_t, Value>>& constants() const { return constants_; }

    /// @brief How many steps read each slot. Indexed by slot. Drives release counting.
    const std::vector<uint32_t>& slot_reader_counts() const { return slot_reader_counts_; }

    /// @brief One slot per interface input, pre-filled from the evaluation request.
    const std::vector<uint32_t>& interface_input_slots() const { return interface_input_slots_; }

    /// @brief Where each interface output reads from.
    const std::vector<SlotSource>& interface_output_sources() const { return interface_output_sources_; }

    /// @brief The interface, copied from the source graph so the graph need not outlive this.
    const std::vector<InterfaceSocket>& inputs() const { return inputs_; }
    const std::vector<InterfaceSocket>& outputs() const { return outputs_; }

    /// @brief Bumped on every successful compile. Part of a consumer's change detection.
    uint64_t revision() const { return revision_; }

    /// @brief Whether any surviving step declares a lazy input.
    bool has_lazy_inputs() const { return has_lazy_inputs_; }

    bool empty() const { return steps_.empty(); }

private:
    friend class GraphCompiler;

    std::vector<Step>                        steps_;
    std::vector<Chain>                       chains_;
    std::vector<std::pair<uint32_t, Value>>  constants_;
    std::vector<uint32_t>                    slot_reader_counts_;
    std::vector<uint32_t>                    interface_input_slots_;
    std::vector<SlotSource>                  interface_output_sources_;
    std::vector<InterfaceSocket>             inputs_;
    std::vector<InterfaceSocket>             outputs_;
    uint32_t                                 slot_count_ = 0;
    uint64_t                                 revision_ = 0;
    bool                                     has_lazy_inputs_ = false;
};

/// @brief Resolves a Group node's `group_ref` to the graph it names. Returns nullptr if missing.
using GroupLoader = std::function<const Graph*(const std::string& ref)>;

/**
 * @class GraphCompiler
 * @brief Runs the pipeline described in the @file doc. One instance per compile.
 *
 * Exposed as a class rather than a single function only so the stages can be read separately;
 * callers use the `compile()` free function below.
 */
class GraphCompiler {
public:
    GraphCompiler(GroupLoader loader, Diagnostics& diag)
        : loader_(std::move(loader)), diag_(diag) {}

    /**
     * @brief Compiles `graph`, keeping only what the named outputs need.
     * @param requested Interface output identifiers to keep. Empty means "all of them".
     */
    CompiledGraph run(const Graph& graph, const std::vector<std::string>& requested) {
        CompiledGraph out;
        out.inputs_  = graph.inputs();
        out.outputs_ = graph.outputs();

        // Interface inputs get the first slots, so the evaluator can fill them by index.
        out.interface_input_slots_.reserve(graph.inputs().size());
        for (size_t i = 0; i < graph.inputs().size(); ++i) {
            out.interface_input_slots_.push_back(next_slot_++);
        }

        std::vector<SlotSource> in_sources;
        in_sources.reserve(out.interface_input_slots_.size());
        for (uint32_t s : out.interface_input_slots_) in_sources.push_back(SlotSource{s, nullptr, k_invalid_type});

        auto out_sources = emit_graph_(graph, in_sources, 0);
        if (out_sources.size() != graph.outputs().size()) out_sources.resize(graph.outputs().size());
        out.interface_output_sources_ = std::move(out_sources);

        out.steps_      = std::move(steps_);
        out.constants_  = std::move(constants_);
        out.slot_count_ = next_slot_;

        prune_(out, requested);
        order_(out);
        count_readers_(out);
        fuse_(out);

        for (const auto& s : out.steps_) {
            for (const auto& in : s.decl.inputs) {
                if (in.lazy()) { out.has_lazy_inputs_ = true; break; }
            }
            if (out.has_lazy_inputs_) break;
        }
        out.revision_ = ++revision_counter_();
        return out;
    }

private:
    static uint64_t& revision_counter_() {
        static uint64_t counter = 0;
        return counter;
    }

    /**
     * @brief Emits steps for one graph, returning where each of its interface outputs reads from.
     *
     * Walks the graph in topological order so a node's inputs always resolve against slots that
     * already exist, which is what makes a nested Group's input bindings available at the moment
     * it recurses.
     */
    std::vector<SlotSource> emit_graph_(const Graph& graph,
                                        const std::vector<SlotSource>& in_sources,
                                        int depth) {
        std::vector<SlotSource> result(graph.outputs().size());
        if (depth > k_max_group_depth) {
            diag_.error("group references nest deeper than " + std::to_string(k_max_group_depth) +
                        " levels; this is almost certainly a cycle");
            return result;
        }

        // Per node, per output index, the slot its value lands in.
        std::unordered_map<NodeId, std::vector<SlotSource>> node_outputs;
        std::unordered_map<NodeId, NodeDeclaration>         node_decls;

        std::vector<NodeId> order = topological_nodes_(graph);

        for (NodeId id : order) {
            const Node* n = graph.node(id);
            if (n == nullptr) continue;

            if (n->type == k_group_input_type) {
                // Its outputs *are* the graph's interface inputs, matched by identifier.
                NodeDeclaration decl;
                std::vector<SlotSource> outs;
                for (size_t i = 0; i < graph.inputs().size(); ++i) {
                    const auto& is = graph.inputs()[i];
                    SocketDecl sd;
                    sd.identifier    = is.identifier;
                    sd.label         = is.label;
                    sd.type          = is.type;
                    sd.default_value = is.default_value;
                    decl.outputs.push_back(std::move(sd));
                    outs.push_back(i < in_sources.size() ? in_sources[i] : SlotSource{});
                }
                node_decls.emplace(id, std::move(decl));
                node_outputs.emplace(id, std::move(outs));
                continue;
            }

            if (n->type == k_group_output_type) {
                NodeDeclaration decl;
                for (const auto& os : graph.outputs()) {
                    SocketDecl sd;
                    sd.identifier    = os.identifier;
                    sd.label         = os.label;
                    sd.type          = os.type;
                    sd.default_value = os.default_value;
                    decl.inputs.push_back(std::move(sd));
                }
                for (size_t i = 0; i < decl.inputs.size(); ++i) {
                    auto sources = resolve_input_(graph, *n, decl.inputs[i], node_outputs, node_decls);
                    result[i] = sources.empty() ? constant_source_(decl.inputs[i]) : sources.front();
                }
                node_decls.emplace(id, std::move(decl));
                continue;
            }

            if (n->type == k_group_node_type) {
                const Graph* sub = loader_ ? loader_(n->group_ref) : nullptr;
                if (sub == nullptr) {
                    diag_.add(Severity::Error, id, n->type,
                              "group '" + n->group_ref + "' could not be resolved");
                    continue;
                }
                if (!push_ref_(n->group_ref)) {
                    diag_.add(Severity::Error, id, n->type,
                              "group '" + n->group_ref + "' includes itself");
                    continue;
                }
                // The group node's sockets mirror the sub-graph's interface.
                NodeDeclaration decl;
                for (const auto& is : sub->inputs()) {
                    SocketDecl sd;
                    sd.identifier    = is.identifier;
                    sd.label         = is.label;
                    sd.type          = is.type;
                    sd.default_value = is.default_value;
                    decl.inputs.push_back(std::move(sd));
                }
                for (const auto& os : sub->outputs()) {
                    SocketDecl sd;
                    sd.identifier = os.identifier;
                    sd.label      = os.label;
                    sd.type       = os.type;
                    decl.outputs.push_back(std::move(sd));
                }
                std::vector<SlotSource> sub_inputs;
                for (const auto& in : decl.inputs) {
                    auto sources = resolve_input_(graph, *n, in, node_outputs, node_decls);
                    sub_inputs.push_back(sources.empty() ? constant_source_(in) : sources.front());
                }
                auto sub_outputs = emit_graph_(*sub, sub_inputs, depth + 1);
                pop_ref_();
                if (sub_outputs.size() != decl.outputs.size()) sub_outputs.resize(decl.outputs.size());
                node_decls.emplace(id, std::move(decl));
                node_outputs.emplace(id, std::move(sub_outputs));
                continue;
            }

            const NodeType* type = NodeTypeRegistry::instance().find(n->type);
            if (type == nullptr) {
                diag_.add(Severity::Error, id, n->type, "unknown node type '" + n->type + "'");
                continue;
            }

            Step step;
            step.type      = type;
            step.props     = n->props;
            step.decl      = type->declaration_for(n->props);
            step.source_id = id;
            step.label     = n->type;

            step.inputs.reserve(step.decl.inputs.size());
            for (const auto& in : step.decl.inputs) {
                auto sources = resolve_input_(graph, *n, in, node_outputs, node_decls);
                if (sources.empty()) sources.push_back(constant_source_(in));
                step.inputs.push_back(std::move(sources));
            }

            std::vector<SlotSource> outs;
            step.out_slots.reserve(step.decl.outputs.size());
            for (size_t i = 0; i < step.decl.outputs.size(); ++i) {
                uint32_t slot = next_slot_++;
                step.out_slots.push_back(slot);
                outs.push_back(SlotSource{slot, nullptr, k_invalid_type});
            }
            step.output_needed.assign(step.decl.outputs.size(), true);

            node_decls.emplace(id, step.decl);
            node_outputs.emplace(id, std::move(outs));
            steps_.push_back(std::move(step));
        }

        return result;
    }

    /// @brief Allocates a constant slot holding a socket's declared default.
    SlotSource constant_source_(const SocketDecl& decl) {
        uint32_t slot = next_slot_++;
        Value v = decl.default_value;
        if (v.empty() && decl.type != k_invalid_type) v = Value::make_default(decl.type);
        constants_.emplace_back(slot, std::move(v));
        return SlotSource{slot, nullptr, k_invalid_type};
    }

    /**
     * @brief Resolves the links feeding one input into slot sources, inserting conversions.
     * @return Empty when nothing is linked, so the caller can fall back to the default.
     */
    std::vector<SlotSource> resolve_input_(
        const Graph& graph, const Node& node, const SocketDecl& in,
        const std::unordered_map<NodeId, std::vector<SlotSource>>& node_outputs,
        const std::unordered_map<NodeId, NodeDeclaration>& node_decls) {
        std::vector<SlotSource> out;
        SocketRef target{node.id, in.identifier};
        for (const Link* l : graph.links_into(target)) {
            auto outs_it = node_outputs.find(l->from.node);
            auto decl_it = node_decls.find(l->from.node);
            if (outs_it == node_outputs.end() || decl_it == node_decls.end()) continue;
            int oi = decl_it->second.find_output(l->from.socket);
            if (oi < 0 || static_cast<size_t>(oi) >= outs_it->second.size()) {
                diag_.add(Severity::Error, node.id, node.type,
                          "link reads output '" + l->from.socket + "' which does not exist");
                continue;
            }
            SlotSource src = outs_it->second[oi];
            if (!src.valid()) continue;

            TypeId from_type = decl_it->second.outputs[oi].type;
            if (from_type != in.type && in.type != k_invalid_type) {
                auto* fn = TypeRegistry::instance().conversion(from_type, in.type);
                if (fn == nullptr) {
                    const auto& reg = TypeRegistry::instance();
                    diag_.add(Severity::Error, node.id, node.type,
                              "cannot connect " + reg.name_of(from_type) + " to " +
                              reg.name_of(in.type) + " on '" + in.identifier + "'");
                    continue;
                }
                // A conversion already carried by the source would be overwritten, so it is
                // composed by converting at the point of use instead: the source slot holds the
                // producer's own type and this binding converts from it.
                src.convert    = fn;
                src.convert_to = in.type;
            }
            out.push_back(src);
            if (!in.multi_input()) break;   // A plain input takes the first link only.
        }
        return out;
    }

    /// @brief Kahn order over the authored nodes. Nodes in a cycle are dropped (and reported).
    std::vector<NodeId> topological_nodes_(const Graph& graph) {
        std::unordered_map<NodeId, int> in_degree;
        std::unordered_map<NodeId, std::vector<NodeId>> consumers;
        for (const auto& n : graph.nodes()) in_degree[n.id] = 0;
        for (const auto& l : graph.links()) {
            if (!in_degree.count(l.to.node) || !in_degree.count(l.from.node)) continue;
            ++in_degree[l.to.node];
            consumers[l.from.node].push_back(l.to.node);
        }
        // Seed in declaration order so a graph's compiled output is deterministic rather than
        // dependent on the hash map's iteration order.
        std::vector<NodeId> ready;
        for (const auto& n : graph.nodes()) {
            if (in_degree[n.id] == 0) ready.push_back(n.id);
        }
        std::vector<NodeId> order;
        order.reserve(graph.nodes().size());
        size_t head = 0;
        while (head < ready.size()) {
            NodeId id = ready[head++];
            order.push_back(id);
            auto it = consumers.find(id);
            if (it == consumers.end()) continue;
            for (NodeId c : it->second) {
                if (--in_degree[c] == 0) ready.push_back(c);
            }
        }
        if (order.size() != graph.nodes().size()) {
            std::string ids;
            for (const auto& n : graph.nodes()) {
                if (in_degree[n.id] > 0) {
                    if (!ids.empty()) ids += ", ";
                    ids += "#" + std::to_string(n.id);
                }
            }
            diag_.error("the graph has a cycle through these nodes: " + ids);
        }
        return order;
    }

    bool push_ref_(const std::string& ref) {
        for (const auto& r : ref_stack_) {
            if (r == ref) return false;
        }
        ref_stack_.push_back(ref);
        return true;
    }
    void pop_ref_() {
        if (!ref_stack_.empty()) ref_stack_.pop_back();
    }

    /// @brief Drops every step that cannot reach a requested interface output.
    void prune_(CompiledGraph& out, const std::vector<std::string>& requested) {
        // slot -> producing step, and which of its outputs.
        std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> producer;
        for (uint32_t si = 0; si < out.steps_.size(); ++si) {
            for (uint32_t oi = 0; oi < out.steps_[si].out_slots.size(); ++oi) {
                producer[out.steps_[si].out_slots[oi]] = {si, oi};
            }
        }

        std::unordered_set<uint32_t> needed_slots;
        for (size_t i = 0; i < out.interface_output_sources_.size(); ++i) {
            if (!requested.empty()) {
                bool asked = false;
                for (const auto& r : requested) {
                    if (i < out.outputs_.size() && out.outputs_[i].identifier == r) { asked = true; break; }
                }
                if (!asked) continue;
            }
            if (out.interface_output_sources_[i].valid()) {
                needed_slots.insert(out.interface_output_sources_[i].slot);
            }
        }

        std::vector<bool> keep(out.steps_.size(), false);
        std::vector<uint32_t> work(needed_slots.begin(), needed_slots.end());
        std::vector<std::vector<bool>> needed_out(out.steps_.size());
        for (uint32_t si = 0; si < out.steps_.size(); ++si) {
            needed_out[si].assign(out.steps_[si].out_slots.size(), false);
        }

        while (!work.empty()) {
            uint32_t slot = work.back();
            work.pop_back();
            auto it = producer.find(slot);
            if (it == producer.end()) continue;   // A constant or an interface input.
            uint32_t si = it->second.first;
            needed_out[si][it->second.second] = true;
            if (keep[si]) continue;
            keep[si] = true;
            for (const auto& links : out.steps_[si].inputs) {
                for (const auto& src : links) {
                    if (src.valid() && needed_slots.insert(src.slot).second) work.push_back(src.slot);
                }
            }
        }

        std::vector<Step> kept;
        kept.reserve(out.steps_.size());
        for (uint32_t si = 0; si < out.steps_.size(); ++si) {
            if (!keep[si]) continue;
            Step s = std::move(out.steps_[si]);
            s.output_needed.assign(needed_out[si].begin(), needed_out[si].end());
            // An output nothing reads keeps its slot (writing it is harmless) but is flagged so a
            // node with an expensive secondary output can skip it.
            kept.push_back(std::move(s));
        }
        out.steps_ = std::move(kept);

        // Constants feeding only dropped steps are dropped too, so a pruned branch's defaults do
        // not sit in the slot table.
        std::unordered_set<uint32_t> live;
        for (const auto& s : out.steps_) {
            for (const auto& links : s.inputs) {
                for (const auto& src : links) {
                    if (src.valid()) live.insert(src.slot);
                }
            }
        }
        for (const auto& src : out.interface_output_sources_) {
            if (src.valid()) live.insert(src.slot);
        }
        std::vector<std::pair<uint32_t, Value>> kept_consts;
        for (auto& c : out.constants_) {
            if (live.count(c.first)) kept_consts.emplace_back(c.first, std::move(c.second));
        }
        out.constants_ = std::move(kept_consts);
    }

    /// @brief Topologically orders the surviving steps and fills `depends_on` / `reads`.
    void order_(CompiledGraph& out) {
        std::unordered_map<uint32_t, uint32_t> producer_step;
        for (uint32_t si = 0; si < out.steps_.size(); ++si) {
            for (uint32_t slot : out.steps_[si].out_slots) producer_step[slot] = si;
        }

        for (uint32_t si = 0; si < out.steps_.size(); ++si) {
            Step& s = out.steps_[si];
            std::unordered_set<uint32_t> deps;
            std::unordered_set<uint32_t> reads;
            for (const auto& links : s.inputs) {
                for (const auto& src : links) {
                    if (!src.valid()) continue;
                    reads.insert(src.slot);
                    auto it = producer_step.find(src.slot);
                    if (it != producer_step.end() && it->second != si) deps.insert(it->second);
                }
            }
            s.depends_on.assign(deps.begin(), deps.end());
            s.reads.assign(reads.begin(), reads.end());
            // Sorted so a compiled graph is byte-comparable between runs.
            insertion_sort_(s.depends_on);
            insertion_sort_(s.reads);
        }

        std::vector<uint32_t> in_degree(out.steps_.size(), 0);
        std::vector<std::vector<uint32_t>> consumers(out.steps_.size());
        for (uint32_t si = 0; si < out.steps_.size(); ++si) {
            in_degree[si] = static_cast<uint32_t>(out.steps_[si].depends_on.size());
            for (uint32_t d : out.steps_[si].depends_on) consumers[d].push_back(si);
        }
        std::vector<uint32_t> ready;
        for (uint32_t si = 0; si < out.steps_.size(); ++si) {
            if (in_degree[si] == 0) ready.push_back(si);
        }
        std::vector<uint32_t> order;
        order.reserve(out.steps_.size());
        size_t head = 0;
        while (head < ready.size()) {
            uint32_t si = ready[head++];
            order.push_back(si);
            for (uint32_t c : consumers[si]) {
                if (--in_degree[c] == 0) ready.push_back(c);
            }
        }
        if (order.size() != out.steps_.size()) {
            diag_.error("compiled graph still has a dependency cycle after flattening");
            return;
        }

        // Rebuild in topological order, remapping every step index.
        std::vector<uint32_t> new_index(out.steps_.size(), 0);
        for (uint32_t pos = 0; pos < order.size(); ++pos) new_index[order[pos]] = pos;
        std::vector<Step> reordered;
        reordered.reserve(order.size());
        for (uint32_t si : order) reordered.push_back(std::move(out.steps_[si]));
        for (auto& s : reordered) {
            for (auto& d : s.depends_on) d = new_index[d];
            insertion_sort_(s.depends_on);
        }
        out.steps_ = std::move(reordered);
    }

    /// @brief Counts, per slot, how many steps read it. See the @file note on release counting.
    void count_readers_(CompiledGraph& out) {
        out.slot_reader_counts_.assign(out.slot_count_, 0);
        for (const auto& s : out.steps_) {
            for (uint32_t slot : s.reads) {
                if (slot < out.slot_reader_counts_.size()) ++out.slot_reader_counts_[slot];
            }
        }
        // An interface output's slot gets a permanent extra reader so it survives to be read out.
        for (const auto& src : out.interface_output_sources_) {
            if (src.valid() && src.slot < out.slot_reader_counts_.size()) {
                ++out.slot_reader_counts_[src.slot];
            }
        }
    }

    /**
     * @brief Groups steps into chains, the unit of job submission.
     *
     * A chain grows while the current step is fusable and has exactly one consumer, and that
     * consumer is fusable and depends on nothing but the current step. Requiring a sole
     * dependency is conservative: fusing a step with two producers would make the chain wait for
     * both, which is correct but can delay work that could have started.
     */
    void fuse_(CompiledGraph& out) {
        const size_t n = out.steps_.size();
        std::vector<std::vector<uint32_t>> consumers(n);
        for (uint32_t si = 0; si < n; ++si) {
            for (uint32_t d : out.steps_[si].depends_on) consumers[d].push_back(si);
        }

        std::vector<bool> taken(n, false);
        std::vector<uint32_t> chain_of(n, 0);

        for (uint32_t si = 0; si < n; ++si) {
            if (taken[si]) continue;
            Chain chain;
            uint32_t current = si;
            for (;;) {
                taken[current] = true;
                chain_of[current] = static_cast<uint32_t>(out.chains_.size());
                chain.steps.push_back(current);

                const Step& cs = out.steps_[current];
                if (cs.type == nullptr || !is_fusable(cs.type->cost)) break;
                if (consumers[current].size() != 1) break;
                uint32_t next = consumers[current].front();
                if (taken[next]) break;
                const Step& ns = out.steps_[next];
                if (ns.type == nullptr || !is_fusable(ns.type->cost)) break;
                if (ns.depends_on.size() != 1 || ns.depends_on.front() != current) break;
                current = next;
            }
            out.chains_.push_back(std::move(chain));
        }

        // A chain depends on every chain producing a step it needs from outside itself.
        for (uint32_t ci = 0; ci < out.chains_.size(); ++ci) {
            std::unordered_set<uint32_t> deps;
            std::unordered_set<uint32_t> inside(out.chains_[ci].steps.begin(), out.chains_[ci].steps.end());
            for (uint32_t si : out.chains_[ci].steps) {
                for (uint32_t d : out.steps_[si].depends_on) {
                    if (inside.count(d)) continue;
                    deps.insert(chain_of[d]);
                }
            }
            out.chains_[ci].depends_on.assign(deps.begin(), deps.end());
            insertion_sort_(out.chains_[ci].depends_on);
        }
    }

    static void insertion_sort_(std::vector<uint32_t>& v) {
        for (size_t i = 1; i < v.size(); ++i) {
            for (size_t j = i; j > 0 && v[j] < v[j - 1]; --j) std::swap(v[j], v[j - 1]);
        }
    }

    GroupLoader              loader_;
    Diagnostics&             diag_;
    std::vector<Step>        steps_;
    std::vector<std::pair<uint32_t, Value>> constants_;
    std::vector<std::string> ref_stack_;
    uint32_t                 next_slot_ = 0;
};

/**
 * @brief Compiles `graph` into an evaluation-ready form.
 *
 * @param graph     The authored graph.
 * @param load_group Resolves a Group node's `group_ref`. May be null when no groups are used.
 * @param requested Interface output identifiers to keep; empty keeps all of them.
 * @param out       Filled with anything that went wrong. Check `out.ok()`.
 */
inline CompiledGraph compile(const Graph& graph,
                             const GroupLoader& load_group,
                             const std::vector<std::string>& requested,
                             Diagnostics& out) {
    GraphCompiler compiler(load_group, out);
    return compiler.run(graph, requested);
}

/// @brief Compiles keeping every interface output, with no group support.
inline CompiledGraph compile(const Graph& graph, Diagnostics& out) {
    return compile(graph, GroupLoader(), {}, out);
}

} // namespace nodegraph
} // namespace coopa

#endif // COOPA_NODEGRAPH_COMPILE_H
