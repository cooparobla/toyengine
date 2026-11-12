#pragma once

/**
 * @file view_helpers.h
 * @brief A fixed view for the device-free viewport tests (picking, gizmos, modal transforms).
 */

#include <glm/gtc/matrix_transform.hpp>

#include "editor/support/fixtures.h"
#include "editor/viewport/gizmo.h"

namespace toy::editor::testing {

/** @brief Camera 10 m back along -Y looking at the origin, Z up, in a 640 x 360 rect at (100, 50). */
inline ViewProj test_view_proj() {
    ViewProj vp;
    vp.view = glm::lookAt(glm::vec3(0, -10, 0), glm::vec3(0), glm::vec3(0, 0, 1));
    vp.proj = glm::perspective(glm::radians(50.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    vp.rect = {100, 50, 640, 360};
    return vp;
}

/** @brief A key press event (no mods unless given). */
inline coopa::input::KeyEvent key_press(coopa::input::Key k, coopa::input::Mods m = coopa::input::Mods::None) {
    coopa::input::KeyEvent e;
    e.key = k;
    e.action = coopa::input::KeyAction::Press;
    e.mods = m;
    return e;
}

} // namespace toy::editor::testing
