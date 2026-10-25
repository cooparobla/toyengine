/**
 * @file view_blocker_system.h
 * @brief Decides which ViewBlockerFade walls are in the way of the x-ray subject, with one
 *        capsule overlap per subject per frame.
 *
 * One query for the whole scene, not one per wall. `overlap_capsule()` along the
 * camera-to-subject segment returns EVERY collider in the way in a single broadphase
 * descent, so the cost is in the number of blockers rather than the number of walls -- a
 * level can carry hundreds of fade-able walls for the price of one query. `sphere_cast()`
 * would have been the obvious choice and is the wrong one: it reports only the nearest hit,
 * so a row of two walls would leave the far one solid.
 *
 * The capsule has thickness for a reason. A bare ray flickers on and off as the player walks
 * behind a wall's edge, because the ray crosses the silhouette in a single frame; the radius
 * turns that into a short overlap where both the edge wall and its neighbour fade, which is
 * what the eye expects.
 *
 * Order 365 -- after `TransformResolve` (350), which is what makes the query correct rather
 * than merely close. Both the subject's resolved world matrix and the camera's final pose for
 * THIS frame exist by then, so the segment is the one actually being rendered; at `Behaviour`
 * (200) the camera has not finished following yet and the segment lags by a frame, which on a
 * fast strafe is a visibly late fade.
 */

#ifndef TOYENGINE_SCENE_VIEW_BLOCKER_SYSTEM_H
#define TOYENGINE_SCENE_VIEW_BLOCKER_SYSTEM_H

#include <coopa/scene/scene.h>

namespace toy {
namespace scene {

/** @brief Default registration order: after TransformResolve (350), beside particles (360). */
inline constexpr int k_view_blocker_order = 365;

/**
 * @class ViewBlockerSystem
 * @brief Marks every ViewBlockerFade between the camera and an XraySubject, then advances
 *        all of their dissolves.
 */
class ViewBlockerSystem : public coopa::scene::ISceneSystem {
public:
    const char* system_name() const override { return "ViewBlocker"; }

    /** @brief Radius of the camera-to-subject overlap capsule, in metres. */
    float probe_radius = 0.4f;

    /**
     * @brief Metres trimmed off the subject end of the segment.
     *
     * Without it the capsule reaches the subject itself and catches the floor it stands on,
     * or a wall it is leaning against -- neither of which is between the camera and it.
     */
    float subject_margin = 0.6f;

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override;
};

/**
 * @brief Constructs a ViewBlockerSystem and registers it after the transform resolve.
 *
 * Call once per scene. A scene without it leaves every ViewBlockerFade wall solid, which is
 * a visible failure rather than a subtle one.
 *
 * @param scene Scene to install into.
 * @param order Registration order; defaults to k_view_blocker_order (365).
 * @return Non-owning pointer to the installed system.
 */
ViewBlockerSystem* install_view_blocker_system(coopa::scene::Scene& scene,
                                               int order = k_view_blocker_order);

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_VIEW_BLOCKER_SYSTEM_H
