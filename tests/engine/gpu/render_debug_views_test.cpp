/**
 * @file render_debug_views_test.cpp
 * @brief Every debug_view channel renders something -- not a uniformly black frame (the
 *        [[ssao-debug-view-broken]] failure mode, where the post chain silently ate a view) -- and
 *        differs from debug_view: off; the contact-shadow channel is static at rest and draws the
 *        LIVE march.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("render_debug_views");

using namespace toy::test;

/**
 * @brief Every debug_view channel renders something -- not a uniformly black frame (the
 * [[ssao-debug-view-broken]] failure mode this pass replaces, where a debug view came out
 * fully black because the post chain silently ate it) -- and differs from debug_view: off.
 *
 * The G-buffer/lighting/ssao/ssr channels share one Engine (debug_view is RUNTIME, so
 * switching between them needs no reconstruction), and the `lines` overlay rides along;
 * contact_shadows (with shadows_enabled off) and dof + volumetrics need STARTUP-FIXED feature
 * flags and so get an Engine each -- three in all. contact_shadows is
 * additionally checked against its own on/off switch, so "differs from off" can't be
 * coincidence -- the same proof-of-life the shipped contact-shadow term itself relies on.
 */
COOPA_TEST(every_debug_view_channel_renders) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    {
        toy::core::AppConfig config =
            make_test_config("assets/scenes/demos/pixel_demo/scene.yaml", 640, 360, 160, 90);
        toy::core::Engine engine(std::move(config));
        tick_frames(engine, kNoiseCycle);
        const Frame off_frame = engine.capture_image(true);

        static const char* kChannels[] = {
            "albedo", "normals", "roughness", "metallic", "emissive", "material_ao",
            "world_pos", "depth", "velocity", "direct", "indirect", "shadows", "ssao",
            "ssr", "ssr_confidence", "ssgi",
        };
        for (const char* name : kChannels) {
            engine.render_config().debug_view = name;
            tick_frames(engine, kNoiseCycle);
            const Frame f = engine.capture_image(true);
            const std::string label = std::string("debug_view '") + name + "'";
            const long long lit = count_nonblack(f);
            expect_at_least(lit, 1, label + " is not a uniformly black frame");
            expect_at_least(count_diff(f, off_frame), 1, label + " differs from debug_view: off");
            if (lit == 0) dump_frame(f, std::string("debug_view_") + name + "_black");
        }

        // lines: an overlay on the NORMAL image, not a replacement (unlike every view above) --
        // must render fine even in a scene with no physics collider to draw a wireframe for.
        engine.render_config().debug_view = "lines";
        tick_frames(engine, kNoiseCycle);
        expect_at_least(count_nonblack(engine.capture_image(true)), 1, "debug_view 'lines' still renders the normal image");
    }

    // contact_shadows: shadows_enabled off isolates the march as the only occlusion term --
    // see pixel_lighting.frag's own doc on this combination.
    {
        toy::core::AppConfig config =
            make_test_config("assets/scenes/demos/pixel_demo/scene.yaml", 640, 360, 160, 90);
        config.render.shadows_enabled         = false;
        config.render.contact_shadows_enabled = true;
        toy::core::Engine engine(std::move(config));
        tick_frames(engine, kNoiseCycle);
        const Frame off_frame = engine.capture_image(true);

        engine.render_config().debug_view = "contact_shadows";
        tick_frames(engine, kNoiseCycle);
        const Frame contact_frame = engine.capture_image(true);
        expect_at_least(count_nonblack(contact_frame), 1,
                        "debug_view 'contact_shadows' is not a uniformly black frame");
        expect_at_least(count_diff(contact_frame, off_frame), 1,
                        "debug_view 'contact_shadows' differs from debug_view: off");

        // contact_shadow_length is RUNTIME -- shortening the march's reach substantially
        // shrinks the occluded area, proving the channel draws the LIVE march rather than a
        // stale buffer. Not all the way to zero pixels: even a zero-length march still
        // samples right at the bias offset (see toy_contact_shadow's own doc), which stays
        // "in contact" for a receiver texel directly against an object's base -- shortening
        // the reach removes everything BEYOND that, which is the bulk of the effect.
        const long long before = count_nonblack(contact_frame);

        // The march reads the receiver position and DIFFERENTIATES it (contact_shadow_body.glsl's
        // CALLER CONTRACT) to undo the TAA sub-pixel jitter. Feed that a point-sampled value, or
        // evaluate it under non-uniform control flow, and `bias` comes out wrong per texel, the
        // ray self-intersects, and the resulting speckle is keyed to the TAA jitter phase -- so
        // it CHANGES every frame even with nothing moving. That temporal signature is what this
        // asserts, because it is the one property a correct term has regardless of scene: with
        // FIXED_DT=0 and a static camera, the channel must be the same image twice.
        //
        // A spatial noise bound is deliberately NOT asserted here. The term is legitimately
        // high-frequency on stepped geometry -- on voxel terrain it is a one-pixel line along
        // every step edge -- so the number is scene- and resolution-dependent (measured 7.6
        // levels/px accumulated at 1920x1080 on this scene, 19 raw, 32 raw at this test's
        // 160x90) with no threshold that means the same thing across them.
        engine.tick();
        const Frame contact_again = engine.capture_image(true);
        const double contact_drift = mean_abs_delta(contact_again, contact_frame);
        expect(contact_drift <= 0.05,
               "debug_view 'contact_shadows' is static when the camera and clock are");
        if (contact_drift > 0.05) {
            std::cerr << "         contact channel drifts " << contact_drift
                      << " levels/px per frame at rest (expected ~0)\n";
            dump_frame(contact_frame, "debug_view_contact_unstable");
        }

        engine.render_config().contact_shadow_length = 0.0f;
        tick_frames(engine, kNoiseCycle);
        const Frame zero_length = engine.capture_image(true);
        const long long after = count_nonblack(zero_length);
        expect(after < before / 2,
               "debug_view 'contact_shadows' shrinks by more than half when contact_shadow_length is 0");
    }

    // dof / volumetrics draw through their OWN pass's existing debug branch (dof_composite.frag
    // / volumetrics_march.frag), not debug_view_pass_ -- see the DebugView enum's own doc -- so each
    // needs its feature enabled at STARTUP, unlike the channels above. One Engine carries both.
    {
        toy::core::AppConfig config =
            make_test_config("assets/scenes/demos/pixel_demo/scene.yaml", 640, 360, 160, 90);
        config.render.dof_enabled         = true;
        config.render.volumetrics_enabled = true;
        toy::core::Engine engine(std::move(config));
        tick_frames(engine, kNoiseCycle);
        const Frame off_frame = engine.capture_image(true);
        engine.render_config().debug_view = "dof";
        tick_frames(engine, kNoiseCycle);
        const Frame dof_frame = engine.capture_image(true);
        expect_at_least(count_nonblack(dof_frame), 1, "debug_view 'dof' is not a uniformly black frame");
        expect_at_least(count_diff(dof_frame, off_frame), 1, "debug_view 'dof' differs from debug_view: off");
        engine.render_config().debug_view = "volumetrics";
        tick_frames(engine, kNoiseCycle);
        const Frame vol_frame = engine.capture_image(true);
        expect_at_least(count_nonblack(vol_frame), 1, "debug_view 'volumetrics' is not a uniformly black frame");
        expect_at_least(count_diff(vol_frame, off_frame), 1, "debug_view 'volumetrics' differs from debug_view: off");
    }
}
