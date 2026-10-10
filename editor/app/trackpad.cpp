#include "editor/app/trackpad.h"

namespace toy {
namespace editor {
namespace trackpad {

State& state() {
    static State s;
    return s;
}

void install() {
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

double take_magnify() {
    const double m = state().magnify;
    state().magnify = 0.0;
    return m;
}

} // namespace trackpad
} // namespace editor
} // namespace toy
