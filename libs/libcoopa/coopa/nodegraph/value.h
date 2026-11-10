/**
 * @file value.h
 * @brief `Value` -- one socket's contents, type-erased through TypeInfo.
 *
 * ### Two storage strategies, and why
 *
 * A value either lives in an inline buffer or in a reference-counted block:
 *
 * - **Inline** (`size <= k_inline_bytes && align <= k_inline_align`) covers the scalars and
 *   small vectors that make up the overwhelming majority of sockets. No allocation, and a copy
 *   is one `copy_construct` call.
 * - **Shared** covers everything else. A copy bumps a refcount, so handing a value to four
 *   consumers costs four pointer copies rather than four deep copies. Writing through
 *   `mutable_data()` clones first if the block is shared, which is ordinary copy-on-write.
 *
 * The inline budget is deliberately small. It is sized for `vec3`/`vec4`/`quat`/handles, not for
 * a consumer's aggregates -- those are expected to be cheap handles themselves (a geometry bundle
 * should be a refcounted COW object, so passing it through a socket moves 16 bytes). If a
 * consumer's main payload type does not fit that description, every socket copy allocates and the
 * fix is in the consumer's type, not here.
 *
 * ### Accessors never throw
 *
 * `as<T>(fallback)` returns the fallback on a type mismatch, matching
 * `coopa::event::EventValue`'s contract: graph data is authored, so a mismatch is a condition to
 * report, not to crash on. `get<T>()` is the asserting form for a node body that has already
 * matched its own declaration and would rather fail loudly in a debug build.
 *
 * `Value` itself is **not** thread-safe to mutate, but the shared block's refcount is atomic, so
 * copying one `Value` on several threads at once is safe. That is exactly what the evaluator
 * does when it fans one slot out to several nodes.
 */

#ifndef COOPA_NODEGRAPH_VALUE_H
#define COOPA_NODEGRAPH_VALUE_H

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

#include <coopa/nodegraph/type_registry.h>

namespace coopa {
namespace nodegraph {

namespace detail {

/**
 * @struct ValueBlock
 * @brief Heap storage for a value too large (or too over-aligned) to live inline.
 *
 * `data` is a separate aligned allocation rather than a trailing array so the block header's own
 * layout never has to satisfy the payload's alignment.
 */
struct ValueBlock {
    std::atomic<int32_t> refs{1};
    TypeId               type = k_invalid_type;
    void*                data = nullptr;
};

inline void* aligned_alloc_(size_t size, size_t align) {
    if (align < alignof(void*)) align = alignof(void*);
    // operator new's aligned overload needs a size that is a multiple of the alignment.
    size_t rounded = ((size + align - 1) / align) * align;
    return ::operator new(rounded, std::align_val_t(align));
}

inline void aligned_free_(void* p, size_t align) {
    if (align < alignof(void*)) align = alignof(void*);
    ::operator delete(p, std::align_val_t(align));
}

} // namespace detail

/**
 * @class Value
 * @brief A socket value of a runtime-registered type.
 */
class Value {
public:
    /// @brief Inline capacity. Sized for a vec4/quat, not for a consumer's aggregates.
    static constexpr size_t k_inline_bytes = 32;
    /// @brief Inline alignment ceiling. A more-aligned type goes to a block.
    static constexpr size_t k_inline_align = 16;

    Value() = default;

    /**
     * @brief Builds a value of registered type `id` from `v`.
     *
     * @tparam T Must be the type registered under `id`; this is not checked, because the whole
     *         point of the registry is that this header cannot name the consumer's types. Passing
     *         a mismatched pair is a programming error that corrupts the value.
     */
    template <typename T>
    static Value make(TypeId id, T&& v) {
        using Bare = std::decay_t<T>;
        Value out;
        const TypeInfo& info = TypeRegistry::instance().info(id);
        if (info.size == 0) return out;   // Unregistered id: stay empty rather than scribble.
        assert(info.size == sizeof(Bare) && "Value::make: type does not match the registered id");
        out.type_ = id;
        // `if constexpr` rather than a runtime branch on `fits_inline`: a runtime branch makes
        // the compiler emit a placement-new of `Bare` into `buf_` even for a type far too large
        // for it, which is a warning at best and is only unreachable by luck. Both sides compute
        // the same predicate from the same type, so they cannot disagree.
        if constexpr (sizeof(Bare) <= k_inline_bytes && alignof(Bare) <= k_inline_align) {
            assert(info.fits_inline(k_inline_bytes, k_inline_align) &&
                   "TypeInfo disagrees with the type about fitting inline");
            out.kind_ = Kind::Inline;
            new (out.buf_) Bare(std::forward<T>(v));
        } else {
            out.kind_  = Kind::Shared;
            out.block_ = new detail::ValueBlock();
            out.block_->type = id;
            out.block_->data = detail::aligned_alloc_(info.size, info.align);
            new (out.block_->data) Bare(std::forward<T>(v));
        }
        return out;
    }

    /// @brief A default-constructed value of registered type `id`. Empty if `id` is unregistered.
    static Value make_default(TypeId id) {
        Value out;
        const TypeInfo& info = TypeRegistry::instance().info(id);
        if (info.size == 0 || info.default_construct == nullptr) return out;
        out.type_ = id;
        if (info.fits_inline(k_inline_bytes, k_inline_align)) {
            out.kind_ = Kind::Inline;
            info.default_construct(out.buf_);
        } else {
            out.kind_  = Kind::Shared;
            out.block_ = new detail::ValueBlock();
            out.block_->type = id;
            out.block_->data = detail::aligned_alloc_(info.size, info.align);
            info.default_construct(out.block_->data);
        }
        return out;
    }

    Value(const Value& other) { copy_from_(other); }

    Value& operator=(const Value& other) {
        if (this == &other) return *this;
        reset();
        copy_from_(other);
        return *this;
    }

    Value(Value&& other) noexcept { move_from_(other); }

    Value& operator=(Value&& other) noexcept {
        if (this == &other) return *this;
        reset();
        move_from_(other);
        return *this;
    }

    ~Value() { reset(); }

    /// @brief Destroys the contents and becomes empty.
    void reset() {
        if (kind_ == Kind::Inline) {
            const TypeInfo& info = TypeRegistry::instance().info(type_);
            if (info.destroy) info.destroy(buf_);
        } else if (kind_ == Kind::Shared && block_ != nullptr) {
            if (block_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                const TypeInfo& info = TypeRegistry::instance().info(block_->type);
                if (block_->data) {
                    if (info.destroy) info.destroy(block_->data);
                    detail::aligned_free_(block_->data, info.align ? info.align : alignof(void*));
                }
                delete block_;
            }
        }
        kind_  = Kind::Empty;
        type_  = k_invalid_type;
        block_ = nullptr;
    }

    TypeId type() const { return type_; }
    bool   empty() const { return kind_ == Kind::Empty; }
    /// @brief Whether the payload is shared with at least one other Value.
    bool   shared() const {
        return kind_ == Kind::Shared && block_ && block_->refs.load(std::memory_order_acquire) > 1;
    }

    /// @brief Read-only payload pointer, or nullptr when empty.
    const void* data() const {
        if (kind_ == Kind::Inline) return buf_;
        if (kind_ == Kind::Shared && block_) return block_->data;
        return nullptr;
    }

    /**
     * @brief Writable payload pointer, cloning first if the payload is shared.
     * @return nullptr when empty. After this returns, `shared()` is false.
     */
    void* mutable_data() {
        if (kind_ == Kind::Inline) return buf_;
        if (kind_ != Kind::Shared || block_ == nullptr) return nullptr;
        if (block_->refs.load(std::memory_order_acquire) > 1) clone_block_();
        return block_->data;
    }

    /// @brief Typed read; returns `fallback` when the type does not match. Never throws.
    template <typename T>
    const T& as(const T& fallback) const {
        const void* p = data();
        if (p == nullptr) return fallback;
        const TypeInfo& info = TypeRegistry::instance().info(type_);
        if (info.size != sizeof(T)) return fallback;
        return *static_cast<const T*>(p);
    }

    /// @brief Typed read, asserting the type matches. For a node body that matched its declaration.
    template <typename T>
    const T& get() const {
        const void* p = data();
        assert(p != nullptr && "Value::get on an empty value");
        assert(TypeRegistry::instance().info(type_).size == sizeof(T) && "Value::get type mismatch");
        return *static_cast<const T*>(p);
    }

    /// @brief Typed write access, cloning a shared payload first.
    template <typename T>
    T* get_mutable() {
        void* p = mutable_data();
        if (p == nullptr) return nullptr;
        if (TypeRegistry::instance().info(type_).size != sizeof(T)) return nullptr;
        return static_cast<T*>(p);
    }

    /// @brief Value equality via TypeInfo::equals. False when types differ or equality is absent.
    bool equals(const Value& other) const {
        if (type_ != other.type_) return false;
        if (empty() && other.empty()) return true;
        const TypeInfo& info = TypeRegistry::instance().info(type_);
        if (info.equals == nullptr) return false;
        const void* a = data();
        const void* b = other.data();
        if (a == nullptr || b == nullptr) return a == b;
        if (a == b) return true;   // Same shared block.
        return info.equals(a, b);
    }

    /**
     * @brief Folds this value into `seed`.
     *
     * A type with no `hash` hook folds in only its type id, which makes two different values of
     * that type hash the same. That is safe for change detection (it reads as "possibly changed"
     * and triggers extra work) but never as "unchanged", which is the direction that would be a
     * correctness bug.
     */
    void hash_into(uint64_t& seed) const {
        fold_(seed, static_cast<uint64_t>(type_));
        const TypeInfo& info = TypeRegistry::instance().info(type_);
        const void* p = data();
        if (p == nullptr) { fold_(seed, 0x9e3779b9u); return; }
        if (info.hash) { info.hash(p, seed); return; }
        fold_(seed, 0xfeedfaceu);   // Unknown contents: see the note above.
    }

    /// @brief A 64-bit digest of this value alone.
    uint64_t hash() const {
        uint64_t seed = 1469598103934665603ull;   // FNV-1a 64 offset basis, as AssetId uses.
        hash_into(seed);
        return seed;
    }

    /**
     * @brief Converts to `to`, returning an empty Value when no conversion is registered.
     *
     * Same-type returns a copy, which is what lets the compiler insert a conversion step
     * unconditionally where types differ and never where they agree.
     */
    Value converted_to(TypeId to) const {
        if (to == type_) return *this;
        if (empty()) return Value();
        auto* fn = TypeRegistry::instance().conversion(type_, to);
        if (fn == nullptr) return Value();
        Value out = make_default(to);
        if (out.empty()) return out;
        fn(data(), out.mutable_data());
        return out;
    }

private:
    enum class Kind : uint8_t { Empty, Inline, Shared };

    void copy_from_(const Value& other) {
        type_ = other.type_;
        kind_ = other.kind_;
        if (kind_ == Kind::Inline) {
            const TypeInfo& info = TypeRegistry::instance().info(type_);
            if (info.copy_construct) info.copy_construct(buf_, other.buf_);
            else kind_ = Kind::Empty;
        } else if (kind_ == Kind::Shared) {
            block_ = other.block_;
            if (block_) block_->refs.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void move_from_(Value& other) {
        type_ = other.type_;
        kind_ = other.kind_;
        if (kind_ == Kind::Inline) {
            const TypeInfo& info = TypeRegistry::instance().info(type_);
            if (info.move_construct) {
                info.move_construct(buf_, other.buf_);
                if (info.destroy) info.destroy(other.buf_);
            } else {
                kind_ = Kind::Empty;
            }
        } else if (kind_ == Kind::Shared) {
            block_ = other.block_;
        }
        other.kind_  = Kind::Empty;
        other.type_  = k_invalid_type;
        other.block_ = nullptr;
    }

    /// @brief Splits a shared block off into a private one. Caller must hold a Shared block.
    void clone_block_() {
        const TypeInfo& info = TypeRegistry::instance().info(block_->type);
        auto* fresh = new detail::ValueBlock();
        fresh->type = block_->type;
        fresh->data = detail::aligned_alloc_(info.size, info.align);
        if (info.copy_construct) info.copy_construct(fresh->data, block_->data);
        else if (info.default_construct) info.default_construct(fresh->data);
        if (block_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            // We were the last owner after all (a racing reader released meanwhile).
            if (info.destroy) info.destroy(block_->data);
            detail::aligned_free_(block_->data, info.align ? info.align : alignof(void*));
            delete block_;
        }
        block_ = fresh;
    }

    static void fold_(uint64_t& seed, uint64_t v) {
        seed ^= v;
        seed *= 1099511628211ull;   // FNV-1a 64 prime.
    }

    TypeId                type_ = k_invalid_type;
    Kind                  kind_ = Kind::Empty;
    alignas(k_inline_align) unsigned char buf_[k_inline_bytes] = {};
    detail::ValueBlock*   block_ = nullptr;
};

/// @brief Folds raw bytes into a seed. The default `hash` hook for trivially-comparable types.
inline void hash_bytes(const void* p, size_t n, uint64_t& seed) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) {
        seed ^= static_cast<uint64_t>(b[i]);
        seed *= 1099511628211ull;
    }
}

} // namespace nodegraph
} // namespace coopa

#endif // COOPA_NODEGRAPH_VALUE_H
