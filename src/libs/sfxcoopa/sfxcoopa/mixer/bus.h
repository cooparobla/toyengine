/**
 * @file bus.h
 * @brief MixerBus: the "track" concept -- a node in the mix graph with its
 * own volume/mute/solo and effect chain, rendered depth-first post-order.
 */

#ifndef SFXCOOPA_MIXER_BUS_H
#define SFXCOOPA_MIXER_BUS_H

#include <sfxcoopa/data/audio_format.h>
#include <sfxcoopa/dsp/effect.h>
#include <sfxcoopa/util/db.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace coopa {
namespace sfx {
namespace mixer {

/** @brief Most children one bus can hold; its storage is allocated up front. See MixerBus::add_child(). */
constexpr uint32_t k_max_bus_children = 64;

/** @brief Most effects one bus's chain can hold; its storage is allocated up front. See MixerBus::add_effect(). */
constexpr uint32_t k_max_bus_effects = 16;

/**
 * @class MixerBus
 * @brief One node of the mix graph. Voices write directly into a bus's own
 * scratch buffer (via scratch()); the bus then recursively pulls in its
 * children, runs its effect chain, and applies its own gain -- so a bus
 * always publishes an already gain-and-effect-applied result for whichever
 * bus reads it next (its parent, or Mixer for the root).
 *
 * Solo is resolved globally and control-side via recompute_effective_mutes():
 * whenever any bus anywhere in the graph has solo() true, every bus that is
 * not that bus, an ancestor of it, or a descendant of it becomes effectively
 * muted for that render -- computed once so the audio thread only ever
 * reads the precomputed effective_mute() flag, never scans graph state
 * itself.
 *
 * ## Linking children and effects while the audio thread renders
 *
 * Both lists hold k_max_bus_children / k_max_bus_effects slots from
 * construction and never grow, and each is published by a release store to
 * its own atomic count that render() acquire-loads. A child or effect is
 * therefore fully constructed -- and, for an effect, already prepare()d --
 * before the count that exposes it is visible, so add_child() and
 * add_effect() are safe to call from the control thread against a concurrent
 * render(). Exceeding either capacity throws rather than reallocating,
 * because a reallocation is exactly what the audio thread cannot survive.
 *
 * Non-copyable, non-movable: children_ store raw pointers to sibling
 * MixerBus instances by address, which a copy or move would invalidate.
 * Mixer owns all MixerBus instances behind stable-address unique_ptrs for
 * exactly this reason.
 */
class MixerBus {
public:
    MixerBus(std::string name, uint32_t index, MixerBus* parent)
        : name_(std::move(name)), index_(index), parent_(parent) {
        children_.resize(k_max_bus_children, nullptr);
        effects_.resize(k_max_bus_effects);
    }

    MixerBus(const MixerBus&) = delete;
    MixerBus& operator=(const MixerBus&) = delete;
    MixerBus(MixerBus&&) = delete;
    MixerBus& operator=(MixerBus&&) = delete;

    const std::string& name() const { return name_; }

    /** @brief Stable index assigned at creation; commands address buses by this, never by name. */
    uint32_t index() const { return index_; }

    MixerBus* parent() const { return parent_; }

    /** @brief How many children are linked. Every index below it is fully published; see the class doc. */
    uint32_t child_count() const { return child_count_.load(std::memory_order_acquire); }

    /** @brief Child `i`, valid for `i` < child_count(). */
    MixerBus* child_at(uint32_t i) const { return children_[i]; }

    /**
     * @brief Links `child` under this bus, publishing it to render() only once it is stored.
     * @throws std::runtime_error if this bus already holds k_max_bus_children children.
     */
    void add_child(MixerBus* child) {
        const uint32_t count = child_count_.load(std::memory_order_relaxed);
        if (count >= k_max_bus_children) {
            throw std::runtime_error("[sfxcoopa] bus '" + name_ + "' already holds its maximum of " +
                                     std::to_string(k_max_bus_children) + " children");
        }
        children_[count] = child;
        child_count_.store(count + 1, std::memory_order_release);
    }

    /** @brief Linear gain applied to this bus's own mix before it's read by its parent (or Mixer, for the root). */
    float volume() const { return volume_; }
    void set_volume(float linear) { volume_ = linear; }
    float volume_db() const { return coopa::sfx::util::linear_to_db(volume_); }
    void set_volume_db(float db) { volume_ = coopa::sfx::util::db_to_linear(db); }

    bool mute() const { return mute_; }
    void set_mute(bool m) { mute_ = m; }
    bool solo() const { return solo_; }
    void set_solo(bool s) { solo_ = s; }

    /** @brief The control-side-computed flag the audio thread actually reads; see recompute_effective_mutes(). */
    bool effective_mute() const { return effective_mute_; }

    /** @brief How many effects are in this bus's chain. */
    uint32_t effect_count() const { return effect_count_.load(std::memory_order_acquire); }

    /**
     * @brief Appends an effect to this bus's chain, run in append order.
     *
     * The effect is prepare()d here when this bus already knows its format and
     * block size, so one added after the graph was prepared arrives with its
     * buffers allocated rather than processing against nothing. It becomes
     * visible to render() only after that.
     *
     * @throws std::runtime_error if this bus's chain already holds k_max_bus_effects effects.
     */
    void add_effect(std::unique_ptr<coopa::sfx::dsp::IEffect> effect) {
        const uint32_t count = effect_count_.load(std::memory_order_relaxed);
        if (count >= k_max_bus_effects) {
            throw std::runtime_error("[sfxcoopa] bus '" + name_ + "' already holds its maximum of " +
                                     std::to_string(k_max_bus_effects) + " effects");
        }
        if (max_frames_ > 0) effect->prepare(format_, max_frames_);
        effects_[count] = std::move(effect);
        effect_count_.store(count + 1, std::memory_order_release);
    }

    /** @brief Allocates this bus's scratch buffer and prepares its effect chain. */
    void prepare(const coopa::sfx::data::AudioFormat& format, uint32_t max_frames) {
        format_ = format;
        max_frames_ = max_frames;
        scratch_.assign(static_cast<size_t>(max_frames) * format.channels, 0.0f);
        const uint32_t effects = effect_count_.load(std::memory_order_relaxed);
        for (uint32_t e = 0; e < effects; ++e) effects_[e]->prepare(format, max_frames);
    }

    /** @brief This bus's scratch buffer -- voices targeting this bus accumulate directly into it. */
    float* scratch() { return scratch_.data(); }

    /** @brief Zeroes this bus's scratch buffer for a new render block. Must be called before voices render into it. */
    void clear_scratch(uint32_t frames) {
        std::fill_n(scratch_.data(), static_cast<size_t>(frames) * format_.channels, 0.0f);
    }

    /**
     * @brief Depth-first post-order render: recursively renders and
     * accumulates every child (each already gain/effect-applied by its own
     * render()), runs this bus's own effect chain, then scales this bus's
     * entire scratch buffer by its own gain (0 if effective_mute()).
     *
     * Assumes clear_scratch() plus any directly-assigned voices' output
     * already populated this bus's scratch buffer for this block.
     */
    void render(uint32_t frames) {
        const uint32_t n = frames * format_.channels;
        const uint32_t children = child_count_.load(std::memory_order_acquire);
        for (uint32_t c = 0; c < children; ++c) {
            MixerBus* child = children_[c];
            child->render(frames);
            const float* child_scratch = child->scratch_.data();
            for (uint32_t i = 0; i < n; ++i) scratch_[i] += child_scratch[i];
        }

        const uint32_t effects = effect_count_.load(std::memory_order_acquire);
        for (uint32_t e = 0; e < effects; ++e) effects_[e]->process(scratch_.data(), frames, format_.channels);

        const float gain = effective_mute_ ? 0.0f : volume_;
        for (uint32_t i = 0; i < n; ++i) scratch_[i] *= gain;
    }

    /**
     * @brief Recomputes effective_mute() for every bus in `all_buses`
     * (main-thread only). If any bus has solo() true, a bus is kept
     * audible only if it *is* a soloed bus, is an ancestor of one (so the
     * soloed signal still has a path up to the root), or is a descendant
     * of one (soloing a bus means "this and everything under it") --
     * every other bus becomes effectively muted. With no solo active
     * anywhere, effective_mute() is just each bus's own mute() flag.
     *
     * A naive "mute everything that isn't itself soloed" rule is wrong
     * here: it would also mute the soloed bus's own ancestors, silencing
     * the very signal the solo was meant to isolate before it ever
     * reaches the root.
     */
    static void recompute_effective_mutes(const std::vector<MixerBus*>& all_buses) {
        bool any_solo = false;
        for (MixerBus* bus : all_buses) any_solo = any_solo || bus->solo_;

        for (MixerBus* bus : all_buses) {
            bool keep = !any_solo || bus->solo_;
            for (MixerBus* p = bus->parent_; p && !keep; p = p->parent_) {
                keep = p->solo_;
            }
            if (!keep) keep = subtree_has_solo_(bus);
            bus->effective_mute_ = bus->mute_ || (any_solo && !keep);
        }
    }

private:
    static bool subtree_has_solo_(MixerBus* bus) {
        if (bus->solo_) return true;
        const uint32_t children = bus->child_count_.load(std::memory_order_relaxed);
        for (uint32_t c = 0; c < children; ++c) {
            if (subtree_has_solo_(bus->children_[c])) return true;
        }
        return false;
    }

    std::string name_;
    uint32_t index_;
    MixerBus* parent_ = nullptr;
    std::vector<MixerBus*> children_;
    std::atomic<uint32_t> child_count_{0};

    float volume_ = 1.0f;
    bool mute_ = false;
    bool solo_ = false;
    bool effective_mute_ = false;

    std::vector<std::unique_ptr<coopa::sfx::dsp::IEffect>> effects_;
    std::atomic<uint32_t> effect_count_{0};
    std::vector<float> scratch_;
    coopa::sfx::data::AudioFormat format_{};
    uint32_t max_frames_ = 0;
};

}  // namespace mixer
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_MIXER_BUS_H
