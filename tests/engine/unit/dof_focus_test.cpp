/**
 * @file dof_focus_test.cpp
 * @brief Depth of field's CPU half (toy_render_math.h): the view-space depth the autofocus measures,
 *        and the exponential focus chase (monotone, framerate-independent). The blur itself is a
 *        GPU pass, covered by render_debug_views' `dof` channel.
 */

#include <coopa/testing/test.h>

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#include <toyengine/render/toy_render_math.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("dof_focus");

COOPA_TEST(view_space_depth_is_distance_along_the_view_axis) {
    // Camera at (0,0,5) looking at the origin, standard RH lookAt -- glm's usual view-space
    // convention (camera looks down its own -Z) applies regardless of which world axis this
    // engine treats as "up" (CameraController's rig is Z-up; that only affects how a scene's
    // camera Transform is built, not this pure view-matrix math).
    glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    expect_near(toy::render::view_space_depth(view, glm::vec3(0.0f)), 5.0f, 1e-4f,
                "view_space_depth: point at the look-at target is 5m in front of a camera 5m away");
    expect_near(toy::render::view_space_depth(view, glm::vec3(0.0f, 0.0f, 2.0f)), 3.0f, 1e-4f,
                "view_space_depth: a point 3m closer to the camera reports 3m less depth");

    // A point behind the eye (camera at z=5 looking toward -z; z=8 is on the far side of the
    // camera from the look-at target) must come back negative -- the case
    // resolve_dof_focus_()'s `depth > 0.0f` guard exists to catch.
    expect(toy::render::view_space_depth(view, glm::vec3(0.0f, 0.0f, 8.0f)) < 0.0f,
           "view_space_depth: a point behind the camera is negative");
}

COOPA_TEST(focus_chase_is_monotone_and_framerate_independent) {
    // rate <= 0 snaps straight to target, regardless of dt.
    expect(toy::render::exp_smooth_toward(0.0f, 10.0f, 0.0f, 0.5f) == 10.0f,
           "exp_smooth_toward: rate <= 0 snaps to target");
    expect(toy::render::exp_smooth_toward(0.0f, 10.0f, -1.0f, 0.5f) == 10.0f,
           "exp_smooth_toward: negative rate also snaps to target");

    // Monotone convergence toward the target, never overshooting it. Asserted ONCE over the
    // whole second rather than per step: 60 identical assertions say nothing 1 cannot.
    float value = 0.0f;
    bool  monotone = true;
    for (int i = 0; i < 60; ++i) {
        const float next = toy::render::exp_smooth_toward(value, 10.0f, 8.0f, 1.0f / 60.0f);
        if (!(next > value && next <= 10.0f)) monotone = false;
        value = next;
    }
    expect(monotone, "exp_smooth_toward: every step of a second moves toward the target without overshooting");
    expect(value > 9.0f, "exp_smooth_toward: converges close to target after 1 second at rate 8");

    // Framerate independence: two half-steps at dt land at the same place as one step at 2*dt --
    // the property the 1 - exp(-rate*dt) form buys over a naive linear lerp.
    const float two_steps = toy::render::exp_smooth_toward(
        toy::render::exp_smooth_toward(2.0f, 20.0f, 5.0f, 0.1f), 20.0f, 5.0f, 0.1f);
    const float one_step = toy::render::exp_smooth_toward(2.0f, 20.0f, 5.0f, 0.2f);
    expect_near(two_steps, one_step, 1e-4f,
                "exp_smooth_toward: two dt steps match one 2*dt step (framerate-independent)");
}
