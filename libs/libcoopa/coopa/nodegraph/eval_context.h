/**
 * @file eval_context.h
 * @brief `EvalContext` -- the only thing a node body is given.
 *
 * A node type's `evaluate` function receives nothing but this. It cannot reach the graph, the
 * compiler, the slot table, the owning scene or whatever drove the evaluation; it reads its
 * declared inputs, writes its declared outputs, and optionally uses the job engine for internal
 * parallelism. That narrowness is the point: it is what lets the same node body run under the
 * inline evaluator and the job-parallel one with no branch of its own, and what makes a node
 * unit-testable without constructing anything.
 *
 * ### `jobs()` may be null, and a node must cope
 *
 * This mirrors `coopa::scene::FrameContext::jobs`: a null engine means "do the work on the calling
 * thread, spawn nothing". A node that wants internal parallelism writes
 *
 * @code
 * if (auto* j = ctx.jobs()) j->parallel_for_blocking(n, 0, body);
 * else                      for (size_t i = 0; i < n; ++i) body(i, i + 1);
 * @endcode
 *
 * Calling `parallel_for` from inside a running node is safe with this job engine: a `wait_for()`
 * on a worker participates as that worker, so a node that waits on work it submitted cannot
 * deadlock.
 *
 * ### `user_data()` is the consumer's bake context
 *
 * The library has no idea what a consumer needs on hand while evaluating -- a terrain snapshot, a
 * material table, a random seed. Rather than inventing an abstraction for it, the evaluation
 * request carries one opaque pointer through to every node. A consumer casts it to its own type.
 */

#ifndef COOPA_NODEGRAPH_EVAL_CONTEXT_H
#define COOPA_NODEGRAPH_EVAL_CONTEXT_H

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <coopa/nodegraph/declaration.h>
#include <coopa/nodegraph/diagnostics.h>
#include <coopa/nodegraph/value.h>

namespace coopa {
namespace job {
class JobEngine;
}
}

namespace coopa {
namespace nodegraph {

/**
 * @struct InputView
 * @brief One input socket's resolved contents.
 *
 * A plain input has `count == 1`. A multi-input has one entry per connected link, in link order.
 * An unconnected input points at its declared default, so a node never has to test for absence.
 */
struct InputView {
    const Value* values = nullptr;
    uint32_t     count = 0;

    const Value& first() const { return count > 0 ? values[0] : empty_(); }
    const Value& at(uint32_t i) const { return i < count ? values[i] : empty_(); }

private:
    static const Value& empty_() {
        static const Value nothing;
        return nothing;
    }
};

/// @brief How the evaluator should obtain a lazily-requested input. Set up by the evaluator.
using LazyRequestFn = const Value* (*)(void* state, uint32_t input_index);

/**
 * @class EvalContext
 * @brief Per-node evaluation state, assembled by the evaluator immediately before the call.
 */
class EvalContext {
public:
    EvalContext() = default;

    /// @brief Built by the evaluator; a node never constructs one.
    EvalContext(const NodeDeclaration* decl, const PropertyBag* props,
                const InputView* inputs, Value* outputs, const bool* output_needed,
                coopa::job::JobEngine* jobs, Diagnostics* diagnostics,
                const std::atomic<bool>* cancel, void* user_data,
                uint32_t node_id, std::string_view node_label)
        : decl_(decl), props_(props), inputs_(inputs), outputs_(outputs),
          output_needed_(output_needed), jobs_(jobs), diagnostics_(diagnostics),
          cancel_(cancel), user_data_(user_data), node_id_(node_id), node_label_(node_label) {}

    // --- Inputs ---

    /// @brief The resolved view of input `id`. An unknown id yields an empty view.
    const InputView& input(std::string_view id) const {
        int i = decl_ ? decl_->find_input(id) : -1;
        if (i < 0 || inputs_ == nullptr) return empty_view_();
        return inputs_[i];
    }

    /// @brief Input `id`'s first value.
    const Value& input_value(std::string_view id) const { return input(id).first(); }

    /// @brief Input `id` read as `T`, falling back when unconnected or mistyped.
    template <typename T>
    T get(std::string_view id, T fallback = T{}) const {
        return input_value(id).as<T>(fallback);
    }

    /// @brief How many links feed a multi-input.
    uint32_t count(std::string_view id) const { return input(id).count; }

    // --- Outputs ---

    /// @brief Writes output `id`. A write to an unknown id is dropped with a warning.
    void set_output(std::string_view id, Value v) {
        int i = decl_ ? decl_->find_output(id) : -1;
        if (i < 0 || outputs_ == nullptr) {
            warn("wrote unknown output '" + std::string(id) + "'");
            return;
        }
        outputs_[i] = std::move(v);
    }

    /// @brief Writes output `id` from a raw `T`, using the socket's declared type.
    template <typename T>
    void set(std::string_view id, T&& v) {
        int i = decl_ ? decl_->find_output(id) : -1;
        if (i < 0 || outputs_ == nullptr) {
            warn("wrote unknown output '" + std::string(id) + "'");
            return;
        }
        outputs_[i] = Value::make(decl_->outputs[i].type, std::forward<T>(v));
    }

    /**
     * @brief Whether anything consumes output `id`.
     *
     * The compiler already prunes nodes no output can reach, but a node with several outputs is
     * kept if *any* of them is used -- so a node whose second output is expensive should skip it
     * when this returns false.
     */
    bool is_output_needed(std::string_view id) const {
        int i = decl_ ? decl_->find_output(id) : -1;
        if (i < 0 || output_needed_ == nullptr) return true;
        return output_needed_[i];
    }

    // --- Settings ---

    const PropertyBag& props() const { return props_ ? *props_ : empty_props_(); }
    const NodeDeclaration& declaration() const { return decl_ ? *decl_ : empty_decl_(); }

    // --- Lazy inputs ---

    /**
     * @brief Evaluates a `Lazy` input on demand and returns it.
     *
     * For a node that must not pay for a branch it does not take (a Switch) or that drives a
     * bounded loop (a Repeat zone). An input not declared `Lazy` is already resolved, and
     * requesting it simply returns it.
     */
    const Value& request(std::string_view id) {
        int i = decl_ ? decl_->find_input(id) : -1;
        if (i < 0) return empty_value_();
        if (lazy_fn_ != nullptr) {
            if (const Value* v = lazy_fn_(lazy_state_, static_cast<uint32_t>(i))) return *v;
        }
        return input(id).first();
    }

    /// @brief Installed by the evaluator when the compiled graph has lazy inputs.
    void set_lazy_source(LazyRequestFn fn, void* state) { lazy_fn_ = fn; lazy_state_ = state; }

    // --- Parallelism, cancellation, consumer context ---

    /// @brief The job engine, or nullptr meaning "run inline". See the @file note.
    coopa::job::JobEngine* jobs() const { return jobs_; }

    bool is_cancelled() const {
        return cancel_ != nullptr && cancel_->load(std::memory_order_relaxed);
    }

    /// @brief The opaque per-evaluation pointer from EvalRequest::user_data.
    void* user_data() const { return user_data_; }

    /// @brief The opaque pointer, cast. The consumer is responsible for the cast being right.
    template <typename T>
    T* user_data_as() const { return static_cast<T*>(user_data_); }

    // --- Diagnostics ---

    /// @brief Marks this node failed. Its outputs stay empty and downstream sees defaults.
    void error(std::string message) {
        if (diagnostics_) diagnostics_->add(Severity::Error, node_id_, std::string(node_label_), std::move(message));
    }

    void warn(std::string message) {
        if (diagnostics_) diagnostics_->add(Severity::Warning, node_id_, std::string(node_label_), std::move(message));
    }

    uint32_t         node_id() const { return node_id_; }
    std::string_view node_label() const { return node_label_; }

private:
    static const InputView& empty_view_() {
        static const InputView nothing;
        return nothing;
    }
    static const Value& empty_value_() {
        static const Value nothing;
        return nothing;
    }
    static const PropertyBag& empty_props_() {
        static const PropertyBag nothing;
        return nothing;
    }
    static const NodeDeclaration& empty_decl_() {
        static const NodeDeclaration nothing;
        return nothing;
    }

    const NodeDeclaration*   decl_ = nullptr;
    const PropertyBag*       props_ = nullptr;
    const InputView*         inputs_ = nullptr;
    Value*                   outputs_ = nullptr;
    const bool*              output_needed_ = nullptr;
    coopa::job::JobEngine*   jobs_ = nullptr;
    Diagnostics*             diagnostics_ = nullptr;
    const std::atomic<bool>* cancel_ = nullptr;
    void*                    user_data_ = nullptr;
    uint32_t                 node_id_ = 0;
    std::string_view         node_label_;
    LazyRequestFn            lazy_fn_ = nullptr;
    void*                    lazy_state_ = nullptr;
};

} // namespace nodegraph
} // namespace coopa

#endif // COOPA_NODEGRAPH_EVAL_CONTEXT_H
