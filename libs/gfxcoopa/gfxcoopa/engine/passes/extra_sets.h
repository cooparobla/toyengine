/**
 * @file extra_sets.h
 * @brief Optional app-supplied descriptor sets appended after a pass's own.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_EXTRA_SETS_H
#define GFXCOOPA_ENGINE_PASSES_EXTRA_SETS_H

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/pipeline/descriptor.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/**
 * @struct ExtraSets
 * @brief Optional app-supplied descriptor sets, appended AFTER a pass's own
 *        sets in its pipeline layout.
 *
 * A pass declares only its own sets and appends whatever the app supplies,
 * rather than naming a specific subsystem (a `gi_layout` parameter bound to
 * a `gi::GiSystem*`) at a hardcoded set index. Empty extras mean the set is
 * not declared in the pipeline layout at all, so a shader built without the
 * subsystem is genuinely built without it rather than left with an unbound
 * set.
 *
 * `layouts` and `bind` are kept in one struct deliberately: the entire class
 * of bug this type exists to prevent is a layout and its binder drifting
 * apart, so the type makes them travel together and validate() turns a
 * mismatch into a startup exception instead of a validation-layer warning
 * discovered at draw time.
 *
 * `bind` takes no VkPipelineLayout: every call site invokes it after the
 * pass's own cmd.bind_pipeline(), which already caches that pipeline's
 * layout on CommandBuffer for the sealed bind_descriptor_set()/
 * push_constants() overloads to use -- see command_buffer.h's bound_layout_.
 */
struct ExtraSets {
    /// Appended in order after a pass's own layouts. Empty (the default)
    /// yields a pipeline layout with no trailing sets at all -- what makes
    /// "no GI" (or no anything else) a real configuration, not a cosmetic
    /// runtime branch against a layout that still declares the set.
    std::vector<const coopa::gfx::pipeline::DescriptorSetLayout*> layouts;

    /// Invoked once per draw that uses this pass's pipeline layout, AFTER
    /// that pass has called cmd.bind_pipeline(). `first_set` is the index of
    /// layouts[0] in that pipeline layout -- never hardcode it at the call
    /// site, since it shifts if the pass gains or loses an owned set. Must
    /// be set iff `layouts` is non-empty (see validate()). The callback must
    /// outlive the pass it's given to.
    std::function<void(coopa::gfx::command::CommandBuffer&, uint32_t first_set)> bind;

    bool empty() const { return layouts.empty(); }

    /// @throws std::logic_error if layouts and bind disagree about whether
    ///         extra sets exist at all.
    void validate(const char* who) const {
        if (layouts.empty() != (bind == nullptr)) {
            throw std::logic_error(std::string(who) +
                ": ExtraSets.layouts and ExtraSets.bind must both be set or both be empty");
        }
    }
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_EXTRA_SETS_H
