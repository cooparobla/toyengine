/**
 * @file curve.h
 * @brief The `Curve` component: an authored path attached to an object.
 *
 * Holds a `CurveData` in the object's LOCAL space, so moving or rotating the object carries the
 * curve with it. The component does nothing on its own -- there is no CurveSystem and nothing
 * updates per frame. It is read by whatever wants a path: the editor's pen tool edits it, and a
 * `ProceduralGraph` instances along it (`toyengine/procgraph/`, the `curve` input binding and
 * the Curve to Points node).
 *
 * Attach it to the same object as the graph, or to a separate one the graph binds by name --
 * both work, because the binding resolves an object and reads the component off it.
 */

#ifndef TOYENGINE_CURVE_CURVE_H
#define TOYENGINE_CURVE_CURVE_H

#include <string>

#include <coopa/scene/component.h>

#include "toyengine/curve/curve_data.h"

namespace toy {
namespace curve {

/**
 * @class Curve
 * @brief An authored path on an object, in its local space.
 */
class Curve : public coopa::scene::Component {
public:
    std::string type_name() const override { return "Curve"; }

    /// @brief The path itself. Local to the owner's transform.
    CurveData data;

    /// @brief Draw it in the viewport even when the object is not selected.
    bool always_show = false;
};

/// @brief The YAML spelling of a handle mode, for the editor's save path.
const char* handle_mode_name(HandleMode mode);

/// @brief Registers the "Curve" scene parser. Called once at startup.
void register_curve_components();

} // namespace curve
} // namespace toy

#endif // TOYENGINE_CURVE_CURVE_H
