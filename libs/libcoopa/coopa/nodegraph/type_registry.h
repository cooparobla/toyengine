/**
 * @file type_registry.h
 * @brief The socket type system: runtime-registered types, and the implicit conversions
 *        between them.
 *
 * ### Why types are registered rather than templated
 *
 * A templated `Graph<ValueVariant>` would bake each consumer's value set into the type, so a
 * geometry graph and an audio graph could not share one compiler or one evaluator -- which
 * defeats the point of this module. Instead a consumer registers its own types at startup and
 * refers to them by `TypeId`, exactly as `coopa::scene::SceneLoader::register_component_parser()`
 * lets libcoopa drive component types it has never heard of.
 *
 * The cost of that choice is that type operations are indirect calls through `TypeInfo` rather
 * than inlined code. That is why `Value` keeps a small inline buffer and why the compiler fuses
 * runs of cheap nodes: the indirection is paid per *value*, so the way to make it cheap is to
 * move fewer values, not to make each move faster.
 *
 * ### Lifetime and threading
 *
 * The registry is process-wide and **write-at-startup, read-concurrently**. `add()` and
 * `add_conversion()` take a unique lock; every read path takes a shared lock. A `TypeId` is
 * never recycled and a registered `TypeInfo` is never mutated, so a `const TypeInfo&` handed out
 * by `info()` stays valid for the life of the process. `clear()` exists for tests only -- calling
 * it while any `Value` is alive invalidates that value's ability to destroy itself.
 *
 * ### Implicit conversions
 *
 * Blender's graphs let a Float feed a Vector socket, an Int feed a Float, and so on. Rather than
 * special-case that in the compiler, a conversion is a registered function and the compiler
 * inserts a conversion step wherever a link's endpoints disagree but a conversion exists. A link
 * whose endpoints disagree with no conversion registered is a validation error naming both types,
 * which is the message an author can act on.
 */

#ifndef COOPA_NODEGRAPH_TYPE_REGISTRY_H
#define COOPA_NODEGRAPH_TYPE_REGISTRY_H

#include <cstdint>
#include <new>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fkYAML/node.hpp>

namespace coopa {
namespace nodegraph {

/// @brief A registered socket type. Index into TypeRegistry; 0 means "no type".
using TypeId = uint16_t;

/// @brief The id no registered type ever has. An empty `Value` reports this.
inline constexpr TypeId k_invalid_type = 0;

/**
 * @struct TypeInfo
 * @brief Everything the module needs to move, copy, compare and serialize one registered type
 *        without knowing what it is.
 *
 * The function pointers are filled automatically by `TypeRegistry::add<T>()`; the two YAML hooks
 * are optional and are what let socket defaults round-trip through a file for a type this module
 * has never seen. A type with no YAML hooks still works -- its sockets simply cannot carry an
 * authored default.
 */
struct TypeInfo {
    std::string name;        ///< The spelling used in graph YAML. Unique within the registry.
    uint32_t    size = 0;
    uint32_t    align = 0;

    void (*default_construct)(void* dst) = nullptr;
    void (*copy_construct)(void* dst, const void* src) = nullptr;
    void (*move_construct)(void* dst, void* src) = nullptr;
    void (*destroy)(void* p) = nullptr;

    /// @brief Value equality. Drives change detection, so a type without it always reads as changed.
    bool (*equals)(const void* a, const void* b) = nullptr;
    /// @brief Folds the value into `seed` (FNV-1a style, as coopa::asset::AssetId does).
    void (*hash)(const void* p, uint64_t& seed) = nullptr;

    /// @brief Reads an authored default. Returns false when the node is the wrong shape.
    bool (*from_yaml)(const fkyaml::node& node, void* dst) = nullptr;
    /// @brief Writes a default back. Must mirror from_yaml or files stop round-tripping.
    bool (*to_yaml)(const void* src, fkyaml::node& out) = nullptr;

    /// @brief True when this type can be stored in a Value's inline buffer (see value.h).
    bool fits_inline(uint32_t inline_bytes, uint32_t inline_align) const {
        return size <= inline_bytes && align <= inline_align;
    }
};

/**
 * @class TypeRegistry
 * @brief Process-wide table of socket types and the conversions between them.
 */
class TypeRegistry {
public:
    /// @brief The one instance. A function-local static, as every other coopa registry uses.
    static TypeRegistry& instance() {
        static TypeRegistry registry;
        return registry;
    }

    /**
     * @brief Registers `T` under `name`, deriving every TypeInfo function from the type.
     *
     * Registering the same name twice returns the existing id without overwriting it, so a
     * consumer may call its registration function more than once (a test fixture usually does)
     * without tearing live ids out from under existing graphs.
     *
     * @tparam T Must be destructible, copy-constructible and move-constructible.
     * @param name The YAML spelling.
     * @return The id for `name`.
     */
    template <typename T>
    TypeId add(std::string name) {
        static_assert(std::is_destructible_v<T>, "a socket type must be destructible");
        static_assert(std::is_copy_constructible_v<T>, "a socket type must be copy-constructible");

        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            auto it = by_name_.find(name);
            if (it != by_name_.end()) return it->second;
        }

        TypeInfo info;
        info.name  = name;
        info.size  = static_cast<uint32_t>(sizeof(T));
        info.align = static_cast<uint32_t>(alignof(T));

        info.default_construct = [](void* dst) { new (dst) T(); };
        info.copy_construct    = [](void* dst, const void* src) { new (dst) T(*static_cast<const T*>(src)); };
        info.move_construct    = [](void* dst, void* src) { new (dst) T(std::move(*static_cast<T*>(src))); };
        info.destroy           = [](void* p) { static_cast<T*>(p)->~T(); };

        if constexpr (detail_has_equality_<T>()) {
            info.equals = [](const void* a, const void* b) {
                return bool(*static_cast<const T*>(a) == *static_cast<const T*>(b));
            };
        }

        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = by_name_.find(name);
        if (it != by_name_.end()) return it->second;   // Lost a race; keep the winner.
        types_.push_back(std::move(info));
        TypeId id = static_cast<TypeId>(types_.size());   // 1-based: 0 is k_invalid_type.
        by_name_.emplace(std::move(name), id);
        return id;
    }

    /**
     * @brief Attaches the optional YAML hooks to an already-registered type.
     *
     * Separate from `add<T>()` because the conversion to and from a `fkyaml::node` is a policy
     * decision about the file format, not a property of the C++ type -- a glm::vec3 might be
     * authored as `[x, y, z]` in one consumer and as `{x:, y:, z:}` in another.
     */
    void set_yaml_hooks(TypeId id,
                        bool (*from_yaml)(const fkyaml::node&, void*),
                        bool (*to_yaml)(const void*, fkyaml::node&)) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (!valid_(id)) return;
        types_[id - 1].from_yaml = from_yaml;
        types_[id - 1].to_yaml   = to_yaml;
    }

    /// @brief Overrides the derived hash for a type whose bytes are not its identity.
    void set_hash(TypeId id, void (*hash)(const void*, uint64_t&)) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (valid_(id)) types_[id - 1].hash = hash;
    }

    /// @brief Overrides the derived equality.
    void set_equals(TypeId id, bool (*equals)(const void*, const void*)) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (valid_(id)) types_[id - 1].equals = equals;
    }

    /// @brief The id registered under `name`, or k_invalid_type.
    TypeId find(std::string_view name) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = by_name_.find(std::string(name));
        return it == by_name_.end() ? k_invalid_type : it->second;
    }

    /**
     * @brief The info for `id`.
     * @return A reference that stays valid for the life of the process. For an invalid id,
     *         a shared empty TypeInfo whose function pointers are all null -- so a caller that
     *         forgot to check still gets a predictable no-op rather than a wild call.
     */
    const TypeInfo& info(TypeId id) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (!valid_(id)) return empty_info_();
        return types_[id - 1];
    }

    /// @brief The name registered for `id`, or "<invalid>".
    std::string name_of(TypeId id) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return valid_(id) ? types_[id - 1].name : std::string("<invalid>");
    }

    size_t type_count() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return types_.size();
    }

    // --- Implicit conversions ---

    /**
     * @brief Registers that a `from` value may feed a `to` socket.
     * @param fn Writes a `to`-typed value into already-constructed storage at `dst`.
     */
    void add_conversion(TypeId from, TypeId to, void (*fn)(const void* src, void* dst)) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        conversions_[key_(from, to)] = fn;
    }

    /// @brief Whether a `from` value may feed a `to` socket. Same-type always converts.
    bool can_convert(TypeId from, TypeId to) const {
        if (from == to) return from != k_invalid_type;
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return conversions_.find(key_(from, to)) != conversions_.end();
    }

    /// @brief The registered conversion, or nullptr. Null for `from == to` (nothing to do).
    void (*conversion(TypeId from, TypeId to) const)(const void*, void*) {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = conversions_.find(key_(from, to));
        return it == conversions_.end() ? nullptr : it->second;
    }

    /// @brief Forgets every type and conversion. Tests only -- see the @file note on lifetime.
    void clear() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        types_.clear();
        by_name_.clear();
        conversions_.clear();
    }

private:
    TypeRegistry() = default;

    bool valid_(TypeId id) const { return id != k_invalid_type && id <= types_.size(); }

    static const TypeInfo& empty_info_() {
        static const TypeInfo empty{};
        return empty;
    }

    static uint32_t key_(TypeId from, TypeId to) {
        return (static_cast<uint32_t>(from) << 16) | static_cast<uint32_t>(to);
    }

    /// @brief Whether `T == T` compiles, so equality is only derived where it exists.
    template <typename T>
    static constexpr bool detail_has_equality_() {
        return requires(const T& a, const T& b) { a == b; };
    }

    mutable std::shared_mutex                    mutex_;
    std::vector<TypeInfo>                        types_;
    std::unordered_map<std::string, TypeId>      by_name_;
    std::unordered_map<uint32_t, void (*)(const void*, void*)> conversions_;
};

} // namespace nodegraph
} // namespace coopa

#endif // COOPA_NODEGRAPH_TYPE_REGISTRY_H
