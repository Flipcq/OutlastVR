// Host-only regression test; no Windows/OpenXR installation required:
// g++ -std=c++17 -Wall -Wextra -Werror tests/use_input_test.cpp -o /tmp/use_input_test
// /tmp/use_input_test
#include "../src/ol_use_input.h"
#include <cassert>
#include <cstdio>

static OLXrPad Sample(OLUsePressBuffer& buffer, uint64_t now,
                     unsigned buttons = 0, float grip = 0,
                     bool enabled = true, bool focused = true) {
    OLXrPad pad = {};
    pad.buttons = buttons;
    pad.gripL = grip;
    pad.moveY = 0.75f;
    buffer.Apply(pad, now, enabled, focused);
    assert(pad.moveY == 0.75f && pad.gripL == grip);
    assert((pad.buttons & ~OLXR_BTN_X) == (buttons & ~OLXR_BTN_X));
    return pad;
}

int main() {
    OLUsePressBuffer buffer;
    assert(!Sample(buffer, 0).buttons);

    // XR samples at 11/22/33 ms; the game only polls at 0/33 ms. Without
    // buffering, publishing the released state at 22 ms erases this whole tap.
    Sample(buffer, 11, OLXR_BTN_X);
    Sample(buffer, 22);
    assert(Sample(buffer, 33).buttons & OLXR_BTN_X);
    assert(Sample(buffer, 110).buttons & OLXR_BTN_X);
    assert(!(Sample(buffer, 111).buttons & OLXR_BTN_X));

    // Sustained Use still works (slow opening), followed by one bounded tail.
    Sample(buffer, 200, OLXR_BTN_X);
    Sample(buffer, 700, OLXR_BTN_X);
    assert(Sample(buffer, 799).buttons & OLXR_BTN_X);
    assert(!(Sample(buffer, 800).buttons & OLXR_BTN_X));
    assert(Sample(buffer, 850, OLXR_BTN_X).buttons & OLXR_BTN_X);

    // Grip + X stays a raw chord. A released X cannot become a recording
    // shortcut when the grip is squeezed during the extension.
    assert(!Sample(buffer, 860, 0, 1.0f).buttons);
    assert(!Sample(buffer, 870).buttons);
    const auto chord = Sample(buffer, 900, OLXR_BTN_X, 1.0f);
    assert(chord.buttons == OLXR_BTN_X && !chord.usePressBuffered);
    assert(!Sample(buffer, 910, 0, 1.0f).buttons);

    // Focus loss, sync failure (Reset), and the ini fallback discard the tail.
    Sample(buffer, 1000, OLXR_BTN_X);
    assert(!Sample(buffer, 1010, 0, 0, true, false).buttons);
    assert(!Sample(buffer, 1020).buttons);
    Sample(buffer, 1100, OLXR_BTN_X);
    buffer.Reset();
    assert(!Sample(buffer, 1110).buttons);
    const auto legacy = Sample(buffer, 1200, OLXR_BTN_X, 0, false);
    assert(legacy.buttons == OLXR_BTN_X && !legacy.usePressBuffered);
    assert(!Sample(buffer, 1210, 0, 0, false).buttons);

    // Other buttons are never stretched; long Windows uptime is safe.
    constexpr uint64_t late = (uint64_t(1) << 32) + 1000;
    assert(Sample(buffer, late, OLXR_BTN_X | OLXR_BTN_A).buttons == (OLXR_BTN_X | OLXR_BTN_A));
    assert(Sample(buffer, late + 10, OLXR_BTN_B).buttons == (OLXR_BTN_X | OLXR_BTN_B));
    assert(Sample(buffer, late + 100, OLXR_BTN_Y).buttons == OLXR_BTN_Y);
    puts("PASS: XR/game timing, expiry, hold, grip chords, focus, reset, fallback, other buttons, long uptime");
}
