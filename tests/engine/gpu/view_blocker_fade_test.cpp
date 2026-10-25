/**
 * @file view_blocker_fade_test.cpp
 * @brief ViewBlockerFade end to end: a wall between the camera and the subject detects that it
 *        is in the way and erodes itself out of the shot, and an identical wall without the
 *        component does not.
 *
 * The unit suite pins the fade CURVE as a pure function. This pins the part that curve can
 * never reach: ViewBlockerSystem finding the camera and the XraySubject, laying a capsule
 * between them, excluding the subject's own subtree from the query, walking a hit collider's
 * owners up to the component, and the cutoff it writes actually reaching the G-buffer's alpha
 * test. Every one of those is a hand-written link that nothing else in the build would notice
 * breaking.
 *
 * Every measurement here is a pixel DIFF against the control scene rather than a match on the
 * wall's colour. That is deliberate: the wall's on-screen colour is its albedo after lighting,
 * exposure and the ACES curve, so a literal expected colour is a guess about the whole post
 * chain, and a wrong guess reads as "the wall never appeared" rather than as a broken test.
 * Against a control that differs ONLY by the component, the diff is the effect itself.
 *
 * The fixtures (tests/fixtures/scenes/view_blocker) are one template with one substitution --
 * see their shared header.
 */

#include <coopa/testing/test.h>

#include <iostream>
#include <string>

#include <toyengine/core/engine.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("view_blocker_fade");

using namespace toy::test;

namespace {

const char* kFading = "tests/fixtures/scenes/view_blocker/scene_fading.yaml";
const char* kFixed  = "tests/fixtures/scenes/view_blocker/scene_fixed.yaml";

/// Renders a fixture after `frames` ticks of simulation.
Frame render(const char* scene, int frames) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");   // the fade is timed, so time has to pass
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config(scene, 640, 360, 240, 135);
    config.render.soft_lighting = true;
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, frames);
    return engine.capture_image(/*low_res=*/true);
}

/// How much a frame departs from the solid-wall control, in pixels.
long long departure_from_control(const Frame& f, const Frame& control) {
    return count_diff(f, control);
}

} // namespace

/**
 * @brief A wall in the way erodes; the same wall without the component does not.
 *
 * Both halves matter. The departure growing proves the detection and the cutoff plumbing work;
 * the control staying put proves the dissolve is driven by the component rather than by
 * something incidental to the fixture -- the CUTOUT material and the noise mask are present in
 * BOTH scenes, so a mask that ate the wall on its own would fail here rather than pass.
 */
COOPA_TEST(a_wall_blocking_the_view_dissolves_and_one_without_the_component_does_not) {
    const Frame control = render(kFixed, 60);
    const Frame early   = render(kFading, 1);    // the fade has barely started
    const Frame late    = render(kFading, 60);   // ~1 s: well past fade_in_time (0.15 s)

    const long long d_early = departure_from_control(early, control);
    const long long d_late  = departure_from_control(late, control);

    // The control must itself be stable, or the diffs below measure noise. Same scene, two
    // tick counts a noise cycle apart.
    const long long control_drift = count_diff(render(kFixed, 60 + kNoiseCycle), control);
    require(control_drift <= kDriftBudget,
            "view_blocker: the control scene is stable (" + std::to_string(control_drift) + " px)");

    expect(d_late > 1000, "view_blocker: a blocking wall has visibly eroded after a second");
    expect(d_late > 8 * (d_early + 1), "view_blocker: and it was still near-solid at the start");
    if (!(d_late > 1000 && d_late > 8 * (d_early + 1))) {
        std::cout << "  departure from the solid control: early=" << d_early
                  << " late=" << d_late << " (control drift " << control_drift << ")\n";
        dump_frame(control, "view_blocker_control");
        dump_frame(early, "view_blocker_early");
        dump_frame(late, "view_blocker_late");
    }
}

/**
 * @brief The dissolve is progressive rather than a single pop.
 *
 * `fade_in_time` is the one knob an author reaches for, and it only means anything if the wall
 * passes through intermediate states. A wall that vanished on the first frame it was blocked
 * would satisfy the test above and ignore the timing entirely.
 */
COOPA_TEST(the_dissolve_passes_through_intermediate_coverage) {
    const Frame control = render(kFixed, 60);
    const long long early = departure_from_control(render(kFading, 1), control);
    const long long mid   = departure_from_control(render(kFading, 6), control);   // ~0.1 s, inside fade_in_time
    const long long late  = departure_from_control(render(kFading, 60), control);

    expect(early < mid && mid < late, "view_blocker: the wall erodes progressively, not in one step");
    expect(mid > 0 && mid < late, "view_blocker: there is a real partway state");
    if (!(early < mid && mid < late)) {
        std::cout << "  departure from control: early=" << early << " mid=" << mid
                  << " late=" << late << "\n";
    }
}

/**
 * @brief The wall's SHADOW goes with it.
 *
 * This is the property the CUTOUT approach buys for free and a surface-shader dissolve would
 * not: the shadow backbone (gfx/surface/shadow_fs.glsl) runs the same alpha-mask test the
 * G-buffer does. A wall that was visually gone but still cast a solid shadow would give the
 * trick away immediately, so it is worth pinning rather than assuming.
 *
 * Measured as the loss of dark pixels against the control rather than by naming a region, so
 * it does not depend on where the sun happens to throw the shadow.
 */
COOPA_TEST(the_dissolving_wall_stops_casting_its_full_shadow) {
    const Frame control = render(kFixed, 60);
    const Frame faded   = render(kFading, 60);
    const long long dark_control = count_near_color(control, 20, 22, 30, 45);
    const long long dark_faded   = count_near_color(faded, 20, 22, 30, 45);
    expect(dark_faded < dark_control,
           "view_blocker: an eroded wall shades less of the scene than the solid control");
    if (dark_faded >= dark_control) {
        std::cout << "  dark pixels: control=" << dark_control << " faded=" << dark_faded << "\n";
        dump_frame(faded, "view_blocker_shadow_faded");
        dump_frame(control, "view_blocker_shadow_control");
    }
}
