"""Every press reaches the game, whatever the game's own frame time is.

Runs the real LatchPresses / LatchForget (src/ol_xr.cpp, the publisher) and the real
ReliablePadButtons (src/d3d9_proxy.cpp, the consumer) against a simulated clock: a finger holds a
button for N ms, the XR worker publishes at display rate, UE3 polls the pad at its own rate. Then it
asserts the press was seen by at least one poll. Windows/game APIs are synthetic - it proves the
timing, not the hardware. Run: python3 tests/pad_press_test.py (g++ required).
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
xr = (ROOT / 'src/ol_xr.cpp').read_text()
proxy = (ROOT / 'src/d3d9_proxy.cpp').read_text()

latch = xr[xr.index('static const ULONGLONG kPadPressHoldMs'):xr.index('// Sync + publish.')]
reliable = proxy[proxy.index('static WORD ReliablePadButtons('):proxy.index('static OLFaceButtonGate g_faceGate;')]

STUBS = r'''
#include <cassert>
#include <cstdio>
#define _countof(a) (sizeof(a)/sizeof(a[0]))
using WORD = unsigned short;
using DWORD = unsigned long;
using LONG = long;
using ULONGLONG = unsigned long long;
static ULONGLONG now = 100000;
static ULONGLONG GetTickCount64() { return now; }
'''

SIM = r'''
// One press of the A button: the finger holds it from pressStart for pressMs, the worker publishes every
// xrMs, the game polls every gameMs. True = some poll saw the button down.
// The publisher speaks OLXR_BTN_* (A = bit 0) and the game speaks XInput (A = 0x1000), exactly like the
// two real halves; MapA is the one line of MapPadButtons this needs.
static unsigned published = 0;
static WORD MapA(unsigned buttons) { return (buttons & 1u) ? 0x1000 : 0; }
static bool PressReaches(int pressMs, int xrMs, int gameMs, int pressStart, bool latch, bool* stuckLater) {
    const ULONGLONG t0 = now;
    bool seen = false;
    if (stuckLater) *stuckLater = false;
    for (int t = 0; t <= 900; ++t) {
        now = t0 + t;
        const unsigned phys = (t >= pressStart && t < pressStart + pressMs) ? 1u : 0u;   // A
        if (t % xrMs == 0) published = latch ? LatchPresses(phys) : phys;
        if (t % gameMs == 0) {
            const WORD w = ReliablePadButtons(MapA(published));
            if (w & 0x1000) { seen = true; if (t >= 500 && stuckLater) *stuckLater = true; }
        }
    }
    now = t0 + 5000;              // let every static expire between cases
    LatchPresses(0);
    ReliablePadButtons(0);
    return seen;
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // 1. An ordinary quick tap at 60 fps: seen before this change too - no regression.
    assert(PressReaches(20, 11, 16, 100, true, nullptr));
    assert(PressReaches(20, 11, 16, 100, false, nullptr));

    // 2. THE BUG. A 20 ms tap that starts and ends between two game polls - a 10 fps hitch, which the
    //    log records as frames slower than 40 ms and which is exactly what a doorway or a ledge costs.
    //    Without the publisher latch the game never saw the press at all.
    assert(!PressReaches(20, 11, 100, 110, false, nullptr));
    assert(PressReaches(20, 11, 100, 110, true, nullptr));

    // 3. The same at 5 fps (a 0.2 s freeze, the level-streaming stalls the tester notes describe).
    assert(!PressReaches(15, 11, 200, 105, false, nullptr));
    assert(PressReaches(15, 11, 200, 105, true, nullptr));

    // 4. A press is not stretched forever: it is down for the publisher's hold plus the consumer's own
    //    100 ms tail, and a second later the game reads nothing at all.
    bool stuck = false;
    assert(PressReaches(20, 11, 16, 100, true, &stuck));
    assert(!stuck);

    // 5. Two presses 400 ms apart stay two presses (the latch never welds them into one long hold).
    {
        const ULONGLONG t0 = now;
        int edges = 0; bool was = false;
        for (int t = 0; t <= 900; ++t) {
            now = t0 + t;
            const unsigned phys = ((t >= 100 && t < 120) || (t >= 500 && t < 520)) ? 0x1000u : 0u;
            if (t % 11 == 0) published = LatchPresses(phys);
            if (t % 16 == 0) {
                const bool down = (ReliablePadButtons((WORD)published) & 0x1000) != 0;
                if (down && !was) ++edges;
                was = down;
            }
        }
        assert(edges == 2);
        now = t0 + 5000; LatchPresses(0); ReliablePadButtons(0);
    }

    // 6. The recenter chord (both stick clicks) swallows its two buttons, and the latch must not
    //    bring them back: the player is not sprinting because they recentred.
    {
        const ULONGLONG t0 = now;
        now = t0;
        assert(LatchPresses(1u << 4) == (1u << 4));        // LSTICK down
        LatchForget(1u << 4);                              // the chord swallows it
        now = t0 + 20;
        assert((LatchPresses(0) & (1u << 4)) == 0);        // and it stays swallowed
        now = t0 + 5000;
    }
    std::printf("Pad presses: tap between polls, 10/5 fps hitches, no welding, chord stays swallowed.\n");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'press.cpp'
    exe = Path(tmp) / 'press-test'
    src.write_text(STUBS + latch + reliable + SIM)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

# The wiring itself, which is what makes the guarantee hold in the shipped build.
assert re.search(r'OLProxy_MouseUseSample\(p\.buttons.*?\);\s*\n\s*//.*PADLATCH', xr), \
    'the mouse path must keep the raw edges: latch only what the game polls'
assert xr.index('p.buttons = LatchPresses(p.buttons);') < xr.index('g_padPub = p; g_padPubValid = 1;'), \
    'the latch must run on the published snapshot, before it is published'
assert 'LatchForget(OLXR_BTN_LSTICK | OLXR_BTN_RSTICK)' in xr, 'the recenter chord must forget its latch'
assert 'kPadPressHoldMs = 150' in xr, 'hold long enough to survive a 150 ms game frame'
print('Pad press latch: wired to the published snapshot, mouse edges untouched, chord forgotten.')
