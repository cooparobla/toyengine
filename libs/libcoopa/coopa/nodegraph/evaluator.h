/**
 * @file evaluator.h
 * @brief `evaluate()` -- runs a CompiledGraph, inline or spread over the job engine.
 *
 * ### Two paths, one set of arithmetic
 *
 * `EvalRequest::jobs == nullptr` runs every chain on the calling thread in topological order;
 * a non-null engine submits one job per chain whose dependencies are the handles of the chains it
 * needs. Both paths share `run_step_()` and both share the slot release counting, so the two are
 * not two implementations -- they are one implementation with a different driver. That is what
 * makes "the parallel result equals the inline result" a meaningful property rather than a
 * coincidence, and it is why the null-engine case is a first-class path rather than a fallback.
 *
 * The parallel path leans entirely on `coopa::job::DependencyGraph`: it has unbounded fan-in,
 * resolves dependencies event-driven with no polling, and fires a parked job the instant its last
 * dependency completes. There is no scheduling logic here at all, only handle bookkeeping.
 *
 * ### Handle-pool exhaustion is a correctness hazard, not a performance one
 *
 * `JobEngine::create_handle()` returns an **invalid** handle when its fixed pool (4096 slots,
 * shared process-wide with every other subsystem) is empty, and an invalid handle reports as
 * *already complete*. Submitting a chain whose dependency is an invalid handle would therefore run
 * it immediately, before its inputs exist -- silently wrong results with no error anywhere.
 *
 * So every handle is created **before** anything is submitted, and if any one of them comes back
 * invalid the whole evaluation falls back to the inline path. Correct, merely serial, and the
 * failure is reported as a warning rather than being invisible.
 *
 * ### A node that throws does not take down a worker
 *
 * Each step runs inside a try/catch. A throwing node is recorded as an error against its own id
 * and leaves its outputs empty, so downstream nodes see defaults and the evaluation still
 * produces a result a tool can display next to the message.
 */

#ifndef COOPA_NODEGRAPH_EVALUATOR_H
#define COOPA_NODEGRAPH_EVALUATOR_H

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <coopa/job/engine.h>
#include <coopa/job/handle.h>
#include <coopa/job/parallel_for.h>
#include <coopa/nodegraph/compile.h>
#include <coopa/nodegraph/diagnostics.h>
#include <coopa/nodegraph/eval_context.h>
#include <coopa/nodegraph/value.h>

namespace coopa {
namespace nodegraph {

/**
 * @struct EvalRequest
 * @brief Everything one evaluation needs.
 */
struct EvalRequest {
    const CompiledGraph* graph = nullptr;
    /// @brief Positional, matching `graph->inputs()`. Short or empty falls back to the defaults.
    std::span<const Value> inputs;
    /// @brief Passed straight through to every node as EvalContext::user_data(). Opaque here.
    void*                  user_data = nullptr;
    /// @brief Null means "run inline on this thread". See the @file doc.
    coopa::job::JobEngine* jobs = nullptr;
    /// @brief Priority for the submitted chains. Low keeps a bake off a frame's critical path.
    coopa::job::Priority   priority = coopa::job::Priority::Low;
    /// @brief Polled by nodes via EvalContext::is_cancelled() and between chains.
    const std::atomic<bool>* cancel = nullptr;
    /// @brief Job type for diagnostics / thread dedication.
    coopa::job::JobType    job_type = 0;
};

/**
 * @struct EvalResult
 * @brief What one evaluation produced.
 */
struct EvalResult {
    /// @brief One per `graph->outputs()`, in order. Empty where a node errored.
    std::vector<Value> outputs;
    Diagnostics        diagnostics;
    bool               ok = false;
    /// @brief How many jobs were submitted. 0 for the inline path; the fusion test reads this.
    uint32_t           chains_submitted = 0;
};

namespace detail {

/**
 * @class Evaluation
 * @brief One in-flight evaluation's mutable state.
 *
 * Held by `shared_ptr` so a submitted chain job can outlive the stack frame that created it
 * without the slot table moving. Slots are never resized after `prepare()`, which is what lets a
 * step point an `InputView` straight at a slot instead of copying it.
 */
class Evaluation {
public:
    Evaluation(const CompiledGraph& graph, const EvalRequest& request, Diagnostics& diag)
        : graph_(graph), request_(request), diag_(diag) {}

    /// @brief Fills the slot table with constants and the request's interface inputs.
    void prepare() {
        slots_.resize(graph_.slot_count());
        remaining_.reserve(graph_.slot_count());
        for (uint32_t i = 0; i < graph_.slot_count(); ++i) {
            uint32_t n = i < graph_.slot_reader_counts().size() ? graph_.slot_reader_counts()[i] : 0;
            remaining_.push_back(std::make_unique<std::atomic<uint32_t>>(n));
        }
        for (const auto& [slot, value] : graph_.constants()) {
            if (slot < slots_.size()) slots_[slot] = value;
        }
        const auto& in_slots = graph_.interface_input_slots();
        for (size_t i = 0; i < in_slots.size(); ++i) {
            if (in_slots[i] >= slots_.size()) continue;
            if (i < request_.inputs.size() && !request_.inputs[i].empty()) {
                // An input of the wrong type is converted where possible rather than dropped,
                // because a consumer binding a Float to a Vector parameter is ordinary.
                const Value& given = request_.inputs[i];
                TypeId wanted = i < graph_.inputs().size() ? graph_.inputs()[i].type : given.type();
                if (given.type() == wanted) {
                    slots_[in_slots[i]] = given;
                } else {
                    Value converted = given.converted_to(wanted);
                    if (converted.empty()) {
                        const auto& reg = TypeRegistry::instance();
                        diag_.warn("input '" + graph_.inputs()[i].identifier + "' was given a " +
                                   reg.name_of(given.type()) + " but wants a " + reg.name_of(wanted) +
                                   "; using the default");
                        slots_[in_slots[i]] = graph_.inputs()[i].default_value;
                    } else {
                        slots_[in_slots[i]] = std::move(converted);
                    }
                }
            } else if (i < graph_.inputs().size()) {
                const InterfaceSocket& is = graph_.inputs()[i];
                slots_[in_slots[i]] = is.default_value.empty() ? Value::make_default(is.type)
                                                               : is.default_value;
            }
        }
    }

    /// @brief Runs one chain's steps in order. Safe to call concurrently for distinct chains.
    void run_chain(uint32_t chain_index) {
        if (chain_index >= graph_.chains().size()) return;
        for (uint32_t si : graph_.chains()[chain_index].steps) {
            if (cancelled()) return;
            run_step_(si);
        }
    }

    /// @brief Reads the interface outputs out of the slot table.
    std::vector<Value> collect_outputs() const {
        std::vector<Value> out;
        out.reserve(graph_.interface_output_sources().size());
        for (const auto& src : graph_.interface_output_sources()) {
            if (!src.valid() || src.slot >= slots_.size()) { out.emplace_back(); continue; }
            if (src.convert != nullptr) {
                Value converted = Value::make_default(src.convert_to);
                if (!converted.empty() && slots_[src.slot].data() != nullptr) {
                    src.convert(slots_[src.slot].data(), converted.mutable_data());
                }
                out.push_back(std::move(converted));
            } else {
                out.push_back(slots_[src.slot]);
            }
        }
        return out;
    }

    bool cancelled() const {
        return request_.cancel != nullptr && request_.cancel->load(std::memory_order_relaxed);
    }

private:
    /// @brief Assembles the context for one step, calls it, then releases the slots it read.
    void run_step_(uint32_t si) {
        const Step& step = graph_.steps()[si];
        if (step.type == nullptr || step.type->evaluate == nullptr) {
            release_reads_(step);
            return;
        }

        const size_t in_count  = step.decl.inputs.size();
        const size_t out_count = step.decl.outputs.size();

        std::vector<InputView> views(in_count);
        // Values that had to be materialized (a conversion, or a multi-input gathering scattered
        // slots). A single unconverted input points straight at its slot and costs nothing.
        std::vector<std::vector<Value>> scratch(in_count);

        for (size_t i = 0; i < in_count; ++i) {
            const auto& links = step.inputs[i];
            if (links.size() == 1 && links[0].convert == nullptr && links[0].valid() &&
                links[0].slot < slots_.size()) {
                views[i].values = &slots_[links[0].slot];
                views[i].count  = 1;
                continue;
            }
            scratch[i].reserve(links.size());
            for (const auto& src : links) {
                if (!src.valid() || src.slot >= slots_.size()) { scratch[i].emplace_back(); continue; }
                if (src.convert != nullptr) {
                    Value converted = Value::make_default(src.convert_to);
                    if (!converted.empty() && slots_[src.slot].data() != nullptr) {
                        src.convert(slots_[src.slot].data(), converted.mutable_data());
                    }
                    scratch[i].push_back(std::move(converted));
                } else {
                    scratch[i].push_back(slots_[src.slot]);
                }
            }
            views[i].values = scratch[i].data();
            views[i].count  = static_cast<uint32_t>(scratch[i].size());
        }

        std::vector<Value> outputs(out_count);
        std::vector<char>  needed(out_count, 1);
        for (size_t i = 0; i < out_count && i < step.output_needed.size(); ++i) {
            needed[i] = step.output_needed[i] ? 1 : 0;
        }
        // EvalContext wants `const bool*`; vector<bool> has no contiguous storage to point at.
        std::unique_ptr<bool[]> needed_flat(new bool[out_count ? out_count : 1]);
        for (size_t i = 0; i < out_count; ++i) needed_flat[i] = needed[i] != 0;

        EvalContext ctx(&step.decl, &step.props, views.data(), outputs.data(), needed_flat.get(),
                        request_.jobs, &diag_, request_.cancel, request_.user_data,
                        step.source_id, step.label);

        try {
            step.type->evaluate(ctx);
        } catch (const std::exception& e) {
            diag_.add(Severity::Error, step.source_id, step.label,
                      std::string("node threw: ") + e.what());
            for (auto& v : outputs) v.reset();
        } catch (...) {
            diag_.add(Severity::Error, step.source_id, step.label, "node threw an unknown exception");
            for (auto& v : outputs) v.reset();
        }

        for (size_t i = 0; i < out_count; ++i) {
            uint32_t slot = i < step.out_slots.size() ? step.out_slots[i] : k_no_slot;
            if (slot != k_no_slot && slot < slots_.size()) slots_[slot] = std::move(outputs[i]);
        }

        release_reads_(step);
    }

    /**
     * @brief Drops this step's claim on every slot it read, clearing any that reached zero.
     *
     * Reference-counted rather than positional, because under the parallel evaluator the last
     * step to *finish* reading a slot is not the last one in topological order.
     */
    void release_reads_(const Step& step) {
        for (uint32_t slot : step.reads) {
            if (slot >= remaining_.size()) continue;
            if (remaining_[slot]->fetch_sub(1, std::memory_order_acq_rel) == 1) {
                slots_[slot].reset();
            }
        }
    }

    const CompiledGraph& graph_;
    const EvalRequest&   request_;
    Diagnostics&         diag_;
    std::vector<Value>   slots_;
    /// @brief Per slot, how many readers have yet to run. unique_ptr because atomics do not move.
    std::vector<std::unique_ptr<std::atomic<uint32_t>>> remaining_;
};

} // namespace detail

/**
 * @brief Evaluates a compiled graph.
 *
 * Never throws. A graph that failed to compile, a null request, a node that threw and a cancelled
 * run all come back as a result whose `diagnostics` explain it and whose `ok` is false.
 */
inline EvalResult evaluate(const EvalRequest& request) {
    EvalResult result;
    if (request.graph == nullptr) {
        result.diagnostics.error("evaluate() called with no compiled graph");
        return result;
    }
    const CompiledGraph& graph = *request.graph;

    detail::Evaluation eval(graph, request, result.diagnostics);
    eval.prepare();

    const auto& chains = graph.chains();

    // --- Inline path ---
    // Chains are already in topological order, so running them in index order satisfies every
    // dependency without any bookkeeping.
    auto run_inline = [&]() {
        for (uint32_t ci = 0; ci < chains.size(); ++ci) {
            if (eval.cancelled()) break;
            eval.run_chain(ci);
        }
    };

    if (request.jobs == nullptr || chains.size() <= 1) {
        run_inline();
    } else {
        coopa::job::JobEngine& engine = *request.jobs;

        // Every handle up front, so an exhausted pool is caught before anything is submitted.
        // See the @file doc: an invalid handle reads as complete and would corrupt ordering.
        std::vector<coopa::job::JobHandle> handles;
        handles.reserve(chains.size());
        bool all_valid = true;
        for (size_t i = 0; i < chains.size(); ++i) {
            coopa::job::JobHandle h = engine.create_handle();
            if (!h.is_valid()) { all_valid = false; break; }
            handles.push_back(h);
        }

        if (!all_valid) {
            for (const auto& h : handles) h.close();
            result.diagnostics.warn(
                "job handle pool exhausted (" + std::to_string(chains.size()) +
                " chains wanted); evaluating inline instead");
            run_inline();
        } else {
            // Captured by pointer, not by shared_ptr: every handle is waited on below before
            // this frame returns, so `eval` strictly outlives every job that touches it.
            detail::Evaluation* state = &eval;
            std::vector<coopa::job::JobHandle> deps;
            for (uint32_t ci = 0; ci < chains.size(); ++ci) {
                deps.clear();
                deps.reserve(chains[ci].depends_on.size());
                for (uint32_t d : chains[ci].depends_on) {
                    if (d < handles.size()) deps.push_back(handles[d]);
                }
                engine.submit([state, ci]() { state->run_chain(ci); },
                              request.job_type, handles[ci],
                              deps.empty() ? nullptr : deps.data(),
                              static_cast<uint32_t>(deps.size()),
                              request.priority);
            }
            result.chains_submitted = static_cast<uint32_t>(chains.size());

            // Waiting at Normal keeps the waiting thread from picking up unrelated Low background
            // work, but the chains themselves run at `priority`, so the floor must not exceed it.
            const coopa::job::Priority floor =
                request.priority == coopa::job::Priority::Low ? coopa::job::Priority::Low
                                                              : coopa::job::Priority::Normal;
            for (const auto& h : handles) engine.wait_for(h, floor);
            for (const auto& h : handles) h.close();
        }
    }

    if (eval.cancelled()) {
        result.diagnostics.warn("evaluation was cancelled");
        result.ok = false;
        return result;
    }

    result.outputs = eval.collect_outputs();
    result.ok      = result.diagnostics.ok();
    return result;
}

/// @brief Compiles and evaluates in one call. For a test or a one-shot; prefer caching the compile.
inline EvalResult compile_and_evaluate(const Graph& graph,
                                       std::span<const Value> inputs = {},
                                       coopa::job::JobEngine* jobs = nullptr,
                                       void* user_data = nullptr) {
    EvalResult result;
    CompiledGraph compiled = compile(graph, result.diagnostics);
    if (!result.diagnostics.ok()) return result;
    EvalRequest request;
    request.graph     = &compiled;
    request.inputs    = inputs;
    request.jobs      = jobs;
    request.user_data = user_data;
    EvalResult inner = evaluate(request);
    // The compile diagnostics are already in `result`; fold the evaluation's in after them.
    for (const auto& e : inner.diagnostics.entries()) {
        result.diagnostics.add(e.severity, e.node, e.node_label, e.message);
    }
    result.outputs          = std::move(inner.outputs);
    result.chains_submitted = inner.chains_submitted;
    result.ok               = result.diagnostics.ok();
    return result;
}

} // namespace nodegraph
} // namespace coopa

#endif // COOPA_NODEGRAPH_EVALUATOR_H
