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

inline State& state() {
    static State s;
    return s;
}

/** @brief Starts watching (main thread, after the window exists). No-op off macOS. */
inline void install() {
    State& st = state();
    if (st.installed) return;
    st.installed = true;
#if defined(__APPLE__) && defined(__BLOCKS__)
    using Id = void*;
    Id ns_event = reinterpret_cast<Id>(objc_getClass("NSEvent"));
    if (!ns_event) return;
    constexpr unsigned long long kScrollWheel = 22, kMagnify = 30;   // NSEventType values
    const unsigned long long mask = (1ull << kScrollWheel) | (1ull << kMagnify);
    Id (^handler)(Id) = ^Id(Id ev) {
        auto uval = [ev](const char* name) { return reinterpret_cast<unsigned long (*)(Id, SEL)>(objc_msgSend)(ev, sel_registerName(name)); };
        const unsigned long type = uval("type");
        State& s = state();
        if (type == kMagnify) {
            s.magnify += reinterpret_cast<double (*)(Id, SEL)>(objc_msgSend)(ev, sel_registerName("magnification"));
        } else if (type == kScrollWheel) {
            const unsigned long phase = uval("phase"), momentum = uval("momentumPhase");
            s.scroll_is_trackpad = phase != 0 || momentum != 0;
            s.momentum = momentum != 0;
        }
        return ev;   // monitors observe; GLFW still gets the event
    };
    reinterpret_cast<Id (*)(Id, SEL, unsigned long long, Id)>(objc_msgSend)(
        ns_event, sel_registerName("addLocalMonitorForEventsMatchingMask:handler:"), mask, reinterpret_cast<Id>(handler));
#endif
}

/** @brief The pinch accumulated since the last call (consumes it). */
inline double take_magnify() {
    const double m = state().magnify;
    state().magnify = 0.0;
    return m;
}

}  // namespace toy::editor::trackpad

#endif  // TOYEDITOR_APP_TRACKPAD_H
