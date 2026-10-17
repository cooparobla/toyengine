/**
 * @file turntable.h
 * @brief Mouse-dragged spinning of the object itself, for a model viewer: a globe, a prop, a
 *        character turned round in front of a camera that never moves.
 *
 * The counterpart to CameraController's orbit mode, and the opposite trade. An orbit rig turns
 * the *camera* round a subject, so the subject's lit side, its silhouette against the sky and
 * its shadows all change as you look. A turntable turns the *subject* in front of a fixed
 * camera, so the light keeps coming from the same place on screen and what moves is the model
 * through it -- which is what a planet spinning under a fixed sun, or a prop being inspected,
 * actually wants. Pair it with an orbit camera whose `mouse_sensitivity` is 0: the scroll wheel
 * still zooms, and the drag goes here instead.
 *
 * The rotation is a globe's, not a trackball's: horizontal drag spins the object about its own
 * polar `axis`, vertical drag tilts that axis toward or away from the viewer, clamped. Poles
 * stay poles and no amount of dragging rolls the model over sideways, which free trackball
 * rotation does within a few seconds of use.
 *
 * Input is PUSHED in by toy::core::Engine::drive_turntables_() before Scene::update() runs --
 * the same contract CameraController, KinematicController and FreeMover follow, so the
 * component is testable with no live GLFW window and NO_INPUT=1 zeroes it for a reproducible
 * headless capture.
 */

#ifndef TOYENGINE_SCENE_TURNTABLE_H
#define TOYENGINE_SCENE_TURNTABLE_H

#include <glm/glm.hpp>

#include <string>

#include <coopa/scene/component.h>

namespace toy {
namespace scene {

/**
 * @class Turntable
 * @brief Spins the owning object from mouse drag, with inertia and an idle drift.
 *
 * The owner's rotation each frame is `tilt about view_right` x `spin about axis`: two angles
 * the component owns outright, not an increment applied to whatever rotation it finds. It
 * therefore OWNS the owner's rotation -- an authored `rotation:` on the same object is the
 * seed read in start() and nothing after that.
 *
 * Releasing a drag leaves the spin running and decaying at `damping` (a fling), and
 * `idle_spin_deg_per_sec` keeps a slow drift underneath so a model left alone still turns.
 *
 * Example YAML:
 * @code
 * - type: Turntable
 *   sensitivity: 0.25           # degrees of spin per pixel of drag
 *   drag_button: left           # "" drags whenever the mouse moves
 *   idle_spin_deg_per_sec: 3.0
 *   damping: 2.5                # 1/s decay of a fling; 0 spins on for ever
 *   min_tilt_deg: -80.0
 *   max_tilt_deg: 80.0
 * @endcode
 */
class Turntable : public coopa::scene::Component {
public:
    std::string type_name() const override { return "Turntable"; }

    /** @brief Degrees of spin (and tilt) per pixel of mouse drag. */
    float sensitivity = 0.25f;

    /**
     * @brief Mouse button that must be held to drag: "" (any motion), "left", "right" or
     *        "middle".
     *
     * Defaults to a held button rather than to free motion because a turntable scene keeps its
     * cursor (`capture_cursor: false` on the camera) -- with a visible pointer, every mouse
     * move spinning the model makes the window impossible to leave alone.
     */
    std::string drag_button = "left";

    /** @brief The object's polar axis, in its own space; horizontal drag spins about it. */
    glm::vec3 axis = glm::vec3(0.0f, 0.0f, 1.0f);

    /** @brief Constant spin added every frame, degrees per second; 0 leaves a released model still. */
    float idle_spin_deg_per_sec = 0.0f;

    /**
     * @brief Decay rate of a fling's leftover spin, 1/s (exponential); 0 keeps it for ever.
     *
     * The spin a drag was carrying when the button came up does not stop dead -- it runs on and
     * decays at this rate, down to `idle_spin_deg_per_sec`.
     */
    float damping = 2.5f;

    /** @brief Tilt clamps, degrees either side of the axis standing straight up on screen. */
    float min_tilt_deg = -80.0f;
    float max_tilt_deg = 80.0f;

    bool invert_x = false; /**< Flips which way a horizontal drag spins. */
    bool invert_y = false; /**< Flips which way a vertical drag tilts. */

    /** @brief Fastest a fling may leave the model spinning, degrees per second. */
    float max_spin_deg_per_sec = 540.0f;

    // --- Per-frame input, written by Engine before Scene::update() ---

    /** @brief This frame's drag in pixels, already gated by `drag_button` (+y is DOWN the screen). */
    glm::vec2 drag_input = glm::vec2(0.0f);

    /** @brief Whether the drag button is held this frame; false lets a fling run on. */
    bool dragging = false;

    /**
     * @brief The camera's right and up axes in world space, for the tilt.
     *
     * Pushed in like the rest of the input, and defaulted to the world axes an orbit camera at
     * yaw 0 looks along, so the component still behaves sensibly in a test with no camera.
     */
    glm::vec3 view_right = glm::vec3(1.0f, 0.0f, 0.0f);

    /** @brief Seeds `spin_deg`/`tilt_deg` from the owner's authored Z and X rotation. */
    void start() override;

    /**
     * @brief Advances the spin by one frame of drag and writes the owner's rotation.
     * @param delta_time Frame delta time in seconds; a non-positive value is a no-op.
     */
    void update(float delta_time) override;

    /** @brief Rotation about `axis` so far, degrees. */
    float spin_deg() const { return spin_deg_; }
    /** @brief Tilt about the view's right axis, degrees, within [min_tilt_deg, max_tilt_deg]. */
    float tilt_deg() const { return tilt_deg_; }
    /** @brief Spin the model is carrying, degrees per second -- the fling `damping` decays. */
    float spin_velocity_deg_per_sec() const { return spin_velocity_; }

private:
    float spin_deg_     = 0.0f;
    float tilt_deg_     = 0.0f;
    float spin_velocity_ = 0.0f;  /**< Degrees per second, from the last drag; decays at `damping`. */
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_TURNTABLE_H
