#pragma once
#include <cstdint>
#include "ol_xr.h"

// Worker-owned: keep a short Use press visible across XR publications, before
// the slower XInput reader can miss it. Match the old 100 ms release extension.
struct OLUsePressBuffer {
    uint64_t until = 0;

    void Reset() { until = 0; }

    void Apply(OLXrPad& pad, uint64_t now, bool enabled, bool focused) {
        // A grip chord uses the legacy mapper. Cancel a previous X tail before
        // it could be reinterpreted as a fresh recording shortcut.
        pad.usePressBuffered = enabled && focused && pad.gripL <= 0.6f;
        if (!pad.usePressBuffered) { Reset(); return; }
        if (pad.buttons & OLXR_BTN_X) until = now + 100;
        if (now < until) pad.buttons |= OLXR_BTN_X;
    }
};
