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
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coopa {
namespace sfx {
namespace mixer {

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
 * Non-copyable, non-movable: children_ store raw pointers to sibling
 * MixerBus instances by address, which a copy or move would invalidate.
 * Mixer owns all MixerBus instances behind stable-address unique_ptrs for
 * exactly this reason.
 */
class MixerBus {
public:
    MixerBus(std::string name, uint32_t index, MixerBus* parent)
        : name_(std::move(name)), index_(index), parent_(parent) {}

    MixerBus(const MixerBus&) = delete;
    MixerBus& operator=(const MixerBus&) = delete;
    MixerBus(MixerBus&&) = delete;
    MixerBus& operator=(MixerBus&&) = delete;

    const std::string& name() const { return name_; }

    /** @brief Stable index assigned at creation; commands address buses by this, never by name. */
    uint32_t index() const { return index_; }

    MixerBus* parent() const { return parent_; }
    const std::vector<MixerBus*>& children() const { return children_; }

    /** @brief Links `child` under this bus. Called only by whoever builds the tree (Mixer::create_bus). */
    void add_child(MixerBus* child) { children_.push_back(child); }

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

    /** @brief Appends an effect to this bus's chain, run in append order. */
    void add_effect(std::unique_ptr<coopa::sfx::dsp::IEffect> effect) {
        effects_.push_back(std::move(effect));
    }

    /** @brief Allocates this bus's scratch buffer and prepares its effect chain. */
    void prepare(const coopa::sfx::data::AudioFormat& format, uint32_t max_frames) {
        format_ = format;
        scratch_.assign(static_cast<size_t>(max_frames) * format.channels, 0.0f);
        for (auto& effect : effects_) effect->prepare(format, max_frames);
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
        for (MixerBus* child : children_) {
            child->render(frames);
            const float* child_scratch = child->scratch_.data();
            for (uint32_t i = 0; i < n; ++i) scratch_[i] += child_scratch[i];
        }

        for (auto& effect : effects_) effect->process(scratch_.data(), frames, format_.channels);

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
        for (MixerBus* child : bus->children_) {
            if (subtree_has_solo_(child)) return true;
        }
        return false;
    }

    std::string name_;
    uint32_t index_;
    MixerBus* parent_ = nullptr;
    std::vector<MixerBus*> children_;

    float volume_ = 1.0f;
    bool mute_ = false;
    bool solo_ = false;
    bool effective_mute_ = false;

    std::vector<std::unique_ptr<coopa::sfx::dsp::IEffect>> effects_;
    std::vector<float> scratch_;
    coopa::sfx::data::AudioFormat format_{};
};

}  // namespace mixer
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_MIXER_BUS_H
