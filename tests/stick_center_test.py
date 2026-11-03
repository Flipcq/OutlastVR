"""A deflect stick always walks, whatever a touch sensor claims (the Valve Index / Pimax report).

Runs the real StickCenter, ApplyDeadZone and TriggerUnit from src/ol_xr.cpp with a synthetic OpenXR
input. OpenXR itself is stubbed: this proves the rule the function now enforces - a stick that is
PUSHED is never turned into nothing, and no "rest point" is ever learned from a pushed stick - and
that the drift correction it exists for still works. Run: python3 tests/stick_center_test.py.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/ol_xr.cpp').read_text()
# The stick/trigger block, from the settings it reads to the end of StickCenter.
logic = source[source.index('static volatile long  g_stickAutoCenter'):
               source.index('// [HAPTICS] WORKER THREAD ONLY')]

STUBS = r'''
#include <cassert>
#include <cmath>
#include <cstdio>
using XrResult = int;
using XrBool32 = unsigned;
using XrStructureType = unsigned;
using XrPath = unsigned long long;
using XrAction = unsigned long long;
using XrSession = unsigned long long;
using ULONGLONG = unsigned long long;
constexpr XrStructureType XR_TYPE_ACTION_STATE_GET_INFO_VALUE = 58;
constexpr XrStructureType XR_TYPE_ACTION_STATE_BOOLEAN_VALUE = 23;
static constexpr int XR_SUCCESS = 0;
static bool XrSucceeded(XrResult r) { return r >= 0; }
struct XrActionStateGetInfo { XrStructureType type; const void* next; XrAction action; XrPath subactionPath; };
struct XrActionStateBoolean {
    XrStructureType type; void* next; XrBool32 currentState, changedSinceLastSync;
    long long lastChangeTime; XrBool32 isActive;
};
// The synthetic controller: touchState is what /input/thumbstick/touch reports.
static bool touchActive = true, touchState = true;
static XrResult GetBool(XrSession, const XrActionStateGetInfo*, XrActionStateBoolean* st) {
    st->isActive = touchActive ? 1 : 0;
    st->currentState = touchState ? 1 : 0;
    return XR_SUCCESS;
}
struct FnTable { XrResult (*getActionStateBoolean)(XrSession, const XrActionStateGetInfo*, XrActionStateBoolean*); };
static FnTable g_fn = { GetBool };
static XrSession g_session = 1;
static XrAction g_aStickTouch = 7;
static XrPath g_handPath[2] = { 1, 2 };
static ULONGLONG now = 100000;
static ULONGLONG GetTickCount64() { return now; }
static void XLOG(const char*, ...) {}
'''

CHECKS = r'''
static bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }
// One sample: push the stick to (x,y), report `thumb` (true = touch sensor sees a thumb).
static void stick(int hand, float x, float y, bool thumb, float* ox, float* oy) {
    touchState = thumb; *ox = x; *oy = y;
    StickCenter(hand, ox, oy);
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    float x, y;

    // A: a healthy controller (touch works) is untouched, resting or pushed - the old promise.
    for (int i = 0; i < 100; ++i) { now += 16; stick(0, 0.0f, 0.0f, false, &x, &y); }
    assert(x == 0.0f && y == 0.0f);
    stick(0, 0.0f, 1.0f, true, &x, &y);
    assert(near(y, 1.0f));

    // B: THE VALVE INDEX / PIMAX BUG. The runtime says "no thumb" while the stick is actually held
    // down. Before, that taught the drift fix that the pushed position WAS the rest point, and the
    // stick then read zero for the rest of the session: "cannot walk, but the grip still works".
    // Holding the stick for a second must never make it dead - at any moment of the hold.
    for (int i = 0; i < 70; ++i) {
        now += 16;
        stick(1, 0.0f, 0.85f, false, &x, &y);
        assert(y > 0.5f);                      // walking, every single sample
    }
    // and once the sensor has been caught lying it is not consulted again: a thumb-on read changes nothing
    stick(1, 0.0f, 1.0f, true, &x, &y);
    assert(near(y, 1.0f));

    // C: a stick that really drifts (no thumb, resting off centre) is still fixed - that is what
    // [STICKCENTER] exists for, and it must survive the rule above.
    now += 5000; touchActive = true; touchState = true;
    for (int i = 0; i < 100; ++i) { now += 16; stick(0, 0.0f, 0.0f, true, &x, &y); }   // settle, no drift
    for (int i = 0; i < 80; ++i) { now += 16; stick(0, 0.0f, 0.14f, false, &x, &y); }   // rest with no thumb
    assert(x == 0.0f && y == 0.0f);                                                     // nothing while untouchied
    // ...and the thumb back on it, still resting: the resting point is taken off, so still nothing.
    for (int i = 0; i < 10; ++i) { now += 16; stick(0, 0.0f, 0.14f, true, &x, &y); }
    assert(near(y, 0.0f));

    // D: even with that drift fix armed, a real push walks. This is the promise the rule makes.
    stick(0, 0.0f, 0.80f, false, &x, &y);
    assert(y > 0.4f);

    // E: no touch sensor at all - untouched, as on a controller without one.
    now += 5000; touchActive = false;
    stick(1, 0.0f, 0.90f, false, &x, &y);
    assert(y > 0.8f);                      // 0.90 through the dead zone is 0.857 of full stick
    stick(1, 0.0f, 0.0f, false, &x, &y);
    assert(x == 0.0f && y == 0.0f);

    std::printf("Stick center: a pushed stick always passes, no rest learned from a push, real drift still fixed.\n");
}
'''

with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'stick.cpp'
    exe = Path(tmp) / 'stick-test'
    src.write_text(STUBS + logic + CHECKS)
    # -Wno-unused-function: TriggerUnit comes along with the block and this test is about the stick;
    # -Wno-misleading-indentation: two of the original's one-line guards, kept exactly as shipped.
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-Wno-misleading-indentation',
                    str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

# The rule and the way it is kept, straight from the source.
assert 'kStickPushed = 0.45f' in source
assert 'if (touch == 0 && pushed) {' in source, 'a pushed stick with no thumb must disable the touch sensor'
assert 's_stickDrift[hand] && touch >= 0 && !pushed' in source, 'a pushed stick must never be zeroed or offset'
print('Stick center: pushed stick rule present, 0.45 threshold, drift fix still armed only when it cannot hurt.')
