#pragma once
#include "ol_xr.h"

// One owner calls this under the proxy's input lock. A failed release retains
// ownership so the watchdog can retry; blocked/held buttons never become clicks
// merely because a menu, grip chord or focus state changes.
struct OLMouseUse {
    static constexpr unsigned long InputTag = 0x4F4C5652; // OLVR synthetic mouse events
    bool previousDown = false;
    bool held = false;
    bool releasePending = false;

    template<class Send> void Sample(bool down, bool allowed, bool canStart, Send send) {
        const bool rising = down && !previousDown;
        previousDown = down;
        if (held && (releasePending || !down || !allowed)) {
            releasePending = true;
            if (send(false)) { held = false; releasePending = false; }
            return; // a release retry cannot turn into a delayed new click
        }
        if (!held && rising && allowed && canStart) held = send(true);
    }
    template<class Send> void Cancel(Send send) {
        Sample(true, false, false, send); // rearm only after a sampled release
    }
};

// The same physical button must not become a different action when crossing a
// menu boundary (e.g. X accepting a menu must not crouch on Resume).
struct OLFaceButtonGate {
    bool inMenu = false;
    unsigned previous = 0, blocked = 0;
    unsigned Apply(unsigned buttons, bool menu) {
        if (menu != inMenu) blocked |= previous & buttons & (OLXR_BTN_X | OLXR_BTN_B);
        blocked &= buttons;
        previous = buttons;
        inMenu = menu;
        return buttons & ~blocked;
    }
};
