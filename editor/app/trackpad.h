/**
 * @file trackpad.h
 * @brief What GLFW doesn't say about trackpads: whether a scroll came from a trackpad (or a
 *        Magic Mouse) rather than a wheel, and pinch-to-zoom.
 *
 * On macOS an AppKit local event monitor (through the Objective-C runtime, as gfxcoopa's
 * Window::set_icon() talks to AppKit) watches scroll and magnify events as GLFW pumps them:
 * gesture scrolls carry a phase, wheel notches don't; magnify is the pinch. Elsewhere, and
 * before install(), it reports a wheel and no pinch -- the editor's mouse behaviour.
 */

#ifndef TOYEDITOR_APP_TRACKPAD_H
#define TOYEDITOR_APP_TRACKPAD_H

#if defined(__APPLE__) && defined(__BLOCKS__)
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace toy::editor::trackpad {

struct State {
    bool installed = false;
    bool scroll_is_trackpad = false;   ///< The latest scroll event was a gesture (has a phase).
    bool momentum = false;             ///< ...and was momentum (the coast after the fingers lift).
    double magnify = 0.0;              ///< Pinch since the last take_magnify(): + = spread (zoom in).
};

State& state();

/** @brief Starts watching (main thread, after the window exists). No-op off macOS. */
void install();

/** @brief The pinch accumulated since the last call (consumes it). */
double take_magnify();

}  // namespace toy::editor::trackpad

#endif  // TOYEDITOR_APP_TRACKPAD_H
