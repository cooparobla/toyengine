/**
 * @file loading.h
 * @brief LoadingState: the engine's manual, refcounted loading-state trigger.
 *
 * scene_loading.h's state machine covers a scene load; this covers whatever a game wants to
 * hold the screen up for that the engine's own warm-up gate knows nothing about -- a save
 * import, a world-gen pass run from gameplay code rather than at scene activation. It is kept
 * in its own header, not scene_loading.h, because a hold can outlive any load (or exist with
 * none in flight at all): the two are independent conditions the cover answers to, not one
 * state machine.
 *
 * @code
 * auto hold = engine.loading().acquire("importing save file");
 * // ... slow work ...
 * // the cover lifts once this (and every other open hold) is dropped, or goes out of scope.
 * @endcode
 */

#ifndef TOYENGINE_CORE_LOADING_H
#define TOYENGINE_CORE_LOADING_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace toy {
namespace core {

namespace detail {
/// Shared between a LoadingState and every Hold it has handed out, so a Hold never dangles
/// however the LoadingState and its holders are destroyed relative to each other.
struct LoadingControl {
    std::unordered_map<uint64_t, std::string> reasons;
    uint64_t next_id = 1;
};
}  // namespace detail

/**
 * @class LoadingState
 * @brief A set of open, named holds; any one of them keeps the loading cover up. Cheap to copy
 *        (every copy shares the same set) -- Engine owns one, by value.
 */
class LoadingState {
public:
    LoadingState() : control_(std::make_shared<detail::LoadingControl>()) {}

    /**
     * @class Hold
     * @brief RAII handle on one open hold. Move-only; the last owner dropping it (explicitly via
     *        release(), or by going out of scope) is what lets the cover go.
     */
    class Hold {
    public:
        Hold() = default;
        ~Hold() { release(); }
        Hold(Hold&& other) noexcept { *this = std::move(other); }
        Hold& operator=(Hold&& other) noexcept {
            if (this != &other) {
                release();
                control_ = std::move(other.control_);
                id_ = other.id_;
                other.control_.reset();
                other.id_ = 0;
            }
            return *this;
        }
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;

        /** @brief Idempotent; safe to call more than once (or never -- ~Hold() calls it too). */
        void release() {
            if (control_) control_->reasons.erase(id_);
            control_.reset();
            id_ = 0;
        }
        bool active() const { return control_ != nullptr; }

    private:
        friend class LoadingState;
        Hold(std::shared_ptr<detail::LoadingControl> control, uint64_t id)
            : control_(std::move(control)), id_(id) {}

        std::shared_ptr<detail::LoadingControl> control_;
        uint64_t id_ = 0;
    };

    /** @brief Opens a new hold with `reason` (shown by the F3 overlay / editor topbar). */
    [[nodiscard]] Hold acquire(std::string reason) {
        const uint64_t id = control_->next_id++;
        control_->reasons.emplace(id, std::move(reason));
        return Hold(control_, id);
    }

    /** @brief True while at least one hold is open. */
    bool held() const { return !control_->reasons.empty(); }
    /** @brief How many holds are open. */
    int hold_count() const { return static_cast<int>(control_->reasons.size()); }
    /** @brief Every open hold's reason, in no particular order. */
    std::vector<std::string> reasons() const {
        std::vector<std::string> out;
        out.reserve(control_->reasons.size());
        for (const auto& [id, reason] : control_->reasons) out.push_back(reason);
        return out;
    }

private:
    std::shared_ptr<detail::LoadingControl> control_;
};

} // namespace core
} // namespace toy

#endif // TOYENGINE_CORE_LOADING_H
