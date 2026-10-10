#pragma once

/**
 * @file imm_harness.h
 * @brief Drives uicoopa's immediate-mode layer headless: no font, no GPU, an 800 x 600 frame per
 *        call with a scripted mouse, typed characters and key events.
 */

#include <functional>
#include <utility>
#include <vector>

#include <uicoopa/immediate/imm.h>

#include "editor/support/fixtures.h"

namespace toy::editor::testing {

struct ImmHarness {
    coopa::ui::imm::Context ctx;
    coopa::ui::DrawList dl;
    glm::vec2 mouse{0.0f};
    bool down = false;
    bool prev_down = false;
    glm::vec2 scroll{0.0f};   ///< Wheel delta for the next frame only

    void frame(const std::function<void(coopa::ui::imm::Context&)>& fn, std::vector<uint32_t> chars = {},
               std::vector<coopa::input::KeyEvent> keys = {}, glm::vec2 delta = glm::vec2(0.0f)) {
        coopa::ui::imm::FrameInput in;
        in.mouse = mouse;
        in.mouse_delta = delta;
        in.down[0] = down;
        in.pressed[0] = down && !prev_down;
        in.released[0] = !down && prev_down;
        in.chars = std::move(chars);
        in.keys = std::move(keys);
        in.scroll = scroll;
        scroll = glm::vec2(0.0f);
        prev_down = down;
        dl.begin(coopa::ui::Rect{{0, 0}, {800, 600}});
        ctx.begin_frame(dl, in, {800, 600});
        fn(ctx);
        ctx.end_frame();
    }
    /** @brief Move to `at`, press, release: three frames. */
    void click(glm::vec2 at, const std::function<void(coopa::ui::imm::Context&)>& fn) {
        mouse = at;
        frame(fn);
        down = true;
        frame(fn);
        down = false;
        frame(fn);
    }
};

} // namespace toy::editor::testing
