"""Run the actual camcorder face-gesture state machine (right-stick hold) with synthetic state.
python3 tests/camcorder_face_test.py (g++). Does not replace Windows/game/VR testing.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
interact = (ROOT / 'src/ol_interact.cpp').read_text()
start = interact.index('static bool s_rsDown = false, s_rsLong = false, s_rsShort = false;')
end = interact.index('\nstatic void Holster(', start)
gesture = interact[start:end]

STUBS = r'''
#include <cassert>
#include <cstdarg>
#include <cstdio>
using ULONGLONG = unsigned long long;
struct OLXrPad { unsigned buttons; };
enum { OLXR_BTN_RSTICK = 1u << 5 };
constexpr ULONGLONG OLVR_CAM_HOLD_MS = 800;
static ULONGLONG now = 1000;
static int s_camUp = 0;
static bool s_faceLock = false, s_facePending = false, s_faceLowering = false;
static ULONGLONG s_facePendingAt = 0, s_faceLowerAt = 0;
static bool s_camByGesture = false, s_eyeArmed = true;
static ULONGLONG s_inEyeSince = 0, s_camPulseUntil = 0, s_camCooldown = 0;
static int toggles = 0, haptics = 0;
static void RequestCamToggle(ULONGLONG t) { s_camPulseUntil = t + 120; s_camCooldown = t + 650; ++toggles; }
static void OLXR_HapticPulse(int, float, int) { ++haptics; }
static void ILog(const char*, ...) {}
'''

CASES = r'''
static void Step(bool pressed, ULONGLONG ms) {
    OLXrPad p = {}; if (pressed) p.buttons = OLXR_BTN_RSTICK;
    now += ms; CamHoldGesture(now, p);
}
static void Reset() {
    s_camUp = 0; s_faceLock = s_facePending = s_faceLowering = false;
    s_facePendingAt = 0; s_faceLowerAt = 0; s_camByGesture = false; s_eyeArmed = true; s_inEyeSince = 0;
    s_camPulseUntil = s_camCooldown = 0; toggles = haptics = 0;
    s_rsDown = s_rsLong = s_rsShort = false; s_rsDownAt = 0;
}
int main() {
    // A short click is still the night-vision switch and does nothing to the camcorder.
    Reset();
    Step(true, 0); Step(true, 200); Step(false, 0);
    assert(s_rsShort && !s_facePending && !s_faceLock && toggles == 0);

    // Hold from down: the game's own raise is requested; the pin lands when the game reports up.
    Reset();
    Step(true, 0); Step(true, 900);
    assert(s_facePending && !s_faceLock && toggles == 1 && haptics == 0);
    Step(false, 0); assert(!s_rsShort);

    // Already up, not pinned: a held click pins it to the face with a buzz and no toggle.
    Reset(); s_camUp = 1;
    Step(true, 0); Step(true, 900);
    assert(s_faceLock && !s_facePending && toggles == 0 && haptics == 1);
    Step(false, 0); assert(!s_rsShort);

    // Pinned at the face: another held click asks the game to lower it (its own animation).
    Reset(); s_camUp = 1; s_faceLock = true;
    Step(true, 0); Step(true, 900);
    assert(s_faceLowering && s_faceLock && toggles == 1 && haptics == 1);
    Step(true, 1400); assert(toggles == 1);          // one action per press, however long it is held
    Step(false, 0); assert(!s_rsShort);

    // A 750 ms hold is still a click: no raise, no pin.
    Reset(); Step(true, 0); Step(true, 750);
    assert(!s_facePending && !s_faceLock && toggles == 0);
    Step(false, 0); assert(s_rsShort);

    puts("Camcorder face gesture: short click, raise, pin without animation, lower, one action per press.");
    return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory() as d:
        src = Path(d) / 'test.cpp'
        src.write_text(STUBS + gesture + CASES)
        exe = Path(d) / 'test'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
