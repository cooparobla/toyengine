/**
 * @file xray_markers.h
 * @brief The two opt-in markers the x-ray silhouette pass reads: who is seen through things,
 *        and what may be seen through.
 *
 * The effect needs both halves to agree at a pixel before it draws anything, which is what
 * makes it safe to leave installed in a scene that only wants it in one place:
 *
 *  - `XraySubject` on an object marks it -- and everything under it -- as something whose
 *    silhouette should show when it is hidden. In a top-down game this is the player.
 *    @code
 *    - name: player
 *      components:
 *        - type: XraySubject
 *    @endcode
 *  - `XrayOccluder` on an object marks it -- and everything under it -- as something that
 *    MAY be seen through. Terrain, walls, a roof. An occluder without this marker hides the
 *    subject completely, which is how a scene keeps one pillar solid while the rest of the
 *    level goes transparent.
 *    @code
 *    - name: ridge
 *      components:
 *        - type: XrayOccluder
 *    @endcode
 *
 * Both are plain markers: they carry one bool and no state, and nothing reads them unless
 * the `xray_enabled` render setting built the pass. Because the subtree walk takes the
 * NEAREST marker, a prefab can opt its whole tree in at the root and one child can opt back
 * out with `enabled: false`.
 *
 * The look -- silhouette or outline, its colour, width and depth tint -- is a per-scene
 * render setting (`xray_mode` / `xray_color` / `xray_thickness` / `xray_depth_fade`), not a
 * per-object one, so every subject in a scene reads as one consistent effect. Several
 * subject meshes overlapping produce ONE union silhouette with one outline around it,
 * because they all draw into the same coverage mask.
 */

#ifndef TOYENGINE_SCENE_XRAY_MARKERS_H
#define TOYENGINE_SCENE_XRAY_MARKERS_H

#include <cstdint>

#include <coopa/scene/component.h>

namespace coopa::scene { class SceneObject; }

namespace toy {
namespace scene {

/** @brief Opts an object (and its subtree) into being drawn as a silhouette when hidden. */
class XraySubject : public coopa::scene::Component {
public:
    bool enabled = true;   ///< False opts this object back out of a marked parent's subtree.
    std::string type_name() const override { return "XraySubject"; }
};

/** @brief Opts an object (and its subtree) into being seen through by an XraySubject. */
class XrayOccluder : public coopa::scene::Component {
public:
    bool enabled = true;   ///< False opts this object back out of a marked parent's subtree.
    std::string type_name() const override { return "XrayOccluder"; }
};

/**
 * @enum XrayRole
 * @brief What an object contributes to the x-ray mask. Values match the role push constant
 *        in assets/shaders/xray_mask.frag.
 */
enum class XrayRole : uint8_t {
    None     = 0,   ///< Takes no part; never drawn by the x-ray pass.
    Subject  = 1,   ///< Drawn where it is hidden (and where it covers, for the outline).
    Occluder = 2,   ///< Drawn where it is the visible surface, granting permission there.
};

/** @brief True if `obj` or its nearest marked ancestor carries an enabled XraySubject. */
bool is_xray_subject(const coopa::scene::SceneObject* obj);

/** @brief True if `obj` or its nearest marked ancestor carries an enabled XrayOccluder. */
bool allows_xray(const coopa::scene::SceneObject* obj);

/**
 * @brief The role `obj` plays in the x-ray mask.
 *
 * Subject wins when an object is somehow both: being seen through yourself is meaningless,
 * and silently dropping the subject would be the harder failure to diagnose.
 */
XrayRole xray_role(const coopa::scene::SceneObject* obj);

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_XRAY_MARKERS_H
