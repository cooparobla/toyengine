/**
 * @file signal.h
 * @brief Header-only multicast signal/slot dispatch with RAII connection tokens.
 *
 * A Signal<Args...> holds any number of callable slots and invokes every one of them
 * from emit(). connect() returns a Connection token that stays safe to query and to
 * disconnect even after the Signal it came from has been destroyed; ScopedConnection
 * wraps that token in move-only RAII.
 *
 * Not thread-safe: connect(), disconnect() and emit() on one Signal must be serialized
 * by the caller.
 */

#ifndef COOPA_EVENT_SIGNAL_H
#define COOPA_EVENT_SIGNAL_H

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace coopa {
namespace event {

/** @brief Per-Signal slot identifier, handed out monotonically and never reused. */
using SlotId = std::size_t;

/** @brief Never returned by a successful connect(); marks an empty Connection. */
inline constexpr SlotId k_invalid_slot = 0;

template<typename... Args>
class Signal;

namespace detail {

/**
 * @struct SignalCore
 * @brief Shared control block linking a Signal to every Connection it handed out.
 *
 * The Signal owns the only shared_ptr to this block; every Connection holds a
 * weak_ptr. ~Signal() clears signal, so a Connection that outlives its Signal
 * degrades to "not connected" rather than dangling.
 */
struct SignalCore {
    void* signal = nullptr;                       /**< Owning Signal, or null once it has been destroyed. */
    bool (*disconnect)(void*, SlotId) = nullptr;  /**< Type-erased Signal::disconnect(SlotId). */
    bool (*connected)(void*, SlotId) = nullptr;   /**< Type-erased Signal::has_slot(SlotId). */
};

}  // namespace detail

/**
 * @class Connection
 * @brief Copyable token identifying one slot, safe to hold past its Signal's lifetime.
 */
class Connection {
public:
    Connection() = default;

    /** @brief True if the signal still exists and the slot is still attached. */
    bool connected() const {
        if (id_ == k_invalid_slot) return false;
        auto core = core_.lock();
        if (!core || !core->signal) return false;
        return core->connected(core->signal, id_);
    }

    /**
     * @brief Detaches the slot from its signal, if both still exist.
     * @return True if a slot was actually removed.
     */
    bool disconnect() {
        if (id_ == k_invalid_slot) return false;
        auto core = core_.lock();
        if (!core || !core->signal) return false;
        return core->disconnect(core->signal, id_);
    }

    /** @brief The slot id, or k_invalid_slot for an empty connection. */
    SlotId id() const { return id_; }

private:
    template<typename... Args> friend class Signal;

    Connection(std::weak_ptr<detail::SignalCore> core, SlotId id)
        : core_(std::move(core)), id_(id) {}

    std::weak_ptr<detail::SignalCore> core_;
    SlotId                            id_ = k_invalid_slot;
};

/**
 * @class ScopedConnection
 * @brief Move-only RAII owner of a Connection; disconnects on destruction.
 *
 * @code
 * coopa::event::ScopedConnection c = button->on_click.connect_scoped([] { ... });
 * @endcode
 */
class ScopedConnection {
public:
    ScopedConnection() = default;
    explicit ScopedConnection(Connection connection) : connection_(std::move(connection)) {}

    ScopedConnection(const ScopedConnection&) = delete;
    ScopedConnection& operator=(const ScopedConnection&) = delete;

    ScopedConnection(ScopedConnection&& other) noexcept : connection_(other.release()) {}

    ScopedConnection& operator=(ScopedConnection&& other) noexcept {
        if (this != &other) {
            connection_.disconnect();
            connection_ = other.release();
        }
        return *this;
    }

    /** @brief Disconnects whatever this held, then takes ownership of connection. */
    ScopedConnection& operator=(Connection connection) {
        connection_.disconnect();
        connection_ = std::move(connection);
        return *this;
    }

    ~ScopedConnection() { connection_.disconnect(); }

    bool connected() const { return connection_.connected(); }
    bool disconnect() { return connection_.disconnect(); }

    /** @brief Gives up ownership without disconnecting. */
    Connection release() {
        Connection c = std::move(connection_);
        connection_ = Connection();
        return c;
    }

    const Connection& connection() const { return connection_; }

private:
    Connection connection_;
};

/**
 * @class Signal
 * @brief Multicast event source: many slots, one emit().
 *
 * @tparam Args Argument types forwarded to every slot.
 *
 * Re-entrancy: a slot may connect a new slot (it runs starting from the *next*
 * emit(), never the one in progress), disconnect itself or any other slot
 * (already-invoked slots are unaffected; a not-yet-invoked disconnected slot is
 * skipped and its callable is destroyed only after emit() finishes unwinding —
 * never while it might still be on the call stack), and may even destroy the
 * Signal itself — emit() detects that and unwinds without touching the dead object.
 *
 * Signal is copy-disabled (duplicating a multicast slot list has no sane
 * ownership story) but move-enabled; outstanding Connections follow a moved
 * Signal to its new address.
 *
 * @code
 * coopa::event::Signal<int> on_value;
 * auto c = on_value.connect([](int v) { std::cout << v; });
 * on_value.emit(42);
 * c.disconnect();
 * @endcode
 */
template<typename... Args>
class Signal {
public:
    using Slot = std::function<void(Args...)>;

    Signal() = default;
    ~Signal() { if (core_) core_->signal = nullptr; }

    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;

    Signal(Signal&& other) noexcept
        : slots_(std::move(other.slots_)), pending_(std::move(other.pending_)),
          core_(std::move(other.core_)), next_id_(other.next_id_), emit_depth_(other.emit_depth_) {
        if (core_) core_->signal = this;
        other.next_id_ = k_invalid_slot + 1;
        other.emit_depth_ = 0;
    }

    Signal& operator=(Signal&& other) noexcept {
        if (this == &other) return *this;
        if (core_) core_->signal = nullptr;
        slots_      = std::move(other.slots_);
        pending_    = std::move(other.pending_);
        core_       = std::move(other.core_);
        next_id_    = other.next_id_;
        emit_depth_ = other.emit_depth_;
        if (core_) core_->signal = this;
        other.next_id_ = k_invalid_slot + 1;
        other.emit_depth_ = 0;
        return *this;
    }

    /**
     * @brief Attaches a slot.
     * @param slot Callable invoked by every subsequent emit(). An empty slot is ignored.
     * @return A Connection token; discard it if the slot should live as long as the Signal.
     */
    Connection connect(Slot slot) {
        if (!slot) return Connection();
        ensure_core_();
        SlotId id = next_id_++;
        Entry entry{id, std::move(slot), true};
        if (emit_depth_ > 0) {
            pending_.push_back(std::move(entry));
        } else {
            slots_.push_back(std::move(entry));
        }
        return Connection(std::weak_ptr<detail::SignalCore>(core_), id);
    }

    /** @brief connect() wrapped in RAII. */
    ScopedConnection connect_scoped(Slot slot) { return ScopedConnection(connect(std::move(slot))); }

    /** @brief Detaches the slot with the given id. @return True if one was removed. */
    bool disconnect(SlotId id) {
        if (id == k_invalid_slot) return false;
        for (Entry& e : slots_) {
            if (e.id == id && e.alive) {
                e.alive = false;  // tombstone: never destroy fn here, it may be executing
                if (emit_depth_ == 0) compact_();
                return true;
            }
        }
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
            if (it->id == id) {
                pending_.erase(it);
                return true;
            }
        }
        return false;
    }

    bool disconnect(const Connection& connection) { return disconnect(connection.id()); }

    /** @brief Detaches every slot. */
    void disconnect_all() {
        if (emit_depth_ > 0) {
            for (Entry& e : slots_) e.alive = false;
            pending_.clear();
        } else {
            slots_.clear();
            pending_.clear();
        }
    }

    /** @brief Invokes every attached slot, in connection order. */
    void emit(Args... args) {
        if (slots_.empty()) return;
        std::shared_ptr<detail::SignalCore> core = core_;  // outlives *this if a slot destroys it
        void* const self = this;
        const std::size_t count = slots_.size();  // connects during emit go to pending_: no realloc here
        ++emit_depth_;
        for (std::size_t i = 0; i < count; ++i) {
            const Entry& e = slots_[i];
            if (e.alive && e.fn) e.fn(args...);
            if (core->signal != self) return;  // this Signal died mid-emit: unwind, touch nothing
        }
        if (--emit_depth_ == 0) flush_();
    }

    void operator()(Args... args) { emit(args...); }

    /** @brief Number of currently attached slots (including ones connected during an emit). */
    std::size_t slot_count() const {
        std::size_t count = 0;
        for (const Entry& e : slots_) {
            if (e.alive) ++count;
        }
        return count + pending_.size();
    }

    bool empty() const { return slot_count() == 0; }

    /** @brief Whether a given slot id is still attached (used by Connection::connected()). */
    bool has_slot(SlotId id) const {
        if (id == k_invalid_slot) return false;
        for (const Entry& e : slots_) {
            if (e.id == id) return e.alive;
        }
        for (const Entry& e : pending_) {
            if (e.id == id) return true;
        }
        return false;
    }

private:
    struct Entry {
        SlotId id = k_invalid_slot;
        Slot   fn;
        bool   alive = true;  /**< False once disconnected; erased once emit() is not in progress. */
    };

    void ensure_core_() {
        if (core_) return;
        core_ = std::make_shared<detail::SignalCore>();
        core_->signal     = this;
        core_->disconnect = [](void* s, SlotId id) { return static_cast<Signal*>(s)->disconnect(id); };
        core_->connected  = [](void* s, SlotId id) { return static_cast<Signal*>(s)->has_slot(id); };
    }

    /** @brief Erases tombstoned entries, then splices in slots connected during the last emit(). */
    void flush_() {
        compact_();
        if (!pending_.empty()) {
            for (Entry& e : pending_) slots_.push_back(std::move(e));
            pending_.clear();
        }
    }

    /** @brief Erases !alive entries, destroying their callables. Never call this during emit(). */
    void compact_() {
        slots_.erase(std::remove_if(slots_.begin(), slots_.end(),
                                     [](const Entry& e) { return !e.alive; }),
                     slots_.end());
    }

    std::vector<Entry>                  slots_;      /**< Live (and tombstoned) slots, in connection order. */
    std::vector<Entry>                  pending_;    /**< Connected during an emit; spliced in on unwind. */
    std::shared_ptr<detail::SignalCore> core_;       /**< Lazily allocated on the first connect(). */
    SlotId      next_id_    = k_invalid_slot + 1;
    std::size_t emit_depth_ = 0;                     /**< >0 while inside emit(); defers structural edits. */
};

}  // namespace event
}  // namespace coopa

#endif  // COOPA_EVENT_SIGNAL_H
